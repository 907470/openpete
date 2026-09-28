/* core/include/bk2_movie.h — BizHawk (.bk2) movie reader.
 *
 * Octoshock PSX core, ports 1 and 2. For each port the digital buttons and
 * the 4 analog axes (LStick X/Y, RStick X/Y) are decoded; a second port's
 * frames go to p2_frames.
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
    int          players;        /* 2 when any frame decoded a second port, else 0 */
    int          frame_count;
    int          first_input_frame; /* index of first frame with any button down,
                                      * or frame_count if the movie has no input */
    int          load_delay;        /* Header.txt "LoadDelay N": the VBLs every
                                      * level load waited when the movie was
                                      * recorded; 0 when the header has no line */
} bk2_movie_t;

/** @brief Load a .bk2 zip into a movie.
 *
 * Parses the "Input Log.txt" entry with bk2_movie_parse_log and reads the
 * optional "LoadDelay N" line of "Header.txt" into load_delay.
 * @param path Path to the .bk2 file.
 * @param out Receives the movie; release it with bk2_movie_free.
 * @return 0 on success; -1 when the zip cannot be opened, has no input
 *         log, or the log holds no frames, with the reason logged. */
int  bk2_movie_load(const char* path, bk2_movie_t* out);

/** @brief Free a movie's frame arrays and zero the struct.
 *
 * Releases the P1 and P2 frame arrays; the struct itself is not freed.
 * @param m Movie loaded by bk2_movie_load or bk2_movie_parse_log; NULL does
 *        nothing. */
void bk2_movie_free(bk2_movie_t* m);

/** @brief Parse an extracted "Input Log.txt" blob into a movie.
 *
 * Every line starting with '|' but not "|[" is one frame, decoded per
 * player; the text is read within @p len and need not be NUL-terminated,
 * and a trailing CR is stripped. A malformed line is kept as a neutral
 * frame, counted and logged. first_input_frame is the first frame with a
 * button down or a stick off-centre. Called by bk2_movie_load and the unit
 * tests.
 * @param text Log text.
 * @param len Length of @p text.
 * @param out Receives the movie; zeroed first; release it with
 *        bk2_movie_free.
 * @return 0 on success; -1 when no frame line exists or allocation fails,
 *         with the reason logged. */
int  bk2_movie_parse_log(const char* text, size_t len, bk2_movie_t* out);

#ifdef __cplusplus
}
#endif

#endif
