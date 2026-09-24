// games/spyro-3/skip_to_level.h — --skip-to-level for Spyro 3 (skip_to_level.c).
#ifndef SPYRO3_SKIP_TO_LEVEL_H
#define SPYRO3_SKIP_TO_LEVEL_H

#include "openpete_psx.h"

#include <stddef.h>
#include <stdint.h>

/* One selectable level. `name` is the --skip-to-level spelling, `display`
 * the dev-overlay dropdown label. Table order is level-number order. */
typedef struct {
    const char* name;
    const char* display;
    int         number;
} spyro3_level_t;

/* The 37 levels, in level-number order. */
const spyro3_level_t* spyro3_level_table(size_t* out_count);

/* game_resolve_level_arg: a level number or lower_snake_case name; -1 and the
 * printed list for an empty or unknown argument. */
int spyro3_level_resolve(const char* arg);

/* game_set_skip_to_level: 0 disarms. */
void spyro3_skip_set(int number);

/* The --skip-to-level target, 0 when none. */
int spyro3_skip_target(void);

/* Windowed runs: hold presents until the skip's release line; `until` names
 * the edge in the log line. Headless: nothing. */
void spyro3_skip_ffwd_arm(const char* until);

/* Per-tick driver (game_on_frame_pre). Inert while disarmed. */
void spyro3_skip_on_tick(CPUState* cpu, uint32_t frame_counter);

/* The loader took the --skip-to-level target; stays set through the level
 * until the game returns to the title. */
int spyro3_skip_level_picked(void);

/* A level overlay is resident and the game is in the gameplay state. */
int spyro3_skip_gameplay_live(CPUState* cpu);

#endif
