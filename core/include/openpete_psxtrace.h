/* core/include/openpete_psxtrace.h — streaming .psxtrace oracle verifier.
 *
 * Opens a trace produced by scripts/pcsx-redux-trace.lua and, on each
 * advance(), reads the next per-frame record + byte-compares its 2 MB RAM
 * payload against openpete's live g_ram. First divergence latches; the
 * caller can read first_divergent_* to fail the run.
 *
 * Format: tools/psxtrace_format.py.
 *
 * v1 limitations:
 *   - Raw (uncompressed) records only. Pre-process zstd traces with
 *     `python -m tools.psxtrace_format compress src.zst dst.raw`.
 *   - Full 2 MB RAM diff. Manifest-restricted mode is a follow-up.
 *   - No controller replay yet — caller is responsible for ensuring input
 *     parity (typically via --bk2 with a movie captured alongside the trace).
 */

#ifndef OPENPETE_PSXTRACE_H
#define OPENPETE_PSXTRACE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    FILE*       fp;
    const char* path;
    uint32_t    version;
    uint32_t    frame_count;
    uint32_t    flags;
    uint32_t    records_consumed;

    /* Latched on first divergence. */
    int      diverged;
    uint32_t first_divergent_frame;
    uint32_t first_divergent_addr;     /* PSX vaddr (KSEG0-relative)        */
    uint8_t  first_divergent_op;       /* openpete byte at that addr        */
    uint8_t  first_divergent_oracle;   /* trace byte at that addr           */

    /* Counts since open. */
    uint64_t bytes_compared;
    uint64_t bytes_diverged;
} openpete_psxtrace_t;

/* Open and parse header. Returns 0 on success, -1 on error (logs reason). */
int openpete_psxtrace_open(openpete_psxtrace_t* t, const char* path);

/* Compare openpete's live g_ram against the trace's next record.
 * Returns 1 if a record was consumed (diff results in t->diverged + counts),
 *         0 if EOF (no more records),
 *        -1 on parse/IO error.
 * On a successful diff that finds a divergence, t->diverged is set; caller
 * decides whether to stop the run or keep going. */
int openpete_psxtrace_advance(openpete_psxtrace_t* t,
                              const uint8_t* live_ram,
                              size_t live_ram_size);

void openpete_psxtrace_close(openpete_psxtrace_t* t);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PSXTRACE_H */
