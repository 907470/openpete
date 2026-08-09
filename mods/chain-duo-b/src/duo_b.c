/* chain-duo-b — rung 4: prove stacking + order semantics (see
 * chain-duo-a). Logs the nesting once: "B>" before base(), "<B" after. */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int                       g_logged;
static int                       g_calls;
static int                       g_relog; /* UI request: re-arm the log */

static void on_camera_update(CPUState* cpu) {
    int log_this;
    if (g_relog) {
        g_relog  = 0;
        g_logged = 0;
    }
    log_this = !g_logged;
    g_logged = 1;
    g_calls++;
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain enter B>");
    g_api->base(cpu);
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain exit  <B");
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    ui->text("CameraUpdate calls: %d", g_calls);
    ui->text_disabled("half B of the pair: logs \"B>\" before base(),"
                      " \"<B\" after");
    ui->text_disabled("enabled-list order decides which half is outermost");
    if (ui->button("Log nesting again")) g_relog = 1;
    ui->tooltip("Re-arms the one-shot nesting log for the next call.");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    api->register_ui(self, ui_draw, NULL);
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
