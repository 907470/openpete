// games/common/features/levelwarp_ui.c - the dev-overlay warp: the
// dropdown, the Warp button, the click queue, the scripted driver and the
// in-flight state machine. See levelwarp_ui.h for the contract.

#include "features/levelwarp_ui.h"

#include "game_extras.h"    /* game_set_skip_to_level (title-screen route) */
#include "openpete_fiber.h" /* openpete_speculative, openpete_frame */
#include "openpete_freecam.h"
#include "openpete_log.h"
#include "openpete_surface.h"
#include "psx_runtime_internal.h" /* CPUState + its read callbacks */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Backstop for a warp that never resolves. Every phase ends on a state the
 * game itself reaches, so this only fires if the game parks somewhere
 * unanticipated; it must not double as a deadline. Counted in calls (one per
 * sim yield): a CD load plus fly-in can run past 3600 in a headless
 * free-running session, which is why no phase leans on it. */
#define WARP_STUCK_CALLS 20000u

/* The registered warps, for the row callbacks, which receive only the row. */
static levelwarp_t* s_warps[2];

/** @brief Find the warp a dev-overlay row belongs to, by the row's id.
 *
 * @param k The row.
 * @return The warp whose templates carry the id; the first warp when none does. */
static levelwarp_t* warp_of(const knob_desc_t* k) {
    for (size_t i = 0; i < sizeof s_warps / sizeof s_warps[0]; i++) {
        const levelwarp_t* w = s_warps[i];
        if (w && (strcmp(k->id, w->game->rows[0].id) == 0 ||
                  strcmp(k->id, w->game->rows[1].id) == 0))
            return s_warps[i];
    }
    return s_warps[0];
}

/** @brief Report whether the warp has a request queued or a warp in flight, the Warp button's second gate.
 *
 * @param w The warp.
 * @return Non-zero while a click is queued or the phase is not idle. */
static int warp_busy(const levelwarp_t* w) {
    return w->pending || w->phase != LEVELWARP_IDLE;
}

/* ---- dev-overlay rows ------------------------------------------------- */

/** @brief Read the dropdown row's selected index.
 *
 * @param k The row.
 * @return The index into the warp's target table. */
static double sel_get(const knob_desc_t* k) { return (double)warp_of(k)->sel; }

/** @brief Select a dropdown entry and stop the dropdown following the current target.
 *
 * @param k The row.
 * @param v The index into the warp's target table.
 * @return 0 when applied; -1 when the index is outside the table. */
static int sel_set(knob_desc_t* k, double v) {
    levelwarp_t* w = warp_of(k);
    int idx = (int)v;
    if (idx < 0 || idx >= w->count) return -1;
    w->sel         = idx;
    w->sel_touched = 1;
    return 0;
}

/** @brief Gate the Warp button on a usable route, an accepted target and no warp in progress.
 *
 * @param k The row.
 * @return 1 when the button may fire, else 0. */
static double go_ready(const knob_desc_t* k) {
    const levelwarp_t* w = warp_of(k);
    return (w->route != LEVELWARP_ROUTE_NONE && !w->refused && !warp_busy(w))
        ? 1.0 : 0.0;
}

/** @brief Queue a warp to the selected target when the Warp button is pressed.
 *
 * Snapshots the selected target number, since the dropdown can move before
 * the tick drains the queue.
 * @param k The row.
 * @param v The press value; unused.
 * @return 0 when queued; -1 when the button is gated. */
static int go_fire(knob_desc_t* k, double v) {
    levelwarp_t* w = warp_of(k);
    (void)v;
    if (go_ready(k) == 0.0) return -1;
    w->pending_id = w->game->table->levels[w->sel].number;
    w->pending    = 1;
    return 0;
}

/** @brief Write the status text shown beside the Warp button.
 *
 * Reads queued while a click waits; the game's leaving status, or
 * "loading..." from LEVELWARP_LOADING, while a warp is in flight; else a
 * note for the title route, the refusal reason or an empty string for the
 * level route, freecam active, or the game's no-route text.
 * @param k The row.
 * @param buf Receives the text.
 * @param cap Size of @p buf in bytes. */
static void go_status(const knob_desc_t* k, char* buf, size_t cap) {
    const levelwarp_t* w = warp_of(k);
    const levelwarp_game_t* g = w->game;
    const char* s;
    if (w->pending)                             s = "queued";
    else if (w->phase == LEVELWARP_LOADING)     s = "loading...";
    else if (w->phase != LEVELWARP_IDLE)        s = g->leaving_status;
    else if (w->route == LEVELWARP_ROUTE_TITLE) s = "from title (resets progress)";
    else if (w->route == LEVELWARP_ROUTE_LEVEL) s = w->refused ? w->refused : "";
    else if (openpete_freecam_active())         s = "freecam active";
    else s = g->none_status ? g->none_status : "not in a level";
    snprintf(buf, cap, "%s", s);
}

/* The level warp's rows, as templates: register_rows fills in the
 * dropdown's display names and the value plumbing, and levelwarp_surface
 * hands the templates to the surface dump as they are. */
const knob_desc_t levelwarp_level_rows[2] = {
    { .id = "warp_level", .label = "Level warp", .section = "Gameplay",
      .help = "Destination for the Warp button. Follows the level you "
              "are in until you pick something.",
      .type = KNOB_ENUM, .mut = KNOB_LIVE,
      .default_note = "the level the player is in" },
    { .id = "warp_go", .label = "Warp to level", .section = "Gameplay",
      .help = "Load the selected level through the game's own level "
              "transition. From the title screen, and on the games whose "
              "transition goes by way of it, that is a fresh session in the "
              "chosen level and progress and lives reset, as with "
              "--skip-to-level.",
      .type = KNOB_ACTION, .mut = KNOB_LIVE, .action_label = "Warp" },
};

/** @brief Add the warp's dropdown and Warp rows to the dev overlay.
 *
 * @param w The warp. */
static void register_rows(levelwarp_t* w) {
    knob_desc_t row = w->game->rows[0];
    row.enum_values = w->display;
    row.enum_count  = w->count;
    row.get_num     = sel_get;
    row.set_num     = sel_set;
    (void)openpete_knob_add(&row);

    row         = w->game->rows[1];
    row.get_num = go_ready;
    row.set_num = go_fire;
    row.get_str = go_status;
    (void)openpete_knob_add(&row);
}

void levelwarp_surface(struct openpete_surface_sink* sink,
                       const knob_desc_t* rows) {
    openpete_surface_row(sink, &rows[0]);
    openpete_surface_row(sink, &rows[1]);
}

/* ---- scripted driver --------------------------------------------------- */

/* <test_env>=<vbl>:<target> queues the same request the Warp button does,
 * at the given VBL counter. The warp is a multi-tick ride through the game's
 * own state machine, so firing it from a script is what makes it testable
 * from a script. A harness variable, not a knob registry row:
 *   OPENPETE_LEVELWARP_TEST=2200:tree_tops build/linux-release/openpete --skip-to-level artisans
 */

/** @brief Arm the scripted warp from the game's test variable.
 *
 * The variable reads <vbl>:<target>. The text after the colon, truncated to
 * 31 bytes, is resolved against the warp's table; an unset or malformed
 * variable, or a target that does not resolve, leaves the trigger disarmed.
 * @param w The warp. */
static void test_trigger_init(levelwarp_t* w) {
    const char* spec = w->game->test_env ? getenv(w->game->test_env) : NULL;
    const char* colon;
    char        lvl[32];
    size_t      n;

    w->test_frame = -1;
    if (!spec || !spec[0]) return;
    colon = strchr(spec, ':');
    if (!colon) return;
    for (n = 0; colon[1 + n] && n < sizeof(lvl) - 1; n++) lvl[n] = colon[1 + n];
    lvl[n] = '\0';

    w->test_id = game_level_resolve(w->game->table, lvl);
    if (w->test_id < 0) { w->test_id = 0; return; }
    w->test_frame = atoi(spec);
    OPL_INFO_F(w->game->log_tag, "harness: warp to %s %d at frame %d",
               w->game->unit, w->test_id, w->test_frame);
}

/** @brief Queue the scripted warp once the frame counter reaches its trigger.
 *
 * Fires once, then disarms.
 * @param w The warp. */
static void test_trigger_tick(levelwarp_t* w) {
    if (w->test_frame < 0 || (int)openpete_frame < w->test_frame) return;
    w->test_frame = -1;
    w->pending_id = w->test_id;
    w->pending    = 1;
}

/* ---- entry points ------------------------------------------------------ */

void levelwarp_register(levelwarp_t* w) {
    const game_level_table_t* t = w->game->table;
    w->count = (int)(t->count < 64 ? t->count : 64);
    for (int i = 0; i < w->count; i++) w->display[i] = t->levels[i].display;
    levelwarp_reset(w);
    w->sel = w->sel_touched = w->pending = 0;
    w->route   = LEVELWARP_ROUTE_NONE;
    w->refused = NULL;
    s_warps[s_warps[0] && s_warps[0] != w ? 1 : 0] = w;
    register_rows(w);
    test_trigger_init(w);
}

void levelwarp_reset(levelwarp_t* w) {
    w->phase  = LEVELWARP_IDLE;
    w->target = 0;
    w->ticks  = 0;
}

void levelwarp_abort(levelwarp_t* w, const char* why) {
    OPL_INFO_F(w->game->log_tag, "aborted (%s)", why);
    levelwarp_reset(w);
}

void levelwarp_called(levelwarp_t* w) {
    w->phase = LEVELWARP_LEAVING;
    w->ticks = 0;
}

/** @brief Pick the route the live guest state allows.
 *
 * The title route when title_word reads title_value; the in-level route when
 * live() holds, demo_word reads 0 and the free camera is off, since the
 * camera is detached from the player under it; else none.
 * @param g The game.
 * @param cpu The guest CPU state.
 * @return A LEVELWARP_ROUTE_* value. */
static int route_of(const levelwarp_game_t* g, CPUState* cpu) {
    if (g->title_word && cpu->read_word(g->title_word) == g->title_value)
        return LEVELWARP_ROUTE_TITLE;
    if (g->live(cpu) && (!g->demo_word || cpu->read_word(g->demo_word) == 0u) &&
        !openpete_freecam_active())
        return LEVELWARP_ROUTE_LEVEL;
    return LEVELWARP_ROUTE_NONE;
}

/** @brief Start a queued request through the route it drained under.
 *
 * @param w The warp.
 * @param cpu The guest CPU state.
 * @param target The target number. */
static void drain(levelwarp_t* w, CPUState* cpu, int target) {
    const levelwarp_game_t* g = w->game;
    if (w->route == LEVELWARP_ROUTE_TITLE) {
        /* The --skip-to-level machinery owns this one: it induces the
         * demo-timeout transition at the next Press-Start window, which also
         * gets the progress and lives reset a fresh session needs. */
        game_set_skip_to_level(target);
        OPL_INFO_F(g->log_tag, "queued title-screen boot into level %d", target);
    } else if (w->route == LEVELWARP_ROUTE_LEVEL && !w->refused) {
        w->target = (uint32_t)target;
        const int ph = g->start(w, cpu, target);
        if (ph == LEVELWARP_IDLE) { levelwarp_reset(w); return; }
        w->phase = (uint32_t)ph;
        w->ticks = 0;
    } else {
        OPL_INFO_F(g->log_tag, "%s", "ignored (no route from this state)");
    }
}

/** @brief Run the front half: refresh the route, follow the dropdown, and drain a queued request.
 *
 * @param w The warp.
 * @param cpu The guest CPU state.
 * @return 1 when the in-flight half must not run this tick: a speculative
 *         tick, a tick holding a request behind an in-flight warp, or a tick
 *         that drained a request; else 0. */
static int ui_tick(levelwarp_t* w, CPUState* cpu) {
    const levelwarp_game_t* g = w->game;
    /* Runahead: a speculative tick's guest writes are rolled back, so
     * consuming the click here would drop the warp. */
    if (openpete_speculative) return 1;
    /* The harness request queues before anything reads `pending`, so it
     * lands where a click lands: set between ticks, drained on the next. */
    test_trigger_tick(w);

    w->route   = route_of(g, cpu);
    w->refused = (w->route == LEVELWARP_ROUTE_LEVEL && g->target_refused)
        ? g->target_refused(cpu, g->table->levels[w->sel].number) : NULL;

    if (!w->sel_touched && w->phase == LEVELWARP_IDLE) {
        int idx = game_level_index(g->table, (int)cpu->read_word(g->current_word));
        if (idx >= 0) w->sel = idx;
    }

    if (!w->pending) return 0;
    if (w->phase != LEVELWARP_IDLE) return 1;   /* drain when idle */
    w->pending = 0;
    if (w->route == LEVELWARP_ROUTE_LEVEL && g->target_refused)
        w->refused = g->target_refused(cpu, w->pending_id);
    drain(w, cpu, w->pending_id);
    return 1;
}

/** @brief Log the arrival, then run the game's arrival hook.
 *
 * @param w The warp.
 * @param cpu The guest CPU state. */
static void arrive(levelwarp_t* w, CPUState* cpu) {
    const levelwarp_game_t* g = w->game;
    const uint32_t landed = cpu->read_word(g->current_word);
    if (landed != w->target)
        OPL_INFO_F(g->log_tag, "landed in %s %u, wanted %u", g->unit,
                   (unsigned)landed, (unsigned)w->target);
    else if (g->arrival_logs_vbl)
        OPL_INFO_F(g->log_tag, "arrived in %s %u at vbl=%u", g->unit,
                   (unsigned)landed, (unsigned)openpete_frame);
    else
        OPL_INFO_F(g->log_tag, "arrived in %s %u", g->unit, (unsigned)landed);
    const int ph = g->on_arrival ? g->on_arrival(w, cpu, landed) : LEVELWARP_IDLE;
    if (ph == LEVELWARP_IDLE) { levelwarp_reset(w); return; }
    w->phase = (uint32_t)ph;
    w->ticks = 0;
}

void levelwarp_on_tick(levelwarp_t* w, CPUState* cpu) {
    const levelwarp_game_t* g = w->game;
    if (ui_tick(w, cpu)) return;
    if (w->phase == LEVELWARP_IDLE || w->phase == LEVELWARP_QUEUED) return;
    if (++w->ticks > WARP_STUCK_CALLS) { levelwarp_abort(w, "stuck"); return; }

    /* The game has the wheel: LEAVING writes only through the game's hook,
     * LOADING writes nothing. The level the warp left is still live on the
     * tick its transition is entered, so arrival is read only once it has
     * gone, however long the CD load takes. */
    if (w->phase == LEVELWARP_LEAVING) {
        const int ph = g->leaving ? g->leaving(w, cpu)
                     : g->live(cpu) ? LEVELWARP_LEAVING : LEVELWARP_LOADING;
        if (ph == LEVELWARP_IDLE) { levelwarp_reset(w); return; }
        if (ph == LEVELWARP_LOADING && g->reset_ticks_on_loading) w->ticks = 0;
        w->phase = (uint32_t)ph;
        return;
    }
    if (g->arrived ? g->arrived(cpu) : g->live(cpu)) arrive(w, cpu);
}
