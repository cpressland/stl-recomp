/*
 * wide.c - the "W" (UTF-16) Win32 calls, on win32hle's "A" ones.
 *
 * win32hle answers the ANSI half of Win32, as a Windows 95 game calls it.
 * This game's window code is Unicode (RegisterClassW, CreateWindowExW,
 * DefWindowProcW), msvcr71 asks for GetCommandLineW at start, and D3DX opens
 * files with CreateFileW. Each is answered by the A call on the same stack:
 * its string arguments narrowed into the guest heap and put where the wide
 * ones were, the A shim run (which pops the arguments, stdcall), and any
 * string it wrote widened back into the caller's buffer.
 *
 * Narrowing is to Windows-1252 as far as the text goes: everything this game
 * passes is a path or a name in ASCII, and a character outside it is '?'.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "win32hle.h"
#include "stl.h"

/* ---- strings ------------------------------------------------------------- */

static uint32_t wlen(uint32_t w) {
    uint32_t n = 0;
    while (MEM16(w + 2 * n)) n++;
    return n;
}

/* A heap copy of the guest's UTF-16 string at w, narrowed. 0 for 0; an atom
 * (below 64 KB) is passed as it is. */
static uint32_t narrow(uint32_t w) {
    if (w < 0x10000u) return w;
    uint32_t n = wlen(w);
    char *a = (char *)hle_alloc(n + 1);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = MEM16(w + 2 * i);
        a[i] = (char)(c < 0x100 ? c : '?');
    }
    return hle_va(a);
}

static void release(uint32_t a, uint32_t w) {
    if (a && a != w) hle_free(hle_ptr(a));
}

/* Widen n characters of a narrow guest string into the guest's wide buffer,
 * with its terminator if it fits in cap characters. Returns how many went. */
static uint32_t widen_into(uint32_t wbuf, uint32_t cap, const char *a, uint32_t n) {
    uint32_t k = n < cap ? n : cap;
    for (uint32_t i = 0; i < k; i++) MEM16(wbuf + 2 * i) = (uint8_t)a[i];
    if (k < cap) MEM16(wbuf + 2 * k) = 0;
    return k;
}

/* Run the A shim for name, as if the guest had called it here (shims.c's
 * aliases use it too). */
void stl_call_a(const char *name) {
    uint32_t va = hle_resolve(name);
    recomp_func_t f = va ? recomp_lookup_import(va) : NULL;
    if (!f) hle_fatal("[wide] no %s to answer its W call", name);
    f();
}

/* The A call with the string arguments in mask narrowed (and those in keep
 * left in the heap: a name the A call holds on to). */
static void w2a(const char *aname, unsigned mask, unsigned keep) {
    uint32_t w[8] = { 0 }, a[8] = { 0 };
    for (int i = 0; i < 8; i++)
        if (mask & (1u << i)) {
            w[i] = A32(i);
            a[i] = narrow(w[i]);
            MEM32(g_esp + 4u + 4u * (uint32_t)i) = a[i];
        }
    stl_call_a(aname);
    for (int i = 0; i < 8; i++)
        if ((mask & (1u << i)) && !(keep & (1u << i))) release(a[i], w[i]);
}

#define WRAP(W, A, mask, keep) static void w_##W(void) { w2a(#A, mask, keep); }

WRAP(GetModuleHandleW, GetModuleHandleA, 1, 0)
WRAP(LoadLibraryW, LoadLibraryA, 1, 0)
WRAP(LoadCursorW, LoadCursorA, 2, 0)
WRAP(UnregisterClassW, UnregisterClassA, 1, 0)
WRAP(DefWindowProcW, DefWindowProcA, 0, 0)
WRAP(GetWindowLongW, GetWindowLongA, 0, 0)
WRAP(SetWindowLongW, SetWindowLongA, 0, 0)
WRAP(GetClassLongW, GetClassLongA, 0, 0)
WRAP(SendMessageW, SendMessageA, 0, 0)
WRAP(PostMessageW, PostMessageA, 0, 0)
WRAP(PeekMessageW, PeekMessageA, 0, 0)
WRAP(GetMessageW, GetMessageA, 0, 0)
WRAP(DispatchMessageW, DispatchMessageA, 0, 0)
WRAP(CallWindowProcW, CallWindowProcA, 0, 0)
WRAP(MessageBoxW, MessageBoxA, 6, 0)
WRAP(CreateFileW, CreateFileA, 1, 0)
WRAP(DeleteFileW, DeleteFileA, 1, 0)
WRAP(MoveFileW, MoveFileA, 3, 0)
WRAP(GetFileAttributesW, GetFileAttributesA, 1, 0)
WRAP(SetFileAttributesW, SetFileAttributesA, 1, 0)
WRAP(CreateDirectoryW, CreateDirectoryA, 1, 0)
WRAP(RemoveDirectoryW, RemoveDirectoryA, 1, 0)
WRAP(SetCurrentDirectoryW, SetCurrentDirectoryA, 1, 0)
WRAP(GetDriveTypeW, GetDriveTypeA, 1, 0)
WRAP(SetEnvironmentVariableW, SetEnvironmentVariableA, 3, 0)
WRAP(FindResourceW, FindResourceA, 6, 0)
WRAP(CreateWindowExW, CreateWindowExA, 6, 6)       /* class and title: the window keeps them */

/* A call whose last two arguments are (buffer, size in characters) and
 * which returns the length it wrote, on its A form. */
static void w_out(const char *aname, int nargs, int buf_arg, int size_arg) {
    uint32_t wbuf = A32(buf_arg), cap = A32(size_arg);
    uint32_t abuf = hle_va(hle_alloc(cap * 2 + 16));
    MEM32(g_esp + 4u + 4u * (uint32_t)buf_arg) = abuf;
    MEM32(g_esp + 4u + 4u * (uint32_t)size_arg) = cap * 2 + 16;
    (void)nargs;
    stl_call_a(aname);
    const char *a = (const char *)hle_ptr(abuf);
    uint32_t n = (uint32_t)strlen(a);
    if (n < cap) widen_into(wbuf, cap, a, n);
    else if (cap) { widen_into(wbuf, cap, a, cap - 1); MEM16(wbuf + 2 * (cap - 1)) = 0; }
    /* eax: the length written, or the size needed (A's answer, kept). */
    hle_free(hle_ptr(abuf));
}
static void w_GetModuleFileNameW(void)  { w_out("GetModuleFileNameA", 3, 1, 2); }
static void w_GetSystemDirectoryW(void) { w_out("GetSystemDirectoryA", 2, 0, 1); }
static void w_GetCurrentDirectoryW(void) {          /* (size, buf): the other way round */
    uint32_t cap = A32(0), wbuf = A32(1);
    uint32_t abuf = hle_va(hle_alloc(cap * 2 + 16));
    MEM32(g_esp + 4u) = cap * 2 + 16;
    MEM32(g_esp + 8u) = abuf;
    stl_call_a("GetCurrentDirectoryA");
    const char *a = (const char *)hle_ptr(abuf);
    uint32_t n = (uint32_t)strlen(a);
    if (n < cap) widen_into(wbuf, cap, a, n);
    else g_eax = n + 1;
    hle_free(hle_ptr(abuf));
}

/* The command line, wide, kept for the run as Windows keeps it. */
static void w_GetCommandLineW(void) {
    static uint32_t w;
    if (!w) {
        uint32_t va = hle_resolve("GetCommandLineA");
        recomp_func_t f = recomp_lookup_import(va);
        PUSH32(g_esp, 0);                               /* a return address for its RET to pop */
        f();
        const char *a = (const char *)hle_ptr(g_eax);
        uint32_t n = (uint32_t)strlen(a);
        w = hle_va(hle_alloc(2 * n + 2));
        widen_into(w, n + 1, a, n);
    }
    RET(w, 0);
}

/* OSVERSIONINFOW (276 bytes) or ...EXW (284): the A answer, its
 * szCSDVersion widened. */
static void w_GetVersionExW(void) {
    uint32_t wi = A32(0), size = MEM32(wi);
    uint32_t ai = hle_va(hle_alloc(160));
    MEM32(ai) = size == 284 ? 156 : 148;
    MEM32(g_esp + 4u) = ai;
    stl_call_a("GetVersionExA");
    for (int k = 4; k < 20; k += 4) MEM32(wi + k) = MEM32(ai + k);   /* major, minor, build, platform */
    const char *csd = (const char *)hle_ptr(ai + 20);
    widen_into(wi + 20, 128, csd, (uint32_t)strnlen(csd, 127));
    if (size == 284) memcpy(hle_ptr(wi + 276), hle_ptr(ai + 148), 8);   /* service pack, suite, type */
    hle_free(hle_ptr(ai));
}

/* WNDCLASSW: as WNDCLASSA, its menu and class names narrowed and kept. */
static void w_RegisterClassW(void) {
    uint32_t wc = A32(0);
    uint32_t ac = hle_va(hle_alloc(40));
    memcpy(hle_ptr(ac), hle_ptr(wc), 40);
    MEM32(ac + 32) = narrow(MEM32(wc + 32));
    MEM32(ac + 36) = narrow(MEM32(wc + 36));
    MEM32(g_esp + 4u) = ac;
    stl_call_a("RegisterClassA");
}

const win32hle_shim stl_wide_shims[] = {
    { "GetCommandLineW", w_GetCommandLineW },
    { "GetModuleHandleW", w_GetModuleHandleW },
    { "GetModuleFileNameW", w_GetModuleFileNameW },
    { "GetSystemDirectoryW", w_GetSystemDirectoryW },
    { "GetCurrentDirectoryW", w_GetCurrentDirectoryW },
    { "SetCurrentDirectoryW", w_SetCurrentDirectoryW },
    { "GetVersionExW", w_GetVersionExW },
    { "LoadLibraryW", w_LoadLibraryW },
    { "LoadCursorW", w_LoadCursorW },
    { "RegisterClassW", w_RegisterClassW },
    { "UnregisterClassW", w_UnregisterClassW },
    { "CreateWindowExW", w_CreateWindowExW },
    { "DefWindowProcW", w_DefWindowProcW },
    { "GetWindowLongW", w_GetWindowLongW },
    { "SetWindowLongW", w_SetWindowLongW },
    { "GetClassLongW", w_GetClassLongW },
    { "SendMessageW", w_SendMessageW },
    { "PostMessageW", w_PostMessageW },
    { "PeekMessageW", w_PeekMessageW },
    { "GetMessageW", w_GetMessageW },
    { "DispatchMessageW", w_DispatchMessageW },
    { "CallWindowProcW", w_CallWindowProcW },
    { "MessageBoxW", w_MessageBoxW },
    { "CreateFileW", w_CreateFileW },
    { "DeleteFileW", w_DeleteFileW },
    { "MoveFileW", w_MoveFileW },
    { "GetFileAttributesW", w_GetFileAttributesW },
    { "SetFileAttributesW", w_SetFileAttributesW },
    { "CreateDirectoryW", w_CreateDirectoryW },
    { "RemoveDirectoryW", w_RemoveDirectoryW },
    { "GetDriveTypeW", w_GetDriveTypeW },
    { "SetEnvironmentVariableW", w_SetEnvironmentVariableW },
    { "FindResourceW", w_FindResourceW },
    { NULL, NULL }
};
