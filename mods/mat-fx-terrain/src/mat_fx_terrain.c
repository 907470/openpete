/**
 * @file mat_fx_terrain.c
 * @brief Material resolution fixture: a texture-keyed terrain refine and a level-keyed untextured-band refine with a declining mode.
 */
#include <openpete_mod_api.h>
#include <stddef.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;
static int g_band = -1;
static int g_level = -2;

/**
 * @brief Claim the texture-1 faces with a fixed tint.
 * @param key The candidate.
 * @param out The uniform block, std140 vec4 rgb + strength.
 * @param cap Bytes available at out.
 * @return 16, or -1 off the terrain channel.
 */
static int refine_tex(const openpete_mod_material_key_t* key, void* out, uint32_t cap) {
    if (cap < 16 || key->channel != OP_CHAN_TERRAIN) return -1;
    float* v = (float*)out;
    v[0] = 0.0f; v[1] = 1.0f; v[2] = 0.0f; v[3] = 0.5f;
    return 16;
}

/**
 * @brief Claim the untextured band on an odd level id, decline it on an even one.
 * @param key The candidate.
 * @param out The uniform block, std140 vec4 rgb + strength.
 * @param cap Bytes available at out.
 * @return 16, or -1 to decline.
 */
static int refine_band(const openpete_mod_material_key_t* key, void* out, uint32_t cap) {
    if (cap < 16 || key->channel != OP_CHAN_TERRAIN) return -1;
    if ((g_level & 1) == 0) return -1;
    float* v = (float*)out;
    v[0] = 1.0f; v[1] = 0.0f; v[2] = 0.5f; v[3] = 0.5f;
    return 16;
}

/**
 * @brief Retune the band refine's level key when the level changes.
 * @param ctx The present.
 */
static void on_present(const openpete_present_ctx_t* ctx) {
    (void)ctx;
    const int level = g_api->level_id(g_self, NULL);
    if (level < 0 || level == g_level) return;
    g_level = level;
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_TERRAIN;
    sel.flags = OP_SEL_UNTEXTURED;
    sel.level = level;
    g_api->material_set_selector(g_self, g_band, &sel);
}

/**
 * @brief Register both terrain refines and the retuning present hook.
 * @param api The engine's function table.
 * @param self This mod.
 * @return 0.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    const int fs = api->shader_register(self, "shaders/tint.frag");
    if (fs < 0) return 0;
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_TERRAIN;
    sel.texture = 1;
    api->material_register_refine(self, &sel, fs, refine_tex);
    sel.texture = -1;
    sel.flags = OP_SEL_UNTEXTURED;
    g_band = api->material_register_refine(self, &sel, fs, refine_band);
    if (g_band >= 0) api->register_present_hook(self, on_present);
    return 0;
}
