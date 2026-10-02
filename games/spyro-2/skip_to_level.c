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
#include "openpete_fiber.h"
#include "openpete_skip.h"
#include "named_target.h"
#include "features/cutscene_skip.h"
#include "spyro2_symbols.gen.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static const spyro2_level_t k_levels[] = {
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
#define LEVEL_COUNT (sizeof(k_levels) / sizeof(k_levels[0]))

const spyro2_level_t* spyro2_level_table(size_t* out_count) {
    if (out_count) *out_count = LEVEL_COUNT;
    return k_levels;
}

int spyro2_level_resolve(const char* arg) {
    const named_target_table_t levels =
        NAMED_TARGET_TABLE(k_levels, spyro2_level_t, name, number);
    return named_target_resolve(arg, "level", &levels, NULL, 0u);
}

/* ---- state ----------------------------------------------------------- */

typedef enum {
    SKIP_IDLE = 0,
    SKIP_ARMED,    /* waiting for the press-start idle sub-state */
    SKIP_SEEDED,   /* list slot seeded, timeout induced; waiting for the pick */
    SKIP_PICKED,   /* loader took the target; holding g_DemoMode clear */
} skip_phase_t;

static int          s_target = 0;
static skip_phase_t s_phase  = SKIP_IDLE;
/* Present fast-forward stop cap, set relative to arming so a runtime warp
 * (armed tens of thousands of frames in) is not cleared instantly. */
static uint32_t     s_ffwd_deadline = 0;

void spyro2_skip_set(int number) {
    s_target = number;
    s_phase  = number > 0 ? SKIP_ARMED : SKIP_IDLE;
    if (number > 0 && openpete_skip_ffwd_windowed) {
        openpete_skip_present_until_level = 1;
        s_ffwd_deadline = 0;
        fprintf(stderr, "[skip-to-level] fast-forwarding presents until "
                        "level %d loads\n", number);
    }
}

int spyro2_skip_target(void) { return s_phase != SKIP_IDLE ? s_target : 0; }

int spyro2_skip_gameplay_live(CPUState* cpu) {
    /* The loader reports the overlay resident about 45 VBLs before the level
     * runs its first tick; the window would hold the last presented loading
     * frame across that gap, so the level's own tick counter decides. */
    return cpu->read_word(S2_LOADER_STATE) == S2_LOADER_RESIDENT &&
           cpu->read_word(S2_LEVEL_TICK) != 0u;
}

/** @brief Releases a skip's present fast-forward and cutscene watch on the playable edge or the fail-safe deadline.
 *
 * Runs every tick while a present fast-forward or a cutscene target is
 * watched, whatever the phase. Presents resume
 * (openpete_skip_present_until_level clears) on the playable edge: the picked
 * level's first live tick, or, while a cutscene target is watched, that
 * target's release predicate (features/cutscene_skip.c), which is tested
 * outside the SKIP_PICKED gate because the cutscene's loader-word-0 store ends
 * the phase. The release line names the engagement values read back at
 * release. A deadline openpete_skip_failsafe_vbls() after the first check is
 * the fail-safe against a never-reached edge and logs its own line. The
 * cutscene watch runs headless too, where there is no fast-forward, so a
 * harness reads the same lines. Either release ends the cutscene watch.
 * @param cpu The guest CPU state.
 * @param frame_counter The openpete_frame counter. */
static void skip_release_on_tick(CPUState* cpu, uint32_t frame_counter) {
    int watching = spyro2_cutscene_skip_watching();
    if (!openpete_skip_present_until_level && !watching) return;
    if (!s_ffwd_deadline)
        s_ffwd_deadline = frame_counter + openpete_skip_failsafe_vbls();
    char eng[128] = "";
    int playable = watching
        ? spyro2_cutscene_skip_released(cpu, eng, sizeof eng)
        : s_phase == SKIP_PICKED && spyro2_skip_gameplay_live(cpu);
    if (playable) {
        fprintf(stderr, "[skip-to-level] level playable - presents resume "
                        "at frame=%u%s\n", (unsigned)frame_counter, eng);
    } else if (frame_counter > s_ffwd_deadline) {
        fprintf(stderr, "[skip-to-level] fail-safe release (no playable "
                        "edge) at frame=%u\n", (unsigned)frame_counter);
    } else {
        return;
    }
    openpete_skip_present_until_level = 0;
    s_ffwd_deadline = 0;
    spyro2_cutscene_skip_finish();
}

void spyro2_skip_on_tick(CPUState* cpu, uint32_t frame_counter) {
    if (s_phase == SKIP_ARMED) {
        if (cpu->read_word(S2_TITLE_SUBSTATE) == S2_SUBSTATE_IDLE) {
            uint32_t idx = cpu->read_word(S2_DEMO_INDEX);
            cpu->write_byte(S2_DEMO_LEVEL_LIST + idx, (uint8_t)s_target);
            cpu->write_word(S2_TITLE_SUBSTATE, S2_SUBSTATE_TIMEOUT);
            cpu->write_word(S2_TITLE_TIMER, 0u);
            s_phase = SKIP_SEEDED;
            fprintf(stderr, "[skip-to-level] induced demo-timeout: level=%d "
                            "(slot=%u) at frame=%u\n",
                    s_target, (unsigned)idx, (unsigned)frame_counter);
        }
    } else if (s_phase == SKIP_SEEDED) {
        if (cpu->read_word(S2_LOADER_STATE) == S2_LOADER_LOADING &&
            cpu->read_word(S2_NEXT_LEVEL_NUM) == (uint32_t)s_target) {
            s_phase = SKIP_PICKED;
            fprintf(stderr, "[skip-to-level] loader took level=%d at "
                            "frame=%u\n", s_target, (unsigned)frame_counter);
        }
    }
    if (s_phase == SKIP_PICKED) {
        /* Hold the demo flag clear from the pick through the level: the pad
         * path reads the controller and the demo-end check never fires. The
         * hold ends when the loader returns to the title (a level exit). */
        if (cpu->read_word(S2_LOADER_STATE) != 0u) {
            cpu->write_word(S2_DEMO_MODE, 0u);
        } else {
            s_phase = SKIP_IDLE;
            fprintf(stderr, "[skip-to-level] loader left the level; demo-mode "
                            "hold ends at frame=%u\n", (unsigned)frame_counter);
        }
    }

    skip_release_on_tick(cpu, frame_counter);
}
