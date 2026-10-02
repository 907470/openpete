#pragma once
/* levelwarp.h - jump to any level from the dev overlay (M key).
 *
 * Owns the route probe and the guest writes of the two warp routes; the
 * dropdown, the Jump button and the click queue are the shared overlay half
 * (games/common/features/levelwarp_ui.h). See levelwarp.c for the mechanism.
 *
 * Called from game_extras_impl.c (game_on_frame_pre). */

typedef struct CPUState CPUState;

/** @brief Drives the level warp once per tick: drains a queued overlay request, then follows an in-flight warp to arrival.
 *
 * levelwarp_ui_tick refreshes the route the overlay button gates on, so
 * this runs every tick from game_on_frame_pre even with no warp pending.
 * An in-flight warp waits for the left level to stop being live, then for
 * the next level to come up, and logs whether it is the target; it aborts
 * after WARP_STUCK_CALLS calls. Past the drain it writes no guest word.
 * @param cpu The guest CPU state. */
void spyro2_levelwarp_on_tick(CPUState* cpu);
