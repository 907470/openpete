/* core/runtime/bk2_movie.h — BizHawk (.bk2) movie reader.
 *
 * v1: Octoshock PSX core, P1 digital pad only. Analog axes and the second
 * controller are parsed-then-ignored (see openpete-bk2-input.md).
 *
 * The decoded per-frame value is a 16-bit PSX pad bitfield in the same
 * "released = 1, pressed = 0" convention PsyCross's
 * PsyX_Pad_UpdateKeyboardInput returns — so the host loop can drop it
 * straight into the keyboard-override slot and the rest of the pad
 * pipeline is unchanged.
 */
#ifndef OPENPETE_BK2_MOVIE_H
#define OPENPETE_BK2_MOVIE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t* frames;            /* PSX pad bitfields, 0xFFFF = all released */
    int       frame_count;
    int       first_input_frame; /* index of first frame with any button down,
                                  * or frame_count if the movie has no input */
} bk2_movie_t;

/* Load a .bk2 zip. Reads "Input Log.txt", parses Octoshock P1 digital
 * buttons. Returns 0 on success; on failure logs via OPL_ERROR_F and
 * returns non-zero. The caller owns `out->frames` and must call
 * bk2_movie_free. */
int  bk2_movie_load(const char* path, bk2_movie_t* out);

void bk2_movie_free(bk2_movie_t* m);

/* Exposed for unit tests: parse an already-extracted "Input Log.txt"
 * blob (NUL-terminated or sized) directly. */
int  bk2_movie_parse_log(const char* text, size_t len, bk2_movie_t* out);

#ifdef __cplusplus
}
#endif

#endif
