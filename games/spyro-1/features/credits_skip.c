// games/spyro-1/features/credits_skip.c — `--skip-to-cutscene credits`.
//
// The game reaches its credits through InitCreditsSequence (0x8002D228),
// which clears the framebuffer, kills sound, resets Spyro, sets
// g_Gamestate = GS_Credits and CDLoadSync's the credits overlay over the
// active one. Its cheat-code caller (cheats.c, CheatApplyEffect case 2)
// additionally stashes g_LevelId in D_80075818 so the roll's final stage
// (func_800333DC, stage 99) reloads the level it was entered from; -1 there
// sends the player to Gnorc Gnexus, or Artisans at 14000 gems.
//
// A credits skip is a normal --skip-to-level (the flag's own target, else
// Artisans; the skip machinery in game_extras_impl.c owns the title-screen
// induction and the progress/lives reset) followed by that exact cheat
// effect on the first live tick: D_80075818 = g_LevelId, g_CreditsSequence for the full roll,
// then InitCreditsSequence called in guest context. The call has to run
// inside the game fiber (CDLoadSync yields on the CD), so it is made from a
// dispatch hook on GamestateUpdate (0x8003385C), the first function main()
// calls each tick, rather than from the host-side game_on_frame_pre. The
// hook falls through, so the same tick's GamestateUpdate already runs the
// credits state — one tick earlier than the cheat, whose effect lands
// mid-update.
//
// After the roll the game reloads Artisans with the fresh-session progress
// the skip started with, as the cheat would.

#include "features/credits_skip.h"

#include "feature_registry.h"
#include "openpete_fiber.h" /* openpete_speculative */
#include "openpete_log.h"
#include "psx_runtime_internal.h" /* CPUState + its read/write callbacks */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADDR_GAMESTATE_UPDATE     0x8003385Cu
#define ADDR_INIT_CREDITS_SEQUENCE 0x8002D228u

#define ADDR_G_GAMESTATE          0x800757D8u /* GS_Playing = 0 */
#define ADDR_G_LEVELID            0x8007596Cu
#define ADDR_G_DEMOMODE           0x80075714u /* DEMO_MODE_NONE = 0 */
#define ADDR_G_CREDITSSEQUENCE    0x800757ACu /* 0 = short roll, 10 = full */
#define ADDR_D_80075818           0x80075818u /* level to reload after the roll */
#define ADDR_D_800756C4           0x800756C4u /* overlay delta time */

#define GS_PLAYING 0u

void call_by_address(CPUState* cpu, uint32_t addr); /* psx_runtime.h */

static const struct { const char* name; int id; } k_cutscenes[] = {
    { "credits",      SPYRO1_CUTSCENE_CREDITS },
    { "credits_100",  SPYRO1_CUTSCENE_CREDITS_FULL },
    { "full_credits", SPYRO1_CUTSCENE_CREDITS_FULL },
};
#define CUTSCENE_COUNT (sizeof(k_cutscenes) / sizeof(k_cutscenes[0]))

static void print_valid_names(void) {
    size_t i;
    fprintf(stderr, "valid cutscene names:\n");
    for (i = 0; i < CUTSCENE_COUNT; i++)
        fprintf(stderr, "  %-18s (%d)\n", k_cutscenes[i].name, k_cutscenes[i].id);
}

int spyro1_cutscene_resolve(const char* arg) {
    size_t i, n;
    char   norm[32];

    if (!arg || !arg[0]) { print_valid_names(); return -1; }

    for (i = 0; arg[i] && isdigit((unsigned char)arg[i]); i++) {}
    if (!arg[i]) {
        int id = atoi(arg);
        for (i = 0; i < CUTSCENE_COUNT; i++)
            if (k_cutscenes[i].id == id) return id;
        print_valid_names();
        return -1;
    }

    for (n = 0; arg[n] && n < sizeof(norm) - 1; n++) {
        char c = (char)tolower((unsigned char)arg[n]);
        norm[n] = (c == '-') ? '_' : c;
    }
    norm[n] = '\0';
    for (i = 0; i < CUTSCENE_COUNT; i++)
        if (strcmp(norm, k_cutscenes[i].name) == 0) return k_cutscenes[i].id;

    print_valid_names();
    return -1;
}

static int s_armed = 0;
static int s_full  = 0;

void spyro1_credits_skip_arm(int full) {
    s_armed = 1;
    s_full  = full ? 1 : 0;
}

int spyro1_credits_skip_pending(void) { return s_armed; }

static int on_gamestate_update(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!s_armed) return 0;
    /* A speculative (runahead) tick's guest writes roll back; the disarm here
     * would not. */
    if (openpete_speculative) return 0;
    if (cpu->read_word(ADDR_G_GAMESTATE) != GS_PLAYING) return 0;
    uint32_t level = cpu->read_word(ADDR_G_LEVELID);
    if (level == 0u || cpu->read_word(ADDR_G_DEMOMODE) != 0u) return 0;
    /* The credits overlay ticks its entry timers by D_800756C4, the delta
     * time the level overlays' moby update loop copies from g_DeltaTime; the
     * credits never write it. Fresh from boot it is 0 and every timer
     * stalls, so wait for a gameplay tick to have set it, as the pad code
     * (entered mid-session) always finds it set. */
    if (cpu->read_word(ADDR_D_800756C4) == 0u) return 0;

    s_armed = 0;
    cpu->write_word(ADDR_D_80075818, level);
    if (s_full) cpu->write_word(ADDR_G_CREDITSSEQUENCE, 10u);

    /* GamestateUpdate takes no arguments and this is its call boundary, so
     * only the return address needs preserving across the nested call. */
    uint32_t ra = cpu->ra;
    cpu->a0 = s_full ? 1u : 0u;
    call_by_address(cpu, ADDR_INIT_CREDITS_SEQUENCE);
    cpu->ra = ra;

    OPL_INFO_F("skip-to-cutscene", "%s credits started from level %u; the "
               "roll returns there", s_full ? "full (100%)" : "short",
               (unsigned)level);
    return 0;
}

static const feature_dispatch_entry_t k_dispatch[] = {
    { ADDR_GAMESTATE_UPDATE, on_gamestate_update },
    { 0, NULL },
};

const feature_module_t credits_skip_module = {
    .name           = "credits-skip",
    .apply_options  = NULL,
    .dispatch_table = k_dispatch,
};
