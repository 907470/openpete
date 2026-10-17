/**
 * @file mat_fx_pulse.c
 * @brief Material resolution fixture: a channel-wide moby refine whose block alternates colour every tick.
 */
#include <openpete_mod_api.h>
#include <stddef.h>

static const openpete_mod_api_t* s_api;
static openpete_mod_t* s_self;

/**
 * @brief Claim every moby with a tint whose colour follows the sampled tick's parity.
 *
 * Reads the tick of the record the engine sampled at the last canonical
 * tick boundary (player_pose with no CPU), so every instance of one scene
 * gets the same block and consecutive ticks get different ones. With no
 * level live the tint is grey.
 * @param key The candidate.
 * @param out The uniform block, std140 vec4 rgb + strength.
 * @param cap Bytes available at out.
 * @return 16, or -1 for a non-moby key or a short buffer.
 */
static int refine(const openpete_mod_material_key_t* key, void* out, uint32_t cap) {
    if (cap < 16 || key->channel != OP_CHAN_MOBY) return -1;
    openpete_mod_pose_t pose = { .struct_size = (uint32_t)sizeof pose };
    float* v = (float*)out;
    if (s_api->player_pose(s_self, NULL, &pose) != 0) {
        v[0] = 0.5f; v[1] = 0.5f; v[2] = 0.5f;
    } else if (pose.tick & 1u) {
        v[0] = 1.0f; v[1] = 0.0f; v[2] = 0.0f;
    } else {
        v[0] = 0.0f; v[1] = 0.0f; v[2] = 1.0f;
    }
    v[3] = 0.6f;
    return 16;
}

/**
 * @brief Register the channel-wide moby refine.
 * @param api The engine's function table.
 * @param self This mod.
 * @return 0.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    s_api = api;
    s_self = self;
    const int fs = api->shader_register(self, "shaders/tint.frag");
    if (fs < 0) return 0;
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_MOBY;
    api->material_register_refine(self, &sel, fs, refine);
    return 0;
}
