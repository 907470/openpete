// games/spyro-2/features/levelwarp.c - jump to any level from the dev
// overlay (M key), from anywhere.
//
// --skip-to-level only works at boot: it hijacks the title screen's demo-
// timeout transition once and then latches. This module adds a level
// dropdown + Warp button to the overlay's Gameplay section that works
// mid-session, so switching levels for an A/B is not a relaunch. The
// dropdown, the button and the click queue are the shared overlay half
// (games/common/features/levelwarp_ui.c); this file supplies the route
// probe and the guest writes.
//
// Two routes, picked from the live game state:
//
// 1. In a level: end the level the way the attract demo's recording
//    running out ends it, then hand the title screen to route 2. The
//    per-tick check at 0x80015ACC (func_800159C8) is gated on the demo-mode
//    word and fires on the flag its fade sets when the recording is spent;
//    from there the game runs its own return to the title, including the
//    loading overlay it pulls in over the level's. Two writes, no guest
//    function calls, and the destination is set before the level goes, so
//    the title screen loads it without ever being presented.
//
//    Two shorter routes into the loader were disproved, both on the overlay
//    the level's own overlay shadows:
//      - g_NextLevelNum plus the loader state the transition entry
//        func_80019230 sets (its type-0 row): the loader calls into
//        0x80070498, an address the resident level overlay owns, and the
//        game hangs there.
//      - the same with the transition entry's portal rows (5, 9, 13): the
//        loader state never advances and the update spins calling
//        0x800709FC, again an address the level overlay owns.
//    Both mean the loading overlay must be pulled in by the game's own exit
//    before anything calls into it, which is what the demo-end path does.
//
//    Progress and lives reset, since the title screen is where the game
//    parks between levels and route 2 restarts from it.
//
// 2. At the title screen: hand off to the existing --skip-to-level
//    machinery (spyro2_skip_set), which induces the demo-timeout
//    transition at the next Press-Start window. That path resets progress,
//    exactly as a fresh --skip-to-level boot does.
//
// The overlay runs outside the sim, so a click only queues a request; every
// guest write happens in the per-tick hook below. Intentional, opt-in
// gameplay divergence like the debug freecam: unclicked, this module writes
// zero guest words, so a default run stays oracle-clean.
//
// Every write lands in one tick, so an in-flight warp holds no guest state
// of its own: the remaining phases only watch for the level to go and the
// next one to come up, and a savestate load across them strands nothing.
//
// Addresses from spyro-2/psx.map and the asm cited above.

#include "levelwarp.h"

#include "feature_registry.h"
#include "features/levelwarp_ui.h"
#include "openpete_log.h"
#include "psx_runtime_internal.h"   /* CPUState + its read/write callbacks */
#include "spyro-2/skip_to_level.h"

#include <stdio.h>

#define S2_LOADER_STATE     0x80066EECu /* 0 title, 2 loading, -1 level resident */
#define S2_NEXT_LEVEL_NUM   0x80066F90u /* the level number the loader loads */
#define S2_DEMO_MODE        0x80067024u /* 1 while an attract demo is running */
#define S2_DEMO_SPENT       0x80067178u /* set when the recording has run out */

#define S2_LOADER_TITLE     0u

/* Backstop for a warp that never resolves. The phase ends on a loader state
 * the game itself reaches, so this only fires if the game parks somewhere
 * unanticipated; it must not double as a deadline. Counted in hook calls
 * (one per sim yield): a CD load can run past 3600 in a headless
 * free-running session, which is why the phase does not lean on it. */
#define WARP_STUCK_CALLS 20000u

/* ---- state ----------------------------------------------------------- */

typedef enum {
    WARP_IDLE = 0,
    WARP_LEAVING,  /* the transition is entered; waiting for the level to go */
    WARP_LOADING   /* the level is gone; waiting for arrival */
} warp_phase_t;

static uint32_t s_phase  = WARP_IDLE;
static uint32_t s_target = 0;
static uint32_t s_ticks  = 0;

/* The dropdown's display strings, built once at apply-options time: the knob
 * registry copies the row but NOT the strings it points at. */
static const char* s_display[64];
static int         s_count;

/* ---- the shared overlay half ------------------------------------------ */

/** @brief Maps a dropdown index to its level number in the Spyro 2 level table.
 *
 * @param index The dropdown index.
 * @return The level number, or -1 for an index out of range. */
static int warp_level_id(int index) {
    size_t  n;
    const spyro2_level_t* t = spyro2_level_table(&n);
    return (index >= 0 && (size_t)index < n) ? t[index].number : -1;
}

/** @brief Reports whether a warp this module started has not finished.
 *
 * @return 1 while the phase is not WARP_IDLE, else 0. */
static int warp_in_flight(void) { return s_phase != WARP_IDLE; }

/** @brief Writes the overlay status text of an in-flight warp.
 *
 * @param buf Receives "loading..." when a warp is in flight; untouched
 *        otherwise.
 * @param cap Size of buf in bytes.
 * @return 1 when a warp is in flight, else 0. */
static int warp_in_flight_status(char* buf, size_t cap) {
    if (s_phase == WARP_IDLE) return 0;
    snprintf(buf, cap, "loading...");
    return 1;
}

/** @brief Picks the warp route the live guest state allows.
 *
 * The title route when the loader state is 0; the in-level route when
 * gameplay is live and g_DemoMode is 0, since the attract demo runs a level
 * with the loader resident and must be excluded explicitly; else none.
 * @param cpu The guest CPU state.
 * @return A LEVELWARP_ROUTE_* value. */
static int warp_route(CPUState* cpu) {
    if (cpu->read_word(S2_LOADER_STATE) == S2_LOADER_TITLE)
        return LEVELWARP_ROUTE_TITLE;
    if (spyro2_skip_gameplay_live(cpu) &&
        cpu->read_word(S2_DEMO_MODE) == 0u)
        return LEVELWARP_ROUTE_LEVEL;
    return LEVELWARP_ROUTE_NONE;
}

/** @brief Reads the level number the loader holds, which the dropdown follows.
 *
 * @param cpu The guest CPU state.
 * @return g_NextLevelNum (0x80066F90) as read; never -1. */
static int warp_current_level(CPUState* cpu) {
    return (int)cpu->read_word(S2_NEXT_LEVEL_NUM);
}

/** @brief Starts the in-level warp by ending the level the way a spent attract-demo recording ends it.
 *
 * Writes the demo-end check's own state: the demo-mode word it is gated on
 * (0x80067024) and the flag its fade sets when the recording is spent
 * (0x80067178). spyro2_skip_set then hands the title screen the target, and
 * the phase moves to WARP_LEAVING.
 * @param cpu The guest CPU state.
 * @param target_id The destination level number. */
static void warp_start(CPUState* cpu, int target_id) {
    uint32_t cur = cpu->read_word(S2_NEXT_LEVEL_NUM);

    cpu->write_word(S2_DEMO_MODE, 1u);
    cpu->write_word(S2_DEMO_SPENT, 1u);
    spyro2_skip_set(target_id);

    s_phase  = WARP_LEAVING;
    s_target = (uint32_t)target_id;
    s_ticks  = 0;
    OPL_INFO_F("levelwarp", "leaving level %u for %d",
               (unsigned)cur, target_id);
}

/* The level count is only readable at run time, so the descriptor is filled
 * rather than initialised. */
static levelwarp_ui_desc_t s_ui;

/** @brief Feature-module option hook that resets the warp state and registers the overlay half.
 *
 * Fills the dropdown strings from the Spyro 2 level table, at most 64
 * entries, and the descriptor's callbacks, then registers it with the
 * shared overlay module.
 * @param opts Unused. */
static void levelwarp_apply_options(const openpete_options_t* opts) {
    size_t  n, i;
    const spyro2_level_t* t = spyro2_level_table(&n);
    (void)opts;

    s_phase  = WARP_IDLE;
    s_target = 0;
    s_ticks  = 0;

    if (n > sizeof(s_display) / sizeof(s_display[0]))
        n = sizeof(s_display) / sizeof(s_display[0]);
    for (i = 0; i < n; i++) s_display[i] = t[i].display;
    s_count = (int)n;

    s_ui.log_tag           = "levelwarp";
    s_ui.display           = s_display;
    s_ui.count             = s_count;
    s_ui.level_id          = warp_level_id;
    s_ui.resolve           = spyro2_level_resolve;
    s_ui.route             = warp_route;
    s_ui.current_level     = warp_current_level;
    s_ui.start_level_route = warp_start;
    s_ui.in_flight         = warp_in_flight;
    s_ui.in_flight_status  = warp_in_flight_status;
    levelwarp_ui_register(&s_ui);
}

/* ---- the warp itself -------------------------------------------------- */

void spyro2_levelwarp_on_tick(CPUState* cpu) {
    /* The shared half refreshes the route, follows the dropdown and drains a
     * queued click into warp_start above or into the title route. It reports
     * back whether this tick may advance the sequence. */
    if (levelwarp_ui_tick(cpu)) return;
    if (s_phase == WARP_IDLE) return;

    if (++s_ticks > WARP_STUCK_CALLS) {
        OPL_INFO_F("levelwarp", "%s", "aborted (stuck)");
        s_phase  = WARP_IDLE;
        s_target = 0;
        s_ticks  = 0;
        return;
    }

    /* The loader has the wheel and these phases write nothing more - they
     * only report status and keep a second warp from stacking. The level the
     * warp left is still live on the tick the transition is entered, so
     * arrival is only read once it has gone; then it waits for the next
     * level to come up, however long the CD load takes. */
    if (s_phase == WARP_LEAVING) {
        if (!spyro2_skip_gameplay_live(cpu)) s_phase = WARP_LOADING;
        return;
    }

    if (spyro2_skip_gameplay_live(cpu)) {
        uint32_t landed = cpu->read_word(S2_NEXT_LEVEL_NUM);
        if (landed == s_target)
            OPL_INFO_F("levelwarp", "arrived in level %u", (unsigned)landed);
        else
            OPL_INFO_F("levelwarp", "landed in level %u, wanted %u",
                       (unsigned)landed, (unsigned)s_target);
        s_phase  = WARP_IDLE;
        s_target = 0;
        s_ticks  = 0;
    }
}

const feature_module_t spyro2_levelwarp_module = {
    .name           = "levelwarp",
    .apply_options  = levelwarp_apply_options,
    .dispatch_table = NULL,
};
