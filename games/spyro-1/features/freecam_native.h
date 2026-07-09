#pragma once
/* freecam_native.h — Spyro-1 half of the debug free camera (F key).
 *
 * The generic on/off + input-intent state lives in core (openpete_freecam.h);
 * this half owns the Spyro-specific pieces: which g_Camera bytes to write and
 * which game functions to freeze while engaged. Called from
 * game_extras_impl.c (game_on_frame_pre → drive; psx_override_dispatch_game →
 * should_freeze). See the port notes in freecam_native.c. */

#include <stdint.h>

typedef struct CPUState CPUState;

/* Per-tick: when the free camera is active, integrate the latched keyboard
 * intent (core state) into g_Camera's position + Euler angles. No-op when
 * inactive. Call from game_on_frame_pre (before the tick builds the view
 * matrix from these fields). */
void freecam_native_drive(CPUState* cpu);

/* Per function-entry: returns 1 iff `addr` should be short-circuited to a
 * no-op this frame — i.e. the free camera is active and `addr` is the game's
 * camera-follow or player-update function. Call from
 * psx_override_dispatch_game. */
int freecam_native_should_freeze(uint32_t addr);
