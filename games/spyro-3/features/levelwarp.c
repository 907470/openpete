// games/spyro-3/features/levelwarp.c - jump to any level from the dev
// overlay (M key), from anywhere.
//
// --skip-to-level only works at boot: it hijacks the title screen's demo-
// timeout transition once and then latches. This module adds a level
// dropdown + Warp button to the overlay's Gameplay section that works
// mid-session, so switching levels for an A/B is not a relaunch. The
// dropdown, the button, the click queue and the in-flight phases are the
// shared warp (games/common/features/levelwarp_ui.c); this file supplies the
// route words and the request.
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
#include "areawarp.h"

#include "feature_registry.h"
#include "features/levelwarp_ui.h"
#include "transition_call.h"
#include "openpete_log.h"
#include "psx_runtime_internal.h"   /* CPUState + its read/write callbacks */
#include "spyro-3/skip_to_level.h"

#define S3_GAME_STATE       0x8006E344u /* 0 gameplay, 5 loading image, 11 title */
#define S3_CURRENT_LEVEL    0x8006C5BCu
#define S3_AREA             0x8006C5C8u
#define S3_IS_DEMO_MODE     0x8006C658u
#define S3_GS_TITLE         11u

static levelwarp_t s_warp;

/** @brief Tells the transition drain whether the in-level warp route is still open.
 *
 * Passed as the request's route callback. The drain calls it at Draw's entry,
 * after the tick's Update; a 0 drops the request and runs warp_dropped.
 *
 * @param cpu The guest CPU state.
 * @return 1 when gameplay is live, isDemoMode is 0 and game.state is not the
 *         title, else 0. */
static int warp_route_level(CPUState* cpu) {
    return cpu->read_word(S3_GAME_STATE) != S3_GS_TITLE &&
           spyro3_skip_gameplay_live(cpu) &&
           cpu->read_word(S3_IS_DEMO_MODE) == 0u;
}

/** @brief Moves the warp to LEVELWARP_LEAVING after the drain made the call.
 *
 * @param cpu Unused. */
static void warp_called(CPUState* cpu) { (void)cpu; levelwarp_called(&s_warp); }

/** @brief Ends the warp when the drain dropped the request.
 *
 * @param cpu Unused. */
static void warp_dropped(CPUState* cpu) { (void)cpu; levelwarp_reset(&s_warp); }

static const s3_transition_store_t k_warp_stores[] = {
    { S3_AREA, 4u, 0u, 0u },
};

int spyro3_levelwarp_enqueue(const char* tag, int target_id,
                             void (*called)(CPUState* cpu),
                             void (*dropped)(CPUState* cpu)) {
    s3_transition_request_t req = {
        .tag      = tag,
        .entry    = S3_TRANSITION_AT_DRAW,
        .fn       = S3_FN_WARP_TO_LEVEL,
        .a0       = 0u,
        .a1       = (uint32_t)target_id,
        .stores   = k_warp_stores,
        .n_stores = sizeof(k_warp_stores) / sizeof(k_warp_stores[0]),
        .route    = warp_route_level,
        .called   = called,
        .dropped  = dropped,
    };
    return s3_transition_enqueue(&req);
}

/** @brief Queues the level warp's WarpToLevel request through spyro3_levelwarp_enqueue.
 *
 * A request made while another is queued is ignored with a log line.
 * @param w The warp; unused.
 * @param cpu The guest CPU state.
 * @param target_id The level number to warp to.
 * @return LEVELWARP_QUEUED, or LEVELWARP_IDLE when ignored. */
static int warp_start(levelwarp_t* w, CPUState* cpu, int target_id) {
    uint32_t cur = cpu->read_word(S3_CURRENT_LEVEL);
    (void)w;
    if (!spyro3_levelwarp_enqueue("levelwarp", target_id, warp_called, warp_dropped)) {
        OPL_INFO_F("levelwarp", "%s", "ignored (a transition is already queued)");
        return LEVELWARP_IDLE;
    }
    OPL_INFO_F("levelwarp", "queued: leaving level %u for %d",
               (unsigned)cur, target_id);
    return LEVELWARP_QUEUED;
}

/* The title route opens on game.state 11; the in-level route while gameplay
 * is live and isDemoMode is 0, since the attract demo runs the gameplay
 * state with a level resident. The dropdown follows currentLevel. */
static const levelwarp_game_t k_warp_game = {
    .log_tag          = "levelwarp",
    .rows             = levelwarp_level_rows,
    .test_env         = "OPENPETE_LEVELWARP_TEST",
    .table            = &spyro3_levels,
    .unit             = "level",
    .title_word       = S3_GAME_STATE,
    .title_value      = S3_GS_TITLE,
    .demo_word        = S3_IS_DEMO_MODE,
    .live             = spyro3_skip_gameplay_live,
    .current_word     = S3_CURRENT_LEVEL,
    .start            = warp_start,
    .leaving_status   = "loading...",
    .arrival_logs_vbl = 1,
};

static levelwarp_t s_warp = { .game = &k_warp_game };

/** @brief Feature-module option hook that registers the level warp's overlay rows, then the area warp's.
 *
 * @param opts Unused. */
static void levelwarp_apply_options(const openpete_options_t* opts) {
    (void)opts;
    levelwarp_register(&s_warp);
    spyro3_areawarp_register();
}

void spyro3_levelwarp_on_tick(CPUState* cpu) {
    levelwarp_on_tick(&s_warp, cpu);
    spyro3_areawarp_on_tick(cpu);
}

const feature_module_t spyro3_levelwarp_module = {
    .name           = "levelwarp",
    .apply_options  = levelwarp_apply_options,
    .dispatch_table = NULL,
};
