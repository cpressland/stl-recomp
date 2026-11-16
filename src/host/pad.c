/*
 * pad.c - a game controller for the game: SDL's GameController (an Xbox
 * pad, a PlayStation pad, a Steam Deck's controls; Xbox names below) handed
 * to win32hle's DirectInput (hle_input_pad), which shows it to the game as
 * Windows shows an Xbox 360 controller. The game's own controller support
 * does the rest (ControllerConfig.xml: A B X Y, LB RB, Back Start, the
 * sticks pressed; the sticks, the triggers and the d-pad).
 *
 * The game looks for a controller once, as it starts (DirectInput's
 * EnumDevices), as it does on Windows: one plugged in later is seen from
 * the next start. The first one found is used.
 *
 * STL_VPAD=FILE: a virtual controller (SDL's), driven by FILE, for tests
 * without one: lines "seconds control value", seconds from the start;
 * control a b x y back guide start ls rs lb rb up down left right (value 0
 * or 1) or lx ly rx ry lt rt (value -1..1, triggers 0..1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "stl.h"

static int g_on;
static SDL_GameController *g_pad;
static hle_pad_t g_last;
static uint32_t g_start_ms, g_scan_ms;

/* ---- the virtual controller (STL_VPAD) ---- */
typedef struct { double t; char what[8]; double v; } vev_t;
static vev_t *g_vev;
static int g_nvev, g_ivev, g_vindex = -1;
static SDL_Joystick *g_vjoy;
static void vpad_open(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[pad] STL_VPAD: cannot read %s\n", path); return; }
    char line[128];
    while (fgets(line, sizeof line, f)) {
        vev_t e;
        if (sscanf(line, "%lf %7s %lf", &e.t, e.what, &e.v) != 3) continue;
        g_vev = (vev_t *)realloc(g_vev, sizeof *g_vev * (size_t)(g_nvev + 1));
        g_vev[g_nvev++] = e;
    }
    fclose(f);
    int i = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    g_vjoy = i >= 0 ? SDL_JoystickOpen(i) : NULL;
    g_vindex = g_vjoy ? i : -1;
    fprintf(stderr, "[pad] STL_VPAD: a virtual controller, %d events from %s%s\n", g_nvev, path, g_vjoy ? "" : " (SDL refused it)");
}
static void vpad_step(double now_s) {
    static const char *const bn[SDL_CONTROLLER_BUTTON_MAX] = { "a", "b", "x", "y", "back", "guide", "start", "ls", "rs",
        "lb", "rb", "up", "down", "left", "right" };
    static const char *const an[SDL_CONTROLLER_AXIS_MAX] = { "lx", "ly", "rx", "ry", "lt", "rt" };
    for (; g_vjoy && g_ivev < g_nvev && g_vev[g_ivev].t <= now_s; g_ivev++) {
        const vev_t *e = &g_vev[g_ivev];
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && bn[b]; b++)
            if (!strcmp(e->what, bn[b])) SDL_JoystickSetVirtualButton(g_vjoy, b, e->v != 0);
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
            if (!strcmp(e->what, an[a])) {
                double v = e->v < -1 ? -1 : e->v > 1 ? 1 : e->v;
                SDL_JoystickSetVirtualAxis(g_vjoy, a, (Sint16)(v * 32767));
            }
        fprintf(stderr, "[pad] %.1fs virtual %s %g\n", now_s, e->what, e->v);
    }
}

/* ---- the controller ---- */
static void find_pad(uint32_t now) {
    if (g_pad && !SDL_GameControllerGetAttached(g_pad)) {
        fprintf(stderr, "[pad] %s gone\n", SDL_GameControllerName(g_pad) ? SDL_GameControllerName(g_pad) : "the controller");
        SDL_GameControllerClose(g_pad);
        g_pad = NULL;
    }
    if (g_pad || (g_scan_ms && now - g_scan_ms < 1000)) return;
    g_scan_ms = now ? now : 1;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (g_vindex >= 0 && i != g_vindex) continue;
        if (!SDL_IsGameController(i) || !(g_pad = SDL_GameControllerOpen(i))) continue;
        fprintf(stderr, "[pad] %s, as an Xbox 360 controller to the game\n",
                SDL_GameControllerName(g_pad) ? SDL_GameControllerName(g_pad) : "a controller");
        break;
    }
}
static void report(void) {
    hle_pad_t p;
    memset(&p, 0, sizeof p);
    if (g_pad) {
        static const struct { SDL_GameControllerButton b; uint16_t bit; } map[] = {
            { SDL_CONTROLLER_BUTTON_A, HLE_PAD_A }, { SDL_CONTROLLER_BUTTON_B, HLE_PAD_B },
            { SDL_CONTROLLER_BUTTON_X, HLE_PAD_X }, { SDL_CONTROLLER_BUTTON_Y, HLE_PAD_Y },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, HLE_PAD_LB }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, HLE_PAD_RB },
            { SDL_CONTROLLER_BUTTON_BACK, HLE_PAD_BACK }, { SDL_CONTROLLER_BUTTON_START, HLE_PAD_START },
            { SDL_CONTROLLER_BUTTON_LEFTSTICK, HLE_PAD_LS }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, HLE_PAD_RS },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, HLE_PAD_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, HLE_PAD_DOWN },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, HLE_PAD_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, HLE_PAD_RIGHT },
        };
        p.connected = 1;
        snprintf(p.name, sizeof p.name, "Controller (XBOX 360 For Windows)");   /* the name Windows gives one */
        p.lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
        p.ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
        p.rx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX);
        p.ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY);
        int lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        p.lt = (uint8_t)(lt < 0 ? 0 : lt / 128), p.rt = (uint8_t)(rt < 0 ? 0 : rt / 128);
        for (size_t i = 0; i < sizeof map / sizeof *map; i++)
            if (SDL_GameControllerGetButton(g_pad, map[i].b)) p.buttons |= map[i].bit;
    }
    if (memcmp(&p, &g_last, sizeof p)) {
        g_last = p;
        hle_input_pad(&p);
    }
}

/* Before the game runs (it looks for a controller once, as it starts). */
void stl_pad_start(void) {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[pad] no game controllers: %s\n", SDL_GetError());
        return;
    }
    g_on = 1;
    g_start_ms = SDL_GetTicks();
    const char *v = getenv("STL_VPAD");
    if (v && *v) vpad_open(v);
    /* A controller already connected appears a moment after the subsystem
     * starts (on a Mac, Bluetooth and USB pads come through the system's
     * own discovery, delivered as events): wait for one, up to a second,
     * since the game looks only once. */
    for (uint32_t t0 = SDL_GetTicks(); !SDL_NumJoysticks() && SDL_GetTicks() - t0 < 1000; SDL_Delay(10)) SDL_PumpEvents();
    SDL_JoystickUpdate();
    find_pad(SDL_GetTicks());
    report();
}
/* Each frame (Present, on the game's thread, holding the machine). */
void stl_pad_poll(void) {
    if (!g_on) return;
    uint32_t now = SDL_GetTicks();
    if (g_vjoy) vpad_step((now - g_start_ms) / 1000.0);
    SDL_JoystickUpdate();
    SDL_GameControllerUpdate();
    find_pad(now);
    report();
}
