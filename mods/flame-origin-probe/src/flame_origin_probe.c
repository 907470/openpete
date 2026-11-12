/* flame-origin-probe - log the flame origin func_80048D10 computes (see
 * mod.toml).
 *
 * Addresses from the game: g_Spyro +0x18 is the animation index and +0x1E
 * the frame index; g_Models[0] +0x38 + 4 * index is the animation header,
 * whose frame words start at +0x24 (4 bytes each). The update reads the
 * frame word there, so the probe reads the same word after it runs.
 */
#include <openpete_mod_api.h>
#include "openpete_sdk_symbols.h"

#define FLAME_ACTIVE 0x9Au

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned g_calls, g_logged;

/** @brief Return the frame word Spyro's current animation frame holds, or 0 when no header is bound. */
static uint32_t spyro_frame_word(void) {
    uint32_t anim = op_read_u8(g_api, OP_GADDR_g_Spyro + 0x18u);
    uint32_t frame = op_read_u8(g_api, OP_GADDR_g_Spyro + 0x1Eu);
    uint32_t model = op_read_u32(g_api, OP_GADDR_g_Models);
    if (!model) return 0;
    uint32_t header = op_read_u32(g_api, model + 0x38u + anim * 4u);
    if (!header) return 0;
    return op_read_u32(g_api, header + 0x24u + frame * 4u);
}

/** @brief Run Spyro's update, then log the flame origin when the flame is active. */
static void on_spyro_update(CPUState* cpu) {
    g_api->base(cpu);
    g_calls++;
    if (!op_read_u8(g_api, OP_GADDR_g_SpyroFlame + FLAME_ACTIVE)) return;
    g_logged++;
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "flame-origin-probe: call=%u origin=(%d, %d, %d) frameword=0x%08X",
               g_calls,
               (int)op_read_u32(g_api, OP_GADDR_g_SpyroFlame + 0u),
               (int)op_read_u32(g_api, OP_GADDR_g_SpyroFlame + 4u),
               (int)op_read_u32(g_api, OP_GADDR_g_SpyroFlame + 8u),
               spyro_frame_word());
}

/** @brief Bind the probe to Spyro's per-tick update.
 *
 * @param api The engine's mod API table.
 * @param self This mod's handle.
 * @return 0 on success, non-zero when the override is refused. */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    return api->override_name(self, "func_80048D10", on_spyro_update);
}
