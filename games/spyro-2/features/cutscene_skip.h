// games/spyro-2/features/cutscene_skip.h — `--skip-to-cutscene` for Spyro 2
// (cutscene_skip.c).
#ifndef SPYRO2_CUTSCENE_SKIP_H
#define SPYRO2_CUTSCENE_SKIP_H

#include <stddef.h>

typedef struct CPUState CPUState;

/* game_resolve_cutscene_arg: a cutscene name, or its id (a story
 * cutscene's k, the argument of func_80019230; a level intro or outro's
 * table id); -1 and the printed list for an empty or unknown argument. */
int spyro2_cutscene_resolve(const char* arg);

/* The level cutscene `id` is entered from (the --skip-to-level it rides):
 * Dragon Shores (65) for a story cutscene, the level's homeworld for an
 * intro, the level itself for an outro; -1 for an unknown id. */
int spyro2_cutscene_source_level(int id);

/* Arm cutscene `id`. The hook acts once, on the first non-speculative tick
 * of the source level's gameplay. */
void spyro2_cutscene_skip_arm(int id);

/* 1 from the arm until spyro2_cutscene_skip_finish: the skip's release block
 * then releases on spyro2_cutscene_skip_released instead of the level skip's
 * playable edge. */
int spyro2_cutscene_skip_watching(void);

/* The armed row's release predicate: the hook has called and cutscene
 * playback runs (D_800681C8 6) with D_800698F4 == k for a story row, with
 * StartCutscene's slot and entry words equal to the row's for a level
 * intro or outro. On 1,
 * `eng` holds the engagement values read back from RAM. */
int spyro2_cutscene_skip_released(CPUState* cpu, char* eng, size_t cap);

/* End the watch (after the release or the fail-safe). */
void spyro2_cutscene_skip_finish(void);

#endif
