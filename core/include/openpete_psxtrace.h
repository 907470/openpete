/* core/include/openpete_psxtrace.h — streaming .psxtrace v2 oracle verifier.
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

    /* PC of the first record — captured during open() by peeking +
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
    uint32_t records_skipped;     /* reference records passed over at load-window ends */
    uint32_t records_uncompared;  /* records consumed inside a load window, not compared */
    int      strict_diverged;
    uint32_t first_strict_divergent_frame;
    int32_t  first_strict_divergent_region;

    /* The gate verdict. Set when a region that is NOT cd_load_transient
     * diverges, OR a cd_load_transient region diverges and fails to
     * re-converge within `heal_window` ticks (or is still diverging at
     * EOF). This — not `diverged` — drives the process exit code and the
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
    uint64_t transient_healed_bursts;    /* count of absorbed (healed) bursts */
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

int openpete_psxtrace_open(openpete_psxtrace_t* t, const char* path);

/* Returns:
 *    1 - record consumed (check t->diverged for verdict)
 *    0 - EOF (no more records)
 *   -1 - parse/IO error
 */
int openpete_psxtrace_advance(openpete_psxtrace_t* t);

/* Call once after the last advance() (EOF). Any cd_load_transient region
 * still mid-burst (never re-converged before the trace ended) is promoted
 * to a hard failure. Also logs a one-line tally of absorbed transients. */
void openpete_psxtrace_finalize(openpete_psxtrace_t* t);

void openpete_psxtrace_close(openpete_psxtrace_t* t);

/* The reference's schedule (psxtrace_schedule.c): the `<trace>.vsync`,
 * `<trace>.loads` and `<trace>.srand` sidecars beside a golden, consumed
 * in order by the thunk layer while the trace is verified. */
int  openpete_sched_open(const char* trace_path);   /* 1 when any sidecar loaded */
int  openpete_sched_active(void);
/* The schedule tracks the reference VBLANK count openpete has reached
 * (`pos`). Each consumer returns a target; the thunk layer charges
 * `target - pos` VBLs (one psyq_vbl_body each) and advances pos. */
void     openpete_sched_set_stamps(const uint32_t* stamps, uint32_t count); /* the trace's pad2 column */
uint32_t openpete_sched_pos(void);
void     openpete_sched_advance_pos(uint32_t vbls);
int      openpete_sched_pos_known(void);
int32_t  openpete_sched_query(uint32_t ra, uint32_t* target_out); /* the reference's answer to the VSync(-1) call from site ra; -1 = unknown */
int32_t  openpete_sched_counter(void);   /* the counter at the position (a waited call's return value) */
int      openpete_sched_registration(uint32_t ra, uint32_t* target_out); /* a callback registration's VBLANK, when behind it */
uint32_t openpete_sched_current_record(void); /* the reference record index the next anchor hit produces */
/* Next waited VSync row: *target_out = VBLANK at its return, *counter_out =
 * the counter the following query returned (-1 = none). 0 when no
 * schedule or exhausted (a failure is recorded). */
int  openpete_sched_next_vsync(int mode, uint32_t* target_out);
/* The reference's VBLANK at anchor hit `record` (0-based record index). */
int  openpete_sched_anchor_target(uint32_t record, uint32_t* target_out);
/* Next read row: issue and completion VBLANK, *sync_out = 1 when the
 * reference completed it before its next anchor hit. Refuses a row whose
 * sectors or destination differ from the issued read. */
int  openpete_sched_read(int sectors, uint32_t dst, uint32_t* issue_vbl_out, uint32_t* done_vbl_out, uint32_t* done_rec_out, int* sync_out);
int  openpete_sched_next_srand(uint32_t* seed_out);
/* Load windows (the golden's .watch sibling and the game's predicate):
 * at an anchor hit with the watched word's value, 1 with *skip_to_rec when
 * openpete just left a window the reference left at a later record. */
int  openpete_sched_anchor_window(uint32_t record, uint32_t value, uint32_t* skip_to_rec);
int  openpete_sched_in_load(void);
/* Whether the kernel's controller/card vblank handler ran on reference
 * VBLANK `vbl` (from the .sio0 sidecar), and whether the controller
 * accepted a command on it (from .padcmd, the vsyncs libpad's command
 * sequence advanced on); `known` is 0 and the answer 1 when no sidecar
 * covers the vblank (none loaded, or before the game's first poll). */
int  openpete_sched_sio0_served(uint32_t vbl, int* known);
int  openpete_sched_padcmd_at(uint32_t vbl, int* known);
/* The reference's answer to the next CdSync poll of this record with this
 * mode from call site `ra` (from the .cdsync sidecar), with the VBLANK the
 * poll returned at; 0 when the record has no such row left. */
int  openpete_sched_cdsync(int mode, uint32_t ra, int32_t* ret_out, uint32_t* target_out);
/* The reference's clock at the row the last waited VSync or CdSync poll
 * was answered from (0: no row), and the clock of the completion callback
 * of the read the last openpete_sched_read answered (0: none). */
uint64_t openpete_sched_last_tick(void);
uint64_t openpete_sched_read_done_tick(void);
/* The next unanswered CdSync row of the current record: call site and
 * clock value; 0 when none is left. */
int  openpete_sched_cdsync_peek(uint32_t* ra_out, uint64_t* tick_out);
/* Verifier: discard reference records up to `rec` (exclusive) so the next
 * advance compares that record; records inside a load window are consumed
 * without a compare. */
int  openpete_psxtrace_skip_to(openpete_psxtrace_t* t, uint32_t rec);
uint32_t openpete_psxtrace_current_record(void);          /* on the active trace (run.c) */
int      openpete_psxtrace_skip_current_to(uint32_t rec);
void openpete_sched_fail(const char* fmt, ...);
int  openpete_sched_failed(const char** reason);
void openpete_sched_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PSXTRACE_H */
