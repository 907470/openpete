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
#include "spyro-3/skip_to_level.h"

#define S3_CURRENT_LEVEL    0x8006C5BCu
#define S3_AREA             0x8006C5C8u
#define S3_IS_DEMO_MODE     0x8006C658u

/* The dropdown's entries: the area index is the area word's value. No area
 * name is held by the game (its strings name levels, eggs and the pause
 * rows), so the entries are numbered. */
static const game_level_t k_areas[] = {
    { "main", "Main area", 0 },
    { "1",    "Area 1",    1 },
    { "2",    "Area 2",    2 },
    { "3",    "Area 3",    3 },
};

static const game_level_table_t k_area_table = {
    GAME_LEVEL_ROWS(k_areas), NULL, 0, "area", "OPENPETE_AREAWARP_TEST", 0u,
};

static const knob_desc_t k_area_rows[2] = {
    { .id = "warp_area", .label = "Area warp", .section = "Gameplay",
      .help = "Destination for the area Warp button, in a Spyro 3 level with "
              "sub-areas: the main area or a numbered sub-area. Follows the "
              "area you are in until you pick something.",
      .type = KNOB_ENUM, .mut = KNOB_LIVE,
      .default_note = "the area the player is in" },
    { .id = "warp_area_go", .label = "Warp to area", .section = "Gameplay",
      .help = "Enter the selected area the way its portal does, keeping "
              "progress. From one sub-area to another the warp passes "
              "through the main area.",
      .type = KNOB_ACTION, .mut = KNOB_LIVE, .action_label = "Warp" },
};

static levelwarp_t s_warp;
static int         s_level = 0;  /* the level the request was made in */
static int         s_from  = 0;  /* the area the queued leg leaves */
static int         s_final = 0;  /* the area the warp ends in */

/** @brief Report why an area cannot be warped to from the current state.
 *
 * @param cpu The guest CPU state.
 * @param target The area index.
 * @return The status text, or NULL when the warp may start. */
static const char* area_refused(CPUState* cpu, int target) {
    const int count = spyro3_area_count((int)cpu->read_word(S3_CURRENT_LEVEL));
    if (count <= 1) return "no sub-areas in this level";
    if (target >= count) return "no such area in this level";
    if ((uint32_t)target == cpu->read_word(S3_AREA)) return "already here";
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
    w->target = (uint32_t)leg;
    OPL_INFO_F("areawarp", "queued: level %d area %d -> area %d (warp to area %d)",
               s_level, from, leg, s_final);
    return LEVELWARP_QUEUED;
}

/** @brief Start an area warp: record the level and the final area, then queue the first leg.
 *
 * @param w The warp.
 * @param cpu The guest CPU state.
 * @param target The area index.
 * @return queue_leg's phase. */
static int area_start(levelwarp_t* w, CPUState* cpu, int target) {
    s_level = (int)cpu->read_word(S3_CURRENT_LEVEL);
    s_final = target;
    return queue_leg(w, cpu);
}

/** @brief Queue the leg from area 0 to the final area after a first leg's arrival in area 0.
 *
 * @param w The warp.
 * @param cpu The guest CPU state.
 * @param landed The area word read on arrival.
 * @return LEVELWARP_QUEUED for a second leg, else LEVELWARP_IDLE. */
static int area_arrival(levelwarp_t* w, CPUState* cpu, uint32_t landed) {
    if (landed != 0u || s_final == 0) return LEVELWARP_IDLE;
    return queue_leg(w, cpu);
}

static const levelwarp_game_t k_area_game = {
    .log_tag          = "areawarp",
    .rows             = k_area_rows,
    .test_env         = "OPENPETE_AREAWARP_TEST",
    .table            = &k_area_table,
    .unit             = "area",
    .demo_word        = S3_IS_DEMO_MODE,
    .live             = spyro3_skip_gameplay_live,
    .current_word     = S3_AREA,
    .target_refused   = area_refused,
    .start            = area_start,
    .on_arrival       = area_arrival,
    .leaving_status   = "entering...",
    .arrival_logs_vbl = 1,
};

static levelwarp_t s_warp = { .game = &k_area_game };

void spyro3_areawarp_register(void) { levelwarp_register(&s_warp); }

void spyro3_areawarp_surface(struct openpete_surface_sink* sink) {
    levelwarp_surface(sink, k_area_rows);
}

void spyro3_areawarp_on_tick(CPUState* cpu) { levelwarp_on_tick(&s_warp, cpu); }
