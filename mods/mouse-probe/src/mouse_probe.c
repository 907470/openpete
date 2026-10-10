/*
 * mouse-probe - the host mouse as a mod sees it.
 *
 * Two readers with separate delta markers: the tick hook (sim worker, once
 * per game tick) and the UI section (present thread, once per present).
 * Each logs or draws its own reading; the deltas of one never include the
 * other's motion because the engine keeps a marker per (mod, hook kind).
 */
#include <openpete_mod_api.h>
#include <openpete_mod_ui.h>

#include <string.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

static openpete_mod_mouse_t s_tick;      /* last tick-hook reading */
static openpete_mod_mouse_t s_present;   /* last present-side reading */
static unsigned s_ticks, s_presents;
static int      s_fire_was;

static void publish_status(void) {
    g_api->ui_status(g_self, "tick: pos %.0f,%.0f mask 0x%x owned %d captured %d",
                     s_tick.px, s_tick.py, s_tick.buttons,
                     s_tick.owned_by_overlay, s_tick.captured);
}

/** @brief Tick hook: read the mouse and log every change. */
static void on_tick(CPUState* cpu) {
    openpete_mod_mouse_t m;
    memset(&m, 0, sizeof m);
    m.struct_size = sizeof m;
    if (g_api->mouse_read(g_self, &m) == 0) {
        if (m.dx != 0.0f || m.dy != 0.0f || m.wheel != 0.0f ||
            m.buttons != s_tick.buttons || m.owned_by_overlay != s_tick.owned_by_overlay ||
            m.captured != s_tick.captured) {
            g_api->log(g_self, OP_MOD_LOG_INFO,
                       "tick %u: pos %.0f,%.0f ov %.3f,%.3f d %.0f,%.0f wheel %.2f "
                       "mask 0x%x in %d captured %d owned %d",
                       s_ticks, m.px, m.py, m.x, m.y, m.dx, m.dy, m.wheel,
                       m.buttons, m.in_window, m.captured, m.owned_by_overlay);
            s_tick = m;
            publish_status();
        }
    }
    int fire = g_api->binding_down(g_self, "fire");
    if (fire != s_fire_was) {
        g_api->log(g_self, OP_MOD_LOG_INFO, "fire=%d (mouse:right) tick %u", fire, s_ticks);
        s_fire_was = fire;
    }
    s_ticks++;
    g_api->base(cpu);
}

/** @brief Always section: read from the present cadence and draw it. */
static void ui_draw(const openpete_mod_ui_t* ui) {
    openpete_mod_mouse_t m;
    memset(&m, 0, sizeof m);
    m.struct_size = sizeof m;
    if (g_api->mouse_read(g_self, &m) == 0) {
        if (m.dx != 0.0f || m.dy != 0.0f || m.wheel != 0.0f ||
            m.buttons != s_present.buttons) {
            g_api->log(g_self, OP_MOD_LOG_INFO,
                       "present %u: d %.0f,%.0f wheel %.2f mask 0x%x owned %d",
                       s_presents, m.dx, m.dy, m.wheel, m.buttons, m.owned_by_overlay);
        }
        s_present = m;
    }
    s_presents++;
    ui->text("mouse-probe present: pos %.0f,%.0f d %.0f,%.0f wheel %.2f mask 0x%x",
             s_present.px, s_present.py, s_present.dx, s_present.dy,
             s_present.wheel, s_present.buttons);
}
OPENPETE_MOD_UI_SECTION_FLAGS(ui_draw, OPENPETE_MOD_UI_ALWAYS)

/**
 * @brief Attach @p fn to the running game's once-per-gameplay-tick camera update.
 *
 * Spyro 1 binds CameraUpdate by name. Spyro 2 and 3 name no such function
 * yet, so their arms bind the main-executable address: Spyro 2
 * func_8001D87C (src/camera.c), Spyro 3 func_8001204C (src/camera.c,
 * called from the UPDATE_CAMERA arm of func_80055294). Each is called once
 * per gameplay tick; the Spyro 2 and 3 calls stop during a portal exit and
 * level load and resume a few ticks into the fade-in.
 * @param api The engine table.
 * @param self This mod's handle.
 * @param fn The override body.
 * @return The override call's result, 0 on success.
 */
static int bind_tick_clock(const openpete_mod_api_t* api, openpete_mod_t* self,
                           openpete_mod_override_fn fn) {
#if OPENPETE_GAME_SPYRO1
    return api->override_name(self, "CameraUpdate", fn);
#elif OPENPETE_GAME_SPYRO2
    return api->override_addr(self, 0x8001D87Cu, fn);
#elif OPENPETE_GAME_SPYRO3
    return api->override_addr(self, 0x8001204Cu, fn);
#else
#error "mouse-probe: no camera clock binding for this game"
#endif
}

/**
 * @brief Mod entry: applies the capture row and attaches the per-tick sampler to the camera clock.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return -1 on an engine below api 12, else the override registration's result, 0 on success.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    if (api->api_version < 12u) {
        api->log(self, OP_MOD_LOG_ERROR, "needs api 12 (engine has %u)", api->api_version);
        return -1;
    }
    api->mouse_capture(self, api->config_bool(self, "capture", 0) ? 1 : 0);
    publish_status();
    return bind_tick_clock(api, self, on_tick);
}
