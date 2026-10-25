// games/common/features/moby_update_all.h - OPENPETE_D1: every live moby of
// the level updates every tick, at any distance from the camera.
//
// The game updates only the mobys its update-list builder lists, and the
// builder drops an undrawn moby outside its update distance. Under
// OPENPETE_D1 the module's hook at the builder (game_layout.update_list)
// writes the same list, listed bytes and pod flags with the distance test
// replaced by a pass, and the entry note marks an undrawn gem drawn before
// its update so it spins at any range. OPENPETE_VIDEO_NOSPARX keeps the
// companion (game_layout.owner.sparx_class) off that list. Both knobs are
// boot-latched and default off; with both off nothing here reads or writes
// guest memory. Both change gameplay state.

#ifndef OPENPETE_MOBY_UPDATE_ALL_H
#define OPENPETE_MOBY_UPDATE_ALL_H

#include <stdint.h>
#include "psx_runtime.h"
#include "feature_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The feature module a game lists in its feature_list.c: the builder hook,
 * bound at apply-options time to game_layout.update_list.builder while
 * OPENPETE_D1 is on. */
extern const feature_module_t moby_update_all_module;

/** @brief Mark an undrawn gem drawn at the entry of the gem update, while OPENPETE_D1 is on.
 *
 * A game with a per-class update table (game_layout.owner.moby_update)
 * marks the gem in a0 when `addr` is the table's gem-class entry; a game
 * with one overlay moby update (game_layout.update_list.overlay_update)
 * marks every live gem when `addr` is that entry. The caller runs it after
 * game_dispatch_preamble returns 0. Off: one flag test.
 * @param cpu The guest CPU state.
 * @param addr The entered function's guest vaddr. */
void moby_update_all_note_entry(CPUState* cpu, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif
