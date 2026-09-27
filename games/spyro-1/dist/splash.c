// games/spyro-1/dist/splash.c — the first-run splash & disc resolver.
//
// One screen, four jobs:
//   DROP mode      — library empty, no --disc/--ingest: drag a .cue/.bin
//                    onto the window (SDL_EVENT_DROP_FILE) to ingest it.
//   SELECTOR mode  — several library entries: pick with ↑/↓ + Enter.
//   PROGRESS mode  — the ingest copy + first-boot module build report
//                    through spyro1_progress_cb; the splash renders a
//                    stage line + bar and keeps the window responsive.
//   In-process continuation — in the default windowed vk mode the game
//                    ADOPTS the splash window (spyro1_splash_detach_window;
//                    native_render retitles + reuses it) so one OS window
//                    carries splash and game; other modes close it and
//                    open their own. No relaunch either way.
//
// Headless/CI never see this (console path unchanged). All art is
// original: flat colors + the in-file 5x7 bitmap font below
// (docs/public-repo-hygiene-audit.md). Debug self-check:
// OPENPETE_SPLASH_SHOT=<path.bmp> dumps each mode's frame.
#include "splash.h"

#include "openpete_log.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tiny original 5x7 font (uppercase + digits + punctuation) ------- */

typedef struct { char ch; unsigned char rows[7]; } glyph_t;

/* Each row is 5 bits, MSB = leftmost pixel. */
static const glyph_t FONT[] = {
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x01,0x01,0x01,0x01,0x11,0x11,0x0E}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {',', {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}},
    {':', {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'/', {0x01,0x01,0x02,0x04,0x08,0x10,0x10}},
    {'(', {0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')', {0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
    {'\'',{0x04,0x04,0x08,0x00,0x00,0x00,0x00}},
    {'%', {0x19,0x1A,0x02,0x04,0x08,0x0B,0x13}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'?', {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
    {'>', {0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
};

static const unsigned char* glyph_rows(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++)
        if (FONT[i].ch == c) return FONT[i].rows;
    return NULL; /* unknown/space: skip cell */
}

/* ---- splash state ----------------------------------------------------- */

static int           s_ui_enabled;   /* set from main: !headless */
static SDL_Window*   s_win;
static SDL_Renderer* s_ren;
static char          s_stage[128];
static int           s_done, s_total;
static char          s_drop_path[4096];
static int           s_req_w, s_req_h;  /* --window override (0 = default) */

/* Logical layout space — matches Spyro 1's default game-window size
 * (game_default_window_w/h, game_layout.h), so the vk-mode window handoff
 * needs no resize. An explicit
 * --window size is honored via s_req_w/h; SDL's logical presentation
 * scales this layout to whatever the real window size is. */
#define SPLASH_W 640
#define SPLASH_H 448
/* The screens below were authored in a 640x360 band; centre it. */
#define YOFF ((SPLASH_H - 360) / 2)

static void draw_text(float x, float y, int scale, const char* s,
                      Uint8 r, Uint8 g, Uint8 b) {
    SDL_SetRenderDrawColor(s_ren, r, g, b, 255);
    for (; *s; s++, x += (float)(6 * scale)) {
        const unsigned char* rows = glyph_rows(*s);
        if (!rows) continue;
        for (int ry = 0; ry < 7; ry++)
            for (int rx = 0; rx < 5; rx++)
                if (rows[ry] & (0x10 >> rx)) {
                    SDL_FRect px = {x + (float)(rx * scale),
                                    y + (float)(ry * scale),
                                    (float)scale, (float)scale};
                    SDL_RenderFillRect(s_ren, &px);
                }
    }
}

static void draw_text_centered(float y, int scale, const char* s,
                               Uint8 r, Uint8 g, Uint8 b) {
    float w = (float)(6 * scale * (int)strlen(s) - scale);
    draw_text((SPLASH_W - w) / 2.0f, y, scale, s, r, g, b);
}

static void maybe_shot(const char* tag) {
    const char* base = getenv("OPENPETE_SPLASH_SHOT");
    if (!base) return;
    char path[4200];
    snprintf(path, sizeof path, "%s.%s.bmp", base, tag);
    SDL_Surface* surf = SDL_RenderReadPixels(s_ren, NULL);
    if (surf) {
        SDL_SaveBMP(surf, path);
        SDL_DestroySurface(surf);
    }
}

static void frame_begin(void) {
    SDL_SetRenderDrawColor(s_ren, 24, 16, 40, 255);   /* deep purple */
    SDL_RenderClear(s_ren);
    draw_text_centered(36 + YOFF, 4, "OPENPETE", 240, 138, 30);
    draw_text_centered(72 + YOFF, 2, "SPYRO THE DRAGON PC PORT", 150, 130, 190);
}

static void frame_end(const char* tag) {
    maybe_shot(tag);
    SDL_RenderPresent(s_ren);
}

void spyro1_splash_set_enabled(int enabled) { s_ui_enabled = enabled; }
int  spyro1_splash_ui_enabled(void) { return s_ui_enabled; }

void spyro1_splash_set_window_size(int w, int h) {
    s_req_w = w;
    s_req_h = h;
}

static SDL_Gamepad* s_pad;

static void open_first_gamepad(void) {
    if (s_pad) return;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (ids) {
        if (n > 0) s_pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
}

int spyro1_splash_open(void) {
    if (s_win) return 0;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        OPL_WARN_F("splash", "SDL video init failed: %s", SDL_GetError());
        return -1;
    }
    int win_w = (s_req_w > 0) ? s_req_w : SPLASH_W;
    int win_h = (s_req_h > 0) ? s_req_h : SPLASH_H;
    s_win = SDL_CreateWindow("OpenPete", win_w, win_h, 0);
    if (!s_win) return -1;
    s_ren = SDL_CreateRenderer(s_win, NULL);
    if (!s_ren) { SDL_DestroyWindow(s_win); s_win = NULL; return -1; }
    /* Draw in the fixed layout space whatever the real window size is
     * (an explicit --window makes them differ). */
    SDL_SetRenderLogicalPresentation(s_ren, SPLASH_W, SPLASH_H,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    open_first_gamepad();
    return 0;
}

/* Async file-dialog result (the SDL callback runs on the SDL thread
 * during event pumping). */
static int  s_dialog_pending;
static int  s_dialog_got;
static void SDLCALL on_file_chosen(void* userdata, const char* const* files,
                                   int filter) {
    (void)userdata; (void)filter;
    s_dialog_pending = 0;
    if (files && files[0]) {
        snprintf(s_drop_path, sizeof s_drop_path, "%s", files[0]);
        s_dialog_got = 1;
    }
}

static void open_file_dialog(void) {
    if (s_dialog_pending) return;
    static const SDL_DialogFileFilter filters[] = {
        { "PSX disc image (.cue, .bin)", "cue;bin" },
        { "All files", "*" },
    };
    s_dialog_pending = 1;
    s_dialog_got = 0;
    SDL_ShowOpenFileDialog(on_file_chosen, NULL, s_win, filters, 2, NULL, false);
}

void spyro1_splash_close(void) {
    if (s_pad) { SDL_CloseGamepad(s_pad); s_pad = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow(s_win); s_win = NULL; }
}

void* spyro1_splash_detach_window(void) {
    /* In-process continuation, window flavour: tear down everything the
     * splash owns EXCEPT the OS window, and hand that to the caller (the
     * native vk renderer adopts it — retitle/resize — instead of closing
     * one window and opening another). The SDL_Renderer must die here:
     * its swapchain has to release the window surface before
     * SDL_Vulkan_CreateSurface can claim it. */
    SDL_Window* win = s_win;
    if (s_pad) { SDL_CloseGamepad(s_pad); s_pad = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    s_win = NULL;
    return win;
}

int spyro1_splash_active(void) { return s_win != NULL; }

/* ---- progress (called from disc_library + recomp_build) --------------- */

/** @brief Draws one progress screen: the banner, the stage line, the bar and the footer.
 *
 * A stage starting "COMPILING MODS" takes the mods banner and footer and
 * shows done/total after the stage text; any other stage takes the
 * first-time setup banner. The bar fills done/total, clamped to 1, and is
 * empty when total is 0.
 * @param stage The stage text.
 * @param done The completed units.
 * @param total The total units. */
static void progress_render(const char* stage, int done, int total) {
    /* Mod compiles ride the same screen as the first-boot disc build
     * but with their own banner — a warm boot that only recompiles a
     * changed mod is neither "first-time" nor about the disc. */
    int mods = strncmp(stage, "COMPILING MODS", 14) == 0;
    frame_begin();
    draw_text_centered(160 + YOFF, 2,
                       mods ? "PREPARING MODS" : "FIRST-TIME SETUP",
                       220, 210, 240);
    char line[160];
    if (mods && total > 0)
        snprintf(line, sizeof line, "%s %d/%d", stage, done, total);
    else
        snprintf(line, sizeof line, "%s", stage);
    draw_text_centered(196 + YOFF, 2, line, 255, 255, 255);
    SDL_FRect outline = {80, 240 + YOFF, SPLASH_W - 160, 22};
    SDL_SetRenderDrawColor(s_ren, 90, 70, 140, 255);
    SDL_RenderRect(s_ren, &outline);
    if (total > 0) {
        float frac = (float)done / (float)total;
        if (frac > 1.0f) frac = 1.0f;
        SDL_FRect fill = {82, 242 + YOFF, (SPLASH_W - 164) * frac, 18};
        SDL_SetRenderDrawColor(s_ren, 240, 138, 30, 255);
        SDL_RenderFillRect(s_ren, &fill);
    }
    draw_text_centered(292 + YOFF, 1,
        mods ? "CHANGED MOD SOURCES ARE COMPILED FOR THIS PC."
             : "YOUR DISC IS REBUILT FOR THIS PC. THIS HAPPENS ONCE.",
        150, 130, 190);
    frame_end("progress");
}

/* Worker-task plumbing (spyro1_splash_run_task below): the blocking
 * pipeline stages run on an SDL thread so a Windows title-bar drag —
 * which traps the pumping thread in the OS modal move loop — can no
 * longer stall the compile. s_stage/s_done/s_total double as the
 * worker→UI mailbox, guarded by s_task_lock while a task is active. */
static SDL_Mutex*   s_task_lock;
static int          s_task_active;
static int          s_task_done;
static int          s_have_progress;  /* worker reported at least once */
static SDL_ThreadID s_ui_tid;

void spyro1_splash_progress(const char* stage, int done, int total) {
    if (!s_ui_enabled) return;
    if (s_task_active && s_task_lock
        && SDL_GetCurrentThreadID() != s_ui_tid) {
        /* Worker context: store only — every SDL video call must stay
         * on the UI thread; the runner loop renders the update. */
        SDL_LockMutex(s_task_lock);
        snprintf(s_stage, sizeof s_stage, "%s", stage);
        s_done = done;
        s_total = total;
        s_have_progress = 1;
        SDL_UnlockMutex(s_task_lock);
        return;
    }

    /* UI-thread path (no worker task): render directly, as before.
     * Lazy-open: the splash appears only when there is a real wait to
     * show — cached warm boots never flash a window. */
    if (!s_win && spyro1_splash_open() != 0) return;
    snprintf(s_stage, sizeof s_stage, "%s", stage);
    s_done = done;
    s_total = total;

    SDL_Event e;
    while (SDL_PollEvent(&e)) { /* swallow; no cancel mid-build in v1 */ }
    progress_render(s_stage, s_done, s_total);
}

/* ---- worker-task runner ------------------------------------------------ */

typedef struct { spyro1_task_fn fn; void* ud; int rc; } task_ctx_t;

static int SDLCALL task_main(void* p) {
    task_ctx_t* t = (task_ctx_t*)p;
    t->rc = t->fn(t->ud);
    SDL_LockMutex(s_task_lock);
    s_task_done = 1;
    SDL_UnlockMutex(s_task_lock);
    return 0;
}

static bool SDLCALL task_event_watch(void* ud, SDL_Event* e) {
    (void)ud;
    /* Keep the progress frame alive from INSIDE the Windows modal
     * move/size loop: our pump is blocked there, but WM_MOVE/WM_PAINT
     * still reach the WindowProc, which pushes these events (and runs
     * event watches) synchronously on the UI thread. */
    if (e->type != SDL_EVENT_WINDOW_EXPOSED &&
        e->type != SDL_EVENT_WINDOW_MOVED)
        return true;
    if (!s_task_active || !s_win || !s_ren) return true;
    if (SDL_GetCurrentThreadID() != s_ui_tid) return true;
    char stage[sizeof s_stage];
    int d, t, have;
    SDL_LockMutex(s_task_lock);
    have = s_have_progress;
    snprintf(stage, sizeof stage, "%s", s_stage);
    d = s_done;
    t = s_total;
    SDL_UnlockMutex(s_task_lock);
    if (have) progress_render(stage, d, t);
    return true;
}

int spyro1_splash_run_task(spyro1_task_fn fn, void* ud) {
    if (!s_ui_enabled) return fn(ud);   /* headless/console: unchanged */
    if (!s_task_lock) s_task_lock = SDL_CreateMutex();
    if (!s_task_lock) return fn(ud);
    s_ui_tid = SDL_GetCurrentThreadID();
    task_ctx_t ctx = { fn, ud, -1 };
    s_task_done = 0;
    s_have_progress = 0;
    s_task_active = 1;
    SDL_Thread* th = SDL_CreateThread(task_main, "openpete-task", &ctx);
    if (!th) {
        s_task_active = 0;
        return fn(ud);                  /* fail-soft: old inline behaviour */
    }
    int watch_added = 0;
    int open_failed = 0;
    for (;;) {
        int done, have, d, t;
        char stage[sizeof s_stage];
        SDL_LockMutex(s_task_lock);
        done = s_task_done;
        have = s_have_progress;
        snprintf(stage, sizeof stage, "%s", s_stage);
        d = s_done;
        t = s_total;
        SDL_UnlockMutex(s_task_lock);

        /* Lazy-open on the UI thread, only once the worker has something
         * to show — warm boots with nothing to build still never flash a
         * window (the task finishes before any progress arrives). */
        if (have && !s_win && !open_failed && spyro1_splash_open() != 0)
            open_failed = 1;            /* no display: task runs on, blind */

        if (s_win) {
            if (!watch_added) {
                SDL_AddEventWatch(task_event_watch, NULL);
                watch_added = 1;
            }
            SDL_Event e;
            while (SDL_PollEvent(&e)) { /* swallow; no cancel mid-build in v1 */ }
            if (have) progress_render(stage, d, t);
        }
        if (done) break;
        SDL_Delay(16);
    }
    if (watch_added) SDL_RemoveEventWatch(task_event_watch, NULL);
    SDL_WaitThread(th, NULL);
    s_task_active = 0;
    return ctx.rc;
}

/* ---- drop mode --------------------------------------------------------- */

const char* spyro1_splash_wait_for_disc(void) {
    if (!s_win) return NULL;
    for (;;) {
        frame_begin();
        draw_text_centered(160 + YOFF, 2, "DRAG YOUR SPYRO THE DRAGON", 255, 255, 255);
        draw_text_centered(186 + YOFF, 2, "DISC IMAGE (.CUE / .BIN) HERE", 255, 255, 255);
        draw_text_centered(232 + YOFF, 1, "OR PRESS SPACE / (A) TO BROWSE",
                           220, 210, 240);
        draw_text_centered(268 + YOFF, 1,
            "YOUR OWN DISC IS REQUIRED. NOTHING IS DOWNLOADED.",
            150, 130, 190);
        draw_text_centered(296 + YOFF, 1, "ESC / (B) TO QUIT", 110, 95, 150);
        frame_end("drop");

        if (s_dialog_got) return s_drop_path;

        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 100)) {
            switch (e.type) {
                case SDL_EVENT_DROP_FILE:
                    snprintf(s_drop_path, sizeof s_drop_path, "%s",
                             e.drop.data);
                    return s_drop_path;
                case SDL_EVENT_QUIT:
                    return NULL;
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.key == SDLK_ESCAPE) return NULL;
                    if (e.key.key == SDLK_SPACE || e.key.key == SDLK_RETURN)
                        open_file_dialog();
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
                        open_file_dialog();
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)
                        return NULL;
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    open_first_gamepad();
                    break;
                default: break;
            }
            if (s_dialog_got) return s_drop_path;
        }
    }
}

/* ---- selector mode ------------------------------------------------------ */

int spyro1_splash_select(const char* const* labels, int count) {
    if (!s_win || count <= 0) return -1;
    int sel = 0;
    for (;;) {
        frame_begin();
        draw_text_centered(140 + YOFF, 2, "CHOOSE A VERSION", 220, 210, 240);
        int y = 180 + YOFF;
        for (int i = 0; i < count && i < 6; i++, y += 26) {
            char line[96];
            snprintf(line, sizeof line, "%s %s", i == sel ? ">" : " ",
                     labels[i]);
            draw_text(120, (float)y, 2, line,
                      i == sel ? 255 : 150,
                      i == sel ? 214 : 130,
                      i == sel ? 120 : 190);
        }
        draw_text_centered(320 + YOFF, 1,
            "UP/DOWN OR STICK - ENTER/(A) PLAY - ESC/(B) QUIT",
            110, 95, 150);
        frame_end("selector");

        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 100)) {
            switch (e.type) {
                case SDL_EVENT_QUIT: return -1;
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.key == SDLK_ESCAPE) return -1;
                    if (e.key.key == SDLK_UP && sel > 0) sel--;
                    if (e.key.key == SDLK_DOWN && sel < count - 1) sel++;
                    if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)
                        return sel;
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP && sel > 0)
                        sel--;
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN
                        && sel < count - 1) sel++;
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) return sel;
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) return -1;
                    break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                    /* Left-stick Y, debounced by re-centering. */
                    if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                        static int armed = 1;
                        if (e.gaxis.value < -20000 && armed) { if (sel > 0) sel--; armed = 0; }
                        else if (e.gaxis.value > 20000 && armed) { if (sel < count - 1) sel++; armed = 0; }
                        else if (e.gaxis.value > -8000 && e.gaxis.value < 8000) armed = 1;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    open_first_gamepad();
                    break;
                case SDL_EVENT_DROP_FILE:
                    snprintf(s_drop_path, sizeof s_drop_path, "%s",
                             e.drop.data);
                    return -2;   /* caller: ingest this instead */
                default: break;
            }
        }
    }
}

const char* spyro1_splash_dropped_path(void) { return s_drop_path; }
