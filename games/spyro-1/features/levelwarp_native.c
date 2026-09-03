// games/spyro-1/features/levelwarp_native.c — jump to any level from the
// dev overlay (M key), from anywhere.
//
// --skip-to-level only works at boot: it hijacks the title screen's demo-
// timeout transition once and then latches. This module adds a level
// dropdown + Jump button to the overlay's Gameplay section that works
// mid-session, so switching levels for an A/B is not a relaunch.
//
// TWO ROUTES, picked from the live gamestate:
//
// 1. IN A LEVEL — ride the game's own "exit level" sequence, then retarget
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
//    ~18 ticks into the sequence — well before the hand-off ~64 ticks in —
//    so writing the destination there is a race against the clobber.
//    Retargeting on the gamestate edge happens strictly after it, and the
//    pre-tick hook runs before gamestate 1 dispatches into LoadLevel.
//
// 2. AT THE TITLE SCREEN — hand off to the existing --skip-to-level
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
// the same source the debug freecam was ported from — its level select is
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

#include "features/levelwarp_native.h"

#include "feature_registry.h"
#include "game_extras.h"    /* game_set_skip_to_level (title-screen route) */
#include "openpete_fiber.h" /* openpete_speculative */
#include "openpete_freecam.h"
#include "openpete_knobs.h" /* dev-overlay rows (M key) */
#include "openpete_log.h"
#include "psx_runtime_internal.h"    /* CPUState + its read/write callbacks */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * calls (one per sim yield), which is a looser unit than it looks — the
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
static const struct { const char* name; int id; } k_level_aliases[] = {
    { "gnastys_world",    60 },
    { "twilight_harbour", 62 },
};
#define LEVEL_ALIAS_COUNT (sizeof(k_level_aliases) / sizeof(k_level_aliases[0]))

/* Display names in table order — the dropdown's enum_values. Built once at
 * apply-options time; the registry copies the row but NOT the strings it
 * points at, so this array has to outlive the process (it is static). */
static const char* s_level_display[LEVEL_COUNT];

const spyro1_level_t* spyro1_level_table(size_t* out_count) {
    if (out_count) *out_count = LEVEL_COUNT;
    return k_levels;
}

static void print_valid_names(void) {
    size_t i;
    fprintf(stderr, "valid level names:\n");
    for (i = 0; i < LEVEL_COUNT; i++)
        fprintf(stderr, "  %-18s (%d)\n", k_levels[i].name, k_levels[i].id);
}

int spyro1_level_resolve(const char* arg) {
    size_t i, n;
    char   norm[32];

    if (!arg || !arg[0]) return -1;

    /* All-digits → numeric level id, passed through verbatim. */
    for (i = 0; arg[i] && isdigit((unsigned char)arg[i]); i++) {}
    if (!arg[i]) return atoi(arg);

    /* Normalise: lowercase, '-' → '_'. */
    for (n = 0; arg[n] && n < sizeof(norm) - 1; n++) {
        char c = (char)tolower((unsigned char)arg[n]);
        norm[n] = (c == '-') ? '_' : c;
    }
    norm[n] = '\0';

    for (i = 0; i < LEVEL_COUNT; i++)
        if (strcmp(norm, k_levels[i].name) == 0) return k_levels[i].id;
    for (i = 0; i < LEVEL_ALIAS_COUNT; i++)
        if (strcmp(norm, k_level_aliases[i].name) == 0) return k_level_aliases[i].id;

    print_valid_names();
    return -1;
}

static int level_index_for_id(int id) {
    size_t i;
    for (i = 0; i < LEVEL_COUNT; i++)
        if (k_levels[i].id == id) return (int)i;
    return -1;
}

/* ---- state ----------------------------------------------------------- */

typedef enum {
    WARP_IDLE = 0,
    WARP_ARMED,     /* exit-level sequence running; waiting for the hand-off */
    WARP_LOADING    /* destination retargeted; waiting for arrival */
} warp_phase_t;

/* Timeline state — travels with every savestate (see *_state_save/load).
 * A savestate load during an in-flight warp restores the guest RAM the warp
 * had written, so the phase driving those writes has to be restored with it;
 * otherwise a quick-load back to before the warp leaves an armed FSM that
 * hijacks whatever level transition the player triggers next.
 *
 * Rewind and runahead need no separate handling. Both are hard-gated on
 * game_is_gameplay_live() (g_Gamestate == GS_Playing && a level loaded), and
 * arming a warp leaves GS_Playing on its first tick — so the whole in-flight
 * window is outside the window they capture or restore in. */
static uint32_t s_phase  = WARP_IDLE;
static uint32_t s_target = 0;
static uint32_t s_ticks  = 0;

/* Host-side UI state — deliberately NOT in the savestate blob. A click is
 * the user's intent right now; restoring an older pending flag over it (the
 * quiet rewind/runahead path reloads the blob every tick) would either eat
 * a fresh click or resurrect a stale one.
 *   s_sel        — dropdown index into k_levels
 *   s_sel_touched— the user has picked something, so stop auto-following
 *                  the current level
 *   s_pending    — a click waiting for the next non-speculative tick
 *   s_route      — cached "what would Jump do right now", for the gate+status */
static int s_sel         = 0;
static int s_sel_touched = 0;
static int s_pending     = 0;
static int s_pending_id  = 0;

typedef enum { ROUTE_NONE = 0, ROUTE_LEVEL, ROUTE_TITLE } warp_route_t;
static int s_route = ROUTE_NONE;

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

/* ---- dev-overlay rows ------------------------------------------------- */

static double sel_get(const knob_desc_t* k) { (void)k; return (double)s_sel; }

static int sel_set(knob_desc_t* k, double v) {
    int idx = (int)v;
    (void)k;
    if (idx < 0 || idx >= (int)LEVEL_COUNT) return -1;
    s_sel         = idx;
    s_sel_touched = 1;
    return 0;
}

/* Button gate: a route has to apply, and no warp may already be queued or in
 * flight. "Busy" is checked HERE rather than folded into s_route, because
 * s_route is also what the tick consults when it drains the queue — a route
 * that reads ROUTE_NONE just because a click is pending would make every
 * click discard itself. */
static int warp_busy(void) { return s_pending || s_phase != WARP_IDLE; }

static double go_ready(const knob_desc_t* k) {
    (void)k;
    return (s_route != ROUTE_NONE && !warp_busy()) ? 1.0 : 0.0;
}

static int go_fire(knob_desc_t* k, double v) {
    (void)k;
    (void)v;
    if (s_route == ROUTE_NONE || warp_busy()) return -1;
    s_pending_id = k_levels[s_sel].id;   /* snapshot: the combo may move */
    s_pending    = 1;
    return 0;
}

static void go_status(const knob_desc_t* k, char* buf, size_t cap) {
    (void)k;
    if (s_pending)                     snprintf(buf, cap, "queued");
    else if (s_phase == WARP_ARMED)    snprintf(buf, cap, "exiting level...");
    else if (s_phase == WARP_LOADING)  snprintf(buf, cap, "loading...");
    else if (s_route == ROUTE_TITLE)   snprintf(buf, cap, "from title (resets progress)");
    else if (s_route == ROUTE_LEVEL)   snprintf(buf, cap, "%s", "");
    else if (openpete_freecam_active())snprintf(buf, cap, "freecam active");
    else                               snprintf(buf, cap, "not in a level");
}

static void register_rows(void) {
    knob_desc_t row;
    size_t i;

    for (i = 0; i < LEVEL_COUNT; i++) s_level_display[i] = k_levels[i].display;

    memset(&row, 0, sizeof row);
    row.id          = "warp_level";
    row.label       = "Warp: level";
    row.section     = "Gameplay";
    row.help        = "Destination for the Jump button below. Follows the "
                      "level you are in until you pick something.";
    row.type        = KNOB_ENUM;
    row.mut         = KNOB_LIVE;
    row.enum_values = s_level_display;
    row.enum_count  = (int)LEVEL_COUNT;
    row.get_num     = sel_get;
    row.set_num     = sel_set;
    (void)openpete_knob_add(&row);

    memset(&row, 0, sizeof row);
    row.id           = "warp_go";
    row.label        = "Warp: jump to level";
    row.section      = "Gameplay";
    row.help         = "Load the selected level. In a level this rides the "
                       "game's own exit-level sequence, keeping your gems and "
                       "progress. At the title screen it starts a fresh "
                       "session in that level, which resets progress and "
                       "lives (same as --skip-to-level). Intentionally "
                       "divergent gameplay state.";
    row.type         = KNOB_ACTION;
    row.mut          = KNOB_LIVE;
    row.get_num      = go_ready;
    row.set_num      = go_fire;
    row.get_str      = go_status;
    row.action_label = "Jump";
    (void)openpete_knob_add(&row);
}

/* Harness trigger: OPENPETE_LEVELWARP_TEST=<frame>:<level> queues the same
 * request the Jump button does, at the given openpete_frame. The warp is a
 * multi-tick ride through the game's own state machine, so being able to
 * fire it from a script is what makes it testable without a human at the
 * overlay. Deliberately not a registry row (harness var, not a config knob):
 *   OPENPETE_LEVELWARP_TEST=2200:tree_tops openpete-spyro1 --skip-to-level artisans
 */
static int s_test_frame = -1;
static int s_test_id    = 0;

extern unsigned openpete_frame;

static void test_trigger_init(void) {
    const char* spec = getenv("OPENPETE_LEVELWARP_TEST");
    const char* colon;
    char        lvl[32];
    size_t      n;

    if (!spec || !spec[0]) return;
    colon = strchr(spec, ':');
    if (!colon) return;
    for (n = 0; colon[1 + n] && n < sizeof(lvl) - 1; n++) lvl[n] = colon[1 + n];
    lvl[n] = '\0';

    s_test_id = spyro1_level_resolve(lvl);
    if (s_test_id < 0) { s_test_id = 0; return; }
    s_test_frame = atoi(spec);
    OPL_INFO_F("levelwarp", "harness: warp to level %d at frame %d",
               s_test_id, s_test_frame);
}

static void test_trigger_tick(void) {
    if (s_test_frame < 0 || (int)openpete_frame < s_test_frame) return;
    s_test_frame = -1;
    s_pending_id = s_test_id;
    s_pending    = 1;
}

static void levelwarp_apply_options(const openpete_options_t* opts) {
    (void)opts;
    s_phase       = WARP_IDLE;
    s_target      = 0;
    s_ticks       = 0;
    s_sel         = 0;
    s_sel_touched = 0;
    s_pending     = 0;
    s_route       = ROUTE_NONE;
    register_rows();
    test_trigger_init();
}

/* ---- the warp itself -------------------------------------------------- */

/* Put the game into its exit-level sequence. Mirrors what the inventory
 * level-select does, minus its collectable wipe (that exists to restart a
 * speedrun clean; a dev warp wants progress kept).
 *
 * Zeroing BOTH step counters is a departure from the practice ROM, which
 * zeroes only D_800758B8: func_8002E084 sets Spyro's gliding pose at
 * D_8007568C == 16 and only while D_800758B8 == 0, so entering with a
 * mid-cycle D_8007568C silently skips the glide and Spyro exits standing.
 * Starting both at 0 makes the animation the same every time. */
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

/* Retarget the load func_8002C664 just kicked off. It pointed the transition
 * at "my homeworld, arriving out of my own portal"; we want the chosen level,
 * arriving the way a portal entry does.
 *
 * m_SphericalPreset = NULL restores the transition camera's own preset pick
 * (func_80037A20 only auto-picks when the field is NULL, and the exit
 * sequence leaves a stale non-NULL one). Homeworlds fade in rather than fly
 * in, which is what g_HasLevelTransition = 0 selects. */
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

static void warp_abort(const char* why) {
    OPL_INFO_F("levelwarp", "aborted (%s)", why);
    s_phase  = WARP_IDLE;
    s_target = 0;
    s_ticks  = 0;
}

void spyro1_levelwarp_on_tick(CPUState* cpu) {
    uint32_t gs;

    /* Runahead: a speculative tick's guest writes are rolled back, so
     * consuming the click here would drop the warp on the floor. Let the
     * committed tick do it. (The FSM phase itself IS snapshotted, so the
     * rollback of an in-flight warp stays consistent.) */
    if (openpete_speculative) return;

    /* Queue the harness request BEFORE anything else reads s_pending, so it
     * lands in the same place a UI click does: set between ticks, drained on
     * the NEXT one. Firing it mid-tick instead would let the harness take a
     * path the button never takes, which is exactly how the drain bug below
     * shipped past a green headless run. */
    test_trigger_tick();

    gs = cpu->read_word(ADDR_G_GAMESTATE);

    /* Which route a click would take from this gamestate. Derived from guest
     * state ONLY — whether a warp is already queued or running is a separate
     * question, asked by warp_busy() at the button. Pause and inventory count
     * as "in a level" for the same reason the cheats module accepts them:
     * they are gameplay with the sim paused, and the exit sequence takes over
     * the gamestate anyway. The attract demo runs GS_Playing with a level
     * loaded, so it needs excluding explicitly. Freecam is refused because the
     * camera is detached from Spyro and the fly-in would be meaningless. */
    if (gs == GS_TITLESCREEN) {
        s_route = ROUTE_TITLE;
    } else if ((gs == GS_PLAYING || gs == GS_PAUSEMENU || gs == GS_INVENTORYMENU) &&
               cpu->read_word(ADDR_G_LEVELID) != 0u &&
               cpu->read_word(ADDR_G_DEMOMODE) == 0u &&
               !openpete_freecam_active()) {
        s_route = ROUTE_LEVEL;
    } else {
        s_route = ROUTE_NONE;
    }

    /* Keep the dropdown on the level the player is actually in until they
     * pick one themselves. */
    if (!s_sel_touched && s_phase == WARP_IDLE) {
        int idx = level_index_for_id((int)cpu->read_word(ADDR_G_LEVELID));
        if (idx >= 0) s_sel = idx;
    }

    if (s_pending && s_phase != WARP_IDLE) return;   /* drain when idle */

    if (s_pending) {
        int target = s_pending_id;
        s_pending  = 0;
        if (s_route == ROUTE_TITLE) {
            /* The --skip-to-level machinery owns this one: it induces the
             * demo-timeout transition at the next Press-Start window, which
             * also gets us the progress/lives reset a fresh session needs. */
            game_set_skip_to_level(target);
            OPL_INFO_F("levelwarp", "queued title-screen boot into level %d",
                       target);
        } else if (s_route == ROUTE_LEVEL) {
            warp_arm(cpu, target);
        } else {
            OPL_INFO_F("levelwarp", "%s", "ignored (no route from this state)");
        }
        return;
    }

    if (s_phase == WARP_IDLE) return;

    if (++s_ticks > WARP_STUCK_CALLS) { warp_abort("stuck"); return; }

    if (s_phase == WARP_ARMED) {
        /* func_8002C664 flips the gamestate as its last act, so this edge
         * means the hand-off has run and its clobber of g_NextLevelId /
         * g_PortalLevelId is behind us. LoadLevel does not read them until
         * gamestate 1 dispatches later in THIS tick, so writing now lands. */
        if (gs == GS_LEVELTRANSITION) { warp_retarget(cpu); return; }
        /* Anything other than the sequence we started means it is no longer
         * ours to steer — a savestate load restoring gameplay, a death, a
         * cutscene. Bail rather than retarget someone else's transition. */
        if (gs != GS_EXITLEVEL) warp_abort("left the exit sequence");
        return;
    }

    /* WARP_LOADING: the loader has the wheel and we write nothing more —
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

const feature_module_t levelwarp_native_module = {
    .name           = "levelwarp",
    .apply_options  = levelwarp_apply_options,
    .dispatch_table = NULL,
};
