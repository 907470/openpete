/* core/runtime/bk2_record.c — see bk2_record.h.
 *
 * Output schema mirrors what BizHawk Octoshock 2.9.1 writes (the same
 * dialect bk2_movie.c parses). One Input Log frame per call to
 * bk2_record_append, plus a stub Header.txt so the file looks valid.
 *
 * Frame line layout (matches the LogKey we emit):
 *   |    1,...|  128,  128,  128,  128,XXXXXXXXXXXXXXXX.|
 *
 * - Segment 0 ("    1,...") is constant: disc-select=1 plus three
 *   '.' for Open|Close|Reset. We never emit disc-swap or reset events.
 * - Segment 1 starts with 4 analog axes always at default 128 (we
 *   only record digital), followed by 16 digital chars + 1 MODE char.
 * - Each digital char is '.' when released, the BizHawk display
 *   letter when pressed (U/D/L/R/s/S/Q/T/O/X/l/r/L/R/L/R) — only the
 *   '.'-vs-non-'.' distinction matters to bk2_movie_load, but the
 *   letters mean the file round-trips through BizHawk too.
 */
#include "bk2_record.h"
#include "openpete_log.h"

#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PSX pad bit → digital-position index (matches kBk2P1DigitalMask in
 * bk2_movie.c). Position 16 (MODE) is never set. */
static const struct { uint16_t mask; char letter; } kButtonChars[16] = {
    { 0x0010, 'U' }, { 0x0040, 'D' }, { 0x0080, 'L' }, { 0x0020, 'R' },
    { 0x0001, 's' }, { 0x0008, 'S' },
    { 0x8000, 'Q' }, { 0x1000, 'T' }, { 0x2000, 'O' }, { 0x4000, 'X' },
    { 0x0400, 'l' }, { 0x0800, 'r' },
    { 0x0100, 'L' }, { 0x0200, 'R' },
    { 0x0002, 'L' }, { 0x0004, 'R' },
};

static struct {
    char*     path;
    uint16_t* frames;
    int       count;
    int       capacity;
    int       active;
} s_rec = {0};

int bk2_record_begin(const char* path) {
    if (s_rec.active) {
        OPL_ERROR_F("bk2-record", "begin called twice");
        return -1;
    }
    s_rec.path = strdup(path);
    s_rec.capacity = 16 * 1024;
    s_rec.frames = (uint16_t*)malloc((size_t)s_rec.capacity * sizeof(uint16_t));
    if (!s_rec.path || !s_rec.frames) {
        OPL_ERROR_F("bk2-record", "out of memory at begin");
        free(s_rec.path); free(s_rec.frames);
        memset(&s_rec, 0, sizeof(s_rec));
        return -1;
    }
    s_rec.count = 0;
    s_rec.active = 1;
    OPL_INFO_F("bk2-record", "recording to %s", path);
    return 0;
}

void bk2_record_append(uint16_t bits) {
    if (!s_rec.active) return;
    if (s_rec.count == s_rec.capacity) {
        int new_cap = s_rec.capacity * 2;
        uint16_t* g = (uint16_t*)realloc(s_rec.frames, (size_t)new_cap * sizeof(uint16_t));
        if (!g) {
            /* Drop the frame rather than terminating the run. The recorded
             * movie will just be truncated; better than crashing the game. */
            OPL_ERROR_F("bk2-record", "realloc failed at %d frames; recording truncated",
                        s_rec.count);
            s_rec.active = 0;
            return;
        }
        s_rec.frames = g;
        s_rec.capacity = new_cap;
    }
    s_rec.frames[s_rec.count++] = bits;
}

/* Render one frame line into `out` (must hold >= 64 chars). Returns
 * bytes written (excluding NUL). */
static int format_frame(char* out, uint16_t bits) {
    /* Constant segment 0 + 4 analog defaults. */
    int n = sprintf(out, "|    1,...|  128,  128,  128,  128,");
    /* 16 digital chars + 1 MODE ('.') + closing pipe + newline. */
    for (int i = 0; i < 16; i++) {
        out[n++] = (bits & kButtonChars[i].mask) ? '.' : kButtonChars[i].letter;
    }
    out[n++] = '.';   /* MODE — we don't track analog toggles */
    out[n++] = '|';
    out[n++] = '\n';
    out[n] = '\0';
    return n;
}

static const char* const kLogKey =
    "[Input]\r\n"
    "LogKey:#Disc Select|Open|Close|Reset|"
    "#P1 LStick X|P1 LStick Y|P1 RStick X|P1 RStick Y|"
    "P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|"
    "P1 Square|P1 Triangle|P1 Circle|P1 Cross|"
    "P1 L1|P1 R1|P1 L2|P1 R2|P1 L3|P1 R3|P1 MODE|\r\n";

static const char* const kInputTrailer = "[/Input]\r\n";

static const char* const kHeader =
    "MovieVersion BizHawk v2.0\r\n"
    "Author openpete\r\n"
    "emuVersion openpete-bk2-record\r\n"
    "Platform PSX\r\n"
    "Core Octoshock\r\n"
    "rerecordCount 0\r\n";

int bk2_record_finalize(void) {
    if (!s_rec.active) return 0;

    /* Build Input Log.txt in memory: header + N frame lines + trailer. */
    size_t key_len     = strlen(kLogKey);
    size_t trailer_len = strlen(kInputTrailer);
    /* Each frame line is exactly 53 bytes ("|    1,...|  128,  128,  128,  128,"
     * = 32, then 16 digital + 1 MODE + '|' + '\n' = 19. Total 51 + '\r' for
     * BizHawk-style CRLF? We write LF; readers we care about handle either.) */
    char frame_buf[64];
    int  frame_line_len = format_frame(frame_buf, 0xFFFF);
    size_t log_size = key_len
                    + (size_t)s_rec.count * (size_t)frame_line_len
                    + trailer_len;
    char* log = (char*)malloc(log_size + 1);
    if (!log) {
        OPL_ERROR_F("bk2-record", "out of memory at finalize (%zu bytes)", log_size);
        free(s_rec.path); free(s_rec.frames);
        memset(&s_rec, 0, sizeof(s_rec));
        return -1;
    }
    char* p = log;
    memcpy(p, kLogKey, key_len); p += key_len;
    for (int i = 0; i < s_rec.count; i++) {
        int n = format_frame(p, s_rec.frames[i]);
        p += n;
    }
    memcpy(p, kInputTrailer, trailer_len); p += trailer_len;
    *p = '\0';

    /* Pack into a .bk2 zip alongside Header.txt. */
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    int rc = -1;
    if (!mz_zip_writer_init_file(&zip, s_rec.path, 0)) {
        OPL_ERROR_F("bk2-record", "cannot open %s for write: %s",
                    s_rec.path,
                    mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        goto out;
    }
    if (!mz_zip_writer_add_mem(&zip, "Header.txt", kHeader, strlen(kHeader),
                               MZ_DEFAULT_COMPRESSION)) {
        OPL_ERROR_F("bk2-record", "Header.txt add failed");
        goto out_close;
    }
    if (!mz_zip_writer_add_mem(&zip, "Input Log.txt", log, (size_t)(p - log),
                               MZ_DEFAULT_COMPRESSION)) {
        OPL_ERROR_F("bk2-record", "Input Log.txt add failed");
        goto out_close;
    }
    if (!mz_zip_writer_finalize_archive(&zip)) {
        OPL_ERROR_F("bk2-record", "finalize failed");
        goto out_close;
    }
    OPL_INFO_F("bk2-record", "wrote %d frames to %s", s_rec.count, s_rec.path);
    rc = 0;
out_close:
    mz_zip_writer_end(&zip);
out:
    free(log);
    free(s_rec.path); free(s_rec.frames);
    memset(&s_rec, 0, sizeof(s_rec));
    return rc;
}
