// games/spyro-2/features/cutscene_skip.h — `--skip-to-cutscene` for Spyro 2
// (cutscene_skip.c).
#ifndef SPYRO2_CUTSCENE_SKIP_H
#define SPYRO2_CUTSCENE_SKIP_H

#include <stddef.h>

typedef struct CPUState CPUState;

/* Every story cutscene is entered from Dragon Shores, the level whose
 * theater replays them. */
#define SPYRO2_CUTSCENE_SOURCE_LEVEL 65

/* game_resolve_cutscene_arg: a cutscene name, or its number k (the argument
 * of func_80019230); -1 and the printed list for an empty or unknown
 * argument. */
int spyro2_cutscene_resolve(const char* arg);

/* Arm cutscene k. The hook acts once, on the first non-speculative tick of
 * Dragon Shores gameplay. */
void spyro2_cutscene_skip_arm(int k);

/* 1 from the arm until spyro2_cutscene_skip_finish: the skip's release block
 * then releases on spyro2_cutscene_skip_released instead of the level skip's
 * playable edge. */
int spyro2_cutscene_skip_watching(void);

/* The armed row's release predicate: the hook has called func_80019230(k)
 * and cutscene playback runs (D_800681C8 6) with D_800698F4 == k. On 1,
 * `eng` holds the engagement values read back from RAM. */
int spyro2_cutscene_skip_released(CPUState* cpu, char* eng, size_t cap);

/* End the watch (after the release or the fail-safe). */
void spyro2_cutscene_skip_finish(void);

#endif
