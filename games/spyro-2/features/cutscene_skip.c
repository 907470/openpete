// games/spyro-2/features/cutscene_skip.c - `--skip-to-cutscene` for Spyro 2.
//
// Two routes start a Spyro 2 cutscene, and every row of the table below
// takes one of them.
//
// Story cutscenes (the theater route). Spyro 2 starts every story cutscene
// through func_80019230(k) (0x80019230): it stores the top-level state
// D_800681C8 = 5, the loader word 0x80066EEC = D_80063250[k], D_800698F4 = k
// (k 7 becomes 9 while byte 0x8006B08D is set, k 11 becomes 13 while byte
// 0x8006B099 is set) and, for k != 0, g_NextLevelNum 0x80066F90 =
// D_8006323C[k]. State 5 then calls StartCutscene (0x80073E48), which stores
// D_800681C8 = 6 while the cutscene plays. The Dragon Shores (level 65)
// theater replays the eleven story cutscenes from inside a level: its moby
// code (func_level_65_8006FA78, 0x8006FB78) calls func_80019230(k) with k
// from its pair table at 0x8006DAFC, then stores g_NextLevelNum = the pair's
// pseudo-level, the loader word 0 (loader word 2 from inside a level would
// run the loader through addresses the resident level overlay owns), byte
// 0x800698BD = 1 and D_800698F4 = k. At the cutscene's end the title overlay
// reads byte 0x800698BD set, calls func_80019230(0) and loads level 65 again.
// A story row is a --skip-to-level 65 followed by the theater's call and
// stores. Precondition read at the call: bytes 0x8006B08D and 0x8006B099 are
// 0 (a set byte remaps k 7 or 11 to a level row); the --skip-to-level reset
// leaves them clear, and a set byte refuses the row.
//
// Level intro and outro cutscenes (the portal route). Leaving a level or a
// homeworld goes through func_80019928(r) (0x80019928): in a homeworld
// (D_80066EE4 set) the destination D_8006715C is byte +66 of portal record r
// of the table at *(0x80066F00) (0x44-byte records, their count in the word
// 0x80066FA8), in a level it is the level's homeworld D_80064B84[idx]; it
// stores the top-level state 7. State 7's transition machine
// (0x80071BD4) calls StartCutscene with slot 0 on arrival in a level whose
// visited byte 0x8006B084[idx] is 0, and with slot 1 on departure from a
// level while byte 0x80066FF4 is set; StartCutscene plays WAD entry
// D_80064914[idx] + slot and stores the slot at 0x80068C40 and the entry at
// 0x80068C3C. Each level load clears 0x80066FF4; the level's completion
// (0x80017034 for idx < 22, 0x80016D7C for idx >= 22) sets it with the
// completion byte 0x8006B264[idx], and for idx < 22 counts the byte's first
// set in 0x80067108.
// - An intro row for level L is a --skip-to-level to L's homeworld, then
//   func_80019928(r) with r the record whose byte +66 is L, as the
//   homeworld's portal moby calls it (0x8002AD9C). Nothing is stored.
// - An outro row for level L is a --skip-to-level L, then the completion
//   stores (0x80066FF4 = 1, 0x8006B264[idx] = 1 and, for idx < 22 with the
//   byte previously 0, 0x80067108 += 1), then func_80019928(0), the call the
//   level's own exit makes. The idx >= 22 completion runs under the test
//   0x80066E72[idx] & 0x80066FF0; the hook makes the stores without it and
//   logs the tested values.
//
// Every call is made from a dispatch hook on the draw function func_800156FC
// (0x800156FC) entry on the first tick of the source level's gameplay
// (D_800681C8 0). The game's own callers call from inside the update
// func_8001B140, so the whole update has run when they call; a call at the
// draw entry follows the tick's whole update, and the draw reads the call's
// stores as it does after the game's. The hook returns 0 so the draw runs.
// The loader-word-0 store (the theater's, or func_80019928's own) ends the
// level skip's demo-mode hold (skip_to_level.c), as on the game's own
// level-exit path.
//
// The hook is inert until armed (the attract demo never arms it) and does
// nothing on a speculative (runahead) tick: a request acted on there is
// rolled back with RAM while the host latch would read it as done.

#include "features/cutscene_skip.h"

#include "feature_registry.h"
#include "named_target.h"
#include "openpete_fiber.h" /* openpete_speculative, openpete_frame */
#include "openpete_log.h"
#include "openpete_psxtrace.h" /* openpete_psxtrace_gate_host_enter: a host-initiated guest call */
#include "psx_runtime_internal.h" /* CPUState + its read/write callbacks */

#include <stdint.h>
#include <stdio.h>

#define ADDR_DRAW             0x800156FCu /* func_800156FC, the per-tick draw */
#define ADDR_CUTSCENE_START   0x80019230u /* func_80019230(k) */
#define ADDR_LEAVE_LEVEL      0x80019928u /* func_80019928(r) */

#define ADDR_TOP_STATE        0x800681C8u /* 0 gameplay, 5 cutscene start,
                                             6 playback, 4 from a replay's
                                             return to level 65, 7 level
                                             transition */
#define ADDR_LOADER_STATE     0x80066EECu /* -1 level resident, 0 title */
#define ADDR_NEXT_LEVEL_NUM   0x80066F90u
#define ADDR_LEVEL_TICK       0x80067008u
#define ADDR_DEMO_MODE        0x80067024u
#define ADDR_CUTSCENE_K       0x800698F4u /* D_800698F4 */
#define ADDR_RETURN_TO_65     0x800698BDu /* byte: replay returns to level 65 */
#define ADDR_REMAP_K7         0x8006B08Du /* byte: k 7 -> 9 */
#define ADDR_REMAP_K11        0x8006B099u /* byte: k 11 -> 13 */

#define ADDR_LEVEL_INDEX      0x80066F54u /* g_LevelIndex, idx 0..28 */
#define ADDR_DESTINATION      0x8006715Cu /* D_8006715C, the level to load */
#define ADDR_PORTAL_TABLE     0x80066F00u /* word: first 0x44-byte record */
#define ADDR_PORTAL_COUNT     0x80066FA8u /* word: record count */
#define PORTAL_STRIDE         0x44u
#define PORTAL_DEST           66u         /* byte: destination level */
#define ADDR_VISITED          0x8006B084u /* bytes [idx] */
#define ADDR_COMPLETE         0x8006B264u /* bytes [idx] */
#define ADDR_COMPLETE_COUNT   0x80067108u /* word */
#define ADDR_OUTRO_PENDING    0x80066FF4u /* byte, cleared by each load */
#define ADDR_COMPLETE_MASK    0x80066FF0u /* byte, idx >= 22 test */
#define ADDR_MASK_TABLE       0x80066E72u /* bytes [idx], idx >= 22 test */
#define ADDR_SLOT             0x80068C40u /* StartCutscene's slot */
#define ADDR_ENTRY            0x80068C3Cu /* StartCutscene's WAD entry */
#define COUNTED_IDX_LIMIT     22

#define TOP_GAMEPLAY    0u
#define TOP_REPLAY_RETURN 4u
#define TOP_PLAYBACK    6u
#define LOADER_RESIDENT 0xFFFFFFFFu
#define THEATER_LEVEL   65u

void call_by_address(CPUState* cpu, uint32_t addr); /* psx_runtime.h */

typedef enum { ROUTE_THEATER, ROUTE_INTRO, ROUTE_OUTRO } route_t;

/* One row per cutscene. Story rows (the theater route): id is k
 * (func_80019230's argument) and level the pseudo-level the theater's pair
 * table gives it (D_8006323C[k]); k 3, 5, 9 and 13 are level rows of that
 * table, and k 0 is the return from a replay. Level rows (the portal route):
 * level is L, idx its g_LevelIndex, entry the WAD entry StartCutscene plays
 * (D_80064914[idx], + 1 for the outro); ids from 20. source is the
 * --skip-to-level the row rides. */
typedef struct {
    const char* name;
    int         id;
    route_t     route;
    uint32_t    level;
    uint32_t    source;
    uint32_t    idx;
    uint32_t    entry;
} cutscene_t;

#define STORY(name, k, pseudo) \
    { name, k, ROUTE_THEATER, pseudo, THEATER_LEVEL, 0u, 0u }
#define LEVEL_PAIR(stem, id, level, idx, home, entry)                    \
    { stem "_intro", id, ROUTE_INTRO, level, home, idx, entry },          \
    { stem "_outro", (id) + 1, ROUTE_OUTRO, level, level, idx, (entry) + 1 }

static const cutscene_t k_cutscenes[] = {
    STORY("intro",          1, 70u), /* from the title's New Game */
    STORY("intro_arrival",  2, 71u), /* chains from k 1, then Glimmer */
    STORY("summer_forest",  4, 72u), /* leaving Glimmer, Summer Forest unvisited */
    STORY("crush_intro",    6, 73u), /* entering Crush's Dungeon, unvisited */
    STORY("crush_defeated", 7, 74u), /* from Crush's Dungeon (level 26) */
    STORY("autumn_plains",  8, 75u), /* chains from k 7, then Autumn Plains */
    STORY("gulp_intro",    10, 76u), /* entering Gulp's Overlook, unvisited */
    STORY("gulp_defeated", 11, 77u), /* from Gulp's Overlook (level 46) */
    STORY("winter_tundra", 12, 78u), /* chains from k 11, then Winter Tundra */
    STORY("ripto_intro",   14, 79u), /* from the homeworld code (level 50) */
    STORY("ending",        15, 80u), /* from Ripto's Arena (level 66) */
    /*          stem               id  L    idx  home entry */
    LEVEL_PAIR("idol_springs",    20, 12u,  2u, 10u,  2u),
    LEVEL_PAIR("colossus",        22, 13u,  3u, 10u,  4u),
    LEVEL_PAIR("hurricos",        24, 21u,  4u, 10u,  6u),
    LEVEL_PAIR("aquaria_towers",  26, 22u,  5u, 10u,  8u),
    LEVEL_PAIR("sunny_beach",     28, 23u,  6u, 10u, 10u),
    LEVEL_PAIR("skelos_badlands", 30, 31u, 10u, 30u, 12u),
    LEVEL_PAIR("crystal_glacier", 32, 32u, 11u, 30u, 14u),
    LEVEL_PAIR("breeze_harbor",   34, 33u, 12u, 30u, 16u),
    LEVEL_PAIR("zephyr",          36, 34u, 13u, 30u, 18u),
    LEVEL_PAIR("scorch",          38, 41u, 15u, 30u, 20u),
    LEVEL_PAIR("shady_oasis",     40, 42u, 16u, 30u, 22u),
    LEVEL_PAIR("magma_cone",      42, 43u, 17u, 30u, 24u),
    LEVEL_PAIR("fracture_hills",  44, 44u, 18u, 30u, 26u),
    LEVEL_PAIR("mystic_marsh",    46, 51u, 22u, 50u, 28u),
    LEVEL_PAIR("cloud_temples",   48, 52u, 23u, 50u, 30u),
    LEVEL_PAIR("robotica_farms",  50, 61u, 25u, 50u, 32u),
    LEVEL_PAIR("metropolis",      52, 62u, 26u, 50u, 34u),
};

/** @brief Finds the cutscene table row with the given id.
 *
 * @param id The row id, a story cutscene's k or a level row's id.
 * @return The first row with that id, or NULL. */
static const cutscene_t* row_of(int id) {
    for (size_t i = 0; i < sizeof(k_cutscenes) / sizeof(k_cutscenes[0]); i++)
        if (k_cutscenes[i].id == id) return &k_cutscenes[i];
    return NULL;
}

void spyro2_cutscene_surface(struct openpete_surface_sink* sink) {
    const named_target_table_t cutscenes =
        NAMED_TARGET_TABLE(k_cutscenes, cutscene_t, name, id);
    named_target_surface(sink, "cutscene", "--skip-to-cutscene", &cutscenes);
}

int spyro2_cutscene_resolve(const char* arg) {
    const named_target_table_t cutscenes =
        NAMED_TARGET_TABLE(k_cutscenes, cutscene_t, name, id);
    return named_target_resolve(arg, "cutscene", &cutscenes, NULL, 0u);
}

int spyro2_cutscene_source_level(int id) {
    const cutscene_t* r = row_of(id);
    return r ? (int)r->source : -1;
}

static const cutscene_t* s_row       = NULL; /* armed row, NULL when idle */
static int               s_induced   = 0;    /* the hook has called */
static int               s_watching  = 0;
static int               s_returning = 0;    /* released; waiting for 65 */

void spyro2_cutscene_skip_arm(int id) {
    s_row       = row_of(id);
    s_induced   = 0;
    s_returning = 0;
    s_watching  = s_row != NULL;
}

int  spyro2_cutscene_skip_watching(void) { return s_watching; }
void spyro2_cutscene_skip_finish(void)   { s_watching = 0; }

/* A story row plays with D_800698F4 == k; a level row with StartCutscene's
 * slot and entry words equal to the row's. See cutscene_skip.h. */
int spyro2_cutscene_skip_released(CPUState* cpu, char* eng, size_t cap) {
    if (!s_row || !s_induced) return 0;
    uint32_t top = cpu->read_word(ADDR_TOP_STATE);
    if (top != TOP_PLAYBACK) return 0;
    uint32_t next = cpu->read_word(ADDR_NEXT_LEVEL_NUM);
    if (s_row->route == ROUTE_THEATER) {
        uint32_t k = cpu->read_word(ADDR_CUTSCENE_K);
        if (k != (uint32_t)s_row->id) return 0;
        snprintf(eng, cap, " cutscene=%s k=%u D_800681C8=%u next_level=%u",
                 s_row->name, (unsigned)k, (unsigned)top, (unsigned)next);
        s_returning = 1;
        return 1;
    }
    uint32_t slot  = cpu->read_word(ADDR_SLOT);
    uint32_t entry = cpu->read_word(ADDR_ENTRY);
    uint32_t want  = s_row->route == ROUTE_OUTRO ? 1u : 0u;
    if (slot != want || entry != s_row->entry) return 0;
    snprintf(eng, cap, " cutscene=%s slot=%u entry=%u level=%u "
             "D_800681C8=%u", s_row->name, (unsigned)slot, (unsigned)entry,
             (unsigned)next, (unsigned)top);
    return 1;
}

/** @brief Tests for a tick of the source level's gameplay, the tick the skip hook acts on.
 *
 * True when the level is resident (loader word -1) with a nonzero level
 * tick, g_NextLevelNum is the level, the top-level state D_800681C8 is 0
 * (the fade-in's 3, which also passes the level skip's live test, is
 * excluded) and the demo flag is clear.
 * @param cpu The guest CPU state.
 * @param level The source level.
 * @return 1 when live, else 0. */
static int source_live(CPUState* cpu, uint32_t level) {
    return cpu->read_word(ADDR_LOADER_STATE) == LOADER_RESIDENT &&
           cpu->read_word(ADDR_LEVEL_TICK) != 0u &&
           cpu->read_word(ADDR_NEXT_LEVEL_NUM) == level &&
           cpu->read_word(ADDR_TOP_STATE) == TOP_GAMEPLAY &&
           cpu->read_word(ADDR_DEMO_MODE) == 0u;
}

/** @brief Logs a refusal of the armed row and drops the arm and the watch.
 *
 * @param name The row's name.
 * @param why The reason, logged verbatim. */
static void refuse(const char* name, const char* why) {
    OPL_INFO_F("skip-to-cutscene", "%s: refused, %s", name, why);
    s_row = NULL;
    s_watching = 0;
}

/** @brief Calls a guest function with one argument from the draw function's entry hook.
 *
 * The draw function takes no arguments and this is its call boundary, so
 * only ra is preserved across the nested call.
 * @param cpu The guest CPU state.
 * @param addr The guest function's address.
 * @param a0 The argument. */
static void guest_call(CPUState* cpu, uint32_t addr, uint32_t a0) {
    uint32_t ra = cpu->ra;
    cpu->a0 = a0;
    openpete_psxtrace_gate_host_enter();
    call_by_address(cpu, addr);
    openpete_psxtrace_gate_host_leave();
    cpu->ra = ra;
}

/** @brief Makes the Dragon Shores theater's func_80019230(k) call and its four stores for a story row.
 *
 * Stores g_NextLevelNum = the row's pseudo-level, loader word 0, byte
 * 0x800698BD = 1 and D_800698F4 = k. Refuses the row when byte 0x8006B08D
 * or 0x8006B099 is set, since either remaps k 7 or 11 to a level row.
 * @param cpu The guest CPU state.
 * @param r The armed row. */
static void theater_hook(CPUState* cpu, const cutscene_t* r) {
    if (cpu->read_byte(ADDR_REMAP_K7) || cpu->read_byte(ADDR_REMAP_K11)) {
        refuse(r->name, "remap byte 0x8006B08D or 0x8006B099 set");
        return;
    }
    guest_call(cpu, ADDR_CUTSCENE_START, (uint32_t)r->id);
    cpu->write_word(ADDR_NEXT_LEVEL_NUM, r->level);
    cpu->write_word(ADDR_LOADER_STATE, 0u);
    cpu->write_byte(ADDR_RETURN_TO_65, 1u);
    cpu->write_word(ADDR_CUTSCENE_K, (uint32_t)r->id);
    OPL_INFO_F("skip-to-cutscene", "%s: func_80019230(%d) from Dragon Shores "
               "at frame=%u (D_800681C8 %u, D_800698F4 %u, next level %u)",
               r->name, r->id, openpete_frame,
               (unsigned)cpu->read_word(ADDR_TOP_STATE),
               (unsigned)cpu->read_word(ADDR_CUTSCENE_K),
               (unsigned)cpu->read_word(ADDR_NEXT_LEVEL_NUM));
}

/** @brief Finds the homeworld portal record whose destination byte (+66) is the level.
 *
 * Scans the 0x44-byte records at *(0x80066F00), their count at 0x80066FA8.
 * @param cpu The guest CPU state.
 * @param level The destination level.
 * @return The first matching record index, or -1. */
static int portal_to(CPUState* cpu, uint32_t level) {
    uint32_t table = cpu->read_word(ADDR_PORTAL_TABLE);
    int32_t  count = (int32_t)cpu->read_word(ADDR_PORTAL_COUNT);
    for (int32_t i = 0; i < count; i++)
        if (cpu->read_byte(table + (uint32_t)i * PORTAL_STRIDE + PORTAL_DEST) ==
            level)
            return (int)i;
    return -1;
}

/** @brief Makes the homeworld portal's func_80019928(r) call toward a level intro row's level, with no store.
 *
 * Refuses the row when the level's visited byte 0x8006B084[idx] is set,
 * since the intro plays only on the first arrival, or when no portal
 * record leads to the level.
 * @param cpu The guest CPU state.
 * @param r The armed row. */
static void intro_hook(CPUState* cpu, const cutscene_t* r) {
    if (cpu->read_byte(ADDR_VISITED + r->idx)) {
        refuse(r->name, "visited byte 0x8006B084[idx] set");
        return;
    }
    int rec = portal_to(cpu, r->level);
    if (rec < 0) {
        refuse(r->name, "no portal record leads to the level");
        return;
    }
    guest_call(cpu, ADDR_LEAVE_LEVEL, (uint32_t)rec);
    OPL_INFO_F("skip-to-cutscene", "%s: func_80019928(%d) from level %u at "
               "frame=%u (D_800681C8 %u, D_8006715C %u)", r->name, rec,
               (unsigned)r->source, openpete_frame,
               (unsigned)cpu->read_word(ADDR_TOP_STATE),
               (unsigned)cpu->read_word(ADDR_DESTINATION));
}

/** @brief Makes a level outro row's completion stores, then the level's exit call func_80019928(0).
 *
 * Stores byte 0x80066FF4 = 1 and, when the completion byte
 * 0x8006B264[idx] is 0, sets it and for idx below 22 increments
 * 0x80067108. The idx >= 22 mask test the game's completion runs under is
 * not applied; its value is logged. Refuses the row when g_LevelIndex is
 * not the row's.
 * @param cpu The guest CPU state.
 * @param r The armed row. */
static void outro_hook(CPUState* cpu, const cutscene_t* r) {
    if (cpu->read_word(ADDR_LEVEL_INDEX) != r->idx) {
        refuse(r->name, "g_LevelIndex is not the row's");
        return;
    }
    unsigned mask = cpu->read_byte(ADDR_MASK_TABLE + r->idx) &
                    cpu->read_byte(ADDR_COMPLETE_MASK);
    cpu->write_byte(ADDR_OUTRO_PENDING, 1u);
    if (!cpu->read_byte(ADDR_COMPLETE + r->idx)) {
        cpu->write_byte(ADDR_COMPLETE + r->idx, 1u);
        if (r->idx < COUNTED_IDX_LIMIT)
            cpu->write_word(ADDR_COMPLETE_COUNT,
                            cpu->read_word(ADDR_COMPLETE_COUNT) + 1u);
    }
    guest_call(cpu, ADDR_LEAVE_LEVEL, 0u);
    OPL_INFO_F("skip-to-cutscene", "%s: completion stores, func_80019928(0) "
               "at frame=%u (D_800681C8 %u, D_8006715C %u, 0x80067108 %u, "
               "idx>=22 mask test %u)", r->name, openpete_frame,
               (unsigned)cpu->read_word(ADDR_TOP_STATE),
               (unsigned)cpu->read_word(ADDR_DESTINATION),
               (unsigned)cpu->read_word(ADDR_COMPLETE_COUNT), mask);
}

/** @brief Logs a story row's return to Dragon Shores after its release and ends the return watch.
 *
 * The return is level 65 resident again with D_800681C8 4. Reads guest RAM
 * only.
 * @param cpu The guest CPU state. */
static void watch_return(CPUState* cpu) {
    if (cpu->read_word(ADDR_LOADER_STATE) != LOADER_RESIDENT ||
        cpu->read_word(ADDR_NEXT_LEVEL_NUM) != THEATER_LEVEL ||
        cpu->read_word(ADDR_TOP_STATE) != TOP_REPLAY_RETURN)
        return;
    s_returning = 0;
    OPL_INFO_F("skip-to-cutscene", "%s: returned to Dragon Shores at "
               "frame=%u", s_row ? s_row->name : "?", openpete_frame);
}

static void (*const k_hooks[])(CPUState*, const cutscene_t*) = {
    [ROUTE_THEATER] = theater_hook,
    [ROUTE_INTRO]   = intro_hook,
    [ROUTE_OUTRO]   = outro_hook,
};

/** @brief Draw-entry hook that runs the armed row's call once on the source level's first live tick.
 *
 * Does nothing when no row is armed or on a speculative tick. Until the
 * hook has called, a tick passing source_live runs the row's route hook;
 * after a story row's release, each tick checks for the return to Dragon
 * Shores.
 * @param cpu The guest CPU state at the draw function's entry.
 * @param addr Unused.
 * @return 0, so the draw runs. */
static int on_draw(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!s_row || openpete_speculative) return 0;
    if (!s_induced) {
        if (source_live(cpu, s_row->source)) {
            s_induced = 1;
            k_hooks[s_row->route](cpu, s_row);
        }
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
