/*
 * setjmp.c - the guest's setjmp and longjmp (msvcr71's _setjmp3, _setjmp
 * and longjmp, run_lift.py HOOK_EXPORTS).
 *
 * D3DX's HLSL compiler recovers from a syntax error by longjmp back to the
 * setjmp at its top. Lifted, longjmp's "return from setjmp a second time" is
 * a jump into the middle of a C function further up the host's stack, which
 * is what eh32's landing pads already do for a caught C++ throw:
 *
 * - run_lift.py gives each function that calls setjmp a landing pad
 *   (RECOMP_EH_ENTER) with a label after each such call, and sets
 *   g_stl_setjmp_resume to that label's address just before the call: a
 *   lifted call pushes no real return address (RECOMP_RETADDR is 0), so
 *   setjmp would not otherwise know where it was called from;
 * - setjmp saves the registers as MSVC's _JUMP_BUFFER has them (Ebp, Ebx,
 *   Edi, Esi, Esp at its return address, Eip, Registration), Eip being that
 *   label, and returns 0;
 * - longjmp puts them back, the SEH chain to its registration, esp past
 *   setjmp's return address, eax the value (1 for 0), finds the frame of
 *   the function that called setjmp (the newest entered above the saved
 *   esp) and resumes it at the label (recomp_eh_resume).
 *
 * ponytail: a longjmp past C++ frames does not run their destructors (as
 * MSVC's would not from C code), and setjmp's unwind data is not kept.
 */
#include <stdint.h>

#include "win32hle.h"
#include "stl.h"

uint32_t g_stl_setjmp_resume;

void stl_setjmp3(void) {          /* cdecl (jmp_buf, count, ...); _setjmp(jmp_buf) too */
    uint32_t jb = A32(0);
    if (!g_stl_setjmp_resume) hle_fatal("[setjmp] called from sub_%08X, which the lift gave no landing pad", g_cur_func);
    MEM32(jb + 0) = g_ebp;
    MEM32(jb + 4) = g_ebx;
    MEM32(jb + 8) = g_edi;
    MEM32(jb + 12) = g_esi;
    MEM32(jb + 16) = g_esp;                  /* at setjmp's return address, as MSVC saves it */
    MEM32(jb + 20) = g_stl_setjmp_resume;
    MEM32(jb + 24) = MEM32(g_fs_base);       /* the SEH registration */
    MEM32(jb + 28) = 0xFFFFFFFFu;            /* TryLevel */
    MEM32(jb + 32) = 0x56433230u;            /* 'VC20': a _setjmp3 buffer */
    MEM32(jb + 36) = 0;                      /* no unwind function */
    g_stl_setjmp_resume = 0;
    g_eax = 0;
    g_esp += 4;                              /* cdecl: the caller pops the arguments */
}

void stl_longjmp(void) {          /* cdecl (jmp_buf, value): never returns */
    uint32_t jb = A32(0), value = A32(1);
    uint32_t esp = MEM32(jb + 16), resume = MEM32(jb + 20);
    recomp_ehframe_t *f = recomp_eh_owner(esp);
    if (!f || !resume) hle_fatal("[longjmp] no lifted frame owns the setjmp at esp 0x%08X", esp);
    g_ebp = MEM32(jb + 0);
    g_ebx = MEM32(jb + 4);
    g_edi = MEM32(jb + 8);
    g_esi = MEM32(jb + 12);
    MEM32(g_fs_base) = MEM32(jb + 24);
    g_esp = esp + 4;                         /* setjmp's return address, popped */
    g_eax = value ? value : 1;
    recomp_eh_resume(f, resume);
}
