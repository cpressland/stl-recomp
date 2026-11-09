/*
 * shims.c - Win32 answers this game needs that win32hle has not got, or
 * answers differently. Registered before win32hle's, so these win.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "win32hle.h"
#include "stl.h"

/* Message boxes print, and say OK (or Yes). */
static void s_MessageBoxA(void) {
    uint32_t type = A32(3), buttons = type & 0xF;
    uint32_t answer = buttons == 4 || buttons == 3 ? 6u : 1u;
    fprintf(stderr, "[messagebox] %s: %s -> %u\n", ASTR(2) ? ASTR(2) : "", ASTR(1) ? ASTR(1) : "", answer);
    RET(answer, 4);
}

/* ---- kernel32 ------------------------------------------------------------ */
/* A system DLL by its full path (the game builds C:\WINDOWS\SYSTEM32\d3d9.dll
 * from GetSystemDirectoryA): win32hle knows them by name, so anything under
 * C:\WINDOWS\ is asked for by its name alone. */
static recomp_func_t hle_own(const char *name) {      /* win32hle's own, under ours */
    for (const win32hle_shim *m = win32hle_module; m->name; m++)
        if (!strcmp(m->name, name)) return m->fn;
    hle_fatal("win32hle has no module function");
    return NULL;
}
static void hle_LoadLibraryA(void) {
    static recomp_func_t f;
    if (!f) f = hle_own("LoadLibraryA");
    f();
}
/* A mission's script (missions\STL12S.dsl) is lifted: loaded, attached and
 * freed by loader.c, its exports its own. */
static void k_LoadLibraryA(void) {
    const char *path = A32(0) ? ASTR(0) : NULL;
    uint32_t lifted = stl_load_library(path);
    if (lifted) RET(lifted, 1);
    if (path && !strncasecmp(path, "C:\\WINDOWS\\", 11)) {
        const char *base = strrchr(path, '\\') + 1;
        uint32_t mark = hle_scratch_mark();
        MEM32(g_esp + 4u) = hle_scratch_copy(base, (uint32_t)strlen(base) + 1);
        hle_LoadLibraryA();
        hle_scratch_release(mark);
        return;
    }
    hle_LoadLibraryA();
}

static void k_GetProcAddress(void) {                     /* (module, name or ordinal) */
    static recomp_func_t f;
    stl_module *m = stl_module_at(A32(0));
    if (m) {
        uint32_t n = A32(1), va = n < 0x10000u ? stl_export_of(m, NULL, n) : stl_export_of(m, ASTR(1), 0);
        if (!va) {
            fprintf(stderr, "[loader] GetProcAddress(%s, %s): none\n", m->file, n < 0x10000u ? "(ordinal)" : ASTR(1));
            hle_set_last_error(127);                     /* ERROR_PROC_NOT_FOUND */
        }
        RET(va, 2);
    }
    if (!f) f = hle_own("GetProcAddress");
    f();
}
static void k_FreeLibrary(void) {
    static recomp_func_t f;
    if (stl_free_library(A32(0))) RET(1, 1);
    if (!f) f = hle_own("FreeLibrary");
    f();
}

static void k_DisableThreadLibraryCalls(void) { RET(1, 1); }
/* (class name, instance): the game unregisters its window class as it
 * quits. win32hle keeps classes for the process's life; there is nothing to
 * undo, and Windows says yes. */
static void u_UnregisterClassA(void)          { RET(1, 2); }
static void k_IsDBCSLeadByte(void)            { RET(0, 1); }   /* Windows-1252: no lead bytes */
static void k_IsDBCSLeadByteEx(void)          { RET(0, 2); }
static void k_GetConsoleCP(void)              { RET(1252, 0); }
static void k_HeapCompact(void)               { RET(0, 2); }
static void k_Beep(void)                      { RET(1, 2); }
static void k_SetLocalTime(void)              { RET(0, 1); }   /* the guest does not set the Mac's clock */

static void k_ExpandEnvironmentStringsA(void) {   /* (src, dst, size): no %VAR% this game uses */
    const char *src = ASTR(0);
    uint32_t n = (uint32_t)strlen(src) + 1, cap = A32(2);
    if (A32(1) && cap >= n) memcpy(APTR(1), src, n);
    RET(n, 3);
}

/* ---- user32 -------------------------------------------------------------- */
static void u_IsIconic(void)        { RET(0, 1); }
static void u_IsZoomed(void)        { RET(0, 1); }
static void u_GetKeyboardLayout(void) { RET(0x04090409u, 1); }   /* US English */
static void u_GetKeyboardType(void) {                            /* an enhanced 101/102-key keyboard */
    uint32_t t = A32(0);
    RET(t == 0 ? 4 : t == 2 ? 12 : 0, 1);
}
static void u_GetIconInfo(void)     { RET(0, 2); }
static void u_SetSystemCursor(void) { RET(1, 2); }
static void u_DrawTextExA(void)     { RET(0, 6); }
static void u_GetDlgItemInt(void)   { if (A32(2)) MEM32(A32(2)) = 0; RET(0, 4); }
static void u_SetDlgItemInt(void)   { RET(1, 4); }
static void u_CheckRadioButton(void) { RET(1, 4); }

/* WINDOWPLACEMENT: length, flags, showCmd, ptMin, ptMax, rcNormal. */
static void u_GetWindowPlacement(void) {
    uint32_t wp = A32(1);
    if (!wp) RET(0, 2);
    uint32_t len = MEM32(wp) ? MEM32(wp) : 44;
    memset(hle_ptr(wp), 0, 44);
    MEM32(wp) = len;
    MEM32(wp + 8) = 1;                                   /* SW_SHOWNORMAL */
    MEM32(wp + 36) = 1024, MEM32(wp + 40) = 768;          /* rcNormal: 0,0 - 1024,768 */
    RET(1, 2);
}
static void u_SetWindowPlacement(void) { RET(1, 2); }

/* SystemParametersInfo(action, uiParam, pvParam, winIni): the work area is
 * the screen; the accessibility settings the game saves and puts back
 * (sticky, filter and toggle keys) read as all off. */
static void u_SystemParametersInfo(void) {
    uint32_t action = A32(0), pv = A32(2);
    if (action == 0x30 && pv) {                           /* SPI_GETWORKAREA */
        MEM32(pv) = 0, MEM32(pv + 4) = 0, MEM32(pv + 8) = 1920, MEM32(pv + 12) = 1080;
    } else if ((action == 0x3A || action == 0x32 || action == 0x34) && pv) {  /* STICKY/FILTER/TOGGLEKEYS */
        uint32_t size = MEM32(pv);
        if (size >= 8 && size <= 64) memset(hle_ptr(pv + 4), 0, size - 4);
    }
    RET(1, 4);
}

/* One display: the monitor handle 1, its rectangle the screen. MONITORINFO
 * (EX): size, rcMonitor, rcWork, flags (MONITORINFOF_PRIMARY). */
static void u_MonitorFromWindow(void) { RET(1, 2); }
static void u_MonitorFromPoint(void)  { RET(1, 3); }
static void u_GetMonitorInfo(void) {
    uint32_t mi = A32(1);
    if (!mi) RET(0, 2);
    uint32_t r[8] = { 0, 0, 1920, 1080, 0, 0, 1920, 1080 };
    memcpy(hle_ptr(mi + 4), r, sizeof r);
    MEM32(mi + 36) = 1;
    if (MEM32(mi) >= 72) memset(hle_ptr(mi + 40), 0, 32);          /* szDevice: no name */
    RET(1, 2);
}

/* ---- gdi32 --------------------------------------------------------------- */
static void g_GetMapMode(void)        { RET(1, 1); }    /* MM_TEXT */
static void g_SetMapMode(void)        { RET(1, 2); }
static void g_EnumFontsA(void)        { RET(1, 4); }    /* no fonts to list: the callback is not called */
static void g_Polyline(void)          { RET(1, 3); }
static void g_SetDIBitsToDevice(void) { RET(A32(8), 12); }
static void g_GetDIBits(void)         { RET(0, 7); }
static void g_CreatePatternBrush(void) {                /* as a black solid brush */
    MEM32(g_esp + 4u) = 0;
    stl_call_a("CreateSolidBrush");
}
static void g_GetObjectW(void)        { stl_call_a("GetObjectA"); }   /* bitmaps: the same structure */

/* ---- shell32 ------------------------------------------------------------- */
/* SHGetFolderPathA(hwnd, csidl, token, flags, path): the user's folders are
 * drive U:, under the Mac's Application Support (main.c), each made when it
 * is first asked for. */
static const char *g_user_root;
void stl_set_user_root(const char *host) { g_user_root = host; hle_set_drive('U', host); }

static void sh_SHGetFolderPathA(void) {
    uint32_t csidl = A32(1) & 0xFF, out = A32(4);
    const char *sub = NULL;
    switch (csidl) {
    case 0x05: sub = "My Documents"; break;                       /* CSIDL_PERSONAL */
    case 0x1A: sub = "Application Data"; break;                   /* CSIDL_APPDATA */
    case 0x1C: sub = "Local Settings\\Application Data"; break;   /* CSIDL_LOCAL_APPDATA */
    case 0x23: sub = "All Users\\Application Data"; break;        /* CSIDL_COMMON_APPDATA */
    case 0x2E: sub = "All Users\\Documents"; break;               /* CSIDL_COMMON_DOCUMENTS */
    case 0x28: sub = ""; break;                                   /* CSIDL_PROFILE */
    }
    if (!sub || !out || !g_user_root) {
        fprintf(stderr, "[shell] SHGetFolderPathA(CSIDL 0x%02X): not a folder this host has\n", csidl);
        RET(0x80004005u, 5);                                      /* E_FAIL */
    }
    char host[4096];
    snprintf(host, sizeof host, "%s/%s", g_user_root, sub);
    for (char *p = host + strlen(g_user_root) + 1; *p; p++) if (*p == '\\') *p = '/';
    for (char *p = host + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(host, 0755); *p = '/'; }
    mkdir(host, 0755);
    snprintf((char *)hle_ptr(out), 260, *sub ? "U:\\%s" : "U:", sub);
    RET(0, 5);                                                    /* S_OK */
}

static void sh_ExtractIcon(void) { RET(0, 3); }    /* no icon: the window has the app's */

/* ---- d3d9's profiling and debug entry points (PIX), looked up by name ---- */
static void p_BeginEvent(void)        { RET((uint32_t)-1, 2); }
static void p_EndEvent(void)          { RET((uint32_t)-1, 0); }
static void p_SetMarker(void)         { RET(0, 2); }
static void p_SetRegion(void)         { RET(0, 2); }
static void p_QueryRepeatFrame(void)  { RET(0, 0); }
static void p_SetOptions(void)        { RET(0, 1); }
static void p_GetStatus(void)         { RET(0, 0); }
static void p_DebugSetMute(void)      { RET(0, 0); }

/* ---- imagehlp, Winsock 2 ------------------------------------------------- */
static void i_UnDecorateSymbolName(void) {    /* (name, out, size, flags): left as it is */
    const char *in = ASTR(0);
    uint32_t cap = A32(2);
    if (!cap) RET(0, 4);
    snprintf((char *)APTR(1), cap, "%s", in);
    RET((uint32_t)strlen((char *)APTR(1)), 4);
}
/* binkw32: the two calls pcrecomp's bink.c has not got. The surface type
 * is BINKSURFACE32 (X8R8G8B8) until d3d9.c has surfaces to ask; the volume
 * is the movie's own. */
static void b_BinkDX9SurfaceType(void) { RET(3, 1); }
static void b_BinkSetVolume(void)      { RET(0, 3); }

/* WS2_32 by ordinal: the same calls as Winsock 1.1's, by name. */
static void ws2_gethostname(void)   { stl_call_a("gethostname"); }
static void ws2_gethostbyname(void) { stl_call_a("gethostbyname"); }
static void ws2_inet_ntoa(void)     { stl_call_a("inet_ntoa"); }
static void ws2_WSAStartup(void)    { stl_call_a("WSAStartup"); }
static void ws2_WSACleanup(void)    { stl_call_a("WSACleanup"); }

const win32hle_shim stl_shims[] = {
    { "MessageBoxA", s_MessageBoxA },
    { "LoadLibraryA", k_LoadLibraryA }, { "GetProcAddress", k_GetProcAddress }, { "FreeLibrary", k_FreeLibrary },
    { "DisableThreadLibraryCalls", k_DisableThreadLibraryCalls }, { "UnregisterClassA", u_UnregisterClassA },
    { "IsDBCSLeadByte", k_IsDBCSLeadByte }, { "IsDBCSLeadByteEx", k_IsDBCSLeadByteEx },
    { "GetConsoleCP", k_GetConsoleCP }, { "GetConsoleOutputCP", k_GetConsoleCP },
    { "HeapCompact", k_HeapCompact }, { "Beep", k_Beep }, { "SetLocalTime", k_SetLocalTime },
    { "ExpandEnvironmentStringsA", k_ExpandEnvironmentStringsA },
    { "IsIconic", u_IsIconic }, { "IsZoomed", u_IsZoomed },
    { "GetKeyboardLayout", u_GetKeyboardLayout }, { "GetKeyboardType", u_GetKeyboardType },
    { "GetIconInfo", u_GetIconInfo }, { "SetSystemCursor", u_SetSystemCursor },
    { "DrawTextExA", u_DrawTextExA }, { "GetDlgItemInt", u_GetDlgItemInt },
    { "SetDlgItemInt", u_SetDlgItemInt }, { "CheckRadioButton", u_CheckRadioButton },
    { "GetWindowPlacement", u_GetWindowPlacement }, { "SetWindowPlacement", u_SetWindowPlacement },
    { "SystemParametersInfoA", u_SystemParametersInfo }, { "SystemParametersInfoW", u_SystemParametersInfo },
    { "GetMapMode", g_GetMapMode }, { "SetMapMode", g_SetMapMode }, { "EnumFontsA", g_EnumFontsA },
    { "Polyline", g_Polyline }, { "SetDIBitsToDevice", g_SetDIBitsToDevice }, { "GetDIBits", g_GetDIBits },
    { "CreatePatternBrush", g_CreatePatternBrush }, { "GetObjectW", g_GetObjectW },
    { "SHGetFolderPathA", sh_SHGetFolderPathA }, { "ExtractIconW", sh_ExtractIcon }, { "ExtractIconA", sh_ExtractIcon },
    { "MonitorFromWindow", u_MonitorFromWindow }, { "MonitorFromPoint", u_MonitorFromPoint },
    { "GetMonitorInfoW", u_GetMonitorInfo }, { "GetMonitorInfoA", u_GetMonitorInfo },
    { "D3DPERF_BeginEvent", p_BeginEvent }, { "D3DPERF_EndEvent", p_EndEvent },
    { "D3DPERF_SetMarker", p_SetMarker }, { "D3DPERF_SetRegion", p_SetRegion },
    { "D3DPERF_QueryRepeatFrame", p_QueryRepeatFrame }, { "D3DPERF_SetOptions", p_SetOptions },
    { "D3DPERF_GetStatus", p_GetStatus }, { "DebugSetMute", p_DebugSetMute },
    { "UnDecorateSymbolName", i_UnDecorateSymbolName },
    { "_BinkDX9SurfaceType@4", b_BinkDX9SurfaceType }, { "_BinkSetVolume@12", b_BinkSetVolume },
    { "ws2_32.dll#57", ws2_gethostname }, { "ws2_32.dll#52", ws2_gethostbyname },
    { "ws2_32.dll#12", ws2_inet_ntoa }, { "ws2_32.dll#115", ws2_WSAStartup },
    { "ws2_32.dll#116", ws2_WSACleanup },
    { NULL, NULL }
};
