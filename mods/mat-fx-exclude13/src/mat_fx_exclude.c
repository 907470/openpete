/**
 * @file mat_fx_exclude.c
 * @brief Material resolution fixture: class-keyed identity exclusions registered before a channel-wide moby tint.
 */
#include <openpete_mod_api.h>

/** Classes used where the game serves no class census start here. */
#define FALLBACK_CLASS 2
#define EXCLUDE_N 3

static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;
static int g_excl[EXCLUDE_N] = {-1, -1, -1};

/**
 * @brief Return a class from the middle of the current level's loaded classes, or a fallback.
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
 * @brief Retune the exclusions to the picked classes on every present.
 * @param ctx The present.
 */
static void on_present(const openpete_present_ctx_t* ctx) {
    (void)ctx;
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_MOBY;
    for (int i = 0; i < EXCLUDE_N; i++) {
        if (g_excl[i] < 0) continue;
        sel.moby_class = pick_class(1 + i);
        g_api->material_set_selector(g_self, g_excl[i], &sel);
    }
}

/**
 * @brief Register the identity exclusions, then the channel-wide tint.
 * @param api The engine's function table.
 * @param self This mod.
 * @return 0.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    const int stock = api->shader_register(self, "shaders/stock.frag");
    const int tint = api->shader_register(self, "shaders/tint.frag");
    if (stock < 0 || tint < 0) return 0;
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_MOBY;
    for (int i = 0; i < EXCLUDE_N; i++) {
        sel.moby_class = FALLBACK_CLASS + 1 + i;
        g_excl[i] = api->material_register(self, &sel, stock);
    }
    sel.moby_class = -1;
    const int wide = api->material_register(self, &sel, tint);
    const float magenta[4] = {1.0f, 0.0f, 1.0f, 0.5f};
    if (wide >= 0) api->material_set_params(self, wide, magenta, sizeof magenta);
    api->register_present_hook(self, on_present);
    return 0;
}
