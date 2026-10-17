/**
 * @file mat_fx_pair.c
 * @brief Material resolution fixture: a C declarative on the player selector the manifest row already holds.
 */
#include <openpete_mod_api.h>

/**
 * @brief Register a green player declarative after the manifest's red one.
 * @param api The engine's function table.
 * @param self This mod.
 * @return 0; a refused registration leaves the manifest row alone.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    const int fs = api->shader_register(self, "shaders/tint.frag");
    if (fs < 0) return 0;
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_PLAYER;
    const int m = api->material_register(self, &sel, fs);
    const float green[4] = {0.0f, 1.0f, 0.0f, 0.5f};
    if (m >= 0) api->material_set_params(self, m, green, sizeof green);
    return 0;
}
