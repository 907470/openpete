/**
 * @file stopwatch.c
 * @brief hud-stopwatch: an "M:SS.hh" gameplay stopwatch drawn as HUD
 *        text, smooth at any render rate.
 *
 * Two hooks with two jobs:
 *
 * - A tick hook (an override on CameraUpdate) counts sim ticks and
 *   handles the reset binding. It runs at the game's rate, 29.913 ticks
 *   per second.
 * - A present hook formats and draws the readout. It runs once per
 *   displayed frame, which at a high --render-fps is many times per
 *   tick.
 *
 * The present hook is handed ctx->alpha, the fraction of the current
 * tick that has elapsed at this present. The mod evaluates its own time
 * at tick + alpha, so the hundredths advance between ticks instead of
 * stepping. The engine interpolates game geometry between ticks but
 * never a mod's visuals: a mod owns its own sub-tick curve, and a mod
 * that ignores alpha draws a readout that updates 29.913 times a second
 * regardless of the display rate.
 *
 * The present hook reads mod state and ctx only. It never writes guest
 * RAM: a present runs outside the tick, and a guest write from there
 * would land at a point in the tick sequence that a replay of the same
 * inputs cannot reproduce.
 */

#include <openpete_mod_api.h>

#include <stdio.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned                  g_ticks;    /**< Sim ticks since start or reset. */
static int                       g_started;  /**< First tick seen. */

/**
 * @name Readout layout
 * Overlay space: x spans 0..aspect from the left edge, y spans 0..1 from
 * the top edge, and sizes are fractions of the window height, so the
 * layout is resolution-independent. Read from the [[config]] rows at
 * entry; a committed edit in the Mods panel reloads the mod, so entry
 * re-reads them.
 * @{
 */
static float g_text_x = 0.022f;   /**< Text origin x; sits under the FPS counter. */
static float g_text_y = 0.058f;   /**< Text origin y. */
static float g_cap_h  = 0.026f;   /**< Glyph cap height. */
/** @} */

static unsigned g_resets;          /**< Completed resets. */
static int      g_reset_was_down;  /**< Key level on the previous tick, for edge detection. */

/**
 * @brief Format an elapsed tick count as "M:SS.hh".
 * @param ticks Elapsed sim ticks; a fractional part gives the sub-tick
 *              hundredths.
 *
 * The time base is sim ticks divided by OPENPETE_TICK_HZ, the NTSC-exact
 * tick rate. That is gameplay time: it matches what the engine paces to,
 * so the readout agrees with an in-game timer instead of drifting
 * against it under a frame cap or a slow machine.
 */
static void format_elapsed(char* buf, size_t cap, double ticks) {
    double   t    = ticks / OPENPETE_TICK_HZ;
    unsigned mins = (unsigned)(t / 60.0);
    double   secs = t - (double)mins * 60.0;
    snprintf(buf, cap, "%u:%05.2f", mins, secs);
}

/**
 * @brief Repaint the Mods-panel status block.
 *
 * Only facts that change rarely go here. The HUD readout is the live
 * display; publishing the time per tick through ui_status would cost a
 * string format every tick for a line nobody reads at that rate.
 */
static void publish_status(void) {
    g_api->ui_status(g_self, g_started
        ? "ticking - the HUD readout is the live display"
        : "waiting for the first sim tick");
    g_api->ui_status(g_self,
        "\"reset\" binding zeroes the stopwatch on the next sim tick");
    if (g_resets)
        g_api->ui_status(g_self, "reset %u time(s)", g_resets);
}

/**
 * @brief Tick hook: count one tick and poll the reset binding.
 *
 * binding_down reports the key level, not an event, so the mod detects
 * the press edge itself. Resetting on level would pin the count at zero
 * for as long as the key is held.
 */
static void on_camera_update(CPUState* cpu) {
    int reset_down;
    if (!g_started) {
        g_started = 1;
        g_api->log(g_self, OP_MOD_LOG_INFO, "hud-stopwatch: ticking");
        publish_status();
    }
    reset_down = g_api->binding_down(g_self, "reset");
    if (reset_down && !g_reset_was_down) {
        g_ticks = 0;
        g_resets++;
        publish_status();
    }
    g_reset_was_down = reset_down;
    g_ticks++;
    g_api->base(cpu);
}

/**
 * @brief Present hook: draw the readout at the sub-tick instant.
 * @param ctx Present context. ctx->alpha is the elapsed fraction of the
 *            current tick at this present, 0..1.
 *
 * The colour is 0xRRGGBB. draw_text draws through the engine's own HUD
 * text path, so nothing here reaches the PSX ordering table or guest RAM.
 */
static void on_present(const openpete_present_ctx_t* ctx) {
    char buf[24];
    format_elapsed(buf, sizeof buf, (double)g_ticks + (double)ctx->alpha);
    g_api->draw_text(g_text_x, g_text_y, g_cap_h, 0x605010u, buf);
}

/**
 * @brief Mod entry point: read the layout, register both hooks.
 *
 * The defaults passed to config_float duplicate the [[config]] defaults
 * in mod.toml on purpose: the manifest's default is what the Mods panel
 * shows, and the code's default is what applies when config.toml has no
 * value yet. Keeping them equal means a fresh install and a reset panel
 * agree.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    g_text_x = (float)api->config_float(self, "layout.x", 0.022);
    g_text_y = (float)api->config_float(self, "layout.y", 0.058);
    g_cap_h  = (float)api->config_float(self, "layout.cap_height", 0.026);
    publish_status();
    if (api->register_present_hook(self, on_present) != 0) return -1;
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
