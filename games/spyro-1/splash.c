// games/spyro-1/splash.c — the first-run splash & disc resolver
// (distribution D2.5b; plan openpete-distribution-local-recomp).
//
// One screen, four jobs (plan §"First-run splash & disc resolver"):
//   DROP mode      — library empty, no --disc/--ingest: drag a .cue/.bin
//                    onto the window (SDL_EVENT_DROP_FILE) to ingest it.
//   SELECTOR mode  — several library entries: pick with ↑/↓ + Enter.
//   PROGRESS mode  — the ingest copy + first-boot module build report
//                    through spyro1_progress_cb; the splash renders a
//                    stage line + bar and keeps the window responsive.
//   In-process continuation — the splash closes and openpete_run()
//                    opens the game window; no relaunch.
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

#define SPLASH_W 640
#define SPLASH_H 360

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
    draw_text_centered(36, 4, "OPENPETE", 240, 138, 30);
    draw_text_centered(72, 2, "SPYRO THE DRAGON PC PORT", 150, 130, 190);
}

static void frame_end(const char* tag) {
    maybe_shot(tag);
    SDL_RenderPresent(s_ren);
}

void spyro1_splash_set_enabled(int enabled) { s_ui_enabled = enabled; }
int  spyro1_splash_ui_enabled(void) { return s_ui_enabled; }

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
    s_win = SDL_CreateWindow("OpenPete", SPLASH_W, SPLASH_H, 0);
    if (!s_win) return -1;
    s_ren = SDL_CreateRenderer(s_win, NULL);
    if (!s_ren) { SDL_DestroyWindow(s_win); s_win = NULL; return -1; }
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

int spyro1_splash_active(void) { return s_win != NULL; }

/* ---- progress (called from disc_library + recomp_build) --------------- */

void spyro1_splash_progress(const char* stage, int done, int total) {
    /* Lazy-open: the splash appears only when there is a real wait to
     * show (ingest copy / first-boot build) — cached warm boots never
     * flash a window. */
    if (!s_win) {
        if (!s_ui_enabled) return;
        if (spyro1_splash_open() != 0) return;
    }
    snprintf(s_stage, sizeof s_stage, "%s", stage);
    s_done = done;
    s_total = total;

    /* Keep the window responsive; the pipeline runs on this thread. */
    SDL_Event e;
    while (SDL_PollEvent(&e)) { /* swallow; no cancel mid-build in v1 */ }

    frame_begin();
    draw_text_centered(160, 2, "FIRST-TIME SETUP", 220, 210, 240);
    char line[160];
    snprintf(line, sizeof line, "%s", s_stage);
    draw_text_centered(196, 2, line, 255, 255, 255);
    SDL_FRect outline = {80, 240, SPLASH_W - 160, 22};
    SDL_SetRenderDrawColor(s_ren, 90, 70, 140, 255);
    SDL_RenderRect(s_ren, &outline);
    if (s_total > 0) {
        float frac = (float)s_done / (float)s_total;
        if (frac > 1.0f) frac = 1.0f;
        SDL_FRect fill = {82, 242, (SPLASH_W - 164) * frac, 18};
        SDL_SetRenderDrawColor(s_ren, 240, 138, 30, 255);
        SDL_RenderFillRect(s_ren, &fill);
    }
    draw_text_centered(292, 1,
        "YOUR DISC IS REBUILT FOR THIS PC. THIS HAPPENS ONCE.",
        150, 130, 190);
    frame_end("progress");
}

/* ---- drop mode --------------------------------------------------------- */

const char* spyro1_splash_wait_for_disc(void) {
    if (!s_win) return NULL;
    for (;;) {
        frame_begin();
        draw_text_centered(160, 2, "DRAG YOUR SPYRO THE DRAGON", 255, 255, 255);
        draw_text_centered(186, 2, "DISC IMAGE (.CUE / .BIN) HERE", 255, 255, 255);
        draw_text_centered(232, 1, "OR PRESS SPACE / (A) TO BROWSE",
                           220, 210, 240);
        draw_text_centered(268, 1,
            "YOUR OWN DISC IS REQUIRED. NOTHING IS DOWNLOADED.",
            150, 130, 190);
        draw_text_centered(296, 1, "ESC / (B) TO QUIT", 110, 95, 150);
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
        draw_text_centered(140, 2, "CHOOSE A VERSION", 220, 210, 240);
        int y = 180;
        for (int i = 0; i < count && i < 6; i++, y += 26) {
            char line[96];
            snprintf(line, sizeof line, "%s %s", i == sel ? ">" : " ",
                     labels[i]);
            draw_text(120, (float)y, 2, line,
                      i == sel ? 255 : 150,
                      i == sel ? 214 : 130,
                      i == sel ? 120 : 190);
        }
        draw_text_centered(320, 1,
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
