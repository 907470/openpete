#pragma once
/* levelwarp.h - jump to any level from the dev overlay (M key).
 *
 * Owns the canonical Spyro 1 level table (shared with --skip-to-level), the
 * two dev-overlay rows ("Warp: level" dropdown + "Jump" button), and the
 * tick-side state machine that performs every guest write. See
 * levelwarp.c for the mechanism.
 *
 * Called from game_extras_impl.c (game_on_frame_pre + the savestate extra
 * blob). */

#include <stddef.h>
#include <stdint.h>

typedef struct CPUState CPUState;

/* One selectable level. `name` is the --skip-to-level spelling, `display`
 * the dev-overlay dropdown label. Table order is level-id order. */
typedef struct {
    const char* name;
    const char* display;
    int         id;
} spyro1_level_t;

/** @brief Returns the table of the 35 levels, in id order.
 *
 * Alias spellings (gnastys_world, twilight_harbour) are a separate list so
 * the dropdown shows each level once; names resolve through
 * spyro1_level_resolve, which also searches the aliases.
 * @param out_count Receives the entry count, or NULL.
 * @return The static table. */
const spyro1_level_t* spyro1_level_table(size_t* out_count);

/** @brief Resolves a --skip-to-level style argument to a level id.
 *
 * Accepts an all-digit id, a level name, or an alias, case-insensitive with
 * '-' accepted for '_' and apostrophes not dropped. An all-digit id passes
 * through unchecked (NAMED_TARGET_NUMERIC_ANY): the warp takes the id the
 * caller names rather than indexing the table with it.
 * @param arg The argument, or NULL.
 * @return The level id, or -1 for an unknown or empty name after printing
 *     the valid list to stderr. */
int spyro1_level_resolve(const char* arg);

struct openpete_surface_sink;
/** @brief Report every level name --skip-to-level accepts to the surface dump.
 *
 * @param sink The dump's collector. */
void spyro1_level_surface(struct openpete_surface_sink* sink);

/** @brief Drives the level-warp state machine for one tick.
 *
 * Runs levelwarp_ui_tick first, which refreshes the route the overlay button
 * gates on, drains a queued request, and ends the call on a speculative tick
 * or a tick that drained or held a request; so this runs every tick even
 * when no warp is pending, once per tick from game_on_frame_pre. While
 * armed, the g_Gamestate edge to GS_LevelTransition retargets the load and
 * any state other than GS_ExitLevel aborts the warp. While loading, the
 * return to GS_Playing ends the warp and logs the level reached. A warp
 * still in flight after 20000 calls is aborted.
 * @param cpu The guest CPU state. */
void spyro1_levelwarp_on_tick(CPUState* cpu);

/** @brief Copies the warp state machine out for a savestate.
 *
 * The phase is host state that gates guest writes, so it travels with every
 * snapshot, file and in-memory, in the game's extra section beside the thunk
 * layer's latch pack.
 * @param phase Receives the phase (idle, armed, loading).
 * @param target Receives the target level id.
 * @param ticks Receives the phase's call count. */
void spyro1_levelwarp_state_save(uint32_t* phase, uint32_t* target,
                                 uint32_t* ticks);
/** @brief Restores the warp state machine from a savestate.
 *
 * @param phase The phase (idle, armed, loading).
 * @param target The target level id.
 * @param ticks The phase's call count. */
void spyro1_levelwarp_state_load(uint32_t phase, uint32_t target,
                                 uint32_t ticks);
