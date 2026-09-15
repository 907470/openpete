/* core/include/bk2_movie.h — BizHawk (.bk2) movie reader.
 *
 * Octoshock PSX core, P1 pad. Digital buttons + the 4 analog axes (LStick
 * X/Y, RStick X/Y) are decoded; the second controller is ignored.
 *
 * Each frame decodes to a bk2_frame_t: a 16-bit PSX pad bitfield in the
 * same "released = 1, pressed = 0" convention PsyCross's
 * PsyX_Pad_UpdateKeyboardInput returns (so the host loop can drop it
 * straight into the keyboard-override slot) plus the 4 analog axis bytes
 * (128 = centre). The pad is always replayed in analog mode (id 0x73) —
 * Spyro behaves identically in digital vs analog mode except that analog
 * actually drives the stick, so legacy digital-only movies (axes = 128)
 * replay unchanged.
 */
#ifndef OPENPETE_BK2_MOVIE_H
#define OPENPETE_BK2_MOVIE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t buttons;  /* PSX pad bitfield, 0xFFFF = all released */
    uint8_t  axes[4];  /* LStick X, LStick Y, RStick X, RStick Y; 128 = centre */
} bk2_frame_t;

typedef struct {
    bk2_frame_t* frames;
    /* Player 2, one entry per frame, neutral where the movie declares only
     * one port. `players` says whether any frame actually carried a second
     * segment — a movie can only declare its port count for its whole
     * length, since the format has no per-frame connect column. */
    bk2_frame_t* p2_frames;
    int          players;        /* 1 or 2 */
    int          frame_count;
    int          first_input_frame; /* index of first frame with any button down,
                                      * or frame_count if the movie has no input */
    int          load_delay;        /* Header.txt "LoadDelay N": the VBLs every
                                      * level load waited when the movie was
                                      * recorded; 0 when the header has no line */
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
