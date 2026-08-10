/* hud-stopwatch — rung 15: the native draw service + present hooks.
 * See mod.toml. Pattern to copy: tick hook counts sim time and draws the
 * per-tick backdrop; the present hook publishes nothing to the sim and
 * only reads mod state + ctx, formatting the smooth time at alpha. */

#include <openpete_mod_api.h>

#include <stdio.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned                  g_ticks;
static int                       g_started;

/* Overlay-space layout of the readout, live from the UI section below.
 * x/y are the text origin (window-height fractions from the top-left);
 * the backdrop panel is derived from them so it keeps hugging the
 * digits at any size. */
static float g_text_x = 0.022f;   /* sits just under the FPS counter */
static float g_text_y = 0.058f;
static float g_cap_h  = 0.026f;

/* Backdrop geometry as multiples of the glyph cap height: the text's
 * inset from the panel's top-left corner, and the panel's extent —
 * sized for the seven glyphs of "M:SS.hh". */
#define PANEL_PAD_X 0.39f
#define PANEL_PAD_Y 0.31f
#define PANEL_W     6.54f
#define PANEL_H     1.62f

/* Set by the UI's Reset button, consumed by the next sim tick. */
static int g_reset_request;

/* "M:SS.hh" for an elapsed count of sim ticks (fractional ticks give the
 * sub-tick hundredths). Time base: sim ticks / the canonical NTSC tick rate
 * — gameplay time, so it matches what the engine paces to rather than
 * drifting against it. */
static void format_elapsed(char* buf, size_t cap, double ticks) {
    double   t    = ticks / OPENPETE_TICK_HZ;
    unsigned mins = (unsigned)(t / 60.0);
    double   secs = t - (double)mins * 60.0;
    snprintf(buf, cap, "%u:%05.2f", mins, secs);
}

static void on_camera_update(CPUState* cpu) {
    if (!g_started) {
        g_started = 1;
        g_api->log(g_self, OP_MOD_LOG_INFO, "hud-stopwatch: ticking");
    }
    if (g_reset_request) {
        g_reset_request = 0;
        g_ticks = 0;
    }
    g_ticks++;
    /* Backdrop panel: tick context — persists across this tick's
     * presents. */
    g_api->draw_rect(g_text_x - PANEL_PAD_X * g_cap_h,
                     g_text_y - PANEL_PAD_Y * g_cap_h,
                     PANEL_W * g_cap_h, PANEL_H * g_cap_h, 0xFF1A1A28u);
    g_api->base(cpu);
}

static void on_present(const openpete_present_ctx_t* ctx) {
    /* Gameplay-time stopwatch, sub-tick smooth: the mod owns its curve —
     * evaluate elapsed sim time at alpha, never rely on engine interp. */
    char buf[24];
    format_elapsed(buf, sizeof buf, (double)g_ticks + (double)ctx->alpha);
    g_api->draw_text(g_text_x, g_text_y, g_cap_h, 0x605010u, buf);
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    char buf[24];
    format_elapsed(buf, sizeof buf, (double)g_ticks);
    ui->text("elapsed %s  (%u ticks)", buf, g_ticks);
    ui->text_disabled("whole ticks here; the HUD readout runs at present alpha");
    if (ui->button("Reset")) g_reset_request = 1;
    ui->tooltip("Zeroes the stopwatch on the next sim tick.");

    ui->separator();
    ui->slider_float("X", &g_text_x, 0.0f, 1.7f);
    ui->tooltip("Overlay space: x spans 0..aspect from the left edge.");
    ui->slider_float("Y", &g_text_y, 0.0f, 1.0f);
    ui->tooltip("Overlay space: y spans 0..1 from the top edge.");
    ui->slider_float("Cap height", &g_cap_h, 0.010f, 0.100f);
    ui->tooltip("Glyph height; the backdrop panel scales with it.");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    api->register_ui(self, ui_draw, NULL);
    if (api->register_present_hook(self, on_present) != 0) return -1;
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
