#pragma once
/* levelwarp.h — jump to any level from the dev overlay (M key).
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

/* The 35 real levels, in id order. Alias spellings ("gnastys_world") are a
 * separate list so the dropdown shows each level exactly once — resolve
 * names through spyro1_level_resolve, not by scanning this. */
const spyro1_level_t* spyro1_level_table(size_t* out_count);

/* Resolve a --skip-to-level style argument (numeric id, level name, or
 * alias; '-' accepted for '_', case-insensitive) to a level id. Returns -1
 * for an unknown or empty name, printing the valid list to stderr. */
int spyro1_level_resolve(const char* arg);

/* Drive the warp state machine: drain a queued overlay request, ride the
 * game's exit-level sequence, and retarget the load. Also refreshes the
 * "can warp right now" state the overlay button gates on, so it must run
 * every tick even when no warp is pending. Call once per tick from
 * game_on_frame_pre. */
void spyro1_levelwarp_on_tick(CPUState* cpu);

/* Savestate participation. The warp FSM is host state that gates guest
 * writes, so it has to travel with every snapshot — including the quiet
 * per-tick rewind/runahead ones, whose rollback would otherwise strand a
 * half-applied warp, beside the thunk layer's latch pack. */
void spyro1_levelwarp_state_save(uint32_t* phase, uint32_t* target,
                                 uint32_t* ticks);
void spyro1_levelwarp_state_load(uint32_t phase, uint32_t target,
                                 uint32_t ticks);
