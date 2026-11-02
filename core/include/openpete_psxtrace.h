/* core/include/openpete_psxtrace.h - streaming .psxtrace v2 oracle verifier.
 *
 * Opens a trace produced by scripts/pcsx-redux-trace.lua and on each
 * advance() reads the next per-frame record's hash array, computes the
 * same hashes from openpete's live RAM via gameplay_hash_compute_ex(),
 * and value-equality-compares them. First divergence is latched: the
 * caller can read first_divergent_* + the oracle/op hash pair to fail.
 *
 * Format spec: tools/psxtrace_format.py.
 *
 * v2 always carries hashes; an optional RAM payload (header flag bit 1)
 * enables per-byte attribution when a hash mismatch fires. Header flag
 * bit 2 adds the FNV-1a-64 of the region table the capture hashed with,
 * bit 3 the git hash of the capturing DuckStation build; open() refuses a
 * trace without bit 2, one whose table hash or region names differ from
 * the registered table, and one carrying a flag bit this reader does not
 * know (its header block would misalign the records).
 */

#ifndef OPENPETE_PSXTRACE_H
#define OPENPETE_PSXTRACE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENPETE_PSXTRACE_REGION_NAME_MAX  64
#define OPENPETE_PSXTRACE_MAX_REGIONS      128

typedef struct {
    FILE*       fp;
    const char* path;
    uint32_t    version;
    uint32_t    frame_count;
    uint32_t    flags;
    uint32_t    region_count;
    uint32_t    records_consumed;

    /* PC of the first record - captured during open() by peeking +
     * rewinding. This is the PC the trace was anchored at; the verifier
     * uses it as the default for openpete_psxtrace_anchor_pc, so the
     * caller doesn't have to thread a per-game constant through core/. */
    uint32_t    inferred_anchor_pc;

    /* Region names from the trace header. Used for the divergence message. */
    char region_names[OPENPETE_PSXTRACE_MAX_REGIONS]
                     [OPENPETE_PSXTRACE_REGION_NAME_MAX];
    /* Header blocks: the table hash (flag bit 2) and the capturing build's
     * git hash (flag bit 3; empty string when absent). */
    uint64_t table_hash;
    char     scm_hash[64];

    /* Latched on first divergence of ANY kind (incl. tolerated CD-load
     * transients). Kept for diagnostics / the per-region count summary. */
    int      diverged;
    uint32_t first_divergent_frame;
    int32_t  first_divergent_region;     /* -1 = total-only mismatch */
    uint64_t first_divergent_oracle;
    uint64_t first_divergent_op;
    /* First divergence in a row that is not gameplay_neutral (a strict
     * or cd_load_transient row); the neutral rows diverge by
     * classification and would otherwise own the first-divergence report. */
    /* Under the schedule: the reference's VBLs minus openpete's, cumulative,
     * the records it has been non-zero for, and the long frames seen. */
    int64_t  vbl_deficit;
    uint32_t vbl_deficit_records;
    uint32_t vbl_long_frames;
    /* Every record's VBL stamp (pad2), read ahead at open for the schedule. */
    uint32_t* stamps;
    uint32_t  stamp_count;
    /* Every record's anchor tick (pad1, header flag bit 5: CPU ticks from
     * the stamped VBLANK to the hit), read ahead with the stamps; NULL
     * when the trace carries no anchor ticks. */
    uint32_t* anchor_ticks;
    uint32_t records_skipped;     /* reference records passed over at load-window ends */
    uint32_t records_uncompared;  /* records consumed inside a load window, not compared */
    int      strict_diverged;
    uint32_t first_strict_divergent_frame;
    int32_t  first_strict_divergent_region;

    /* The gate verdict. Set when a region that is NOT cd_load_transient
     * diverges, OR a cd_load_transient region diverges and fails to
     * re-converge within `heal_window` ticks (or is still diverging at
     * EOF). This - not `diverged` - drives the process exit code and the
     * non-keep-going early abort. A run whose only divergences are
     * healing CD-load transients leaves hard_failed == 0. */
    int      hard_failed;
    uint32_t hard_fail_frame;
    int32_t  hard_fail_region;           /* -1 = none latched */
    uint64_t hard_fail_oracle;
    uint64_t hard_fail_op;
    const char* hard_fail_reason;        /* "non-transient" | "unhealed" */

    /* Per-region CD-load-transient heal tracking. transient_open_frame[r]
     * is the tick at which the current unhealed transient burst in region
     * r began, or -1 if r is currently matching. Tolerance window in
     * ticks (default 600, env OPENPETE_PSXTRACE_HEAL_WINDOW). */
    int64_t  transient_open_frame[OPENPETE_PSXTRACE_MAX_REGIONS];
    uint64_t transient_healed_bursts;    /* cd_load_transient bursts that healed */
    uint64_t neutral_bursts;             /* gameplay_neutral / host_precision bursts */
    uint32_t heal_window;

    /* Per-record VBL stamps (header flag bit 4: the trace's pad2 is the
     * reference's VBLANK count at the hit; openpete stamps its own
     * `openpete_vbl` at the same hit). Absolutes differ by the boot offset,
     * so the per-record deltas are compared. OPENPETE_PSXTRACE_VBL_POLICY:
     * `exact` hard-fails the first differing delta (reason "vbl-delta"),
     * `log` (default) counts them. Both sides' delta histograms are kept
     * for the summary; bin 16 collects every delta >= 16. */
    int      has_vbl_stamps;
    int      vbl_policy_exact;
    uint32_t prev_trace_vbl;
    int32_t  prev_op_vbl;
    uint64_t vbl_delta_mismatches;
    uint32_t first_vbl_mismatch_frame;
    int32_t  first_vbl_mismatch_trace, first_vbl_mismatch_op;
    uint64_t vbl_hist_trace[17];
    uint64_t vbl_hist_op[17];

    /* Engagement: records on which each region's oracle hash differed from
     * the previous record's. A region at zero over the run is named in the
     * summary; without `constant_ok` on its manifest row the summary line
     * reads `zero-change regions unjustified:` and the gate script fails. */
    uint64_t per_region_change_count[OPENPETE_PSXTRACE_MAX_REGIONS];
    uint64_t prev_oracle_per[OPENPETE_PSXTRACE_MAX_REGIONS];

    /* Counts since open. */
    uint64_t frames_compared;
    uint64_t hashes_diverged;            /* per-region mismatches across run */
    uint64_t per_region_diverge_count[OPENPETE_PSXTRACE_MAX_REGIONS];

    /* If header has RAM payload, last record's RAM is staged here and the
     * caller can byte-diff against live g_ram for attribution. NULL when
     * the trace is hash-only (typical). */
    uint8_t* ram_buf;                    /* malloc'd once, reused */

    /* Coarse terminal verdict written from the dispatch hook so the host
     * loop can observe it without sharing the file pointer:
     *   0 = still running, 1 = EOF reached, 2 = divergence latched
     *   (caller decides whether to honour --psxtrace-keep-going), 3 =
     *   I/O / parse error. */
    int terminal_status;
} openpete_psxtrace_t;

/** @brief Open a reference trace, validate its header against the registered region table, and prime the verifier.
 *
 * Zeroes t, then reads OPENPETE_PSXTRACE_HEAL_WINDOW (ticks, default 600)
 * and OPENPETE_PSXTRACE_VBL_POLICY (`exact` or `log`; any other value
 * fails). The header must carry the PSXTRACE magic, version 2, the PSX RAM
 * size, no unknown flag bits, and a region count that is nonzero, within
 * OPENPETE_PSXTRACE_MAX_REGIONS and equal to the registered table's. The
 * region names must equal the table's row for row, and the header must
 * carry a table hash equal to the registered one. The flagged header
 * blocks (table hash, build id) are read after the names. Opens the
 * CD-ROM MMIO sidecar and the schedule sidecars beside the trace; when a
 * schedule loads and the trace carries VBL stamps, the delta compare is
 * exact unless the policy is `log`. The first record's pc becomes
 * inferred_anchor_pc, and in a hash-only trace with stamps every record's
 * stamp is read ahead and handed to the schedule. The file position is left
 * at the first record. A failure logs its reason, closes the file and frees
 * the RAM buffer; the schedule and CD-ROM MMIO sidecars stay loaded.
 * @param t Verifier state to fill; path is borrowed, not copied.
 * @param path Path of the golden trace.
 * @return 0 on success; -1 on any open, header or table mismatch. */
int openpete_psxtrace_open(openpete_psxtrace_t* t, const char* path);

/** @brief Read the next reference record and compare openpete's region hashes against it.
 *
 * Called at each hash anchor. Checks the record's VBL delta against
 * openpete's when the trace carries stamps, applies the
 * OPENPETE_PSXTRACE_POKE negative control, then hashes every manifest
 * region and compares per region. A mismatch sets @c diverged; a mismatch
 * in a region that is neither transient nor neutral, a CD-load transient
 * that outlives @c heal_window, a VBL-delta mismatch under the exact
 * policy, or a failed schedule sets @c hard_failed, which decides the exit
 * code. Inside a scheduler load window the record is consumed without a
 * compare.
 * @param t Trace opened by openpete_psxtrace_open().
 * @return 1 when a record was consumed, 0 at EOF, -1 on a parse or I/O
 *         error. */
int openpete_psxtrace_advance(openpete_psxtrace_t* t);

/** @brief Settle the trace compare after the last openpete_psxtrace_advance() and log its PASS/FAIL line.
 *
 * When every record was consumed: a region whose divergence count exceeds
 * its manifest row's diverge_baseline sets @c hard_failed (reason
 * "ratchet"), and a CD-load transient still open sets it (reason
 * "unhealed-eof"). When the run ended before the trace did, an open
 * transient is logged as unverified and not promoted. An open burst in a
 * gameplay_neutral or host_precision region is logged and never promoted.
 * Then logs the scheduler and CD-ROM MMIO summaries, the first hard
 * failure, and the `verdict:` line with the absorbed-transient and
 * neutral-burst counts.
 * @param t Trace opened by openpete_psxtrace_open(). */
void openpete_psxtrace_finalize(openpete_psxtrace_t* t);

/** @brief Close the trace file and free the RAM payload buffer and the VBL stamp array.
 *
 * The pointers are cleared, so a second call is harmless. The schedule
 * holds the stamp array by pointer and is not told; it stays active with
 * a dangling array until the process ends.
 * @param t Trace opened by openpete_psxtrace_open(). */
void openpete_psxtrace_close(openpete_psxtrace_t* t);

/** @brief Load the reference's schedule from the sidecars beside a golden and activate it.
 *
 * The schedule is the `<trace>.vsync`, `<trace>.loads` and `<trace>.srand`
 * sidecars, consumed in order by the thunk layer while the trace is
 * verified; `.watch`, `.padcmd`, `.sio0` and `.cdsync` are loaded when
 * present. The position starts at the first waited VSync's entry or the
 * first read's issue, whichever is earlier. OPENPETE_SCHED_LOG=path logs
 * every consumed row to path.
 * @param trace_path Path of the golden trace; sidecar paths append their
 *        suffix to it.
 * @return 1 when a .vsync, .loads or .srand sidecar loaded and the schedule
 *         is active, else 0. */
int  openpete_sched_open(const char* trace_path);   /* 1 when any sidecar loaded */
/** @brief Report whether a reference schedule is loaded.
 *
 * Set by openpete_sched_open() and never cleared; a schedule that later
 * failed (openpete_sched_failed) stays active.
 * @return 1 when openpete_sched_open() loaded a schedule, else 0. */
int  openpete_sched_active(void);
/** @brief Report which sidecars of the schedule proper openpete_sched_open() read.
 *
 * Bit 0 is `<trace>.vsync`, bit 1 `<trace>.loads`, bit 2 `<trace>.srand`;
 * a sidecar that is absent, unreadable or carries the wrong magic leaves
 * its bit clear. 0 before openpete_sched_open().
 * @return The bit mask of the sidecars read. */
unsigned openpete_sched_core_loaded(void);
/* The schedule tracks the reference VBLANK count openpete has reached
 * (`pos`). Each consumer returns a target; the thunk layer charges
 * `target - pos` VBLs (one psyq_vbl_body each) and advances pos. */
/** @brief Give the schedule the reference's VBLANK stamp of every trace record.
 *
 * The stamps are the trace's pad2 column, read ahead by the verifier. The
 * array is borrowed, not copied.
 * @param stamps Stamp per record index.
 * @param count Records in stamps. */
void     openpete_sched_set_stamps(const uint32_t* stamps, uint32_t count); /* the trace's pad2 column */
/** @brief Give the schedule the reference's anchor tick of every trace record.
 *
 * The ticks are the trace's pad1 column under header flag bit 5: the CPU
 * ticks from the VBLANK the record's stamp counts to the anchor hit. The
 * array is borrowed, not copied, and indexed like the stamps.
 * @param ticks Anchor tick per record index.
 * @param count Records in ticks. */
void     openpete_sched_set_anchor_ticks(const uint32_t* ticks, uint32_t count);
/** @brief Return the reference VBLANK and anchor tick of a record.
 * @param rec Record index.
 * @param vbl_out Receives the record's VBLANK stamp.
 * @param tick_out Receives the CPU ticks from that VBLANK to the anchor hit.
 * @return 1 when the trace carries both for rec, else 0. */
int      openpete_sched_anchor(uint32_t rec, uint32_t* vbl_out, uint32_t* tick_out);
/** @brief Return the reference's start of the card transfer the driver began on a VBLANK.
 *
 * The `.card` sidecar holds one row per transfer start: the CPU ticks from
 * the VBLANK to the store that marked the transfer active, which the
 * read, write and info handlers' first state makes as the select byte goes
 * out.
 * @param vbl Reference VBLANK the card step ran on.
 * @param off_out Receives the start's tick offset from that VBLANK.
 * @return 1 when the sidecar has a start on vbl, else 0. */
int      openpete_sched_card_start(uint32_t vbl, uint32_t* off_out);
/** @brief Return the reference's step ticks of the card transfer started on a VBLANK.
 *
 * A step is one SIO0 interrupt the card driver serviced after the select
 * byte: a handler state (the store of its byte counter) or a fast-track
 * byte (the store of the fast-track count), as ticks from the store that
 * marked the transfer active, in order.
 * @param vbl Reference VBLANK the card step ran on.
 * @param n_out Receives the number of steps.
 * @return The step ticks, or NULL when the sidecar row carries none. */
const uint32_t* openpete_sched_card_steps(uint32_t vbl, uint32_t* n_out);
/** @brief Return the reference VBLANK count openpete has reached.
 *
 * The position is set when the schedule opens or a consumer first meets it
 * (openpete_sched_pos_known) and advances by openpete_sched_advance_pos.
 * @return The position; 0 until it is known. */
uint32_t openpete_sched_pos(void);

/** @brief Advance the position by vbls reference VBLANKs.
 *
 * Called by the thunk layer after it runs the VBLs a consumer charged. The
 * position moves whether or not it was known.
 * @param vbls VBLANKs to add. */
void     openpete_sched_advance_pos(uint32_t vbls);

/** @brief Count one VBLANK the position advanced over that the CPU did not take (D).
 *
 * Called for a VBLANK a critical section holds. The libetc counter the
 * schedule answers with is position - K - D at every conversion. */
void     openpete_sched_drop_vbl(void);

/** @brief Uncount one dropped VBLANK: the latched VBLANK was taken when the critical section ended. */
void     openpete_sched_take_dropped(void);

/** @brief Return D, the VBLANKs the position counts that the CPU has not taken.
 * @return D; 0 from anchor 0's rebase until a hold. */
int32_t  openpete_sched_dropped(void);

/** @brief Decide what a polling-loop quantum delivers under the schedule: the next VBLANK the reference's rows show taken before its next row point, or nothing.
 *
 * Nothing is due before the first anchor hit, inside a load window, after
 * a schedule failure or past the trace. On the clock row the record's next
 * pending `.vblpos` row is consumed and its VBLANK is the target; on the
 * default row the target is the position plus one when that is at or below
 * the bound the record's next `.vsync` row puts on in-work VBLANKs. Every
 * call is counted (delivered or no row due) for the loop summary line.
 * @param op_insns openpete's classified-loop instructions since the
 *        record's anchor, paired with the clock row's `insns` column.
 * @param target_out Receives the VBLANK to charge to.
 * @return 1 when a VBLANK is due, else 0. */
int      openpete_sched_loop_due(uint64_t op_insns, uint32_t* target_out);

/** @brief Restore D from a savestate.
 * @param d The dropped count the state carried. */
void     openpete_sched_set_dropped(int32_t d);

/** @brief Read the conversions made while D was nonzero, per site, and those that broke counter = position - K - D.
 * @param hits Receives the counts: [0] wait return, [1] VSync(-1) answer, [2] replay row skip, [3] charge target.
 * @param bad Receives the mismatches per site. */
void     openpete_sched_conv_report(uint32_t hits[4], uint32_t bad[4]);

/** @brief Report whether the position has been set.
 *
 * The position is set at openpete_sched_open() to the earlier of the first
 * waited VSync's entry and the first read's issue, or by the first consumer
 * that supplies a VBLANK when neither existed.
 * @return 1 once the position is set, else 0. */
int      openpete_sched_pos_known(void);

/** @brief Answer a VSync(-1) query with the reference's counter for the same read of the record.
 *
 * The answer is the next query row of the current record from call site ra
 * (any site when the sidecar has none); rows before it, whether of an
 * earlier record, behind the position, from another site or from a
 * registration, count as reads openpete did not make, and a waited row
 * ends the search. A row covers a run of equal queries. When the row's
 * VBLANK, its counter plus the capture's constant offset, lies ahead of
 * the position, target_out is that VBLANK and the caller charges up to
 * it. Otherwise, and before the first anchor hit under
 * OPENPETE_SCHED_BOOT_OWN, inside a load window, after a failure, or when
 * no row answers, the answer is the position's counter and target_out is
 * the position.
 * @param ra Call site of the query.
 * @param target_out Receives the reference VBLANK to charge the position
 *        to.
 * @return The counter, or -1 when the schedule is inactive, the position
 *         is unknown, or the sidecar carries no counter. */
int32_t  openpete_sched_query(uint32_t ra, uint32_t* target_out);

/** @brief Return the game-visible counter at the position.
 *
 * The counter is the reference VBLANK count minus the capture's constant
 * offset, the value a waited VSync returns.
 * @return The counter, or -1 when the schedule is inactive, the position is
 *         unknown, or the sidecar carries no counter. */
int32_t  openpete_sched_counter(void);

/** @brief Charge the VBLs before a callback registration to the reference's VBLANK for it.
 *
 * Matches the next registration row of the current record or an earlier
 * one, from call site ra when the sidecar records sites, and marks it
 * consumed. Rows before it stay for their own consumers.
 * @param ra Call site of the registration.
 * @param target_out Receives the registration's VBLANK.
 * @return 1 with target_out set when the row's VBLANK lies ahead of the
 *         position; 0 when the schedule is inactive, failed or without a
 *         position, inside a load window, past the trace's last record,
 *         when no row of the record matches, or when the position is not
 *         behind the row. */
int      openpete_sched_registration(uint32_t ra, uint32_t* target_out);

/** @brief Return the reference record index the next anchor hit produces.
 *
 * Anchor hits from the target hit on count from record 0; the load-window
 * re-alignment offset is added and the result is floored at 0.
 * @return The record index. */
uint32_t openpete_sched_current_record(void);
/** @brief Answer a game read of root counter 2's count register from the reference's `.rcnt2` rows.
 *
 * Takes the next unread row of the current record; rows of records
 * openpete has passed are skipped. Without the sidecar, or with no row
 * left in the record, the caller keeps its own value.
 * @param value_out Receives the count the reference read.
 * @return 1 when a row answered, else 0. */
int openpete_sched_rcnt2_read(uint16_t* value_out);
/** @brief Answer a waited VSync call with the reference VBLANK it returns at.
 *
 * The answer is the return VBLANK of the next waited row of the current
 * record or an earlier one; the query rows skipped to reach it count as
 * reads openpete did not make, and a row whose mode differs records a
 * schedule failure. When the reference made no further waited call before
 * its next anchor hit, the target is that record's stamp and no row is
 * consumed; a stamp at or behind the position records a failure and
 * returns 0. Inside a load window, and before the first anchor hit under
 * OPENPETE_SCHED_BOOT_OWN (record 0's rows skipped), the target is the
 * position plus mode (plus 1 for mode 0). The clock of the answering row
 * is kept for openpete_sched_last_tick. After a schedule failure every
 * consumer answers as it would past the trace, so the waits run under
 * openpete's own count and a run kept going ends instead of spinning.
 * @param mode The VSync mode openpete called with.
 * @param target_out Receives the reference VBLANK to charge the position
 *        to.
 * @return 1 with target_out set; 0 when the schedule is inactive or
 *         failed, openpete is past the trace's last record (counted as
 *         past the trace end), no row or stamp answers, or the position
 *         is unknown inside a window or the boot. */
int  openpete_sched_next_vsync(int mode, uint32_t* target_out);
/** @brief Return the reference's VBLANK stamp at anchor hit record and check the position against it.
 *
 * At record 0 the boot, which ran under openpete's own count, ends: the
 * position is set to the stamp. At a later record a position ahead of the
 * stamp outside a load window records a schedule failure, and a position
 * behind it counts a long frame, charged by the next wait.
 * @param record 0-based reference record index.
 * @param target_out Receives the stamp.
 * @return 1 with target_out set; 0 when the schedule is inactive or failed,
 *         carries no stamps, record is past them, or the position is
 *         unknown. */
int  openpete_sched_anchor_target(uint32_t record, uint32_t* target_out);
/** @brief Answer a CdRead with the next read row's issue and completion VBLANKs.
 *
 * A row whose sector count or destination differs from the issued read,
 * or a read past the sidecar's rows inside the trace's records, records a
 * schedule failure and returns 0; a read past the sidecar's rows after the
 * trace's last record is counted as past the trace end and returns 0. The
 * read's next event bound is the earliest of the following read's
 * issue, the first waited VSync entered at or after this issue, and the
 * next anchor stamp. A read with a recorded completion is asynchronous when
 * a waited VSync entered between its issue and completion: the completion
 * is deferred to its VBLANK. Otherwise it is synchronous and completes at
 * the later of its completion and the bound, since the VBLs up to the next
 * event are the tick's own work after the read. A read with no completion
 * callback on the game completes synchronously at the bound, or at its
 * issue when there is none.
 * @param sectors Sectors openpete issued.
 * @param dst Destination address openpete issued.
 * @param issue_vbl_out Receives the reference's issue VBLANK.
 * @param done_vbl_out Receives the completion VBLANK.
 * @param done_rec_out Receives the reference record of the completion, or
 *        of the issue when the read has no completion callback.
 * @param sync_out Receives 1 when the read is charged inside the call, 0
 *        when its completion is deferred.
 * @return 1 with every output set, else 0. */
int  openpete_sched_read(int sectors, uint32_t dst, uint32_t* issue_vbl_out, uint32_t* done_vbl_out, uint32_t* done_rec_out, int* sync_out);
/** @brief Report whether a guest spin may be charged one more VBL: the position is behind the reference's next event.
 *
 * The next event is the next unissued read's issue, the first waited
 * VSync entered at or after the position, or the current record's anchor
 * stamp.
 * A VBL the reference spent in a tick's work before its wait lies below
 * that wait's entry; the wait charges every VBL from its entry on.
 * Off the schedule, inside a load window, before the position is set and
 * after a failure nothing bounds the charge.
 * @return 0 when the position has reached the bound, else 1. */
int  openpete_sched_spin_due(void);
/** @brief Answer an srand call with the reference's next seed.
 *
 * Seeds are consumed in order from the `.srand` sidecar. A call past the
 * last seed records a schedule failure.
 * @param seed_out Receives the seed.
 * @return 1 with seed_out set; 0 when the schedule is inactive, no `.srand`
 *         sidecar loaded, or the seeds are exhausted. */
int  openpete_sched_next_srand(uint32_t* seed_out);
/** @brief Track the load window at an anchor hit and report a record re-alignment when openpete leaves one.
 *
 * The windows come from the golden's `.watch` sidecar under the game's
 * psyq_game_load_window_active predicate; value decides whether openpete is
 * inside one. On leaving a window that the reference left at a later
 * record, the record offset grows by the difference, the VSync rows inside
 * the reference's window are dropped, and skip_to_rec is set; the caller
 * charges the position to that record's stamp and has the verifier skip to
 * it. Leaving a window the reference left earlier, or more windows than the
 * reference saw, records a schedule failure.
 * @param record openpete's current reference record index.
 * @param value The watched word's value at the hit.
 * @param skip_to_rec Receives the reference record where its window ended.
 * @return 1 when a re-alignment is due, else 0. */
int  openpete_sched_anchor_window(uint32_t record, uint32_t value, uint32_t* skip_to_rec);
/** @brief Report whether openpete is inside a reference load window.
 *
 * The value is the one openpete_sched_anchor_window() last computed from
 * the watched word; 0 before its first call that has load windows.
 * @return 1 while inside a load window, else 0. */
int  openpete_sched_in_load(void);
/** @brief Return whether the kernel's controller and card vblank handler ran on reference VBLANK vbl.
 *
 * Read from the `.sio0` sidecar. The BIOS card model holds a pending card
 * timeout back while the reference's handler did not run.
 * @param vbl Reference VBLANK count.
 * @param known Receives 0 when no sidecar covers vbl (none loaded, the
 *        schedule inactive, or outside the recorded range), else 1; may be
 *        NULL.
 * @return 1 when the handler ran or vbl is not covered, else 0. */
int  openpete_sched_sio0_served(uint32_t vbl, int* known);
/** @brief Return whether the controller accepted a command on reference VBLANK vbl.
 *
 * Read from the `.padcmd` sidecar, the vsyncs libpad's command sequence
 * advanced on. Before the first recorded command the game had not started
 * polling, so those vblanks are not covered.
 * @param vbl Reference VBLANK count.
 * @param known Receives 0 when no sidecar covers vbl, else 1; may be NULL.
 * @return 1 when a command was accepted or vbl is not covered, else 0. */
int  openpete_sched_padcmd_at(uint32_t vbl, int* known);
/** @brief Answer a CdSync poll with the reference's value for the same poll of the record.
 *
 * The answer is the next `.cdsync` row of the current record with this mode
 * and call site. Rows of earlier records count as polls openpete did not
 * make; rows of this record with another mode or site ahead of the match
 * are passed over. A row covers a run of n equal polls. The row's clock is kept for openpete_sched_last_tick,
 * which reads 0 when no row answered.
 * @param mode CdSync mode.
 * @param ra Call site of the poll.
 * @param ret_out Receives the reference's return value.
 * @param target_out Receives the VBLANK the reference's poll returned at.
 * @return 1 with both outputs set; 0 when the schedule is inactive or
 *         failed, no `.cdsync` sidecar loaded, or the record has no such
 *         row left. */
int  openpete_sched_cdsync(int mode, uint32_t ra, int32_t* ret_out, uint32_t* target_out);
/** @brief Return the reference's CPU clock at the row the last waited VSync or CdSync poll was answered from.
 *
 * A reference event recorded with a lower clock value, such as a read's
 * completion callback, preceded that call.
 * @return The clock in CPU cycles, 0 when the last answer came from no
 *         row. */
uint64_t openpete_sched_last_tick(void);
/** @brief Return the reference's CPU clock at the completion callback of the read the last openpete_sched_read answered.
 *
 * @return The clock in CPU cycles, 0 when that read has no recorded
 *         completion. */
uint64_t openpete_sched_read_done_tick(void);
/** @brief Report the call site and clock of the next unanswered CdSync row of the current record without consuming it.
 *
 * The thunk layer delivers a deferred read completion when that row is the
 * game's completion-witness call site at or after the completion's clock.
 * @param ra_out Receives the row's call site.
 * @param tick_out Receives the row's clock in CPU cycles.
 * @return 1 with both outputs set; 0 when the schedule is inactive or
 *         failed, or the record has no row left. */
int  openpete_sched_cdsync_peek(uint32_t* ra_out, uint64_t* tick_out);
/** @brief Seek past reference records up to rec, exclusive, so the next advance compares record rec.
 *
 * Each skipped record counts as consumed and skipped. When the trace
 * carries stamps, the previous-VBL stamp follows the skipped records, so
 * the next record's VBL delta is taken against the last skipped one. Stops
 * at the trace's last record. The schedule calls it through
 * openpete_psxtrace_skip_current_to when a load window re-aligns the
 * record index.
 * @param t Trace opened by openpete_psxtrace_open().
 * @param rec Reference record index the next advance reads.
 * @return 0 on success; -1 when the trace is closed, carries RAM payloads
 *         (the seek stride excludes them), or a seek fails. */
int  openpete_psxtrace_skip_to(openpete_psxtrace_t* t, uint32_t rec);
/** @brief Return the record index the active trace's next advance consumes.
 *
 * Defined in run.c against the one trace open per run.
 * @return The number of records consumed, or 0 when no trace is active. */
uint32_t openpete_psxtrace_current_record(void);
/** @brief Skip the active trace forward so its next advance reads record rec.
 *
 * Defined in run.c against the one trace open per run; the thunk layer calls
 * it when a load window re-aligns the record index.
 * @param rec Reference record index the next advance reads.
 * @return The result of openpete_psxtrace_skip_to (0 on success, -1 on
 *         failure), or -1 when no trace is active. */
int      openpete_psxtrace_skip_current_to(uint32_t rec);
/** @brief Record a schedule failure, keeping the first reason.
 *
 * Formats the reason into a 256-byte buffer, truncating, and logs it as an
 * error. Later calls are ignored. Once failed, the consumers answer as they
 * would past the trace.
 * @param fmt printf format of the reason, followed by its arguments. */
void openpete_sched_fail(const char* fmt, ...);

/** @brief Report whether a schedule failure was recorded.
 *
 * @param reason Receives a pointer to the first failure's text, valid for
 *        the process; may be NULL. It is set whether or not a failure was
 *        recorded, and holds an empty string when none was.
 * @return 1 when a failure was recorded, else 0. */
int  openpete_sched_failed(const char** reason);

/** @brief Log one line of the schedule's consumption counters, with the failure reason when one was recorded.
 *
 * Covers waited rows, reads, seeds, the VSync calls and reads past the
 * trace end, queries, registrations, long frames, boot waits, load
 * windows, controller handler vblanks and CdSync polls.
 * Does nothing when the schedule is inactive. */
void openpete_sched_summary(void);

/* Clock replay. Under OPENPETE_PSXTRACE_CLOCK_REPLAY=1 with `<trace>.vblpos`
 * and `<trace>.spuq` beside the golden, the reference's in-work VBLANKs are
 * delivered at the function entry the reference recorded for them, and the
 * SPU envelope is charged to the reference's exact clock at every wait,
 * poll, query and libspu call instead of a fixed count per VBL. */

/** @brief Report whether the clock replay is on: the knob was set and both sidecars loaded at openpete_sched_open().
 *
 * Latched at openpete_sched_open(); 0 before it.
 * @return 1 when the replay is on, else 0. */
int  openpete_sched_clock_replay(void);
/** @brief Count one per-entry gate hit of addr and return the in-work VBLANK due at it.
 *
 * Counts the entries of every target the current record's `.vblpos` rows
 * name since the record's anchor. When the hit is the k-th entry of the
 * next row's target, every row sharing that point is consumed; a row whose
 * VBLANK the position has already reached is counted as covered.
 * @param addr Guest address of the function entered.
 * @param target_out The reference VBLANK to charge to.
 * @return 1 with target_out set when a VBLANK is due here, else 0. */
int  openpete_sched_vblpos_gate(uint32_t addr, uint32_t* target_out);
/** @brief Report whether the clock replay holds `.vblpos` rows that end at a game load of a RAM word.
 *
 * Latched at openpete_sched_open(); the read gate is installed only then.
 * @return 1 when the replay is on and has such rows, else 0. */
int  openpete_sched_vblpos_read_words(void);
/** @brief Count one guest load at addr and return the in-work VBLANK due before it.
 *
 * A load of a RAM word no `read` row of the golden names returns 0 at
 * once. Otherwise the loads of the word since the record's anchor are
 * counted, and at the k-th load the current record's next `read` row for
 * that word is due; rows sharing the point are consumed together.
 * @param addr Guest address of the load.
 * @param target_out The reference VBLANK to charge to.
 * @return 1 with target_out set when a VBLANK is due here, else 0. */
int  openpete_sched_vblpos_read_gate(uint32_t addr, uint32_t* target_out);
/** @brief Consume the `.vblpos` rows whose entry openpete did not reach before an anchor or a wait.
 *
 * At an anchor (bound 0) the rows of every record before the current one
 * are consumed; at a wait charging to bound, the rows whose VBLANK is at or
 * below it. Each consumed row ahead of the position counts as late, each
 * at or behind it as covered. Inside a load window nothing is consumed.
 * @param bound The wait's target VBLANK, or 0 at an anchor.
 * @return The highest VBLANK of the late rows, 0 when none is ahead of the position. */
uint32_t openpete_sched_vblpos_late(uint32_t bound);
/** @brief Match a libspu call to the next unconsumed `.spuq` row of the current record with the same kind, call site and arguments.
 *
 * Rows of earlier records left unconsumed count as unmade; a call with no
 * row counts as unmatched. Inside a load window nothing matches.
 * @param kind 0 SpuSetKey, 1 SpuGetKeyStatus, 2 SpuGetAllKeysStatus, 3 SpuSetVoiceAttr.
 * @param ra The call's return address.
 * @param a0 First argument (on, voice mask, buffer, or voice mask of the attribute).
 * @param a1 Second argument (SpuSetKey voices, SpuSetVoiceAttr mask, else 0).
 * @param tick_out The reference's exact clock at the call.
 * @return 1 with tick_out set when a row matched, else 0. */
int  openpete_sched_spuq(int kind, uint32_t ra, uint32_t a0, uint32_t a1, uint64_t* tick_out);
/** @brief Return the reference's tick of an SPU register access inside the libspu call openpete_sched_spuq last matched.
 *
 * The access is the next unconsumed `.spureg` row of that call with the
 * given kind and unit. Returns 0 when the last openpete_sched_spuq call
 * matched no row or the call has no such access.
 * @param kind 0 KON store, 1 sample-rate (pitch) store, 2 envelope-level (ENVX) load.
 * @param unit The KON half (0 voices 0-15, 1 voices 16-23) for kind 0, else the voice.
 * @param tick_out The reference's exact clock at the access.
 * @return 1 with tick_out set when an access matched, else 0. */
int  openpete_sched_spureg(int kind, int unit, uint64_t* tick_out);
/** @brief Return the exact return clock of the waited `.vsync` row the last openpete_sched_next_vsync consumed.
 * @return The row's return tick, 0 when the last call consumed no row. */
uint64_t openpete_sched_wait_tick(void);
/** @brief Return the exact clock of the query row the last openpete_sched_query answered from.
 * @return The row's tick, 0 when the last call answered from no row. */
uint64_t openpete_sched_query_tick(void);
/** @brief Report and clear the request to re-seed the SPU clock that the schedule raises at open and at every load-window skip.
 * @return 1 when a re-seed was pending, else 0. */
int  openpete_sched_clock_reseed_take(void);
/** @brief Record the exact clock the SPU clock was seeded from, for the summary's count of key-on rows before it.
 * @param tick The seeding row's clock. */
void openpete_sched_clock_seeded(uint64_t tick);

/** @brief Enter a host-initiated guest call: per-entry gate hits inside it are not counted for the clock replay.
 *
 * Bracket every call into guest code that the guest did not make at that
 * point (an interrupt handler, a BIOS service, a host feature calling a
 * guest body), paired with openpete_psxtrace_gate_host_leave(). */
void openpete_psxtrace_gate_host_enter(void);
/** @brief Leave a host-initiated guest call entered with openpete_psxtrace_gate_host_enter(). */
void openpete_psxtrace_gate_host_leave(void);

/* GTE hash row. Under OPENPETE_GTE_HASH=1 with a `<trace>.gte` sidecar
 * beside the golden, every gte_execute call is folded into one hash per
 * record interval by the rule the capture's recorder uses (the read view of
 * the 64 registers after each op plus the instruction word, FNV-1a-64 over
 * 32-bit words; a register enters as 0 until the game has changed it), and
 * the verifier compares row k when record k+1 is consumed. Nonzero only
 * while the fold is armed; gte_execute tests it before calling in. */
extern int openpete_gte_hash_on;

/** @brief Fold one executed GTE instruction into the open record interval.
 *
 * Takes the read view of the registers before and after the op, marks a
 * register written when its view changed across the op or since the
 * previous op (a store in between), and folds the instruction word and the
 * 64 post-op values (0 for an unwritten register) into the interval's
 * hash. Writes a dump row when the interval's record lies in the
 * OPENPETE_GTE_HASH_DUMP window.
 * @param cmd The cop2 instruction word.
 * @param pc Guest PC of the instruction.
 * @param pre_data Data registers as stored before the op.
 * @param pre_ctrl Control registers as stored before the op.
 * @param post_data Data registers as stored after the op.
 * @param post_ctrl Control registers as stored after the op. */
void openpete_gte_hash_op(uint32_t cmd, uint32_t pc,
                          const uint32_t pre_data[32], const uint32_t pre_ctrl[32],
                          const uint32_t post_data[32], const uint32_t post_ctrl[32]);

/* Called with the dispatched address on each hit of
 * openpete_psxtrace_anchor_pc (core/psx/dispatch.c); NULL when no verifier
 * is attached. */
extern void (*openpete_psxtrace_dispatch_hook)(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PSXTRACE_H */
