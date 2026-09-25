#pragma once
/* skip_to_area.h — --skip-to-area <level>:<area> for Spyro 3: the level by
 * --skip-to-level, then the sub-area by the game's EnterSubArea
 * func_80052918, called through the transition queue with the level's
 * area-0 portal pair. See skip_to_area.c. */

#include <stddef.h>
#include <stdint.h>

typedef struct CPUState CPUState;

/* game_resolve_area_arg: the level number, *out_area the area index; -1
 * and the list of valid targets for an empty, unknown or unreachable
 * argument. */
int spyro3_area_resolve(const char* arg, int* out_area);

/* game_set_skip_to_area: arms --skip-to-level `level` and the sub-area
 * entry; level 0 disarms. */
void spyro3_area_set(int level, int area);

/* Per-tick driver (game_on_frame_pre). Inert while disarmed. */
void spyro3_area_on_tick(CPUState* cpu, uint32_t frame_counter);

/* An area target is armed and its release or fail-safe line is not yet
 * logged: the skip release block (skip_to_level.c) watches it every tick,
 * headless too. */
int spyro3_area_watching(void);

/* An area target is armed and not yet released: from the level skip through
 * the sub-area load. A scripted freecam pose waits on it, since the sub-area
 * transition re-seeds the camera from the game's. */
int spyro3_area_pending(void);

/* The armed target's release predicate held: game.state 13 was seen after
 * the call, then gameplay went live with the area word == the target. On a
 * release, writes " level=L area=A" read back from 0x8006C5BC and
 * 0x8006C5C8 to eng. */
int spyro3_area_released(CPUState* cpu, char* eng, size_t cap);

/* The release block logged its line; the watch ends. */
void spyro3_area_finish(void);
