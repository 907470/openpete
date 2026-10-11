// games/spyro-3/features/skip_to_area.c - --skip-to-area <level>:<area>.
//
// Boots into a level's sub-area (the skate parks, the sub-levels behind a
// level's portals) the way the game enters one: --skip-to-level loads the
// level in area 0, then EnterSubArea func_80052918(area, P) is called in
// guest context through the transition queue (transition_call.c), where P is
// the moby a portal passes. A portal moby (class 1022 at moby+54; props =
// *(moby+0); props+64 the area it stands in, props+52 its destination,
// props+48 the index of its paired moby) calls EnterSubArea(props+52,
// *(0x8006C550) + props+48 * 88), the paired moby in the destination area,
// after its proximity test (func_level_11_8007A238, one copy per level
// overlay). The pause menu's return path finds the pair the same way,
// walking the moby table from *(0x8006C550) to *(0x8006C704) in 88-byte
// strides (func_8005783C). The portal's path from its entry to the call
// stores only its own stack frame, so the request carries no store.
//
// The portal moby's gate refuses game.state 1 (speech) and 15 (egg
// collect) and requires the area word to equal props+64, so the request's
// route is gameplay live (game.state 0, loadStage -1) in area 0 with
// isDemoMode clear, on the target level. A level whose first visit opens a
// speech (Frozen Altars) waits in game.state 1 until the speech ends.
//
// Release: game.state 13 (GAMESTATE_LOADING_SUBLEVEL, EnterSubArea's own
// store) seen after the call, then gameplay live with the area word
// 0x8006C5C8 == the target.

#include "skip_to_area.h"

#include "transition_call.h"
#include "openpete_fiber.h"          /* openpete_speculative */
#include "openpete_log.h"
#include "psx_runtime_internal.h"    /* CPUState + its read/write callbacks */
#include "spyro-3/skip_to_level.h"
#include "openpete_surface.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S3_GAME_STATE       0x8006E344u
#define S3_CURRENT_LEVEL    0x8006C5BCu
#define S3_AREA             0x8006C5C8u
#define S3_IS_DEMO_MODE     0x8006C658u
#define S3_MOBY_TABLE       0x8006C550u
#define S3_MOBY_TABLE_END   0x8006C704u
#define S3_GS_LOADING_SUBLEVEL 13u

#define MOBY_STRIDE         88u
#define MOBY_CLASS_OFF      54u
#define PORTAL_CLASS        1022
#define PROPS_PAIR_OFF      48u
#define PROPS_DEST_OFF      52u
#define PROPS_IN_AREA_OFF   64u

/* The levels with sub-areas and their area count (the level WAD header's
 * area table); every area 1..count-1 is entered by one area-0 portal. */
typedef struct { int level; int areas; } area_level_t;
static const area_level_t k_area_levels[] = {
    { 11, 3 }, { 12, 3 }, { 13, 3 }, { 14, 4 },
    { 21, 3 }, { 22, 3 }, { 23, 3 }, { 24, 3 },
    { 31, 3 }, { 32, 3 }, { 33, 4 }, { 34, 3 },
    { 41, 3 }, { 42, 3 }, { 43, 3 }, { 44, 4 },
    { 50, 4 },
};
#define AREA_LEVEL_COUNT (sizeof(k_area_levels) / sizeof(k_area_levels[0]))

/** @brief Returns a level's name from the level table, for messages.
 *
 * @param number The level number.
 * @return The table's name string, or "?" when the table has no such level. */
static const char* level_name(int number) {
    size_t n, i;
    const spyro3_level_t* t = spyro3_level_table(&n);
    for (i = 0; i < n; i++)
        if (t[i].number == number) return t[i].name;
    return "?";
}

/** @brief Prints every valid --skip-to-area target to stderr.
 *
 * One line per level in k_area_levels: the number, the name and the area
 * indices 1 up to the area count minus one. */
static void print_targets(void) {
    size_t i;
    fprintf(stderr, "valid --skip-to-area <level>:<area> targets (the level "
                    "by number or name):\n");
    for (i = 0; i < AREA_LEVEL_COUNT; i++) {
        int a;
        fprintf(stderr, "  %d %-18s", k_area_levels[i].level,
                level_name(k_area_levels[i].level));
        for (a = 1; a < k_area_levels[i].areas; a++) fprintf(stderr, " %d", a);
        fputc('\n', stderr);
    }
}

/** @brief Returns the number of areas of a level.
 *
 * @param level The level number.
 * @return The count from k_area_levels, or 1 (area 0 only) for a level not
 *     listed. */
static int area_count(int level) {
    size_t i;
    for (i = 0; i < AREA_LEVEL_COUNT; i++)
        if (k_area_levels[i].level == level) return k_area_levels[i].areas;
    return 1;
}

/** @brief Parses a non-negative decimal area index.
 *
 * The string must be non-empty and all decimal digits. The value is atoi's,
 * with no range check, so a digit string beyond int range converts as atoi
 * converts it.
 *
 * @param s The NUL-terminated text after the colon of the argument.
 * @return The value, or -1 for an empty or non-digit string. */
static int parse_index(const char* s) {
    const char* p = s;
    if (!*p) return -1;
    for (; *p; p++)
        if (!isdigit((unsigned char)*p)) return -1;
    return atoi(s);
}

void spyro3_area_surface(struct openpete_surface_sink* sink) {
    size_t i;
    char   slug[64];
    for (i = 0; i < AREA_LEVEL_COUNT; i++) {
        int a;
        for (a = 1; a < k_area_levels[i].areas; a++) {
            snprintf(slug, sizeof slug, "%s:%d", level_name(k_area_levels[i].level), a);
            openpete_surface_vocab(sink, "area", "--skip-to-area", slug);
        }
    }
}

int spyro3_area_resolve(const char* arg, int* out_area) {
    const char* colon = arg ? strchr(arg, ':') : NULL;
    char        lvl[40];
    size_t      n;
    int         level, area;

    if (!colon || (size_t)(colon - arg) >= sizeof(lvl)) {
        print_targets();
        return -1;
    }
    n = (size_t)(colon - arg);
    memcpy(lvl, arg, n);
    lvl[n] = '\0';
    level = spyro3_level_resolve(lvl);
    if (level < 0) return -1;
    area = parse_index(colon + 1);
    if (area < 1 || area >= area_count(level)) {
        fprintf(stderr, "level %d (%s) has no sub-area '%s'\n", level,
                level_name(level), colon + 1);
        print_targets();
        return -1;
    }
    *out_area = area;
    return level;
}

/* ---- state ----------------------------------------------------------- */

typedef enum {
    AREA_IDLE = 0,
    AREA_ARMED,     /* waiting for the level's first live tick in area 0 */
    AREA_QUEUED,    /* the EnterSubArea request waits in the transition queue */
    AREA_CALLED,    /* EnterSubArea ran; waiting for game.state 13 */
    AREA_LOADING,   /* game.state 13 seen; waiting for the sub-area */
    AREA_RELEASED,  /* live in the sub-area */
} area_phase_t;

static area_phase_t s_phase    = AREA_IDLE;
static int          s_level    = 0;
static int          s_area     = 0;
static int          s_reported = 0;

void spyro3_area_set(int level, int area) {
    s_level    = level;
    s_area     = area;
    s_reported = 0;
    s_phase    = level > 0 ? AREA_ARMED : AREA_IDLE;
    if (level > 0) spyro3_skip_set(level);
}

int  spyro3_area_watching(void) { return s_phase != AREA_IDLE && !s_reported; }
int  spyro3_area_pending(void)  { return s_phase != AREA_IDLE && s_phase != AREA_RELEASED; }
void spyro3_area_finish(void)   { s_reported = 1; }

int spyro3_area_released(CPUState* cpu, char* eng, size_t cap) {
    if (s_phase != AREA_RELEASED) return 0;
    snprintf(eng, cap, " level=%u area=%u",
             (unsigned)cpu->read_word(S3_CURRENT_LEVEL),
             (unsigned)cpu->read_word(S3_AREA));
    return 1;
}

/** @brief Tells the transition drain whether the target level is live in area 0.
 *
 * Passed as the request's route callback, and tested by spyro3_area_on_tick
 * before it enqueues. It requires gameplay live (game.state 0, loadStage -1),
 * isDemoMode 0, the area word 0x8006C5C8 0 and currentLevel equal to the
 * target. The drain calls it at Draw's entry, after the tick's Update; a 0
 * drops the request and runs area_dropped.
 *
 * @param cpu The guest CPU state.
 * @return 1 to make the EnterSubArea call, else 0. */
static int area_route(CPUState* cpu) {
    return spyro3_skip_gameplay_live(cpu) &&
           cpu->read_word(S3_IS_DEMO_MODE) == 0u &&
           cpu->read_word(S3_AREA) == 0u &&
           cpu->read_word(S3_CURRENT_LEVEL) == (uint32_t)s_level;
}

/** @brief Moves the module to AREA_CALLED after the drain has made the EnterSubArea call.
 *
 * Passed as the request's called callback.
 *
 * @param cpu Unused. */
static void area_called(CPUState* cpu)  { (void)cpu; s_phase = AREA_CALLED; }
/** @brief Returns the module to AREA_ARMED when the drain dropped the request.
 *
 * Passed as the request's dropped callback. spyro3_area_on_tick then enqueues
 * again once the route holds.
 *
 * @param cpu Unused. */
static void area_dropped(CPUState* cpu) { (void)cpu; s_phase = AREA_ARMED; }

/** @brief Finds the paired moby of the area-0 portal leading to a sub-area.
 *
 * Walks the moby table from *(0x8006C550) to *(0x8006C704) in 88-byte strides
 * for a class-1022 moby (the half at moby+54) whose props, *(moby+0), hold 0
 * at props+64 (the area it stands in) and `area` at props+52 (its
 * destination), as the pause menu's return path func_8005783C does.
 * @param cpu The guest CPU state.
 * @param area The destination sub-area index.
 * @return The table base plus props+48 times 88, or 0 when no such portal exists. */
static uint32_t find_portal_pair(CPUState* cpu, int area) {
    uint32_t base = cpu->read_word(S3_MOBY_TABLE);
    uint32_t end  = cpu->read_word(S3_MOBY_TABLE_END);
    uint32_t m;
    for (m = base; m < end; m += MOBY_STRIDE) {
        uint32_t props;
        if ((int16_t)cpu->read_half(m + MOBY_CLASS_OFF) != PORTAL_CLASS) continue;
        props = cpu->read_word(m);
        if (cpu->read_word(props + PROPS_IN_AREA_OFF) == 0u &&
            cpu->read_word(props + PROPS_DEST_OFF) == (uint32_t)area)
            return base + cpu->read_word(props + PROPS_PAIR_OFF) * MOBY_STRIDE;
    }
    return 0u;
}

/** @brief Queues EnterSubArea(area, paired portal moby) for the armed target through the transition queue.
 *
 * The second argument is find_portal_pair's result, and the request carries no
 * store. With no portal found the module logs a warning and goes to AREA_IDLE,
 * staying in area 0. The phase becomes AREA_QUEUED when the queue accepts the
 * request; when a request is already pending the phase stays AREA_ARMED and the
 * next tick tries again.
 *
 * @param cpu The guest CPU state. */
static void area_enqueue(CPUState* cpu) {
    s3_transition_request_t req = {
        .tag     = "skip-to-area",
        .entry   = S3_TRANSITION_AT_DRAW,
        .fn      = S3_FN_ENTER_SUB_AREA,
        .a0      = (uint32_t)s_area,
        .route   = area_route,
        .called  = area_called,
        .dropped = area_dropped,
    };
    req.a1 = find_portal_pair(cpu, s_area);
    if (!req.a1) {
        OPL_WARN_F("skip-to-area", "level %d has no area-0 portal to area %d; "
                   "staying in area 0", s_level, s_area);
        s_phase = AREA_IDLE;
        return;
    }
    if (s3_transition_enqueue(&req)) s_phase = AREA_QUEUED;
}

void spyro3_area_on_tick(CPUState* cpu, uint32_t frame_counter) {
    /* A speculative tick's guest state rolls back; the phase would not. */
    if (s_phase == AREA_IDLE || openpete_speculative) return;
    if (s_phase == AREA_ARMED) {
        if (spyro3_skip_level_picked() && area_route(cpu)) area_enqueue(cpu);
    } else if (s_phase == AREA_CALLED) {
        if (cpu->read_word(S3_GAME_STATE) == S3_GS_LOADING_SUBLEVEL)
            s_phase = AREA_LOADING;
    } else if (s_phase == AREA_LOADING) {
        if (spyro3_skip_gameplay_live(cpu) &&
            cpu->read_word(S3_AREA) == (uint32_t)s_area) {
            s_phase = AREA_RELEASED;
            fprintf(stderr, "[skip-to-area] released: level=%d area=%u at "
                            "frame=%u\n", s_level,
                    (unsigned)cpu->read_word(S3_AREA), (unsigned)frame_counter);
        }
    }
}
