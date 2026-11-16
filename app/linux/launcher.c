/* Star Trek: Legacy's launcher on Linux: what Star Trek Legacy.app is on a Mac
 * (app/Launcher), drawn with SDL so it works as well with a controller in a
 * Steam Deck's Game Mode as with a mouse on a desktop.
 *
 * Play when the game is built; otherwise build it (tools/install/install.py,
 * which downloads the disc image and the 1.2 patch the first time), showing
 * its progress lines: `@step i n title`, `@progress f`, `@error text`, `@done`,
 * and ninja's `[n/m]` for the compile. The game runs as a child; the window
 * hides meanwhile, and comes back only if the game crashed.
 *
 *   cc -O2 launcher.c $(pkg-config --cflags --libs sdl2 SDL2_ttf) -o star-trek-legacy
 *
 * Where things are (the Flatpak's layout by default):
 *   STL_SHARE   the sources the build needs, as install.py --export-sources
 *               writes them, under src/ (default /app/share/star-trek-legacy)
 *   STL_PYTHON  a Python with pefile, capstone and unicorn (default
 *               /app/venv/bin/python3)
 *   STL_HOME    the game, its build and logs ($XDG_DATA_HOME/star-trek-legacy)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef STL_VERSION
#define STL_VERSION ""
#endif
#define TITLE "Star Trek: Legacy"
#define W 960
#define H 600
#define GAME_CRASHED 3          /* the host's exit after a fault (src/host/main.c, on_fault) */

/* ---- where things are ------------------------------------------------------ */

static char g_share[1024], g_python[1024], g_home[1024], g_logs[1100];

static const char *env_or(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}

static void paths(void) {
    snprintf(g_share, sizeof g_share, "%s", env_or("STL_SHARE", "/app/share/star-trek-legacy"));
    snprintf(g_python, sizeof g_python, "%s", env_or("STL_PYTHON", "/app/venv/bin/python3"));
    const char *home = getenv("STL_HOME");
    if (home && *home) {
        snprintf(g_home, sizeof g_home, "%s", home);
    } else {
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg && *xdg) snprintf(g_home, sizeof g_home, "%s/star-trek-legacy", xdg);
        else snprintf(g_home, sizeof g_home, "%s/.local/share/star-trek-legacy", env_or("HOME", "."));
    }
    snprintf(g_logs, sizeof g_logs, "%s/logs", g_home);
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

/* ---- the installer ------------------------------------------------------- */

enum phase { CHECKING, SETUP, UPDATE, WORKING, FAILED, READY, CRASHED };

static struct {
    enum phase phase;
    char message[512];          /* FAILED / CRASHED */
    char step[160];
    int step_i, step_n;
    double fraction;            /* < 0: no measure of this step */
    char last[256];             /* the last plain output line */
    Uint32 started;
    pid_t pid;                  /* the installer, while WORKING */
    int fd;
    char pending[8192];
    size_t npending;
    FILE *log;
    int cancelled, needs_disc;
} S = { .phase = CHECKING, .fraction = -1, .fd = -1 };

static void install_argv(const char **argv, int *n, const char *extra) {
    static char installer[1200], pcrecomp[1200];
    snprintf(installer, sizeof installer, "%s/src/stl/tools/install/install.py", g_share);
    snprintf(pcrecomp, sizeof pcrecomp, "%s/src/pcrecomp", g_share);
    *n = 0;
    argv[(*n)++] = g_python;
    argv[(*n)++] = "-I";
    argv[(*n)++] = installer;
    argv[(*n)++] = "--dest";
    argv[(*n)++] = g_home;
    argv[(*n)++] = "--pcrecomp";
    argv[(*n)++] = pcrecomp;
    argv[(*n)++] = extra;
    argv[*n] = NULL;
}

/* install.py --status: ready, or what a build would need. */
static void check_status(void) {
    const char *argv[12];
    int n, fds[2];
    install_argv(argv, &n, "--status");
    if (pipe(fds) != 0) goto fail;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, 2);
        close(fds[0]);
        execv(argv[0], (char **)argv);
        _exit(127);
    }
    close(fds[1]);
    char out[4096] = {0};
    size_t got = 0;
    ssize_t r;
    while (pid > 0 && got < sizeof out - 1 && (r = read(fds[0], out + got, sizeof out - 1 - got)) > 0) got += (size_t)r;
    close(fds[0]);
    int status = 0;
    if (pid > 0) waitpid(pid, &status, 0);
    if (pid <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) goto fail;
    char *line = strrchr(out, '{');          /* the last line is the JSON */
    if (!line) goto fail;
    if (strstr(line, "\"ready\": true")) { S.phase = READY; return; }
    S.needs_disc = strstr(line, "\"needs_disc\": true") != NULL;
    S.phase = S.needs_disc ? SETUP : UPDATE;
    return;
fail:
    S.phase = FAILED;
    snprintf(S.message, sizeof S.message, "The installer could not run (%.300s). Reinstalling the app may help.", g_python);
}

static void start_build(void) {
    const char *argv[12];
    int n, fds[2];
    install_argv(argv, &n, "--download");
    mkdirs(g_logs);
    char logpath[1200];
    snprintf(logpath, sizeof logpath, "%s/install.log", g_logs);
    if (S.log) fclose(S.log);
    S.log = fopen(logpath, "w");
    if (pipe(fds) != 0) { S.phase = FAILED; snprintf(S.message, sizeof S.message, "The build could not start."); return; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        setenv("STL_PGROUP", "1", 1);         /* its own process group: Cancel stops the compilers too */
        setenv("PYTHONUNBUFFERED", "1", 1);
        execv(argv[0], (char **)argv);
        fprintf(stderr, "@error cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); S.phase = FAILED; snprintf(S.message, sizeof S.message, "The build could not start."); return; }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    S.pid = pid, S.fd = fds[0], S.npending = 0;
    S.phase = WORKING, S.cancelled = 0;
    S.step[0] = S.last[0] = 0;
    snprintf(S.step, sizeof S.step, "Starting");
    S.step_i = S.step_n = 0, S.fraction = -1;
    S.started = SDL_GetTicks();
}

static void take(char *line) {
    if (!strncmp(line, "@step ", 6)) {
        int i, n, at = 0;
        if (sscanf(line + 6, "%d %d %n", &i, &n, &at) == 2 && at) {
            S.step_i = i, S.step_n = n, S.fraction = -1;
            snprintf(S.step, sizeof S.step, "%.159s", line + 6 + at);
        }
    } else if (!strncmp(line, "@progress ", 10)) {
        double f = atof(line + 10);
        S.fraction = f < 0 ? -1 : f > 1 ? 1 : f;
    } else if (!strncmp(line, "@error ", 7)) {
        S.phase = FAILED;
        snprintf(S.message, sizeof S.message, "%.511s", line + 7);
    } else if (!strcmp(line, "@done")) {
        /* the exit says it */
    } else {
        unsigned done, all;
        if (line[0] == '[' && sscanf(line, "[%u/%u]", &done, &all) == 2 && all) S.fraction = (double)done / all;
        if (*line) snprintf(S.last, sizeof S.last, "%.255s", line);
    }
}

static void pump_build(void) {
    if (S.fd < 0) return;
    char buf[4096];
    ssize_t r;
    while ((r = read(S.fd, buf, sizeof buf)) > 0) {
        if (S.log) fwrite(buf, 1, (size_t)r, S.log), fflush(S.log);
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n' || S.npending == sizeof S.pending - 1) {
                S.pending[S.npending] = 0;
                take(S.pending);
                S.npending = 0;
            } else if (buf[i] != '\r') {
                S.pending[S.npending++] = buf[i];
            }
        }
    }
    if (r == 0) {                                  /* the installer has finished */
        close(S.fd);
        S.fd = -1;
        int status = 0;
        waitpid(S.pid, &status, 0);
        S.pid = 0;
        if (S.log) fclose(S.log), S.log = NULL;
        if (S.cancelled) {
            S.phase = S.needs_disc ? SETUP : UPDATE;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            S.phase = READY, S.needs_disc = 0;
        } else if (S.phase != FAILED) {
            S.phase = FAILED;
            snprintf(S.message, sizeof S.message, "The build stopped (%s %d). Its log has the details.",
                     WIFEXITED(status) ? "exit" : "signal", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
        }
    }
}

static void cancel_build(void) {
    if (S.pid > 0) { S.cancelled = 1; kill(-S.pid, SIGTERM); }
}

/* ---- the game ---------------------------------------------------------------- */

/* Runs the game and waits. 1: it crashed (the window comes back to say so). */
static int play(SDL_Window *win, int argc, char **argv) {
    char stl[1200], game[1200], first[1200], logpath[1200], old[1200];
    snprintf(stl, sizeof stl, "%s/build/host/stl", g_home);
    snprintf(game, sizeof game, "%s/game", g_home);
    snprintf(first, sizeof first, "%s/.launched", g_home);
    mkdirs(g_logs);
    snprintf(logpath, sizeof logpath, "%s/stl.log", g_logs);
    snprintf(old, sizeof old, "%s/stl.1.log", g_logs);
    rename(logpath, old);

    const char *args[64];
    int n = 0;
    args[n++] = stl, args[n++] = "--game", args[n++] = game, args[n++] = "--run";
    /* The first time, the desktop's own size; after that the game remembers
     * whatever was chosen (User/resolution.txt). */
    if (!exists(first)) {
        args[n++] = "--resolution", args[n++] = "native";
        FILE *f = fopen(first, "w");
        if (f) fclose(f);
    }
    for (int i = 1; i < argc && n < 63; i++) args[n++] = argv[i];
    args[n] = NULL;

    SDL_HideWindow(win);
    SDL_PumpEvents();
    pid_t pid = fork();
    if (pid == 0) {
        if (!isatty(2)) {
            int fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) dup2(fd, 1), dup2(fd, 2);
        }
        execv(stl, (char **)args);
        fprintf(stderr, "cannot run %s: %s\n", stl, strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (pid > 0) while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) != GAME_CRASHED && WEXITSTATUS(status) != 127) return 0;
    S.phase = CRASHED;
    snprintf(S.message, sizeof S.message, "%s", pid < 0 || (WIFEXITED(status) && WEXITSTATUS(status) == 127)
             ? "The game could not start." : "The game stopped unexpectedly.");
    SDL_ShowWindow(win);
    SDL_RaiseWindow(win);
    return 1;
}

/* ---- drawing -------------------------------------------------------------------- */

static SDL_Renderer *R;
static TTF_Font *F_TITLE, *F_BODY, *F_SMALL, *F_BUTTON;

static TTF_Font *font(int bold, int size) {
    /* STL_FONTS first: the AppImage carries its own (a system may have none) */
    static const char *dirs[] = { "", "/usr/share/fonts/dejavu", "/usr/share/fonts/truetype/dejavu",
                                  "/usr/share/fonts/TTF", "/usr/share/fonts/dejavu-sans-fonts", NULL };
    dirs[0] = env_or("STL_FONTS", "/usr/share/fonts/dejavu");
    char p[1100];
    for (int i = 0; dirs[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", dirs[i], bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf");
        TTF_Font *f = TTF_OpenFont(p, size);
        if (f) return f;
    }
    return NULL;
}

static void text(TTF_Font *f, int x, int y, int wrap, SDL_Color c, const char *fmt, ...) {
    char s[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    if (!f || !*s) return;
    SDL_Surface *surf = wrap ? TTF_RenderUTF8_Blended_Wrapped(f, s, c, (Uint32)wrap) : TTF_RenderUTF8_Blended(f, s, c);
    if (!surf) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(R, surf);
    SDL_Rect d = { x, y, surf->w, surf->h };
    if (!wrap && d.w > W - x - 40) d.w = W - x - 40, d.h = surf->h;   /* a long line: clipped, not squeezed */
    SDL_Rect src = { 0, 0, d.w, d.h };
    SDL_RenderCopy(R, t, &src, &d);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(surf);
}

static void fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    SDL_SetRenderDrawColor(R, r, g, b, a);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderFillRect(R, &rc);
}

/* The icon's deep space, top to bottom: violet to near black. */
static void background(void) {
    for (int y = 0; y < H; y += 4) {
        double t = (double)y / H;
        Uint8 r = (Uint8)(44 - 30 * t), g = (Uint8)(30 - 20 * t), b = (Uint8)(82 - 50 * t);
        fill(0, y, W, 4, r, g, b, 255);
    }
}

/* ---- buttons --------------------------------------------------------------------- */

enum action { A_NONE, A_PLAY, A_BUILD, A_CANCEL, A_RETRY, A_QUIT };
typedef struct { const char *label; enum action act; SDL_Rect r; } button;
static button B[3];
static int NB, FOCUS;

static void buttons(int n, ...) {
    va_list ap;
    va_start(ap, n);
    NB = 0;
    int x = W - 48;
    button tmp[3];
    for (int i = 0; i < n; i++) {
        tmp[i].label = va_arg(ap, const char *);
        tmp[i].act = (enum action)va_arg(ap, int);
    }
    va_end(ap);
    /* Laid out from the right: the last given is the main one, rightmost. */
    for (int i = n - 1; i >= 0; i--) {
        int tw = 0, th = 0;
        TTF_SizeUTF8(F_BUTTON, tmp[i].label, &tw, &th);
        int w = tw + 64 < 180 ? 180 : tw + 64;
        x -= w;
        tmp[i].r = (SDL_Rect){ x, H - 48 - 64, w, 64 };
        x -= 20;
    }
    for (int i = 0; i < n; i++) B[NB++] = tmp[i];
    if (FOCUS >= NB) FOCUS = NB - 1;
}

static void draw_buttons(void) {
    for (int i = 0; i < NB; i++) {
        SDL_Rect r = B[i].r;
        int on = i == FOCUS;
        if (on) fill(r.x - 4, r.y - 4, r.w + 8, r.h + 8, 120, 180, 245, 255);
        fill(r.x, r.y, r.w, r.h, on ? 250 : 60, on ? 250 : 50, on ? 250 : 100, on ? 255 : 200);
        int tw = 0, th = 0;
        TTF_SizeUTF8(F_BUTTON, B[i].label, &tw, &th);
        SDL_Color c = on ? (SDL_Color){ 20, 20, 50, 255 } : (SDL_Color){ 255, 255, 255, 255 };
        text(F_BUTTON, r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, 0, c, "%s", B[i].label);
    }
}

/* ---- the screens ------------------------------------------------------------------- */

static const SDL_Color WHITE = { 255, 255, 255, 255 }, SOFT = { 225, 220, 245, 255 }, DIM = { 190, 185, 215, 255 };

static void draw(void) {
    background();
    const char *title = S.phase == READY || S.phase == CRASHED || S.phase == CHECKING ? TITLE
                      : S.needs_disc ? "Set up " TITLE : "Updating " TITLE;
    text(F_TITLE, 48, 48, 0, WHITE, "%s", title);
    int y = 112;
    switch (S.phase) {
    case CHECKING:
        text(F_BODY, 48, y, 0, SOFT, "Checking the game…");
        NB = 0;
        break;
    case READY:
        text(F_BODY, 48, y, 0, SOFT, *STL_VERSION ? "Ready to play. Version " STL_VERSION "." : "Ready to play.");
        buttons(2, "Quit", A_QUIT, "Play", A_PLAY);
        break;
    case SETUP:
        text(F_BODY, 48, y, W - 96, SOFT, "The game is built on this computer, once, before its first run. "
             "It downloads the game's disc image and its 1.2 patch (2.2 GB), then builds the game from them.\n\n"
             "That takes 15 minutes on a fast computer, about an hour on a Steam Deck, and about 8 GB of space.");
        buttons(2, "Quit", A_QUIT, "Download and Build", A_BUILD);
        break;
    case UPDATE:
        text(F_BODY, 48, y, W - 96, SOFT, "This version needs the game rebuilt, which takes a few minutes. "
             "Your saves are kept.");
        buttons(2, "Quit", A_QUIT, "Build", A_BUILD);
        break;
    case WORKING: {
        if (S.step_n) text(F_BODY, 48, y, 0, WHITE, "Step %d of %d: %s", S.step_i, S.step_n, S.step);
        else text(F_BODY, 48, y, 0, WHITE, "%s", S.step);
        Uint32 s = (SDL_GetTicks() - S.started) / 1000;
        int tw = 0, th = 0;
        char el[32];
        snprintf(el, sizeof el, "%u:%02u", s / 60, s % 60);
        TTF_SizeUTF8(F_BODY, el, &tw, &th);
        text(F_BODY, W - 48 - tw, y, 0, DIM, "%s", el);
        y += 48;
        fill(48, y, W - 96, 14, 30, 24, 60, 255);
        if (S.fraction >= 0) {
            fill(48, y, (int)((W - 96) * S.fraction), 14, 120, 180, 245, 255);
        } else {                                   /* no measure: a block that sweeps */
            int span = (W - 96) / 4, at = (int)(SDL_GetTicks() / 4 % (Uint32)(W - 96 + span)) - span;
            int x0 = at < 0 ? 0 : at, x1 = at + span > W - 96 ? W - 96 : at + span;
            if (x1 > x0) fill(48 + x0, y, x1 - x0, 14, 120, 180, 245, 255);
        }
        y += 36;
        text(F_SMALL, 48, y, 0, DIM, "%s", S.last);
        text(F_SMALL, 48, H - 48 - 64 + 22, 0, DIM, "You can leave it running; it needs no help.");
        buttons(1, "Cancel", A_CANCEL);
        break;
    }
    case FAILED:
    case CRASHED:
        text(F_BODY, 48, y, W - 96, WHITE, "%s", S.message);
        text(F_SMALL, 48, y + 110, W - 96, DIM, "The logs are in %s", g_logs);
        if (S.phase == FAILED) buttons(2, "Quit", A_QUIT, "Try Again", A_RETRY);
        else buttons(2, "Quit", A_QUIT, "Play Again", A_PLAY);
        break;
    }
    draw_buttons();
}

/* ---- the program ---------------------------------------------------------------- */

static SDL_Window *WIN;

static int act(enum action a, int argc, char **argv) {
    switch (a) {
    case A_PLAY:
        if (!play(WIN, argc, argv)) return 1;   /* played and quit: so does the launcher */
        break;
    case A_BUILD:
    case A_RETRY:
        if (S.phase == FAILED && !exists(g_home)) S.needs_disc = 1;
        start_build();
        break;
    case A_CANCEL: cancel_build(); break;
    case A_QUIT: return 1;
    default: break;
    }
    return 0;
}

static int hit(int x, int y) {
    for (int i = 0; i < NB; i++)
        if (x >= B[i].r.x && x < B[i].r.x + B[i].r.w && y >= B[i].r.y && y < B[i].r.y + B[i].r.h) return i;
    return -1;
}

/* STL_LAUNCHER_SHOT=FILE.bmp: draw one screen and write it out (with
 * STL_LAUNCHER_STATE=setup|update|working|failed|ready|crashed), for checks. */
static int shot(const char *path) {
    const char *st = env_or("STL_LAUNCHER_STATE", "");
    S.needs_disc = 1;
    if (!strcmp(st, "setup")) S.phase = SETUP;
    else if (!strcmp(st, "update")) S.phase = UPDATE, S.needs_disc = 0;
    else if (!strcmp(st, "working")) {
        S.phase = WORKING, S.step_i = 7, S.step_n = 7, S.fraction = .42, S.started = SDL_GetTicks() - 754000;
        snprintf(S.step, sizeof S.step, "Compiling");
        snprintf(S.last, sizeof S.last, "[37/89] Building C object CMakeFiles/stl_gen.dir/recomp_legacy_0042_0.c.o");
    } else if (!strcmp(st, "failed")) {
        S.phase = FAILED;
        snprintf(S.message, sizeof S.message, "The disc image did not download: the server could not be reached.");
    } else if (!strcmp(st, "crashed")) {
        S.phase = CRASHED;
        snprintf(S.message, sizeof S.message, "The game stopped unexpectedly.");
    } else S.phase = READY;
    FOCUS = 99;
    draw();
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_RenderReadPixels(R, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    int rc = SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    paths();
    signal(SIGPIPE, SIG_IGN);
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0 || TTF_Init() != 0) {
        fprintf(stderr, "launcher: %s\n", SDL_GetError());
        return 1;
    }
    const char *shot_path = getenv("STL_LAUNCHER_SHOT");
    WIN = SDL_CreateWindow(TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (shot_path ? SDL_WINDOW_HIDDEN : 0));
    R = WIN ? SDL_CreateRenderer(WIN, -1, shot_path ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!R && WIN) R = SDL_CreateRenderer(WIN, -1, SDL_RENDERER_SOFTWARE);
    if (!R) { fprintf(stderr, "launcher: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(R, W, H);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    F_TITLE = font(1, 40), F_BODY = font(0, 24), F_SMALL = font(0, 16), F_BUTTON = font(1, 24);
    if (!F_BODY) fprintf(stderr, "launcher: no DejaVu Sans font found; text will be missing\n");
    if (shot_path) return shot(shot_path);

    draw();
    SDL_RenderPresent(R);
    check_status();
    FOCUS = 99;                                       /* the main button (clamped to the last) */

    for (int quit = 0; !quit;) {
        SDL_Event e;
        int had = S.phase == WORKING ? SDL_WaitEventTimeout(&e, 50) : SDL_WaitEventTimeout(&e, 250);
        for (; had && !quit; had = SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                if (S.phase == WORKING) cancel_build();
                quit = 1;
                break;
            case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); break;
            case SDL_MOUSEMOTION: { int i = hit(e.motion.x, e.motion.y); if (i >= 0) FOCUS = i; break; }
            case SDL_MOUSEBUTTONUP: {
                int i = hit(e.button.x, e.button.y);
                if (e.button.button == SDL_BUTTON_LEFT && i >= 0) FOCUS = i, quit = act(B[i].act, argc, argv);
                break;
            }
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                case SDLK_LEFT: case SDLK_UP: if (FOCUS > 0) FOCUS--; break;
                case SDLK_RIGHT: case SDLK_DOWN: case SDLK_TAB: if (FOCUS < NB - 1) FOCUS++; break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                    if (FOCUS >= 0 && FOCUS < NB) quit = act(B[FOCUS].act, argc, argv);
                    break;
                case SDLK_ESCAPE:
                    if (S.phase == WORKING) cancel_build(); else quit = 1;
                    break;
                default: break;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: case SDL_CONTROLLER_BUTTON_DPAD_UP: if (FOCUS > 0) FOCUS--; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: case SDL_CONTROLLER_BUTTON_DPAD_DOWN: if (FOCUS < NB - 1) FOCUS++; break;
                case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START:
                    if (FOCUS >= 0 && FOCUS < NB) quit = act(B[FOCUS].act, argc, argv);
                    break;
                case SDL_CONTROLLER_BUTTON_B:
                    if (S.phase == WORKING) cancel_build();
                    break;
                default: break;
                }
                break;
            default: break;
            }
        }
        enum phase before = S.phase;
        pump_build();
        if (S.phase != before) FOCUS = 99;
        draw();
        SDL_RenderPresent(R);
    }
    if (S.pid > 0) { int st; waitpid(S.pid, &st, 0); }
    TTF_Quit();
    SDL_Quit();
    return 0;
}
