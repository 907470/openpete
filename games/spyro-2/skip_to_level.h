// games/spyro-2/skip_to_level.h — --skip-to-level for Spyro 2 (skip_to_level.c).
#ifndef SPYRO2_SKIP_TO_LEVEL_H
#define SPYRO2_SKIP_TO_LEVEL_H

#include "openpete_psx.h"

#include <stddef.h>
#include <stdint.h>

/* One selectable level. `name` is the --skip-to-level spelling, `display`
 * the dev-overlay dropdown label. Table order is level-number order. */
typedef struct {
    const char* name;
    const char* display;
    int         number;
} spyro2_level_t;

/** @brief Returns the table of the 29 selectable levels, in level-number order.
 *
 * It is the one list --skip-to-level, the dev-overlay level warp and the terrain
 * feeder's level names resolve against.
 * @param out_count Receives the entry count when non-NULL.
 * @return The table, static storage. */
const spyro2_level_t* spyro2_level_table(size_t* out_count);

/** @brief Resolves a --skip-to-level argument to a level number (game_resolve_level_arg).
 *
 * Accepts a level number or a lower_snake_case name. A number is accepted only
 * when the table holds it: an unknown number would index the game's level
 * table past its end. An empty or unknown argument prints the valid names.
 * @param arg The command-line value, or NULL.
 * @return The level number, or -1. */
int spyro2_level_resolve(const char* arg);

/** @brief Arms the skip to a level number, or disarms it with 0 or less (game_set_skip_to_level).
 *
 * Arming in a windowed run also sets openpete_skip_present_until_level, so
 * presents fast-forward until the release in the per-tick driver, and resets
 * that release's fail-safe deadline.
 * @param number The level number (10..66), or 0 or less to disarm. */
void spyro2_skip_set(int number);

/** @brief Returns the armed --skip-to-level target.
 *
 * @return The level number, or 0 when none is armed or the skip has ended. */
int spyro2_skip_target(void);

/** @brief Advances the skip one tick; called from game_on_frame_pre.
 *
 * Armed, it waits for the title's press-start idle sub-state (2), writes the
 * target into the demo level list at g_DemoIndex and forces the demo-timeout
 * sub-state (6) with the title timer zeroed. Seeded, it waits for the loader to
 * take the target (loader state 2 with g_NextLevelNum equal to it). Picked, it
 * holds g_DemoMode at 0 so the pad path reads the controller and the demo-end
 * check never fires, until the loader state returns to 0 (the title). Every
 * tick it then runs the release check that ends the present fast-forward or a
 * cutscene watch. Each transition logs one line.
 * @param cpu The guest CPU state.
 * @param frame_counter The openpete_frame counter, for the log lines and the
 *     fail-safe deadline. */
void spyro2_skip_on_tick(CPUState* cpu, uint32_t frame_counter);

/** @brief Reports whether a level overlay is resident and the level has run its first tick.
 *
 * The loader state reads -1 (resident) about 45 VBLs before the level's first
 * tick, so the level tick counter (0x80067008) must also be nonzero. Title,
 * loading and the exit transition report not live.
 * @param cpu The guest CPU state.
 * @return 1 when gameplay is live, else 0. */
int spyro2_skip_gameplay_live(CPUState* cpu);

#endif
