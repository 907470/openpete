// games/spyro-3/features/areawarp.c - move between a level's areas from the
// dev overlay (M key).
//
// A Spyro 3 level with sub-areas (the levels of k_area_levels in
// skip_to_area.c) holds them all in one WAD; the area word 0x8006C5C8 names
// the one loaded. The game changes area through EnterSubArea func_80052918
// (area, P): it copies the pair moby P's position (P+0xC) into Spyro's and
// the camera's, stores the area word = area (0x80052994) and game.state 13
// (GAMESTATE_LOADING_SUBLEVEL, 0x80052A30), and the sub-level loader then
// returns to gameplay in the new area. Every caller passes the pair of a
// portal moby (class 1022 at moby+54; props = *(moby+0); props+64 the area
// it stands in, props+52 its destination, props+48 its pair's index):
//   - each level overlay's portal moby, on its proximity test
//     (func_level_11_8007A238, calls at 0x8007A2EC and 0x8007A4B0):
//     EnterSubArea(props+52, *(0x8006C550) + props+48 * 88);
//   - the pause menu's Exit Area (func_80057834 at 0x80058254): the portal
//     with props+64 == area and props+52 == 0, EnterSubArea(0, its pair)
//     (0x800582E0);
//   - the fade-in's re-entry after a level load (func_80053A10 at
//     0x80053CE8): the area-0 portal with props+52 == the byte 0x8006FBA6,
//     EnterSubArea(props+52, its pair) (0x80053EA8).
// No sub-area portal leads to another sub-area, so a warp between two
// sub-areas goes by way of area 0, as play does.
//
// The dropdown lists every sub-area of every level in skip_to_area.c's table,
// "<level name> - Area N". From another level the warp first loads the level
// in area 0 with the level warp's own request (WarpToLevel, levelwarp.c), and
// once that level is live enters the area as above. From the title screen it
// arms --skip-to-area, which does the same at boot.
//
// The warp makes the portal's call through the transition queue
// (transition_call.c), at Draw func_8001E638 entry after the tick's whole
// Update, as the portal moby calls from inside Update. The portal's path
// from its entry to the call stores only its own stack frame, so the request
// carries no store. The route is the portal moby's own gate: gameplay live
// (game.state 0, loadStage -1; the gate refuses game.state 1 speech and 15
// egg collect), isDemoMode clear, and the area word equal to the portal's
// props+64.
//
// The rows, the click queue and the in-flight phases are the shared warp
// (games/common/features/levelwarp_ui.c). Intentional, opt-in gameplay
// divergence like the level warp: unclicked, this module writes zero guest
// words.

#include "areawarp.h"

#include "features/levelwarp_ui.h"
#include "skip_to_area.h"
#include "transition_call.h"
#include "openpete_log.h"
#include "psx_runtime_internal.h"   /* CPUState + its read callbacks */
#include "levelwarp.h"
#include "spyro-3/skip_to_level.h"

#include <stdio.h>

#define S3_GAME_STATE       0x8006E344u
#define S3_CURRENT_LEVEL    0x8006C5BCu
#define S3_AREA             0x8006C5C8u
#define S3_IS_DEMO_MODE     0x8006C658u
#define S3_GS_TITLE         11u

/* A dropdown entry's number packs the level and the area: level * 16 +
 * area. Every area count of the sub-area table is below 16. */
#define AREA_KEY(level, area) ((level) * 16 + (area))
#define KEY_LEVEL(key)        ((key) / 16)
#define KEY_AREA(key)         ((key) % 16)

/* The entries, built at registration from the sub-area table and the level
 * table's display names. The game holds no area name (its strings name
 * levels, eggs and the pause rows), so the areas are numbered. */
#define AREA_ENTRIES_MAX 64
static game_level_t k_areas[AREA_ENTRIES_MAX];
static char         s_area_slug[AREA_ENTRIES_MAX][32];
static char         s_area_display[AREA_ENTRIES_MAX][48];

static game_level_table_t k_area_table = {
    k_areas, 0, NULL, 0, "area", "OPENPETE_AREAWARP_TEST", 0u,
};

static const knob_desc_t k_area_rows[2] = {
    { .id = "warp_area", .label = "Area warp", .section = "Gameplay",
      .help = "Destination for the area Warp button: a sub-area of a Spyro 3 "
              "level. Follows the sub-area you are in until you pick "
              "something.",
      .type = KNOB_ENUM, .mut = KNOB_LIVE,
      .default_note = "the sub-area the player is in" },
    { .id = "warp_area_go", .label = "Warp to area", .section = "Gameplay",
      .help = "Enter the selected sub-area the way its portal does, keeping "
              "progress. From another level the warp loads that level first, "
              "as the level warp does; from another sub-area of the same "
              "level it passes through the main area. From the title screen "
              "it starts a fresh session there, as --skip-to-area does.",
      .type = KNOB_ACTION, .mut = KNOB_LIVE, .action_label = "Warp" },
};

static levelwarp_t s_warp;
static int         s_level = 0;  /* the level the warp ends in */
static int         s_from  = 0;  /* the area the queued area leg leaves */
static int         s_final = 0;  /* the area the warp ends in */

/** @brief Fill the dropdown's entries from the sub-area table, one per sub-area.
 *
 * The slug is "<level name>:<area>", the --skip-to-area spelling; the
 * display is "<level display name> - Area <area>". */
static void build_entries(void) {
    size_t n = 0;
    int    areas = 0;
    for (size_t i = 0; spyro3_area_level_at(i, &areas) >= 0; i++) {
        const int level = spyro3_area_level_at(i, NULL);
        const int li    = game_level_index(&spyro3_levels, level);
        for (int a = 1; a < areas && n < AREA_ENTRIES_MAX; a++, n++) {
            snprintf(s_area_slug[n], sizeof s_area_slug[n], "%s:%d",
                     li >= 0 ? spyro3_levels.levels[li].name : "?", a);
            snprintf(s_area_display[n], sizeof s_area_display[n], "%s - Area %d",
                     li >= 0 ? spyro3_levels.levels[li].display : "?", a);
            k_areas[n].name    = s_area_slug[n];
            k_areas[n].display = s_area_display[n];
            k_areas[n].number  = AREA_KEY(level, a);
        }
    }
    k_area_table.count = n;
}

/** @brief Read the entry number the player is at: the current level and area word packed by AREA_KEY.
 *
 * @param cpu The guest CPU state.
 * @return AREA_KEY(currentLevel, area word). */
static uint32_t area_current(CPUState* cpu) {
    return (uint32_t)AREA_KEY((int)cpu->read_word(S3_CURRENT_LEVEL),
                              (int)cpu->read_word(S3_AREA));
}

/** @brief Report why an entry cannot be warped to from the current state.
 *
 * @param cpu The guest CPU state.
 * @param target The entry number.
 * @return "already here" in that level and area, else NULL. */
static const char* area_refused(CPUState* cpu, int target) {
    if ((uint32_t)target == area_current(cpu)) return "already here";
    return NULL;
}

/** @brief Tell the transition drain whether the queued leg's portal gate still holds.
 *
 * @param cpu The guest CPU state.
 * @return 1 when gameplay is live, isDemoMode is 0, the level is the
 *         request's and the area word is the leg's source, else 0. */
static int leg_route(CPUState* cpu) {
    return spyro3_skip_gameplay_live(cpu) &&
           cpu->read_word(S3_IS_DEMO_MODE) == 0u &&
           cpu->read_word(S3_CURRENT_LEVEL) == (uint32_t)s_level &&
           cpu->read_word(S3_AREA) == (uint32_t)s_from;
}

/** @brief Move the warp to LEVELWARP_LEAVING after the drain made the EnterSubArea call.
 *
 * @param cpu Unused. */
static void leg_called(CPUState* cpu) { (void)cpu; levelwarp_called(&s_warp); }

/** @brief End the warp when the drain dropped the leg.
 *
 * @param cpu Unused. */
static void leg_dropped(CPUState* cpu) { (void)cpu; levelwarp_reset(&s_warp); }

/** @brief Queue EnterSubArea(leg, pair) for one portal crossing from the current area.
 *
 * The leg is the target from area 0, else area 0. The pair is the paired
 * moby of the portal standing in the current area and leading to the leg.
 * @param w The warp; its target becomes the leg.
 * @param cpu The guest CPU state.
 * @return LEVELWARP_QUEUED, or LEVELWARP_IDLE when the level has no such
 *         portal or a transition is already queued. */
static int queue_leg(levelwarp_t* w, CPUState* cpu) {
    const int from = (int)cpu->read_word(S3_AREA);
    const int leg  = from == 0 ? s_final : 0;
    s3_transition_request_t req = {
        .tag     = "areawarp",
        .entry   = S3_TRANSITION_AT_DRAW,
        .fn      = S3_FN_ENTER_SUB_AREA,
        .a0      = (uint32_t)leg,
        .a1      = spyro3_area_portal_pair(cpu, from, leg),
        .route   = leg_route,
        .called  = leg_called,
        .dropped = leg_dropped,
    };
    if (!req.a1) {
        OPL_WARN_F("areawarp", "level %d has no portal from area %d to area %d",
                   s_level, from, leg);
        return LEVELWARP_IDLE;
    }
    s_from = from;
    if (!s3_transition_enqueue(&req)) {
        OPL_INFO_F("areawarp", "%s", "ignored (a transition is already queued)");
        return LEVELWARP_IDLE;
    }
    w->target = (uint32_t)AREA_KEY(s_level, leg);
    OPL_INFO_F("areawarp", "queued: level %d area %d -> area %d (warp to area %d)",
               s_level, from, leg, s_final);
    return LEVELWARP_QUEUED;
}

/** @brief Start an area warp: record the level and area, then queue the level leg or the first area leg.
 *
 * In another level the first leg is the level warp's WarpToLevel request to
 * the target level, landing in area 0; in the target level it is queue_leg.
 * @param w The warp.
 * @param cpu The guest CPU state.
 * @param target The entry number.
 * @return LEVELWARP_QUEUED, or LEVELWARP_IDLE when the request is ignored. */
static int area_start(levelwarp_t* w, CPUState* cpu, int target) {
    s_level = KEY_LEVEL(target);
    s_final = KEY_AREA(target);
    if (cpu->read_word(S3_CURRENT_LEVEL) == (uint32_t)s_level)
        return queue_leg(w, cpu);
    if (!spyro3_levelwarp_enqueue("areawarp", s_level, leg_called, leg_dropped)) {
        OPL_INFO_F("areawarp", "%s", "ignored (a transition is already queued)");
        return LEVELWARP_IDLE;
    }
    w->target = (uint32_t)AREA_KEY(s_level, 0);
    OPL_INFO_F("areawarp", "queued: level %u -> level %d (warp to area %d)",
               (unsigned)cpu->read_word(S3_CURRENT_LEVEL), s_level, s_final);
    return LEVELWARP_QUEUED;
}

/** @brief Queue the area leg after a leg's arrival in the target level's area 0.
 *
 * @param w The warp.
 * @param cpu The guest CPU state.
 * @param landed The entry number read on arrival.
 * @return LEVELWARP_QUEUED for the leg to the final area, else LEVELWARP_IDLE. */
static int area_arrival(levelwarp_t* w, CPUState* cpu, uint32_t landed) {
    if (landed != (uint32_t)AREA_KEY(s_level, 0) || s_final == 0) return LEVELWARP_IDLE;
    return queue_leg(w, cpu);
}

/** @brief Arm --skip-to-area for an entry chosen at the title screen.
 *
 * @param target The entry number. */
static void area_title_start(int target) {
    spyro3_area_set(KEY_LEVEL(target), KEY_AREA(target));
}

static const levelwarp_game_t k_area_game = {
    .log_tag          = "areawarp",
    .rows             = k_area_rows,
    .test_env         = "OPENPETE_AREAWARP_TEST",
    .table            = &k_area_table,
    .unit             = "area",
    .title_word       = S3_GAME_STATE,
    .title_value      = S3_GS_TITLE,
    .demo_word        = S3_IS_DEMO_MODE,
    .live             = spyro3_skip_gameplay_live,
    .current          = area_current,
    .title_start      = area_title_start,
    .target_refused   = area_refused,
    .start            = area_start,
    .on_arrival       = area_arrival,
    .leaving_status   = "entering...",
    .arrival_logs_vbl = 1,
};

static levelwarp_t s_warp = { .game = &k_area_game };

void spyro3_areawarp_register(void) {
    build_entries();
    levelwarp_register(&s_warp);
}

void spyro3_areawarp_surface(struct openpete_surface_sink* sink) {
    levelwarp_surface(sink, k_area_rows);
}

void spyro3_areawarp_on_tick(CPUState* cpu) { levelwarp_on_tick(&s_warp, cpu); }
