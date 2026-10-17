/**
 * @file mat_fx_levelkey.c
 * @brief Material resolution fixture: a declarative keyed to another level over a refine on the same moby class.
 */
#include <openpete_mod_api.h>
#include <stddef.h>

/** Class used where the game serves no class census. */
#define FALLBACK_CLASS 6

static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;
static int g_decl = -1, g_refine = -1;
static int g_level = -2, g_class = -2;

/**
 * @brief Claim every instance with a fixed tint.
 * @param key The candidate.
 * @param out The uniform block, std140 vec4 rgb + strength.
 * @param cap Bytes available at out.
 * @return 16, or -1 off the moby channel.
 */
static int refine(const openpete_mod_material_key_t* key, void* out, uint32_t cap) {
    if (cap < 16 || key->channel != OP_CHAN_MOBY) return -1;
    float* v = (float*)out;
    v[0] = 1.0f; v[1] = 0.0f; v[2] = 0.0f; v[3] = 0.7f;
    return 16;
}

/**
 * @brief Return a class from the current level's loaded classes, or FALLBACK_CLASS.
 * @return A class id.
 */
static int pick_class(void) {
    uint16_t cls[768];
    const int n = g_api->moby_classes(g_self, cls, 768);
    if (n <= 0) return FALLBACK_CLASS;
    return cls[(n / 2 + 4) % n];
}

/**
 * @brief Retune both materials when the level or the picked class changes.
 * @param ctx The present.
 */
static void on_present(const openpete_present_ctx_t* ctx) {
    (void)ctx;
    const int level = g_api->level_id(g_self, NULL);
    const int cls = pick_class();
    if (level < 0 || (level == g_level && cls == g_class)) return;
    g_level = level;
    g_class = cls;
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_MOBY;
    sel.moby_class = cls;
    g_api->material_set_selector(g_self, g_refine, &sel);
    sel.level = level + 1;
    g_api->material_set_selector(g_self, g_decl, &sel);
}

/**
 * @brief Register the level-keyed declarative first, then the refine, and the retuning present hook.
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
    sel.channel = OP_CHAN_MOBY;
    sel.moby_class = FALLBACK_CLASS;
    sel.level = 0x7FFF;
    g_decl = api->material_register(self, &sel, fs);
    const float yellow[4] = {1.0f, 1.0f, 0.0f, 0.7f};
    if (g_decl >= 0) api->material_set_params(self, g_decl, yellow, sizeof yellow);
    sel.level = -1;
    g_refine = api->material_register_refine(self, &sel, fs, refine);
    if (g_decl >= 0 && g_refine >= 0) api->register_present_hook(self, on_present);
    return 0;
}
