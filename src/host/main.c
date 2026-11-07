/*
 * Star Trek: Legacy - the native host, for macOS (and later Linux).
 *
 * The lifted C of all four modules (run_lift.py) on pcrecomp's
 * runtime/win32hle: Legacy.exe, d3dx9_27.dll and the Visual C++ 7.1
 * runtime DLLs are mapped and bound to each other (loader.c), every other
 * import is win32hle's or this host's (shims.c, d3d9.c), and the DLLs are
 * attached before the game's own entry is called, as Windows would.
 *
 * The guest's memory is one 4 GB block of the host's (based), the game
 * folder is drive C:, and the registry holds what the installer wrote.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>

#include "win32hle.h"
#include "host.h"
#include "stl.h"

#define HKLM 0x80000002u

int stl_verbose;
static int g_headless;
static int g_windowed;
static unsigned g_watchdog_s;

int  script_arg(int argc, char **argv, int i);   /* script.c (tpw-recomp's): scripted input */
void script_start(void);

/* A function the lift left as a stub (run_lift.py writes none, today). */
void recomp_not_lifted(uint32_t va) { hle_fatal("sub_%08X was not lifted", va); }

/* What the installer leaves in the registry (Star Trek Legacy.msi's
 * Registry table: one value). */
static void seed_registry(void) {
    hle_reg_set_string(HKLM, "Software\\Bethesda Softworks\\Legacy\\Settings", "ProductLanguage", "EN");
}

/* ---- faults and the watchdog ---------------------------------------------- */
static void report(const char *why) {
    fprintf(stderr, "\n=== %s ===\n  in lifted sub_%08X, last import %s\n", why, g_cur_func,
            g_cur_import ? g_cur_import : "(none)");
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    fprintf(stderr, "last indirect calls (newest first):\n");
    for (int i = 1; i <= 40 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char *nm = hle_name(g_icall_trace[k]);
        fprintf(stderr, "  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    if (hle_vm_readable(g_esp, 128)) {
        fprintf(stderr, "guest stack at %08X:", g_esp);
        for (int k = 0; k < 32; k++) fprintf(stderr, "%s%08X", k % 8 ? " " : "\n  ", MEM32(g_esp + 4 * k));
        fprintf(stderr, "\n");
    }
}

static void on_exit_summary(int code) { (void)code; stl_d3d9_summary(); }

static void on_fault(int sig, siginfo_t *si, void *uc) {
    static volatile int once;
    (void)uc;
    if (__sync_lock_test_and_set(&once, 1)) _exit(3);
    char why[96];
    uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)g_mem_base;
    if (a >= base && a - base < 0x100000000ull)
        snprintf(why, sizeof why, "fault: signal %d at guest 0x%08X", sig, (uint32_t)(a - base));
    else
        snprintf(why, sizeof why, "fault: signal %d, host address %p", sig, si->si_addr);
    report(why);
    stl_d3d9_summary();
    fflush(stderr);
    _exit(3);
}

static void *watchdog(void *arg) {
    (void)arg;
    sleep(g_watchdog_s);
    fprintf(stderr, "\n[watchdog] %u s: in sub_%08X, last import %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, g_cur_import ? g_cur_import : "(none)", g_icall_count);
    fflush(stderr);
    hle_exit(4);
    return NULL;
}

static void usage(void) {
    printf("usage: stl [--run] [--game DIR] [--headless] [--windowed] [--mute] [--watchdog S] [--trace] [--verbose]\n"
           "  --run        enter the game (without it: map, bind and say what is missing)\n"
           "  --game DIR   the game folder (default: game)\n"
           "  --headless   no window, no sound device\n"
           "  --watchdog S stop after S seconds, saying where the game was\n"
           "  --trace      every import call, as it is made\n"
           "  --verbose    every stub as it is bound, every Direct3D 9 call\n"
           "  --move x,y@s --click x,y@s --rclick x,y@s --key vk@s   input, s seconds after the start\n");
}

/* The program's main (exe.c), and the app's way in. */
__attribute__((visibility("default"))) int stl_main(int argc, char **argv) {
    const char *game = "game";
    int run = 0;
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        int n = script_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--windowed")) g_windowed = 1;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--mute")) hle_dsound_set_master(0.0f);
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--trace")) win32hle_trace = 1;
        else if (!strcmp(argv[i], "--verbose")) stl_verbose = 1;
        else { usage(); return !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ? 0 : 1; }
    }
    char game_full[PATH_MAX];
    if (!realpath(game, game_full)) { fprintf(stderr, "no game folder at %s\n", game); return 1; }

    {   /* faults report on their own stack: a guest stack overflow is one */
        static uint8_t alt[1 << 16];
        stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
        sigaltstack(&ss, NULL);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
    }

    win32hle_register(stl_shims);        /* first: the game's own answers win */
    win32hle_register(stl_d3d9_shims);
    win32hle_register(stl_wide_shims);
    win32hle_register(win32hle_eh32);
    recomp_host_init();
    seed_registry();
    hle_set_drive('C', game_full);
    {   /* the user's folders (SHGetFolderPathA): drive U:, beside the game */
        static char user[PATH_MAX + 8];
        char *slash = strrchr(game_full, '/');
        snprintf(user, sizeof user, "%.*s/User", slash ? (int)(slash - game_full) : 1, game_full);
        mkdir(user, 0755);
        stl_set_user_root(user);
    }
    printf(STL_TITLE " recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    /* The modules, before anything allocates: the heap is placed above them. */
    if (stl_load(game_full) != 0) return 1;
    hle_set_command_line("\"C:\\Legacy.exe\"");
    printf("  %d modules mapped and bound, %d imports not implemented yet%s\n", g_stl_nmod, g_stl_stubs,
           g_stl_stubs && !stl_verbose ? " (--verbose lists them)" : "");
    if (!run) {
        printf("\n(dry run: --run enters 0x%08X)\n", g_stl_mod[0].img.entry);
        return 0;
    }

    if (chdir(game_full) != 0) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    if (hle_screen_open(STL_TITLE, !g_windowed, g_headless) != 0) return 1;
    if (g_watchdog_s) {
        pthread_t t;
        pthread_create(&t, NULL, watchdog, NULL);
        pthread_detach(t);
    }
    hle_on_exit = on_exit_summary;
    if (stl_attach_dlls() != 0) return 1;
    script_start();
    printf("  entering 0x%08X\n\n", g_stl_mod[0].img.entry);
    fflush(stdout);
    uint32_t rc = hle_call_guest(g_stl_mod[0].img.entry, 0, NULL);
    printf("\nentry returned eax=%08X\n", rc);
    hle_exit((int)rc);
    return 0;
}
