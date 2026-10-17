/**
 * @file mat_fx_wildrefine.c
 * @brief Material resolution fixture: a channel-wide moby refine claiming mobys of even class id.
 */
#include <openpete_mod_api.h>

/**
 * @brief Claim a moby of even class id with a fixed tint; decline every other.
 * @param key The candidate.
 * @param out The uniform block, std140 vec4 rgb + strength.
 * @param cap Bytes available at out.
 * @return 16, or -1 to decline.
 */
static int refine(const openpete_mod_material_key_t* key, void* out, uint32_t cap) {
    if (cap < 16 || key->channel != OP_CHAN_MOBY) return -1;
    if ((key->u.moby.moby_class & 1) != 0) return -1;
    float* v = (float*)out;
    v[0] = 1.0f; v[1] = 0.5f; v[2] = 0.0f; v[3] = 0.5f;
    return 16;
}

/**
 * @brief Register the channel-wide moby refine.
 * @param api The engine's function table.
 * @param self This mod.
 * @return 0.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    const int fs = api->shader_register(self, "shaders/tint.frag");
    if (fs < 0) return 0;
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_MOBY;
    api->material_register_refine(self, &sel, fs, refine);
    return 0;
}
