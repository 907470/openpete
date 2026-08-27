/* openpete_pace_log.h — unified pacing event log + always-on pacing census.
 *
 * One binary file, one timebase (op_now), typed fixed-size records. Every VBL
 * and every present is logged — never sampled, because a throttled trace picks
 * lucky phases and hides exactly the races this exists to find. Raw timestamps
 * only; statistics are computed offline by scripts/pacing/.
 *
 * The point of one file rather than one per subsystem: attributing a late tick
 * to the present that blocked it is a cross-correlation, and that needs both
 * events on the same timeline.
 *
 *   header (64 B + env_bytes of "key=value\n" text):
 *     u32 magic 'OPPL'   u32 version        u32 header_bytes  u32 record_bytes
 *     u32 env_bytes      u32 reserved
 *     i64 created_unix_ns   i64 op_epoch_ns
 *     i64 vbl_hz_num     i64 vbl_hz_den     i64 tick_vbls
 *
 *   record (48 B):
 *     u8 type   u8 flags   u16 aux   u32 index   i64 t_ns   i64 a,b,c,d
 *
 * Multi-byte fields are written in host byte order. Every target this ships
 * on is little-endian; the reader assumes it rather than negotiating.
 *
 * The env block is mandatory and carries build/CPU/GPU/refresh/present-mode/
 * governor. Baselines only compare within a matching environment, so the
 * analyzer refuses a cross-env diff rather than silently subtracting numbers
 * taken on different machines.
 *
 * The census (counters below) is compiled into every build and needs no log
 * file: pacing health is reported in the end-of-run summary of every run, the
 * way the determinism gates are ambient. It refuses to report phase-error
 * statistics for a run that was never paced (headless fast-forward, Tab
 * unlock) — a grid nobody followed has no error worth quoting.
 */

#ifndef OPENPETE_PACE_LOG_H
#define OPENPETE_PACE_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OP_PACE_MAGIC        0x4C50504FU   /* 'OPPL' LE */
#define OP_PACE_VERSION      1U
#define OP_PACE_HEADER_FIXED 64U
#define OP_PACE_RECORD_BYTES 48U

/* Record types. Never renumber: logs outlive the build that wrote them. */
enum {
    OP_PACE_VBL         = 1,
    OP_PACE_PRESENT     = 2,
    OP_PACE_SHED        = 3,
    OP_PACE_SLIP        = 4,
    OP_PACE_INPUT_EDGE  = 5,
    OP_PACE_WINDOW      = 6,
    OP_PACE_CLOCKSAMPLE = 7,
    OP_PACE_WAKE        = 8,
    OP_PACE_PRESENT_SPAN = 9
};

/* VBL flags. */
enum {
    OP_PACE_F_SLIPPED     = 1u << 0,
    OP_PACE_F_UNLOCK      = 1u << 1,  /* deadline ignored this iteration */
    OP_PACE_F_PAUSED      = 1u << 2,
    OP_PACE_F_STEPPED     = 1u << 3,
    OP_PACE_F_REWIND      = 1u << 4,
    OP_PACE_F_SPECULATIVE = 1u << 5,  /* runahead speculated past this VBL */
    OP_PACE_F_NOT_LIVE    = 1u << 6,  /* world not live (load / transition)  */
    OP_PACE_F_QUIESCE     = 1u << 7   /* loop-head yield for a snapshot edge */
};

/* PRESENT / SHED kinds. */
enum { OP_PACE_CANONICAL = 0, OP_PACE_INTERMEDIATE = 1 };

/* SLIP kinds. */
enum { OP_PACE_SLIP_SIM = 0, OP_PACE_SLIP_RENDER = 1 };

/* WINDOW ids, paired begin/end (flags bit 0 set = begin). */
enum {
    OP_PACE_W_UNLOCK     = 1,
    OP_PACE_W_PAUSE      = 2,
    OP_PACE_W_FRAMESTEP  = 3,
    OP_PACE_W_REWIND     = 4,
    OP_PACE_W_STATELOAD  = 5,
    OP_PACE_W_LEVELLOAD  = 6
};

/* ---- lifecycle ---------------------------------------------------------- */

/* Open PATH and write the header. `env` is the pre-rendered "key=value\n"
 * environment block (see openpete_pace_env_build). Returns 0 on success; on
 * failure the log stays closed and every emit below is a no-op, so callers
 * need no second check. */
int  openpete_pace_log_open(const char* path, const char* env);

/* Flush + close. Safe on an unopened log. */
void openpete_pace_log_close(void);

/* Non-zero while a log file is open — for callers that would otherwise do
 * work (a timestamp read, a string format) purely to feed it. */
int  openpete_pace_log_active(void);

/* Render the mandatory environment block into `buf`. Fills in what it can
 * discover itself (build hash, OS, CPU count, governor); the caller supplies
 * the display/present facts it alone knows. `refresh_mhz` is the exact
 * measured refresh in millihertz (59940 and 60000 are different machines for
 * this plan's purposes), `present_mode` the mode actually GRANTED, not the one
 * requested. */
void openpete_pace_env_build(char* buf, unsigned buf_bytes,
                             const char* gpu, const char* driver,
                             int refresh_mhz, const char* present_mode,
                             int fullscreen, int render_fps,
                             int interp_on, int runahead_n,
                             double tick_scale);

/* ---- emit --------------------------------------------------------------- */

/* One VBL. `deadline_ns` is the grid point this iteration was paced to;
 * `latch_ns` is the PUBLISH instant of the pad snapshot this tick consumed,
 * captured at the latch — not the time of the latch call. Post-thread-split
 * the producer republishes on its own cadence, so reading it back later names
 * a sample the tick never saw; exec_start/end bracket the game fiber.
 * A zero timestamp means "not applicable this iteration" and the analyzer
 * treats it as missing rather than as the epoch. */
void openpete_pace_vbl(uint32_t vbl_index, uint8_t flags, uint16_t pad_reads,
                       int64_t deadline_ns, int64_t latch_ns,
                       int64_t exec_start_ns, int64_t exec_end_ns);

/* One present ATTEMPT — recorded even when the swapchain refused an image, so
 * a run that stops displaying is visible rather than silently absent.
 *
 * `done_ns` is when the present path RETURNED, not when it was submitted:
 * everything before it is accounted for by the block times, which are the
 * waits the present path imposed on whichever thread called it. Under the
 * thread split that thread also runs the sim, which is what makes a blocked
 * present a candidate cause for the next tick's lateness. `recreate_block_ns`
 * carries the swapchain rebuild — the single largest of these, and previously
 * attributed to nothing. */
void openpete_pace_present(uint8_t kind, int64_t done_ns,
                           int64_t fence_block_ns, int64_t acquire_block_ns,
                           int64_t queue_present_block_ns,
                           int64_t recreate_block_ns);

/* A present deliberately not made. No shedder exists yet (pacing plan Phase
 * 2/3); the counter reads zero until one does, which is a true statement
 * rather than a missing one. */
void openpete_pace_shed(uint8_t kind, uint16_t reason,
                        int64_t predicted_cost_ns, int64_t deadline_margin_ns);

/* A deadline clamp: `periods` of wall time discarded because the host fell
 * `behind_ns` behind. Feeds both the record stream and the census. The
 * analyzer, not the runtime, attributes a cause. */
void openpete_pace_slip(uint8_t kind, int64_t periods, int64_t behind_ns);

/* A pad edge, from the always-on per-VBL input producer. Pad bits only. */
void openpete_pace_input_edge(int64_t sample_ns, uint16_t bits,
                              uint16_t bits_delta, int analog_delta);

/* Begin/end of a span during which the pacing grid does not apply. */
void openpete_pace_window(uint16_t id, int begin);

/* The pacer's sleep: when it was requested, where it aimed, when it returned.
 *
 * All THREE instants are required, and the third is the one that makes the
 * record honest. `wake - target` conflates two opposite situations: the OS
 * oversleeping past a future deadline, and a deadline that had ALREADY PASSED
 * when the sleep was requested — the second is the loop running behind, and
 * blaming it on the scheduler points every investigation at the wrong system.
 * Measured: 26 ms of apparent "overshoot" that was entirely the former.
 *
 * So the log records the OS's share as `wake - max(target, request)` and how
 * far behind the loop already was as `max(0, request - target)`. */
void openpete_pace_wake(int64_t request_ns, int64_t target_ns, int64_t wake_ns);

/* How long a present occupied the calling thread, start to finish.
 *
 * The PRESENT record's block times only cover the calls that BLOCK. The rest
 * of the present path — rebuilding the interpolated scene, recording the
 * command buffer, submitting — is ordinary CPU work on the same thread, and
 * under the host split that thread owns the sim clock. With the blocking
 * removed it becomes the dominant remaining cause of a late tick, and it was
 * invisible: attribution reported it as unattributed, which is the log
 * saying it lacks an event type. This is that event type.
 *
 * `extract_ns` splits out the scene rebuild, the largest single part and the
 * one a shed predicate can actually avoid. */
void openpete_pace_present_span(uint8_t kind, int64_t start_ns,
                                int64_t end_ns, int64_t extract_ns);

/* ---- census (always compiled, no log file needed) ----------------------- */

#define OP_PACE_HIST_BUCKETS 9

typedef struct {
    uint64_t vbls;              /* VBL records seen                          */
    uint64_t vbls_paced;        /* ... of which the deadline actually applied */
    uint64_t vbls_clean;        /* ... paced AND not paused/stepped/rewound/
                                 * loading: the pacing-evidence population    */
    uint64_t vbls_transient;    /* paced but flagged: reported, not folded in */
    uint64_t presents[2];       /* canonical, intermediate                    */
    uint64_t sheds;
    uint64_t slip_events[2];    /* sim, render                                */
    uint64_t slip_periods[2];
    int64_t  phase_err_max_ns;  /* max (exec_start - deadline) over clean VBLs */
    int64_t  phase_err_min_ns;  /* most-early (negative) start                 */
    int64_t  phase_err_transient_max_ns; /* same, over the flagged ones        */
    uint64_t phase_hist[OP_PACE_HIST_BUCKETS];
    int64_t  latch_offset_max_ns; /* max exec_start - input sample            */
    uint64_t wakes;             /* pacer sleeps observed                     */
    int64_t  wake_overshoot_max_ns; /* worst OS overshoot past a FUTURE target */
    int64_t  wake_behind_max_ns;    /* worst "deadline already gone" at request */
    uint64_t latch_samples;     /* VBLs that contributed one: 0 means the
                                 * bound is unmeasured, not perfect          */
} openpete_pace_census_t;

void openpete_pace_census_get(openpete_pace_census_t* out);

/* Upper bound (ns, exclusive) of each phase-error histogram bucket; the last
 * bucket is unbounded. Shared with the analyzer so both sides bucket alike. */
extern const int64_t openpete_pace_hist_edges_ns[OP_PACE_HIST_BUCKETS];

/* Format the end-of-run census summary into `buf`. Reports "unpaced" rather
 * than a phase-error figure when no iteration ran against a live deadline. */
void openpete_pace_census_format(char* buf, unsigned buf_bytes);

/* ---- instrument self-verification --------------------------------------- */

/* OPENPETE_PACE_INJECT_US=N[,EVERY]: busy-block N µs immediately before the
 * game fiber runs, on every EVERY-th paced VBL (default 30). The log must then
 * report a phase error of ~N µs on those VBLs and nowhere else — a known input
 * with a known answer, so the instrument is calibrated against truth instead of
 * trusted because it compiled. Returns the µs actually blocked (0 when off).
 * Call once per iteration, immediately before the yield. */
int64_t openpete_pace_inject(int paced);

/* OPENPETE_PACE_INJECT_PRESENT_US=N: add N µs of busy work to every
 * intermediate present, inflating its measured cost.
 *
 * This is the shed predicate's proof of engagement. A shed that has never
 * been observed to fire is an assertion, not a mechanism: the fault injector
 * makes presents too expensive to fit before the tick deadline, and the
 * contract — presents are shed, ticks hold phase — becomes a thing that can
 * be checked rather than believed. Returns the µs actually spent. */
int64_t openpete_pace_inject_present(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PACE_LOG_H */
