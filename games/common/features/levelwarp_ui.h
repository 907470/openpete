#pragma once
/* levelwarp_ui.h - the dev-overlay warp (M key): a destination dropdown, a
 * Warp button, the click queue, the scripted driver, and the in-flight
 * state machine that follows a warp to its arrival.
 *
 * One levelwarp_t is one warp row pair: the level warp of every game, and
 * Spyro 3's area warp. The overlay runs outside the sim, so a click only
 * queues a request; the queue is drained on the next non-speculative tick,
 * and every guest write happens in the game's start and leaving hooks. Idle,
 * nothing here writes guest memory; the route probe reads it.
 *
 * A game supplies a levelwarp_game_t: its target table, the words its route
 * and arrival read, and the hooks only it can supply - entering its own
 * transition toward a target and, where the transition is retargeted on the
 * way, the leaving phase. The title-screen route hands the target to the
 * --skip-to-level machinery (game_set_skip_to_level, or the game's
 * title_start), so this module owns it; the attract-demo route makes the
 * game's demo-end stores (demo_end) and then hands off the same way. */

#include "features/level_table.h"
#include "openpete_knobs.h" /* knob_desc_t */

#include <stddef.h>
#include <stdint.h>

typedef struct CPUState CPUState;
struct openpete_surface_sink;

enum {
    LEVELWARP_ROUTE_NONE = 0,  /* no warp is possible from this guest state */
    LEVELWARP_ROUTE_LEVEL,     /* in a level: the game's own transition */
    LEVELWARP_ROUTE_TITLE,     /* at the title or its menu: the --skip-to-level machinery */
    LEVELWARP_ROUTE_DEMO       /* in the attract demo: its own end, then the title route */
};

/* Warp phases. The values are a savestate format (Spyro 1 saves the phase):
 * new phases take new numbers. */
enum {
    LEVELWARP_IDLE    = 0,
    LEVELWARP_LEAVING = 1,  /* the transition is entered; the target not yet loading */
    LEVELWARP_LOADING = 2,  /* waiting for arrival */
    LEVELWARP_QUEUED  = 3   /* the game's request waits for a later drain */
};

typedef struct levelwarp levelwarp_t;

typedef struct levelwarp_game {
    const char*               log_tag;
    const knob_desc_t*        rows;      /* [0] the dropdown, [1] the button */
    const char*               test_env;  /* scripted driver: <vbl>:<target> */
    const game_level_table_t* table;     /* the dropdown's targets */
    const char*               unit;      /* "level" or "area", in log lines */

    /* Route. The title route opens while title_word reads title_value
     * (title_word 0: no title route); the in-level route while live() holds
     * and demo_word (0: none) reads 0, and never under the free camera. */
    uint32_t title_word, title_value;
    uint32_t demo_word;
    int (*live)(CPUState* cpu);
    /* The target number the player is at, read for the dropdown's
     * auto-follow and on arrival: current() when set, else current_word. */
    uint32_t current_word;
    uint32_t (*current)(CPUState* cpu);
    /* Optional title route for a target: NULL is
     * game_set_skip_to_level(target). */
    void (*title_start)(int target);
    /* Optional: a reason the selected target cannot be warped to from the
     * current state, shown as the button's status; NULL when it can. */
    const char* (*target_refused)(CPUState* cpu, int target);
    /* Optional attract-demo route: makes the stores the game's own demo end
     * makes, so the demo returns to the title, where the title route takes
     * the target. The route opens while live() holds and demo_word reads
     * nonzero; NULL leaves no route from the demo. */
    void (*demo_end)(CPUState* cpu);
    /* Status text with no route; NULL reads "not in a level". */
    const char* none_status;

    /* Enter the game's transition toward `target` (w->target holds it on
     * entry). Returns the phase to enter: LEVELWARP_IDLE when the request is
     * ignored. */
    int (*start)(levelwarp_t* w, CPUState* cpu, int target);
    /* Optional LEVELWARP_LEAVING step returning the next phase; NULL leaves
     * for LEVELWARP_LOADING once live() stops holding. A hook that aborts
     * logs its reason and returns LEVELWARP_IDLE. */
    int (*leaving)(levelwarp_t* w, CPUState* cpu);
    /* Optional arrival test in LEVELWARP_LOADING; NULL is live(). */
    int (*arrived)(CPUState* cpu);
    /* Optional, run on arrival after its log line: returns the next phase
     * (LEVELWARP_IDLE ends the warp). */
    int (*on_arrival)(levelwarp_t* w, CPUState* cpu, uint32_t landed);
} levelwarp_game_t;

/* One warp row pair's state. Host-side, outside every savestate except where
 * a game saves the phase itself: a click is the user's intent now, and
 * restoring an older pending flag over it (the quiet rewind and runahead
 * paths reload the blob every tick) would eat a fresh click or resurrect a
 * stale one. */
struct levelwarp {
    const levelwarp_game_t* game;
    uint32_t    phase, target, ticks;
    const char* display[64];
    int         count;
    int         sel, sel_touched;
    int         pending, pending_id;
    int         route;
    const char* refused;
    int         test_frame, test_id;
};

/* The level warp's two rows ("warp_level", "warp_go"). */
extern const knob_desc_t levelwarp_level_rows[2];

/** @brief Reset a warp, add its two dev-overlay rows and arm its scripted driver.
 *
 * Called once from the feature module's apply_options. At most two warps
 * register per process.
 * @param w The warp; it and its game must outlive the process. */
void levelwarp_register(levelwarp_t* w);

/** @brief Report two row templates to the surface dump, without registering them.
 *
 * @param sink The dump's collector.
 * @param rows The two templates. */
void levelwarp_surface(struct openpete_surface_sink* sink,
                       const knob_desc_t* rows);

/** @brief Run one tick: drain a queued request, then follow an in-flight warp to arrival.
 *
 * Runs every tick, even with no warp pending, so the button's route stays
 * current. A speculative tick does nothing. A due scripted request queues
 * first, so it lands where a click lands. The dropdown follows the current
 * target until the user picks one. A queued request drains once no warp is
 * in flight: the title route through game_set_skip_to_level, the demo route
 * through demo_end and then the title route, the in-level route through the
 * game's start hook. An in-flight warp then advances:
 * LEVELWARP_LEAVING through the leaving hook, LEVELWARP_LOADING to arrival,
 * which logs whether the target was reached, naming openpete_vbl. The call
 * count restarts on entering LEVELWARP_LOADING; a phase that runs past 20000
 * calls aborts the warp.
 * @param w The warp.
 * @param cpu The guest CPU state. */
void levelwarp_on_tick(levelwarp_t* w, CPUState* cpu);

/** @brief Move a queued warp to LEVELWARP_LEAVING once its request was made, restarting the call count.
 *
 * @param w The warp. */
void levelwarp_called(levelwarp_t* w);

/** @brief End a warp with no log line: its request was dropped, or a hook aborted it.
 *
 * @param w The warp. */
void levelwarp_reset(levelwarp_t* w);

/** @brief Log "aborted (<why>)" and end the warp.
 *
 * @param w The warp.
 * @param why The reason. */
void levelwarp_abort(levelwarp_t* w, const char* why);
