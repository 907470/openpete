// games/spyro-1/mods/mod_moby_ops.c - Spyro 1 game ops for the mod content registry
// (core/mods/mod_registry.c) and the SFX definition service.
//
// Ground truth (cross-checked include/moby.h + asm/42CC4.s +
// moby.c):
//   - g_Models @ 0x80076378: flat 512×4B Model* table indexed DIRECTLY by
//     Moby.m_Class (+0x36). Populated per level by the loader; a NULL row
//     means the level never uses that class.
//   - Gameplay gate: g_Gamestate @ 0x800757D8 == 0 (GS_Playing). Reading
//     the table only in GS_Playing guarantees the loader finished
//     populating g_Models (no half-loaded-table race).

#include "openpete_mods.h"

#include <string.h>

#define ADDR_G_MODELS     0x80076378u
#define MODEL_COUNT       512
#define ADDR_G_GAMESTATE  0x800757D8u
#define ADDR_G_LEVELID    0x8007596Cu

#include "psx_runtime_internal.h" /* CPUState */

/** @brief Returns the current level id, or 0 outside gameplay.
 *
 * @param cpu The guest CPU state.
 * @return g_LevelId when g_Gamestate is GS_Playing, else 0. */
static int ops_level_id(CPUState* cpu) {
    if (cpu->read_word(ADDR_G_GAMESTATE) != 0u) return 0; /* GS_Playing */
    return (int)cpu->read_word(ADDR_G_LEVELID);
}

/** @brief Lists the moby classes the current level loaded.
 *
 * Returns every class id whose g_Models row is non-NULL: the table is
 * indexed directly by m_Class and the loader populates only the rows the
 * level uses. Outside GS_Playing the list is unknown, since mid-load the
 * loader is still filling the table, and the call returns -1.
 * @param cpu The guest CPU state.
 * @param out Receives up to `cap` class ids in ascending order.
 * @param cap The capacity of `out`.
 * @return The number of ids written, or -1 outside GS_Playing. */
static int ops_classes_loaded(CPUState* cpu, uint16_t* out, int cap) {
    if (cpu->read_word(ADDR_G_GAMESTATE) != 0u) return -1;   /* GS_Playing */
    int n = 0;
    for (int id = 0; id < MODEL_COUNT && n < cap; id++)
        if (cpu->read_word(ADDR_G_MODELS + 4u * (uint32_t)id))
            out[n++] = (uint16_t)id;
    return n;
}

static const openpete_moby_ops_t s_spyro1_moby_ops = {
    .update_hook_addr = 0x8003385Cu, /* GamestateUpdate */
    .level_id         = ops_level_id,
    .classes_loaded   = ops_classes_loaded,
};

/** @brief Registers Spyro 1's moby ops with the mod registry. */
void spyro1_moby_ops_register(void) {
    openpete_mods_set_moby_ops(&s_spyro1_moby_ops);
}
