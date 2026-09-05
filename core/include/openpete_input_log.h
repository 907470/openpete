/* core/include/openpete_input_log.h — per-VBL input log, the single source
 * of truth for "what pad state was latched at VBL index i of this session".
 *
 * Producer: the host loop (run.c) appends once per VBL the game fiber
 * consumed on a ticked iteration (the same `ticks`-copies expansion the
 * bk2 recorder used before the lift-out — both VBLs of a gameplay tick
 * carry the tick-latched bits, preserving the tick-quantized semantics the
 * bk2 corpus was validated under). Always on; one record per VBL carries
 * every pad slot, ~28 bytes/VBL ≈ 6 MB/hour.
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

/* Pad slots the log carries. Slot 0 is player 1, the only slot the retail
 * game reads; slots 1-3 are host-only and reach the guest solely through a
 * mod that decodes them into its own pad buffer. */
#define OPENPETE_PAD_SLOTS 4

typedef struct {
    uint16_t bits;      /* PSX pad bitfield: 1 = released, 0 = pressed */
    uint8_t  axes[4];   /* LX, LY, RX, RY; 128 = centre */
    uint8_t  present;   /* 1 = a device drove this slot at this VBL */
} openpete_input_port_t;

typedef struct {
    openpete_input_port_t port[OPENPETE_PAD_SLOTS];
} openpete_input_rec_t;

/* Canonical empty slot: every button released, both sticks centred, absent.
 * The same bytes real libpad reports for a port with no controller in it. */
void input_port_clear(openpete_input_port_t* p);

/* --- Staging -------------------------------------------------------------
 *
 * The record for the tick that is ABOUT to run. Producers fill it before the
 * yield; `input_log_append_staged` commits it after. It exists because the
 * log entry for tick T is appended once T has already executed, so a
 * consumer running INSIDE T cannot read T's entry — it reads the staged
 * record instead, and gets the same bytes the log will carry. That identity
 * is what makes a slot read reproduce under record, replay, rewind and
 * runahead alike.
 *
 * Slot 0 is staged from the pad override channel, which every producer
 * (live host decode, bk2 replay, the rewind burst, the settings menu) has
 * already stamped by then. The value the log stores for slot 0 is still the
 * post-yield guest readback, unchanged: staging only publishes it early. */

/* Reset the staged record to four empty ports. Once per host iteration,
 * before any port is staged. */
void input_log_stage_reset(void);

/* Fill one staged port and mark it present. No-op for an out-of-range slot. */
void input_log_stage_port(int slot, uint16_t bits, const uint8_t axes[4]);

/* Overwrite slot 0's bytes without disturbing the other ports — the
 * post-yield readback correcting what was staged pre-yield. */
void input_log_stage_p1_readback(uint16_t bits, const uint8_t axes[4]);

/* The staged record. Never NULL. */
const openpete_input_rec_t* input_log_staged(void);

/* Append the staged record as one VBL. Returns the entry's index, or -1 on
 * allocation failure (logged; the log stops growing but never crashes the
 * run). */
int input_log_append_staged(void);

/* Read the pad bytes of the tick currently executing. Returns 1 and fills
 * `out` when `slot` is in range, 0 otherwise (leaving `out` cleared). */
int input_log_read_slot(int slot, openpete_input_port_t* out);

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
