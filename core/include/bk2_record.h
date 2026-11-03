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
 * One Input Log line per pad write of a yield, holding player 1's pad as
 * the host delivered it at that write (bk2_pad_write_t) and the other
 * ports of the entry the write's yield logged, marked with the Header.txt
 * line "OpenpeteFeedKey consume". The rows are grouped by openpete_consume (E, an entry of the
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

#include "bk2_feed.h"   /* bk2_pad_write_t */

#include <stddef.h>
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

/** @brief Note the pad writes a just-logged entry's yield took, with player 1's pad at each, and log its pad transitions while recording.
 *
 * Always on: the note store keeps every entry from the floor
 * bk2_notes_retain_from sets (and from an active recording's first entry).
 * The caller holds input_log_lock across the append and this note. The
 * entry is written @p writes times by a window writer, one line per write
 * with that write's player 1 pad: its first write as row @p j of the group
 * of E @p e, the rest as rows of the group of E @p e_rest. A note at or
 * before the store's last entry first truncates the store to it. While
 * recording, a line is logged for each write whose bits differ from the
 * previous write's.
 * @param log_idx Input log index input_log_append_staged returned; the line shows
 *        it relative to the movie's first entry.
 * @param writes Yield-path pad writes the entry's yield took; 0 adds no row.
 * @param e openpete_consume at the entry's first write.
 * @param e_rest openpete_consume at its last write.
 * @param j openpete_consume_write at the entry's first write.
 * @param pads Player 1's pad at each of the @p writes writes, in order. */
void bk2_record_note(int log_idx, unsigned writes, unsigned e, unsigned e_rest,
                     unsigned j, const bk2_pad_write_t* pads);

/** @brief Drop the row notes of the entries before an input-log index.
 *
 * An active recording keeps its entries whatever the index. The caller
 * holds input_log_lock.
 * @param idx The first entry kept. */
void bk2_notes_retain_from(int idx);

/** @brief Drop the row notes of the entries at and after an input-log index, after the log was truncated to it.
 *
 * The caller holds input_log_lock.
 * @param count The log's entry count after its truncation. */
void bk2_notes_truncate(int count);

/** @brief Return the number of entries the note store holds.
 * @param bytes Receives the store's allocated bytes; may be NULL.
 * @return The entry count. */
size_t bk2_notes_held(size_t* bytes);

/** @brief Build the movie of the input-log entries from an index to the log's end as a .bk2 zip on the heap.
 *
 * The same writer as the recorder's finalize (Header.txt, Input Log.txt,
 * Input Groups.txt and, for two players, SyncSettings.json), with the
 * groups numbered from @p group_base: a replay passes --bk2-frame-offset
 * @p group_base. The caller holds input_log_lock.
 * @param start_idx The first entry; the note store must hold it.
 * @param group_base openpete_consume at the first entry.
 * @param zip Receives the archive; free with free().
 * @param len Receives its length.
 * @param rows Receives the movie rows written; may be NULL.
 * @return 0 on success; -1 when the store does not hold the entry, the
 *         window holds no pad write or allocation fails. */
int bk2_record_window_zip(int start_idx, unsigned group_base, void** zip, size_t* len,
                          size_t* rows);

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

/** @brief End recording without writing anything: the file at the record path and its sidecar are left as they were.
 *
 * Frees the copied path, and the args sidecar, and clears
 * the active flag under input_log_lock. The guest discontinuity handler
 * begin registered stays registered and is inert while recording is not
 * active. No effect when recording is not active. */
void bk2_record_discard(void);

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
