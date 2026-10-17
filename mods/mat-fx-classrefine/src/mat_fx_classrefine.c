/**
 * @file mat_fx_classrefine.c
 * @brief Material resolution fixture: a class-keyed moby refine retuned every present.
 */
#include <openpete_mod_api.h>

/** Class used where the game serves no class census. */
#define FALLBACK_CLASS 1

static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;
static int g_mat = -1;

/**
 * @brief Claim an instance whose guest address has bit 4 clear; decline the rest.
 * @param key The candidate.
 * @param out The uniform block, std140 vec4 rgb + strength.
 * @param cap Bytes available at out.
 * @return 16, or -1 to decline.
 */
static int refine(const openpete_mod_material_key_t* key, void* out, uint32_t cap) {
    if (cap < 16 || key->channel != OP_CHAN_MOBY) return -1;
    if (key->u.moby.vaddr & 0x10u) return -1;
    float* v = (float*)out;
    v[0] = 0.0f; v[1] = 1.0f; v[2] = 1.0f; v[3] = 0.6f;
    return 16;
}

/**
 * @brief Return the middle class of the current level's loaded classes, or FALLBACK_CLASS.
 * @param offset Entries past the middle.
 * @return A class id.
 */
static int pick_class(int offset) {
    uint16_t cls[768];
    const int n = g_api->moby_classes(g_self, cls, 768);
    if (n <= 0) return FALLBACK_CLASS + offset;
    return cls[(n / 2 + offset) % n];
}

/**
 * @brief Retune the material to the picked class on every present, changed or not.
 * @param ctx The present.
 */
static void on_present(const openpete_present_ctx_t* ctx) {
    (void)ctx;
    if (g_mat < 0) return;
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_MOBY;
    sel.moby_class = pick_class(0);
    g_api->material_set_selector(g_self, g_mat, &sel);
}

/**
 * @brief Register the class-keyed refine and the present hook that retunes it.
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
    g_mat = api->material_register_refine(self, &sel, fs, refine);
    api->register_present_hook(self, on_present);
    return 0;
}
