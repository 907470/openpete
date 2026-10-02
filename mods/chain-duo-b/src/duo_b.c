/* chain-duo-b — proves stacking + order semantics (see
 * chain-duo-a). Logs the nesting once: "B>" before base(), "<B" after. */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int                       g_logged;
static int                       g_calls;

/* Status lines, repainted from the override (per tick). The nesting
 * log is a one-shot per load (hot reload re-arms it). */
static void publish_status(void) {
    g_api->ui_status(g_self, "CameraUpdate calls: %d", g_calls);
    g_api->ui_status(g_self, "half B of the pair: logs \"B>\" before "
                     "base(), \"<B\" after");
    g_api->ui_status(g_self,
                     "enabled-list order decides which half is outermost");
}

static void on_camera_update(CPUState* cpu) {
    int log_this;
    log_this = !g_logged;
    g_logged = 1;
    g_calls++;
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain enter B>");
    g_api->base(cpu);
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain exit  <B");
    publish_status();
}


int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    publish_status();
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
