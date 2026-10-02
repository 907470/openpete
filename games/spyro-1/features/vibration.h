#pragma once
/* vibration.h - --vibration: preset Spyro 1's own controller-vibration
 * setting.
 *
 * The host half of --vibration (motor amplitude, and the headless / focus
 * gates) lives in core/input/host_rumble.c. This is the guest half: the
 * game's options menu carries a VIBRATION row backed by one enable flag, and
 * the flag is what decides whether the game bothers to write actuator bytes at
 * all. A positive --vibration presets it on, so a launch starts with rumble
 * enabled without a trip through the menu; --vibration 0 silences the motors
 * host-side and writes no guest word.
 *
 * Same contract as the --sfx / --music slider presets: stamped at a tick
 * boundary, re-stamped over the clobbers the game does on its own, and
 * released for good the moment the player toggles that row by hand.
 *
 * Called from game_extras_impl.c (game_on_frame_pre). */

typedef struct CPUState CPUState;

/** @brief Stamps a positive --vibration request into g_ActEnabled for one tick.
 *
 * Called once per tick. Makes no guest write while the request is -1 (unset)
 * or 0 (host-side silence), and none on a speculative tick. A positive
 * request writes g_ActEnabled 1 when the guest value differs. While the
 * player's cursor is on the options submenu's vibration row, nothing is
 * written, and a guest value moved away from the last stamped one drops the
 * request to -1, handing the setting to the menu.
 * @param cpu The guest CPU state. */
void spyro1_vibration_on_tick(CPUState* cpu);
