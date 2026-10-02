// games/spyro-3/skip_to_level.h - --skip-to-level for Spyro 3 (skip_to_level.c).
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

/** @brief Return the table of the 37 selectable levels, in level-number order.
 *
 * @param out_count Receives the entry count when non-NULL.
 * @return The first entry of the static table. */
const spyro3_level_t* spyro3_level_table(size_t* out_count);

/** @brief Resolve a --skip-to-level argument, a level number or lower_snake_case name, to a level number.
 *
 * Backs game_resolve_level_arg. Resolution goes through named_target_resolve
 * with apostrophes dropped, so "sheila's_alp" matches "sheilas_alp". A
 * numeric argument is accepted only when the table holds it; an unknown
 * number would index the level table past its end.
 * @param arg The command-line value, or NULL.
 * @return The level number, or -1 after printing the valid names for an
 *         empty, NULL, or unknown argument. */
int spyro3_level_resolve(const char* arg);

/** @brief Arm the level skip for `number`, or disarm it for 0 or below.
 *
 * Backs game_set_skip_to_level. Arming records the target, sets the phase
 * that waits for the title's press-start idle sub-state, and calls
 * spyro3_skip_ffwd_arm with "level N loads".
 * @param number The target level number; 0 or below disarms. */
void spyro3_skip_set(int number);

/** @brief Return the --skip-to-level target level number.
 *
 * The value persists after the skip completes; a disarm sets it to 0.
 * @return The level number last passed to spyro3_skip_set. */
int spyro3_skip_target(void);

/** @brief Hold presents in a windowed run until the skip's release, logging what the fast-forward waits for.
 *
 * Sets openpete_skip_present_until_level and zeroes the fail-safe deadline,
 * which the next release check sets from the current counter. Does nothing
 * when openpete_skip_ffwd_windowed is clear (headless).
 * @param until Text naming the awaited edge, printed in the log line. */
void spyro3_skip_ffwd_arm(const char* until);

/** @brief Drive the level skip once per game_on_frame_pre call.
 *
 * Armed: on the title screen's press-start idle sub-state (1) at phase 1,
 * writes the target into the demo level list at g_DemoIndex with sub-area 0,
 * then stores sub-state 21 into g_TitleSubState and g_TitlePrevState and
 * zeroes the five phase bytes, which is the idle timeout's transition without
 * its isDemoMode = 1 store. Seeded: when game.state reaches 5 (loading image)
 * with currentLevel equal to the target, the level counts as picked. Picked:
 * writes isDemoMode = 0 every call until game.state returns to the title
 * (11), which disarms. While a present fast-forward, a --skip-to-area target
 * or a --skip-to-cutscene target is armed, it also runs the release check
 * that clears openpete_skip_present_until_level. Inert while disarmed.
 * @param cpu The guest CPU state.
 * @param frame_counter openpete_frame, the host's per-VSync-yield counter;
 *        printed in the log lines and used for the fail-safe deadline. */
void spyro3_skip_on_tick(CPUState* cpu, uint32_t frame_counter);

/** @brief Report whether the loader took the --skip-to-level target.
 *
 * Stays set through the level until game.state returns to the title.
 * @return 1 while the skip is in its picked phase, else 0. */
int spyro3_skip_level_picked(void);

/** @brief Report whether a level overlay is resident and the game is in the gameplay state.
 *
 * Reads loadStage (0x8006C518) for -1 and game.state (0x8006E344) for 0.
 * @param cpu The guest CPU state.
 * @return 1 when both hold, else 0. */
int spyro3_skip_gameplay_live(CPUState* cpu);

#endif
