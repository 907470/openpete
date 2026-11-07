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
//    exactly as a fresh --skip-to-level boot does. The menus Press Start
//    opens run inside the title (loader state 0, mode word 0x80067E04 1 or
//    2), and the press-start window is mode 0, so from a menu the warp
//    first makes the menu's own Triangle back-out: the body of
//    func_titlescreen_and_loading_80077190 (stores 0x80067E04 = 0,
//    0x80067E0C = 4, 0x80067E00 = 0, then the card driver's synchronous
//    wait func_titlescreen_and_loading_8007DC3C(0, 0x80067E68, 0x80067E6C)
//    and its stop func_titlescreen_and_loading_8007CCD8), from exactly the
//    menu states whose handlers call it. The stop matters: the card driver
//    hooks VSync callback slot 7 into title-overlay code that a level load
//    overwrites. The back-out is a guest call, so it runs from a hook on
//    the title update's entry (func_titlescreen_and_loading_80079DF0), a
//    call boundary inside the game's tick.
//
// 3. In the attract demo: the stores of route 1 are the demo's own end, so
//    the demo returns to the title and route 2 takes the target.
//
// The overlay runs outside the sim, so a click only queues a request; every
// guest write happens in warp_start below. Intentional, opt-in
// gameplay divergence like the debug freecam: unclicked, this module writes
// zero guest words, so a default run stays oracle-clean.
//
// Every write lands in one tick, so an in-flight warp holds no guest state
// of its own: the remaining phases (games/common/features/levelwarp_ui.c)
// only watch for the level to go and the next one to come up, and a
// savestate load across them strands nothing.
//
// Addresses from spyro-2/psx.map and the asm cited above.

#include "levelwarp.h"

#include "feature_registry.h"
#include "features/levelwarp_ui.h"
#include "openpete_fiber.h"     /* openpete_speculative */
#include "openpete_log.h"
#include "features/guest_call.h"  /* game_guest_call: a host-initiated guest call */
#include "psx_runtime_internal.h"   /* CPUState + its read/write callbacks */
#include "spyro-2/skip_to_level.h"
#include "openpete_thread_roles.h"

#define S2_GAME_STATE       0x800681C8u /* D_800681C8: 0 gameplay, 4 pause menu,
                                           11 title (func_8001AAA4) */
#define S2_NEXT_LEVEL_NUM   0x80066F90u /* the level number the loader loads */
#define S2_DEMO_MODE        0x80067024u /* 1 while an attract demo is running */
#define S2_DEMO_SPENT       0x80067178u /* set when the recording has run out */
#define S2_TITLE_MODE       0x80067E04u /* 0 title, 1 and 2 the Press Start menus */
#define S2_TITLE_SUBSTATE   0x80067E0Cu
#define S2_TITLE_TIMER      0x80067E00u
#define S2_CARD_SYNC        0x8007DC3Cu /* func_titlescreen_and_loading_8007DC3C */
#define S2_CARD_STOP        0x8007CCD8u /* func_titlescreen_and_loading_8007CCD8 */
#define S2_CARD_SYNC_CMD    0x80067E68u
#define S2_CARD_SYNC_RESULT 0x80067E6Cu
#define S2_TITLE_UPDATE     0x80079DF0u /* func_titlescreen_and_loading_80079DF0 */
#define S2_OVERLAY_TITLE    34u         /* titlescreen_and_loading */

#define S2_GS_TITLE         11u
#define S2_GS_PAUSE         4u
#define S2_SUBSTATE_BACKOUT 4u


/* A title-route warp armed from a menu, waiting for the title update. */
static int s_backout_armed = 0;

/** @brief Makes the stores of the game's demo end: the demo-mode word and the recording-spent flag.
 *
 * The per-tick check at 0x80015ACC (func_800159C8) is gated on the
 * demo-mode word 0x80067024 and fires on the flag 0x80067178 its fade sets
 * when the recording is spent; from there the game returns to the title.
 * @param cpu The guest CPU state. */
static void end_as_demo(CPUState* cpu) {
    cpu->write_word(S2_DEMO_MODE, 1u);
    cpu->write_word(S2_DEMO_SPENT, 1u);
}

/** @brief Starts the in-level warp by ending the level the way a spent attract-demo recording ends it.
 *
 * Makes the demo end's stores (end_as_demo); spyro2_skip_set then hands
 * the title screen the target.
 * @param w The warp; unused.
 * @param cpu The guest CPU state.
 * @param target_id The destination level number.
 * @return LEVELWARP_LEAVING. */
static int warp_start(levelwarp_t* w, CPUState* cpu, int target_id) {
    uint32_t cur = cpu->read_word(S2_NEXT_LEVEL_NUM);
    (void)w;
    end_as_demo(cpu);
    spyro2_skip_set(target_id);
    OPL_INFO_F("levelwarp", "leaving level %u for %d",
               (unsigned)cur, target_id);
    return LEVELWARP_LEAVING;
}

/** @brief Arms the title route, and the menu back-out the title update's hook makes before it.
 *
 * @param target The destination level number. */
static void warp_title_start(int target) {
    s_backout_armed = 1;
    spyro2_skip_set(target);
}

/** @brief Reports whether a menu state's handler calls the Triangle back-out func_titlescreen_and_loading_80077190.
 *
 * Mode 1 sub-states 2, 3, 5, 6, 8, 9, 11 and 12 and mode 2 sub-state 1
 * call it (func_titlescreen_and_loading_80079DF0's two jump tables); the
 * others run a card access of their own and make no back-out.
 * @param mode The mode word 0x80067E04.
 * @param sub The sub-state word 0x80067E0C.
 * @return 1 when the state backs out, else 0. */
static int menu_backs_out(uint32_t mode, uint32_t sub) {
    if (mode == 2u) return sub == 1u;
    return mode == 1u && sub < 13u && ((0x1B6Cu >> sub) & 1u);
}

/** @brief Title-update entry hook that makes the menu's Triangle back-out for an armed title-route warp.
 *
 * Inert unless a title-route warp armed it, and on a speculative tick. The
 * arm lasts while the level skip holds a target, so a press of Start after
 * the click still meets it. In a menu state that backs out, the hook makes
 * the back-out's body (func_titlescreen_and_loading_80077190 past its
 * Triangle test) and clears the arm; at mode 0 and in any other menu state
 * it waits, since a card access there is the game's to finish. The update
 * then runs mode 0 sub-state 4, the return to the press-start window.
 * @param cpu The guest CPU state at the title update's entry.
 * @param addr Unused.
 * @return 0, so the update runs. */
static int on_title_update(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    (void)addr;
    if (!s_backout_armed || openpete_speculative) return 0;
    if (spyro2_skip_target() == 0) { s_backout_armed = 0; return 0; }
    const uint32_t mode = cpu->read_word(S2_TITLE_MODE);
    const uint32_t sub  = cpu->read_word(S2_TITLE_SUBSTATE);
    if (!menu_backs_out(mode, sub)) return 0;
    s_backout_armed = 0;
    cpu->write_word(S2_TITLE_MODE, 0u);
    cpu->write_word(S2_TITLE_SUBSTATE, S2_SUBSTATE_BACKOUT);
    cpu->write_word(S2_TITLE_TIMER, 0u);
    game_guest_call(cpu, S2_CARD_SYNC, 3, 0u, S2_CARD_SYNC_CMD, S2_CARD_SYNC_RESULT);
    game_guest_call(cpu, S2_CARD_STOP, 3, 0u, 0u, 0u);
    OPL_INFO_F("levelwarp", "backed out of menu mode %u sub-state %u to the "
               "title", (unsigned)mode, (unsigned)sub);
    return 0;
}

/* The title route opens on D_800681C8 11, the state func_8001AAA4 enters
 * at the title; the loader word 0x80066EEC also reads 0 on a level-exit
 * loading screen (D_800681C8 7) and at a story cutscene's start (5), where
 * an armed level skip would never meet the title. The in-level route opens
 * while gameplay is live, D_800681C8 is 0 and g_DemoMode is 0: the
 * demo-end check func_800159C8 runs only from state 0 of func_8001B140's
 * dispatch, while func_8001271C copies demo rows into the pad words in
 * every state, so the route's stores from the pause menu (4) or a level's
 * fly-in (3) would hold the menu on a dead pad; and the attract demo runs
 * a level with the loader resident. The dropdown follows g_NextLevelNum. */
static const levelwarp_game_t k_warp_game = {
    .log_tag        = "levelwarp",
    .rows           = levelwarp_level_rows,
    .test_env       = "OPENPETE_LEVELWARP_TEST",
    .table          = &spyro2_levels,
    .unit           = "level",
    .title_word     = S2_GAME_STATE,
    .title_value    = S2_GS_TITLE,
    .demo_word      = S2_DEMO_MODE,
    .state_word     = S2_GAME_STATE,
    .pause_value    = S2_GS_PAUSE,
    .live           = spyro2_skip_gameplay_live,
    .current_word   = S2_NEXT_LEVEL_NUM,
    .title_start    = warp_title_start,
    .demo_end       = end_as_demo,
    .start          = warp_start,
};

static levelwarp_t s_warp = { .game = &k_warp_game };

/** @brief Feature-module option hook that registers the warp's overlay rows.
 *
 * @param opts Unused. */
static void levelwarp_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    levelwarp_register(&s_warp);
}

void spyro2_levelwarp_on_tick(CPUState* cpu) { levelwarp_on_tick(&s_warp, cpu); }

static const feature_dispatch_entry_t k_dispatch[] = {
    { S2_TITLE_UPDATE, on_title_update, S2_OVERLAY_TITLE },
    { 0, NULL, 0u },
};

const feature_module_t spyro2_levelwarp_module = {
    .name           = "levelwarp",
    .apply_options  = levelwarp_apply_options,
    .dispatch_table = k_dispatch,
};
