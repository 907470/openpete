/* chain-duo-a — rung 4: prove stacking + order semantics (see mod.toml).
 * Logs the nesting once: "A>" before base(), "<A" after. */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int                       g_logged;
static unsigned                  g_fires;

/* Status lines, repainted from the override (per tick). base() descends
 * to the next mod overriding CameraUpdate, then the original; the
 * enabled-list order decides who wraps whom. */
static void publish_status(void) {
    g_api->ui_status(g_self, "CameraUpdate fires: %u", g_fires);
    if (!g_fires)
        g_api->ui_status(g_self, "waiting for the first CameraUpdate");
    g_api->ui_status(g_self, "nesting: A>  base()  <A");
}

static void on_camera_update(CPUState* cpu) {
    int log_this = !g_logged;
    g_logged = 1;
    g_fires++;
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain enter A>");
    g_api->base(cpu);
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain exit  <A");
    publish_status();
}


int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    publish_status();
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
