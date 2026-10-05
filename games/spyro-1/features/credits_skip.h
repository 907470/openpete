#pragma once
/* credits_skip.h - `--skip-to-cutscene` for Spyro 1.
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

/** @brief Resolves a --skip-to-cutscene argument to a cutscene id.
 *
 * Backs game_resolve_cutscene_arg. Accepts a row name (intro, ending,
 * ending_loot, credits, credits_100, full_credits) or an all-digit id the
 * table holds, through named_target_resolve.
 * @param arg The command-line value, or NULL.
 * @return The SPYRO1_CUTSCENE_* id, or -1 after printing the valid names
 *     for an empty or unknown argument. */
int spyro1_cutscene_resolve(const char* arg);

struct openpete_surface_sink;
/** @brief Report every cutscene name --skip-to-cutscene accepts to the surface dump.
 *
 * @param sink The dump's collector. */
void spyro1_cutscene_surface(struct openpete_surface_sink* sink);

/** @brief Returns the level a cutscene target is entered from.
 *
 * @param id A SPYRO1_CUTSCENE_* id.
 * @return 0 for `intro` (the title), 63 / 64 for the endings, -1 for the
 *     credits (any live level) and for an unknown id. */
int spyro1_cutscene_source_level(int id);

/** @brief Arms a cutscene target and starts the release watch.
 *
 * The target's hook acts once, on the first non-speculative tick whose state
 * matches the row's gate. An unknown id disarms every hook and leaves the
 * watch off.
 * @param id A SPYRO1_CUTSCENE_* id. */
void spyro1_cutscene_skip_arm(int id);

/** @brief Reports whether a cutscene skip is being watched for its release.
 *
 * While set, the skip's fast-forward block releases on
 * spyro1_cutscene_skip_released instead of the level skip's playable edge.
 * @return 1 from an arm of a known id until spyro1_cutscene_skip_finish,
 *     else 0. */
int spyro1_cutscene_skip_watching(void);

/** @brief Tests the armed row's release predicate.
 *
 * The predicate holds when the row's hook has acted and the game shows the
 * row's state: g_Gamestate 15 (GS_Credits) for the credits rows, else
 * g_Gamestate 14 (GS_Cutscene) with the row's g_CutsceneIdx.
 * @param cpu The guest CPU state.
 * @param eng Receives, on 1, the engagement values read back from RAM (the
 *     row name, g_CreditsSequence or g_CutsceneIdx, and D_80075818).
 * @param cap Size of `eng` in bytes.
 * @return 1 when the predicate holds, else 0. */
int spyro1_cutscene_skip_released(CPUState* cpu, char* eng, size_t cap);

/** @brief Ends the release watch, after the release or the fail-safe.
 *
 * The armed row stays armed; only spyro1_cutscene_skip_watching drops to 0. */
void spyro1_cutscene_skip_finish(void);
