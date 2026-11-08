/*
 * loader.c - the lifted modules into guest memory, and their imports
 * bound, as Windows' loader would. The missions' scripts (missions\\*.dsl,
 * *.drl) are mapped with the rest and attached when the game loads them
 * (stl_load_library, from LoadLibraryA).
 *
 * Each module is mapped at the base it was lifted at (tools/gen/modules.py,
 * STL_MODULES in recomp_target.h): Legacy.exe and the two runtime DLLs at
 * their own, d3dx9_27.dll moved to 0x01000000 and relocated here, so the
 * game folder holds the files as shipped. All of them go in before the first
 * heap allocation, which win32hle places from 0x10000000 up.
 *
 * win32hle's recomp_pe_bind asks its resolver for a name alone; these
 * modules import from each other, so the import tables are walked here: an
 * import from a lifted module is that module's export (by name or ordinal,
 * from its own export directory), anything else is win32hle's shim, or a
 * stub that names itself when it is called.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "win32hle.h"
#include "pe_format.h"
#include "pe_loader.h"
#include "stl.h"

#define PAGE 0x1000u
#define DIR_EXPORT 0
#define DIR_RELOC  5

stl_module g_stl_mod[STL_MAX_MODULES];
int g_stl_nmod, g_stl_stubs;

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = len > 0 ? malloc((size_t)len) : NULL;
    if (p && fread(p, 1, (size_t)len, f) != (size_t)len) { free(p); p = NULL; }
    fclose(f);
    *n = (size_t)len;
    return p;
}

/* Map path at base: headers, sections, and its relocations if base is not
 * the file's own. 0 on success. */
static int map_at(const char *path, uint32_t base, pe_image *img, int reserve);
int stl_map_at(const char *path, uint32_t base, pe_image *img) { return map_at(path, base, img, 1); }
/* reserve 0: base is already this module's (a script loaded again), its
 * pages are written over from the file. */
static int map_at(const char *path, uint32_t base, pe_image *img, int reserve) {
    size_t flen;
    uint8_t *file = slurp(path, &flen);
    if (!file) { fprintf(stderr, "[loader] cannot read %s\n", path); return 1; }
    pe_dos_header *dos = (pe_dos_header *)file;
    pe_nt_headers32 *nt = (pe_nt_headers32 *)(file + (dos->e_lfanew > 0 ? dos->e_lfanew : 0));
    if (flen < 0x200 || dos->e_magic != 0x5A4D || (size_t)dos->e_lfanew + sizeof *nt > flen ||
        nt->Signature != 0x00004550 || nt->OptionalHeader.Magic != 0x10B) {
        free(file);
        fprintf(stderr, "[loader] %s is not a 32-bit PE\n", path);
        return 1;
    }
    pe_opt_header32 *opt = &nt->OptionalHeader;
    uint32_t span = (opt->SizeOfImage + PAGE - 1) & ~(PAGE - 1);
    if (reserve && hle_vm_reserve(base, span) != base) {
        free(file);
        fprintf(stderr, "[loader] 0x%08X-0x%08X is in use: %s cannot go there\n", base, base + span, path);
        return 1;
    }
    if (!reserve) memset(hle_ptr(base), 0, span);          /* .bss and the rest: as a fresh map */
    memcpy(hle_ptr(base), file, opt->SizeOfHeaders < flen ? opt->SizeOfHeaders : flen);
    pe_section_header *sec = (pe_section_header *)((uint8_t *)opt + nt->FileHeader.SizeOfOptionalHeader);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t n = sec[i].SizeOfRawData < sec[i].VirtualSize ? sec[i].SizeOfRawData : sec[i].VirtualSize;
        if (!sec[i].VirtualSize) n = sec[i].SizeOfRawData;
        if (n && (size_t)sec[i].PointerToRawData + n <= flen && sec[i].VirtualAddress + n <= span)
            memcpy(hle_ptr(base + sec[i].VirtualAddress), file + sec[i].PointerToRawData, n);
    }
    if (base != opt->ImageBase) {
        pe_data_dir r = opt->DataDirectory[DIR_RELOC];
        if (!r.Size) { free(file); fprintf(stderr, "[loader] %s has no relocations\n", path); return 1; }
        recomp_pe_relocate(base, r.VirtualAddress, r.Size, (int32_t)(base - opt->ImageBase));
        /* As Windows does: the mapped header says where the image is. */
        pe_nt_headers32 *mnt = (pe_nt_headers32 *)hle_ptr(base + (uint32_t)dos->e_lfanew);
        mnt->OptionalHeader.ImageBase = base;
    }
    img->base = base;
    img->image_base = opt->ImageBase;
    img->span = span;
    img->entry = opt->AddressOfEntryPoint ? base + opt->AddressOfEntryPoint : 0;
    img->import_rva = opt->DataDirectory[PE_DIR_IMPORT].VirtualAddress;
    img->resource_rva = opt->DataDirectory[2].VirtualAddress;
    img->resource_size = opt->DataDirectory[2].Size;
    img->stamp = nt->FileHeader.TimeDateStamp;
    free(file);
    return 0;
}

/* d3dx9_27.dll was built against Windows' own C runtime, msvcrt.dll, which
 * is not lifted: its imports are bound to msvcr71's exports, the same API
 * (so its throws reach the same eh32 hook, and its heap is msvcr71's). One
 * msvcr71 lacks falls through to win32hle as any other import. */
static const char *alias(const char *dll) {
    return !strcasecmp(dll, "msvcrt.dll") ? "msvcr71.dll" : dll;
}

static const char *base_name(const char *p) {
    const char *a = strrchr(p, '\\'), *b = strrchr(p, '/');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

stl_module *stl_module_named(const char *dll) {
    dll = alias(base_name(dll));
    for (int i = 0; i < g_stl_nmod; i++)
        if (!strcasecmp(g_stl_mod[i].file, dll)) return &g_stl_mod[i];
    return NULL;
}

/* An export of a mapped module, by name or (name NULL) ordinal; 0 if none.
 * A forwarder (an RVA inside the export directory) is not followed: none of
 * these modules has one. */
stl_module *stl_module_at(uint32_t base) {
    for (int i = 0; i < g_stl_nmod; i++)
        if (g_stl_mod[i].img.base == base) return &g_stl_mod[i];
    return NULL;
}

uint32_t stl_export_of(const stl_module *m, const char *name, uint32_t ordinal) {
    pe_dos_header *dos = (pe_dos_header *)hle_ptr(m->img.base);
    pe_nt_headers32 *nt = (pe_nt_headers32 *)hle_ptr(m->img.base + (uint32_t)dos->e_lfanew);
    pe_data_dir d = nt->OptionalHeader.DataDirectory[DIR_EXPORT];
    if (!d.Size) return 0;
    uint32_t e = m->img.base + d.VirtualAddress;
    uint32_t obase = MEM32(e + 16), nfunc = MEM32(e + 20), nname = MEM32(e + 24);
    uint32_t funcs = m->img.base + MEM32(e + 28), names = m->img.base + MEM32(e + 32),
             ords = m->img.base + MEM32(e + 36);
    uint32_t idx = UINT32_MAX;
    if (name) {
        for (uint32_t i = 0; i < nname; i++)
            if (!strcmp((const char *)hle_ptr(m->img.base + MEM32(names + 4 * i)), name)) {
                idx = MEM16(ords + 2 * i);
                break;
            }
    } else if (ordinal >= obase) {
        idx = ordinal - obase;
    }
    if (idx >= nfunc || !MEM32(funcs + 4 * idx)) return 0;
    uint32_t rva = MEM32(funcs + 4 * idx);
    if (rva >= d.VirtualAddress && rva < d.VirtualAddress + d.Size) {
        fprintf(stderr, "[loader] %s!%s forwards; not followed\n", m->file, name ? name : "(ordinal)");
        return 0;
    }
    return m->img.base + rva;
}

uint32_t stl_export(const char *dll, const char *name) {
    stl_module *m = stl_module_named(dll);
    return m ? stl_export_of(m, name, 0) : 0;
}

/* Bind every import of m. Returns how many are stubs (win32hle lacks them). */
int stl_bind(stl_module *m) {
    if (!m->img.import_rva) return 0;
    uint32_t base = m->img.base;
    int stubs = 0, lifted = 0, shims = 0;
    for (pe_import_descriptor *d = (pe_import_descriptor *)hle_ptr(base + m->img.import_rva); d->Name; d++) {
        const char *dll = (const char *)hle_ptr(base + d->Name);
        stl_module *to = stl_module_named(dll);
        uint32_t *ilt = (uint32_t *)hle_ptr(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        uint32_t *iat = (uint32_t *)hle_ptr(base + d->FirstThunk);
        for (; *ilt; ilt++, iat++) {
            int by_ord = (*ilt & 0x80000000u) != 0;
            const char *name = by_ord ? NULL : (const char *)hle_ptr(base + *ilt + 2);
            uint32_t va = 0;
            /* A host answer for a lifted export wins (stl_shims: none needed yet). */
            if (to) va = stl_export_of(to, name, *ilt & 0xFFFFu);
            if (va) {
                lifted++;
            } else if (to && strcasecmp(dll, to->file) == 0) {
                fprintf(stderr, "[loader] %s: %s has no export %s\n", m->file, dll, name ? name : "(ordinal)");
            } else {
                char nm[128];
                if (by_ord) {
                    size_t k = 0;
                    for (; dll[k] && k < 100; k++) nm[k] = (char)(dll[k] >= 'A' && dll[k] <= 'Z' ? dll[k] + 32 : dll[k]);
                    snprintf(nm + k, sizeof nm - k, "#%u", *ilt & 0xFFFFu);
                    name = nm;
                }
                va = hle_resolve(name);
                if (va) shims++;
                else {
                    char *keep = strdup(name);
                    va = hle_resolve_or_stub(keep);
                    stubs++;
                    if (stl_verbose) fprintf(stderr, "[loader] %s: %s!%s is a stub\n", m->file, dll, name);
                }
            }
            *iat = va;
        }
    }
    fprintf(stderr, "[loader] %s: %d imports from lifted modules, %d answered by win32hle, %d stubs\n",
            m->file, lifted, shims, stubs);
    return stubs;
}

int stl_load(const char *game) {
    static const struct { const char *file; uint32_t base, entry, stamp; int on_demand; } want[] = STL_MODULES;
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        stl_module *m = &g_stl_mod[g_stl_nmod];
        char guest[300], host[4096];
        snprintf(guest, sizeof guest, "C:\\%s", want[i].file);
        /* The missions' scripts are mapped now too (so the heap never takes
         * their place), and attached when the game loads them. */
        if (!hle_host_path(guest, host, sizeof host)) {
            fprintf(stderr, "[loader] no %s in %s\n", want[i].file, game);
            return 1;
        }
        if (stl_map_at(host, want[i].base, &m->img) != 0) return 1;
        if (m->img.stamp != want[i].stamp) {
            fprintf(stderr, "[loader] %s is not the build this host was lifted from (PE timestamp 0x%08X)\n",
                    host, m->img.stamp);
            return 1;
        }
        m->file = base_name(want[i].file);
        m->path = want[i].file;
        m->is_exe = i == 0;
        m->on_demand = want[i].on_demand;
        if (!m->on_demand) hle_module_add(guest, m->img.base, m->img.resource_rva, m->img.resource_size, m->is_exe);
        g_stl_nmod++;
        if (!m->on_demand || stl_verbose)
            fprintf(stderr, "[loader] %-13s 0x%08X-0x%08X entry 0x%08X\n", m->file, m->img.base,
                    m->img.base + m->img.span, m->img.entry);
    }
    for (int i = 0; i < g_stl_nmod; i++) g_stl_stubs += stl_bind(&g_stl_mod[i]);
    return 0;
}

static uint32_t dll_main(stl_module *m, uint32_t reason) {
    if (!m->img.entry) return 1;
    uint32_t args[3] = { m->img.base, reason, 0 };
    return hle_call_guest(m->img.entry, 3, args);
}

/* The lifted modules a module imports from that the game loads itself: a
 * mission's R imports its S. Calls fn on each. */
static void each_on_demand_import(stl_module *m, void (*fn)(stl_module *)) {
    if (!m->img.import_rva) return;
    for (pe_import_descriptor *d = (pe_import_descriptor *)hle_ptr(m->img.base + m->img.import_rva); d->Name; d++) {
        stl_module *to = stl_module_named((const char *)hle_ptr(m->img.base + d->Name));
        if (to && to != m && to->on_demand) fn(to);
    }
}
static void load_dep(stl_module *m) { stl_load_library(m->path); }
static void free_dep(stl_module *m) { stl_free_library(m->img.base); }

/* LoadLibrary of a mission's script: as Windows would, the modules it
 * imports from first, then its DllMain. A module freed and loaded again
 * starts from the file again, its data as the file has it. */
uint32_t stl_load_library(const char *name) {
    stl_module *m = name ? stl_module_named(name) : NULL;
    if (!m || !m->on_demand) return 0;
    if (m->refs++) return m->img.base;
    char guest[300], host[4096];
    snprintf(guest, sizeof guest, "C:\\%s", m->path);
    if (m->used) {
        if (!hle_host_path(guest, host, sizeof host) || map_at(host, m->img.base, &m->img, 0) != 0) {
            m->refs = 0;
            return 0;
        }
        stl_bind(m);
    }
    m->used = 1;
    if (!m->listed) {
        hle_module_add(guest, m->img.base, m->img.resource_rva, m->img.resource_size, 0);
        m->listed = 1;
    }
    each_on_demand_import(m, load_dep);
    fprintf(stderr, "[loader] %s loaded at 0x%08X: DllMain(PROCESS_ATTACH)\n", m->file, m->img.base);
    if (!dll_main(m, 1)) {
        fprintf(stderr, "[loader] %s refused to attach\n", m->file);
        each_on_demand_import(m, free_dep);
        m->refs = 0;
        return 0;
    }
    return m->img.base;
}

int stl_free_library(uint32_t base) {
    stl_module *m = stl_module_at(base);
    if (!m || !m->on_demand) return 0;
    if (m->refs > 0 && --m->refs == 0) {
        fprintf(stderr, "[loader] %s freed: DllMain(PROCESS_DETACH)\n", m->file);
        dll_main(m, 0);
        each_on_demand_import(m, free_dep);
    }
    return 1;
}

/* DLL_PROCESS_ATTACH for each DLL, a DLL after those it imports from, as
 * Windows runs them before the program's entry: msvcr71, msvcp71 (which
 * imports msvcr71), d3dx9_27. */
int stl_attach_dlls(void) {
    static const char *order[] = { "msvcr71.dll", "msvcp71.dll", "d3dx9_27.dll" };
    for (size_t k = 0; k < sizeof order / sizeof order[0]; k++) {
        stl_module *m = stl_module_named(order[k]);
        if (!m || !m->img.entry) continue;
        uint32_t args[3] = { m->img.base, 1 /* DLL_PROCESS_ATTACH */, 0 };
        fprintf(stderr, "[loader] %s: DllMain(PROCESS_ATTACH)\n", m->file);
        uint32_t ok = hle_call_guest(m->img.entry, 3, args);
        if (!ok) { fprintf(stderr, "[loader] %s refused to attach\n", m->file); return 1; }
    }
    return 0;
}
