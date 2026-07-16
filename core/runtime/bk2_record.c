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
 * - Segment 1 starts with the 4 analog axes (LX, LY, RX, RY), followed
 *   by 16 digital chars + 1 MODE char ('M' when the pad was analog).
 * - Each digital char is '.' when released, the BizHawk display
 *   letter when pressed (U/D/L/R/s/S/Q/T/O/X/l/r/L/R/L/R) — only the
 *   '.'-vs-non-'.' distinction matters to bk2_movie_load, but the
 *   letters mean the file round-trips through BizHawk too.
 */
#include "bk2_record.h"
#include "bk2_movie.h"   /* bk2_frame_t */
#include "openpete_discontinuity.h"
#include "openpete_log.h"

#include "miniz.h"

#include <errno.h>
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
    char*        path;
    char*        args;       /* .args sidecar content, or NULL — written at finalize */
    bk2_frame_t* frames;
    int          count;
    int          capacity;
    int          active;
    uint16_t     prev_bits;  /* last bits seen, for press/release diff logging */
} s_rec = {0};

/* A savestate load replaces guest state mid-recording. Frames appended
 * after the jump would splice two timelines into one non-replayable movie
 * (the replay can't reproduce the load), so finalize NOW: the .bk2 on disk
 * stays valid up to the load point, and appends after this no-op. Proper
 * truncate-to-loaded-frame support arrives with the input_log lift-out
 * (savestate plan P1). */
static void bk2_record_on_guest_discontinuity(void) {
    if (!s_rec.active) return;
    OPL_ERROR_F("bk2-record",
                "guest state replaced (savestate load) at frame %d — "
                "finalizing movie here; frames after the load are NOT recorded",
                s_rec.count);
    (void)bk2_record_finalize();
}

int bk2_record_begin(const char* path, const char* args_sidecar) {
    if (s_rec.active) {
        OPL_ERROR_F("bk2-record", "begin called twice");
        return -1;
    }
    (void)openpete_on_guest_discontinuity(bk2_record_on_guest_discontinuity);
    s_rec.path = strdup(path);
    s_rec.capacity = 16 * 1024;
    s_rec.frames = (bk2_frame_t*)malloc((size_t)s_rec.capacity * sizeof(bk2_frame_t));
    if (!s_rec.path || !s_rec.frames) {
        OPL_ERROR_F("bk2-record", "out of memory at begin");
        free(s_rec.path); free(s_rec.frames);
        memset(&s_rec, 0, sizeof(s_rec));
        return -1;
    }
    /* The sidecar is a nice-to-have; a failed copy just skips it. */
    s_rec.args = args_sidecar ? strdup(args_sidecar) : NULL;
    if (args_sidecar && !s_rec.args)
        OPL_ERROR_F("bk2-record", "out of memory copying args sidecar; skipping it");
    s_rec.count = 0;
    s_rec.active = 1;
    s_rec.prev_bits = 0xFFFF;  /* all released */
    OPL_INFO_F("bk2-record", "recording to %s", path);
    return 0;
}

/* Write "<bk2_path>.args" with `content`. Best-effort: a failure is
 * logged but never fails the recording (the .bk2 is the real output). */
static void write_args_sidecar(const char* bk2_path, const char* content) {
    size_t plen = strlen(bk2_path);
    char* apath = (char*)malloc(plen + 6);  /* ".args" + NUL */
    if (!apath) {
        OPL_ERROR_F("bk2-record", "out of memory building .args path");
        return;
    }
    memcpy(apath, bk2_path, plen);
    memcpy(apath + plen, ".args", 6);

    FILE* f = fopen(apath, "wb");
    if (!f) {
        OPL_ERROR_F("bk2-record", "cannot open %s for write: %s", apath, strerror(errno));
        free(apath);
        return;
    }
    size_t clen = strlen(content);
    if (clen && fwrite(content, 1, clen, f) != clen)
        OPL_ERROR_F("bk2-record", "short write on %s", apath);
    else
        OPL_INFO_F("bk2-record", "wrote args sidecar %s", apath);
    fclose(f);
    free(apath);
}

void bk2_format_button_diff(uint16_t prev, uint16_t cur,
                            char press_out[17], char release_out[17]) {
    /* PSX convention: 1=released, 0=pressed. A press is prev=1 -> cur=0. */
    uint16_t pressed  = (uint16_t)(prev & ~cur);
    uint16_t released = (uint16_t)(~prev & cur);
    int pn = 0, rn = 0;
    for (int i = 0; i < 16; i++) {
        if (pressed  & kButtonChars[i].mask) press_out[pn++]   = kButtonChars[i].letter;
        if (released & kButtonChars[i].mask) release_out[rn++] = kButtonChars[i].letter;
    }
    press_out[pn] = '\0';
    release_out[rn] = '\0';
}

void bk2_record_append(uint16_t bits, const uint8_t axes[4]) {
    if (!s_rec.active) return;
    /* Diff against the previous frame and log press/release transitions
     * so a debug-level run shows the same input timeline that lands in
     * the .bk2. */
    if (bits != s_rec.prev_bits) {
        char p_buf[17], r_buf[17];
        bk2_format_button_diff(s_rec.prev_bits, bits, p_buf, r_buf);
        OPL_INFO_F("bk2-record", "frame %d: press=[%s] release=[%s] bits=0x%04x",
                    s_rec.count, p_buf, r_buf, bits);
    }
    s_rec.prev_bits = bits;
    if (s_rec.count == s_rec.capacity) {
        int new_cap = s_rec.capacity * 2;
        bk2_frame_t* g = (bk2_frame_t*)realloc(s_rec.frames, (size_t)new_cap * sizeof(bk2_frame_t));
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
    bk2_frame_t* fr = &s_rec.frames[s_rec.count++];
    fr->buttons = bits;
    if (axes) {
        fr->axes[0] = axes[0]; fr->axes[1] = axes[1];
        fr->axes[2] = axes[2]; fr->axes[3] = axes[3];
    } else {
        fr->axes[0] = fr->axes[1] = fr->axes[2] = fr->axes[3] = 128;
    }
}

/* Render one frame line into `out` (must hold >= 64 chars). Returns
 * bytes written (excluding NUL). The 4 axes are emitted %5d (fixed width,
 * matching Octoshock's "  128" columns) so every line is the same length. */
static int format_frame(char* out, const bk2_frame_t* fr) {
    /* Constant segment 0 + 4 analog axes. */
    int n = sprintf(out, "|    1,...|%5d,%5d,%5d,%5d,",
                    fr->axes[0], fr->axes[1], fr->axes[2], fr->axes[3]);
    /* 16 digital chars + 1 MODE + closing pipe + newline. */
    for (int i = 0; i < 16; i++) {
        out[n++] = (fr->buttons & kButtonChars[i].mask) ? '.' : kButtonChars[i].letter;
    }
    out[n++] = '.';   /* MODE column — replay is always analog, no toggle */
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
    bk2_frame_t probe = { 0xFFFF, {128, 128, 128, 128} };
    int  frame_line_len = format_frame(frame_buf, &probe);
    size_t log_size = key_len
                    + (size_t)s_rec.count * (size_t)frame_line_len
                    + trailer_len;
    char* log = (char*)malloc(log_size + 1);
    if (!log) {
        OPL_ERROR_F("bk2-record", "out of memory at finalize (%zu bytes)", log_size);
        free(s_rec.path); free(s_rec.args); free(s_rec.frames);
        memset(&s_rec, 0, sizeof(s_rec));
        return -1;
    }
    char* p = log;
    memcpy(p, kLogKey, key_len); p += key_len;
    for (int i = 0; i < s_rec.count; i++) {
        int n = format_frame(p, &s_rec.frames[i]);
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
    /* Drop the launch-args sidecar next to the movie now that the .bk2
     * exists, so `--bk2 <path>` picks it up on replay. */
    if (s_rec.args)
        write_args_sidecar(s_rec.path, s_rec.args);
    rc = 0;
out_close:
    mz_zip_writer_end(&zip);
out:
    free(log);
    free(s_rec.path); free(s_rec.args); free(s_rec.frames);
    memset(&s_rec, 0, sizeof(s_rec));
    return rc;
}
