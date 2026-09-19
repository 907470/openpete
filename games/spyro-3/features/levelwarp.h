#pragma once
/* levelwarp.h — jump to any level from the dev overlay (M key).
 *
 * Owns the route probe and the guest writes of the two warp routes; the
 * dropdown, the Jump button and the click queue are the shared overlay half
 * (games/common/features/levelwarp_ui.h). See levelwarp.c for the mechanism.
 *
 * Called from game_extras_impl.c (game_on_frame_pre). */

typedef struct CPUState CPUState;

/* Drive the warp: drain a queued overlay request, enter the game's own level
 * transition, and report arrival. Also refreshes the "can warp right now"
 * state the overlay button gates on, so it must run every tick even when no
 * warp is pending. Call once per tick from game_on_frame_pre. */
void spyro3_levelwarp_on_tick(CPUState* cpu);
