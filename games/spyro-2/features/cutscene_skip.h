// games/spyro-2/features/cutscene_skip.h - `--skip-to-cutscene` for Spyro 2
// (cutscene_skip.c).
#ifndef SPYRO2_CUTSCENE_SKIP_H
#define SPYRO2_CUTSCENE_SKIP_H

#include <stddef.h>

typedef struct CPUState CPUState;

/** @brief Resolves a --skip-to-cutscene argument to a cutscene id.
 *
 * Backs game_resolve_cutscene_arg. The argument is a cutscene name or its
 * id: a story cutscene's k (func_80019230's argument), or a level intro or
 * outro's table id from 20. An empty or unknown argument prints the list.
 * @param arg The argument.
 * @return The id, or -1. */
int spyro2_cutscene_resolve(const char* arg);

struct openpete_surface_sink;
/** @brief Report every cutscene name --skip-to-cutscene accepts to the surface dump.
 *
 * @param sink The dump's collector. */
void spyro2_cutscene_surface(struct openpete_surface_sink* sink);

/** @brief Returns the level a cutscene is entered from, the --skip-to-level it rides.
 *
 * Dragon Shores (65) for a story cutscene, the level's homeworld for an
 * intro, the level itself for an outro.
 * @param id The cutscene id.
 * @return The level, or -1 for an unknown id. */
int spyro2_cutscene_source_level(int id);

/** @brief Arms a cutscene for the skip hook, which acts once on the first non-speculative tick of the source level's gameplay.
 *
 * Resets the hook's call and return-watch state; an unknown id leaves the
 * hook idle and the watch off.
 * @param id The cutscene id. */
void spyro2_cutscene_skip_arm(int id);

/** @brief Reports whether a cutscene watch is active, from the arm until spyro2_cutscene_skip_finish or a refusal.
 *
 * While it is, the level skip's release block releases on
 * spyro2_cutscene_skip_released instead of the level skip's playable edge.
 * @return 1 while watching, else 0. */
int spyro2_cutscene_skip_watching(void);

/** @brief Tests the armed cutscene's release predicate and formats its engagement values.
 *
 * True once the hook has made its call and cutscene playback runs
 * (D_800681C8 6), with D_800698F4 equal to k for a story row, or with
 * StartCutscene's slot (0x80068C40; 1 for an outro, 0 for an intro) and
 * entry (0x80068C3C) equal to the row's for a level row. A story row's
 * release starts the watch for the replay's return to Dragon Shores.
 * @param cpu The guest CPU state.
 * @param eng Receives the engagement values read back from RAM on a release.
 * @param cap The size of eng.
 * @return 1 on release, else 0. */
int spyro2_cutscene_skip_released(CPUState* cpu, char* eng, size_t cap);

/** @brief Ends the cutscene watch, after the release or the level skip's fail-safe. */
void spyro2_cutscene_skip_finish(void);

#endif
