/* hud-stopwatch — rung 15: the native draw service + present hooks.
 * See mod.toml. Pattern to copy: tick hook counts sim time and draws the
 * per-tick backdrop; the present hook publishes nothing to the sim and
 * only reads mod state + ctx, formatting the smooth time at alpha. */

#include <openpete_mod_api.h>

#include <stdio.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned                  g_ticks;

static void on_camera_update(CPUState* cpu) {
    if (g_ticks++ == 0)
        g_api->log(g_self, OP_MOD_LOG_INFO, "hud-stopwatch: ticking");
    /* Backdrop panel: tick context — persists across this tick's
     * presents. Overlay space: x/y in window-height fractions from the
     * top-left; sits just under the FPS counter's corner. */
    g_api->draw_rect(0.012f, 0.050f, 0.170f, 0.042f, 0xFF1A1A28u);
    g_api->base(cpu);
}

static void on_present(const openpete_present_ctx_t* ctx) {
    /* Gameplay-time stopwatch, sub-tick smooth: the mod owns its curve —
     * evaluate elapsed sim time at alpha, never rely on engine interp. */
    double t = ((double)g_ticks + (double)ctx->alpha) / 29.97;
    unsigned mins = (unsigned)(t / 60.0);
    double   secs = t - (double)mins * 60.0;
    char buf[24];
    snprintf(buf, sizeof buf, "%u:%05.2f", mins, secs);
    g_api->draw_text(0.022f, 0.058f, 0.026f, 0x605010u, buf);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->register_present_hook(self, on_present) != 0) return -1;
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
