// games/spyro-1/features/levelwarp.c - jump to any level from the
// dev overlay (M key), from anywhere.
//
// --skip-to-level only works at boot: it hijacks the title screen's demo-
// timeout transition once and then latches. This module adds a level
// dropdown + Jump button to the overlay's Gameplay section that works
// mid-session, so switching levels for an A/B is not a relaunch.
//
// The dropdown, the Jump button and the click queue are the shared overlay
// half (games/common/features/levelwarp_ui.c); this file supplies the level
// table, the route probe and the two guest-write phases below.
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
//    transition at the next Press-Start window. That path resets progress
//    and lives, exactly as a fresh --skip-to-level boot does.
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

#include "features/levelwarp.h"

#include "feature_registry.h"
#include "features/levelwarp_ui.h"  /* the shared dropdown, button and queue */
#include "named_target.h"
#include "openpete_log.h"
#include "psx_runtime_internal.h"    /* CPUState + its read/write callbacks */

#include <stdio.h>

#define ADDR_G_GAMESTATE         0x800757D8u
#define ADDR_G_DEMOMODE          0x80075714u /* DEMO_MODE_NONE = 0 */
#define ADDR_G_LEVELID           0x8007596Cu
#define ADDR_G_NEXTLEVELID       0x800758B4u
#define ADDR_G_PORTALLEVELID     0x800758ACu
#define ADDR_G_HASLEVELTRANSITION 0x800756D0u
#define ADDR_D_8007568C          0x8007568Cu /* exit-sequence tick counter */
#define ADDR_D_800758B8          0x800758B8u /* exit-sequence step counter */

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

/* Backstop for a warp that never resolves. Both phases end on a gamestate
 * the game itself reaches, so this only fires if the game parks somewhere we
 * did not anticipate; it must not double as a deadline. Counted in hook
 * calls (one per sim yield), which is a looser unit than it looks - the
 * exit-level hand-off is ~90 calls, but a CD load plus fly-in ran past 3600
 * in a headless free-running session, which is exactly why neither phase
 * leans on this number to decide it is done. */
#define WARP_STUCK_CALLS 20000u

/* ---- the level table ------------------------------------------------- */

/* Ids follow the game's scheme: tens digit = homeworld, units = HOME(0),
 * levels(1-3), boss(4), flight(5). Names are g_LevelNames (decomp
 * strings.c) lower_snake_cased. */
static const spyro1_level_t k_levels[] = {
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
#define LEVEL_COUNT (sizeof(k_levels) / sizeof(k_levels[0]))

/* Alternate spellings accepted on the command line. Kept out of k_levels so
 * the dropdown lists each level once. */
typedef struct { const char* name; int id; } level_alias_t;
static const level_alias_t k_level_aliases[] = {
    { "gnastys_world",    60 },
    { "twilight_harbour", 62 },
};

/* Display names in table order - the dropdown's enum_values. Built once at
 * apply-options time; the registry copies the row but NOT the strings it
 * points at, so this array has to outlive the process (it is static). */
static const char* s_level_display[LEVEL_COUNT];

const spyro1_level_t* spyro1_level_table(size_t* out_count) {
    if (out_count) *out_count = LEVEL_COUNT;
    return k_levels;
}

void spyro1_level_surface(struct openpete_surface_sink* sink) {
    const named_target_table_t levels =
        NAMED_TARGET_TABLE(k_levels, spyro1_level_t, name, id);
    const named_target_table_t aliases =
        NAMED_TARGET_TABLE(k_level_aliases, level_alias_t, name, id);
    named_target_surface(sink, "level", "--skip-to-level", &levels);
    named_target_surface(sink, "level", "--skip-to-level", &aliases);
}

/* A numeric argument passes through unchecked (see levelwarp.h). */
int spyro1_level_resolve(const char* arg) {
    const named_target_table_t levels =
        NAMED_TARGET_TABLE(k_levels, spyro1_level_t, name, id);
    const named_target_table_t aliases =
        NAMED_TARGET_TABLE(k_level_aliases, level_alias_t, name, id);
    return named_target_resolve(arg, "level", &levels, &aliases,
                                NAMED_TARGET_NUMERIC_ANY);
}

/* ---- state ----------------------------------------------------------- */

typedef enum {
    WARP_IDLE = 0,
    WARP_ARMED,     /* exit-level sequence running; waiting for the hand-off */
    WARP_LOADING    /* destination retargeted; waiting for arrival */
} warp_phase_t;

/* Timeline state - travels with every savestate (see *_state_save/load).
 * A savestate load during an in-flight warp restores the guest RAM the warp
 * had written, so the phase driving those writes has to be restored with it;
 * otherwise a quick-load back to before the warp leaves an armed FSM that
 * hijacks whatever level transition the player triggers next.
 *
 * Rewind and runahead need no separate handling. Both are hard-gated on
 * game_is_gameplay_live() (g_Gamestate == GS_Playing && a level loaded), and
 * arming a warp leaves GS_Playing on its first tick - so the whole in-flight
 * window is outside the window they capture or restore in. */
static uint32_t s_phase  = WARP_IDLE;
static uint32_t s_target = 0;
static uint32_t s_ticks  = 0;

void spyro1_levelwarp_state_save(uint32_t* phase, uint32_t* target,
                                 uint32_t* ticks) {
    *phase  = s_phase;
    *target = s_target;
    *ticks  = s_ticks;
}

void spyro1_levelwarp_state_load(uint32_t phase, uint32_t target,
                                 uint32_t ticks) {
    s_phase  = phase;
    s_target = target;
    s_ticks  = ticks;
}

/* ---- the shared overlay half ------------------------------------------ */

/** @brief Returns the level id at a level-table index, for the shared overlay half.
 *
 * @param index An index into k_levels.
 * @return The level id. */
static int warp_level_id(int index) { return k_levels[index].id; }

/** @brief Reports whether a warp is armed or loading.
 *
 * @return Nonzero when the warp phase is not idle. */
static int warp_in_flight(void) { return s_phase != WARP_IDLE; }

/** @brief Writes the overlay status text for a running warp phase.
 *
 * Writes "exiting level..." while armed and "loading..." while loading;
 * writes nothing when idle.
 * @param buf Destination for the text.
 * @param cap Size of buf in bytes.
 * @return 1 when text was written, 0 when the warp is idle. */
static int warp_in_flight_status(char* buf, size_t cap) {
    if (s_phase == WARP_ARMED)   { snprintf(buf, cap, "exiting level..."); return 1; }
    if (s_phase == WARP_LOADING) { snprintf(buf, cap, "loading...");       return 1; }
    return 0;
}

/** @brief Returns the route a warp click would take from the current game state.
 *
 * Derived from guest state only; whether a warp is already queued or
 * running is asked at the button. Pause and inventory count as in a level:
 * they are gameplay with the sim paused, and the exit sequence takes over
 * g_Gamestate. The attract demo runs GS_Playing with a level loaded, so a
 * nonzero g_DemoMode excludes it.
 * @param cpu The guest CPU state.
 * @return LEVELWARP_ROUTE_TITLE at GS_TitleScreen, LEVELWARP_ROUTE_LEVEL in
 *     play, pause or inventory with a level loaded and no demo, else
 *     LEVELWARP_ROUTE_NONE. */
static int warp_route(CPUState* cpu) {
    uint32_t gs = cpu->read_word(ADDR_G_GAMESTATE);
    if (gs == GS_TITLESCREEN) return LEVELWARP_ROUTE_TITLE;
    if ((gs == GS_PLAYING || gs == GS_PAUSEMENU || gs == GS_INVENTORYMENU) &&
        cpu->read_word(ADDR_G_LEVELID) != 0u &&
        cpu->read_word(ADDR_G_DEMOMODE) == 0u)
        return LEVELWARP_ROUTE_LEVEL;
    return LEVELWARP_ROUTE_NONE;
}

/** @brief Reads the id of the level the guest currently has loaded.
 *
 * @param cpu The guest CPU state.
 * @return g_LevelId as an int. */
static int warp_current_level(CPUState* cpu) {
    return (int)cpu->read_word(ADDR_G_LEVELID);
}

static void warp_arm(CPUState* cpu, int target_id);

static const levelwarp_ui_desc_t k_ui = {
    .log_tag           = "levelwarp",
    .display           = s_level_display,
    .count             = (int)LEVEL_COUNT,
    .level_id          = warp_level_id,
    .resolve           = spyro1_level_resolve,
    .route             = warp_route,
    .current_level     = warp_current_level,
    .start_level_route = warp_arm,
    .in_flight         = warp_in_flight,
    .in_flight_status  = warp_in_flight_status,
};

/** @brief Resets the warp state and registers the level dropdown with the overlay.
 *
 * Fills the display-name array in table order and hands the overlay
 * descriptor to the shared warp UI.
 * @param opts Unused. */
static void levelwarp_apply_options(const openpete_options_t* opts) {
    size_t i;
    (void)opts;
    s_phase  = WARP_IDLE;
    s_target = 0;
    s_ticks  = 0;
    for (i = 0; i < LEVEL_COUNT; i++) s_level_display[i] = k_levels[i].display;
    levelwarp_ui_register(&k_ui);
}

/* ---- the warp itself -------------------------------------------------- */

/** @brief Puts the game into its exit-level sequence and arms the warp toward a level.
 *
 * Makes the practice ROM's inventory level-select stores minus its
 * collectable wipe, so progress is kept: g_Gamestate GS_ExitLevel, Spyro's
 * m_State 0 and m_AirTime 1, and both exit-sequence counters 0. The practice
 * ROM zeroes only D_800758B8; func_8002E084 sets Spyro's gliding pose at
 * D_8007568C == 16 and only while D_800758B8 == 0, so a mid-cycle
 * D_8007568C skips the glide and Spyro exits standing. Zeroing both makes
 * the animation the same every time.
 * @param cpu The guest CPU state.
 * @param target_id The destination level id. */
static void warp_arm(CPUState* cpu, int target_id) {
    cpu->write_word(ADDR_G_GAMESTATE, GS_EXITLEVEL);
    cpu->write_word(ADDR_G_SPYRO + SPYRO_M_STATE, 0u);
    cpu->write_word(ADDR_G_SPYRO + SPYRO_M_AIRTIME, 1u); /* Tree Tops */
    cpu->write_word(ADDR_D_8007568C, 0u);
    cpu->write_word(ADDR_D_800758B8, 0u);

    s_phase  = WARP_ARMED;
    s_target = (uint32_t)target_id;
    s_ticks  = 0;
    OPL_INFO_F("levelwarp", "exiting level %u for %d",
               (unsigned)cpu->read_word(ADDR_G_LEVELID), target_id);
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
 * stale non-NULL one). Moves the phase to loading.
 * @param cpu The guest CPU state. */
static void warp_retarget(CPUState* cpu) {
    uint32_t target = s_target;

    cpu->write_word(ADDR_G_NEXTLEVELID, target);
    cpu->write_word(ADDR_G_PORTALLEVELID, 0u);
    cpu->write_word(ADDR_G_HASLEVELTRANSITION, (target % 10u) ? 1u : 0u);
    cpu->write_word(ADDR_G_SPYRO + SPYRO_M_FLYINGABILITY, 0u);
    cpu->write_word(ADDR_G_CAMERA + CAMERA_M_SPHERICALPRESET, 0u);

    s_phase = WARP_LOADING;
    s_ticks = 0;
    OPL_INFO_F("levelwarp", "retargeted transition to level %u",
               (unsigned)target);
}

/** @brief Cancels the warp in progress and returns the phase to idle.
 *
 * Logs the reason and clears the phase, target and tick count; no guest
 * word is written.
 * @param why Short reason text for the log line. */
static void warp_abort(const char* why) {
    OPL_INFO_F("levelwarp", "aborted (%s)", why);
    s_phase  = WARP_IDLE;
    s_target = 0;
    s_ticks  = 0;
}

void spyro1_levelwarp_on_tick(CPUState* cpu) {
    uint32_t gs;

    /* The shared half refreshes the route, follows the dropdown and drains a
     * queued click into warp_arm below or into the title route. It reports
     * back whether this tick may advance the sequence. (The FSM phase itself
     * IS snapshotted, so the rollback of an in-flight warp stays
     * consistent.) */
    if (levelwarp_ui_tick(cpu)) return;

    if (s_phase == WARP_IDLE) return;

    gs = cpu->read_word(ADDR_G_GAMESTATE);

    if (++s_ticks > WARP_STUCK_CALLS) { warp_abort("stuck"); return; }

    if (s_phase == WARP_ARMED) {
        /* func_8002C664 flips the gamestate as its last act, so this edge
         * means the hand-off has run and its clobber of g_NextLevelId /
         * g_PortalLevelId is behind us. LoadLevel does not read them until
         * gamestate 1 dispatches later in THIS tick, so writing now lands. */
        if (gs == GS_LEVELTRANSITION) { warp_retarget(cpu); return; }
        /* Anything other than the sequence we started means it is no longer
         * ours to steer - a savestate load restoring gameplay, a death, a
         * cutscene. Bail rather than retarget someone else's transition. */
        if (gs != GS_EXITLEVEL) warp_abort("left the exit sequence");
        return;
    }

    /* WARP_LOADING: the loader has the wheel and we write nothing more -
     * this phase only reports status and keeps a second warp from stacking.
     * It ends when the game hands back to gameplay, however long the CD load
     * and fly-in take (a fly-in can outlast any tick budget worth setting,
     * so the exit condition is the gamestate, not a clock). */
    if (gs == GS_PLAYING) {
        uint32_t landed = cpu->read_word(ADDR_G_LEVELID);
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
