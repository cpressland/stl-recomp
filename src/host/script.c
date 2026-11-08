/*
 * Scripted input for headless runs: what a mouse and keyboard would give, at
 * set times, through win32hle's own input path (hle_input_mouse and
 * hle_input_key, as screen.c feeds them from SDL), so the game sees exactly
 * what a player's input becomes.
 *
 *   --move x,y@s        the mouse to x,y (screen pixels)
 *   --click x,y@s       the left button pressed and released there
 *   --rclick x,y@s      the right button
 *   --key vk@s          a key pressed and released (a virtual-key code:
 *                       27 Escape, 13 Return, 0x41 'A', ...)
 *
 * s is seconds from the game's entry. Events run in time order on their own
 * thread, each touch of the window manager made holding the machine
 * (mach_enter), as the game's own threads do. A button or key is held for
 * 150 ms, long enough for a game that polls once a frame to see it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "win32hle.h"

#define WM_MOUSEMOVE   0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP   0x0205u

typedef struct { char kind; int x, y; double t; } ev_t;   /* m move, c click, r right click, k key, d/u key down/up */
#define MAX_EV 256
static ev_t g_ev[MAX_EV];
static int g_nev;

/* Consume one option at argv[i]; how many argv entries it took, 0 if not ours. */
int script_arg(int argc, char **argv, int i) {
    if (i + 1 >= argc || g_nev >= MAX_EV) return 0;
    ev_t e = { 0, 0, 0, 0 };
    const char *a = argv[i + 1];
    if (!strcmp(argv[i], "--move")) e.kind = 'm';
    else if (!strcmp(argv[i], "--click")) e.kind = 'c';
    else if (!strcmp(argv[i], "--rclick")) e.kind = 'r';
    else if (!strcmp(argv[i], "--key")) e.kind = 'k';
    else if (!strcmp(argv[i], "--keydown")) e.kind = 'd';
    else if (!strcmp(argv[i], "--keyup")) e.kind = 'u';
    else return 0;
    if (e.kind == 'k' || e.kind == 'd' || e.kind == 'u') {
        char *at = strchr(a, '@');
        if (!at) return 0;
        e.x = (int)strtol(a, NULL, 0);
        e.t = atof(at + 1);
    } else if (sscanf(a, "%d,%d@%lf", &e.x, &e.y, &e.t) != 3) {
        return 0;
    }
    g_ev[g_nev++] = e;
    return 2;
}

static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }
static void mouse(uint32_t m, int x, int y) { mach_enter(); hle_input_mouse(m, x, y); mach_leave(); }
/* A key's set-1 scan code, as a keyboard's message carries it in lParam (a
 * game may read the key from it): the keys a script presses. */
static uint32_t scan_of(uint32_t vk) {
    static const char row1[] = "1234567890", qrow[] = "QWERTYUIOP", arow[] = "ASDFGHJKL", zrow[] = "ZXCVBNM";
    const char *p;
    if (vk >= '1' && vk <= '9') return 0x02 + (vk - '1');
    if (vk == '0') return 0x0B;
    if ((p = strchr(qrow, (int)vk)) && vk) return 0x10 + (uint32_t)(p - qrow);
    if ((p = strchr(arow, (int)vk)) && vk) return 0x1E + (uint32_t)(p - arow);
    if ((p = strchr(zrow, (int)vk)) && vk) return 0x2C + (uint32_t)(p - zrow);
    (void)row1;
    switch (vk) {
    case 0x1B: return 0x01;                     /* Escape */
    case 0x08: return 0x0E;                     /* Backspace */
    case 0x09: return 0x0F;                     /* Tab */
    case 0x0D: return 0x1C;                     /* Return */
    case 0x20: return 0x39;                     /* Space */
    case 0xBD: return 0x0C;                     /* - */
    case 0xBB: return 0x0D;                     /* = */
    case 0x25: return 0x4B; case 0x26: return 0x48; case 0x27: return 0x4D; case 0x28: return 0x50;   /* arrows */
    }
    if (vk >= 0x70 && vk <= 0x79) return 0x3B + (vk - 0x70);   /* F1-F10 */
    return 0;
}
static void key(int down, uint32_t vk) { mach_enter(); hle_input_key(down, vk, scan_of(vk)); mach_leave(); }

static int by_time(const void *a, const void *b) {
    double d = ((const ev_t *)a)->t - ((const ev_t *)b)->t;
    return d < 0 ? -1 : d > 0;
}
static void *run(void *arg) {
    (void)arg;
    uint32_t t0 = hle_ticks_ms();
    for (int i = 0; i < g_nev; i++) {
        const ev_t *e = &g_ev[i];
        int32_t wait = (int32_t)(e->t * 1000.0) - (int32_t)(hle_ticks_ms() - t0);
        if (wait > 0) sleep_ms((unsigned)wait);
        switch (e->kind) {
        case 'm':
            mouse(WM_MOUSEMOVE, e->x, e->y);
            fprintf(stderr, "[input] %.1fs move %d,%d\n", e->t, e->x, e->y);
            break;
        case 'c': case 'r': {
            uint32_t dn = e->kind == 'c' ? WM_LBUTTONDOWN : WM_RBUTTONDOWN, up = e->kind == 'c' ? WM_LBUTTONUP : WM_RBUTTONUP;
            mouse(WM_MOUSEMOVE, e->x, e->y);
            sleep_ms(100);
            mouse(dn, e->x, e->y);
            sleep_ms(150);
            mouse(up, e->x, e->y);
            fprintf(stderr, "[input] %.1fs %s %d,%d\n", e->t, e->kind == 'c' ? "click" : "rclick", e->x, e->y);
            break;
        }
        case 'k':
            key(1, (uint32_t)e->x);
            sleep_ms(150);
            key(0, (uint32_t)e->x);
            fprintf(stderr, "[input] %.1fs key %d\n", e->t, e->x);
            break;
        case 'd': case 'u':                     /* a key held: down, and later up */
            key(e->kind == 'd', (uint32_t)e->x);
            fprintf(stderr, "[input] %.1fs key %d %s\n", e->t, e->x, e->kind == 'd' ? "down" : "up");
            break;
        }
    }
    return NULL;
}

/* Start the script, if there is one: after the window exists. */
void script_start(void) {
    if (!g_nev) return;
    qsort(g_ev, (size_t)g_nev, sizeof *g_ev, by_time);
    pthread_t t;
    pthread_create(&t, NULL, run, NULL);
    pthread_detach(t);
}
