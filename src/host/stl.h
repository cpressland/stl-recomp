/* stl.h - what the host's files share. */
#pragma once
#include <stdint.h>
#include "win32hle.h"
#include "pe_loader.h"
#include "recomp_target.h"

#define STL_MAX_MODULES 8

typedef struct {
    const char *file;     /* as the game folder has it: Legacy.exe, d3dx9_27.dll, ... */
    pe_image    img;
    int         is_exe;
} stl_module;

extern stl_module g_stl_mod[STL_MAX_MODULES];
extern int g_stl_nmod, g_stl_stubs;
extern int stl_verbose;
extern int stl_headless;                 /* --headless: no window, no sound device */                  /* --verbose: every stub bound, every D3D9 call */

int  stl_map_at(const char *path, uint32_t base, pe_image *img);    /* loader.c */
int  stl_load(const char *game);
int  stl_bind(stl_module *m);
int  stl_attach_dlls(void);
stl_module *stl_module_named(const char *dll);
uint32_t stl_export(const char *dll, const char *name);

void stl_cpuid(void);
void stl_pump(void);                                                /* main.c: SDL's events, on the main thread */                                               /* cpu.c */

extern const win32hle_shim stl_shims[];                             /* shims.c */
void stl_set_user_root(const char *host);                           /* drive U: */
void stl_call_a(const char *name);                                  /* wide.c: run a shim by name */
extern const win32hle_shim stl_d3d9_shims[];                        /* d3d9.c */
void stl_d3d9_summary(void);                                        /* the Direct3D 9 calls made, at exit */
extern const win32hle_shim stl_wide_shims[];                        /* wide.c */
