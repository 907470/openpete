#pragma once
/* credits_skip.h — `--skip-to-cutscene` for Spyro 1.
 *
 * Five targets. `intro` enters from the title through New Game's branch;
 * `ending` and `ending_loot` enter from Gnasty's Lair (63) and Gnasty's Loot
 * (64) through the boss overlays' own GS_Cutscene stores; `credits` and
 * `credits_100` replay the "CREDITS" pad code once a level skip has landed.
 * See credits_skip.c for the mechanism. */

#include <stddef.h>
#include <stdint.h>

typedef struct CPUState CPUState;

/* Cutscene ids accepted by --skip-to-cutscene (resolved from the names). */
#define SPYRO1_CUTSCENE_CREDITS      1 /* short roll: after defeating Gnasty */
#define SPYRO1_CUTSCENE_CREDITS_FULL 2 /* full roll: the 100% ending */
#define SPYRO1_CUTSCENE_INTRO        3 /* g_CutsceneIdx 1, from New Game */
#define SPYRO1_CUTSCENE_ENDING       4 /* g_CutsceneIdx 2, Gnasty defeated */
#define SPYRO1_CUTSCENE_ENDING_LOOT  5 /* g_CutsceneIdx 3, Gnasty's Loot */

/* game_resolve_cutscene_arg: a cutscene id or name; -1 and the printed list
 * for an empty or unknown argument. */
int spyro1_cutscene_resolve(const char* arg);

/* The level a target is entered from: 0 for `intro` (the title), 63 / 64 for
 * the endings, -1 for the credits (any live level). */
int spyro1_cutscene_source_level(int id);

/* Arm a target. Its hook acts once, on the first non-speculative tick whose
 * state matches the row's gate. */
void spyro1_cutscene_skip_arm(int id);

/* 1 from the arm until spyro1_cutscene_skip_finish: the skip's fast-forward
 * block then releases on spyro1_cutscene_skip_released instead of the level
 * skip's playable edge. */
int spyro1_cutscene_skip_watching(void);

/* The armed row's release predicate: its hook has acted and the game shows
 * the row's state (g_Gamestate 14 with the row's g_CutsceneIdx, or
 * GS_Credits). On 1, `eng` holds the engagement values read back from RAM. */
int spyro1_cutscene_skip_released(CPUState* cpu, char* eng, size_t cap);

/* End the watch (after the release or the fail-safe). */
void spyro1_cutscene_skip_finish(void);
