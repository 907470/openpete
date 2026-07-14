#include <openpete_mod_api.h>
static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;

/* Present hook: a bright HUD marker drawn AFTER the SCENE fog pass, so
 * it must stay crisp (unfogged) — the under-HUD proof. Overlay space
 * is [0,aspect] × [0,1]; a small square in the top-left. */
static void on_present(const openpete_present_ctx_t* ctx) {
    (void)ctx;
    g_api->draw_rect(0.04f, 0.04f, 0.10f, 0.10f, 0xFF00FFFFu); /* yellow */
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    int fs = api->shader_register(self, "shaders/sepia.frag");
    if (fs < 0) { api->log(self, OP_MOD_LOG_ERROR, "sepia.frag failed"); return 0; }
    int fx = api->postfx_register(self, fs, OP_POSTFX_SCENE, 0);
    if (fx < 0) { api->log(self, OP_MOD_LOG_INFO, "SCENE point unavailable"); return 0; }
    api->postfx_enable(self, fx, 1);
    api->log(self, OP_MOD_LOG_INFO, "scene sepia armed (SCENE point)");
    return api->register_present_hook(self, on_present);
}
