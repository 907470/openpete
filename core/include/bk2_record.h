/* core/include/bk2_record.h - record live pad input into a BizHawk
 * (.bk2) movie zip. Sister of bk2_movie.{c,h} (replay).
 *
 * The per-VBL input buffer lives in openpete_input_log (always-on, fed by
 * the host loop); this module marks the window [begin, finalize) and
 * serializes those entries as "Input Log.txt" packed into a .bk2 zip
 * alongside a stub "Header.txt" so the file round-trips through BizHawk
 * and through our own bk2_movie_load. The log is shared with the
 * rewind/runahead drivers.
 *
 * One Input Log line per pad write of a yield, holding the input of the
 * yield the write took, marked with the Header.txt line "OpenpeteFeedKey
 * consume". The rows are grouped by openpete_consume (E, an entry of the
 * game's pad-ring consumer); "Input Groups.txt" holds each group's row
 * count, one per line from E 0. A recording that opens inside a group
 * repeats its first row for the group's writes before it.
 *
 * Each port's 16 digital buttons and 4 analog axes are recorded; the MODE
 * column is always '.'. Port 2 is recorded when it was present for any
 * recorded VBL, and the movie then also carries SyncSettings.json.
 */
#ifndef OPENPETE_BK2_RECORD_H
#define OPENPETE_BK2_RECORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Begin recording the input log from its current end into a .bk2 at finalize.
 *
 * Must be paired with bk2_record_finalize. The path is copied and used at
 * finalize; the file is not opened until then. A guest discontinuity
 * handler is registered so a savestate load finalizes the movie.
 * @param path Output .bk2 path.
 * @param args_sidecar Written verbatim to "<path>.args" at finalize, so
 *        `--bk2 <path>` re-applies the launch flags the caller built it
 *        from; NULL writes no sidecar. Copied; a failed copy skips the
 *        sidecar.
 * @return 0 on success; -1 when recording is already active or the path
 *         cannot be copied, with the reason logged. */
int  bk2_record_begin(const char* path, const char* args_sidecar);

/** @brief Record the pad writes a just-logged entry's yield took and log its pad transitions while recording.
 *
 * The bits already live in the input log. The caller holds input_log_lock
 * across the append and this note. The entry is written @p writes
 * times at finalize: its first write as row @p j of the group of E @p e,
 * the rest as rows of the group of E @p e_rest. A
 * line is logged only when the bits differ from the previous call's. No
 * effect when recording is not active.
 * @param bits Player 1 pad bitfield (1 released, 0 pressed).
 * @param log_idx Input log index input_log_append_staged returned; the line shows
 *        it relative to the movie's first entry.
 * @param writes Yield-path pad writes the entry's yield took; 0 adds no row.
 * @param e openpete_consume at the entry's first write.
 * @param e_rest openpete_consume at its last write.
 * @param j openpete_consume_write at the entry's first write. */
void bk2_record_note(uint16_t bits, int log_idx, unsigned writes, unsigned e,
                     unsigned e_rest, unsigned j);

/** @brief Write the recorded window of the input log as a .bk2 zip and end recording.
 *
 * The zip holds Header.txt (with a LoadDelay line when the CD load delay
 * is nonzero), Input Log.txt, Input Groups.txt, and, when any recorded VBL had a second port
 * present, SyncSettings.json declaring two DualShock ports; the args
 * sidecar is written before the zip. Recording ends on every path, so the
 * call is safe from an exit path. Holds input_log_lock while it reads the
 * log and the recorder's rows, so a finalize on another thread waits out
 * an append and its note in flight.
 * @return 0 on success or when recording is not active; -1 on an
 *         allocation or write failure, with the reason logged. */
int  bk2_record_finalize(void);

/** @brief Format the press/release diff between two PSX pad bitfields as BizHawk button letters.
 *
 * Bitfields use 1 released, 0 pressed. Letters follow the Input Log's
 * column order (U D L R s S Q T O X l r L R L R). Used for debug logging
 * on the record and replay paths.
 * @param prev Previous bitfield.
 * @param cur Current bitfield.
 * @param press_out Receives the NUL-terminated letters of buttons pressed
 *        in @p cur; at least 17 bytes.
 * @param release_out Receives the NUL-terminated letters of buttons
 *        released in @p cur; at least 17 bytes. */
void bk2_format_button_diff(uint16_t prev, uint16_t cur,
                            char press_out[17], char release_out[17]);

#ifdef __cplusplus
}
#endif

#endif
