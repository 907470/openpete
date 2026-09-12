// games/spyro-3/skip_to_level.c — --skip-to-level for Spyro: Year of the Dragon.
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
#include "openpete_fiber.h"

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

#define S3_SUBSTATE_IDLE     1u
#define S3_SUBSTATE_TIMEOUT  21u
#define S3_GS_GAMEPLAY       0u
#define S3_GS_LOADING_IMG    5u
#define S3_GS_TITLE          11u
#define S3_LOAD_RESIDENT     0xFFFFFFFFu

/* Level numbers are the game's own (currentLevel); names are the levels'
 * titles in lower_snake_case. */
static const spyro3_level_t k_levels[] = {
    { "sunrise_spring",    10 }, { "sunny_villa",       11 },
    { "cloud_spires",      12 }, { "molten_crater",     13 },
    { "seashell_shore",    14 }, { "mushroom_speedway", 15 },
    { "sheilas_alp",       16 }, { "buzzs_dungeon",     17 },
    { "crawdad_farm",      18 }, { "midday_gardens",    20 },
    { "icy_peak",          21 }, { "enchanted_towers",  22 },
    { "spooky_swamp",      23 }, { "bamboo_terrace",    24 },
    { "country_speedway",  25 }, { "sgt_byrds_base",    26 },
    { "spikes_arena",      27 }, { "spider_town",       28 },
    { "evening_lake",      30 }, { "frozen_altars",     31 },
    { "lost_fleet",        32 }, { "fireworks_factory", 33 },
    { "charmed_ridge",     34 }, { "honey_speedway",    35 },
    { "bentleys_outpost",  36 }, { "scorchs_pit",       37 },
    { "starfish_reef",     38 }, { "midnight_mountain", 40 },
    { "crystal_islands",   41 }, { "desert_ruins",      42 },
    { "haunted_tomb",      43 }, { "dino_mines",        44 },
    { "harbor_speedway",   45 }, { "agent_9s_lab",      46 },
    { "sorceresss_lair",   47 }, { "bugbot_factory",    48 },
    { "super_bonus_round", 50 },
};
#define LEVEL_COUNT (sizeof(k_levels) / sizeof(k_levels[0]))

const spyro3_level_t* spyro3_level_table(size_t* out_count) {
    if (out_count) *out_count = LEVEL_COUNT;
    return k_levels;
}

static int level_known(int number) {
    size_t i;
    for (i = 0; i < LEVEL_COUNT; i++)
        if (k_levels[i].number == number) return 1;
    return 0;
}

static void print_valid_names(void) {
    size_t i;
    fprintf(stderr, "valid level names:\n");
    for (i = 0; i < LEVEL_COUNT; i++)
        fprintf(stderr, "  %-18s (%d)\n", k_levels[i].name, k_levels[i].number);
}

int spyro3_level_resolve(const char* arg) {
    size_t i, n;
    char   norm[32];

    /* No value: the list is the answer (`--skip-to-level` alone). */
    if (!arg || !arg[0]) { print_valid_names(); return -1; }

    /* All-digits: a level number, accepted only if the table has it. */
    for (i = 0; arg[i] && isdigit((unsigned char)arg[i]); i++) {}
    if (!arg[i]) {
        int number = atoi(arg);
        if (level_known(number)) return number;
        print_valid_names();
        return -1;
    }

    /* Normalise: lowercase, '-' -> '_', apostrophes dropped. */
    for (i = 0, n = 0; arg[i] && n < sizeof(norm) - 1; i++) {
        char c = (char)tolower((unsigned char)arg[i]);
        if (c == '\'') continue;
        norm[n++] = (c == '-') ? '_' : c;
    }
    norm[n] = '\0';

    for (i = 0; i < LEVEL_COUNT; i++)
        if (strcmp(norm, k_levels[i].name) == 0) return k_levels[i].number;

    print_valid_names();
    return -1;
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

void spyro3_skip_set(int number) {
    s_target = number;
    s_phase  = number > 0 ? SKIP_ARMED : SKIP_IDLE;
    if (number > 0 && openpete_skip_ffwd_windowed) {
        openpete_skip_present_until_level = 1;
        s_ffwd_deadline = 0;
        fprintf(stderr, "[skip-to-level] fast-forwarding presents until "
                        "level %d loads\n", number);
    }
}

int spyro3_skip_gameplay_live(CPUState* cpu) {
    return cpu->read_word(S3_LOAD_STAGE) == S3_LOAD_RESIDENT &&
           cpu->read_word(S3_GAME_STATE) == S3_GS_GAMEPLAY;
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

    if (openpete_skip_present_until_level) {
        if (!s_ffwd_deadline)
            s_ffwd_deadline = frame_counter + 4000u;
        int playable = s_phase == SKIP_PICKED && spyro3_skip_gameplay_live(cpu);
        if (playable || frame_counter > s_ffwd_deadline) {
            openpete_skip_present_until_level = 0;
            s_ffwd_deadline = 0;
            fprintf(stderr, "[skip-to-level] level playable — presents resume "
                            "at frame=%u\n", (unsigned)frame_counter);
        }
    }
}
