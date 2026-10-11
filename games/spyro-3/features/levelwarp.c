// games/spyro-3/features/levelwarp.c - jump to any level from the dev
// overlay (M key), from anywhere.
//
// --skip-to-level only works at boot: it hijacks the title screen's demo-
// timeout transition once and then latches. This module adds a level
// dropdown + Warp button to the overlay's Gameplay section that works
// mid-session, so switching levels for an A/B is not a relaunch. The
// dropdown, the button and the click queue are the shared overlay half
// (games/common/features/levelwarp_ui.c); this file supplies the route
// probe and the request.
//
// Two routes, picked from the live game state:
//
// 1. In a level: the game's own level transition, the one a portal moby
//    enters: WarpToLevel(0, target) (func_800584BC; every level overlay's
//    moby code calls it, e.g. func_level_10_8007EEBC at 0x8007F094), called
//    in guest context through the transition queue (transition_call.c),
//    which drains it at Draw's entry after that tick's Update. The one store
//    before the call is area 0: a portal entry picks a sub-area, and leaving
//    the previous level's choice in place would land the warp in a sub-area
//    the target may not have. The loading overlay's state machine then runs
//    the load.
//
// 2. At the title screen: hand off to the existing --skip-to-level
//    machinery (game_set_skip_to_level), which induces the demo-timeout
//    transition at the next Press-Start window. That path resets progress,
//    exactly as a fresh --skip-to-level boot does.
//
// The overlay runs outside the sim, so a click only queues a request; every
// guest write happens in the drain. Intentional, opt-in gameplay divergence
// like the debug freecam: unclicked, this module writes zero guest words, so
// a default run stays oracle-clean.
//
// Addresses from spyro-3/symbol_addrs.txt.

#include "levelwarp.h"

#include "feature_registry.h"
#include "features/levelwarp_ui.h"
#include "transition_call.h"
#include "openpete_fiber.h"         /* openpete_frame */
#include "openpete_log.h"
#include "psx_runtime_internal.h"   /* CPUState + its read/write callbacks */
#include "spyro-3/skip_to_level.h"

#include <stdio.h>

#define S3_GAME_STATE       0x8006E344u /* 0 gameplay, 5 loading image, 11 title */
#define S3_CURRENT_LEVEL    0x8006C5BCu
#define S3_AREA             0x8006C5C8u
#define S3_IS_DEMO_MODE     0x8006C658u
#define S3_GS_TITLE         11u

/* Backstop for a warp that never resolves. The phase ends on a game state
 * the game itself reaches, so this only fires if the game parks somewhere
 * unanticipated; it must not double as a deadline. Counted in hook calls
 * (one per sim yield): a CD load plus fly-in can run past 3600 in a headless
 * free-running session, which is why the phase does not lean on it. */
#define WARP_STUCK_CALLS 20000u

/* ---- state ----------------------------------------------------------- */

typedef enum {
    WARP_IDLE = 0,
    WARP_QUEUED,   /* the request waits in the transition queue */
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

/** @brief Maps a dropdown index to its level number in the Spyro 3 level table.
 *
 * @param index The dropdown index.
 * @return The level number, or -1 for an index out of range. */
static int warp_level_id(int index) {
    size_t  n;
    const spyro3_level_t* t = spyro3_level_table(&n);
    return (index >= 0 && (size_t)index < n) ? t[index].number : -1;
}

/** @brief Reports whether a warp this module started has not finished.
 *
 * The shared overlay half reads it to gate the Warp button and the dropdown's
 * auto-follow.
 *
 * @return 1 while the phase is not WARP_IDLE, else 0. */
static int warp_in_flight(void) { return s_phase != WARP_IDLE; }

/** @brief Writes the in-flight status text for the overlay while a warp runs.
 *
 * @param buf Receives "loading..."; untouched when no warp is in flight.
 * @param cap The size of buf in bytes.
 * @return 1 when buf was written, 0 when no warp is in flight. */
static int warp_in_flight_status(char* buf, size_t cap) {
    if (s_phase == WARP_IDLE) return 0;
    snprintf(buf, cap, "loading...");
    return 1;
}

/** @brief Picks the warp route the live guest state allows.
 *
 * The title route when game.state is 11; the in-level route when gameplay is
 * live and isDemoMode is 0, since the attract demo runs the gameplay state
 * with a level resident and must be excluded explicitly; else none.
 * @param cpu The guest CPU state.
 * @return A LEVELWARP_ROUTE_* value. */
static int warp_route(CPUState* cpu) {
    if (cpu->read_word(S3_GAME_STATE) == S3_GS_TITLE)
        return LEVELWARP_ROUTE_TITLE;
    if (spyro3_skip_gameplay_live(cpu) &&
        cpu->read_word(S3_IS_DEMO_MODE) == 0u)
        return LEVELWARP_ROUTE_LEVEL;
    return LEVELWARP_ROUTE_NONE;
}

/** @brief Reads the guest's currentLevel word 0x8006C5BC.
 *
 * The dropdown follows it until the user picks a level. It never reports -1.
 *
 * @param cpu The guest CPU state.
 * @return The level number the game holds. */
static int warp_current_level(CPUState* cpu) {
    return (int)cpu->read_word(S3_CURRENT_LEVEL);
}

/** @brief Tells the transition drain whether the in-level warp route is still open.
 *
 * Passed as the request's route callback. The drain calls it at Draw's entry,
 * after the tick's Update; a 0 drops the request and runs warp_dropped.
 *
 * @param cpu The guest CPU state.
 * @return 1 when warp_route reports LEVELWARP_ROUTE_LEVEL, else 0. */
static int warp_route_level(CPUState* cpu) {
    return warp_route(cpu) == LEVELWARP_ROUTE_LEVEL;
}

/** @brief Moves the warp to WARP_LEAVING and restarts its tick count after the drain made the call.
 *
 * Passed as the request's called callback.
 *
 * @param cpu Unused. */
static void warp_called(CPUState* cpu) {
    (void)cpu;
    s_phase = WARP_LEAVING;
    s_ticks = 0;
}

/** @brief Resets the warp to WARP_IDLE when the drain dropped the request.
 *
 * Passed as the request's dropped callback. The target and the tick count are
 * cleared.
 *
 * @param cpu Unused. */
static void warp_dropped(CPUState* cpu) {
    (void)cpu;
    s_phase  = WARP_IDLE;
    s_target = 0;
    s_ticks  = 0;
}

static const s3_transition_store_t k_warp_stores[] = {
    { S3_AREA, 4u, 0u, 0u },
};

/** @brief Queues WarpToLevel func_800584BC(0, target_id) through the transition queue.
 *
 * The request drains at the next Draw func_8001E638 entry, after this tick's
 * Update; the drain re-reads warp_route_level, then writes 0 to the area word
 * 0x8006C5C8 before the call. A request made while another is queued is
 * ignored with a log line; an accepted one moves the warp to WARP_QUEUED.
 * @param cpu The guest CPU state.
 * @param target_id The level number to warp to. */
static void warp_start(CPUState* cpu, int target_id) {
    s3_transition_request_t req = {
        .tag      = "levelwarp",
        .entry    = S3_TRANSITION_AT_DRAW,
        .fn       = S3_FN_WARP_TO_LEVEL,
        .a0       = 0u,
        .a1       = (uint32_t)target_id,
        .stores   = k_warp_stores,
        .n_stores = sizeof(k_warp_stores) / sizeof(k_warp_stores[0]),
        .route    = warp_route_level,
        .called   = warp_called,
        .dropped  = warp_dropped,
    };
    uint32_t cur = cpu->read_word(S3_CURRENT_LEVEL);
    if (!s3_transition_enqueue(&req)) {
        OPL_INFO_F("levelwarp", "%s", "ignored (a transition is already queued)");
        return;
    }
    s_phase  = WARP_QUEUED;
    s_target = (uint32_t)target_id;
    s_ticks  = 0;
    OPL_INFO_F("levelwarp", "queued: leaving level %u for %d",
               (unsigned)cur, target_id);
}

/* The level count is only readable at run time, so the descriptor is filled
 * rather than initialised. */
static levelwarp_ui_desc_t s_ui;

/** @brief Resets the warp state, copies the level display strings and registers the overlay rows.
 *
 * The display array holds at most 64 entries, so a longer level table is
 * truncated in the dropdown. The descriptor s_ui is filled here, since the
 * level count is known only at run time, then handed to levelwarp_ui_register.
 *
 * @param opts Unused. */
static void levelwarp_apply_options(const openpete_options_t* opts) {
    size_t  n, i;
    const spyro3_level_t* t = spyro3_level_table(&n);
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
    s_ui.resolve           = spyro3_level_resolve;
    s_ui.route             = warp_route;
    s_ui.current_level     = warp_current_level;
    s_ui.start_level_route = warp_start;
    s_ui.in_flight         = warp_in_flight;
    s_ui.in_flight_status  = warp_in_flight_status;
    levelwarp_ui_register(&s_ui);
}

/* ---- the warp itself -------------------------------------------------- */

void spyro3_levelwarp_on_tick(CPUState* cpu) {
    /* The shared half refreshes the route, follows the dropdown and drains a
     * queued click into warp_start above or into the title route. It reports
     * back whether this tick may advance the sequence. */
    if (levelwarp_ui_tick(cpu)) return;
    if (s_phase == WARP_IDLE || s_phase == WARP_QUEUED) return;

    if (++s_ticks > WARP_STUCK_CALLS) {
        OPL_INFO_F("levelwarp", "%s", "aborted (stuck)");
        s_phase  = WARP_IDLE;
        s_target = 0;
        s_ticks  = 0;
        return;
    }

    /* The loader has the wheel and these phases write nothing - they
     * only report status and keep a second warp from stacking. Arrival is
     * read only once the level the warp left has stopped being live; then it
     * waits for the next level to come up, however long the CD load takes. */
    if (s_phase == WARP_LEAVING) {
        if (!spyro3_skip_gameplay_live(cpu)) s_phase = WARP_LOADING;
        return;
    }

    if (spyro3_skip_gameplay_live(cpu)) {
        uint32_t landed = cpu->read_word(S3_CURRENT_LEVEL);
        if (landed == s_target)
            OPL_INFO_F("levelwarp", "arrived in level %u at vbl=%u",
                       (unsigned)landed, (unsigned)openpete_frame);
        else
            OPL_INFO_F("levelwarp", "landed in level %u, wanted %u",
                       (unsigned)landed, (unsigned)s_target);
        s_phase  = WARP_IDLE;
        s_target = 0;
        s_ticks  = 0;
    }
}

const feature_module_t spyro3_levelwarp_module = {
    .name           = "levelwarp",
    .apply_options  = levelwarp_apply_options,
    .dispatch_table = NULL,
};
