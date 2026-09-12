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
#include "openpete_fiber.h"      /* openpete_cd_load_delay */
#include "openpete_input_log.h"
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
    int          start_idx;  /* input_log index of the movie's first frame */
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
                input_log_count() - s_rec.start_idx);
    (void)bk2_record_finalize();
}

int bk2_record_begin(const char* path, const char* args_sidecar) {
    if (s_rec.active) {
        OPL_ERROR_F("bk2-record", "begin called twice");
        return -1;
    }
    (void)openpete_on_guest_discontinuity(bk2_record_on_guest_discontinuity);
    s_rec.path = strdup(path);
    if (!s_rec.path) {
        OPL_ERROR_F("bk2-record", "out of memory at begin");
        memset(&s_rec, 0, sizeof(s_rec));
        return -1;
    }
    /* The sidecar is a nice-to-have; a failed copy just skips it. */
    s_rec.args = args_sidecar ? strdup(args_sidecar) : NULL;
    if (args_sidecar && !s_rec.args)
        OPL_ERROR_F("bk2-record", "out of memory copying args sidecar; skipping it");
    s_rec.start_idx = input_log_count();
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

void bk2_record_note(uint16_t bits, int log_idx) {
    if (!s_rec.active) return;
    /* Diff against the previous frame and log press/release transitions
     * so a debug-level run shows the same input timeline that lands in
     * the .bk2. The bits are already in the input log; nothing buffers. */
    if (bits != s_rec.prev_bits) {
        char p_buf[17], r_buf[17];
        bk2_format_button_diff(s_rec.prev_bits, bits, p_buf, r_buf);
        OPL_INFO_F("bk2-record", "frame %d: press=[%s] release=[%s] bits=0x%04x",
                    log_idx - s_rec.start_idx, p_buf, r_buf, bits);
    }
    s_rec.prev_bits = bits;
}

/* Append one player's segment: 4 axes then the 17 digital columns.
 *
 * Octoshock emits a port as axes first, then bool buttons — the order
 * BizHawk's log-entry generator produces by walking the controller
 * definition's axes before its buttons — so a second player is this same
 * segment again under a "P2 " prefix. Axes are %5d to match its PadLeft(5). */
static int format_port(char* out, const openpete_input_port_t* p) {
    int n = sprintf(out, "%5d,%5d,%5d,%5d,",
                    p->axes[0], p->axes[1], p->axes[2], p->axes[3]);
    for (int i = 0; i < 16; i++)
        out[n++] = (p->bits & kButtonChars[i].mask) ? '.' : kButtonChars[i].letter;
    out[n++] = '.';   /* MODE column — replay is always analog, no toggle */
    out[n++] = '|';
    return n;
}

/* Render one frame line into `out` (must hold >= 64 chars per player).
 * Returns bytes written (excluding NUL). Every line is the same length, so
 * the buffer size can be computed from one probe. */
static int format_frame_rec(char* out, const openpete_input_rec_t* r,
                            int players) {
    /* Constant segment 0: disc-select 1, then Open|Close|Reset unpressed. */
    int n = sprintf(out, "|    1,...|");
    for (int pl = 0; pl < players; pl++)
        n += format_port(out + n, &r->port[pl]);
    out[n++] = '\n';
    out[n] = '\0';
    return n;
}

/* One player's column names, in the order Octoshock's controller definition
 * yields them. `%d` takes the player number. */
static const char* const kPortKeyFmt =
    "#P%d LStick X|P%d LStick Y|P%d RStick X|P%d RStick Y|"
    "P%d Up|P%d Down|P%d Left|P%d Right|P%d Select|P%d Start|"
    "P%d Square|P%d Triangle|P%d Circle|P%d Cross|"
    "P%d L1|P%d R1|P%d L2|P%d R2|P%d L3|P%d R3|P%d MODE|";

/* Build the [Input] header for `players` ports. The key is documentation
 * rather than a parsing directive — BizHawk reads frames positionally
 * against whatever controller definition the core is configured for, which
 * is why a multi-player movie also has to carry SyncSettings. */
static int format_log_key(char* out, size_t cap, int players) {
    int n = snprintf(out, cap, "[Input]\r\n"
                     "LogKey:#Disc Select|Open|Close|Reset|");
    for (int pl = 1; pl <= players; pl++) {
        int p = pl;
        n += snprintf(out + n, cap - (size_t)n, kPortKeyFmt,
                      p, p, p, p, p, p, p, p, p, p, p,
                      p, p, p, p, p, p, p, p, p, p, p);
    }
    n += snprintf(out + n, cap - (size_t)n, "\r\n");
    return n;
}

/* Core sync settings for a two-player movie.
 *
 * A movie whose log key declares two ports only parses against a core
 * configured for two, so the player count has to travel with the movie.
 * Octoshock takes it from this lump: DualShock (2) in each of the PSX's two
 * real controller ports, which are Devices8 slots 0 and 4 — slots 1-3 and
 * 5-7 exist only behind a multitap, and Spyro never used one. Memcards
 * matches the core's own default so nothing else about the session shifts. */
static const char* const kSyncSettings2P =
    "{\"EnableLEC\":false,\"FIOConfig\":{"
    "\"Multitaps\":[false,false],"
    "\"Memcards\":[true,false],"
    "\"Devices8\":[2,0,0,0,2,0,0,0]}}";

static const char* const kInputTrailer = "[/Input]\r\n";

static const char* const kHeader =
    "MovieVersion BizHawk v2.0\r\n"
    "Author openpete\r\n"
    "emuVersion openpete-bk2-record\r\n"
    "Platform PSX\r\n"
    "Core Octoshock\r\n"
    "rerecordCount 0\r\n";

/* The header plus the load delay the movie was recorded under. Loads consume
 * VBLs, so every input after a load lands on a different tick under another
 * value; replay refuses the mismatch. A movie recorded instant carries no
 * line and stays byte-identical to what this writer always produced. */
static int build_header(char* out, size_t cap) {
    int n = snprintf(out, cap, "%s", kHeader);
    if (openpete_cd_load_delay > 0)
        n += snprintf(out + n, cap - (size_t)n, "LoadDelay %d\r\n",
                      openpete_cd_load_delay);
    return n;
}

int bk2_record_finalize(void) {
    if (!s_rec.active) return 0;

    /* The movie is the input-log window [start_idx, count). */
    int frame_count = input_log_count() - s_rec.start_idx;
    if (frame_count < 0) frame_count = 0;   /* defensive (log reset under us) */

    /* How many ports the movie declares. A slot that was present for any
     * recorded frame is a player for the whole movie: BizHawk fixes the port
     * count in SyncSettings, and no per-frame connect/disconnect column
     * exists to say otherwise, so a slot that comes and goes serialises its
     * absent frames as neutral. */
    int players = 1;
    for (int i = 0; i < frame_count; i++) {
        const openpete_input_rec_t* r = input_log_get(s_rec.start_idx + i);
        if (r && r->port[1].present) { players = 2; break; }
    }

    /* Build Input Log.txt in memory: header + N frame lines + trailer. */
    char key_buf[1024];
    size_t key_len     = (size_t)format_log_key(key_buf, sizeof key_buf, players);
    size_t trailer_len = strlen(kInputTrailer);
    /* Every line is the same width, so one probe sizes the buffer. */
    char frame_buf[192];
    openpete_input_rec_t probe;
    for (int i = 0; i < OPENPETE_PAD_SLOTS; i++) input_port_clear(&probe.port[i]);
    int  frame_line_len = format_frame_rec(frame_buf, &probe, players);
    size_t log_size = key_len
                    + (size_t)frame_count * (size_t)frame_line_len
                    + trailer_len;
    char* log = (char*)malloc(log_size + 1);
    if (!log) {
        OPL_ERROR_F("bk2-record", "out of memory at finalize (%zu bytes)", log_size);
        free(s_rec.path); free(s_rec.args);
        memset(&s_rec, 0, sizeof(s_rec));
        return -1;
    }
    char* p = log;
    memcpy(p, key_buf, key_len); p += key_len;
    for (int i = 0; i < frame_count; i++) {
        const openpete_input_rec_t* r = input_log_get(s_rec.start_idx + i);
        if (!r) break;   /* defensive; log frozen mid-recording */
        p += format_frame_rec(p, r, players);
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
    char header[256];
    build_header(header, sizeof header);
    if (!mz_zip_writer_add_mem(&zip, "Header.txt", header, strlen(header),
                               MZ_DEFAULT_COMPRESSION)) {
        OPL_ERROR_F("bk2-record", "Header.txt add failed");
        goto out_close;
    }
    if (!mz_zip_writer_add_mem(&zip, "Input Log.txt", log, (size_t)(p - log),
                               MZ_DEFAULT_COMPRESSION)) {
        OPL_ERROR_F("bk2-record", "Input Log.txt add failed");
        goto out_close;
    }
    /* Only a multi-player movie carries this: a one-player recording stays
     * byte-identical to what this writer has always produced, and loads
     * against whatever single-pad configuration the user already has. */
    if (players > 1 &&
        !mz_zip_writer_add_mem(&zip, "SyncSettings.json", kSyncSettings2P,
                               strlen(kSyncSettings2P), MZ_DEFAULT_COMPRESSION)) {
        OPL_ERROR_F("bk2-record", "SyncSettings.json add failed");
        goto out_close;
    }
    if (!mz_zip_writer_finalize_archive(&zip)) {
        OPL_ERROR_F("bk2-record", "finalize failed");
        goto out_close;
    }
    OPL_INFO_F("bk2-record", "wrote %d frames to %s", frame_count, s_rec.path);
    /* Drop the launch-args sidecar next to the movie now that the .bk2
     * exists, so `--bk2 <path>` picks it up on replay. */
    if (s_rec.args)
        write_args_sidecar(s_rec.path, s_rec.args);
    rc = 0;
out_close:
    mz_zip_writer_end(&zip);
out:
    free(log);
    free(s_rec.path); free(s_rec.args);
    memset(&s_rec, 0, sizeof(s_rec));
    return rc;
}
