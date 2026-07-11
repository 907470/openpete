/* crash-dummy — rung 6: deliberate segfault inside an override, so the
 * crash handler's attribution line can be asserted. See mod.toml. */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned                  g_calls;

static void on_camera_update(CPUState* cpu) {
    g_calls++;
    if (g_calls == 100u) {
        g_api->log(g_self, OP_MOD_LOG_WARN,
                   "dereferencing NULL on purpose — attribution test");
        *(volatile int*)0 = 1; /* boom */
    }
    g_api->base(cpu);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
