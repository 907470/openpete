#pragma once
/* levelwarp.h - jump to any level from the dev overlay (M key).
 *
 * Owns the canonical Spyro 1 level table (shared with --skip-to-level) and
 * the warp's guest writes; the rows and phases are the shared warp
 * (games/common/features/levelwarp_ui.h). See levelwarp.c for the mechanism.
 *
 * Called from game_extras_impl.c (game_on_frame_pre + the savestate extra
 * blob). */

#include <stddef.h>
#include <stdint.h>

#include "features/level_table.h"

typedef struct CPUState CPUState;

/* The 35 levels, in id order, with the alias spellings (gnastys_world,
 * twilight_harbour) in their own list so the dropdown shows each level once.
 * Names resolve case-insensitive with '-' accepted for '_' and apostrophes
 * not dropped; an all-digit id passes through unchecked
 * (NAMED_TARGET_NUMERIC_ANY): the warp takes the id the caller names rather
 * than indexing the table with it. */
extern const game_level_table_t spyro1_levels;

/** @brief Resolves a --skip-to-level style argument to a level id through spyro1_levels.
 *
 * @param arg The argument, or NULL.
 * @return The level id, or -1 for an unknown or empty name after printing
 *     the valid list to stderr. */
int spyro1_level_resolve(const char* arg);

/** @brief Drives the level-warp state machine for one tick.
 *
 * Runs levelwarp_on_tick, which refreshes the route the overlay button gates
 * on and drains a queued request, so this runs every tick even when no warp
 * is pending, once per tick from game_on_frame_pre. While leaving, the
 * g_Gamestate edge to GS_LevelTransition retargets the load and any state
 * other than GS_ExitLevel aborts the warp. While loading, the return to
 * GS_Playing ends the warp and logs the level reached.
 * @param cpu The guest CPU state. */
void spyro1_levelwarp_on_tick(CPUState* cpu);

/** @brief Copies the warp state machine out for a savestate.
 *
 * The phase is host state that gates guest writes, so it travels with every
 * snapshot, file and in-memory, in the game's extra section beside the thunk
 * layer's latch pack.
 * @param phase Receives the phase (idle, armed, loading).
 * @param target Receives the target level id.
 * @param ticks Receives the phase's call count. */
void spyro1_levelwarp_state_save(uint32_t* phase, uint32_t* target,
                                 uint32_t* ticks);
/** @brief Restores the warp state machine from a savestate.
 *
 * @param phase The phase (idle, armed, loading).
 * @param target The target level id.
 * @param ticks The phase's call count. */
void spyro1_levelwarp_state_load(uint32_t phase, uint32_t target,
                                 uint32_t ticks);
