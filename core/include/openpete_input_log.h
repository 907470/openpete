/* core/include/openpete_input_log.h — per-VBL input log, the single source
 * of truth for "what pad state was latched at VBL index i of this session".
 *
 * Producer: the host loop (run.c) appends once per VBL the game fiber
 * consumed on a ticked iteration (the same `ticks`-copies expansion the
 * bk2 recorder used before the lift-out — both VBLs of a gameplay tick
 * carry the tick-latched bits, preserving the tick-quantized semantics the
 * bk2 corpus was validated under). Always on; ~6 bytes/VBL ≈ 1.3 MB/hour.
 *
 * Consumers:
 *   - bk2_record: serializes entries [begin_idx, count) into the movie.
 *   - rewind driver (savestate plan P4): replays entries forward from a
 *     keyframe, and truncates the log after a restore (it KNOWS its
 *     restores rewind this session's own timeline — that's why truncation
 *     is an explicit API and NOT an openpete_discontinuity subscriber: an
 *     arbitrary savestate load is a foreign timeline, where truncating
 *     would fabricate a bogus input history; bk2_record's finalize-on-load
 *     guard handles that case instead).
 *   - runahead driver (P5): source of held-input for speculative ticks.
 *
 * Entries use bk2 axis order (LX, LY, RX, RY) — the recorder's storage
 * order, converted from the PSX readback order by the producer.
 *
 * Indices are absolute append counts for the process lifetime (no sliding
 * window yet). If a window is ever needed, add a base offset then — the
 * rewind keyframe ring will define the retention policy.
 */

#ifndef OPENPETE_INPUT_LOG_H
#define OPENPETE_INPUT_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t bits;      /* PSX pad bitfield: 1 = released, 0 = pressed */
    uint8_t  axes[4];   /* LX, LY, RX, RY; 128 = centre */
} openpete_input_rec_t;

/* Append one VBL's input. Returns the entry's index, or -1 on allocation
 * failure (logged; the log stops growing but never crashes the run). */
int input_log_append(uint16_t bits, const uint8_t axes[4]);

/* Number of entries logged so far. */
int input_log_count(void);

/* Entry by index, or NULL if out of range. Pointer valid until the next
 * append/truncate/reset. */
const openpete_input_rec_t* input_log_get(int idx);

/* Keep the first `count` entries, drop the rest (rewind restore: the
 * future beyond the restored frame is invalidated). No-op if count >=
 * current count. */
void input_log_truncate(int count);

/* Drop everything (process-lifetime reset; tests). */
void input_log_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_INPUT_LOG_H */
