// core/runtime/host_loop_internal.h - the host loop's session: what one run
// opens before its first iteration and closes after its last.
#pragma once

#include "openpete.h"
#include "openpete_frame_time_log.h"
#include "bk2_movie.h"
#include "openpete_thread_roles.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>

/* The bk2 movie session: replay (--bk2) and recording (--bk2-record). */
typedef struct {
    bk2_movie_t movie;      /* owner: open (bk2 load); freed by close */
    int         offset;     /* owner: open (bk2 load); the movie row of feed key 0 */
    bool        active;     /* owner: open; a movie is loaded for replay */
    bool        recording;  /* owner: open; bk2_record_begin succeeded */
} host_bk2_t;

/* The run's output files. The pace log and the profile are module state
 * (openpete_pace_log_*, op_prof_*); their close is a no-op when unopened. */
typedef struct {
    FILE* position_log;                /* owner: open; --position-log, else NULL */
    FILE* hash_log;                    /* owner: open; --gameplay-hash-log, else NULL */
    openpete_frame_time_log_t ftl;     /* owner: open; appended by the inline and eager presents */
} host_outputs_t;

/* One run of the host loop. Zeroed by host_session_open; every field a
 * failed open leaves set records a resource host_session_close releases. */
typedef struct host_session {
    const openpete_options_t* opts;    /* owner: open */
    bool        split;                 /* owner: open; the thread split applies */
    Uint64      start;                 /* owner: open; SDL ticks the run's elapsed time counts from */
    const char* exit_reason;           /* owner: open on a setup error, else the loop */
    int         exit_code;             /* owner: close (its argument); raised by the psxtrace verdict */
    bool        loop_entered;          /* owner: the loop; close runs its output steps only when set */
    int         refresh_hz;            /* owner: open; display refresh, 60 headless or unknown */
    int         refresh_mhz;           /* owner: open; display refresh in mHz, for the pace log */
    int         render_fps;            /* owner: the pacer step; the launch --render-fps, resolved */
    double      tick_scale;            /* owner: the pacer step; --tick-scale, 1 when unset */
    host_bk2_t     bk2;
    host_outputs_t out;
} host_session_t;

/** @brief Open the run up to the pacer: display claim, mods guard, game boot hook, game fiber, deadline and refresh query.
 *
 * Zeroes @p S first. On a failure S->exit_reason names the step.
 * @param S The session.
 * @param opts The parsed options.
 * @return OPENPETE_EXIT_OK, or OPENPETE_EXIT_PANIC when the game fiber cannot be allocated. */
int host_session_open(host_session_t* S, const openpete_options_t* opts) OP_REQUIRES(sim);

/** @brief Open the run's outputs after the pacer: logs, bk2 record and replay, profile, latches, menu and eager production.
 *
 * Runs after the pacer step has set S->render_fps and S->tick_scale. On a
 * failure S->exit_reason names the step and every output opened so far
 * stays recorded in @p S for host_session_close.
 * @param S The session.
 * @return OPENPETE_EXIT_OK, or OPENPETE_EXIT_SETUP_ERROR. */
int host_session_open_outputs(host_session_t* S) OP_REQUIRES(sim);

/** @brief Tear the run down in order and return its exit code.
 *
 * Each step is a release step, which runs on every path and is guarded by
 * what @p S records as opened, or an output step (reports, the psxtrace
 * verdict, the bk2 movie write), which runs only when S->loop_entered.
 * On a setup error the bk2 recorder is discarded instead of finalized, so
 * no zero-row movie replaces the file at the record path.
 * @param S The session.
 * @param rc The exit code so far: the loop's, or the failed open step's.
 * @return @p rc, or OPENPETE_EXIT_DIVERGENCE when the psxtrace verdict failed a run that was OK. */
int host_session_close(host_session_t* S, int rc) OP_REQUIRES(sim);
