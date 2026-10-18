/* core/record/bk2_record.c - see bk2_record.h.
 *
 * Output schema mirrors what BizHawk Octoshock 2.9.1 writes (the same
 * dialect bk2_movie.c parses). At finalize, the input-log entries between
 * bk2_record_begin and bk2_record_finalize are written as one Input Log line
 * per pad write of a yield, beside a Header.txt and the per-E row counts in
 * Input Groups.txt.
 *
 * Frame line layout (matches the LogKey we emit):
 *   |    1,...|  128,  128,  128,  128,XXXXXXXXXXXXXXXX.|
 *
 * - Segment 0 ("    1,...") is constant: disc-select=1 plus three
 *   '.' for Open|Close|Reset. We never emit disc-swap or reset events.
 * - Segment 1 starts with the 4 analog axes (LX, LY, RX, RY), followed
 *   by 16 digital chars + 1 MODE char, always '.'. A two-player movie
 *   repeats this segment for port 2.
 * - Each digital char is '.' when released, the BizHawk display
 *   letter when pressed (U/D/L/R/s/S/Q/T/O/X/l/r/L/R/L/R) - only the
 *   '.'-vs-non-'.' distinction matters to bk2_movie_load, but the
 *   letters mean the file round-trips through BizHawk too.
 */
#include "bk2_record.h"
#include "bk2_movie.h"   /* bk2_frame_t */
#include "openpete_atomic_file.h"
#include "openpete_discontinuity.h"
#include "openpete_fiber.h"      /* openpete_cd_load_delay */
#include "openpete_input_log.h"
#include "openpete_log.h"

#include "miniz.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PSX pad bit -> digital-position index (matches kBk2P1DigitalMask in
 * bk2_movie.c). Position 16 (MODE) is never set. */
static const struct { uint16_t mask; char letter; } kButtonChars[16] = {
    { 0x0010, 'U' }, { 0x0040, 'D' }, { 0x0080, 'L' }, { 0x0020, 'R' },
    { 0x0001, 's' }, { 0x0008, 'S' },
    { 0x8000, 'Q' }, { 0x1000, 'T' }, { 0x2000, 'O' }, { 0x4000, 'X' },
    { 0x0400, 'l' }, { 0x0800, 'r' },
    { 0x0100, 'L' }, { 0x0200, 'R' },
    { 0x0002, 'L' }, { 0x0004, 'R' },
};

/* One recorded entry's pad writes: the (E, j) of its first and the E of the
 * rest. */
typedef struct {
    unsigned writes;
    unsigned e;
    unsigned e_rest;
    unsigned j;
} bk2_rec_entry_t;

static struct {
    char*        path;
    char*        args;       /* .args sidecar content, or NULL - written at finalize */
    int          start_idx;  /* input_log index of the movie's first frame */
    int          active;
    uint16_t     prev_bits;  /* last bits seen, for press/release diff logging */
    bk2_rec_entry_t* rows;   /* per entry, indexed from start_idx */
    int          rows_cap;
} s_rec = {0};

/** @brief Finalize the recording when guest state is replaced (a savestate load).
 *
 * Input after the load would splice two timelines into one movie the
 * replay cannot reproduce, so the movie is written up to the load point
 * and recording ends; input after the load is not recorded. The movie is
 * not truncated back to the loaded state's VBL. No effect when recording
 * is not active. */
static void bk2_record_on_guest_discontinuity(void) {
    if (!s_rec.active) return;
    OPL_ERROR_F("bk2-record",
                "guest state replaced (savestate load) at frame %d - "
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

/** @brief Replace "<bk2_path>.args" with the sidecar content atomically (synced).
 *
 * Best-effort: a failure is logged, leaves any previous sidecar whole, and
 * never fails the recording.
 * @param bk2_path The .bk2 path.
 * @param content Sidecar text, written verbatim. */
static void write_args_sidecar(const char* bk2_path, const char* content) {
    size_t plen = strlen(bk2_path);
    char* apath = (char*)malloc(plen + 6);  /* ".args" + NUL */
    if (!apath) {
        OPL_ERROR_F("bk2-record", "out of memory building .args path");
        return;
    }
    memcpy(apath, bk2_path, plen);
    memcpy(apath + plen, ".args", 6);

    if (op_atomic_write_file(apath, content, strlen(content)) != 0)
        OPL_ERROR_F("bk2-record", "args sidecar %s not written", apath);
    else
        OPL_INFO_F("bk2-record", "wrote args sidecar %s", apath);
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

void bk2_record_note(uint16_t bits, int log_idx, unsigned writes, unsigned e,
                     unsigned e_rest, unsigned j) {
    if (!s_rec.active) return;
    const int at = log_idx - s_rec.start_idx;
    if (at >= 0) {
        if (at >= s_rec.rows_cap) {
            int cap = s_rec.rows_cap ? s_rec.rows_cap : 4096;
            while (cap <= at) cap *= 2;
            bk2_rec_entry_t* grown = (bk2_rec_entry_t*)realloc(s_rec.rows, (size_t)cap * sizeof *grown);
            if (!grown) {
                OPL_ERROR_F("bk2-record", "out of memory growing the row counts");
                return;
            }
            memset(grown + s_rec.rows_cap, 0, (size_t)(cap - s_rec.rows_cap) * sizeof *grown);
            s_rec.rows = grown;
            s_rec.rows_cap = cap;
        }
        s_rec.rows[at] = (bk2_rec_entry_t){writes, e, e_rest, j};
    }
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

/** @brief Write one player's segment: 4 axes, the 16 button columns, the MODE column, and the closing pipe.
 *
 * Octoshock emits a port as axes first, then bool buttons (BizHawk's
 * log-entry generator walks the controller definition's axes before its
 * buttons), so a second player is this same segment again. Axes are %5d to
 * match its PadLeft(5). A pressed button writes its letter, a released one
 * '.'; MODE is always '.' since replay is always analog.
 * @param out Destination; not NUL-terminated.
 * @param p Port record.
 * @return Bytes written. */
static int format_port(char* out, const openpete_input_port_t* p) {
    int n = sprintf(out, "%5d,%5d,%5d,%5d,",
                    p->axes[0], p->axes[1], p->axes[2], p->axes[3]);
    for (int i = 0; i < 16; i++)
        out[n++] = (p->bits & kButtonChars[i].mask) ? '.' : kButtonChars[i].letter;
    out[n++] = '.';   /* MODE column - replay is always analog, no toggle */
    out[n++] = '|';
    return n;
}

/** @brief Write one Input Log line: the constant system segment, then each player's segment and a newline.
 *
 * Every line is the same length while each axis fits %5d, so the finalize
 * sizes its buffer from one probe line.
 * @param out Destination; must hold 64 bytes per player; NUL-terminated.
 * @param r Input log record.
 * @param players Ports to write, 1 or 2.
 * @return Bytes written, excluding the NUL. */
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

/** @brief Write the [Input] line and the LogKey line for a number of ports.
 *
 * The key is documentation rather than a parsing directive: BizHawk reads
 * lines positionally against the controller definition the core is
 * configured for, which is why a two-player movie also carries
 * SyncSettings.
 * @param out Destination.
 * @param cap Capacity of @p out.
 * @param players Ports, 1 or 2.
 * @return Bytes written, excluding the NUL. */
static int format_log_key(char* out, size_t cap, int players) {
    int n = snprintf(out, cap, "[Input]\r\n"
                     "LogKey:#Disc Select|Open|Close|Reset|");
    for (int pl = 1; pl <= players; pl++) {
        int p = pl;
        n += snprintf(out + n, cap - (size_t)n, kPortKeyFmt,
                      p, p, p, p, p, p, p, p, p, p, p,
                      p, p, p, p, p, p, p, p, p, p);
    }
    n += snprintf(out + n, cap - (size_t)n, "\r\n");
    return n;
}

/* Core sync settings for a two-player movie.
 *
 * A movie whose log key declares two ports only parses against a core
 * configured for two, so the player count has to travel with the movie.
 * Octoshock takes it from this lump: DualShock (2) in each of the PSX's two
 * real controller ports, which are Devices8 slots 0 and 4 - slots 1-3 and
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

/** @brief Write Header.txt's text: the fixed header, the load delay the movie was recorded under, and its row key.
 *
 * Loads consume VBLs, so every input after a load lands on a different
 * tick under another delay; replay refuses the mismatch. A delay of 0
 * writes no LoadDelay line. Every movie has one row per pad write of a yield,
 * grouped by openpete_consume, and says so with "OpenpeteFeedKey consume".
 * @param out Destination.
 * @param cap Capacity of @p out.
 * @return Bytes written, excluding the NUL. */
static int build_header(char* out, size_t cap) {
    int n = snprintf(out, cap, "%s", kHeader);
    if (openpete_cd_load_delay > 0)
        n += snprintf(out + n, cap - (size_t)n, "LoadDelay %d\r\n",
                      openpete_cd_load_delay);
    n += snprintf(out + n, cap - (size_t)n, "OpenpeteFeedKey consume\r\n");
    return n;
}

/** @brief Return the Input Log lines a recorded entry expands to.
 *
 * One per pad write of a yield the entry's yield took, as noted by
 * bk2_record_note; the first entry that wrote also repeats for the writes
 * its group took before the recording began (its j).
 * @param i The entry's index from the movie's first entry.
 * @param first The index of the first entry that wrote, or -1.
 * @return The line count. */
static unsigned entry_rows(int i, int first) {
    if (!s_rec.rows || i >= s_rec.rows_cap) return 0u;
    const bk2_rec_entry_t* r = &s_rec.rows[i];
    return r->writes + (i == first ? r->j : 0u);
}

/** @brief Return the index of the first recorded entry that took a write.
 * @param count Entries in the window.
 * @return The index, or -1 when no entry wrote. */
static int first_written_entry(int count) {
    for (int i = 0; i < count && i < s_rec.rows_cap; i++)
        if (s_rec.rows && s_rec.rows[i].writes) return i;
    return -1;
}

/** @brief Serialise the per-E row counts as Input Groups.txt text.
 *
 * One line per E from 0 to the last written entry's E, holding the rows
 * written for it; an E no recorded write took writes 0, so the movie replays
 * from boot at offset 0.
 * @param count Entries in the window.
 * @param out_len Receives the text's length.
 * @return The malloc'd text; NULL when no entry wrote or allocation
 *         fails, with the reason logged. */
static char* build_groups(int count, size_t* out_len) {
    const int first = first_written_entry(count);
    if (first < 0) {
        OPL_ERROR_F("bk2-record", "no pad write in the recorded window; no rows to write");
        return NULL;
    }
    unsigned e_last = s_rec.rows[first].e;
    for (int i = first; i < count && i < s_rec.rows_cap; i++)
        if (s_rec.rows[i].writes) e_last = s_rec.rows[i].e_rest;
    const size_t groups = (size_t)e_last + 1u;
    unsigned* counts = (unsigned*)calloc(groups, sizeof *counts);
    char* text = (char*)malloc(groups * 11u + 1u);
    if (!counts || !text) {
        free(counts); free(text);
        OPL_ERROR_F("bk2-record", "out of memory writing %zu groups", groups);
        return NULL;
    }
    for (int i = first; i < count && i < s_rec.rows_cap; i++) {
        const bk2_rec_entry_t* r = &s_rec.rows[i];
        if (!r->writes) continue;
        counts[r->e] += entry_rows(i, first) - (r->writes - 1u);
        counts[r->e_rest] += r->writes - 1u;
    }
    size_t n = 0;
    for (size_t g = 0; g < groups; g++) n += (size_t)sprintf(text + n, "%u\n", counts[g]);
    free(counts);
    *out_len = n;
    return text;
}

/** @brief Release the recorder's buffers and clear its state. */
static void rec_reset(void) {
    free(s_rec.path); free(s_rec.args); free(s_rec.rows);
    memset(&s_rec, 0, sizeof(s_rec));
}

/** @brief Serialise the recorded window of the input log as Input Log.txt text.
 *
 * The [Input] header and LogKey for @p players ports, then each entry's line
 * repeated entry_rows times, then the trailer. Every line has the same width,
 * so one probe line sizes the buffer.
 * @param frame_count Entries in the window [start_idx, start_idx + frame_count).
 * @param players Ports per line, 1 or 2.
 * @param out_len Receives the text's length, excluding the NUL.
 * @param out_lines Receives the number of movie rows written.
 * @return The malloc'd, NUL-terminated text; NULL when allocation fails,
 *         with the reason logged. */
static char* build_input_log(int frame_count, int players, size_t* out_len,
                             size_t* out_lines) {
    char key_buf[1024];
    size_t key_len     = (size_t)format_log_key(key_buf, sizeof key_buf, players);
    size_t trailer_len = strlen(kInputTrailer);
    char frame_buf[192];
    openpete_input_rec_t probe;
    for (int i = 0; i < OPENPETE_PAD_SLOTS; i++) input_port_clear(&probe.port[i]);
    int  frame_line_len = format_frame_rec(frame_buf, &probe, players);
    size_t line_count = 0;
    const int first = first_written_entry(frame_count);
    for (int i = 0; i < frame_count; i++) line_count += entry_rows(i, first);
    size_t log_size = key_len + line_count * (size_t)frame_line_len + trailer_len;
    char* log = (char*)malloc(log_size + 1);
    if (!log) {
        OPL_ERROR_F("bk2-record", "out of memory at finalize (%zu bytes)", log_size);
        return NULL;
    }
    char* p = log;
    memcpy(p, key_buf, key_len); p += key_len;
    for (int i = 0; i < frame_count; i++) {
        const openpete_input_rec_t* r = input_log_get(s_rec.start_idx + i);
        if (!r) break;   /* defensive; log frozen mid-recording */
        for (unsigned k = entry_rows(i, first); k > 0; k--)
            p += format_frame_rec(p, r, players);
    }
    memcpy(p, kInputTrailer, trailer_len); p += trailer_len;
    *p = '\0';
    *out_len = (size_t)(p - log);
    *out_lines = line_count;
    return log;
}

/** @brief Write the recorded window of the input log as a .bk2 zip and end recording; the caller holds input_log_lock.
 *
 * @return 0 on success or when recording is not active; -1 on an
 *         allocation or write failure, with the reason logged. */
static int rec_write_and_end(void) {
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

    /* Build Input Log.txt in memory: header + one line per movie row +
     * trailer; and the per-E row counts. */
    size_t log_len = 0, line_count = 0, groups_len = 0;
    char* log = build_input_log(frame_count, players, &log_len, &line_count);
    char* groups = log ? build_groups(frame_count, &groups_len) : NULL;
    if (!groups) {
        free(log);
        rec_reset();
        return -1;
    }

    /* The launch-args sidecar goes first, so a movie on disk always has the
     * sidecar it was recorded under (`--bk2 <path>` reads it on replay). */
    if (s_rec.args)
        write_args_sidecar(s_rec.path, s_rec.args);

    /* Pack into a .bk2 zip alongside Header.txt, on the heap; the finished
     * archive replaces the file in one rename, so a failed or interrupted
     * write leaves the previous movie whole. */
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    int rc = -1;
    void* zbuf = NULL;
    size_t zlen = 0;
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        OPL_ERROR_F("bk2-record", "cannot start the archive for %s: %s",
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
    if (!mz_zip_writer_add_mem(&zip, "Input Log.txt", log, log_len,
                               MZ_DEFAULT_COMPRESSION)) {
        OPL_ERROR_F("bk2-record", "Input Log.txt add failed");
        goto out_close;
    }
    if (!mz_zip_writer_add_mem(&zip, "Input Groups.txt", groups, groups_len,
                               MZ_DEFAULT_COMPRESSION)) {
        OPL_ERROR_F("bk2-record", "Input Groups.txt add failed");
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
    if (!mz_zip_writer_finalize_heap_archive(&zip, &zbuf, &zlen)) {
        OPL_ERROR_F("bk2-record", "finalize failed");
        goto out_close;
    }
    if (op_atomic_write_file(s_rec.path, zbuf, zlen) != 0) {
        OPL_ERROR_F("bk2-record", "cannot write %s", s_rec.path);
        goto out_close;
    }
    OPL_INFO_F("bk2-record", "wrote %zu frames to %s", line_count, s_rec.path);
    rc = 0;
out_close:
    mz_zip_writer_end(&zip);
    mz_free(zbuf);
out:
    free(log);
    free(groups);
    rec_reset();
    return rc;
}

int bk2_record_finalize(void) {
    input_log_lock();
    const int rc = rec_write_and_end();
    input_log_unlock();
    return rc;
}
