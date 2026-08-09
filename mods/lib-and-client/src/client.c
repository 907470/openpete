/* lib-and-client — rung 5 client: import a cross-mod service. */

#include <openpete_mod_api.h>

typedef int (*magic_fn)(void);

static magic_fn s_magic;    /* set once the import resolves at entry */
static int s_last_value;    /* result of the most recent magic() call */
static int s_call_count;    /* magic() calls made since entry */

static int call_magic(void) {
    s_last_value = s_magic();
    s_call_count++;
    return s_last_value;
}

/* Overlay section: the import's state. Only reachable once the import
 * resolved — entry fails the mod otherwise. The imported symbol is a
 * plain host-side function in demo-lib's module, so re-calling it here
 * is sim-neutral and safe inside the render-pure callback. */
static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    ui->text("demo-lib:magic resolved -> %d", s_last_value);
    ui->text("calls: %d", s_call_count);
    if (ui->button("Call demo-lib:magic")) {
        call_magic();
    }
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    s_magic = (magic_fn)api->import_sym(self, "demo-lib:magic");
    if (!s_magic) {
        /* Shouldn't happen — deps topo-sort loads demo-lib first — but
         * degrade loudly rather than crash. */
        api->log(self, OP_MOD_LOG_ERROR, "demo-lib:magic missing");
        return -1;
    }
    api->register_ui(self, ui_draw, NULL);
    api->log(self, OP_MOD_LOG_INFO,
             "imported demo-lib:magic -> %d", call_magic());
    return 0;
}
