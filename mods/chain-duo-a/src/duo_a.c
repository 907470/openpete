/* chain-duo-a — rung 4: prove stacking + order semantics (see mod.toml).
 * Logs the nesting once: "A>" before base(), "<A" after. */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int                       g_logged;
static unsigned                  g_fires;

static void on_camera_update(CPUState* cpu) {
    int log_this = !g_logged;
    g_logged = 1;
    g_fires++;
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain enter A>");
    g_api->base(cpu);
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain exit  <A");
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    ui->text("CameraUpdate fires: %u", g_fires);
    if (!g_fires) ui->text_disabled("waiting for the first CameraUpdate");
    ui->text("nesting: A>  base()  <A");
    ui->tooltip("base() descends to the next mod overriding CameraUpdate, "
                "then to the original. Enabled-list order decides who "
                "wraps whom: later in enabled.txt runs first.");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    api->register_ui(self, ui_draw, NULL);
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
