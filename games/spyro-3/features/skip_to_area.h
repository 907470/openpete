#pragma once
/* skip_to_area.h - --skip-to-area <level>:<area> for Spyro 3: the level by
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

/** @brief Returns the number of areas of a level, from the level WAD header's area table.
 *
 * @param level The level number.
 * @return The count; 1 (area 0 only) for a level with no sub-area. */
int spyro3_area_count(int level);

/** @brief Finds the paired moby of the portal standing in one area and leading to another.
 *
 * Walks the moby table from *(0x8006C550) to *(0x8006C704) in 88-byte
 * strides for a class-1022 moby (the half at moby+54) whose props, *(moby+0),
 * hold `from` at props+64 (the area it stands in) and `to` at props+52 (its
 * destination). The fade-in's sub-area re-entry (func_80053A10, 0x80053D14)
 * walks it so from area 0, the pause menu's Exit Area (func_80057834,
 * 0x80058278) from the current area to area 0; both then call
 * EnterSubArea(props+52, table base + props+48 * 88).
 * @param cpu The guest CPU state.
 * @param from The area the portal stands in.
 * @param to The destination area.
 * @return The paired moby's address, or 0 when no such portal exists. */
uint32_t spyro3_area_portal_pair(CPUState* cpu, int from, int to);

struct openpete_surface_sink;
/** @brief Report every --skip-to-area target to the surface dump, as "<level name>:<area>".
 *
 * @param sink The dump's collector. */
void spyro3_area_surface(struct openpete_surface_sink* sink);

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
