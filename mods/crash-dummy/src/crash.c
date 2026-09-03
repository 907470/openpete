/* crash-dummy — rung 6: deliberate segfault inside an override, so the
 * crash handler's attribution line can be asserted. See mod.toml. */

#include <openpete_mod_api.h>

#define AUTO_CRASH_AT 100u

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned                  g_calls;

/* Host-side controls published by the overlay section, consumed inside
 * the override — attribution only names the mod when the fault happens
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
               "dereferencing NULL on purpose — %s", why);
    *(volatile int*)0 = 1; /* boom */
}

static void on_camera_update(CPUState* cpu) {
    int down;
    g_calls++;
    /* Crash on the binding's press edge — a held key must not re-fault
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


int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    g_auto_armed = api->config_bool(self, "crash.auto_arm", 1);
    publish_status();
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
