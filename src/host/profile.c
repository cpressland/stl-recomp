/*
 * profile.c - STL_PROFILE=N: a sampling profiler. Every millisecond of the
 * game's thread's CPU time (the thread that presents; SIGPROF from a timer
 * on its own CPU clock) its instruction pointer is kept; every N seconds (10
 * with no number) it names where those samples were, as functions: the
 * lifted game's (the executable exports them: ENABLE_EXPORTS), D3DX's,
 * win32hle's, the host's and the libraries'.
 *
 * Only the game's thread: a timer on the whole process's CPU time
 * (setitimer) interrupted the movies' and the sound's threads too, whose
 * waits gave up on the signal, and the game waited for them for ever. A
 * Mac has no timer on one thread's clock: there it is the process's, and
 * the other threads' samples are said apart.
 *
 * For "the game is slow here": run with it, play to the slow part, and the
 * lines it writes from then say which code the time went to.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifndef __APPLE__
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <sys/ucontext.h>                           /* <ucontext.h> wants _XOPEN_SOURCE there */
#else
#include <ucontext.h>
#endif

#include "stl.h"

#define RING 65536                                   /* samples between reports: 65 s at 1 kHz */

static uintptr_t g_pc[RING], g_ret[RING];           /* where, and the word at the stack's top (a leaf's return address) */
static uint8_t g_game[RING];                         /* the sample was on the game's thread */
static atomic_uint g_head;
static unsigned g_tail;
static pthread_t g_game_thread;
static int g_game_set, g_on = -1, g_every = 10;
static uint64_t g_last;

/* The instruction pointer, and who a leaf function will return to: on
 * x86-64 the word at the stack's top, on arm64 the link register. */
static uintptr_t pc_of(void *uc, uintptr_t *ret) {
    ucontext_t *u = (ucontext_t *)uc;
#if defined(__APPLE__) && defined(__aarch64__)
    *ret = (uintptr_t)u->uc_mcontext->__ss.__lr;
    return (uintptr_t)u->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__)
    *ret = *(uintptr_t *)u->uc_mcontext->__ss.__rsp;
    return (uintptr_t)u->uc_mcontext->__ss.__rip;
#elif defined(__x86_64__)
    *ret = *(uintptr_t *)u->uc_mcontext.gregs[REG_RSP];
    return (uintptr_t)u->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    *ret = (uintptr_t)u->uc_mcontext.regs[30];
    return (uintptr_t)u->uc_mcontext.pc;
#else
    (void)u;
    *ret = 0;
    return 0;
#endif
}

static volatile int g_reporting;                     /* the report's own time is not sampled */

static void on_prof(int sig, siginfo_t *si, void *uc) {
    (void)sig, (void)si;
    if (g_reporting) return;
    unsigned i = atomic_fetch_add_explicit(&g_head, 1, memory_order_relaxed) % RING;
    g_pc[i] = pc_of(uc, &g_ret[i]);
    g_game[i] = g_game_set && pthread_equal(pthread_self(), g_game_thread);
}

static uint64_t now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec; }

/* On the game's thread, at its first Present. */
static void start(void) {
    const char *e = getenv("STL_PROFILE");
    g_on = e && *e;
    if (!g_on) return;
    if (atoi(e) > 0) g_every = atoi(e);
    g_game_thread = pthread_self(), g_game_set = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof, sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
#ifdef __APPLE__
    struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
    setitimer(ITIMER_PROF, &it, NULL);
    const char *whose = "the process's (a Mac has no timer on one thread)";
#else
    timer_t tm;
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_THREAD_ID, sev.sigev_signo = SIGPROF;
#ifdef sigev_notify_thread_id
    sev.sigev_notify_thread_id = (pid_t)syscall(SYS_gettid);
#else
    sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
#endif
    struct itimerspec its = { { 0, 1000000 }, { 0, 1000000 } };
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &tm) != 0 || timer_settime(tm, 0, &its, NULL) != 0) {
        fprintf(stderr, "[profile] no timer on this thread's CPU clock: not profiling\n");
        g_on = 0;
        return;
    }
    const char *whose = "the game's thread's";
#endif
    g_last = now_s();
    fprintf(stderr, "[profile] sampling every 1 ms of %s CPU time; a report every %d s\n", whose, g_every);
}

typedef struct { const char *name; uintptr_t at; unsigned n, game; } hit_t;

/* A function's name and start, or a library's (no name: an internal one).
 * dladdr reads every symbol (65,000 of the game's), so each address is
 * looked up once a run: it was a third of the samples, its own. */
typedef struct { uintptr_t pc, at; const char *name; int named; } seen_t;
static seen_t *g_seen;
static unsigned g_nseen, g_capseen;
static const char *lookup(uintptr_t pc, uintptr_t *at, int *named);
static const char *name_of(uintptr_t pc, uintptr_t *at, int *named) {
    unsigned lo = 0, hi = g_nseen;                        /* g_seen is sorted by pc */
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (g_seen[m].pc < pc) lo = m + 1; else hi = m; }
    if (lo < g_nseen && g_seen[lo].pc == pc) { *at = g_seen[lo].at, *named = g_seen[lo].named; return g_seen[lo].name; }
    const char *name = lookup(pc, at, named);
    if (g_nseen == g_capseen) g_capseen = g_capseen ? g_capseen * 2 : 4096, g_seen = (seen_t *)realloc(g_seen, g_capseen * sizeof *g_seen);
    memmove(&g_seen[lo + 1], &g_seen[lo], (g_nseen - lo) * sizeof *g_seen);
    g_seen[lo] = (seen_t){ pc, *at, name, *named };
    g_nseen++;
    return name;
}
static const char *lookup(uintptr_t pc, uintptr_t *at, int *named) {
    Dl_info di;
    *at = 0, *named = 0;
    if (!pc || !dladdr((void *)pc, &di)) return "?";
    if (di.dli_sname) { *at = (uintptr_t)di.dli_saddr, *named = 1; return di.dli_sname; }
    if (!di.dli_fname) return "?";
    *at = (uintptr_t)di.dli_fbase;
    return strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname;
}
static void add(hit_t *h, unsigned *nh, const char *name, uintptr_t at, int game) {
    unsigned j = 0;
    while (j < *nh && !(h[j].at == at && !strcmp(h[j].name, name))) j++;
    if (j == *nh) h[j].name = name, h[j].at = at, h[j].n = h[j].game = 0, (*nh)++;
    h[j].n++, h[j].game += (unsigned)game;
}
static int by_count(const void *a, const void *b) { return (int)((const hit_t *)b)->n - (int)((const hit_t *)a)->n; }

static void report(void) {
    g_reporting = 1;
    unsigned head = atomic_load(&g_head), n = head - g_tail;
    if (n > RING) g_tail = head - RING, n = RING;
    if (!n) { g_reporting = 0; return; }
    hit_t *h = (hit_t *)calloc(n, sizeof *h), *via = (hit_t *)calloc(n, sizeof *via), *off = (hit_t *)calloc(n, sizeof *off);
    unsigned nh = 0, nv = 0, no = 0, game = 0, unnamed = 0;
    for (unsigned k = 0; k < n; k++) {
        unsigned i = (g_tail + k) % RING;
        uintptr_t at;
        int named;
        const char *name = name_of(g_pc[i], &at, &named);
        add(h, &nh, name, at, g_game[i]);
        game += g_game[i];
        if (!named) {                                     /* inside a library's own code: where in it, and who called it */
            add(off, &no, name, (g_pc[i] - at) & ~(uintptr_t)63, g_game[i]);
            uintptr_t cat;
            int cnamed;
            const char *caller = name_of(g_ret[i], &cat, &cnamed);
            add(via, &nv, caller, cat, g_game[i]);
            unnamed++;
        }
    }
    g_tail = head;
    qsort(h, nh, sizeof *h, by_count);
    qsort(via, nv, sizeof *via, by_count);
    qsort(off, no, sizeof *off, by_count);
    fprintf(stderr, "[profile] %u samples (%.0f%% on the game's thread) in %d s; the most:\n", n, 100.0 * game / n, g_every);
    for (unsigned j = 0; j < nh && j < 20; j++)
        fprintf(stderr, "[profile]   %5.1f%%  %-40s %s\n", 100.0 * h[j].n / n, h[j].name,
                h[j].game == h[j].n ? "" : h[j].game ? "(partly another thread)" : "(another thread)");
    if (unnamed) {
        fprintf(stderr, "[profile] the %.0f%% in a library's unnamed code, by who called it (a leaf's caller; a guess for others):\n",
                100.0 * unnamed / n);
        for (unsigned j = 0; j < nv && j < 8; j++)
            fprintf(stderr, "[profile]   %5.1f%%  %s\n", 100.0 * via[j].n / n, via[j].name);
        fprintf(stderr, "[profile] ...and where in it (64-byte blocks, for objdump):\n");
        for (unsigned j = 0; j < no && j < 12; j++)
            fprintf(stderr, "[profile]   %5.1f%%  %s+0x%lx\n", 100.0 * off[j].n / n, off[j].name, (unsigned long)off[j].at);
    }
    free(h), free(via), free(off);
    g_reporting = 0;
}

/* Each frame, on the game's thread (Present). */
void stl_profile_frame(void) {
    if (g_on < 0) start();
    if (!g_on) return;
    uint64_t t = now_s();
    if (t - g_last >= (uint64_t)g_every) { g_last = t; report(); }
}
