/* core/record/bk2_movie.c — see bk2_movie.h.
 *
 * Format reference (Octoshock 2.9.1):
 *
 *   [Input]
 *   LogKey:#Disc Select|Open|Close|Reset|#P1 LStick X|P1 LStick Y|...|P1 MODE|
 *   |    1,...|  128,  128,  128,  128,.................|
 *   |    1,...|  128,  128,  128,  128,.....S...X.......|
 *   ...
 *   [/Input]
 *
 * The LogKey line declares two segments. Segment 0 (system bus) holds the
 * disc-select int + 3 buttons; we ignore it. Segment 1 (P1) holds 4 analog
 * ints (LX, LY, RX, RY — default 128) then 17 digital chars in this order:
 *   Up, Down, Left, Right, Select, Start,
 *   Square, Triangle, Circle, Cross,
 *   L1, R1, L2, R2, L3, R3, MODE
 *
 * "MODE" is the BizHawk analog-toggle key, not a real PSX pad bit; we drop
 * it. Each digital position is a single character: '.' = released, anything
 * else = pressed. We only check `c != '.'` so we don't depend on which
 * letter BizHawk picked for each button.
 *
 * The output bitfield uses the same convention as PsyCross's keyboard
 * reader (PsyX_Pad_UpdateKeyboardInput in
 * third_party/PsyCross/src/pad/PsyX_pad.cpp): start at 0xFFFF,
 * AND off the bit for each pressed button.
 */
#include "bk2_movie.h"
#include "openpete_log.h"

#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* P1-segment digital position → PSX pad bitmask (released-when-set). */
static const uint16_t kBk2P1DigitalMask[16] = {
    0x0010, /* 0  Up      */
    0x0040, /* 1  Down    */
    0x0080, /* 2  Left    */
    0x0020, /* 3  Right   */
    0x0001, /* 4  Select  */
    0x0008, /* 5  Start   */
    0x8000, /* 6  Square  */
    0x1000, /* 7  Triangle*/
    0x2000, /* 8  Circle  */
    0x4000, /* 9  Cross   */
    0x0400, /* 10 L1      */
    0x0800, /* 11 R1      */
    0x0100, /* 12 L2      */
    0x0200, /* 13 R2      */
    0x0002, /* 14 L3      */
    0x0004, /* 15 R3      */
    /* index 16 is MODE — not a PSX bit; handled by stopping the loop at 16. */
};

/** @brief Decode one player segment of an input line into a bk2_frame_t.
 *
 * The segment starts with 4 comma-terminated analog ints (LX, LY, RX, RY),
 * each clamped to 0..255 (128 = centre), then the digital tail: 16 button
 * characters, '.' released and any other character pressed, followed by
 * the MODE column, which is ignored. @p out is reset to neutral first.
 * @param seg Segment text between two pipes.
 * @param seg_len Length of @p seg.
 * @param out Receives the buttons and axes.
 * @return 0 on success; -1 when an axis lacks its digits or comma, or the
 *         tail is shorter than 16 characters. */
static int bk2_decode_port(const char* seg, size_t seg_len, bk2_frame_t* out) {
    out->buttons = 0xFFFF;
    out->axes[0] = out->axes[1] = out->axes[2] = out->axes[3] = 128;

    /* Parse the 4 analog axes, each an int up to its comma. Octoshock always
     * emits all 4 (default 128); clamp into the PSX 0..255 range. */
    const char* p = seg;
    const char* end = seg + seg_len;
    for (int a = 0; a < 4; a++) {
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        int neg = 0;
        if (p < end && (*p == '-' || *p == '+')) { neg = (*p == '-'); p++; }
        int val = 0, digits = 0;
        while (p < end && *p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); digits++; p++; }
        while (p < end && *p != ',') p++;       /* to the comma */
        if (digits == 0 || p >= end) return -1; /* missing value or comma */
        p++;                                    /* past the comma */
        if (neg) val = -val;
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        out->axes[a] = (uint8_t)val;
    }

    /* Digital tail — 16 buttons, then MODE at index 16. */
    size_t tail_len = (size_t)(end - p);
    if (tail_len < 16) return -1;

    uint16_t bits = 0xFFFF;
    for (int i = 0; i < 16; i++) {
        if (p[i] != '.') {
            bits &= (uint16_t)~kBk2P1DigitalMask[i];
        }
    }
    out->buttons = bits;
    /* MODE column (index 16) is ignored — the pad is always replayed analog. */
    return 0;
}

/** @brief Decode one input line into player 1's frame and, when present, player 2's.
 *
 * The line is "|seg0|seg1|seg2|", scanned within its length without a NUL.
 * Segment 0 is the system bus and is skipped; segment 1 is player 1 and
 * segment 2, when the movie declares one, is player 2. Both outputs are
 * reset to neutral first; a malformed player 2 segment leaves @p p2 with
 * its buttons released and the axes decoded before the fault.
 * @param line Line text, CR stripped.
 * @param line_len Length of @p line.
 * @param p1 Receives player 1's frame.
 * @param p2 Receives player 2's frame; may be NULL.
 * @param out_ports Receives how many player segments decoded (0, 1 or 2);
 *        may be NULL.
 * @return 0 on success; -1 when the line does not start with '|', has
 *         fewer than two segments, or player 1's segment is malformed. */
static int bk2_decode_frame(const char* line, size_t line_len,
                            bk2_frame_t* p1, bk2_frame_t* p2, int* out_ports) {
    p1->buttons = 0xFFFF;
    p1->axes[0] = p1->axes[1] = p1->axes[2] = p1->axes[3] = 128;
    if (p2) {
        p2->buttons = 0xFFFF;
        p2->axes[0] = p2->axes[1] = p2->axes[2] = p2->axes[3] = 128;
    }
    if (out_ports) *out_ports = 0;

    /* Walk the pipe-delimited segments — bounded scan, no NUL assumption. */
    if (line_len < 3 || line[0] != '|') return -1;
    size_t pos = 1;
    const char* seg[3];
    size_t seg_len[3];
    int nseg = 0;
    while (nseg < 3 && pos < line_len) {
        const char* s0 = line + pos;
        const char* e0 = (const char*)memchr(s0, '|', line_len - pos);
        if (!e0) break;
        seg[nseg] = s0;
        seg_len[nseg] = (size_t)(e0 - s0);
        nseg++;
        pos = (size_t)(e0 - line) + 1;
    }
    if (nseg < 2) return -1;   /* need the system segment and player 1 */

    if (bk2_decode_port(seg[1], seg_len[1], p1) != 0) return -1;
    if (out_ports) *out_ports = 1;

    if (nseg >= 3 && p2 && bk2_decode_port(seg[2], seg_len[2], p2) == 0)
        if (out_ports) *out_ports = 2;
    return 0;
}

int bk2_movie_parse_log(const char* text, size_t len, bk2_movie_t* out) {
    memset(out, 0, sizeof(*out));

    /* First pass: count input lines (lines starting with '|', not '[').
     * The very first '|' line is the LogKey, prefixed by "LogKey:" which
     * starts with 'L' not '|', so we don't need to special-case it. */
    int capacity = 0;
    {
        const char* p = text;
        const char* end = text + len;
        while (p < end) {
            const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
            const char* line_end = nl ? nl : end;
            if (line_end > p && p[0] == '|' && p[1] != '[') capacity++;
            if (!nl) break;
            p = nl + 1;
        }
    }
    if (capacity == 0) {
        OPL_ERROR_F("bk2", "no input frames found in Input Log.txt");
        return -1;
    }

    out->frames = (bk2_frame_t*)malloc((size_t)capacity * sizeof(bk2_frame_t));
    out->p2_frames = (bk2_frame_t*)malloc((size_t)capacity * sizeof(bk2_frame_t));
    if (!out->frames || !out->p2_frames) {
        OPL_ERROR_F("bk2", "out of memory (%d frames)", capacity);
        free(out->frames); free(out->p2_frames);
        out->frames = out->p2_frames = NULL;
        return -1;
    }

    /* Second pass: decode. */
    int frame_count = 0;
    int first_input = -1;
    int errors = 0;
    const char* p = text;
    const char* end = text + len;
    while (p < end && frame_count < capacity) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        const char* line_end = nl ? nl : end;
        size_t line_len = (size_t)(line_end - p);
        /* Strip trailing CR (BizHawk writes CRLF). */
        if (line_len > 0 && p[line_len - 1] == '\r') line_len--;

        if (line_len > 0 && p[0] == '|' && (line_len < 2 || p[1] != '[')) {
            bk2_frame_t fr, fr2;
            int ports = 0;
            if (bk2_decode_frame(p, line_len, &fr, &fr2, &ports) != 0) {
                if (errors < 3) {
                    OPL_ERROR_F("bk2", "malformed frame at index %d (skipped)",
                                frame_count);
                }
                errors++;
                fr.buttons = 0xFFFF;
                fr.axes[0] = fr.axes[1] = fr.axes[2] = fr.axes[3] = 128;
                fr2 = fr;
            }
            if (ports >= 2 && out->players < 2) out->players = 2;
            out->frames[frame_count] = fr;
            out->p2_frames[frame_count] = fr2;
            /* "Has input" for first-input offset detection: a button down OR
             * any stick off-centre (an analog-only movie still has a start). */
            int has_input = (fr.buttons != 0xFFFF) ||
                            fr.axes[0] != 128 || fr.axes[1] != 128 ||
                            fr.axes[2] != 128 || fr.axes[3] != 128;
            if (first_input < 0 && has_input) first_input = frame_count;
            frame_count++;
        }
        if (!nl) break;
        p = nl + 1;
    }

    out->frame_count = frame_count;
    out->first_input_frame = (first_input >= 0) ? first_input : frame_count;
    if (errors > 0) {
        OPL_ERROR_F("bk2", "%d malformed frame(s) total", errors);
    }
    return 0;
}

int bk2_movie_load(const char* path, bk2_movie_t* out) {
    memset(out, 0, sizeof(*out));

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0)) {
        OPL_ERROR_F("bk2", "cannot open %s as zip: %s",
                    path, mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        return -1;
    }

    size_t log_size = 0;
    void* log_data = mz_zip_reader_extract_file_to_heap(
        &zip, "Input Log.txt", &log_size, 0);
    if (!log_data) {
        OPL_ERROR_F("bk2", "%s has no 'Input Log.txt' entry: %s",
                    path,
                    mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        mz_zip_reader_end(&zip);
        return -1;
    }

    int rc = bk2_movie_parse_log((const char*)log_data, log_size, out);
    mz_free(log_data);

    /* Header.txt is optional metadata; the one line replay depends on is the
     * load delay the movie was recorded under (absent = instant). */
    size_t hdr_size = 0;
    char* hdr = (char*)mz_zip_reader_extract_file_to_heap(&zip, "Header.txt",
                                                          &hdr_size, 0);
    if (hdr) {
        const char* key = "LoadDelay ";
        for (size_t i = 0; i + strlen(key) < hdr_size; i++) {
            if ((i == 0 || hdr[i - 1] == '\n') &&
                memcmp(hdr + i, key, strlen(key)) == 0) {
                out->load_delay = atoi(hdr + i + strlen(key));
                break;
            }
        }
        mz_free(hdr);
    }
    mz_zip_reader_end(&zip);
    return rc;
}

void bk2_movie_free(bk2_movie_t* m) {
    if (!m) return;
    free(m->frames);
    free(m->p2_frames);
    memset(m, 0, sizeof(*m));
}
