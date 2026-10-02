// games/common/features/levelwarp_ui.c - the dev-overlay level warp's
// game-independent half: the dropdown, the Jump button, the click queue and
// the scripted driver. See levelwarp_ui.h for the contract.

#include "features/levelwarp_ui.h"

#include "game_extras.h"    /* game_set_skip_to_level (title-screen route) */
#include "openpete_fiber.h" /* openpete_speculative */
#include "openpete_freecam.h"
#include "openpete_knobs.h" /* dev-overlay rows (M key) */
#include "openpete_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host-side UI state - deliberately NOT in any savestate. A click is the
 * user's intent right now; restoring an older pending flag over it (the
 * quiet rewind/runahead path reloads the blob every tick) would either eat a
 * fresh click or resurrect a stale one.
 *   s_sel         - dropdown index into the descriptor's table
 *   s_sel_touched - the user has picked something, so stop auto-following
 *                   the level they are in
 *   s_pending     - a click waiting for the next non-speculative tick
 *   s_route       - cached "what would Jump do right now", for the gate and
 *                   the status line */
static const levelwarp_ui_desc_t* s_desc;
static int s_sel         = 0;
static int s_sel_touched = 0;
static int s_pending     = 0;
static int s_pending_id  = 0;
static int s_route       = LEVELWARP_ROUTE_NONE;

/** @brief Find a level id's index in the descriptor's level table.
 *
 * @param id The level id.
 * @return The table index; -1 when the id is not in the table. */
static int level_index_for_id(int id) {
    int i;
    for (i = 0; i < s_desc->count; i++)
        if (s_desc->level_id(i) == id) return i;
    return -1;
}

/* ---- dev-overlay rows ------------------------------------------------- */

/** @brief Read the dropdown row's selected index.
 *
 * @param k The row; unused.
 * @return The index into the descriptor's level table. */
static double sel_get(const knob_desc_t* k) { (void)k; return (double)s_sel; }

/** @brief Select a dropdown entry and stop the dropdown following the player's level.
 *
 * @param k The row; unused.
 * @param v The index into the descriptor's level table.
 * @return 0 when applied; -1 when the index is outside the table. */
static int sel_set(knob_desc_t* k, double v) {
    int idx = (int)v;
    (void)k;
    if (idx < 0 || idx >= s_desc->count) return -1;
    s_sel         = idx;
    s_sel_touched = 1;
    return 0;
}

/** @brief Report whether a warp is queued or in flight, the Jump button's second gate.
 *
 * Kept apart from s_route because the tick also consults s_route when it
 * drains the queue: a route that read none while a click is pending would
 * make every click discard itself.
 * @return Non-zero while a click is queued or the descriptor reports a warp
 *         in flight. */
static int warp_busy(void) {
    return s_pending || (s_desc->in_flight && s_desc->in_flight());
}

/** @brief Gate the Jump button on a usable route and no warp in progress.
 *
 * @param k The row; unused.
 * @return 1 when the cached route is not none and no warp is queued or in
 *         flight, else 0. */
static double go_ready(const knob_desc_t* k) {
    (void)k;
    return (s_route != LEVELWARP_ROUTE_NONE && !warp_busy()) ? 1.0 : 0.0;
}

/** @brief Queue a warp to the selected level when the Jump button is pressed.
 *
 * Snapshots the selected level id, since the dropdown can move before the tick
 * drains the queue.
 * @param k The row; unused.
 * @param v The press value; unused.
 * @return 0 when queued; -1 when the route is none or a warp is queued or in
 *         flight. */
static int go_fire(knob_desc_t* k, double v) {
    (void)k;
    (void)v;
    if (s_route == LEVELWARP_ROUTE_NONE || warp_busy()) return -1;
    s_pending_id = s_desc->level_id(s_sel);  /* snapshot: the combo may move */
    s_pending    = 1;
    return 0;
}

/** @brief Write the status text shown beside the Jump button.
 *
 * Reads queued while a click waits; else the descriptor's in-flight status
 * when it supplies one; else a note for the title route, an empty string for
 * the level route, freecam active, or not in a level.
 * @param k The row; unused.
 * @param buf Receives the text.
 * @param cap Size of @p buf in bytes. */
static void go_status(const knob_desc_t* k, char* buf, size_t cap) {
    (void)k;
    if (s_pending)                              snprintf(buf, cap, "queued");
    else if (s_desc->in_flight_status &&
             s_desc->in_flight_status(buf, cap)) { /* the game wrote it */ }
    else if (s_route == LEVELWARP_ROUTE_TITLE)  snprintf(buf, cap, "from title (resets progress)");
    else if (s_route == LEVELWARP_ROUTE_LEVEL)  snprintf(buf, cap, "%s", "");
    else if (openpete_freecam_active())         snprintf(buf, cap, "freecam active");
    else                                        snprintf(buf, cap, "not in a level");
}

/** @brief Add the dropdown and Jump rows to the dev overlay's Gameplay section. */
static void register_rows(void) {
    knob_desc_t row;

    memset(&row, 0, sizeof row);
    row.id          = "warp_level";
    row.label       = "Warp: level";
    row.section     = "Gameplay";
    row.help        = "Destination for the Jump button below. Follows the "
                      "level you are in until you pick something.";
    row.type        = KNOB_ENUM;
    row.mut         = KNOB_LIVE;
    row.enum_values = s_desc->display;
    row.enum_count  = s_desc->count;
    row.get_num     = sel_get;
    row.set_num     = sel_set;
    (void)openpete_knob_add(&row);

    memset(&row, 0, sizeof row);
    row.id           = "warp_go";
    row.label        = "Warp: jump to level";
    row.section      = "Gameplay";
    row.help         = "Load the selected level through the game's own level "
                       "transition. From the title screen, and on the games "
                       "whose transition goes by way of it, that is a fresh "
                       "session in the chosen level and progress and lives "
                       "reset, as with --skip-to-level. Intentionally "
                       "divergent gameplay state.";
    row.type         = KNOB_ACTION;
    row.mut          = KNOB_LIVE;
    row.get_num      = go_ready;
    row.set_num      = go_fire;
    row.get_str      = go_status;
    row.action_label = "Jump";
    (void)openpete_knob_add(&row);
}

/* ---- scripted driver --------------------------------------------------- */

/* OPENPETE_LEVELWARP_TEST=<vbl>:<level> queues the same request the Jump
 * button does, at the given VBL counter. The warp is a multi-tick ride
 * through the game's own state machine, so firing it from a script is what
 * makes it testable from a script. Deliberately not a knob
 * registry row (harness variable, not a config knob):
 *   OPENPETE_LEVELWARP_TEST=2200:tree_tops openpete-spyro1 --skip-to-level artisans
 */
static int s_test_frame = -1;
static int s_test_id    = 0;

extern unsigned openpete_frame;

/** @brief Arm the scripted warp from OPENPETE_LEVELWARP_TEST.
 *
 * The variable reads <vbl>:<level>. The level text after the colon is
 * resolved through the callback, truncated to 31 bytes; an unset or malformed
 * variable, or a level that resolves negative, leaves the trigger disarmed.
 * @param resolve The level-argument resolver. */
static void test_trigger_init(int (*resolve)(const char* arg)) {
    const char* spec = getenv("OPENPETE_LEVELWARP_TEST");
    const char* colon;
    char        lvl[32];
    size_t      n;

    if (!spec || !spec[0]) return;
    colon = strchr(spec, ':');
    if (!colon) return;
    for (n = 0; colon[1 + n] && n < sizeof(lvl) - 1; n++) lvl[n] = colon[1 + n];
    lvl[n] = '\0';

    s_test_id = resolve(lvl);
    if (s_test_id < 0) { s_test_id = 0; return; }
    s_test_frame = atoi(spec);
    OPL_INFO_F(s_desc->log_tag, "harness: warp to level %d at frame %d",
               s_test_id, s_test_frame);
}

/** @brief Queue the scripted warp once the frame counter reaches its trigger.
 *
 * Fires once, then disarms. */
static void test_trigger_tick(void) {
    if (s_test_frame < 0 || (int)openpete_frame < s_test_frame) return;
    s_test_frame = -1;
    s_pending_id = s_test_id;
    s_pending    = 1;
}

/* ---- entry points ------------------------------------------------------ */

void levelwarp_ui_register(const levelwarp_ui_desc_t* desc) {
    s_desc        = desc;
    s_sel         = 0;
    s_sel_touched = 0;
    s_pending     = 0;
    s_route       = LEVELWARP_ROUTE_NONE;
    register_rows();
    test_trigger_init(desc->resolve);
}

int levelwarp_ui_tick(CPUState* cpu) {
    /* Runahead: a speculative tick's guest writes are rolled back, so
     * consuming the click here would drop the warp on the floor. Let the
     * committed tick do it. */
    if (openpete_speculative) return 1;

    /* Queue the harness request BEFORE anything else reads s_pending, so it
     * lands where a UI click lands: set between ticks, drained on the next
     * one. Firing it mid-tick instead would let the harness take a path the
     * button never takes. */
    test_trigger_tick();

    s_route = s_desc->route(cpu);
    /* The camera is detached from the player under the debug free camera, so
     * a transition's fly-in would be meaningless. */
    if (s_route == LEVELWARP_ROUTE_LEVEL && openpete_freecam_active())
        s_route = LEVELWARP_ROUTE_NONE;

    /* Keep the dropdown on the level the player is actually in until they
     * pick one themselves. */
    if (!s_sel_touched && !(s_desc->in_flight && s_desc->in_flight())) {
        int idx = level_index_for_id(s_desc->current_level(cpu));
        if (idx >= 0) s_sel = idx;
    }

    if (s_pending && s_desc->in_flight && s_desc->in_flight())
        return 1;                    /* drain when idle */

    if (s_pending) {
        int target = s_pending_id;
        s_pending  = 0;
        if (s_route == LEVELWARP_ROUTE_TITLE) {
            /* The --skip-to-level machinery owns this one: it induces the
             * demo-timeout transition at the next Press-Start window, which
             * also gets the progress and lives reset a fresh session needs. */
            game_set_skip_to_level(target);
            OPL_INFO_F(s_desc->log_tag,
                       "queued title-screen boot into level %d", target);
        } else if (s_route == LEVELWARP_ROUTE_LEVEL) {
            s_desc->start_level_route(cpu, target);
        } else {
            OPL_INFO_F(s_desc->log_tag, "%s",
                       "ignored (no route from this state)");
        }
        return 1;
    }
    return 0;
}
