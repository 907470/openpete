#include <openpete_mod_api.h>
static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;

/* std140 mirror of sepia.frag's sepia_params block. */
typedef struct { float strength; } sepia_params_t;

static int   g_fx = -1;          /* postfx handle, -1 = never armed      */
static const char* g_why = "not armed";  /* why, when g_fx < 0           */
static float g_strength = 1.0f;  /* 0 = untouched scene, 1 = full sepia  */
static int   g_marker = 1;       /* draw the under-HUD proof marker      */
static unsigned g_presents;

static void push_strength(void) {
    sepia_params_t p;
    p.strength = g_strength;
    g_api->postfx_set_uniforms(g_self, g_fx, &p, sizeof p);
}

/* Present hook: a cyan HUD text marker drawn AFTER the SCENE pass, so it
 * must stay cyan - a colour a sepia tone cannot produce, the under-HUD
 * proof. Overlay space is [0,aspect] × [0,1]; the marker sits in the
 * top-left. */
static void on_present(const openpete_present_ctx_t* ctx) {
    (void)ctx;
    g_presents++;
    if (g_marker) g_api->draw_text(0.04f, 0.04f, 0.05f, 0x00FFFFu, "SCENE");
}

/* Status lines: the armed/unavailable verdict settles at entry and
 * never changes, so one paint suffices. */
static void publish_status(void) {
    if (g_fx < 0) {
        g_api->ui_status(g_self, "%s", g_why);
        return;
    }
    g_api->ui_status(g_self, "SCENE grade armed (sepia %.2f%s)",
                     (double)g_strength,
                     g_marker ? ", proof marker on" : "");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    /* No version check: mod.toml's `api` pin is the minimum this mod
     * needs, and the loader refuses an older engine before entry. */
    g_strength = (float)api->config_float(self, "sepia.strength", 1.0);
    if (g_strength < 0.0f) g_strength = 0.0f;
    if (g_strength > 1.0f) g_strength = 1.0f;
    g_marker = api->config_bool(self, "sepia.marker", 1);
    int fs = api->shader_register(self, "shaders/sepia.frag");
    if (fs < 0) {
        g_why = "sepia.frag failed to compile";
        api->log(self, OP_MOD_LOG_ERROR, "sepia.frag failed");
        publish_status(); return 0;
    }
    int fx = api->postfx_register(self, fs, OP_POSTFX_SCENE, 0);
    if (fx < 0) {
        g_why = "SCENE point unavailable in this engine build";
        api->log(self, OP_MOD_LOG_INFO, "SCENE point unavailable");
        publish_status(); return 0;
    }
    g_fx = fx;
    push_strength();
    api->postfx_enable(self, fx, 1);
    api->log(self, OP_MOD_LOG_INFO, "scene sepia armed (SCENE point)");
    publish_status();
    return api->register_present_hook(self, on_present);
}
