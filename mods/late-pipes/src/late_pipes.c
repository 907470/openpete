/**
 * @file late_pipes.c
 * @brief Harness fixture: registers postfx passes and materials after boot.
 *
 * The passes and materials register either at entry or from the toggle
 * hook at the first enable after the entry (late.at_entry). Registration
 * happens once; later toggles change nothing here, since a disabled mod's
 * passes and materials are already left out of every draw.
 */
#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;
static int g_done;       /**< registration ran */
static int g_materials;  /**< late.materials */

/**
 * @brief Register one fragment shader as a postfx pass at a point and enable it.
 * @param path Shader path inside the mod.
 * @param point OP_POSTFX_SCENE or OP_POSTFX_COMPOSITE.
 * @param order Order within this mod at the point.
 * @return 1 when the pass is registered and enabled, else 0.
 */
static int add_pass(const char* path, int point, int order) {
    const int fs = g_api->shader_register(g_self, path);
    if (fs < 0) return 0;
    const int fx = g_api->postfx_register(g_self, fs, point, order);
    if (fx < 0) return 0;
    return g_api->postfx_enable(g_self, fx, 1) == 0;
}

/**
 * @brief Register one declarative material on a channel and enable it.
 * @param shader Material fragment shader handle.
 * @param channel OP_CHAN_* channel.
 * @return 1 when the material is registered and enabled, else 0.
 */
static int add_material(int shader, int channel) {
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = channel;
    const int m = g_api->material_register(g_self, &sel, shader);
    if (m < 0) return 0;
    return g_api->material_enable(g_self, m, 1) == 0;
}

/** @brief Register the passes and materials once, logging the counts. */
static void register_all(void) {
    if (g_done) return;
    g_done = 1;
    int fx = add_pass("shaders/scene_a.frag", OP_POSTFX_SCENE, 0);
    fx += add_pass("shaders/scene_b.frag", OP_POSTFX_SCENE, 1);
    fx += add_pass("shaders/composite.frag", OP_POSTFX_COMPOSITE, 0);
    int mats = 0;
    if (g_materials) {
        const int sh = g_api->shader_register(g_self, "shaders/material.frag");
        if (sh >= 0) {
            mats += add_material(sh, OP_CHAN_TERRAIN);
            mats += add_material(sh, OP_CHAN_MOBY);
        }
    }
    g_api->log(g_self, OP_MOD_LOG_INFO, "late-pipes: registered %d postfx passes, %d materials",
               fx, mats);
}

/**
 * @brief Toggle hook: register at the first enable after the entry.
 * @param cpu Guest CPU (unused).
 * @param on 1 on an enable.
 */
static void on_toggle(CPUState* cpu, int on) {
    (void)cpu;
    if (on) register_all();
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    g_materials = api->config_bool(self, "late.materials", 1);
    if (api->config_bool(self, "late.at_entry", 1)) register_all();
    return api->register_toggle_hook(self, on_toggle);
}
