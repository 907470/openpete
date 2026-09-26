#pragma once
/* levelwarp_ui.h — the game-independent half of the dev-overlay level warp
 * (M key): the level dropdown, the Jump button, the click queue and the
 * scripted driver.
 *
 * The overlay runs outside the sim, so a click only queues a request. The
 * queue is drained on the next non-speculative tick, and every guest write
 * happens in the per-game half this module calls back into. Unclicked and
 * undriven, nothing here reads or writes guest memory beyond the route
 * probe the descriptor supplies.
 *
 * A game supplies a levelwarp_ui_desc_t: its level table, the route its
 * current guest state allows, and the two things only it can do — start the
 * in-level route and report an in-flight warp. The title-screen route is the
 * same for every game (game_set_skip_to_level), so this module owns it. */

#include <stddef.h>
#include <stdint.h>

typedef struct CPUState CPUState;

enum {
    LEVELWARP_ROUTE_NONE = 0,  /* no warp is possible from this guest state */
    LEVELWARP_ROUTE_LEVEL,     /* in a level: the game's own level transition */
    LEVELWARP_ROUTE_TITLE      /* at the title: the --skip-to-level machinery */
};

typedef struct {
    /* Log tag and the dropdown's entries, in table order. `display` must
     * outlive the process: the knob registry copies the row, not the
     * strings it points at. */
    const char*        log_tag;
    const char* const* display;
    int                count;

    /* index -> the game's own level number, and the reverse of the
     * --skip-to-level resolver's numeric arm. */
    int  (*level_id)(int index);

    /* The --skip-to-level argument resolver, for the scripted driver. */
    int  (*resolve)(const char* arg);

    /* The route the live guest state allows, ignoring whether a warp is
     * already queued or running. LEVELWARP_ROUTE_LEVEL is vetoed here when
     * the debug free camera is engaged. */
    int  (*route)(CPUState* cpu);

    /* The level number the player is in, or -1. Drives the dropdown's
     * auto-follow until the user picks something. */
    int  (*current_level)(CPUState* cpu);

    /* Put the game into its own level transition toward `target_id`. Called
     * on a non-speculative tick with LEVELWARP_ROUTE_LEVEL live. */
    void (*start_level_route)(CPUState* cpu, int target_id);

    /* A warp this module started has not finished. Gates the button and the
     * dropdown's auto-follow. */
    int  (*in_flight)(void);

    /* Optional status text for an in-flight warp ("loading...", ...).
     * Writes nothing and returns 0 when no warp is in flight. */
    int  (*in_flight_status)(char* buf, size_t cap);
} levelwarp_ui_desc_t;

/** @brief Register the two dev-overlay rows and arm the scripted driver.
 *
 * Resets the dropdown selection, the queued click and the cached route, adds
 * the "Warp: level" dropdown and the "Warp: jump to level" action row, and
 * reads OPENPETE_LEVELWARP_TEST through the descriptor's resolver. Called once
 * from the feature module's apply_options.
 * @param desc The game's descriptor; it must outlive the process. */
void levelwarp_ui_register(const levelwarp_ui_desc_t* desc);

/** @brief Run the per-tick front half: refresh the route, follow the dropdown, and drain a queued request.
 *
 * Queues a due scripted request first, so it lands where a click lands. The
 * route is the descriptor's, with LEVELWARP_ROUTE_LEVEL vetoed while the free
 * camera is engaged. Until the user picks a level and while no warp is in
 * flight, the dropdown follows the current level. A queued request drains
 * once no warp is in flight: the title route through game_set_skip_to_level,
 * the in-level route through the descriptor's start_level_route, and with no
 * route it is logged and dropped.
 * @param cpu The guest CPU state.
 * @return 1 when the caller must not advance its own state machine this
 *         tick: a speculative tick, a tick holding a request behind an
 *         in-flight warp, or a tick that drained a request; else 0. */
int levelwarp_ui_tick(CPUState* cpu);
