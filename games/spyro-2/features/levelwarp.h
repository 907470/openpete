#pragma once
/* levelwarp.h - jump to any level from the dev overlay (M key).
 *
 * Owns the in-level route's guest writes and the route words; the dropdown,
 * the Warp button, the click queue and the in-flight phases are the shared
 * warp (games/common/features/levelwarp_ui.h). See levelwarp.c for the
 * mechanism.
 *
 * Called from game_extras_impl.c (game_on_frame_pre). */

typedef struct CPUState CPUState;

/** @brief Drives the level warp once per tick through levelwarp_on_tick.
 *
 * Runs every tick from game_on_frame_pre even with no warp pending, so the
 * button's route stays current. An in-flight warp waits for the left level
 * to stop being live, then for the next level to come up, and logs whether
 * it is the target. Past the drain it writes no guest word.
 * @param cpu The guest CPU state. */
void spyro2_levelwarp_on_tick(CPUState* cpu);
