/* core/record/bk2_record.c - see bk2_record.h.
 *
 * Output schema mirrors what BizHawk Octoshock 2.9.1 writes (the same
 * dialect bk2_movie.c parses). At finalize, the input-log entries between
 * bk2_record_begin and bk2_record_finalize are written as one Input Log line
 * per pad write of a yield, player 1 taken from the pad the host delivered
 * at that write, beside a Header.txt and the per-E row counts in Input
 * Groups.txt.
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

/* One input-log entry's pad writes: the (E, j) of its first, the E of the
 * rest, and the absolute index of its writes' first player 1 pad. */
typedef struct {
    unsigned writes;
    unsigned e;
    unsigned e_rest;
    unsigned j;
    size_t   pad0;
} bk2_rec_entry_t;

/* The row notes, always on: one entry per input-log index in
 * [base, base + n), and player 1 at every noted write, pads[k] holding
 * absolute pad index pad_base + k. Read by the recorder and by the
 * bug-report segment writer; trimmed from the front by
 * bk2_notes_retain_from and from the back by bk2_notes_truncate. */
static struct {
    bk2_rec_entry_t* rows;
    int          base, n, cap;
    bk2_pad_write_t* pads;
    size_t       pad_base, pads_n, pads_cap;
} s_notes = {0};

static struct {
    char*        path;
    char*        args;       /* .args sidecar content, or NULL - written at finalize */
    int          start_idx;  /* input_log index of the movie's first frame */
    int          active;
    uint16_t     prev_bits;  /* last bits seen, for press/release diff logging */
} s_rec = {0};

/** @brief Return the note of an input-log entry, or NULL when the store does not hold it.
 * @param idx The entry's input-log index. */
static const bk2_rec_entry_t* note_at(int idx) {
    if (idx < s_notes.base || idx >= s_notes.base + s_notes.n) return NULL;
    return &s_notes.rows[idx - s_notes.base];
}

/** @brief Return the pad of an absolute pad index. */
static const bk2_pad_write_t* note_pad(size_t k) {
    return &s_notes.pads[k - s_notes.pad_base];
}

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

/** @brief Grow the note store to hold an entry index.
 * @param idx The entry's input-log index, at or past base.
 * @return 0 when it fits, -1 on allocation failure (logged). */
static int notes_reserve(int idx) {
    const int at = idx - s_notes.base;
    if (at < s_notes.cap) return 0;
    int cap = s_notes.cap ? s_notes.cap : 4096;
    while (cap <= at) cap *= 2;
    bk2_rec_entry_t* grown = (bk2_rec_entry_t*)realloc(s_notes.rows, (size_t)cap * sizeof *grown);
    if (!grown) {
        OPL_ERROR_F("bk2-record", "out of memory growing the row notes");
        return -1;
    }
    s_notes.rows = grown;
    s_notes.cap = cap;
    return 0;
}

/** @brief Append pads to the note store's pad array.
 * @param pads The pads.
 * @param n Their count.
 * @return 0 on success, -1 on allocation failure (logged). */
static int notes_pads_append(const bk2_pad_write_t* pads, unsigned n) {
    if (s_notes.pads_n + n > s_notes.pads_cap) {
        size_t cap = s_notes.pads_cap ? s_notes.pads_cap : 4096u;
        while (cap < s_notes.pads_n + n) cap *= 2u;
        bk2_pad_write_t* grown = (bk2_pad_write_t*)realloc(s_notes.pads, cap * sizeof *grown);
        if (!grown) {
            OPL_ERROR_F("bk2-record", "out of memory growing the pad rows");
            return -1;
        }
        s_notes.pads = grown;
        s_notes.pads_cap = cap;
    }
    memcpy(s_notes.pads + s_notes.pads_n, pads, (size_t)n * sizeof *pads);
    s_notes.pads_n += n;
    return 0;
}

void bk2_notes_retain_from(int idx) {
    if (s_rec.active && s_rec.start_idx < idx) idx = s_rec.start_idx;
    if (idx <= s_notes.base) return;
    int drop = idx - s_notes.base;
    if (drop >= s_notes.n) {
        s_notes.base = idx;
        s_notes.pad_base += s_notes.pads_n;
        s_notes.n = 0;
        s_notes.pads_n = 0;
        return;
    }
    const size_t pad_drop = s_notes.rows[drop].pad0 - s_notes.pad_base;
    memmove(s_notes.rows, s_notes.rows + drop, (size_t)(s_notes.n - drop) * sizeof *s_notes.rows);
    memmove(s_notes.pads, s_notes.pads + pad_drop, (s_notes.pads_n - pad_drop) * sizeof *s_notes.pads);
    s_notes.n -= drop;
    s_notes.base = idx;
    s_notes.pads_n -= pad_drop;
    s_notes.pad_base += pad_drop;
}

void bk2_notes_truncate(int count) {
    if (count >= s_notes.base + s_notes.n) return;
    if (count <= s_notes.base) {
        s_notes.pads_n = 0;
        s_notes.n = 0;
        s_notes.base = count < 0 ? 0 : count;
        return;
    }
    s_notes.n = count - s_notes.base;
    s_notes.pads_n = s_notes.rows[s_notes.n].pad0 - s_notes.pad_base;
}

size_t bk2_notes_held(size_t* bytes) {
    if (bytes) *bytes = (size_t)s_notes.cap * sizeof *s_notes.rows + s_notes.pads_cap * sizeof *s_notes.pads;
    return (size_t)s_notes.n;
}

void bk2_record_note(int log_idx, unsigned writes, unsigned e, unsigned e_rest,
                     unsigned j, const bk2_pad_write_t* pads) {
    if (log_idx < 0) return;
    if (log_idx < s_notes.base + s_notes.n) bk2_notes_truncate(log_idx);
    if (s_notes.n == 0) s_notes.base = log_idx;
    if (notes_reserve(log_idx) != 0) return;
    const size_t pad0 = s_notes.pad_base + s_notes.pads_n;
    /* An entry the log took without a note (a gap) holds no write. */
    while (s_notes.base + s_notes.n < log_idx)
        s_notes.rows[s_notes.n++] = (bk2_rec_entry_t){0u, 0u, 0u, 0u, pad0};
    if (writes && notes_pads_append(pads, writes) != 0) return;
    s_notes.rows[s_notes.n++] = (bk2_rec_entry_t){writes, e, e_rest, j, pad0};
    if (!s_rec.active) return;
    const int at = log_idx - s_rec.start_idx;
    /* Log each write's press/release transitions so a debug-level run
     * shows the same input timeline that lands in the .bk2. */
    for (unsigned k = 0; k < writes; k++) {
        const uint16_t bits = pads[k].bits;
        if (bits != s_rec.prev_bits) {
            char p_buf[17], r_buf[17];
            bk2_format_button_diff(s_rec.prev_bits, bits, p_buf, r_buf);
            OPL_INFO_F("bk2-record", "frame %d: press=[%s] release=[%s] bits=0x%04x",
                        at, p_buf, r_buf, bits);
        }
        s_rec.prev_bits = bits;
    }
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

/* A window of the input log the writer serialises: the entries
 * [start, start + count), their row groups numbered from group_base. */
typedef struct {
    int      start;
    int      count;
    unsigned group_base;
} bk2_window_t;

/** @brief Return the Input Log lines a window's entry expands to.
 *
 * One per pad write of a yield the entry's yield took, as noted by
 * bk2_record_note; the first entry that wrote also repeats for the writes
 * its group took before the window began (its j).
 * @param w The window.
 * @param i The entry's index from the window's first entry.
 * @param first The index of the first entry that wrote, or -1.
 * @return The line count. */
static unsigned entry_rows(const bk2_window_t* w, int i, int first) {
    const bk2_rec_entry_t* r = note_at(w->start + i);
    if (!r) return 0u;
    return r->writes + (i == first ? r->j : 0u);
}

/** @brief Return the index of a window's first entry that took a write.
 * @param w The window.
 * @return The index, or -1 when no entry wrote. */
static int first_written_entry(const bk2_window_t* w) {
    for (int i = 0; i < w->count; i++) {
        const bk2_rec_entry_t* r = note_at(w->start + i);
        if (r && r->writes) return i;
    }
    return -1;
}

/** @brief Serialise a window's per-E row counts as Input Groups.txt text.
 *
 * One line per E from the window's group base to the last written entry's
 * E, holding the rows written for it; an E no write took writes 0, so a
 * window based at E 0 replays from boot at offset 0.
 * @param w The window.
 * @param out_len Receives the text's length.
 * @return The malloc'd text; NULL when no entry wrote, an entry's E lies
 *         below the base or allocation fails, with the reason logged. */
static char* build_groups(const bk2_window_t* w, size_t* out_len) {
    const int first = first_written_entry(w);
    if (first < 0) {
        OPL_ERROR_F("bk2-record", "no pad write in the recorded window; no rows to write");
        return NULL;
    }
    const bk2_rec_entry_t* f = note_at(w->start + first);
    if (f->e < w->group_base) {
        OPL_ERROR_F("bk2-record", "the window's first write is in group %u, below its base %u",
                    f->e, w->group_base);
        return NULL;
    }
    unsigned e_last = f->e;
    for (int i = first; i < w->count; i++) {
        const bk2_rec_entry_t* r = note_at(w->start + i);
        if (r && r->writes) e_last = r->e_rest;
    }
    const size_t groups = (size_t)(e_last - w->group_base) + 1u;
    unsigned* counts = (unsigned*)calloc(groups, sizeof *counts);
    char* text = (char*)malloc(groups * 11u + 1u);
    if (!counts || !text) {
        free(counts); free(text);
        OPL_ERROR_F("bk2-record", "out of memory writing %zu groups", groups);
        return NULL;
    }
    for (int i = first; i < w->count; i++) {
        const bk2_rec_entry_t* r = note_at(w->start + i);
        if (!r || !r->writes) continue;
        counts[r->e - w->group_base] += entry_rows(w, i, first) - (r->writes - 1u);
        counts[r->e_rest - w->group_base] += r->writes - 1u;
    }
    size_t n = 0;
    for (size_t g = 0; g < groups; g++) n += (size_t)sprintf(text + n, "%u\n", counts[g]);
    free(counts);
    *out_len = n;
    return text;
}

/** @brief Release the recorder's copies and clear its state. */
static void rec_reset(void) {
    free(s_rec.path); free(s_rec.args);
    memset(&s_rec, 0, sizeof(s_rec));
}

/** @brief Serialise a window of the input log as Input Log.txt text.
 *
 * The [Input] header and LogKey for @p players ports, then entry_rows lines
 * per entry, each the entry's record with player 1 replaced by the pad of
 * the write the line is for, then the trailer. Every line has the same
 * width, so one probe line sizes the buffer.
 * @param w The window.
 * @param players Ports per line, 1 or 2.
 * @param out_len Receives the text's length, excluding the NUL.
 * @param out_lines Receives the number of movie rows written.
 * @return The malloc'd, NUL-terminated text; NULL when allocation fails,
 *         with the reason logged. */
static char* build_input_log(const bk2_window_t* w, int players, size_t* out_len,
                             size_t* out_lines) {
    char key_buf[1024];
    size_t key_len     = (size_t)format_log_key(key_buf, sizeof key_buf, players);
    size_t trailer_len = strlen(kInputTrailer);
    char frame_buf[192];
    openpete_input_rec_t probe;
    for (int i = 0; i < OPENPETE_PAD_SLOTS; i++) input_port_clear(&probe.port[i]);
    int  frame_line_len = format_frame_rec(frame_buf, &probe, players);
    size_t line_count = 0;
    const int first = first_written_entry(w);
    for (int i = 0; i < w->count; i++) line_count += entry_rows(w, i, first);
    size_t log_size = key_len + line_count * (size_t)frame_line_len + trailer_len;
    char* log = (char*)malloc(log_size + 1);
    if (!log) {
        OPL_ERROR_F("bk2-record", "out of memory at finalize (%zu bytes)", log_size);
        return NULL;
    }
    char* p = log;
    memcpy(p, key_buf, key_len); p += key_len;
    for (int i = 0; i < w->count; i++) {
        const openpete_input_rec_t* r = input_log_get(w->start + i);
        if (!r) break;   /* defensive; log frozen mid-recording */
        const unsigned rows = entry_rows(w, i, first);
        if (!rows) continue;
        /* The first written entry's rows for the writes before the
         * window began repeat its first write's pad. */
        const bk2_rec_entry_t* en = note_at(w->start + i);
        const unsigned pre = rows - en->writes;
        openpete_input_rec_t line = *r;
        for (unsigned k = 0; k < rows; k++) {
            const bk2_pad_write_t* pad = note_pad(en->pad0 + (k < pre ? 0u : k - pre));
            line.port[0].bits = pad->bits;
            memcpy(line.port[0].axes, pad->axes, sizeof pad->axes);
            p += format_frame_rec(p, &line, players);
        }
    }
    memcpy(p, kInputTrailer, trailer_len); p += trailer_len;
    *p = '\0';
    *out_len = (size_t)(p - log);
    *out_lines = line_count;
    return log;
}

/** @brief Build a window of the input log as a .bk2 zip on the heap; the caller holds input_log_lock.
 *
 * @param w The window.
 * @param zbuf Receives the archive, freed with mz_free.
 * @param zlen Receives its length.
 * @param lines Receives the movie rows written.
 * @return 0 on success; -1 when the window holds no write or on an
 *         allocation or archive failure, with the reason logged. */
static int window_zip(const bk2_window_t* w, void** zbuf, size_t* zlen, size_t* lines) {
    /* How many ports the movie declares. A slot that was present for any
     * recorded frame is a player for the whole movie: BizHawk fixes the port
     * count in SyncSettings, and no per-frame connect/disconnect column
     * exists to say otherwise, so a slot that comes and goes serialises its
     * absent frames as neutral. */
    int players = 1;
    for (int i = 0; i < w->count; i++) {
        const openpete_input_rec_t* r = input_log_get(w->start + i);
        if (r && r->port[1].present) { players = 2; break; }
    }

    /* Build Input Log.txt in memory: header + one line per movie row +
     * trailer; and the per-E row counts. */
    size_t log_len = 0, groups_len = 0;
    char* log = build_input_log(w, players, &log_len, lines);
    char* groups = log ? build_groups(w, &groups_len) : NULL;
    if (!groups) {
        free(log);
        return -1;
    }
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    int rc = -1;
    *zbuf = NULL;
    *zlen = 0;
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        OPL_ERROR_F("bk2-record", "cannot start the archive: %s",
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
    if (!mz_zip_writer_finalize_heap_archive(&zip, zbuf, zlen)) {
        OPL_ERROR_F("bk2-record", "finalize failed");
        goto out_close;
    }
    rc = 0;
out_close:
    mz_zip_writer_end(&zip);
    if (rc != 0) { mz_free(*zbuf); *zbuf = NULL; *zlen = 0; }
out:
    free(log);
    free(groups);
    return rc;
}

int bk2_record_window_zip(int start_idx, unsigned group_base, void** zip, size_t* len,
                          size_t* rows) {
    const bk2_window_t w = { start_idx, input_log_count() - start_idx, group_base };
    void* zbuf = NULL;
    size_t zlen = 0, lines = 0;
    if (w.count <= 0 || note_at(start_idx) == NULL || window_zip(&w, &zbuf, &zlen, &lines) != 0)
        return -1;
    *zip = malloc(zlen);
    if (!*zip) { mz_free(zbuf); return -1; }
    memcpy(*zip, zbuf, zlen);
    mz_free(zbuf);
    *len = zlen;
    if (rows) *rows = lines;
    return 0;
}

/** @brief Write the recorded window of the input log as a .bk2 zip and end recording; the caller holds input_log_lock.
 *
 * @return 0 on success or when recording is not active; -1 on an
 *         allocation or write failure, with the reason logged. */
static int rec_write_and_end(void) {
    if (!s_rec.active) return 0;

    /* The movie is the input-log window [start_idx, count), its groups
     * numbered from E 0. */
    int frame_count = input_log_count() - s_rec.start_idx;
    if (frame_count < 0) frame_count = 0;   /* defensive (log reset under us) */
    const bk2_window_t w = { s_rec.start_idx, frame_count, 0u };
    void* zbuf = NULL;
    size_t zlen = 0, line_count = 0;
    if (window_zip(&w, &zbuf, &zlen, &line_count) != 0) {
        OPL_ERROR_F("bk2-record", "%s not written", s_rec.path);
        rec_reset();
        return -1;
    }

    /* The launch-args sidecar goes first, so a movie on disk always has the
     * sidecar it was recorded under (`--bk2 <path>` reads it on replay). */
    if (s_rec.args)
        write_args_sidecar(s_rec.path, s_rec.args);

    /* The finished archive replaces the file in one rename, so a failed or
     * interrupted write leaves the previous movie whole. */
    int rc = 0;
    if (op_atomic_write_file(s_rec.path, zbuf, zlen) != 0) {
        OPL_ERROR_F("bk2-record", "cannot write %s", s_rec.path);
        rc = -1;
    } else {
        OPL_INFO_F("bk2-record", "wrote %zu frames to %s", line_count, s_rec.path);
    }
    mz_free(zbuf);
    rec_reset();
    return rc;
}

void bk2_record_discard(void) {
    input_log_lock();
    if (s_rec.active) rec_reset();
    input_log_unlock();
}

int bk2_record_finalize(void) {
    input_log_lock();
    const int rc = rec_write_and_end();
    input_log_unlock();
    return rc;
}
