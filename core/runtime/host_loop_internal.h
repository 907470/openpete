// core/runtime/host_loop_internal.h - the host loop's session: what one run
// opens before its first iteration and closes after its last.
#pragma once

#include "openpete.h"
#include "host_pacer.h"
#include "openpete_frame_time_log.h"
#include "bk2_movie.h"
#include "openpete_thread_roles.h"
#include "openpete_time.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
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
    host_pacer_t   pacer;              /* owner: open (init, start); advanced by the loop tail */
    host_bk2_t     bk2;
    host_outputs_t out;
} host_session_t;

/* The pace-log record of the VBL an iteration runs: the input sample it
 * latched. */
typedef struct {
    op_time_t latch_ns;    /* owner: host_input_latch; publish instant of the latched sample, 0 on the legacy path */
    unsigned  latch_seq;   /* owner: host_input_latch; the latched sample's identity, OP_PACE_LATCH_NONE on the legacy path */
    uint16_t  latch_bits;  /* owner: host_input_latch; pad bits of the latched sample */
} host_vbl_rec_t;

/* One iteration of the host loop. Zeroed at the iteration's start. */
typedef struct {
    bool unlock_at_entry;   /* owner: the loop top; the unlock verdict from the previous keymap sample */
    bool sim_due;           /* owner: the loop top, after the live pacer changes; the sim runs this iteration */
    bool ovr_claimed;       /* owner: host_input_arbitrate; a bk2 row or an overlay claimed the pad override channel */
    unsigned bk2_stamped_frame; /* owner: host_input_arbitrate; openpete_frame the bk2 stamp was made for */
    bool p1_staged;         /* owner: host_input_latch; slot 0 of the input record was staged before the yield */
    unsigned frame_before_yield; /* owner: the tick, before the yield; read by host_input_commit */
    host_vbl_rec_t vbl;     /* owner: host_input_latch (latch fields) */
} host_iter_t;

/** @brief Open the run: display claim, mods guard, game boot hook, game fiber, deadline, refresh query, pacer, rewind ring, runahead request and the outputs.
 *
 * Zeroes @p S first. On a failure S->exit_reason names the step and every
 * resource opened so far stays recorded in @p S for host_session_close.
 * @param S The session.
 * @param opts The parsed options.
 * @return OPENPETE_EXIT_OK, OPENPETE_EXIT_PANIC when the game fiber cannot be allocated, or OPENPETE_EXIT_SETUP_ERROR when an output cannot be opened. */
int host_session_open(host_session_t* S, const openpete_options_t* opts) OP_REQUIRES(sim);

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
