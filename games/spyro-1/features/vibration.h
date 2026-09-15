#pragma once
/* vibration.h — --vibration: preset Spyro 1's own controller-vibration
 * setting.
 *
 * The host half of --vibration (motor amplitude, and the headless / focus
 * gates) lives in core/runtime/host_rumble.c. This is the guest half: the
 * game's options menu carries a VIBRATION row backed by one enable flag, and
 * the flag is what decides whether the game bothers to write actuator bytes at
 * all. Presetting it is what lets a launch start with rumble off (or back on)
 * without a trip through the menu.
 *
 * Same contract as the --sfx / --music slider presets: stamped at a tick
 * boundary, re-stamped over the clobbers the game does on its own, and
 * released for good the moment the player toggles that row by hand.
 *
 * Called from game_extras_impl.c (game_on_frame_pre). */

typedef struct CPUState CPUState;

/* Apply the pending preset. Call once per tick; inert (no guest writes) while
 * the preset is unset. */
void spyro1_vibration_on_tick(CPUState* cpu);
