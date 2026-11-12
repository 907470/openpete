/* openpete_pace_log.h - unified pacing event log + always-on pacing census.
 *
 * One binary file, one timebase (op_now), typed fixed-size records. Every VBL
 * and every present is logged - never sampled, because a throttled trace picks
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
 *   record (64 B):
 *     u8 type   u8 flags   u16 aux   u32 index   i64 t_ns   i64 a,b,c,d,e,f
 *
 * Version 2 added the payload slots `e` and `f` (v1 records were 48 B with
 * neither). The reader takes the record size from the header rather than
 * assuming it, so a v1 log stays readable - the slots read as absent.
 *
 * Multi-byte fields are written in host byte order. Every target this ships
 * on is little-endian; the reader assumes it rather than negotiating.
 *
 * The env block is mandatory and carries build/CPU/GPU/refresh/present-mode/
 * governor/compositor. Baselines only compare within a matching environment,
 * so the analyzer refuses a cross-env diff rather than silently subtracting
 * numbers taken on different machines.
 *
 * The census (counters below) is compiled into every build and needs no log
 * file: pacing health is reported in the end-of-run summary of every run, the
 * way the determinism gates are ambient. It refuses to report phase-error
 * statistics for a run that was never paced (headless fast-forward, Tab
 * unlock) - a grid nobody followed has no error worth quoting.
 */

#ifndef OPENPETE_PACE_LOG_H
#define OPENPETE_PACE_LOG_H

#include <stdint.h>

#include "openpete_thread_roles.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reasons a present did not reach the display (SHED record `reason`). */
enum { PACE_SHED_SLOT_BUSY = 1, PACE_SHED_NO_BUDGET = 2, PACE_SHED_NO_IMAGE = 3,
       PACE_SHED_INSTANT_PASSED = 4 };
#define OP_PACE_MAGIC        0x4C50504FU   /* 'OPPL' LE */
#define OP_PACE_VERSION      2U
#define OP_PACE_HEADER_FIXED 64U
#define OP_PACE_RECORD_BYTES 64U

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
    OP_PACE_PRESENT_SPAN = 9,
    OP_PACE_PAD_READ    = 10,
    OP_PACE_FLIP        = 11,
    OP_PACE_CD_MISS     = 12,  /* a disc cache miss served from the image file: a = file read ns, b = end offset */
    OP_PACE_HOST_PUMP   = 13,  /* one iteration of the host pump loop (thread split only) */
    /* What a PRESENT showed and where: index = the PRESENT's index, aux = its
     * kind, flags = OP_PACE_PD_*, a = swapchain image index (-1 none), b, c =
     * swapchain extent, d = openpete_interp_tick_stamp, e =
     * openpete_interp_present_now, f = the RHI's swapchain presents so far. */
    OP_PACE_PRESENT_DETAIL = 14,
    /* CPU time per tick, written after each drawing VBL's fiber run for the
     * interval since the previous one. Slots carry two u32 microsecond values
     * each (low word first): a = sim (every fiber run in the interval),
     * extract (re-extracts, PRESENT_SPAN extract_ns); b = terrain (canonical
     * terrain build and scene feed), moby (canonical moby native pass after
     * the game's own draw); c = snap in sub-tick re-extracts, snap in the
     * canonical feed (moby contact resolve with its carry); d = render
     * (scene render and submit on the presenting thread), present (staged
     * present: acquire, blit, queue present); e = u16 counts: extracts,
     * renders, presents, snap resolves; f = the drawing VBL's own fiber run
     * (low word). index = openpete_frame of the drawing VBL. */
    OP_PACE_TICK_SPLIT = 15,
    /* The eager planner's span plan, one per canonical tick while eager
     * production runs (the fields of the OPENPETE_EAGER_DBG plan line):
     * index = openpete_frame, flags = OP_PACE_EP_*, aux = span level (low
     * byte) and standing level (high byte), t_ns = the plan instant, a =
     * budget ns, b = per-scene tail cost ns, c = lag ns, d = non-drawing VBL
     * cost ns, e = tick start lateness ns, f = slots (bits 0-15), afford
     * (16-31), planned scenes (32-47), planned grid period in us (48-63,
     * saturated). */
    OP_PACE_EAGER_PLAN = 16,
    /* GPU time of one rendered frame from the GPU timestamp zones, written
     * when its results are harvested (frames in flight later): index = low
     * word of the frame's RHI token, aux = zone count, a = first zone start to
     * last zone end ns, b = sum of depth-0 zone durations ns, c = the token. */
    OP_PACE_GPU_FRAME = 17
};

/* EAGER_PLAN flags. */
enum {
    OP_PACE_EP_CUT    = 1u << 0,  /* the previous span left targets unproduced */
    OP_PACE_EP_LATE   = 1u << 1,  /* the tick started past its deadline (stall) */
    OP_PACE_EP_PINNED = 1u << 2,  /* OPENPETE_EAGER_LEVEL pins the level */
    OP_PACE_EP_GREEDY = 1u << 3,  /* OPENPETE_EAGER_POLICY=greedy */
    OP_PACE_EP_RING   = 1u << 4,  /* with CUT: the scene ring stayed full (the present thread's drain) */
    OP_PACE_EP_WITHHELD = 1u << 5 /* with CUT: production was withheld (world not live, or not resumed) */
};

/* TICK_SPLIT accumulators (the duration slots, in record order). */
enum {
    OP_PACE_SPLIT_SIM = 0,
    OP_PACE_SPLIT_EXTRACT,
    OP_PACE_SPLIT_TERRAIN,
    OP_PACE_SPLIT_MOBY,
    OP_PACE_SPLIT_SNAP_REX,
    OP_PACE_SPLIT_SNAP_CANON,
    OP_PACE_SPLIT_RENDER,
    OP_PACE_SPLIT_PRESENT,
    OP_PACE_SPLIT_COUNT
};

/* PRESENT_DETAIL flags: the interpolation mode the present ran under. */
enum {
    OP_PACE_PD_INTERP        = 1u << 0,  /* sub-tick capture enabled */
    OP_PACE_PD_EAGER         = 1u << 1,  /* the present thread owned the display */
    OP_PACE_PD_DETERMINISTIC = 1u << 2,  /* deterministic capture stamps */
    OP_PACE_PD_PAIRED        = 1u << 3   /* the image had a prev/curr pair */
};

/* HOST_PUMP flags. */
enum {
    /* An aggregate over the iterations of one reporting window, rather than a
     * single iteration. See openpete_pace_host_pump. */
    OP_PACE_HP_SUMMARY = 1u << 0,
    /* A window op (fullscreen toggle) ran in this iteration. */
    OP_PACE_HP_WINDOW  = 1u << 1,
    /* The iteration was recorded for its PARK, not its work: the wait held
     * the thread far past the timeout it asked for. On Win32 this is the
     * modal size/move loop, which runs inside the wait and does no work at
     * all - the quietest iteration of the run by active time, while input
     * publication has stopped for the length of the drag. */
    OP_PACE_HP_PARKED  = 1u << 2
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
/* A REPEAT is the same image presented again: the present thread re-blitting
 * the last composed frame onto a slot the sim thread was going to refuse. It
 * fills a hole in the DISPLAY without advancing animation by a microsecond, so
 * it is its own kind -- folding it into either of the others would let a
 * duplicate be counted as motion, which a REPEAT must never be. */
enum { OP_PACE_CANONICAL = 0, OP_PACE_INTERMEDIATE = 1, OP_PACE_REPEAT = 2 };

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

/** @brief Open path, write the 64-byte header and the environment block, and start accepting records.
 *
 * The header carries the magic, version, header and record sizes, the
 * environment block's length, the creation time in Unix nanoseconds, the
 * op_now() epoch, the VBL rate as OP_VBL_HZ_NUM / OP_VBL_HZ_DEN, and
 * OP_TICK_VBLS. On failure the log stays closed and every emit is a no-op,
 * so callers need no second check. A second call while a log is open
 * returns 0 without touching the file.
 * @param path Output file path; NULL or empty fails.
 * @param env The environment block from openpete_pace_env_build, or NULL.
 * @return 0 when a log is open, -1 when path is empty, the file cannot be
 *         opened, the header write fails, or the writer mutex cannot be
 *         created. */
int  openpete_pace_log_open(const char* path, const char* env);

/** @brief Flush buffered records, close the log file, and log the record count.
 *
 * Emitters return early from the moment the file pointer is cleared under
 * the writer lock. No-op when no log is open. */
void openpete_pace_log_close(void);

/** @brief Return nonzero while a log file is open.
 *
 * For callers that would otherwise do work (a timestamp read, a string
 * format) only to feed the log.
 * @return 1 while a log file is open, else 0. */
int  openpete_pace_log_active(void);

/** @brief Render the pacing log's mandatory environment block into @p buf as "key=value" lines.
 *
 * Fills in what it can discover itself (build id, platform, CPU model and
 * count, the Linux cpufreq governor, the compositor, the pacer slack, the
 * virtual clock, the input-pulse and eager-production settings); the caller
 * supplies the display and present facts only it knows.
 * @param buf Destination.
 * @param buf_bytes Capacity of @p buf.
 * @param gpu GPU name, or NULL for "unknown".
 * @param driver Driver name or version, or NULL for "unknown".
 * @param refresh_mhz Exact measured refresh in millihertz; 59940 and 60000
 *                    key different baselines.
 * @param present_mode The present mode actually granted, not the one
 *                     requested; NULL for "unknown".
 * @param fullscreen Non-zero when fullscreen.
 * @param render_fps The render-fps target.
 * @param interp_on Non-zero when interpolation is on.
 * @param runahead_n Runahead depth in ticks.
 * @param tick_scale The tick-rate scale.
 * @param thread_split Non-zero when the host thread split is active.
 * @param input_legacy Non-zero on the legacy input path.
 * @param headless Non-zero for a headless run. */
void openpete_pace_env_build(char* buf, unsigned buf_bytes,
                             const char* gpu, const char* driver,
                             int refresh_mhz, const char* present_mode,
                             int fullscreen, int render_fps,
                             int interp_on, int runahead_n,
                             double tick_scale,
                             int thread_split, int input_legacy,
                             int headless);

/* ---- ring ---------------------------------------------------------------- */

/* The always-on ring holds the newest OP_PACE_RING_RECORDS records (64 B
 * each) whatever the file sink does; an image of it is a log file a
 * scripts/pacing/ reader parses unchanged. */
#define OP_PACE_RING_RECORDS   131072u
#define OP_PACE_RING_WINDOW_NS ((int64_t)30 * 1000 * 1000 * 1000)

typedef struct {
    uint64_t appended;    /* records written to the ring */
    uint64_t held;        /* records the ring holds */
    uint64_t overwritten; /* records replaced by newer ones */
    uint64_t young_lost;  /* of those, replaced while inside OP_PACE_RING_WINDOW_NS */
    int64_t  span_ns;     /* newest minus oldest held record's t_ns */
    size_t   bytes;       /* ring and environment block allocation */
} openpete_pace_ring_counts_t;

/** @brief Allocate the always-on record ring and keep a copy of the environment block for its images.
 *
 * From the call on every emitter builds its record and pace_emit stores it
 * in the ring under the writer lock, beside the file buffer when a log file
 * is open; the file's record stream is unchanged by the ring. A second call
 * returns 0. OPENPETE_PACE_RING=0 leaves the ring unarmed.
 * @param env The environment block from openpete_pace_env_build, or NULL.
 * @return 0 when the ring is armed, -1 when disabled or allocation fails. */
int  openpete_pace_ring_arm(const char* env);

/** @brief Build a pace log file image of the ring's records inside a window ending now, on the heap.
 *
 * The image is the 64-byte header, the environment block given to
 * openpete_pace_ring_arm and the held records with t_ns at or after
 * op_now() - window_ns, in emission order. The records are copied under
 * the writer lock, which every emitting thread (sim, main, present)
 * takes per record; the header and environment are written outside it.
 * @param window_ns The window; 0 or less takes every held record.
 * @param out Receives the image; free with free().
 * @param len Receives its length in bytes.
 * @param records Receives the record count; may be NULL.
 * @return 0 on success, -1 when the ring is not armed or allocation fails. */
int  openpete_pace_ring_image(int64_t window_ns, void** out, size_t* len, size_t* records);

/** @brief Read the ring's counters under the writer lock; all zero when the ring is not armed.
 * @param out Receives the counters. */
void openpete_pace_ring_counts(openpete_pace_ring_counts_t* out);

/* ---- emit --------------------------------------------------------------- */

/** @brief Classify one VBL into the census, close the pad-read bracket, and write its VBL record.
 *
 * Under the virtual clock OP_PACE_F_UNLOCK is forced on, since exec_start
 * then equals the deadline by construction. A VBL is paced when it lacks
 * OP_PACE_F_UNLOCK and has a nonzero deadline; its phase error is
 * exec_start_ns - deadline_ns, signed. A paced VBL flagged paused, stepped,
 * rewound, not live, or slipped is transient: counted and folded into the
 * transient max only. A clean paced VBL feeds the phase max, min and
 * histogram, and when it consumed a host sample (latch_ns nonzero,
 * latched_seq at least 2) the latch-offset bound; latched_seq
 * OP_PACE_LATCH_CLAIMED counts it as claimed instead. The bracket closes
 * before the sink check so the census classifies pad reads the same
 * with or without a sink. With a sink (the ring or an open log), the
 * record carries deadline,
 * latch, exec start and end, latched_seq and latched_bits, and a
 * CLOCKSAMPLE record follows every 10 s. A zero timestamp means not
 * applicable this iteration; the analyzer treats it as missing.
 * @param vbl_index openpete_frame of the VBL.
 * @param flags OP_PACE_F_* bits.
 * @param pad_reads Guest pad reads in this VBL.
 * @param deadline_ns Grid point this iteration was paced to.
 * @param latch_ns Publish instant of the pad sample this tick consumed,
 *        captured at the latch; under the thread split a later read-back
 *        would name a sample the tick never saw.
 * @param exec_start_ns Start of the game fiber run.
 * @param exec_end_ns End of the game fiber run.
 * @param latched_seq Publish sequence of the latched sample, or an
 *        OP_PACE_LATCH_* value.
 * @param latched_bits Pad bits of the latched sample. */
void openpete_pace_vbl(uint32_t vbl_index, uint8_t flags, uint16_t pad_reads,
                       int64_t deadline_ns, int64_t latch_ns,
                       int64_t exec_start_ns, int64_t exec_end_ns,
                       unsigned latched_seq, uint16_t latched_bits);

/* `latched_seq` values, in the record's `e` slot. Publish sequences are even
 * and step by 2, so the odd values are free to name the two different reasons
 * a VBL consumed no host sample - reasons a bare zero would conflate, leaving
 * a bk2 replay's input statistics indistinguishable from a live run's. */
#define OP_PACE_LATCH_NONE    0u   /* no latch on this path (legacy input)  */
#define OP_PACE_LATCH_CLAIMED 1u   /* bk2 / menu / devui owned the pad      */

/* `latched_bits` - the pad bits of that same sample.
 *
 * The sequence alone answers "was this exact sample read", which is NOT the
 * question. The producer republishes far faster than the consumer latches, so
 * most samples are legitimately never read, and a held button reaches the game
 * through a LATER sample carrying the same state. Whether a press was seen is
 * therefore a question about bits: the first VBL to latch at or after the edge
 * either still carries the edge's new state, or the press came and went
 * between two latches and the game never saw it at all. Without the bits,
 * every superseded sample looks like a dropped press. */

/** @brief Publish the interpolation alpha the next present will show and whether a prev/curr pair existed.
 *
 * Called by each game's re-extract and by the cutscene guard once the alpha
 * is resolved. The value is stored in micro-alpha (1e-6 of a tick), signed,
 * since a canonical present may carry a slightly negative alpha. Two copies
 * are written: one read by openpete_pace_extract_alpha, one read by
 * openpete_pace_last_alpha and carried by openpete_pace_present.
 * @param alpha Sub-tick alpha of the scene.
 * @param paired Nonzero when a prev/curr pair existed. */
void openpete_pace_note_alpha(double alpha, int paired);
/** @brief Return the paired flag of the alpha last published by openpete_pace_note_alpha.
 *
 * @return 1 when the re-extract that published the alpha had a prev/curr
 *         pair to interpolate between, else 0. */
int  openpete_pace_last_alpha_paired(void);
/** @brief Return the alpha of the last openpete_pace_note_alpha call from the extraction side's copy.
 *
 * Eager production reads it to tag the scene it publishes; no present
 * record writes this copy.
 * @param paired Receives the paired flag when non-NULL.
 * @return The alpha, at micro-alpha precision. */
double openpete_pace_extract_alpha(int *paired);
/** @brief Return the alpha of the last openpete_pace_note_alpha call.
 *
 * The inline present path passes it to openpete_pace_record_present, and
 * the scene-signature diagnostic prints it.
 * @return The alpha, at micro-alpha precision. */
double openpete_pace_last_alpha(void);

/** @brief Set the nominal present slot against which a display hole counts as long.
 *
 * An intermediate present that follows the previous present by more than
 * two slots increments gaps_mid_long. Set at interpolation init and
 * whenever the render period changes; 0 disables that counter rather than
 * judging against a stale rate.
 * @param slot_ns Present period in ns (1 / --render-fps); a negative value
 *        is stored as 0. */
void openpete_pace_set_slot_ns(int64_t slot_ns);

/** @brief Record that a present reached the display.
 *
 * This is the only timestamp in the log that is not a submit time: every
 * other record says when an image was handed over, and the display decides
 * when to show it. The record keeps the low 32 bits of id as its index and
 * the full id in slot a. Writes no census counter; no-op without a sink
 * (the ring or an open log).
 * @param id Swapchain present id waited on.
 * @param t_ns Instant the wait returned, taken as the flip time.
 * @param waited_ns How long the wait blocked; a near-zero wait means the
 *        flip had already happened and t_ns is only when it was asked. */
void openpete_pace_flip(uint64_t id, int64_t t_ns, int64_t waited_ns);

/** @brief Record the present that just returned on the calling thread.
 *
 * Takes the RHI block times, then compares the native renderer's fence and
 * acquire shed counters against their values at the previous call: any
 * increase is written as that many SHED records (PACE_SHED_SLOT_BUSY for a
 * fence refusal, PACE_SHED_NO_IMAGE for an acquire refusal) and no PRESENT
 * record. Otherwise it records a PRESENT through
 * openpete_pace_present_alpha, stamped with op_now(). The inline present
 * path and the present thread share it so the refusal ledger has one
 * owner; the two never present concurrently.
 * @param kind OP_PACE_CANONICAL, OP_PACE_INTERMEDIATE or OP_PACE_REPEAT.
 * @param alpha Sub-tick alpha the image showed.
 * @param paired 0 when the image had no prev/curr pair. */
void openpete_pace_record_present(uint8_t kind, double alpha, int paired);

/** @brief Record one present attempt with its block times and alpha, and update the present census.
 *
 * The census counts the present by kind and updates the display-cadence
 * statistics: an intermediate present more than two slots after the
 * previous present counts in gaps_mid_long, a canonical present's gap feeds
 * hold_ema_ns, and a repeat updates neither. The alpha travels as an
 * argument because the note_alpha global belongs to the extracting thread
 * and a present recorded on another thread would otherwise log another
 * scene's alpha.
 * @param kind OP_PACE_CANONICAL, OP_PACE_INTERMEDIATE or OP_PACE_REPEAT.
 * @param done_ns When the present path returned, not when it submitted;
 *        the time before it is accounted for by the block times, the waits
 *        the present imposed on the calling thread.
 * @param fence_block_ns Time blocked on the frame fence.
 * @param acquire_block_ns Time blocked acquiring a swapchain image.
 * @param queue_present_block_ns Time blocked in the queue present.
 * @param recreate_block_ns Time spent rebuilding the swapchain.
 * @param alpha_micro Alpha the image showed, in 1e-6 of a tick.
 * @param paired 0 when the image had no prev/curr pair. */
void openpete_pace_present_alpha(uint8_t kind, int64_t done_ns,
                                 int64_t fence_block_ns, int64_t acquire_block_ns,
                                 int64_t queue_present_block_ns,
                                 int64_t recreate_block_ns,
                                 int64_t alpha_micro, int paired);
/** @brief Record one present attempt with the alpha last published by openpete_pace_note_alpha.
 * @param kind OP_PACE_CANONICAL, OP_PACE_INTERMEDIATE or OP_PACE_REPEAT.
 * @param done_ns When the present path returned.
 * @param fence_block_ns Time blocked on the frame fence.
 * @param acquire_block_ns Time blocked acquiring a swapchain image.
 * @param queue_present_block_ns Time blocked in the queue present.
 * @param recreate_block_ns Time spent rebuilding the swapchain. */
void openpete_pace_present(uint8_t kind, int64_t done_ns,
                           int64_t fence_block_ns, int64_t acquire_block_ns,
                           int64_t queue_present_block_ns,
                           int64_t recreate_block_ns);

/** @brief Record a present deliberately not made, and count it in the census.
 *
 * The census counter increments whether or not a log file is open. The host
 * loop sheds an intermediate present with PACE_SHED_NO_BUDGET when its
 * predicted cost does not fit before the sim deadline; eager production
 * sheds an intermediate scene with PACE_SHED_NO_BUDGET when its span ends
 * with the scene unproduced for the deadline or a full scene ring, and with
 * PACE_SHED_INSTANT_PASSED when its display instant passed before it could
 * be produced;
 * openpete_pace_record_present records swapchain refusals as
 * PACE_SHED_SLOT_BUSY (fence) and PACE_SHED_NO_IMAGE (acquire), with the
 * matching block time as the cost.
 * @param kind The present kind: OP_PACE_CANONICAL, OP_PACE_INTERMEDIATE or
 *        OP_PACE_REPEAT.
 * @param reason A PACE_SHED_* code.
 * @param predicted_cost_ns Predicted cost of the skipped present.
 * @param deadline_margin_ns Time left before the sim deadline at the shed. */
void openpete_pace_shed(uint8_t kind, uint16_t reason,
                        int64_t predicted_cost_ns, int64_t deadline_margin_ns);

/** @brief Record a deadline clamp that discarded wall time because the host fell behind.
 *
 * Counts the event and the discarded periods (a negative count adds 0) in
 * the census per kind, whether or not a log is open, and writes a SLIP
 * record when one is. The analyzer, not the runtime, attributes a cause.
 * @param kind OP_PACE_SLIP_RENDER, or OP_PACE_SLIP_SIM (any other value
 *        counts as sim).
 * @param periods Pacing periods discarded.
 * @param behind_ns How far behind the deadline the host was. */
void openpete_pace_slip(uint8_t kind, int64_t periods, int64_t behind_ns);
/** @brief Record a disc cache miss served from the image file.
 *
 * The disc cache calls it after the reader's file read. Writes a CD_MISS
 * record only; no census counter; no-op without a sink (the ring
 * or an open log).
 * @param waited_ns Time the file read took.
 * @param end_offset End byte offset of the requested range. */
void openpete_pace_cd_miss(int64_t waited_ns, uint64_t end_offset);

/** @brief Record a pad edge published by the always-on input producer.
 *
 * Counts the edge in the census whether or not a log is open. seq is the
 * field that makes the record attributable: a VBL records the sequence it
 * latched, so which tick consumed an edge, and whether any did, is answered
 * by identity rather than by timestamp order.
 * @param sample_ns Instant the sample was taken; the record's timestamp.
 * @param seq Publish sequence of the sample carrying the edge.
 * @param bits Pad bits after the edge.
 * @param bits_delta Bits that changed against the previous edge's sample.
 * @param analog_delta Nonzero when an analog axis changed; stored as flag 1. */
void openpete_pace_input_edge(int64_t sample_ns, unsigned seq, uint16_t bits,
                              uint16_t bits_delta, int analog_delta);

/** @brief Write a WINDOW record marking the begin or end of a span during which the pacing grid does not apply.
 *
 * The record carries the id in `aux`, flag 1 on begin, 0 on end, and
 * op_now() as its timestamp. No-op without a sink (the ring or an open
 * log); the census is not touched.
 * @param id OP_PACE_W_* span kind.
 * @param begin Nonzero at the span's begin, zero at its end. */
void openpete_pace_window(uint16_t id, int begin);

/** @brief Account one pacer sleep in the census and write a WAKE record.
 *
 * wake - target conflates two opposite cases: the OS oversleeping past a
 * future deadline, and a deadline that had already passed when the sleep
 * was requested, which is the loop running behind rather than the
 * scheduler. The OS's share is therefore measured as
 * wake - max(target, request) and the lag as max(0, request - target). The
 * census keeps the maximum of each and counts every wake whether or not a
 * log is open. The record carries wake_ns as its timestamp, the target in
 * `a`, the OS share in `b` and the lag in `c`.
 * @param request_ns Instant the sleep was requested.
 * @param target_ns Deadline the sleep aimed at.
 * @param wake_ns Instant the sleep returned. */
void openpete_pace_wake(int64_t request_ns, int64_t target_ns, int64_t wake_ns);

/** @brief Record how long a present occupied the calling thread, start to finish.
 *
 * The PRESENT record's block times cover only the calls that block. The rest
 * of the present path (rebuilding the interpolated scene, recording the
 * command buffer, submitting) is CPU work on the same thread, and under the
 * host split that thread owns the sim clock, so this time can make a tick
 * late; the PRESENT_SPAN record attributes it. Emitted by the host loop for
 * canonical and intermediate presents and by eager production for each
 * scene it builds. No-op without a sink (the ring or an open log).
 * @param kind OP_PACE_CANONICAL or OP_PACE_INTERMEDIATE.
 * @param start_ns Start instant.
 * @param end_ns End instant.
 * @param extract_ns Time spent rebuilding the scene, the largest part and
 *                   the one a shed avoids. */
void openpete_pace_present_span(uint8_t kind, int64_t start_ns,
                                int64_t end_ns, int64_t extract_ns);

/** @brief Account one iteration of the host pump loop, the thread that owns the window.
 *
 * Under the thread split no other record is stamped on that thread, yet a
 * Win32 modal resize or move (the OS loop runs inside the wait), a
 * fullscreen toggle, and a gamepad hotplug each stall input publication and
 * the window ops the other threads queued there. The instants split the
 * iteration into the park (start to wait_end, SDL_WaitEventTimeout, the
 * loop idling) and three pieces of work: event pump, input, and the rest;
 * active time is end_ns - wait_end_ns. The census takes every iteration.
 * With a log open, an iteration whose active time reaches 1 ms or whose park
 * reaches 4 ms gets its own HOST_PUMP record (OP_PACE_HP_PARKED set for the
 * park case, OP_PACE_HP_WINDOW for a window op), and every 100 ms a record
 * flagged OP_PACE_HP_SUMMARY carries the window's iteration count, active
 * sum and max, event-pump max, park sum, and in `e` how many of its
 * iterations were recorded individually. The selection is on duration, not
 * phase, so every long iteration is kept. Called only from the main thread;
 * the window accumulators take no lock.
 * @param start_ns Iteration start, before the wait.
 * @param wait_end_ns When the event wait returned.
 * @param pump_end_ns When the event pump finished.
 * @param input_end_ns When input publication finished.
 * @param end_ns Iteration end.
 * @param window_op Nonzero when the iteration performed a window op. */
void openpete_pace_host_pump(int64_t start_ns, int64_t wait_end_ns,
                             int64_t pump_end_ns, int64_t input_end_ns,
                             int64_t end_ns, int window_op);

/** @brief Open the pad-read bracket for the VBL the guest is about to run.
 *
 * Stores the VBL index and its two instants, zeroes the VBL's pad-read
 * count, opens the bracket and clears a suspension. The pad-read probe fires
 * from inside the dispatcher, which does not know the pacing grid; the
 * stored instants let each PAD_READ record carry its own offsets into the
 * VBL period without a join against a VBL record not yet written.
 * @param vbl_index     Index of the VBL about to run.
 * @param exec_start_ns Instant the VBL's guest execution starts.
 * @param latch_ns      Input latch instant stamped for the VBL, 0 when none. */
void openpete_pace_vbl_begin(uint32_t vbl_index, int64_t exec_start_ns,
                              int64_t latch_ns);

/** @brief Suspend or resume the VBL bracket for fiber runs that are not the bracketed VBL.
 *
 * Runahead speculates further fiber runs inside one bracket, and rewind
 * replays past ticks before the next bracket opens. Both fire the guest's
 * pad read, and a record stamped against the committed VBL's exec_start
 * would carry an offset into a period that read never belonged to. Reads
 * while suspended are recorded with OP_PACE_PR_UNATTRIBUTED and kept out of
 * the phase statistics.
 * @param on Non-zero to suspend, zero to resume. */
void openpete_pace_vbl_suspend(int on);

/* PAD_READ flags. */
enum {
    /* The read did not happen inside the VBL the record names: speculation,
     * rewind replay, or before any bracket was opened. Counted, never folded
     * into the read-phase distribution. */
    OP_PACE_PR_UNATTRIBUTED = 1u << 0
};

/** @brief Account one guest read of its pad buffer.
 *
 * The dispatcher calls it at the entry of the game function that consumes
 * the buffer PsyX_Pad_InternalPadUpdates wrote: the moment that decides
 * which host sample the tick plays, which on hardware is a
 * software-initiated SIO0 transaction with no tie to vblank. Counted whether
 * or not a log file is open; the count feeds the VBL record's pad_reads
 * field. A read outside an open, unsuspended VBL bracket counts as
 * unattributed and is recorded with OP_PACE_PR_UNATTRIBUTED. Inside the
 * bracket, the first read of the VBL sets the census bounds on its offset
 * from exec start and on the latched sample's age; every read gets a
 * PAD_READ record with its ordinal (saturated to u16), the exec start and
 * the latch instant. Game-fiber thread only. */
void openpete_pace_pad_read(void);

/** @brief Return the pad reads attributed to the current VBL bracket, saturated to u16.
 *
 * Zero is ambiguous without openpete_pace_pad_probe_armed: an unarmed probe
 * reports zero reads too.
 * @return Reads since the last openpete_pace_vbl_begin. */
uint16_t openpete_pace_pad_reads_this_vbl(void);

/** @brief Return nonzero once a pad-read probe address has been armed.
 *
 * Without it every tick reports zero reads, which is a missing measurement,
 * not evidence that the game never read the pad; the census line prints
 * "no probe" then.
 * @return 1 when armed, else 0. */
int  openpete_pace_pad_probe_armed(void);
/** @brief Mark the pad-read probe armed; run.c calls it once it has set the probe's guest address. */
void openpete_pace_pad_probe_arm(void);

/* ---- per-tick CPU split, planner and GPU records ------------------------ */

/** @brief Return nonzero while any sink takes records (the ring or a log file), so the split's clock reads are worth taking.
 * @return 1 while a sink is on, else 0. */
int  openpete_pace_split_on(void);

/** @brief Add a measured duration to one TICK_SPLIT accumulator; callable from any thread.
 *
 * The extract, render, present and snap accumulators also count one call
 * each. A relaxed atomic add; no-op without a sink.
 * @param which OP_PACE_SPLIT_* slot; out of range is ignored.
 * @param ns Duration in nanoseconds; negative adds 0. */
void openpete_pace_split_add(int which, int64_t ns);

/** @brief Write the TICK_SPLIT record of the interval ending at this drawing VBL and zero the accumulators.
 *
 * Sim thread, after the drawing VBL's fiber run. No-op without a sink.
 * @param vbl_index openpete_frame of the drawing VBL.
 * @param tick_ns The drawing VBL's own fiber run. */
void openpete_pace_tick_split(uint32_t vbl_index, int64_t tick_ns);

/** @brief Write one EAGER_PLAN record (the fields of the OPENPETE_EAGER_DBG plan line).
 * @param vbl_index openpete_frame at the plan.
 * @param t_ns The plan instant.
 * @param flags OP_PACE_EP_* bits.
 * @param level The span's divisor level.
 * @param std_level The standing divisor level.
 * @param slots The span's slot count.
 * @param afford The scenes the budget affords.
 * @param planned The scenes planned.
 * @param period_ns The planned grid period.
 * @param budget_ns The production budget.
 * @param tail_ns The per-scene tail cost.
 * @param lag_ns The animation lag.
 * @param vbl_ns The non-drawing VBL's mean cost.
 * @param late_ns The tick's start lateness. */
void openpete_pace_eager_plan(uint32_t vbl_index, int64_t t_ns, uint8_t flags,
                              int level, int std_level, int slots, int afford,
                              int planned, int64_t period_ns, int64_t budget_ns,
                              int64_t tail_ns, int64_t lag_ns, int64_t vbl_ns,
                              int64_t late_ns);

/** @brief Write one GPU_FRAME record for a harvested frame's timestamp zones.
 * @param token The frame's RHI token.
 * @param frame_ns First zone start to last zone end.
 * @param busy_ns Sum of the depth-0 zone durations.
 * @param zones Zone count. */
void openpete_pace_gpu_frame(uint64_t token, int64_t frame_ns, int64_t busy_ns, int zones);

/** @brief Return nonzero when OPENPETE_BUGREPORT_GPU asks for GPU_FRAME records (read once).
 * @return 1 when set to a nonzero value, else 0. */
int  openpete_pace_gpu_records_wanted(void);

#if defined(__GNUC__) || defined(__clang__)
typedef struct { int which; int64_t t0; } op_pace_split_guard;
/** @brief Cleanup handler of OP_PACE_SPLIT_SCOPE: add the scope's elapsed time to its accumulator.
 * @param g The scope's guard; t0 is 0 when no sink was on at entry. */
void op_pace_split__cleanup(op_pace_split_guard* g);
#define OP_PACE_SPLIT__CAT2(a, b) a##b
#define OP_PACE_SPLIT__CAT(a, b)  OP_PACE_SPLIT__CAT2(a, b)
/* Times the rest of the enclosing block into the OP_PACE_SPLIT_* slot. */
#define OP_PACE_SPLIT_SCOPE(slot) \
    op_pace_split_guard OP_PACE_SPLIT__CAT(op_pace_sg_, __LINE__) \
        __attribute__((cleanup(op_pace_split__cleanup))) = \
        { (slot), openpete_pace_split_on() ? op_pace_split_now() : 0 }
#else
#define OP_PACE_SPLIT_SCOPE(slot) ((void)0)
#endif
/** @brief Return op_now() for the split scopes, without including openpete_time.h in every user.
 * @return The op_now() instant in nanoseconds. */
int64_t op_pace_split_now(void);

/* ---- coarse session ring ----------------------------------------------- */

/* One row per second of the session (a row covers span_s seconds; when the
 * rows are full, adjacent pairs merge and span_s doubles), folded from the
 * records pace_emit sees, so the rows cover what the 30 s ring has let go. */
#define OP_PACE_COARSE_ROWS    8192u
#define OP_PACE_COARSE_VERSION 1u
#define OP_PACE_COARSE_MAGIC   "OPPCRS01"
enum {
    OP_PACE_CR_T0_S = 0,      /* row start, seconds since the ring was armed */
    OP_PACE_CR_SPAN_S,        /* seconds the row covers */
    OP_PACE_CR_VBLS,
    OP_PACE_CR_VBLS_NOT_LIVE,
    OP_PACE_CR_PRESENTS,      /* canonical and intermediate */
    OP_PACE_CR_REPEATS,
    OP_PACE_CR_SHEDS,
    OP_PACE_CR_IV_MAX_US,     /* longest present-to-present interval ending in the row */
    OP_PACE_CR_IV_LONG,       /* intervals over 25 ms */
    OP_PACE_CR_TICKS,         /* TICK_SPLIT records */
    OP_PACE_CR_TICK_US_SUM,   /* drawing VBL fiber run */
    OP_PACE_CR_TICK_US_MAX,
    OP_PACE_CR_SIM_US_SUM,
    OP_PACE_CR_EXTRACT_US_SUM,
    OP_PACE_CR_TERRAIN_US_SUM,
    OP_PACE_CR_MOBY_US_SUM,
    OP_PACE_CR_SNAP_US_SUM,   /* both legs */
    OP_PACE_CR_RENDER_US_SUM,
    OP_PACE_CR_PRESENT_US_SUM,
    OP_PACE_CR_EXTRACTS,
    OP_PACE_CR_PLANS,         /* EAGER_PLAN records */
    OP_PACE_CR_PLAN_SLOTS,
    OP_PACE_CR_PLAN_SCENES,
    OP_PACE_CR_PLAN_CUTS,
    OP_PACE_CR_LEVEL_MAX,
    OP_PACE_CR_GPU_FRAMES,
    OP_PACE_CR_GPU_US_SUM,
    OP_PACE_CR_GPU_US_MAX,
    OP_PACE_CR_SLIPS,
    OP_PACE_CR_RESERVED0,
    OP_PACE_CR_RESERVED1,
    OP_PACE_CR_RESERVED2,
    OP_PACE_CR_FIELDS         /* 32 u32 per row, 128 B */
};

/** @brief Build the coarse session ring's image on the heap: a 40-byte header and the rows from the first to the newest.
 *
 * Header: magic OP_PACE_COARSE_MAGIC (8 B), u32 version, u32 row bytes,
 * u32 rows, u32 span_s, i64 the ring's creation time in Unix ns, i64 the
 * image instant in ns since the ring was armed. Copied under the writer
 * lock.
 * @param out Receives the image; free with free().
 * @param len Receives its length.
 * @param rows Receives the row count; may be NULL.
 * @return 0 on success, -1 when the ring is not armed or allocation fails. */
int  openpete_pace_coarse_image(void** out, size_t* len, size_t* rows);

/* ---- census (always compiled, no log file needed) ----------------------- */

#define OP_PACE_HIST_BUCKETS 9

typedef struct {
    uint64_t vbls;              /* VBL records seen                          */
    uint64_t vbls_paced;        /* ... of which the deadline actually applied */
    uint64_t vbls_clean;        /* ... paced AND not paused/stepped/rewound/
                                 * loading: the pacing-evidence population    */
    uint64_t vbls_transient;    /* paced but flagged: reported, not folded in */
    uint64_t presents[3];       /* canonical, intermediate, repeat            */
    uint64_t sheds;
    /* Display cadence, which is what a viewer perceives and what a shed count
     * cannot show: a shed is only a defect if it left a hole. `gaps_mid_long`
     * counts holes of more than two present slots that end at an INTERMEDIATE
     * present - the schedule's own doing. Holes ending at a canonical present
     * are the tick executing on the presenting thread, unreachable by any
     * schedule, so they are reported separately as `hold_ema_ns` rather than
     * mixed into a count that then never goes to zero. */
    uint64_t gaps_mid_long;
    int64_t  hold_ema_ns;
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
    uint64_t vbls_claimed;      /* consumed no host sample: another writer   */
    uint64_t input_edges;       /* pad edges the producer published          */
    uint64_t latch_samples;     /* VBLs that contributed one: 0 means the
                                 * bound is unmeasured, not perfect          */
    uint64_t pad_reads;         /* guest reads of its pad buffer             */
    uint64_t pad_read_vbls;     /* VBLs in which at least one read fired     */
    uint64_t pad_reads_unattributed; /* speculative / replayed: not this VBL  */
    int64_t  pad_read_offset_max_ns; /* worst first-read delay into the VBL   */
    int64_t  pad_read_age_max_ns;    /* worst age of the sample when read     */
    /* Host pump loop (thread split only; zero iterations means the loop never
     * ran, which is the single-threaded path, not a perfect result). `active`
     * excludes the park, which is the loop idling rather than a cost. */
    uint64_t host_pump_iters;
    uint64_t host_pump_long;         /* ... of which crossed the record floor */
    int64_t  host_pump_active_ns;    /* summed active time                    */
    int64_t  host_pump_park_ns;      /* summed park: a loop that is not
                                      * sleeping shows up here and nowhere
                                      * else, since spinning costs a core
                                      * without lengthening any iteration    */
    int64_t  host_pump_max_ns;       /* worst single iteration                */
    int64_t  host_pump_park_max_ns;  /* longest park: a wait that held the
                                      * thread past its timeout is a stall,
                                      * however little work it did          */
    int64_t  host_pump_events_max_ns;/* worst event pump alone: modal loops   */
} openpete_pace_census_t;

/** @brief Copy the current pacing census into out.
 *
 * The copy is a plain struct read with no lock. The sim and present threads
 * update the counters concurrently, some under the census lock and some
 * without it, so a snapshot can mix values from adjacent updates.
 * @param out Receives the snapshot; nothing happens when NULL. */
void openpete_pace_census_get(openpete_pace_census_t* out);

/* Upper bound (ns, exclusive) of each phase-error histogram bucket; the last
 * bucket is unbounded. Shared with the analyzer so both sides bucket alike. */
extern const int64_t openpete_pace_hist_edges_ns[OP_PACE_HIST_BUCKETS];

/** @brief Format the end-of-run pacing census as one summary line into `buf`.
 *
 * When no VBL was paced against a wall deadline, or none of the paced
 * VBLs was clean, the line reads "unpaced" with VBL, present and slip
 * counts and no phase error. Otherwise it carries the VBL and present
 * counts, the share of clean paced VBLs that started within 250 us, the
 * phase error bounds, the latch bound (n/a when unmeasured), the pad-read
 * bounds ("no probe" when the probe is unarmed, "NEVER" when it is armed and
 * never fired), slips, sheds, host pump timings when a separate pump thread ran, and an
 * input seqlock failure count when non-zero.
 * @param buf Destination buffer.
 * @param buf_bytes Capacity of `buf`; the line is truncated to fit. */
void openpete_pace_census_format(char* buf, unsigned buf_bytes);

/* ---- instrument self-verification --------------------------------------- */

/** @brief Busy-wait a configured stall before the game fiber runs, to calibrate the phase-error measurement.
 *
 * OPENPETE_PACE_INJECT_US=N[,EVERY[,FROM[,COUNT]]] blocks N µs on paced VBL
 * FROM (counted from 0 over paced VBLs; default 0) and on every EVERY-th
 * paced VBL after it (EVERY defaults to 30), COUNT times in all (0, the
 * default: without end). The log must then show a phase error of about N
 * µs on those VBLs and nowhere else: a known input with a known answer.
 * Under the virtual clock the stall is an advance of the clock by N µs
 * (op_sleep_until), so a stall longer than the slip threshold makes the
 * pacer discard time as a stalled host does. The variable is read once.
 * Inactive on unpaced iterations. Call once per iteration, immediately
 * before the yield.
 * @param paced Non-zero when this iteration runs against a live deadline.
 * @return The µs blocked, 0 when nothing was injected. */
int64_t openpete_pace_inject(int paced) OP_REQUIRES(sim);

/** @brief Spend OPENPETE_PACE_INJECT_PRESENT_US=N[,M[,K]] µs in an intermediate present.
 *
 * Inflating the measured cost of intermediate presents until they no longer
 * fit before the tick deadline makes the shed predicate fire, so the
 * contract that presents are shed while ticks hold phase can be measured.
 * With M the cost switches between N and M every K calls (K defaults to 1,
 * alternating per call): alternation gives the cost estimate a non-zero
 * deviation, and blocks of K calls move it between regimes in which the
 * predicate admits strictly, admits late, sheds, and probes.
 * OPENPETE_PACE_INJECT_PRESENT_VBL=FROM[,TO] limits the injection to presents
 * made while openpete_frame is at least FROM and below TO (TO 0 or absent:
 * without end); a present outside the window spends nothing and does not
 * advance the N/M switch.
 * The inline intermediate present and the eager producer's intermediate
 * scene each spend it inside the span logged as that present's
 * PRESENT_SPAN cost. Off the virtual clock the cost is a busy-wait; under
 * it the virtual clock is advanced by the cost.
 * The variable is read once and a warning logged when a value is positive.
 * @return The µs spent, 0 when nothing was injected. */
int64_t openpete_pace_inject_present(void);

/** @brief Busy-wait OPENPETE_PACE_INJECT_CANON_US=N µs in every canonical present.
 *
 * A canonical present cannot be shed, since it shows the image the tick just
 * produced. Injecting cost into it reproduces, on any machine, the case where
 * the unavoidable present does not fit before the tick deadline, so whether
 * the sim grid holds there can be measured. It is a separate variable from
 * OPENPETE_PACE_INJECT_PRESENT_US so the shed check calibrated against
 * intermediate presents keeps its baseline. The variable is read once;
 * inactive under the virtual clock.
 * @return The µs spent, 0 when nothing was injected. */
int64_t openpete_pace_inject_canonical(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PACE_LOG_H */
