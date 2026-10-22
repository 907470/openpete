// games/spyro-3/features/cutscene_skip.c - `--skip-to-cutscene` for Spyro 3.
//
// Spyro 3 starts every story cutscene through WarpToLevel func_800584BC(6,
// id), id 61..80 (72 unused): it stores game.state 5, loadStage 0 and
// currentLevel 0x8006C5BC = id, and the cutscene overlay then stores its
// index n at 0x8006EDD8 and game.state 6 while it plays. At its end a
// shared routine warps to previousLevel 0x8006FB95 when that byte is set,
// else on to the next cutscene of a chain (61 -> 75 -> 76, 69 -> 70 -> 71
// -> 73) or to a fixed level.
//
// Each row is reached by the call its game trigger makes, queued through
// the transition module (transition_call.c), which drains it at Draw
// func_8001E638 entry after the tick's whole Update, as the triggers call
// from inside Update:
//
// - 61 (the title's New Game): at the title's idle sub-state phase 1 (byte
//   0x8007F12C 1, byte 0x8007F12E 1, the level skip's induction gate) the
//   request makes New Game's commit (func_title_8007566C sub-state 20):
//   func_title_80079B94 in guest context, D_8006C534 = 1, WarpToLevel(6,
//   61). The natural path reaches sub-state 20 through sub-state 19, whose
//   phase 0 calls the card teardown func_title_8007C268 (VSyncCallbacks(7,
//   0), then CloseEvent on the four card events); the request calls it
//   first, as interrupt line 7 otherwise keeps the title-only callback
//   0x8007D5AC after the loading overlay replaces the title.
// - Level rows: --skip-to-level to the trigger's level, then on its first
//   live tick (loadStage -1, game.state 0, isDemoMode clear) the constant
//   stores the trigger's branch makes before its call, then WarpToLevel(6,
//   id). The homeworld portal mobys (63, 65, 67) and Scorch's (68) store the
//   words 0x80070534, 0x80070564, 0x8006E174, 0x8006E178, 0x8006E160,
//   D_8006C598 (the fade, 255 at the call), then, except Scorch, the half
//   0x8007015A (the gate value), and their seen byte; the Super Bonus Round
//   moby (74) ORs 0x10000002 into 0x80070534 and stores its seen byte; the
//   GoToLevel func_80053F50 branches (62, 64, 66) store the seen byte. The
//   moby rows 77..80 and 69 store nothing on the path. Stores the triggers
//   compute from state (the moby's own fields, Spyro's vectors, the camera)
//   are not made.
// - Chain rows (75, 76 from 61; 70, 71, 73 from 69): the head's route, then
//   the game's own chain plays through the preceding cutscenes.
//
// Preconditions read at the call: previousLevel 0 (a set byte would end the
// cutscene in a warp back to it instead of the chain), the row's seen byte
// 0 (its trigger's gate), and for 61 currentLevel 0 and n 0 (New Game's
// branch). The --skip-to-level reset leaves them clear; a set one refuses
// the row.
//
// Release: game.state 6 with n == the row's index, after the call. The
// module is inert until armed (the attract demo never arms it) and does
// nothing on a speculative (runahead) tick.

#include "cutscene_skip.h"

#include "transition_call.h"
#include "named_target.h"
#include "openpete_fiber.h"          /* openpete_speculative, openpete_frame */
#include "openpete_log.h"
#include "psx_runtime_internal.h"    /* CPUState + its read/write callbacks */
#include "spyro-3/skip_to_level.h"

#include <stdio.h>

#define S3_GAME_STATE      0x8006E344u
#define S3_CURRENT_LEVEL   0x8006C5BCu
#define S3_PREVIOUS_LEVEL  0x8006FB95u /* byte */
#define S3_CUTSCENE_N      0x8006EDD8u
#define S3_IS_DEMO_MODE    0x8006C658u
#define S3_TITLE_SUBSTATE  0x8007F12Cu /* byte; 1 press-start idle */
#define S3_TITLE_PHASE     0x8007F12Eu /* byte; the idle sub-state's phase */
#define S3_FN_TITLE_CUTSCENE_END 0x80079B94u /* func_title_80079B94 */
#define S3_FN_TITLE_CARD_CLOSE   0x8007C268u /* func_title_8007C268 */

#define S3_GS_CUTSCENE 6u
#define S3_GS_TITLE    11u
#define PTYPE_CUTSCENE 6u
#define TITLE_SOURCE   0

#define W32(a, v) { (a), 4u, 0u, (v) }
#define W16(a, v) { (a), 2u, 0u, (v) }
#define W8(a, v)  { (a), 1u, 0u, (v) }
#define OR32(a, v) { (a), 4u, 1u, (v) }

/* New Game's store between its guest call and WarpToLevel(6, 61). */
static const s3_transition_store_t k_st_61[] = { W32(0x8006C534u, 1u) };
/* Sub-state 19 phase 0's teardown, then New Game's guest call. */
static const uint32_t k_pre_61[] = { S3_FN_TITLE_CARD_CLOSE,
                                     S3_FN_TITLE_CUTSCENE_END };

/* The homeworld portal branch (func_level_10_8007F0BC state 6 and its level
 * 20 and 30 copies) and Scorch's (func_level_37_800818EC), in path order. */
#define PORTAL_STORES(w534)                                                 \
    W32(0x80070534u, (w534)), W32(0x80070564u, 0u), W32(0x8006E174u, 0u),  \
    W32(0x8006E178u, 0u), W32(0x8006E160u, 17u), W32(0x8006C598u, 255u)
static const s3_transition_store_t k_st_63[] = {
    PORTAL_STORES(0x10008000u), W16(0x8007015Au, 17u), W8(0x800715B4u, 1u) };
static const s3_transition_store_t k_st_65[] = {
    PORTAL_STORES(0x10008000u), W16(0x8007015Au, 27u), W8(0x800715B5u, 1u) };
static const s3_transition_store_t k_st_67[] = {
    PORTAL_STORES(0x1001A000u), W16(0x8007015Au, 37u), W8(0x800715B6u, 1u) };
static const s3_transition_store_t k_st_68[] = {
    PORTAL_STORES(0x1001A000u), W8(0x800715BAu, 1u) };
/* GoToLevel's cutscene branches. */
static const s3_transition_store_t k_st_62[] = { W8(0x800715B7u, 1u) };
static const s3_transition_store_t k_st_64[] = { W8(0x800715B8u, 1u) };
static const s3_transition_store_t k_st_66[] = { W8(0x800715B9u, 1u) };
/* The Super Bonus Round moby (func_level_50_8008B434) state 30. */
static const s3_transition_store_t k_st_74[] = {
    OR32(0x80070534u, 0x10000002u), W8(0x800715BBu, 1u) };

#define STORES(a) (a), (sizeof(a) / sizeof((a)[0]))

/* One row per cutscene. `name` is the game's loading caption (the table at
 * 0x80069E98, indexed by the caption WarpToLevel sets from id) in
 * lower_snake_case. `head` is the row whose route plays it: its own id, or
 * the chain's first cutscene. `source` is the trigger's level (0: the
 * title) and `seen` the byte its trigger gates on and sets (0: none). */
typedef struct {
    const char*                  name;
    int                          id;
    uint32_t                     n;
    int                          head;
    int                          source;
    uint32_t                     seen;
    const s3_transition_store_t* stores;
    size_t                       n_stores;
} cutscene_t;

static const cutscene_t k_cutscenes[] = {
    { "an_evil_plot_unfolds",           61,  1, 61, TITLE_SOURCE, 0u, STORES(k_st_61) },
    { "the_second_warning",             62,  7, 62, 10, 0x800715B7u, STORES(k_st_62) },
    { "bianca_strikes_back",            63,  4, 63, 10, 0x800715B4u, STORES(k_st_63) },
    { "hunters_tussle",                 64,  8, 64, 20, 0x800715B8u, STORES(k_st_64) },
    { "spike_is_born",                  65,  5, 65, 20, 0x800715B5u, STORES(k_st_65) },
    { "an_apology_and_lunch",           66,  9, 66, 30, 0x800715B9u, STORES(k_st_66) },
    { "a_monster_to_end_all_monsters",  67,  6, 67, 30, 0x800715B6u, STORES(k_st_67) },
    { "the_escape",                     68, 10, 68, 37, 0x800715BAu, STORES(k_st_68) },
    { "deja_vu",                        69, 15, 69, 47, 0u, NULL, 0 },
    { "a_familiar_face",                70, 16, 69, 47, 0u, NULL, 0 },
    { "billy_in_the_wall",              71, 17, 69, 47, 0u, NULL, 0 },
    { "one_less_noble_warrior",         73, 18, 69, 47, 0u, NULL, 0 },
    { "the_end",                        74, 19, 74, 50, 0x800715BBu, STORES(k_st_74) },
    { "a_powerful_villain_emerges",     75,  2, 61, TITLE_SOURCE, 0u, NULL, 0 },
    { "a_desperate_rescue_begins",      76,  3, 61, TITLE_SOURCE, 0u, NULL, 0 },
    { "no_hard_feelings",               77, 11, 77, 10, 0u, NULL, 0 },
    { "byrd_james_byrd",                78, 12, 78, 20, 0u, NULL, 0 },
    { "a_duplicitous_larcenous_ursine", 79, 13, 79, 30, 0u, NULL, 0 },
    { "the_dancing_bear",               80, 14, 80, 40, 0u, NULL, 0 },
};
#define CUTSCENE_COUNT (sizeof(k_cutscenes) / sizeof(k_cutscenes[0]))

/** @brief Finds the cutscene row with a given WarpToLevel id.
 *
 * @param id The cutscene id (61..80; 72 has no row).
 * @return The row of k_cutscenes, or NULL when no row has that id. */
static const cutscene_t* row_of(int id) {
    size_t i;
    for (i = 0; i < CUTSCENE_COUNT; i++)
        if (k_cutscenes[i].id == id) return &k_cutscenes[i];
    return NULL;
}

void spyro3_cutscene_surface(struct openpete_surface_sink* sink) {
    const named_target_table_t cutscenes =
        NAMED_TARGET_TABLE(k_cutscenes, cutscene_t, name, id);
    named_target_surface(sink, "cutscene", "--skip-to-cutscene", &cutscenes);
}

int spyro3_cutscene_resolve(const char* arg) {
    const named_target_table_t cutscenes =
        NAMED_TARGET_TABLE(k_cutscenes, cutscene_t, name, id);
    return named_target_resolve(arg, "cutscene", &cutscenes, NULL, 0u);
}

/* ---- state ----------------------------------------------------------- */

typedef enum {
    CUT_IDLE = 0,
    CUT_ARMED,   /* waiting for the route's tick */
    CUT_QUEUED,  /* the WarpToLevel request waits in the transition queue */
    CUT_CALLED,  /* WarpToLevel ran; the chain plays */
} cut_phase_t;

static cut_phase_t       s_phase    = CUT_IDLE;
static const cutscene_t* s_row      = NULL; /* the armed row */
static const cutscene_t* s_route    = NULL; /* the row whose route runs */
static int               s_watching = 0;
static uint32_t          s_last_n   = 0;

void spyro3_cutscene_arm(int id, int skip_level) {
    const cutscene_t* r = row_of(id);
    if (!r) return;
    const cutscene_t* h = row_of(r->head);
    if (skip_level > 0 && skip_level != h->source) {
        if (h->source == TITLE_SOURCE)
            fprintf(stderr, "openpete: --skip-to-cutscene: %s starts from the "
                            "title, not --skip-to-level %d; refused\n",
                    r->name, skip_level);
        else
            fprintf(stderr, "openpete: --skip-to-cutscene: %s starts from level "
                            "%d, not --skip-to-level %d; refused\n",
                    r->name, h->source, skip_level);
        return;
    }
    s_row      = r;
    s_route    = h;
    s_phase    = CUT_ARMED;
    s_watching = 1;
    s_last_n   = 0;
    if (h->source == TITLE_SOURCE)
        spyro3_skip_ffwd_arm("the cutscene plays");
    else if (skip_level <= 0)
        spyro3_skip_set(h->source);
}

int  spyro3_cutscene_watching(void) { return s_watching; }
void spyro3_cutscene_finish(void)   { s_watching = 0; }

int spyro3_cutscene_released(CPUState* cpu, char* eng, size_t cap) {
    uint32_t n;
    if (s_phase != CUT_CALLED) return 0;
    n = cpu->read_word(S3_CUTSCENE_N);
    if (cpu->read_word(S3_GAME_STATE) != S3_GS_CUTSCENE || n != s_row->n)
        return 0;
    snprintf(eng, cap, " cutscene=%s n=%u level=%u", s_row->name, (unsigned)n,
             (unsigned)cpu->read_word(S3_CURRENT_LEVEL));
    return 1;
}

/** @brief Reports whether this tick is the armed route's tick.
 *
 * For a title-source route: game.state 11 (title) with the idle sub-state byte
 * 0x8007F12C and its phase byte 0x8007F12E both 1. For a level route: the level
 * skip has picked its target, gameplay is live, and currentLevel 0x8006C5BC is
 * the route's source level. isDemoMode is tested by preconditions.
 * @param cpu The guest CPU state.
 * @return 1 on the route's tick, else 0. */
static int route_tick(CPUState* cpu) {
    if (s_route->source == TITLE_SOURCE)
        return cpu->read_word(S3_GAME_STATE) == S3_GS_TITLE &&
               cpu->read_byte(S3_TITLE_SUBSTATE) == 1u &&
               cpu->read_byte(S3_TITLE_PHASE) == 1u;
    return spyro3_skip_level_picked() && spyro3_skip_gameplay_live(cpu) &&
           cpu->read_word(S3_CURRENT_LEVEL) == (uint32_t)s_route->source;
}

/** @brief Reports whether the state the trigger's branch reads before its call admits the route.
 *
 * Requires previousLevel 0x8006FB95 0, isDemoMode 0x8006C658 0, the route's
 * seen byte 0 when it has one, and for a title-source route currentLevel and
 * the cutscene index 0x8006EDD8 both 0 (New Game's branch).
 * @param cpu The guest CPU state.
 * @return 1 when every precondition holds, else 0. */
static int preconditions(CPUState* cpu) {
    if (cpu->read_byte(S3_PREVIOUS_LEVEL) != 0u) return 0;
    if (cpu->read_word(S3_IS_DEMO_MODE) != 0u) return 0;
    if (s_route->seen && cpu->read_byte(s_route->seen) != 0u) return 0;
    if (s_route->source == TITLE_SOURCE)
        return cpu->read_word(S3_CURRENT_LEVEL) == 0u &&
               cpu->read_word(S3_CUTSCENE_N) == 0u;
    return 1;
}

/** @brief Tells the transition drain whether the armed cutscene route still applies.
 *
 * Passed as the request's route callback. The drain calls it at Draw's entry,
 * after the tick's Update; a 0 drops the request and runs cut_dropped. It is
 * route_tick and preconditions on the current guest state.
 *
 * @param cpu The guest CPU state.
 * @return 1 to make the WarpToLevel call, else 0. */
static int cut_route(CPUState* cpu) { return route_tick(cpu) && preconditions(cpu); }

/** @brief Moves the module to CUT_CALLED after the drain has made the WarpToLevel call.
 *
 * Passed as the request's called callback. Later ticks then log each cutscene
 * index the chain plays and test for the release.
 *
 * @param cpu Unused. */
static void cut_called(CPUState* cpu)  { (void)cpu; s_phase = CUT_CALLED; }
/** @brief Returns the module to CUT_ARMED when the drain dropped the request.
 *
 * Passed as the request's dropped callback. The route closed between the
 * enqueue and the drain, so spyro3_cutscene_on_tick tests the route again on
 * later ticks and enqueues again.
 *
 * @param cpu Unused. */
static void cut_dropped(CPUState* cpu) { (void)cpu; s_phase = CUT_ARMED; }

/** @brief Queues WarpToLevel(6, id) of the armed route through the transition queue.
 *
 * The request carries the route row's constant stores, and for id 61 the guest
 * calls k_pre_61 (the card teardown, then New Game's commit), which the drain
 * makes before the stores. The phase becomes CUT_QUEUED when the queue accepts
 * the request; when a request is already pending the phase stays CUT_ARMED and
 * the next tick tries again. Enqueueing writes no guest memory. */
static void cut_enqueue(void) {
    s3_transition_request_t req = {
        .tag      = "skip-to-cutscene",
        .entry    = S3_TRANSITION_AT_DRAW,
        .fn       = S3_FN_WARP_TO_LEVEL,
        .a0       = PTYPE_CUTSCENE,
        .a1       = (uint32_t)s_route->id,
        .stores   = s_route->stores,
        .n_stores = s_route->n_stores,
        .route    = cut_route,
        .called   = cut_called,
        .dropped  = cut_dropped,
    };
    if (s_route->id == 61) {
        req.pre_calls   = k_pre_61;
        req.n_pre_calls = sizeof(k_pre_61) / sizeof(k_pre_61[0]);
    }
    if (s3_transition_enqueue(&req)) s_phase = CUT_QUEUED;
}

/** @brief Logs each cutscene the chain plays, once when game.state 6 shows a new cutscene index.
 *
 * Reads guest RAM only.
 * @param cpu The guest CPU state.
 * @param frame_counter The openpete_frame counter printed on the line. */
static void log_playing(CPUState* cpu, uint32_t frame_counter) {
    uint32_t n;
    if (cpu->read_word(S3_GAME_STATE) != S3_GS_CUTSCENE) return;
    n = cpu->read_word(S3_CUTSCENE_N);
    if (n == s_last_n) return;
    s_last_n = n;
    OPL_INFO_F("skip-to-cutscene", "%s: cutscene n=%u (level %u) plays at "
               "frame=%u", s_row->name, (unsigned)n,
               (unsigned)cpu->read_word(S3_CURRENT_LEVEL),
               (unsigned)frame_counter);
}

void spyro3_cutscene_on_tick(CPUState* cpu, uint32_t frame_counter) {
    /* A speculative tick's guest state rolls back; the phase would not. */
    if (s_phase == CUT_IDLE || openpete_speculative) return;
    if (s_phase == CUT_ARMED) {
        if (!route_tick(cpu)) return;
        if (preconditions(cpu)) {
            cut_enqueue();
            return;
        }
        OPL_WARN_F("skip-to-cutscene", "%s: refused, previousLevel %u, "
                   "isDemoMode %u, seen byte %u, currentLevel %u, n %u",
                   s_row->name, (unsigned)cpu->read_byte(S3_PREVIOUS_LEVEL),
                   (unsigned)cpu->read_word(S3_IS_DEMO_MODE),
                   s_route->seen ? (unsigned)cpu->read_byte(s_route->seen) : 0u,
                   (unsigned)cpu->read_word(S3_CURRENT_LEVEL),
                   (unsigned)cpu->read_word(S3_CUTSCENE_N));
        s_phase = CUT_IDLE;
    } else if (s_phase == CUT_CALLED) {
        log_playing(cpu, frame_counter);
    }
}
