// games/spyro-2/skip_to_level.c - --skip-to-level for Spyro 2: Ripto's Rage!
//
// Loads directly into a level by inducing the title screen's own
// demo-timeout transition, seeded so it loads the target instead of the next
// attract demo, then holding the demo-mode flag clear so the controller
// drives Spyro instead of the recorded demo input.
//
// The title overlay's state machine (titlescreen_and_loading, SCUS-94425):
//   g_TitleSubState  0x80067E0C  2 = press-start idle, 6 = demo-timeout fade
//   g_TitleTimer     0x80067E00  +1 per VBL, zeroed on each sub-state change
//   g_DemoLevelList  0x800647A4  bytes, 0xFF-terminated: 22 13 41 62 31 52
//   g_DemoIndex      0x80066D40  post-incremented by the picker, wraps at 0xFF
//   g_NextLevelNum   0x80066F90  level number the loader loads (10..66)
//   g_DemoMode       0x80067024  1 from the pick until the level exits
//   g_LoaderState    0x80066EEC  2 = load g_NextLevelNum, -1 = level resident,
//                                0 = title
//   g_LevelIndex     0x80066F54  linear index 0..28 via the byte table at
//                                0x80064940; overlay id = index + 5
//   g_LevelTick      0x80067008  ticks since the level began running; 0
//                                through the load and the overlay swap
// Sub-state 6's handler (0x8007A34C) waits for the timer to reach 12, then
// runs the picker (0x8007A3D4): g_NextLevelNum = list[g_DemoIndex++],
// g_DemoMode = 1, demo input buffer pointers, g_LoaderState = 2. The pad
// path (0x8001200C) substitutes the recorded input while g_DemoMode is set,
// and the per-tick demo-end check (0x80015AD0) ends the level when the tick
// counter reaches the recording's length.

#include "skip_to_level.h"
#include "features/cutscene_skip.h"
#include "features/level_skip.h"
#include "spyro-2/gen/spyro2_symbols.gen.h"

#include <stdio.h>

#define S2_TITLE_SUBSTATE  0x80067E0Cu
#define S2_TITLE_TIMER     0x80067E00u
#define S2_DEMO_LEVEL_LIST 0x800647A4u
#define S2_DEMO_INDEX      0x80066D40u
#define S2_NEXT_LEVEL_NUM  0x80066F90u
#define S2_DEMO_MODE       0x80067024u
#define S2_LOADER_STATE    G_GAMESTATE
#define S2_LEVEL_TICK      0x80067008u

#define S2_SUBSTATE_IDLE     2u
#define S2_SUBSTATE_TIMEOUT  6u
#define S2_LOADER_LOADING    2u
#define S2_LOADER_RESIDENT   0xFFFFFFFFu

/* Level numbers are the game's own (g_NextLevelNum); names are the overlay
 * filenames' lower_snake_case and displays the levels' own titles. */
static const game_level_t k_levels[] = {
    { "summer_forest",   "Summer Forest",   10 },
    { "glimmer",         "Glimmer",         11 },
    { "idol_springs",    "Idol Springs",    12 },
    { "colossus",        "Colossus",        13 },
    { "hurricos",        "Hurricos",        21 },
    { "aquaria_towers",  "Aquaria Towers",  22 },
    { "sunny_beach",     "Sunny Beach",     23 },
    { "ocean_speedway",  "Ocean Speedway",  25 },
    { "crushs_dungeon",  "Crush's Dungeon", 26 },
    { "autumn_plains",   "Autumn Plains",   30 },
    { "skelos_badlands", "Skelos Badlands", 31 },
    { "crystal_glacier", "Crystal Glacier", 32 },
    { "breeze_harbor",   "Breeze Harbor",   33 },
    { "zephyr",          "Zephyr",          34 },
    { "metro_speedway",  "Metro Speedway",  35 },
    { "scorch",          "Scorch",          41 },
    { "shady_oasis",     "Shady Oasis",     42 },
    { "magma_cone",      "Magma Cone",      43 },
    { "fracture_hills",  "Fracture Hills",  44 },
    { "icy_speedway",    "Icy Speedway",    45 },
    { "gulps_overlook",  "Gulp's Overlook", 46 },
    { "winter_tundra",   "Winter Tundra",   50 },
    { "mystic_marsh",    "Mystic Marsh",    51 },
    { "cloud_temples",   "Cloud Temples",   52 },
    { "canyon_speedway", "Canyon Speedway", 55 },
    { "robotica_farms",  "Robotica Farms",  61 },
    { "metropolis",      "Metropolis",      62 },
    { "dragon_shores",   "Dragon Shores",   65 },
    { "riptos_arena",    "Ripto's Arena",   66 },
};
const game_level_table_t spyro2_levels = {
    GAME_LEVEL_ROWS(k_levels), NULL, 0, "level", "--skip-to-level", 0u,
};

int spyro2_level_resolve(const char* arg) {
    return game_level_resolve(&spyro2_levels, arg);
}

int spyro2_skip_gameplay_live(CPUState* cpu) {
    /* The loader reports the overlay resident about 45 VBLs before the level
     * runs its first tick; the window would hold the last presented loading
     * frame across that gap, so the level's own tick counter decides. */
    return cpu->read_word(S2_LOADER_STATE) == S2_LOADER_RESIDENT &&
           cpu->read_word(S2_LEVEL_TICK) != 0u;
}

/** @brief Induces the title's demo-timeout transition toward the target at the press-start idle sub-state.
 *
 * Writes the target into the demo level list at g_DemoIndex, sub-state 6 and
 * the title timer 0.
 * @param cpu The guest CPU state.
 * @param target The level number.
 * @param log_frame The frame value the log line prints.
 * @return 1 when induced, 0 while the sub-state is not idle. */
static int skip_induce(CPUState* cpu, int target, uint32_t log_frame) {
    if (cpu->read_word(S2_TITLE_SUBSTATE) != S2_SUBSTATE_IDLE) return 0;
    uint32_t idx = cpu->read_word(S2_DEMO_INDEX);
    cpu->write_byte(S2_DEMO_LEVEL_LIST + idx, (uint8_t)target);
    cpu->write_word(S2_TITLE_SUBSTATE, S2_SUBSTATE_TIMEOUT);
    cpu->write_word(S2_TITLE_TIMER, 0u);
    fprintf(stderr, "[skip-to-level] induced demo-timeout: level=%d "
                    "(slot=%u) at frame=%u\n",
            target, (unsigned)idx, (unsigned)log_frame);
    return 1;
}

/** @brief Tests whether the loader took the target: loader state 2 with g_NextLevelNum equal to it.
 *
 * @param cpu The guest CPU state.
 * @param target The level number.
 * @return 1 when taken, else 0. */
static int skip_picked(CPUState* cpu, int target) {
    return cpu->read_word(S2_LOADER_STATE) == S2_LOADER_LOADING &&
           cpu->read_word(S2_NEXT_LEVEL_NUM) == (uint32_t)target;
}

/** @brief Holds g_DemoMode clear for one tick until the loader returns to the title.
 *
 * The pad path then reads the controller and the demo-end check never
 * fires. The hold ends when the loader state reads 0 (a level exit).
 * @param cpu The guest CPU state.
 * @return 1 while the hold runs, 0 when it ends. */
static int skip_hold(CPUState* cpu) {
    if (cpu->read_word(S2_LOADER_STATE) == 0u) return 0;
    cpu->write_word(S2_DEMO_MODE, 0u);
    return 1;
}

/** @brief Tests the picked level's playable edge: its first live tick.
 *
 * @param cpu The guest CPU state.
 * @return 1 when gameplay is live, else 0. */
static int skip_playable(CPUState* cpu) { return spyro2_skip_gameplay_live(cpu); }

static const level_skip_watch_t k_watches[] = {
    { spyro2_cutscene_skip_watching, spyro2_cutscene_skip_released,
      spyro2_cutscene_skip_finish },
};

/* The cutscene watch's release predicate is tested before the level skip's
 * own edge: the cutscene's loader-word-0 store ends the picked phase. */
const level_skip_game_t spyro2_level_skip = {
    .induce          = skip_induce,
    .picked          = skip_picked,
    .hold            = skip_hold,
    .hold_end_log    = "loader left the level; demo-mode hold ends",
    .playable        = skip_playable,
    .watches         = k_watches,
    .n_watches       = sizeof k_watches / sizeof k_watches[0],
    .cutscene_source = spyro2_cutscene_source_level,
    .cutscene_rides  = spyro2_cutscene_rides_level_skip,
    .cutscene_arm    = spyro2_cutscene_skip_arm,
};

void spyro2_skip_set(int number) { level_skip_set(&spyro2_level_skip, number); }

int spyro2_skip_target(void) { return level_skip_target(&spyro2_level_skip); }

void spyro2_skip_on_tick(CPUState* cpu, uint32_t frame_counter) {
    level_skip_on_tick(&spyro2_level_skip, cpu, frame_counter);
}
