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

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PSXTRACE_H */
