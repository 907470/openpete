#pragma once
/* skip_to_area.h — --skip-to-area <level>:<area> for Spyro 3: the level by
 * --skip-to-level, then the sub-area by the game's EnterSubArea
 * func_80052918, called through the transition queue with the level's
 * area-0 portal pair. See skip_to_area.c. */

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

/* An area target is armed (from the arm until its release). */
int spyro3_area_armed(void);

/* The armed target's release predicate held: game.state 13 was seen after
 * the call, then gameplay went live with the area word == the target. */
int spyro3_area_released(void);

/* The armed target's area index, for the release line. */
int spyro3_area_target(void);
