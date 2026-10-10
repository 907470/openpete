// games/spyro-3/features/state_log.c - OPENPETE_S3_STATE_LOG.
//
// Prints the game's state words each time one of them changes, so a
// headless harness can count the VBLs a run spent in each game.state
// (enum Gamestate: 0 gameplay, 1 speech, 2 speedway menu, 3 fade-in,
// 5 loading image, 9 game over, 11 title screen, 13 loading sub-level)
// without a savestate or RAM dump per sample. Reads only.

#include "features/state_log.h"

#include "openpete_fiber.h"          /* openpete_speculative */
#include "psx_runtime_internal.h"    /* CPUState + its read callbacks */
#include "skip_to_level.h"           /* spyro3_skip_gameplay_live */

#include <stdio.h>
#include <stdlib.h>

#define S3_GAME_STATE     0x8006E344u
#define S3_LOAD_STAGE     0x8006C518u
#define S3_CURRENT_LEVEL  0x8006C5BCu
#define S3_AREA           0x8006C5C8u

void spyro3_state_log_on_tick(CPUState* cpu, uint32_t frame_counter) {
    static int s_enabled = -1;
    static uint32_t s_prev[4];
    static int s_have_prev = 0;
    uint32_t cur[4];
    int i, changed = !s_have_prev;
    if (s_enabled < 0) s_enabled = getenv("OPENPETE_S3_STATE_LOG") != NULL;
    if (!s_enabled || openpete_speculative) return;
    cur[0] = cpu->read_word(S3_GAME_STATE);
    cur[1] = cpu->read_word(S3_LOAD_STAGE);
    cur[2] = cpu->read_word(S3_CURRENT_LEVEL);
    cur[3] = cpu->read_word(S3_AREA);
    for (i = 0; i < 4; i++)
        if (cur[i] != s_prev[i]) changed = 1;
    if (!changed) return;
    for (i = 0; i < 4; i++) s_prev[i] = cur[i];
    s_have_prev = 1;
    fprintf(stderr, "[s3-state] frame=%u state=%d load=%d level=%d area=%d "
                    "live=%d\n",
            (unsigned)frame_counter, (int)cur[0], (int)cur[1], (int)cur[2],
            (int)cur[3], spyro3_skip_gameplay_live(cpu));
}
