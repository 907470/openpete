// games/spyro-2/skip_to_level.c — --skip-to-level for Spyro 2: Ripto's Rage!
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
// Sub-state 6's handler (0x8007A34C) waits for the timer to reach 12, then
// runs the picker (0x8007A3D4): g_NextLevelNum = list[g_DemoIndex++],
// g_DemoMode = 1, demo input buffer pointers, g_LoaderState = 2. The pad
// path (0x8001200C) substitutes the recorded input while g_DemoMode is set,
// and the per-tick demo-end check (0x80015AD0) ends the level when the tick
// counter reaches the recording's length.

#include "skip_to_level.h"
#include "openpete_fiber.h"

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
#define S2_LOADER_STATE    0x80066EECu

#define S2_SUBSTATE_IDLE     2u
#define S2_SUBSTATE_TIMEOUT  6u
#define S2_LOADER_LOADING    2u
#define S2_LOADER_RESIDENT   0xFFFFFFFFu

/* Level numbers are the game's own (g_NextLevelNum); names are the overlay
 * filenames' lower_snake_case. */
static const spyro2_level_t k_levels[] = {
    { "summer_forest",   10 }, { "glimmer",         11 },
    { "idol_springs",    12 }, { "colossus",        13 },
    { "hurricos",        21 }, { "aquaria_towers",  22 },
    { "sunny_beach",     23 }, { "ocean_speedway",  25 },
    { "crushs_dungeon",  26 }, { "autumn_plains",   30 },
    { "skelos_badlands", 31 }, { "crystal_glacier", 32 },
    { "breeze_harbor",   33 }, { "zephyr",          34 },
    { "metro_speedway",  35 }, { "scorch",          41 },
    { "shady_oasis",     42 }, { "magma_cone",      43 },
    { "fracture_hills",  44 }, { "icy_speedway",    45 },
    { "gulps_overlook",  46 }, { "winter_tundra",   50 },
    { "mystic_marsh",    51 }, { "cloud_temples",   52 },
    { "canyon_speedway", 55 }, { "robotica_farms",  61 },
    { "metropolis",      62 }, { "dragon_shores",   65 },
    { "riptos_arena",    66 },
};
#define LEVEL_COUNT (sizeof(k_levels) / sizeof(k_levels[0]))

const spyro2_level_t* spyro2_level_table(size_t* out_count) {
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

int spyro2_level_resolve(const char* arg) {
    size_t i, n;
    char   norm[32];

    /* No value: the list is the answer (`--skip-to-level` alone). */
    if (!arg || !arg[0]) { print_valid_names(); return -1; }

    /* All-digits: a level number, accepted only if the table has it (an
     * unknown number would index the level table past its end). */
    for (i = 0; arg[i] && isdigit((unsigned char)arg[i]); i++) {}
    if (!arg[i]) {
        int number = atoi(arg);
        if (level_known(number)) return number;
        print_valid_names();
        return -1;
    }

    /* Normalise: lowercase, '-' -> '_'. */
    for (n = 0; arg[n] && n < sizeof(norm) - 1; n++) {
        char c = (char)tolower((unsigned char)arg[n]);
        norm[n] = (c == '-') ? '_' : c;
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

int spyro2_skip_gameplay_live(CPUState* cpu) {
    return cpu->read_word(S2_LOADER_STATE) == S2_LOADER_RESIDENT;
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
        if (cpu->read_word(S2_LOADER_STATE) != 0u)
            cpu->write_word(S2_DEMO_MODE, 0u);
        else
            s_phase = SKIP_IDLE;
    }

    if (openpete_skip_present_until_level) {
        if (!s_ffwd_deadline)
            s_ffwd_deadline = frame_counter + 4000u;
        int playable = s_phase == SKIP_PICKED && spyro2_skip_gameplay_live(cpu);
        if (playable || frame_counter > s_ffwd_deadline) {
            openpete_skip_present_until_level = 0;
            s_ffwd_deadline = 0;
            fprintf(stderr, "[skip-to-level] level playable — presents resume "
                            "at frame=%u\n", (unsigned)frame_counter);
        }
    }
}
