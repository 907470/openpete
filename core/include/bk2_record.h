/* core/runtime/bk2_record.h — record live pad input into a BizHawk
 * (.bk2) movie zip. Sister of bk2_movie.{c,h} (replay).
 *
 * The per-VBL input buffer lives in openpete_input_log (always-on, fed by
 * the host loop); this module marks the window [begin, finalize) and
 * serializes those entries as "Input Log.txt" packed into a .bk2 zip
 * alongside a stub "Header.txt" so the file round-trips through BizHawk
 * and through our own bk2_movie_load. (Savestate plan P1 lift-out: the
 * log is shared with the rewind/runahead drivers.)
 *
 * P1 digital buttons and the 4 analog axes are recorded, plus the analog-
 * mode flag (MODE column) — see openpete-bk2-input.md.
 */
#ifndef OPENPETE_BK2_RECORD_H
#define OPENPETE_BK2_RECORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Begin recording. Must be paired with a finalize. Returns 0 on
 * success, non-zero (and logs via OPL_ERROR_F) if the buffer can't be
 * allocated. The path is stored verbatim and used at finalize time;
 * the file is not opened until then.
 *
 * `args_sidecar`, if non-NULL, is written verbatim to "<path>.args"
 * alongside the .bk2 at finalize time (the caller builds this from the
 * launch flags so `--bk2 <path>` re-applies the scenario). NULL = no
 * sidecar. The string is copied; the caller may free it after the call. */
int  bk2_record_begin(const char* path, const char* args_sidecar);

/* Note a just-logged frame while recording: emits the press/release
 * transition log line (`log_idx` is the input_log index the host loop got
 * back from input_log_append; the line shows the movie-relative frame).
 * The bits themselves already live in the input log — this never buffers.
 * No-op if recording isn't active. */
void bk2_record_note(uint16_t bits, int log_idx);

/* Write the .bk2 zip and free the buffer. No-op if recording isn't
 * active. Returns 0 on success, non-zero on write failure. Always safe
 * to call from an exit path. */
int  bk2_record_finalize(void);

/* Format the press/release diff between `prev` and `cur` PSX pad bitfields
 * (1=released, 0=pressed) into two NUL-terminated strings of BizHawk
 * button letters. Each output buffer must hold at least 17 bytes. Useful
 * for debug logging on both the record and replay paths. */
void bk2_format_button_diff(uint16_t prev, uint16_t cur,
                            char press_out[17], char release_out[17]);

#ifdef __cplusplus
}
#endif

#endif
