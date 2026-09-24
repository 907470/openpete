// games/spyro-3/features/cutscene_skip.h — `--skip-to-cutscene` for Spyro 3
// (cutscene_skip.c).
#ifndef SPYRO3_CUTSCENE_SKIP_H
#define SPYRO3_CUTSCENE_SKIP_H

#include <stddef.h>
#include <stdint.h>

typedef struct CPUState CPUState;

/* game_resolve_cutscene_arg: a cutscene name, or its id (WarpToLevel's
 * pType-6 argument, 61..80); -1 and the printed list for an empty or unknown
 * argument. */
int spyro3_cutscene_resolve(const char* arg);

/* game_set_skip_to_cutscene: arm cutscene `id`. `skip_level` is the
 * --skip-to-level target (0 when none): a row that starts from the title
 * refuses any, a row that starts from a level refuses another level and
 * arms the level skip itself when none is given. A refused row leaves the
 * level skip as given. */
void spyro3_cutscene_arm(int id, int skip_level);

/* Per-tick driver (game_on_frame_pre). Inert while disarmed. */
void spyro3_cutscene_on_tick(CPUState* cpu, uint32_t frame_counter);

/* 1 from the arm until spyro3_cutscene_finish: the skip release block
 * (skip_to_level.c) releases on spyro3_cutscene_released instead of the
 * level skip's playable edge, headless too. */
int spyro3_cutscene_watching(void);

/* The armed row's release predicate: its route's WarpToLevel call ran and
 * the cutscene overlay plays (game.state 6) with its index 0x8006EDD8 ==
 * the row's. On 1, `eng` holds the name and the index and level words read
 * back from RAM. */
int spyro3_cutscene_released(CPUState* cpu, char* eng, size_t cap);

/* End the watch (after the release or the fail-safe). */
void spyro3_cutscene_finish(void);

#endif
