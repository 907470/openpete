#pragma once
/* cheats_native.h — Spyro-1 half of --cheats (auto-applied built-in cheats).
 *
 * The CLI/registry half lives in core (options.c parses --cheats and hands
 * the spec to the feature module's apply_options); this half owns the cheat
 * name table, the gameplay globals each cheat writes, and the "apply once a
 * level is live" timing. Called from game_extras_impl.c (game_on_frame_pre).
 * See cheats_native.c for the mechanism. */

#include <stdint.h>

typedef struct CPUState CPUState;

/* Apply the selected cheats once gameplay is live, re-arming whenever the
 * player returns to the title screen (so New Game after a quit gets them
 * again), plus any cheat the dev overlay's per-cheat button has queued. Also
 * refreshes the "in a level" state those buttons gate on, so it must run every
 * tick even without --cheats. Call once per tick from game_on_frame_pre. */
void spyro1_cheats_on_tick(CPUState* cpu);
