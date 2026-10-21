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
#include "openpete_log.h"
#include "psx_runtime_internal.h"   /* CPUState + its read/write callbacks */
#include "spyro-2/skip_to_level.h"

#define S2_LOADER_STATE     0x80066EECu /* 0 title, 2 loading, -1 level resident */
#define S2_NEXT_LEVEL_NUM   0x80066F90u /* the level number the loader loads */
#define S2_DEMO_MODE        0x80067024u /* 1 while an attract demo is running */
#define S2_DEMO_SPENT       0x80067178u /* set when the recording has run out */

#define S2_LOADER_TITLE     0u

/** @brief Starts the in-level warp by ending the level the way a spent attract-demo recording ends it.
 *
 * Writes the demo-end check's own state: the demo-mode word it is gated on
 * (0x80067024) and the flag its fade sets when the recording is spent
 * (0x80067178). spyro2_skip_set then hands the title screen the target.
 * @param w The warp; unused.
 * @param cpu The guest CPU state.
 * @param target_id The destination level number.
 * @return LEVELWARP_LEAVING. */
static int warp_start(levelwarp_t* w, CPUState* cpu, int target_id) {
    uint32_t cur = cpu->read_word(S2_NEXT_LEVEL_NUM);
    (void)w;
    cpu->write_word(S2_DEMO_MODE, 1u);
    cpu->write_word(S2_DEMO_SPENT, 1u);
    spyro2_skip_set(target_id);
    OPL_INFO_F("levelwarp", "leaving level %u for %d",
               (unsigned)cur, target_id);
    return LEVELWARP_LEAVING;
}

/* The title route opens on loader state 0; the in-level route while gameplay
 * is live and g_DemoMode is 0, since the attract demo runs a level with the
 * loader resident. The dropdown follows g_NextLevelNum. */
static const levelwarp_game_t k_warp_game = {
    .log_tag        = "levelwarp",
    .rows           = levelwarp_level_rows,
    .test_env       = "OPENPETE_LEVELWARP_TEST",
    .table          = &spyro2_levels,
    .unit           = "level",
    .title_word     = S2_LOADER_STATE,
    .title_value    = S2_LOADER_TITLE,
    .demo_word      = S2_DEMO_MODE,
    .live           = spyro2_skip_gameplay_live,
    .current_word   = S2_NEXT_LEVEL_NUM,
    .start          = warp_start,
    .leaving_status = "loading...",
};

static levelwarp_t s_warp = { .game = &k_warp_game };

/** @brief Feature-module option hook that registers the warp's overlay rows.
 *
 * @param opts Unused. */
static void levelwarp_apply_options(const openpete_options_t* opts) {
    (void)opts;
    levelwarp_register(&s_warp);
}

void spyro2_levelwarp_on_tick(CPUState* cpu) { levelwarp_on_tick(&s_warp, cpu); }

const feature_module_t spyro2_levelwarp_module = {
    .name           = "levelwarp",
    .apply_options  = levelwarp_apply_options,
    .dispatch_table = NULL,
};
