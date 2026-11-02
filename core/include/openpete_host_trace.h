/* openpete_host_trace.h - the host loop's decision trace (OPENPETE_LOOP_TRACE).
 *
 * OPENPETE_LOOP_TRACE=<path> writes one fixed-size 64-byte record per host
 * loop iteration: the iteration's gates (sim due, unlock, override claim,
 * rewind, yield, tick), the present it made, the runahead outcome, the exit
 * reason, the VBL pace flags and the two pacer deadlines. A header record
 * opens each process's stream and close-step records list the teardown
 * steps in the order they ran. Every record is flushed when written, so a
 * killed or exec'd process leaves every completed iteration on disk.
 *
 * Unset, the setters store only into the host-side record below and nothing
 * is written; the trace reads loop state and stores into no PSX RAM.
 *
 * Record layout (little-endian):
 *   0  u8  type        HT_REC_HEADER, HT_REC_ITER, HT_REC_CLOSE
 *   1  u8  present     host_trace_present_t (iteration) / step count (close)
 *   2  u8  ra          host_trace_ra_t
 *   3  u8  exit        host_trace_exit_t, set on the iteration that ended the loop
 *   4  u32 flags       HT_F_* bits
 *   8  u32 iter        iteration index, from 0 per process
 *  12  u32 frame       openpete_frame at the iteration's end
 *  16  u32 feed_key    openpete_feed_key_now() at the iteration's end
 *  20  u32 canon_tick  openpete_canon_tick at the iteration's end
 *  24  u8  pace_flags  the VBL record's OP_PACE_F_* (meaningful with HT_F_YIELD)
 *  25  u8  verdict     host_trace_verdict_t of the inline shed predictor
 *  26  u16 reserved
 *  28  u32 reserved
 *  32  i64 next_sim    sim deadline after the tail advanced it
 *  40  i64 next_render render deadline after the tail advanced it
 *  48  16 bytes reserved
 * A close record carries the step ids (host_trace_step_t) from byte 8, up
 * to HT_CLOSE_STEPS_PER_REC per record, its count in byte 1. A header
 * record carries "OPLT" at byte 8, the format version at 12 and the
 * HT_H_* process flags at 16. */
#ifndef OPENPETE_HOST_TRACE_H
#define OPENPETE_HOST_TRACE_H

#include <stdint.h>

#include "openpete_thread_roles.h"

#define HT_REC_BYTES 64
#define HT_CLOSE_STEPS_PER_REC 56
#define HT_VERSION 1

enum { HT_REC_HEADER = 1, HT_REC_ITER = 2, HT_REC_CLOSE = 3 };

/* Header flags. */
enum {
    HT_H_RESUMED  = 1u << 0,   /* --resume-handoff: the stream is appended */
    HT_H_HEADLESS = 1u << 1,
};

/* Iteration flags. */
enum {
    HT_F_SIM_DUE      = 1u << 0,
    HT_F_UNLOCK_ENTRY = 1u << 1,   /* the loop-top unlock peek */
    HT_F_OVR_CLAIMED  = 1u << 2,
    HT_F_REWOUND      = 1u << 3,
    HT_F_YIELD        = 1u << 4,
    HT_F_TICKED       = 1u << 5,
    HT_F_EAGER        = 1u << 6,   /* eager active after the present phase */
    HT_F_INTERP_CAP   = 1u << 7,   /* the interp ring capture ran */
    HT_F_INTER_REX    = 1u << 8,   /* the inline intermediate re-extracted (world live) */
    HT_F_PREDICTOR    = 1u << 9,   /* the inline shed predictor block ran */
    HT_F_DID_PRESENT  = 1u << 10,  /* a canonical present ran */
    HT_F_PAUSED       = 1u << 11,
    HT_F_STEP_ONE     = 1u << 12,
    HT_F_UNLOCK_TAIL  = 1u << 13,  /* the loop-tail unlock verdict */
    HT_F_VCLOCK       = 1u << 14,  /* the virtual clock drives op_now */
    HT_F_SPINE        = 1u << 15,  /* spine_on at the canonical present */
    HT_F_EAGER_LIVE   = 1u << 16,  /* s_eager.world_live (wall clock off the virtual clock) */
    HT_F_NATIVE_INTER = 1u << 17,  /* native_intermediate */
};

typedef enum {
    HT_P_NONE = 0,
    HT_P_CANON_INLINE,     /* canonical, neither eager nor the lag re-extract */
    HT_P_CANON_EAGER,      /* canonical, eager plan and produce */
    HT_P_CANON_LAG,        /* canonical, inline lag re-extract */
    HT_P_INTERMEDIATE,     /* inline intermediate, strict admit or predictor cold */
    HT_P_LATE_ADMIT,       /* inline intermediate admitted on the late budget */
    HT_P_PROBE,            /* inline intermediate admitted as the periodic probe */
    HT_P_SHED,             /* inline intermediate shed */
    HT_P_EAGER_PRODUCE,    /* eager produce on a non-drawing iteration */
    HT_P_PAUSED_REPAINT,
} host_trace_present_t;

typedef enum { HT_V_NONE = 0, HT_V_STRICT, HT_V_LATE, HT_V_PROBE, HT_V_SHED } host_trace_verdict_t;

typedef enum { HT_RA_NONE = 0, HT_RA_SPECULATED, HT_RA_ADOPTED, HT_RA_LEFT } host_trace_ra_t;

typedef enum {
    HT_X_NONE = 0, HT_X_FRAME_LIMIT, HT_X_BK2_END, HT_X_PSXTRACE_EOF,
    HT_X_PSXTRACE_DIVERGENCE, HT_X_PSXTRACE_ERROR, HT_X_USER_QUIT,
    HT_X_GAME_RETURNED, HT_X_RECORD_FRAMES, HT_X_MAX_RUNTIME, HT_X_OTHER,
} host_trace_exit_t;

/* Teardown steps, in the order run_host_loop runs them. A conditional step
 * is recorded only when it ran. */
typedef enum {
    HT_S_PRESENT_THREAD_STOP = 1, HT_S_MODS_SHUTDOWN, HT_S_SETTINGS_FLUSH,
    HT_S_REBIND_SUMMARY, HT_S_BK2_FEED_REPORT, HT_S_BK2_FREE, HT_S_BK2_FINALIZE,
    HT_S_RECORD_FLUSH, HT_S_RECORD_SHUTDOWN, HT_S_CAPTURE_REPORT,
    HT_S_POSITION_LOG_CLOSE, HT_S_HASH_LOG_CLOSE, HT_S_PSXTRACE_SUMMARY,
    HT_S_PSXTRACE_CLOSE, HT_S_EXIT_LOG, HT_S_PACE_CENSUS, HT_S_SWAPCHAIN_REPORT,
    HT_S_PACE_LOG_CLOSE, HT_S_SLIP_TOTALS, HT_S_FTL_CLOSE, HT_S_FUNCTRACE,
    HT_S_INTERP_CENSUS, HT_S_RA_PREDICT_REPORT, HT_S_VSTREAM_CENSUS,
    HT_S_SFXPACK_CENSUS, HT_S_INTERP_SHUTDOWN, HT_S_FIBER_DELETE,
    HT_S_PRESENT_THREAD_REPORT, HT_S_PROF_CLOSE, HT_S_NATIVE_PRESENT_CENSUS,
    HT_S_EAGER_REPORT, HT_S_CAPTURE_INTER_REPORT, HT_S_DISC_CENSUS,
    HT_S_INPUT_PARITY, HT_S_INPUT_LOG_REPORT, HT_S_RUMBLE_REPORT,
    HT_S_REWIND_REPORT, HT_S_SAVESTATE_REPORT, HT_S_VBL_DELIVERY,
    HT_S_OSD_CENSUS, HT_S_VERSION_CENSUS, HT_S_GPU_AUDIT, HT_S_SFX_SHUTDOWN,
    HT_S_FEATURE_SHUTDOWN, HT_S_RENDER_SHUTDOWN, HT_S_EXIT_STDOUT,
    HT_S_INPUT_STDOUT, HT_S_POSE_STDOUT, HT_S_BK2_DISCARD,
} host_trace_step_t;

/* The record under construction. Written only through the setters below;
 * read by host_trace_iter_end. */
typedef struct {
    int      on;
    int      open;      /* an iteration's record has begun and is not written */
    uint32_t flags;
    uint8_t  present;
    uint8_t  verdict;
    uint8_t  ra;
} host_trace_cur_t;

extern host_trace_cur_t g_host_trace OP_GUARDED_BY(sim);  /* thread-roles: the host loop fills it on sim */

/** @brief Open the trace named by OPENPETE_LOOP_TRACE and write the process header record.
 *
 * Appends when the process resumes a handoff, so the predecessor's records
 * stay; truncates otherwise. A no-op when the variable is unset.
 * @param resumed Non-zero when --resume-handoff is set.
 * @param headless Non-zero for a headless run. */
void host_trace_open(int resumed, int headless) OP_REQUIRES(sim);

/** @brief Write the pending close-step record and close the trace.
 *
 * Called after run_host_loop returns on every path, so a setup-error return
 * still leaves its (possibly empty) close record. */
void host_trace_finish(void) OP_REQUIRES(sim);

/** @brief Start an iteration's record: clear the flags, present kind, verdict and runahead outcome. */
static inline void host_trace_iter_begin(void) OP_REQUIRES(sim) {
    g_host_trace.open = 1;
    g_host_trace.flags = 0;
    g_host_trace.present = HT_P_NONE;
    g_host_trace.verdict = HT_V_NONE;
    g_host_trace.ra = HT_RA_NONE;
}

/** @brief Set or clear one HT_F_* bit of the current record.
 *
 * @param bit The HT_F_* bit.
 * @param v Non-zero sets it. */
static inline void host_trace_bit(uint32_t bit, int v) OP_REQUIRES(sim) {
    g_host_trace.flags = v ? (g_host_trace.flags | bit) : (g_host_trace.flags & ~bit);
}

/** @brief Record the iteration's present kind (the last call wins).
 *
 * @param kind A host_trace_present_t. */
static inline void host_trace_present(host_trace_present_t kind) OP_REQUIRES(sim) {
    g_host_trace.present = (uint8_t)kind;
}

/** @brief Record the inline shed predictor's verdict.
 *
 * @param v A host_trace_verdict_t. */
static inline void host_trace_verdict(host_trace_verdict_t v) OP_REQUIRES(sim) {
    g_host_trace.verdict = (uint8_t)v;
}

/** @brief Record an admitting predictor verdict: strict when the strict margin fits, else late.
 *
 * @param fits_strict Non-zero when the strict margin fits before the deadline. */
static inline void host_trace_admit(int fits_strict) OP_REQUIRES(sim) {
    g_host_trace.verdict = (uint8_t)(fits_strict ? HT_V_STRICT : HT_V_LATE);
}

/** @brief Record an inline intermediate present, its kind taken from the predictor's verdict. */
static inline void host_trace_intermediate(void) OP_REQUIRES(sim) {
    g_host_trace.present = (uint8_t)(g_host_trace.verdict == HT_V_LATE  ? HT_P_LATE_ADMIT
                                   : g_host_trace.verdict == HT_V_PROBE ? HT_P_PROBE
                                                                        : HT_P_INTERMEDIATE);
}

/** @brief Record the runahead outcome.
 *
 * @param r A host_trace_ra_t. */
static inline void host_trace_ra(host_trace_ra_t r) OP_REQUIRES(sim) {
    g_host_trace.ra = (uint8_t)r;
}

/** @brief Record the end of a speculation that left gameplay or was adopted; adopted takes precedence.
 *
 * @param left Non-zero when the speculation left gameplay.
 * @param adopted Non-zero when the speculation was adopted. */
static inline void host_trace_ra_end(int left, int adopted) OP_REQUIRES(sim) {
    if (left || adopted) g_host_trace.ra = (uint8_t)(adopted ? HT_RA_ADOPTED : HT_RA_LEFT);
}

/** @brief Write the current iteration's record.
 *
 * @param quit Non-zero when this iteration ended the loop.
 * @param exit_reason The loop's exit reason, recorded only when quit is set.
 * @param pace_flags The VBL record's OP_PACE_F_* flags.
 * @param next_sim The sim deadline after the tail.
 * @param next_render The render deadline after the tail. */
void host_trace_iter_end(int quit, const char* exit_reason, uint8_t pace_flags,
                         int64_t next_sim, int64_t next_render) OP_REQUIRES(sim);

/** @brief Write the record of an iteration the loop left mid-way (the bk2-end break).
 *
 * A no-op when the last iteration's record was already written.
 * @param exit_reason The loop's exit reason. */
void host_trace_loop_end(const char* exit_reason) OP_REQUIRES(sim);

/** @brief Append one teardown step to the close-step record.
 *
 * @param step A host_trace_step_t. */
void host_trace_close_step(host_trace_step_t step) OP_REQUIRES(sim);

#endif /* OPENPETE_HOST_TRACE_H */
