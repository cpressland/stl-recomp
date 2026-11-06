/*
 * The CPU the game and D3DX are told they run on. Every lifted cpuid comes
 * here (run_lift.py CPUID_HOST): ten in d3dx9_27.dll, which picks its
 * vector maths by them, and one in msvcr71.
 *
 * A Pentium III (family 6, model 7): FPU, TSC, CX8, CMOV, MMX and SSE, no
 * SSE2 and no 3DNow!. Legacy itself asks nothing (x87 throughout). D3DX has
 * x87, SSE, SSE2 and 3DNow! paths; 3DNow! is AMD's and the lifter has no
 * lifting of it, and SSE is the path it does lift (lift32_sse.py), so D3DX
 * takes that. STL_CPU=x87 says no SSE either, if the SSE path misbehaves.
 */
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "stl.h"

#define FEAT_FPU   (1u << 0)
#define FEAT_TSC   (1u << 4)
#define FEAT_CX8   (1u << 8)
#define FEAT_CMOV  (1u << 15)
#define FEAT_MMX   (1u << 23)
#define FEAT_FXSR  (1u << 24)
#define FEAT_SSE   (1u << 25)

void stl_cpuid(void) {
    static int sse = -1;
    if (sse < 0) { const char *c = getenv("STL_CPU"); sse = !(c && !strcmp(c, "x87")); }
    uint32_t leaf = g_eax;
    if (leaf == 0) {                             /* the highest leaf, and the vendor */
        g_eax = 1;
        memcpy(&g_ebx, "Genu", 4);
        memcpy(&g_edx, "ineI", 4);
        memcpy(&g_ecx, "ntel", 4);
    } else if (leaf == 1) {
        g_eax = 0x00000673u;                     /* family 6, model 7, stepping 3 */
        g_ebx = 0;
        g_ecx = 0;
        g_edx = FEAT_FPU | FEAT_TSC | FEAT_CX8 | FEAT_CMOV | FEAT_MMX | (sse ? FEAT_FXSR | FEAT_SSE : 0);
    } else if (leaf == 0x80000000u) {            /* no extended leaves: no 3DNow! */
        g_eax = 0x80000000u;
        g_ebx = g_ecx = g_edx = 0;
    } else {
        g_eax = g_ebx = g_ecx = g_edx = 0;
    }
}
