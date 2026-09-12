// games/spyro-2/skip_to_level.h — --skip-to-level for Spyro 2 (skip_to_level.c).
#ifndef SPYRO2_SKIP_TO_LEVEL_H
#define SPYRO2_SKIP_TO_LEVEL_H

#include "openpete_psx.h"

#include <stddef.h>
#include <stdint.h>

typedef struct { const char* name; int number; } spyro2_level_t;

/* The 29 levels, in level-number order. */
const spyro2_level_t* spyro2_level_table(size_t* out_count);

/* game_resolve_level_arg: a level number or lower_snake_case name; -1 and the
 * printed list for an empty or unknown argument. */
int spyro2_level_resolve(const char* arg);

/* game_set_skip_to_level: 0 disarms. */
void spyro2_skip_set(int number);

/* Per-tick driver (game_on_frame_pre). Inert while disarmed. */
void spyro2_skip_on_tick(CPUState* cpu, uint32_t frame_counter);

/* A level overlay is resident and its state machine running. */
int spyro2_skip_gameplay_live(CPUState* cpu);

#endif
