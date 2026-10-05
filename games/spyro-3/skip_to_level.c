// games/spyro-3/skip_to_level.c - --skip-to-level for Spyro: Year of the Dragon.
//
// Loads directly into a level by inducing the title screen's own
// demo-timeout transition, seeded so its picker loads the target instead of
// the next attract demo, then holding isDemoMode clear so the controller
// drives Spyro and the level never ends as a demo would.
//
// The title overlay's state machine (title.ovl, SCUS-94467), updated once per
// frame by func_title_8007566C while game.state == GAMESTATE_TITLE_SCREEN:
//   g_TitleSubState  0x8007F12C  byte; 1 = press-start idle, 21 = demo
//                                timeout, 20 = New Game / Continue
//   g_TitlePrevState 0x8007F12D  byte; the update's tail compares it with
//                                the sub-state and zeroes the phase bytes
//                                below on a change
//   g_TitlePhase     0x8007F12E..0x8007F132  per-sub-state phase counters
//   g_TitleIdleTimer 0x8007F138  word; +1 per frame in sub-state 1, zeroed
//                                by any pad press; 900 triggers the timeout
//   g_DemoLevelList  0x80066EDC  bytes, 0xFF-terminated: 13 33 22 31 33 44 35
//   g_DemoAreaList   0x80066EE4  bytes, parallel: the sub-area each demo
//                                starts in
//   g_DemoIndex      0x8006C3F0  word, post-incremented by the picker, wraps
//                                at the 0xFF sentinel
//   currentLevel     0x8006C5BC  level number the loader loads (10..50)
//   area             0x8006C5C8  sub-area of the level
//   isDemoMode       0x8006C658  1 from the timeout until the level exits
//   game.state       0x8006E344  enum Gamestate: 0 gameplay, 3 fade-in,
//                                5 loading image, 11 title screen
//   loadStage        0x8006C518  loading overlay's state; -1 once the level
//                                overlay is resident
// Sub-state 1 with the idle timer at 900 sets isDemoMode = 1 and sub-state
// 21 (0x80075D38). Sub-state 21's handler (func_title_8007777C) waits for its
// fade, stops the title music (phase 0), then runs the picker
// (func_title_800777DC): currentLevel = list[g_DemoIndex], area =
// arealist[g_DemoIndex++], per-demo start position, WarpToLevel(0,
// currentLevel) (func_800584BC: game.state = 5, loadStage = 0), isDemoMode =
// 1, demo input buffer pointers. The pad path substitutes the recorded input
// while isDemoMode is set and the level exits when the recording runs out;
// the loading overlay also starts a demo in its sub-area (UpdateLoadingImage
// case 1) and takes the demo branch of its fade-in (case 3).

#include "skip_to_level.h"
#include "features/cutscene_skip.h"
#include "features/skip_to_area.h"
#include "openpete_fiber.h"
#include "openpete_skip.h"
#include "named_target.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S3_TITLE_SUBSTATE   0x8007F12Cu
#define S3_TITLE_PREVSTATE  0x8007F12Du
#define S3_TITLE_PHASE      0x8007F12Eu   /* 5 bytes through 0x8007F132 */
#define S3_DEMO_LEVEL_LIST  0x80066EDCu
#define S3_DEMO_AREA_LIST   0x80066EE4u
#define S3_DEMO_INDEX       0x8006C3F0u
#define S3_CURRENT_LEVEL    0x8006C5BCu
#define S3_IS_DEMO_MODE     0x8006C658u
#define S3_GAME_STATE       0x8006E344u
#define S3_LOAD_STAGE       0x8006C518u

#define S3_GS_CUTSCENE       6u
#define S3_SUBSTATE_IDLE     1u
#define S3_SUBSTATE_TIMEOUT  21u
#define S3_GS_GAMEPLAY       0u
#define S3_GS_LOADING_IMG    5u
#define S3_GS_TITLE          11u
#define S3_LOAD_RESIDENT     0xFFFFFFFFu

/* Level numbers are the game's own (currentLevel); names are the levels'
 * titles in lower_snake_case and displays those titles. */
static const spyro3_level_t k_levels[] = {
    { "sunrise_spring",    "Sunrise Spring",    10 },
    { "sunny_villa",       "Sunny Villa",       11 },
    { "cloud_spires",      "Cloud Spires",      12 },
    { "molten_crater",     "Molten Crater",     13 },
    { "seashell_shore",    "Seashell Shore",    14 },
    { "mushroom_speedway", "Mushroom Speedway", 15 },
    { "sheilas_alp",       "Sheila's Alp",      16 },
    { "buzzs_dungeon",     "Buzz's Dungeon",    17 },
    { "crawdad_farm",      "Crawdad Farm",      18 },
    { "midday_gardens",    "Midday Gardens",    20 },
    { "icy_peak",          "Icy Peak",          21 },
    { "enchanted_towers",  "Enchanted Towers",  22 },
    { "spooky_swamp",      "Spooky Swamp",      23 },
    { "bamboo_terrace",    "Bamboo Terrace",    24 },
    { "country_speedway",  "Country Speedway",  25 },
    { "sgt_byrds_base",    "Sgt. Byrd's Base",  26 },
    { "spikes_arena",      "Spike's Arena",     27 },
    { "spider_town",       "Spider Town",       28 },
    { "evening_lake",      "Evening Lake",      30 },
    { "frozen_altars",     "Frozen Altars",     31 },
    { "lost_fleet",        "Lost Fleet",        32 },
    { "fireworks_factory", "Fireworks Factory", 33 },
    { "charmed_ridge",     "Charmed Ridge",     34 },
    { "honey_speedway",    "Honey Speedway",    35 },
    { "bentleys_outpost",  "Bentley's Outpost", 36 },
    { "scorchs_pit",       "Scorch's Pit",      37 },
    { "starfish_reef",     "Starfish Reef",     38 },
    { "midnight_mountain", "Midnight Mountain", 40 },
    { "crystal_islands",   "Crystal Islands",   41 },
    { "desert_ruins",      "Desert Ruins",      42 },
    { "haunted_tomb",      "Haunted Tomb",      43 },
    { "dino_mines",        "Dino Mines",        44 },
    { "harbor_speedway",   "Harbor Speedway",   45 },
    { "agent_9s_lab",      "Agent 9's Lab",     46 },
    { "sorceresss_lair",   "Sorceress's Lair",  47 },
    { "bugbot_factory",    "Bugbot Factory",    48 },
    { "super_bonus_round", "Super Bonus Round", 50 },
};
#define LEVEL_COUNT (sizeof(k_levels) / sizeof(k_levels[0]))

const spyro3_level_t* spyro3_level_table(size_t* out_count) {
    if (out_count) *out_count = LEVEL_COUNT;
    return k_levels;
}

void spyro3_level_surface(struct openpete_surface_sink* sink) {
    const named_target_table_t levels =
        NAMED_TARGET_TABLE(k_levels, spyro3_level_t, name, number);
    named_target_surface(sink, "level", "--skip-to-level", &levels);
}

int spyro3_level_resolve(const char* arg) {
    const named_target_table_t levels =
        NAMED_TARGET_TABLE(k_levels, spyro3_level_t, name, number);
    return named_target_resolve(arg, "level", &levels, NULL, NAMED_TARGET_DROP_APOSTROPHE);
}

/* ---- state ----------------------------------------------------------- */

typedef enum {
    SKIP_IDLE = 0,
    SKIP_ARMED,    /* waiting for the press-start idle sub-state */
    SKIP_SEEDED,   /* list slot seeded, timeout induced; waiting for the pick */
    SKIP_PICKED,   /* loader took the target; holding isDemoMode clear */
} skip_phase_t;

static int          s_target = 0;
static skip_phase_t s_phase  = SKIP_IDLE;
/* Present fast-forward stop cap, set relative to arming so a runtime warp
 * (armed tens of thousands of frames in) is not cleared instantly. */
static uint32_t     s_ffwd_deadline = 0;

void spyro3_skip_ffwd_arm(const char* until) {
    if (!openpete_skip_ffwd_windowed) return;
    openpete_skip_present_until_level = 1;
    s_ffwd_deadline = 0;
    fprintf(stderr, "[skip-to-level] fast-forwarding presents until %s\n", until);
}

void spyro3_skip_set(int number) {
    char until[40];
    s_target = number;
    s_phase  = number > 0 ? SKIP_ARMED : SKIP_IDLE;
    if (number <= 0) return;
    snprintf(until, sizeof until, "level %d loads", number);
    spyro3_skip_ffwd_arm(until);
}

int spyro3_skip_target(void) { return s_target; }

int spyro3_skip_level_picked(void) { return s_phase == SKIP_PICKED; }

int spyro3_skip_gameplay_live(CPUState* cpu) {
    return cpu->read_word(S3_LOAD_STAGE) == S3_LOAD_RESIDENT &&
           cpu->read_word(S3_GAME_STATE) == S3_GS_GAMEPLAY;
}

/** @brief Test for the playable edge that ends the skip's present fast-forward.
 *
 * An armed --skip-to-area watch decides first, then an armed
 * --skip-to-cutscene watch; with neither, the edge is the first call after
 * the pick on which spyro3_skip_gameplay_live holds.
 * @param cpu The guest CPU state.
 * @param eng Buffer the area or cutscene predicate fills with the words it
 *        appends to the release log line.
 * @param cap Size of `eng` in bytes.
 * @return Nonzero on the playable edge. */
static int target_released(CPUState* cpu, char* eng, size_t cap) {
    if (spyro3_area_watching()) return spyro3_area_released(cpu, eng, cap);
    if (spyro3_cutscene_watching()) return spyro3_cutscene_released(cpu, eng, cap);
    return s_phase == SKIP_PICKED && spyro3_skip_gameplay_live(cpu);
}

/** @brief Ends the armed --skip-to-area and --skip-to-cutscene watches.
 *
 * Each module's finish runs only while its watch is armed. */
static void target_finish(void) {
    if (spyro3_area_watching()) spyro3_area_finish();
    if (spyro3_cutscene_watching()) spyro3_cutscene_finish();
}

/** @brief Release the skip on its playable edge or on the fail-safe deadline.
 *
 * Runs on every call while a present fast-forward, a --skip-to-area target
 * or a --skip-to-cutscene target is armed. On the edge (target_released) it
 * logs "level playable", with the words the area and cutscene modules read
 * back at release. The deadline is openpete_skip_failsafe_vbls() past the
 * first check and is re-set on each game.state 6 (cutscene) entry, since a
 * cutscene chain holds the game out of gameplay for longer than one
 * deadline; passing it logs "fail-safe release (no playable edge)". Either
 * release clears openpete_skip_present_until_level, zeroes the deadline and
 * finishes the armed area or cutscene watch. The area and cutscene watches
 * run headless too, where there is no fast-forward, so a harness reads the
 * same lines.
 * @param cpu The guest CPU state.
 * @param frame_counter openpete_frame, compared with the deadline and
 *        printed in the log lines. */
static void skip_release_tick(CPUState* cpu, uint32_t frame_counter) {
    static uint32_t s_prev_state = 0;
    uint32_t state = cpu->read_word(S3_GAME_STATE);
    char eng[96] = "";
    if (!s_ffwd_deadline || (state == S3_GS_CUTSCENE && s_prev_state != S3_GS_CUTSCENE))
        s_ffwd_deadline = frame_counter + openpete_skip_failsafe_vbls();
    s_prev_state = state;
    if (target_released(cpu, eng, sizeof eng)) {
        fprintf(stderr, "[skip-to-level] level playable - presents resume "
                        "at frame=%u%s\n", (unsigned)frame_counter, eng);
    } else if (frame_counter > s_ffwd_deadline) {
        fprintf(stderr, "[skip-to-level] fail-safe release (no playable edge) "
                        "at frame=%u\n", (unsigned)frame_counter);
    } else {
        return;
    }
    openpete_skip_present_until_level = 0;
    s_ffwd_deadline = 0;
    target_finish();
}

void spyro3_skip_on_tick(CPUState* cpu, uint32_t frame_counter) {
    if (s_phase == SKIP_ARMED) {
        /* The idle sub-state's first frames still fade the menu in (phase 0);
         * the timeout fires from phase 1, so the induction waits for it. */
        if (cpu->read_word(S3_GAME_STATE) == S3_GS_TITLE &&
            cpu->read_byte(S3_TITLE_SUBSTATE) == S3_SUBSTATE_IDLE &&
            cpu->read_byte(S3_TITLE_PHASE) == 1u) {
            uint32_t idx = cpu->read_word(S3_DEMO_INDEX);
            int i;
            cpu->write_byte(S3_DEMO_LEVEL_LIST + idx, (uint8_t)s_target);
            cpu->write_byte(S3_DEMO_AREA_LIST + idx, 0u);
            /* The timeout's own store plus the update tail's change handling
             * (previous state mirrored, phase counters zeroed), without the
             * isDemoMode = 1 that precedes it. */
            cpu->write_byte(S3_TITLE_SUBSTATE, S3_SUBSTATE_TIMEOUT);
            cpu->write_byte(S3_TITLE_PREVSTATE, S3_SUBSTATE_TIMEOUT);
            for (i = 0; i < 5; i++) cpu->write_byte(S3_TITLE_PHASE + i, 0u);
            s_phase = SKIP_SEEDED;
            fprintf(stderr, "[skip-to-level] induced demo-timeout: level=%d "
                            "(slot=%u) at frame=%u\n",
                    s_target, (unsigned)idx, (unsigned)frame_counter);
        }
    } else if (s_phase == SKIP_SEEDED) {
        if (cpu->read_word(S3_GAME_STATE) == S3_GS_LOADING_IMG &&
            cpu->read_word(S3_CURRENT_LEVEL) == (uint32_t)s_target) {
            s_phase = SKIP_PICKED;
            fprintf(stderr, "[skip-to-level] loader took level=%d at "
                            "frame=%u\n", s_target, (unsigned)frame_counter);
        }
    }
    if (s_phase == SKIP_PICKED) {
        /* Hold the demo flag clear from the pick through the level: the pad
         * path reads the controller, the loader takes its non-demo branches
         * and the recording's end never exits the level. The hold ends when
         * the game returns to the title (a level exit). */
        if (cpu->read_word(S3_GAME_STATE) != S3_GS_TITLE)
            cpu->write_word(S3_IS_DEMO_MODE, 0u);
        else
            s_phase = SKIP_IDLE;
    }

    if (openpete_skip_present_until_level || spyro3_area_watching() ||
        spyro3_cutscene_watching())
        skip_release_tick(cpu, frame_counter);
}
