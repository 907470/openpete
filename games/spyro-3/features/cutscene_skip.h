// games/spyro-3/features/cutscene_skip.h - `--skip-to-cutscene` for Spyro 3
// (cutscene_skip.c).
#ifndef SPYRO3_CUTSCENE_SKIP_H
#define SPYRO3_CUTSCENE_SKIP_H

#include <stddef.h>
#include <stdint.h>

typedef struct CPUState CPUState;

/** @brief Resolves a --skip-to-cutscene argument to a cutscene id (game_resolve_cutscene_arg).
 *
 * Accepts a cutscene name (the loading caption in lower_snake_case, an
 * apostrophe dropped) or its id, WarpToLevel's pType-6 argument, 61..80 with
 * 72 absent. An empty or unknown argument prints the valid names.
 * @param arg The command-line value, or NULL.
 * @return The cutscene id, or -1. */
int spyro3_cutscene_resolve(const char* arg);

struct openpete_surface_sink;
/** @brief Report every cutscene name --skip-to-cutscene accepts to the surface dump.
 *
 * @param sink The dump's collector. */
void spyro3_cutscene_surface(struct openpete_surface_sink* sink);

/** @brief Arms cutscene id for the per-tick driver and starts its watch (game_set_skip_to_cutscene).
 *
 * The route is the row's chain head. A head that starts from the title
 * refuses any --skip-to-level and arms the present fast-forward itself; a head
 * that starts from a level refuses another level and arms the level skip to
 * its own level when none is given. A refusal logs one line; a refused or
 * unknown id leaves the module and the level skip as they were.
 * @param id The cutscene id.
 * @param skip_level The --skip-to-level target, 0 when none. */
void spyro3_cutscene_arm(int id, int skip_level);

/** @brief Advances the armed cutscene one tick; called from game_on_frame_pre.
 *
 * Inert while disarmed and on a speculative (runahead) tick. Armed, on the
 * route's tick it queues the head's WarpToLevel(6, id) call through the
 * transition module when the preconditions hold, else logs the words it read
 * and disarms. Once the call ran, it logs each cutscene the chain plays.
 * @param cpu The guest CPU state.
 * @param frame_counter The openpete_frame counter, for the log lines. */
void spyro3_cutscene_on_tick(CPUState* cpu, uint32_t frame_counter);

/** @brief Reports whether a cutscene watch is active, from the arm until spyro3_cutscene_finish.
 *
 * While it is, the skip release block (skip_to_level.c) releases on
 * spyro3_cutscene_released instead of the level skip's playable edge, headless
 * too. A refusal at the route's tick leaves the watch on until the fail-safe.
 * @return 1 while watching, else 0. */
int spyro3_cutscene_watching(void);

/** @brief Tests the armed row's release predicate and formats its engagement values.
 *
 * True once the route's WarpToLevel call ran and the cutscene overlay plays
 * (game.state 6) with its index 0x8006EDD8 equal to the row's.
 * @param cpu The guest CPU state.
 * @param eng Receives the name and the index and level words read back from
 *     RAM on a release.
 * @param cap The size of eng.
 * @return 1 on release, else 0. */
int spyro3_cutscene_released(CPUState* cpu, char* eng, size_t cap);

/** @brief Ends the cutscene watch, after the release or the level skip's fail-safe. */
void spyro3_cutscene_finish(void);

#endif
