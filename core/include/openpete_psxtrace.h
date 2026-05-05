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
 * enables per-byte attribution when a hash mismatch fires.
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

    /* Latched on first divergence. */
    int      diverged;
    uint32_t first_divergent_frame;
    int32_t  first_divergent_region;     /* -1 = total-only mismatch */
    uint64_t first_divergent_oracle;
    uint64_t first_divergent_op;

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

void openpete_psxtrace_close(openpete_psxtrace_t* t);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PSXTRACE_H */
