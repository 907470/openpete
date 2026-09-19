// games/spyro-3/features/levelwarp.c — jump to any level from the dev
// overlay (M key), from anywhere.
//
// --skip-to-level only works at boot: it hijacks the title screen's demo-
// timeout transition once and then latches. This module adds a level
// dropdown + Jump button to the overlay's Gameplay section that works
// mid-session, so switching levels for an A/B is not a relaunch. The
// dropdown, the button and the click queue are the shared overlay half
// (games/common/features/levelwarp_ui.c); this file supplies the route
// probe and the guest writes.
//
// Two routes, picked from the live game state:
//
// 1. In a level: enter the game's own level transition, the one a portal
//    moby enters (WarpToLevel, func_800584BC: every level overlay's moby
//    code calls it, e.g. func_level_10_8007EEBC at 0x8007F094). Its first
//    argument selects the loading-image pane and its second the level. The
//    writes below are that function's own stores for pType 0 with
//    isDemoMode clear and the game not in the credits roll, read off
//    spyro-3/asm/nonmatchings/warp/func_800584BC.s lines 191-236; the pane
//    fold (pLevelId / 10 - 1, or 3 from level 50) is its lines 211-220 in
//    src/warp.c. The loading overlay's state machine then runs the load,
//    so nothing here loads anything itself.
//
//    Its two calls that are not stores are skipped: func_8001FB10(0x14000)
//    and func_8003BEDC() stop the level's audio, which the loader restarts
//    from the level wad anyway.
//
// 2. At the title screen: hand off to the existing --skip-to-level
//    machinery (game_set_skip_to_level), which induces the demo-timeout
//    transition at the next Press-Start window. That path resets progress,
//    exactly as a fresh --skip-to-level boot does.
//
// The overlay runs outside the sim, so a click only queues a request; every
// guest write happens in the per-tick hook below. Intentional, opt-in
// gameplay divergence like the debug freecam: unclicked, this module writes
// zero guest words, so a default run stays oracle-clean.
//
// Every write lands in one tick, so an in-flight warp holds no guest state
// of its own: the remaining phase only waits for the level to come up and a
// savestate load across it strands nothing.
//
// Addresses from spyro-3/symbol_addrs.txt and the asm cited above.

#include "features/levelwarp.h"

#include "feature_registry.h"
#include "features/levelwarp_ui.h"
#include "openpete_log.h"
#include "psx_runtime_internal.h"   /* CPUState + its read/write callbacks */
#include "skip_to_level.h"

#include <stdio.h>

/* WarpToLevel's stores (func_800584BC). */
#define S3_NOT_CREDITS      0x8006C548u /* the loading caption is suppressed while set */
#define S3_SPEEDWAY_RUN     0x8006FA40u /* speedway attempt state */
#define S3_LOAD_CAPTION     0x8006FBD4u /* index into g_LoadingScreenStrings */
#define S3_GAME_STATE       0x8006E344u /* 0 gameplay, 5 loading image, 11 title */
#define S3_LOAD_IMG_SUBSTATE 0x8006FBC8u
#define S3_LOAD_IMG_FRAMES  0x8006FBC4u
#define S3_LOAD_IMG_PANE    0x8006FBCCu
#define S3_LOAD_STAGE       0x8006C518u
#define S3_PREV_LEVEL_INDEX 0x8006C60Cu
#define S3_LEVEL_INDEX      0x8006C58Cu
#define S3_PREV_LEVEL       0x8006C73Cu /* the spawn picker reads it on arrival */
#define S3_CURRENT_LEVEL    0x8006C5BCu
#define S3_AREA             0x8006C5C8u
#define S3_MUSIC_ENABLED    0x8006E49Cu
#define S3_IS_DEMO_MODE     0x8006C658u
#define S3_GS_GAMEPLAY       0u
#define S3_GS_LOADING_IMG    5u
#define S3_GS_TITLE         11u

/* The loading image's fade colour, written as three bytes per draw env. */
#define S3_FADE_A           0x8006FC15u
#define S3_FADE_B           0x8006FC89u
#define S3_FADE_LEVEL       8u

/* Backstop for a warp that never resolves. The phase ends on a game state
 * the game itself reaches, so this only fires if the game parks somewhere
 * unanticipated; it must not double as a deadline. Counted in hook calls
 * (one per sim yield): a CD load plus fly-in can run past 3600 in a headless
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

static int warp_level_id(int index) {
    size_t  n;
    const spyro3_level_t* t = spyro3_level_table(&n);
    return (index >= 0 && (size_t)index < n) ? t[index].number : -1;
}

static int warp_in_flight(void) { return s_phase != WARP_IDLE; }

static int warp_in_flight_status(char* buf, size_t cap) {
    if (s_phase == WARP_IDLE) return 0;
    snprintf(buf, cap, "loading...");
    return 1;
}

/* The attract demo runs the gameplay state with a level resident, so it
 * needs excluding explicitly. */
static int warp_route(CPUState* cpu) {
    if (cpu->read_word(S3_GAME_STATE) == S3_GS_TITLE)
        return LEVELWARP_ROUTE_TITLE;
    if (spyro3_skip_gameplay_live(cpu) &&
        cpu->read_word(S3_IS_DEMO_MODE) == 0u)
        return LEVELWARP_ROUTE_LEVEL;
    return LEVELWARP_ROUTE_NONE;
}

static int warp_current_level(CPUState* cpu) {
    return (int)cpu->read_word(S3_CURRENT_LEVEL);
}

/* Enter the transition, with the destination WarpToLevel's second argument
 * would have carried. The area is the level's first: a portal entry picks a
 * sub-area, and leaving the previous level's choice in place would land the
 * warp in a sub-area the target may not have. */
static void warp_start(CPUState* cpu, int target_id) {
    uint32_t target = (uint32_t)target_id;
    uint32_t cur    = cpu->read_word(S3_CURRENT_LEVEL);
    uint32_t pane   = target >= 50u ? 3u : target / 10u - 1u;
    int      i;

    cpu->write_word(S3_NOT_CREDITS, 0u);
    cpu->write_word(S3_SPEEDWAY_RUN, 0u);
    cpu->write_word(S3_LOAD_CAPTION, 0u);
    cpu->write_word(S3_GAME_STATE, S3_GS_LOADING_IMG);
    cpu->write_word(S3_LOAD_IMG_SUBSTATE, 0u);
    cpu->write_word(S3_LOAD_IMG_FRAMES, 0u);
    cpu->write_word(S3_LOAD_IMG_PANE, pane);
    cpu->write_word(S3_LOAD_STAGE, 0u);
    cpu->write_word(S3_PREV_LEVEL_INDEX, cpu->read_word(S3_LEVEL_INDEX));
    if (cur < 60u || cur >= 77u) cpu->write_word(S3_PREV_LEVEL, cur);
    cpu->write_word(S3_CURRENT_LEVEL, target);
    cpu->write_word(S3_AREA, 0u);
    for (i = 0; i < 3; i++) {
        cpu->write_byte(S3_FADE_A + (uint32_t)i, S3_FADE_LEVEL);
        cpu->write_byte(S3_FADE_B + (uint32_t)i, S3_FADE_LEVEL);
    }
    cpu->write_word(S3_MUSIC_ENABLED, 1u);

    s_phase  = WARP_LEAVING;
    s_target = target;
    s_ticks  = 0;
    OPL_INFO_F("levelwarp", "leaving level %u for %d (pane %u)",
               (unsigned)cur, target_id, (unsigned)pane);
}

/* The level count is only readable at run time, so the descriptor is filled
 * rather than initialised. */
static levelwarp_ui_desc_t s_ui;

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
    if (s_phase == WARP_IDLE) return;

    if (++s_ticks > WARP_STUCK_CALLS) {
        OPL_INFO_F("levelwarp", "%s", "aborted (stuck)");
        s_phase  = WARP_IDLE;
        s_target = 0;
        s_ticks  = 0;
        return;
    }

    /* The loader has the wheel and these phases write nothing more — they
     * only report status and keep a second warp from stacking. The level the
     * warp left is still live on the tick the transition is entered, so
     * arrival is only read once it has gone; then it waits for the next
     * level to come up, however long the CD load takes. */
    if (s_phase == WARP_LEAVING) {
        if (!spyro3_skip_gameplay_live(cpu)) s_phase = WARP_LOADING;
        return;
    }

    if (spyro3_skip_gameplay_live(cpu)) {
        uint32_t landed = cpu->read_word(S3_CURRENT_LEVEL);
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

const feature_module_t levelwarp_module = {
    .name           = "levelwarp",
    .apply_options  = levelwarp_apply_options,
    .dispatch_table = NULL,
};
