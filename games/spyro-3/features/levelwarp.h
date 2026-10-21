#pragma once
/* levelwarp.h - jump to any level from the dev overlay (M key).
 *
 * Owns the in-level route's request and the route words; the dropdown, the
 * Warp button, the click queue and the in-flight phases are the shared warp
 * (games/common/features/levelwarp_ui.h). See levelwarp.c for the mechanism.
 *
 * Called from game_extras_impl.c (game_on_frame_pre). */

typedef struct CPUState CPUState;

/** @brief Drives the level warp once per tick through levelwarp_on_tick, then the area warp.
 *
 * Runs every tick from game_on_frame_pre even with no warp pending, so the
 * button's route stays current. An in-level request queues WarpToLevel(0,
 * target) with the area word zeroed in the transition queue, which makes the
 * call at the next Draw entry. After the call the warp waits for the left
 * level to stop being live, then for the next level to come up, and logs
 * whether it is the target. This function writes no guest word itself.
 * @param cpu The guest CPU state. */
void spyro3_levelwarp_on_tick(CPUState* cpu);

/** @brief Queues WarpToLevel func_800584BC(0, target_id) through the transition queue.
 *
 * The request drains at the next Draw func_8001E638 entry, after this tick's
 * Update. The drain re-reads the in-level route (gameplay live, isDemoMode 0,
 * game.state not the title), then writes 0 to the area word 0x8006C5C8 and
 * makes the call. The level warp and the area warp's level leg both use it.
 * @param tag The request's log tag.
 * @param target_id The level number to warp to.
 * @param called Run by the drain after the call.
 * @param dropped Run by the drain when the route no longer holds.
 * @return Non-zero when queued; 0 when another request is already queued. */
int spyro3_levelwarp_enqueue(const char* tag, int target_id,
                             void (*called)(CPUState* cpu),
                             void (*dropped)(CPUState* cpu));
