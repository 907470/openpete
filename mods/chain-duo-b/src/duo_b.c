/* chain-duo-b — rung 4: prove stacking + order semantics (see
 * chain-duo-a). Logs the nesting once: "B>" before base(), "<B" after. */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int                       g_logged;

static void on_camera_update(CPUState* cpu) {
    int log_this = !g_logged;
    g_logged = 1;
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain enter B>");
    g_api->base(cpu);
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain exit  <B");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
