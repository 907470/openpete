#pragma once
/* skip_to_area.h — --skip-to-area <level>:<area> for Spyro 3: the level by
 * --skip-to-level, then the sub-area by the game's EnterSubArea
 * func_80052918, called through the transition queue with the level's
 * area-0 portal pair. See skip_to_area.c. */

#include <stddef.h>
#include <stdint.h>

typedef struct CPUState CPUState;

/** @brief Resolves a --skip-to-area LEVEL:AREA argument for game_resolve_area_arg.
 *
 * The level part goes through spyro3_level_resolve (number or name); the area
 * part must be decimal digits naming an area 1..count-1 of a level in the
 * sub-area table. A missing colon or an invalid area prints the valid targets
 * to stderr; an unknown level prints the level list.
 * @param arg The argument.
 * @param out_area Receives the area index on success.
 * @return The level number, or -1. */
int spyro3_area_resolve(const char* arg, int* out_area);

/** @brief Arms a --skip-to-area target for game_set_skip_to_area.
 *
 * A level above 0 arms --skip-to-level `level` through spyro3_skip_set and the
 * sub-area entry; 0 or less disarms the entry and leaves the level skip as it
 * is.
 * @param level The level number.
 * @param area The sub-area index. */
void spyro3_area_set(int level, int area);

/** @brief Advances the sub-area entry by one tick, from game_on_frame_pre.
 *
 * Inert while disarmed and on a speculative tick. Armed, once the level skip
 * has picked its target and the route holds (gameplay live, isDemoMode clear,
 * area word 0, the target level current), it queues EnterSubArea(area, the
 * area-0 portal's paired moby) for the next Draw entry, or disarms with a
 * warning when the level has no such portal. After the call it waits for
 * game.state 13, then releases, with a log line, when gameplay is live with
 * the area word 0x8006C5C8 equal to the target.
 * @param cpu The guest CPU state.
 * @param frame_counter The openpete_frame counter printed on the release line. */
void spyro3_area_on_tick(CPUState* cpu, uint32_t frame_counter);

/** @brief Reports whether an area target is armed and its release or fail-safe line is not yet logged.
 *
 * The skip release block (skip_to_level.c) watches the target every tick
 * while this holds, headless too.
 * @return 1 while the target is watched, else 0. */
int spyro3_area_watching(void);

/** @brief Reports whether an area target is armed and not yet released.
 *
 * Holds from the level skip through the sub-area load. The scripted free
 * camera engage waits on it, since the sub-area transition re-seeds the camera
 * from the game's.
 * @return 1 while the target is pending, else 0. */
int spyro3_area_pending(void);

/** @brief Reports whether the armed target's release predicate has held.
 *
 * The predicate: game.state 13 seen after the call, then gameplay live with
 * the area word equal to the target. On a release, writes " level=L area=A",
 * read back from 0x8006C5BC and 0x8006C5C8, to eng.
 * @param cpu The guest CPU state.
 * @param eng Receives the release line's suffix.
 * @param cap The capacity of eng in bytes.
 * @return 1 once released, else 0. */
int spyro3_area_released(CPUState* cpu, char* eng, size_t cap);

/** @brief Ends the watch after the skip release block has logged the target's line. */
void spyro3_area_finish(void);
