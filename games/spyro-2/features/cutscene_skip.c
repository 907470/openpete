// games/spyro-2/features/cutscene_skip.c — `--skip-to-cutscene` for Spyro 2.
//
// Spyro 2 starts every story cutscene through func_80019230(k) (0x80019230):
// it stores the top-level state D_800681C8 = 5, the loader word 0x80066EEC =
// D_80063250[k], D_800698F4 = k (k 7 becomes 9 while byte 0x8006B08D is set,
// k 11 becomes 13 while byte 0x8006B099 is set) and, for k != 0,
// g_NextLevelNum 0x80066F90 = D_8006323C[k]. State 5 then calls StartCutscene
// (0x80073E48), which stores D_800681C8 = 6 while the cutscene plays.
//
// The Dragon Shores (level 65) theater replays the eleven story cutscenes
// from inside a level: its moby code (func_level_65_8006FA78, 0x8006FB78)
// calls func_80019230(k) with k from its pair table at 0x8006DAFC, then
// stores g_NextLevelNum = the pair's pseudo-level, the loader word 0 (loader
// word 2 from inside a level would run the loader through addresses the
// resident level overlay owns), byte 0x800698BD = 1 and D_800698F4 = k. At
// the cutscene's end the title overlay reads byte 0x800698BD set, calls
// func_80019230(0) and loads level 65 again.
//
// A cutscene skip is a --skip-to-level 65 followed by the theater's call and
// stores, made from a dispatch hook on the draw function func_800156FC
// (0x800156FC) entry on the first tick of Dragon Shores gameplay
// (D_800681C8 0). The theater calls from moby code inside the update
// func_8001B140, so the whole update has run when it calls; a call at the
// draw entry follows the tick's whole update, and the draw reads the call's
// stores as it does after the theater's. The hook returns 0 so the draw
// runs. The loader-word-0 store also ends the level skip's demo-mode hold
// (skip_to_level.c), as the theater's own store does on the level-exit path.
//
// Precondition read at the call: bytes 0x8006B08D and 0x8006B099 are 0 (a
// set byte remaps k 7 or 11 to a level row); the --skip-to-level reset
// leaves them clear, and a set byte refuses the row.
//
// The hook is inert until armed (the attract demo never arms it) and does
// nothing on a speculative (runahead) tick: a request acted on there is
// rolled back with RAM while the host latch would read it as done.

#include "features/cutscene_skip.h"

#include "feature_registry.h"
#include "named_target.h"
#include "openpete_fiber.h" /* openpete_speculative, openpete_frame */
#include "openpete_log.h"
#include "psx_runtime_internal.h" /* CPUState + its read/write callbacks */

#include <stdint.h>
#include <stdio.h>

#define ADDR_DRAW             0x800156FCu /* func_800156FC, the per-tick draw */
#define ADDR_CUTSCENE_START   0x80019230u /* func_80019230(k) */

#define ADDR_TOP_STATE        0x800681C8u /* 0 gameplay, 5 cutscene start,
                                             6 playback, 4 from a replay's
                                             return to level 65 */
#define ADDR_LOADER_STATE     0x80066EECu /* -1 level resident, 0 title */
#define ADDR_NEXT_LEVEL_NUM   0x80066F90u
#define ADDR_LEVEL_TICK       0x80067008u
#define ADDR_DEMO_MODE        0x80067024u
#define ADDR_CUTSCENE_K       0x800698F4u /* D_800698F4 */
#define ADDR_RETURN_TO_65     0x800698BDu /* byte: replay returns to level 65 */
#define ADDR_REMAP_K7         0x8006B08Du /* byte: k 7 -> 9 */
#define ADDR_REMAP_K11        0x8006B099u /* byte: k 11 -> 13 */

#define TOP_GAMEPLAY    0u
#define TOP_REPLAY_RETURN 4u
#define TOP_PLAYBACK    6u
#define LOADER_RESIDENT 0xFFFFFFFFu

void call_by_address(CPUState* cpu, uint32_t addr); /* psx_runtime.h */

/* One row per story cutscene, in k order: the name, k (func_80019230's
 * argument) and the pseudo-level the theater's pair table gives it
 * (D_8006323C[k]). k 3, 5, 9 and 13 are level rows of that table, and k 0
 * is the return from a replay. */
typedef struct {
    const char* name;
    int         k;
    uint32_t    pseudo_level;
} cutscene_t;

static const cutscene_t k_cutscenes[] = {
    { "intro",          1, 70u }, /* from the title's New Game */
    { "intro_arrival",  2, 71u }, /* chains from k 1, then Glimmer */
    { "summer_forest",  4, 72u }, /* leaving Glimmer, Summer Forest unvisited */
    { "crush_intro",    6, 73u }, /* entering Crush's Dungeon, unvisited */
    { "crush_defeated", 7, 74u }, /* from Crush's Dungeon (level 26) */
    { "autumn_plains",  8, 75u }, /* chains from k 7, then Autumn Plains */
    { "gulp_intro",    10, 76u }, /* entering Gulp's Overlook, unvisited */
    { "gulp_defeated", 11, 77u }, /* from Gulp's Overlook (level 46) */
    { "winter_tundra", 12, 78u }, /* chains from k 11, then Winter Tundra */
    { "ripto_intro",   14, 79u }, /* from the homeworld code (level 50) */
    { "ending",        15, 80u }, /* from Ripto's Arena (level 66) */
};

static const cutscene_t* row_of(int k) {
    for (size_t i = 0; i < sizeof(k_cutscenes) / sizeof(k_cutscenes[0]); i++)
        if (k_cutscenes[i].k == k) return &k_cutscenes[i];
    return NULL;
}

int spyro2_cutscene_resolve(const char* arg) {
    const named_target_table_t cutscenes =
        NAMED_TARGET_TABLE(k_cutscenes, cutscene_t, name, k);
    return named_target_resolve(arg, "cutscene", &cutscenes, NULL, 0u);
}

static const cutscene_t* s_row       = NULL; /* armed row, NULL when idle */
static int               s_induced   = 0;    /* the hook has called */
static int               s_watching  = 0;
static int               s_returning = 0;    /* released; waiting for 65 */

void spyro2_cutscene_skip_arm(int k) {
    s_row       = row_of(k);
    s_induced   = 0;
    s_returning = 0;
    s_watching  = s_row != NULL;
}

int  spyro2_cutscene_skip_watching(void) { return s_watching; }
void spyro2_cutscene_skip_finish(void)   { s_watching = 0; }

int spyro2_cutscene_skip_released(CPUState* cpu, char* eng, size_t cap) {
    if (!s_row || !s_induced) return 0;
    uint32_t top = cpu->read_word(ADDR_TOP_STATE);
    uint32_t k   = cpu->read_word(ADDR_CUTSCENE_K);
    if (top != TOP_PLAYBACK || k != (uint32_t)s_row->k) return 0;
    snprintf(eng, cap, " cutscene=%s k=%u D_800681C8=%u next_level=%u",
             s_row->name, (unsigned)k, (unsigned)top,
             (unsigned)cpu->read_word(ADDR_NEXT_LEVEL_NUM));
    s_returning = 1;
    return 1;
}

/* The first tick of Dragon Shores gameplay: the level resident and running,
 * the top-level state gameplay (the fade-in's 3 also passes the level
 * skip's live test) and the demo flag clear. */
static int dragon_shores_live(CPUState* cpu) {
    return cpu->read_word(ADDR_LOADER_STATE) == LOADER_RESIDENT &&
           cpu->read_word(ADDR_LEVEL_TICK) != 0u &&
           cpu->read_word(ADDR_NEXT_LEVEL_NUM) ==
               (uint32_t)SPYRO2_CUTSCENE_SOURCE_LEVEL &&
           cpu->read_word(ADDR_TOP_STATE) == TOP_GAMEPLAY &&
           cpu->read_word(ADDR_DEMO_MODE) == 0u;
}

/* The theater's call and its four stores (see the file header). */
static void cutscene_hook(CPUState* cpu) {
    const cutscene_t* r = s_row;
    s_induced = 1;
    if (cpu->read_byte(ADDR_REMAP_K7) || cpu->read_byte(ADDR_REMAP_K11)) {
        OPL_INFO_F("skip-to-cutscene", "%s: refused, remap byte "
                   "0x8006B08D or 0x8006B099 set", r->name);
        s_row = NULL;
        s_watching = 0;
        return;
    }
    /* The draw function takes no arguments and this is its call boundary,
     * so only the return address needs preserving across the nested call. */
    uint32_t ra = cpu->ra;
    cpu->a0 = (uint32_t)r->k;
    call_by_address(cpu, ADDR_CUTSCENE_START);
    cpu->ra = ra;
    cpu->write_word(ADDR_NEXT_LEVEL_NUM, r->pseudo_level);
    cpu->write_word(ADDR_LOADER_STATE, 0u);
    cpu->write_byte(ADDR_RETURN_TO_65, 1u);
    cpu->write_word(ADDR_CUTSCENE_K, (uint32_t)r->k);
    OPL_INFO_F("skip-to-cutscene", "%s: func_80019230(%d) from Dragon Shores "
               "at frame=%u (D_800681C8 %u, D_800698F4 %u, next level %u)",
               r->name, r->k, openpete_frame,
               (unsigned)cpu->read_word(ADDR_TOP_STATE),
               (unsigned)cpu->read_word(ADDR_CUTSCENE_K),
               (unsigned)cpu->read_word(ADDR_NEXT_LEVEL_NUM));
}

/* After the release: the replay's return to Dragon Shores (the level
 * resident again with D_800681C8 4). Reads only. */
static void watch_return(CPUState* cpu) {
    if (cpu->read_word(ADDR_LOADER_STATE) != LOADER_RESIDENT ||
        cpu->read_word(ADDR_NEXT_LEVEL_NUM) !=
            (uint32_t)SPYRO2_CUTSCENE_SOURCE_LEVEL ||
        cpu->read_word(ADDR_TOP_STATE) != TOP_REPLAY_RETURN)
        return;
    s_returning = 0;
    OPL_INFO_F("skip-to-cutscene", "%s: returned to Dragon Shores at "
               "frame=%u", s_row ? s_row->name : "?", openpete_frame);
}

static int on_draw(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!s_row || openpete_speculative) return 0;
    if (!s_induced) {
        if (dragon_shores_live(cpu)) cutscene_hook(cpu);
    } else if (s_returning) {
        watch_return(cpu);
    }
    return 0;
}

static const feature_dispatch_entry_t k_dispatch[] = {
    { ADDR_DRAW, on_draw },
    { 0, NULL },
};

const feature_module_t cutscene_skip_module = {
    .name           = "cutscene-skip",
    .apply_options  = NULL,
    .dispatch_table = k_dispatch,
};
