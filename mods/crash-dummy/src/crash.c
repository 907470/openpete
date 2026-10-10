/* crash-dummy - deliberate segfault inside an override, so the
 * crash handler's attribution line can be asserted. See mod.toml. */

#include <openpete_mod_api.h>

#define AUTO_CRASH_AT 100u

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned                  g_calls;

/* Host-side controls published by the overlay section, consumed inside
 * the override - attribution only names the mod when the fault happens
 * on the chain frame, so the UI never dereferences anything itself. */
static int g_auto_armed = 1;
static int g_crash_held;

/* Status lines, repainted from the override (per tick). */
static void publish_status(void) {
    g_api->ui_status(g_self, "CameraUpdate calls: %u", g_calls);
    if (g_auto_armed) {
        if (g_calls < AUTO_CRASH_AT)
            g_api->ui_status(g_self, "armed: crashing on call %u (%u to go)",
                             AUTO_CRASH_AT, AUTO_CRASH_AT - g_calls);
        else
            g_api->ui_status(g_self, "auto crash point already passed");
    } else {
        g_api->ui_status(g_self, "auto crash disarmed (crash.auto_arm)");
    }
    g_api->ui_status(g_self, "\"crash\" binding faults on the next "
                     "CameraUpdate - the report must name this mod");
}

static void crash_now(const char* why) {
    g_api->log(g_self, OP_MOD_LOG_WARN,
               "dereferencing NULL on purpose - %s", why);
    *(volatile int*)0 = 1; /* boom */
}

static void on_camera_update(CPUState* cpu) {
    int down;
    g_calls++;
    /* Crash on the binding's press edge - a held key must not re-fault
     * a recovering process. */
    down = g_api->binding_down(g_self, "crash");
    if (down && !g_crash_held) {
        crash_now("attribution test (crash binding)");
    }
    g_crash_held = down;
    if (g_auto_armed && g_calls == AUTO_CRASH_AT) {
        crash_now("attribution test");
    }
    g_api->base(cpu);
    publish_status();
}


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
#error "crash-dummy: no camera clock binding for this game"
#endif
}

/**
 * @brief Mod entry: reads crash.auto_arm and attaches the faulting override to the camera clock.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return The override registration's result, 0 on success.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    g_auto_armed = api->config_bool(self, "crash.auto_arm", 1);
    publish_status();
    return bind_tick_clock(api, self, on_camera_update);
}
