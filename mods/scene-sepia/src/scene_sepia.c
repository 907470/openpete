#include <openpete_mod_api.h>
static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;

/* std140 mirror of sepia.frag's sepia_params block. */
typedef struct { float strength; } sepia_params_t;

static int   g_fx = -1;          /* postfx handle, -1 = never armed      */
static const char* g_why = "not armed";  /* why, when g_fx < 0           */
static float g_strength = 1.0f;  /* 0 = untouched scene, 1 = full sepia  */
static int   g_strength_dirty;   /* set by the UI, consumed at present   */
static int   g_marker = 1;       /* draw the under-HUD proof marker      */
static unsigned g_presents;

static void push_strength(void) {
    sepia_params_t p;
    p.strength = g_strength;
    g_api->postfx_set_uniforms(g_self, g_fx, &p, sizeof p);
}

/* Present hook: a bright HUD marker drawn AFTER the SCENE fog pass, so
 * it must stay crisp (unfogged) — the under-HUD proof. Overlay space
 * is [0,aspect] × [0,1]; a small square in the top-left. */
static void on_present(const openpete_present_ctx_t* ctx) {
    (void)ctx;
    g_presents++;
    if (g_strength_dirty) { g_strength_dirty = 0; push_strength(); }
    if (g_marker) g_api->draw_rect(0.04f, 0.04f, 0.10f, 0.10f, 0xFF00FFFFu);
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    if (g_fx < 0) { ui->text_disabled("%s", g_why); return; }
    ui->text("SCENE grade armed — %u presents", g_presents);
    if (ui->slider_float("Sepia strength", &g_strength, 0.0f, 1.0f))
        g_strength_dirty = 1;
    ui->tooltip("Blend between the untouched scene (0) and the full "
                "sepia tone (1).");
    ui->checkbox("Under-HUD proof marker", &g_marker);
    ui->tooltip("The yellow square drawn after the SCENE pass. It stays "
                "full-colour, which is what distinguishes SCENE from "
                "COMPOSITE.");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    api->register_ui(self, ui_draw, NULL);
    int fs = api->shader_register(self, "shaders/sepia.frag");
    if (fs < 0) {
        g_why = "sepia.frag failed to compile";
        api->log(self, OP_MOD_LOG_ERROR, "sepia.frag failed"); return 0;
    }
    int fx = api->postfx_register(self, fs, OP_POSTFX_SCENE, 0);
    if (fx < 0) {
        g_why = "SCENE point unavailable in this engine build";
        api->log(self, OP_MOD_LOG_INFO, "SCENE point unavailable"); return 0;
    }
    g_fx = fx;
    push_strength();
    api->postfx_enable(self, fx, 1);
    api->log(self, OP_MOD_LOG_INFO, "scene sepia armed (SCENE point)");
    return api->register_present_hook(self, on_present);
}
