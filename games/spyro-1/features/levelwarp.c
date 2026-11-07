#include "openpete_thread_roles.h"
// games/spyro-1/features/levelwarp.c - jump to any level from the
// dev overlay (M key), from anywhere.
//
// --skip-to-level only works at boot: it hijacks the title screen's demo-
// timeout transition once and then latches. This module adds a level
// dropdown + Warp button to the overlay's Gameplay section that works
// mid-session, so switching levels for an A/B is not a relaunch.
//
// The dropdown, the Warp button, the click queue and the in-flight phases
// are the shared warp (games/common/features/levelwarp_ui.c); this file
// supplies the level table, the route words and the two guest-write steps
// below (the arm, and the retarget in the leaving phase).
//
// TWO ROUTES, picked from the live gamestate:
//
// 1. IN A LEVEL - ride the game's own "exit level" sequence, then retarget
//    the load it kicks off. No guest function calls; every step is a plain
//    write into a global the game already drives:
//
//      arm:   g_Gamestate = GS_ExitLevel, Spyro's state/air-time reset, and
//             the two exit-sequence step counters (D_8007568C/D_800758B8)
//             zeroed so the animation runs from its start.
//      fire:  the exit sequence ends in func_8002C664 (gamestates/init.c),
//             which flips g_Gamestate to GS_LevelTransition and points the
//             load at "my homeworld, exiting from my portal". We observe
//             that edge and overwrite the destination before LoadLevel
//             reads it on the next tick.
//
//    The edge is the correct hook point, NOT Spyro's walking-state == 8
//    ("loading") substate: func_8002C664 CLOBBERS g_NextLevelId and
//    g_PortalLevelId unconditionally, and walking-state 8 can be reached
//    ~18 ticks into the sequence - well before the hand-off ~64 ticks in -
//    so writing the destination there is a race against the clobber.
//    Retargeting on the gamestate edge happens strictly after it, and the
//    pre-tick hook runs before gamestate 1 dispatches into LoadLevel.
//
// 2. AT THE TITLE SCREEN - hand off to the existing --skip-to-level
//    machinery (game_set_skip_to_level), which induces the demo-timeout
//    transition at the next Press-Start window, or from the menus Press
//    Start opens makes the save-file picker's own commit. That path resets
//    progress and lives, exactly as a fresh --skip-to-level boot does.
//
// 3. IN THE ATTRACT DEMO - start the demo's own end (g_DemoFadeTimer 1, the
//    store a pad press makes); the demo fades back to the title screen,
//    where route 2 takes the target.
//
// The overlay runs OUTSIDE the sim, so a click only queues a request; every
// guest write happens in the per-tick hook below. Intentional, opt-in
// gameplay divergence like --cheats and the debug freecam: unclicked, this
// module writes zero guest words, so a default run stays oracle-clean.
//
// Mechanism derived from C0mposer's Spyro-1-Practice-Rom (src/levelselect.c),
// the same source the debug freecam was ported from - its level select is
// the battle-tested version of route 1. Two deliberate departures:
//   - It triggers only from the inventory menu, because that is where its
//     hotkeys live; nothing in the writes needs the inventory, so we arm
//     straight from gameplay.
//   - It carries a 36-entry table of per-level camera fly-in presets. We
//     write NULL instead: func_80037A20 (spyro-1/src/camera.c) picks a
//     preset itself from Spyro's facing WHEN THE FIELD IS NULL, and the
//     only reason the table is needed is that GS_ExitLevel leaves a stale
//     non-NULL value that suppresses the auto-pick.
//
// Addresses from spyro-1/build/psx.map; g_Spyro/g_Camera field offsets
// verified against spyro-1/include/{spyro,camera}.h.

#include "levelwarp.h"

#include "feature_registry.h"
#include "features/levelwarp_ui.h"  /* the shared dropdown, button and queue */
#include "named_target.h"
#include "openpete_log.h"
#include "psx_runtime_internal.h"    /* CPUState + its read/write callbacks */

#define ADDR_G_GAMESTATE         0x800757D8u
#define ADDR_G_DEMOMODE          0x80075714u /* DEMO_MODE_NONE = 0 */
#define ADDR_G_LEVELID           0x8007596Cu
#define ADDR_G_NEXTLEVELID       0x800758B4u
#define ADDR_G_PORTALLEVELID     0x800758ACu
#define ADDR_G_HASLEVELTRANSITION 0x800756D0u
#define ADDR_D_8007568C          0x8007568Cu /* exit-sequence tick counter */
#define ADDR_D_800758B8          0x800758B8u /* exit-sequence step counter */
#define ADDR_G_DEMOFADETIMER     0x80075884u /* nonzero once the demo is ending */

#define ADDR_G_SPYRO             0x80078A58u
#define SPYRO_M_STATE            0x078u
#define SPYRO_M_AIRTIME          0x09Cu
#define SPYRO_M_FLYINGABILITY    0x24Cu

#define ADDR_G_CAMERA            0x80076DD0u
#define CAMERA_M_SPHERICALPRESET 0x0D8u

#define GS_PLAYING          0u
#define GS_LEVELTRANSITION  1u
#define GS_PAUSEMENU        2u
#define GS_INVENTORYMENU    3u
#define GS_EXITLEVEL       10u
#define GS_TITLESCREEN     13u

/* ---- the level table ------------------------------------------------- */

/* Ids follow the game's scheme: tens digit = homeworld, units = HOME(0),
 * levels(1-3), boss(4), flight(5). Names are g_LevelNames (decomp
 * strings.c) lower_snake_cased. */
static const game_level_t k_levels[] = {
    { "artisans",        "Artisans",         10 },
    { "stone_hill",      "Stone Hill",       11 },
    { "dark_hollow",     "Dark Hollow",      12 },
    { "town_square",     "Town Square",      13 },
    { "toasty",          "Toasty",           14 },
    { "sunny_flight",    "Sunny Flight",     15 },
    { "peace_keepers",   "Peace Keepers",    20 },
    { "dry_canyon",      "Dry Canyon",       21 },
    { "cliff_town",      "Cliff Town",       22 },
    { "ice_cavern",      "Ice Cavern",       23 },
    { "doctor_shemp",    "Doctor Shemp",     24 },
    { "night_flight",    "Night Flight",     25 },
    { "magic_crafters",  "Magic Crafters",   30 },
    { "alpine_ridge",    "Alpine Ridge",     31 },
    { "high_caves",      "High Caves",       32 },
    { "wizard_peak",     "Wizard Peak",      33 },
    { "blowhard",        "Blowhard",         34 },
    { "crystal_flight",  "Crystal Flight",   35 },
    { "beast_makers",    "Beast Makers",     40 },
    { "terrace_village", "Terrace Village",  41 },
    { "misty_bog",       "Misty Bog",        42 },
    { "tree_tops",       "Tree Tops",        43 },
    { "metalhead",       "Metalhead",        44 },
    { "wild_flight",     "Wild Flight",      45 },
    { "dream_weavers",   "Dream Weavers",    50 },
    { "dark_passage",    "Dark Passage",     51 },
    { "lofty_castle",    "Lofty Castle",     52 },
    { "haunted_towers",  "Haunted Towers",   53 },
    { "jacques",         "Jacques",          54 },
    { "icy_flight",      "Icy Flight",       55 },
    { "gnorc_gnexus",    "Gnorc Gnexus",     60 },
    { "gnorc_cove",      "Gnorc Cove",       61 },
    { "twilight_harbor", "Twilight Harbor",  62 },
    { "gnasty_gnorc",    "Gnasty Gnorc",     63 },
    { "gnastys_loot",    "Gnasty's Loot",    64 },
};

/* Alternate spellings accepted on the command line. Kept out of k_levels so
 * the dropdown lists each level once. */
static const game_level_t k_level_aliases[] = {
    { "gnastys_world",    NULL, 60 },
    { "twilight_harbour", NULL, 62 },
};

/* Names are matched case-insensitive with '-' for '_' and apostrophes dropped.
 * An all-digit id passes through unchecked: the warp takes the id the
 * caller names rather than indexing the table with it. */
const game_level_table_t spyro1_levels = {
    GAME_LEVEL_ROWS(k_levels), GAME_LEVEL_ROWS(k_level_aliases), "level",
    "--skip-to-level", NAMED_TARGET_NUMERIC_ANY,
};

int spyro1_level_resolve(const char* arg) {
    return game_level_resolve(&spyro1_levels, arg);
}

/* ---- the warp itself -------------------------------------------------- */

/* Timeline state - travels with every savestate (see *_state_save/load).
 * A savestate load during an in-flight warp restores the guest RAM the warp
 * had written, so the phase driving those writes has to be restored with it;
 * otherwise a quick-load back to before the warp leaves an armed warp that
 * hijacks whatever level transition the player triggers next.
 *
 * Rewind and runahead need no separate handling. Both are hard-gated on
 * game_is_gameplay_live() (g_Gamestate == GS_Playing && a level loaded), and
 * arming a warp leaves GS_Playing on its first tick - so the whole in-flight
 * window is outside the window they capture or restore in. */
static levelwarp_t s_warp;

void spyro1_levelwarp_state_save(uint32_t* phase, uint32_t* target,
                                 uint32_t* ticks) {
    *phase  = s_warp.phase;
    *target = s_warp.target;
    *ticks  = s_warp.ticks;
}

void spyro1_levelwarp_state_load(uint32_t phase, uint32_t target,
                                 uint32_t ticks) {
    s_warp.phase  = phase;
    s_warp.target = target;
    s_warp.ticks  = ticks;
}

/** @brief Reports whether the in-level route is open, before the demo and free-camera tests.
 *
 * Pause and inventory count as in a level: they are gameplay with the sim
 * paused, and the exit sequence takes over g_Gamestate. The attract demo
 * runs GS_Playing with a level loaded; the shared route excludes it on
 * g_DemoMode.
 * @param cpu The guest CPU state.
 * @return 1 in play, pause or inventory with a level loaded, else 0. */
static int warp_live(CPUState* cpu) {
    uint32_t gs = cpu->read_word(ADDR_G_GAMESTATE);
    return (gs == GS_PLAYING || gs == GS_PAUSEMENU || gs == GS_INVENTORYMENU) &&
           cpu->read_word(ADDR_G_LEVELID) != 0u;
}

/** @brief Reports the warp's arrival: g_Gamestate back at GS_Playing.
 *
 * @param cpu The guest CPU state.
 * @return 1 at GS_Playing, else 0. */
static int warp_arrived(CPUState* cpu) {
    return cpu->read_word(ADDR_G_GAMESTATE) == GS_PLAYING;
}

/** @brief Puts the game into its exit-level sequence toward a level.
 *
 * Makes the practice ROM's inventory level-select stores minus its
 * collectable wipe, so progress is kept: g_Gamestate GS_ExitLevel, Spyro's
 * m_State 0 and m_AirTime 1, and both exit-sequence counters 0. The practice
 * ROM zeroes only D_800758B8; func_8002E084 sets Spyro's gliding pose at
 * D_8007568C == 16 and only while D_800758B8 == 0, so a mid-cycle
 * D_8007568C skips the glide and Spyro exits standing. Zeroing both makes
 * the animation the same every time.
 * @param w The warp; unused.
 * @param cpu The guest CPU state.
 * @param target_id The destination level id.
 * @return LEVELWARP_LEAVING. */
static int warp_arm(levelwarp_t* w, CPUState* cpu, int target_id) {
    (void)w;
    cpu->write_word(ADDR_G_GAMESTATE, GS_EXITLEVEL);
    cpu->write_word(ADDR_G_SPYRO + SPYRO_M_STATE, 0u);
    cpu->write_word(ADDR_G_SPYRO + SPYRO_M_AIRTIME, 1u); /* Tree Tops */
    cpu->write_word(ADDR_D_8007568C, 0u);
    cpu->write_word(ADDR_D_800758B8, 0u);
    OPL_INFO_F("levelwarp", "exiting level %u for %d",
               (unsigned)cpu->read_word(ADDR_G_LEVELID), target_id);
    return LEVELWARP_LEAVING;
}

/** @brief Retargets the load func_8002C664 kicked off to the armed level, entered as a portal entry is.
 *
 * func_8002C664 points the transition at the current homeworld, arriving
 * out of the current level's portal. Stores g_NextLevelId = target,
 * g_PortalLevelId 0, g_HasLevelTransition 1 for a level and 0 for a
 * homeworld (id divisible by 10: homeworlds fade in rather than fly in),
 * Spyro's m_FlyingAbility 0, and g_Camera.m_SphericalPreset NULL, which
 * restores the transition camera's own preset pick (func_80037A20
 * auto-picks only when the field is NULL, and the exit sequence leaves a
 * stale non-NULL one).
 * @param cpu The guest CPU state.
 * @param target The destination level id. */
static void warp_retarget(CPUState* cpu, uint32_t target) {
    cpu->write_word(ADDR_G_NEXTLEVELID, target);
    cpu->write_word(ADDR_G_PORTALLEVELID, 0u);
    cpu->write_word(ADDR_G_HASLEVELTRANSITION, (target % 10u) ? 1u : 0u);
    cpu->write_word(ADDR_G_SPYRO + SPYRO_M_FLYINGABILITY, 0u);
    cpu->write_word(ADDR_G_CAMERA + CAMERA_M_SPHERICALPRESET, 0u);
    OPL_INFO_F("levelwarp", "retargeted transition to level %u",
               (unsigned)target);
}

/** @brief Steps the exit sequence: retarget on its hand-off edge, abort when the game leaves it.
 *
 * func_8002C664 flips the gamestate to GS_LevelTransition as its last act,
 * so that edge means the hand-off has run and its clobber of g_NextLevelId
 * and g_PortalLevelId is behind; LoadLevel does not read them until
 * gamestate 1 dispatches later in this tick, so the retarget lands. Any
 * state other than GS_ExitLevel means the sequence is no longer this warp's
 * to steer (a savestate load restoring gameplay, a death, a cutscene).
 * @param w The warp.
 * @param cpu The guest CPU state.
 * @return LEVELWARP_LOADING after the retarget, LEVELWARP_IDLE after an
 *         abort, else LEVELWARP_LEAVING. */
static int warp_leaving(levelwarp_t* w, CPUState* cpu) {
    uint32_t gs = cpu->read_word(ADDR_G_GAMESTATE);
    if (gs == GS_LEVELTRANSITION) { warp_retarget(cpu, w->target); return LEVELWARP_LOADING; }
    if (gs != GS_EXITLEVEL) { levelwarp_abort(w, "left the exit sequence"); return LEVELWARP_IDLE; }
    return LEVELWARP_LEAVING;
}

/** @brief Ends the attract demo the way a pad press ends it: g_DemoFadeTimer 1.
 *
 * func_800334D4 (gamestates/update.c) sets g_DemoFadeTimer to 1 when the
 * recording runs out or a button is pressed, then fades for 16 ticks and
 * loads the title screen back in.
 * @param cpu The guest CPU state. */
static void warp_end_demo(CPUState* cpu) {
    cpu->write_word(ADDR_G_DEMOFADETIMER, 1u);
}

/* The title route opens at GS_TitleScreen. The loading phase ends when the
 * game hands back to gameplay, however long the CD load and fly-in take. */
static const levelwarp_game_t k_warp_game = {
    .log_tag                = "levelwarp",
    .rows                   = levelwarp_level_rows,
    .test_env               = "OPENPETE_LEVELWARP_TEST",
    .table                  = &spyro1_levels,
    .unit                   = "level",
    .title_word             = ADDR_G_GAMESTATE,
    .title_value            = GS_TITLESCREEN,
    .demo_word              = ADDR_G_DEMOMODE,
    .live                   = warp_live,
    .demo_end               = warp_end_demo,
    .current_word           = ADDR_G_LEVELID,
    .start                  = warp_arm,
    .leaving                = warp_leaving,
    .arrived                = warp_arrived,
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

void spyro1_levelwarp_on_tick(CPUState* cpu) { levelwarp_on_tick(&s_warp, cpu); }

const feature_module_t spyro1_levelwarp_module = {
    .name           = "levelwarp",
    .apply_options  = levelwarp_apply_options,
    .dispatch_table = NULL,
};
