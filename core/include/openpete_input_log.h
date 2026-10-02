/* core/include/openpete_input_log.h - per-VBL input log, the single source
 * of truth for "what pad state was latched at VBL index i of this session".
 *
 * Producer: the host loop (run.c) appends once per VBL the game fiber
 * consumed on a ticked iteration (the same `ticks`-copies expansion the
 * bk2 recorder used before the lift-out - both VBLs of a gameplay tick
 * carry the tick-latched bits, preserving the tick-quantized semantics the
 * bk2 corpus was validated under). Always on; one record per VBL carries
 * every pad slot, sizeof(openpete_input_rec_t) = 32 bytes (4 ports of 8 bytes,
 * padded) per VBL ≈ 6.9 MB/hour at 59.826 VBL/s.
 *
 * Consumers:
 *   - bk2_record: serializes entries [begin_idx, count) into the movie.
 *   - rewind driver: replays entries forward from a
 *     keyframe, and truncates the log after a restore (it KNOWS its
 *     restores rewind this session's own timeline - that's why truncation
 *     is an explicit API and NOT an openpete_discontinuity subscriber: an
 *     arbitrary savestate load is a foreign timeline, where truncating
 *     would fabricate a bogus input history; bk2_record's finalize-on-load
 *     guard handles that case instead).
 *   - runahead driver: source of held-input for speculative ticks.
 *
 * Entries use bk2 axis order (LX, LY, RX, RY) - the recorder's storage
 * order, converted from the PSX readback order by the producer.
 *
 * Indices are absolute append counts for the process lifetime (no sliding
 * window yet). If a window is ever needed, add a base offset then - the
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

/** @brief Reset a port to the canonical empty slot.
 *
 * Every button released (bits 0xFFFF), all four axes centred (128), and
 * present 0.
 * @param p the port to clear; must not be NULL. */
void input_port_clear(openpete_input_port_t* p);

/* --- Staging -------------------------------------------------------------
 *
 * The record for the tick that is ABOUT to run. Producers fill it before the
 * yield; `input_log_append_staged` commits it after. It exists because the
 * log entry for tick T is appended once T has already executed, so a
 * consumer running INSIDE T cannot read T's entry - it reads the staged
 * record instead, and gets the same bytes the log will carry. That identity
 * is what makes a slot read reproduce under record, replay, rewind and
 * runahead alike.
 *
 * Slot 0 is staged from the pad override channel, which every producer
 * (live host decode, bk2 replay, the rewind burst, the settings menu) has
 * already stamped by then. The value the log stores for slot 0 is still the
 * post-yield guest readback, unchanged: staging only publishes it early. */

/** @brief Reset the staged record to empty ports and stamp the VBL of the tick about to run.
 *
 * Called once per host iteration before any port is staged; the rewind
 * burst calls it again per replayed tick with that tick's VBL before
 * re-staging the logged record.
 * @param vbl the VBL counter value the tick about to run carries. */
void input_log_stage_reset(unsigned vbl);

/** @brief Fill one staged port and mark it present.
 *
 * Does nothing for a slot outside 0 .. OPENPETE_PAD_SLOTS - 1.
 * @param slot pad slot.
 * @param bits PSX pad bitfield, 1 = released.
 * @param axes LX, LY, RX, RY in bk2 order, or NULL for centred sticks. */
void input_log_stage_port(int slot, uint16_t bits, const uint8_t axes[4]);

/** @brief Overwrite staged slot 0 with the post-yield guest readback and mark it present.
 *
 * The other ports are left as staged. The readback is the value the log
 * stores for slot 0; the pre-yield staging only published it early.
 * @param bits PSX pad bitfield read back from the guest, 1 = released.
 * @param axes LX, LY, RX, RY in bk2 order, or NULL for centred sticks. */
void input_log_stage_p1_readback(uint16_t bits, const uint8_t axes[4]);

/** @brief Return the staged record.
 *
 * @return the staged record; never NULL. */
const openpete_input_rec_t* input_log_staged(void);

/** @brief Return the VBL the staged record was stamped with by input_log_stage_reset.
 *
 * A tick re-executed under runahead speculation reads the record staged for
 * the last committed tick, so this is that tick's VBL, not the speculative
 * one.
 * @return the stamped VBL. */
unsigned input_log_staged_vbl(void);

/** @brief Append the staged record as one VBL entry.
 *
 * Capacity starts at 16384 entries and doubles. A failed reallocation is
 * logged and freezes the log: this and every later append return -1 until
 * input_log_reset.
 * @return the entry's index, or -1 when the log is frozen. */
int input_log_append_staged(void);

/** @brief Read one slot of the staged record, the pad bytes of the tick currently executing.
 *
 * @param slot pad slot.
 * @param out receives the port; cleared with input_port_clear when the slot
 *        is out of range.
 * @return 1 when the slot is in range and out is filled, 0 for an
 *         out-of-range slot or a NULL out. */
int input_log_read_slot(int slot, openpete_input_port_t* out);

/** @brief Return the number of entries in the log.
 *
 * @return the entry count, lowered by input_log_truncate. */
int input_log_count(void);

/** @brief Return a log entry by index.
 *
 * @param idx 0 .. input_log_count() - 1.
 * @return the entry, or NULL when idx is out of range. The pointer is valid
 *         until the next append, which may reallocate, or reset. */
const openpete_input_rec_t* input_log_get(int idx);

/** @brief Keep the first count entries and drop the rest.
 *
 * Called after a rewind restore, whose later entries belong to a timeline
 * that no longer runs. A negative count is treated as 0; a count at or
 * above the current count changes nothing.
 * @param count the number of entries to keep. */
void input_log_truncate(int count);

/** @brief Free the log, clear the frozen state, and reset the staged record with VBL 0. */
void input_log_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_INPUT_LOG_H */
