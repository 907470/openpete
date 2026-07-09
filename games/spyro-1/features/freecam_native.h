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

/* Per function-entry hook. Returns 1 (⇒ the recompiled body should
 * short-circuit to a no-op) when the free camera is active and `addr` is the
 * game's player-update or camera-follow function. For the camera-follow case
 * it FIRST drives the camera from the latched input + resyncs the render
 * region (running on the game fiber, in place of the follow logic). Call from
 * psx_override_dispatch_game. */
int freecam_native_should_freeze(CPUState* cpu, uint32_t addr);

/* Optional: engage the free camera once the game is Playing when
 * OPENPETE_FREECAM=1 is set (scripted / headless use). No-op otherwise. Call
 * once per frame from game_on_frame_pre. */
void freecam_native_maybe_autoengage(CPUState* cpu);
