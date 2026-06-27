/* core/runtime/bk2_movie.c — see bk2_movie.h.
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
 * reader (third_party/PsyCross/src/pad/PsyX_pad.cpp:364): start at 0xFFFF,
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

/* Decode one input line into a bk2_frame_t (buttons + 4 analog axes).
 *
 * The line shape we care about is a "|...|...|" frame with at least two
 * pipe-delimited segments. Segment 1 (P1) starts with 4 comma-terminated
 * analog ints (LX, LY, RX, RY — 0..255, 128 = centre), then a 17-char
 * digital tail (16 buttons + MODE). */
static int bk2_decode_frame(const char* line, size_t line_len, bk2_frame_t* out) {
    out->buttons = 0xFFFF;
    out->axes[0] = out->axes[1] = out->axes[2] = out->axes[3] = 128;

    /* Find segment boundaries — bounded scan, no NUL assumption. */
    if (line_len < 3 || line[0] != '|') return -1;
    const char* seg0 = line + 1;
    const char* seg0_end = (const char*)memchr(seg0, '|', line_len - 1);
    if (!seg0_end) return -1;
    size_t after = (size_t)(seg0_end - line) + 1;
    if (after >= line_len) return -1;
    const char* seg1 = line + after;
    const char* seg1_end = (const char*)memchr(seg1, '|', line_len - after);
    if (!seg1_end) return -1;
    size_t seg1_len = (size_t)(seg1_end - seg1);

    /* Parse the 4 analog axes, each an int up to its comma. Octoshock always
     * emits all 4 (default 128); clamp into the PSX 0..255 range. */
    const char* p = seg1;
    const char* end = seg1 + seg1_len;
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
    if (!out->frames) {
        OPL_ERROR_F("bk2", "out of memory (%d frames)", capacity);
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
            bk2_frame_t fr;
            if (bk2_decode_frame(p, line_len, &fr) != 0) {
                if (errors < 3) {
                    OPL_ERROR_F("bk2", "malformed frame at index %d (skipped)",
                                frame_count);
                }
                errors++;
                fr.buttons = 0xFFFF;
                fr.axes[0] = fr.axes[1] = fr.axes[2] = fr.axes[3] = 128;
            }
            out->frames[frame_count] = fr;
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
    mz_zip_reader_end(&zip);
    return rc;
}

void bk2_movie_free(bk2_movie_t* m) {
    if (!m) return;
    free(m->frames);
    memset(m, 0, sizeof(*m));
}
