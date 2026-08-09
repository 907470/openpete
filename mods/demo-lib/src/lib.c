/* demo-lib — rung 5 library: publish a cross-mod service. */

#include <openpete_mod_api.h>

static int magic(void) { return 42; }

/* Overlay section: report the published service so a client mod's import
 * can be checked against what this library actually exports. */
static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    ui->text("exports demo-lib:magic -> int(void)");
    ui->text("current value: %d", magic());
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    api->register_ui(self, ui_draw, NULL);
    return api->export_sym(self, "magic", (void*)&magic);
}
