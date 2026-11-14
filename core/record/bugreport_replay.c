/* core/record/bugreport_replay.c - --bug-report-replay; see
 * openpete_bugreport_replay.h. */
#include "openpete_bugreport_replay.h"

#include "bugreport_internal.h"
#include "../host/sha256.h"
#include "bk2_feed.h"
#include "game_extras.h"
#include "miniz.h"
#include "openpete.h"
#include "openpete_atomic_file.h"
#include "openpete_bugrec.h"
#include "openpete_bugreport.h"
#include "openpete_build_id.h"
#include "openpete_config.h"
#include "openpete_crash.h"
#include "openpete_fiber.h"
#include "openpete_handoff.h"
#include "openpete_log.h"
#include "openpete_memcard.h"
#include "openpete_modsave.h"
#include "openpete_paths.h"
#include "openpete_psx.h"
#include "openpete_recomp_module.h"
#include "openpete_state.h"

#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a JSON reader for manifest.json ---------------------------------- */

typedef enum { J_NULL = 0, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;

typedef struct jv {
    jtype_t     t;
    double      num;
    char*       str;    /* J_STR; the raw text of a J_NUM */
    struct jv*  kid;    /* J_ARR, J_OBJ members */
    char**      key;    /* J_OBJ keys */
    int         n, cap;
} jv_t;

typedef struct {
    const char* p;
    const char* end;
    int         depth;
} jparse_t;

static int j_value(jparse_t* P, jv_t* out);

/** @brief Skip JSON whitespace. */
static void j_ws(jparse_t* P)
{
    while (P->p < P->end && (*P->p == ' ' || *P->p == '\n' || *P->p == '\r' || *P->p == '\t')) P->p++;
}

/** @brief Append one code point to a string buffer as UTF-8.
 * @param o The buffer, at least 4 bytes free.
 * @param cp The code point.
 * @return Bytes written. */
static int j_utf8(char* o, unsigned cp)
{
    if (cp < 0x80u) { o[0] = (char)cp; return 1; }
    if (cp < 0x800u) { o[0] = (char)(0xC0u | (cp >> 6)); o[1] = (char)(0x80u | (cp & 0x3Fu)); return 2; }
    o[0] = (char)(0xE0u | (cp >> 12));
    o[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    o[2] = (char)(0x80u | (cp & 0x3Fu));
    return 3;
}

/** @brief Decode one escape sequence after its backslash.
 * @param P The parser, at the character after the backslash.
 * @param o Receives the decoded bytes.
 * @return Bytes written, or -1 on a malformed escape. */
static int j_escape(jparse_t* P, char* o)
{
    const char c = *P->p++;
    switch (c) {
    case '"': case '\\': case '/': o[0] = c; return 1;
    case 'b': o[0] = '\b'; return 1;
    case 'f': o[0] = '\f'; return 1;
    case 'n': o[0] = '\n'; return 1;
    case 'r': o[0] = '\r'; return 1;
    case 't': o[0] = '\t'; return 1;
    case 'u': {
        if (P->end - P->p < 4) return -1;
        char h[5] = {P->p[0], P->p[1], P->p[2], P->p[3], 0};
        P->p += 4;
        return j_utf8(o, (unsigned)strtoul(h, NULL, 16));
    }
    default: return -1;
    }
}

/** @brief Parse a JSON string at the opening quote into a heap copy.
 * @param P The parser.
 * @return The decoded string, or NULL when malformed. */
static char* j_string(jparse_t* P)
{
    if (P->p >= P->end || *P->p != '"') return NULL;
    P->p++;
    const char* s = P->p;
    while (P->p < P->end && *P->p != '"') P->p += (*P->p == '\\') ? 2 : 1;
    if (P->p >= P->end) return NULL;
    char* out = (char*)malloc((size_t)(P->p - s) * 3 + 1);
    if (!out) return NULL;
    const char* stop = P->p;
    P->p = s;
    size_t n = 0;
    while (P->p < stop) {
        if (*P->p != '\\') { out[n++] = *P->p++; continue; }
        P->p++;
        const int k = j_escape(P, out + n);
        if (k < 0) { free(out); return NULL; }
        n += (size_t)k;
    }
    out[n] = '\0';
    P->p = stop + 1;
    return out;
}

/** @brief Append a member slot to an array or object.
 * @param v The container.
 * @return The new slot, zeroed, or NULL on allocation failure. */
static jv_t* j_push(jv_t* v)
{
    if (v->n == v->cap) {
        const int cap = v->cap ? v->cap * 2 : 8;
        jv_t* k = (jv_t*)realloc(v->kid, (size_t)cap * sizeof *k);
        if (!k) return NULL;
        v->kid = k;
        if (v->t == J_OBJ) {
            char** ks = (char**)realloc(v->key, (size_t)cap * sizeof *ks);
            if (!ks) return NULL;
            v->key = ks;
        }
        v->cap = cap;
    }
    memset(&v->kid[v->n], 0, sizeof v->kid[0]);
    if (v->t == J_OBJ) v->key[v->n] = NULL;
    return &v->kid[v->n++];
}

/** @brief Parse an array or object body after its opening bracket.
 * @param P The parser.
 * @param v The container, its type set.
 * @param close The closing bracket.
 * @return 0, or -1 when malformed. */
static int j_container(jparse_t* P, jv_t* v, char close)
{
    j_ws(P);
    if (P->p < P->end && *P->p == close) { P->p++; return 0; }
    for (;;) {
        char* key = NULL;
        if (v->t == J_OBJ) {
            j_ws(P);
            key = j_string(P);
            j_ws(P);
            if (!key || P->p >= P->end || *P->p++ != ':') { free(key); return -1; }
        }
        jv_t* slot = j_push(v);
        if (!slot) { free(key); return -1; }
        if (v->t == J_OBJ) v->key[v->n - 1] = key;
        if (j_value(P, slot) != 0) return -1;
        j_ws(P);
        if (P->p >= P->end) return -1;
        const char c = *P->p++;
        if (c == close) return 0;
        if (c != ',') return -1;
    }
}

/** @brief Parse a JSON literal or number.
 * @param P The parser.
 * @param out Receives the value.
 * @return 0, or -1 when malformed. */
static int j_scalar(jparse_t* P, jv_t* out)
{
    static const struct { const char* w; jtype_t t; double v; } lit[] = {
        {"null", J_NULL, 0}, {"true", J_BOOL, 1}, {"false", J_BOOL, 0}};
    for (size_t i = 0; i < sizeof lit / sizeof lit[0]; i++) {
        const size_t n = strlen(lit[i].w);
        if ((size_t)(P->end - P->p) >= n && !memcmp(P->p, lit[i].w, n)) {
            out->t = lit[i].t;
            out->num = lit[i].v;
            P->p += n;
            return 0;
        }
    }
    const char* s = P->p;
    while (P->p < P->end && strchr("+-0123456789.eE", *P->p)) P->p++;
    if (P->p == s) return -1;
    out->t = J_NUM;
    out->str = (char*)malloc((size_t)(P->p - s) + 1);
    if (!out->str) return -1;
    memcpy(out->str, s, (size_t)(P->p - s));
    out->str[P->p - s] = '\0';
    out->num = strtod(out->str, NULL);
    return 0;
}

/** @brief Parse one JSON value.
 * @param P The parser.
 * @param out Receives the value.
 * @return 0, or -1 when malformed or nested past 64 levels. */
static int j_value(jparse_t* P, jv_t* out)
{
    j_ws(P);
    if (P->p >= P->end || ++P->depth > 64) return -1;
    int rc;
    if (*P->p == '{' || *P->p == '[') {
        const char open = *P->p++;
        out->t = open == '{' ? J_OBJ : J_ARR;
        rc = j_container(P, out, open == '{' ? '}' : ']');
    } else if (*P->p == '"') {
        out->t = J_STR;
        out->str = j_string(P);
        rc = out->str ? 0 : -1;
    } else {
        rc = j_scalar(P, out);
    }
    P->depth--;
    return rc;
}

/** @brief Return an object's member by key, or NULL. */
static const jv_t* j_get(const jv_t* o, const char* key)
{
    if (!o || o->t != J_OBJ) return NULL;
    for (int i = 0; i < o->n; i++)
        if (o->key[i] && !strcmp(o->key[i], key)) return &o->kid[i];
    return NULL;
}

/** @brief Return an object's string member, or "" when absent or not a string. */
static const char* j_s(const jv_t* o, const char* key)
{
    const jv_t* v = j_get(o, key);
    return v && v->t == J_STR && v->str ? v->str : "";
}

/** @brief Return an object's numeric member (or a "0x" hex string) as an unsigned, 0 when absent. */
static unsigned j_u(const jv_t* o, const char* key)
{
    const jv_t* v = j_get(o, key);
    if (!v) return 0u;
    if (v->t == J_STR && v->str) return (unsigned)strtoul(v->str, NULL, 0);
    return v->t == J_NUM ? (unsigned)v->num : 0u;
}

/* ---- the replay's state ------------------------------------------------ */

#define RP_PATH 1400
#define RP_LABELS 96
#define RP_REASON 600

/* One region column of hashes.csv against this build's table. */
typedef struct {
    int col;        /* the bundle's column (0 = total), -1 when the bundle has none */
} rp_col_t;

static struct {
    int      on;              /* a replay was prepared */
    char     bundle[RP_PATH];
    char     scratch[RP_PATH];
    char     sandbox[RP_PATH];
    jv_t     man;             /* manifest.json */
    /* What the manifest names. */
    char     game[32];
    unsigned anchor_frame, anchor_consume, press_frame, press_feed_key;
    /* A fatal-exit bundle (manifest press null) holds no press.state: its
     * end frame is hashes.csv's last row and the section compare is skipped. */
    int      fatal;
    char     trigger[32];
    char     taint[256];
    char     labels[RP_LABELS];
    /* hashes.csv: the header's region names and one row per frame. */
    char**   cols;
    int      ncols;           /* region columns after frame, vbl, total */
    uint64_t* rows;           /* (2 + 1 + ncols) per row: frame, vbl, total, regions */
    size_t   nrows;
    rp_col_t* map;            /* per region of this build */
    size_t   nmap;
    /* The run. */
    unsigned beyond;          /* --bug-report-replay-beyond: ticks past the last recorded one */
    unsigned end_frame;       /* the frame the run stops at: the end frame plus rp_beyond() */
    int      loaded;          /* the anchor load committed */
    int      done;            /* the end-state compare ran */
    int      finished;        /* the shutdown was requested at end_frame */
    int      refused;
    char     reason[RP_REASON];
    uint64_t rows_compared;
    uint64_t rows_matched;    /* rows compared equal before the first difference */
    /* A fatal exit of the replay itself (the hang watchdog, the CdSync spin
     * guard): its reason, the tick it did not complete, the hang trip. */
    const char* run_fatal;
    unsigned run_fatal_tick;
    const char* hang_kind;
    uint32_t hang_at;
    unsigned div_frame;       /* 0 = no row differed */
    int      div_vbl;
    char     div_regions[256];
    int      sections_equal;
    char     section[160];    /* the first differing section and offset, "" when equal */
    char     section_all[400];
    char     vram_note[160];  /* a VRAM difference, reported and not counted */
    unsigned load_site_waits, load_cd_waits;
    /* The tester's launch scripts (k_launch): passed when the anchor precedes
     * every one of them, dropped otherwise. */
    int      launch_keep;
    char     launch_why[160];
    char     launch_flags[400];   /* the launch flags of the tester's argv, with values */
} R;

/** @brief Record a refusal reason (the first one wins) and print it on stderr.
 * @param fmt printf format. */
static void rp_refuse(const char* fmt, ...)
{
    if (!R.refused) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(R.reason, sizeof R.reason, fmt, ap);
        va_end(ap);
        R.refused = 1;
    }
    fprintf(stderr, "bug-report-replay: REFUSED %s\n", R.reason);
}

/** @brief Append a label to the label list once. */
static void rp_label(const char* l)
{
    if (strstr(R.labels, l)) return;
    const size_t n = strlen(R.labels);
    snprintf(R.labels + n, sizeof R.labels - n, "%s%s", n ? "," : "", l);
}

/* ---- files ------------------------------------------------------------- */

/** @brief Join two path parts with '/'.
 *
 * A result that does not fit in cap leaves out empty, so a truncated path
 * is never opened. */
static void rp_join(char* out, size_t cap, const char* a, const char* b)
{
    const int n = snprintf(out, cap, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= cap) out[0] = '\0';
}

/** @brief Create a directory and its missing parents.
 * @return 0, or -1 when one cannot be created. */
static int rp_mkdirs(const char* path)
{
    char tmp[RP_PATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        const char c = *p;
        *p = '\0';
        if (p[-1] != ':') (void)SDL_CreateDirectory(tmp);
        *p = c;
    }
    SDL_PathInfo info;
    return (SDL_CreateDirectory(tmp) || (SDL_GetPathInfo(tmp, &info) && info.type == SDL_PATHTYPE_DIRECTORY))
               ? 0 : -1;
}

/** @brief Write a file, creating its parent directories.
 * @return 0, or -1 on failure. */
static int rp_write(const char* path, const void* data, size_t n)
{
    char dir[RP_PATH];
    snprintf(dir, sizeof dir, "%s", path);
    char* sl = strrchr(dir, '/');
    if (sl) { *sl = '\0'; if (rp_mkdirs(dir) != 0) return -1; }
    return op_atomic_write_file(path, data, n);
}

/** @brief Report whether a path is an existing directory. */
static int rp_is_dir(const char* path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

/** @brief Report whether a path is an existing file. */
static int rp_is_file(const char* path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

/** @brief Remove one directory entry, recursing into directories (SDL_EnumerateDirectory callback). */
static SDL_EnumerationResult SDLCALL rp_rm_cb(void* ud, const char* dirname, const char* fname)
{
    (void)ud;
    char p[RP_PATH];
    snprintf(p, sizeof p, "%s%s", dirname, fname);
    if (rp_is_dir(p)) (void)SDL_EnumerateDirectory(p, rp_rm_cb, NULL);
    (void)SDL_RemovePath(p);
    return SDL_ENUM_CONTINUE;
}

/* The file marking a directory as one this replay made, so only such a
 * directory is emptied. */
#define RP_MARKER ".openpete-bugreport-replay"

/** @brief Empty the scratch directory (only one carrying the marker), or create it.
 * @return 0, or -1 when it exists without the marker or cannot be made. */
static int rp_scratch_reset(const char* dir)
{
    char marker[RP_PATH];
    rp_join(marker, sizeof marker, dir, RP_MARKER);
    if (rp_is_dir(dir)) {
        if (!rp_is_file(marker)) {
            rp_refuse("scratch directory %s exists and was not made by a replay; remove it or set "
                      "OPENPETE_BUGREPORT_REPLAY_DIR", dir);
            return -1;
        }
        (void)SDL_EnumerateDirectory(dir, rp_rm_cb, NULL);
    }
    if (rp_mkdirs(dir) != 0 || rp_write(marker, "", 0) != 0) {
        rp_refuse("cannot create the scratch directory %s", dir);
        return -1;
    }
    return 0;
}

/* ---- the bundle -------------------------------------------------------- */

typedef struct {
    int             is_zip;
    mz_zip_archive  z;
    const char*     path;
} rp_src_t;

/** @brief Read one member of the bundle into a heap buffer (NUL-terminated).
 * @return The bytes, or NULL when absent. */
static uint8_t* rp_src_read(rp_src_t* s, const char* name, size_t* len)
{
    if (s->is_zip) {
        size_t n = 0;
        void* d = mz_zip_reader_extract_file_to_heap(&s->z, name, &n, 0);
        if (!d) return NULL;
        uint8_t* out = (uint8_t*)realloc(d, n + 1);
        if (!out) { mz_free(d); return NULL; }
        out[n] = '\0';
        *len = n;
        return out;
    }
    char p[RP_PATH];
    rp_join(p, sizeof p, s->path, name);
    uint8_t* d = NULL;
    return br_read_file(p, &d, len) == 0 ? d : NULL;
}

/** @brief Report whether a member name is safe to write under the scratch directory (relative, no `..`). */
static int rp_name_safe(const char* n)
{
    if (!n[0] || n[0] == '/' || n[0] == '\\' || strchr(n, ':')) return 0;
    for (const char* p = n; *p; p++)
        if (p[0] == '.' && p[1] == '.' && (p == n || p[-1] == '/' || p[-1] == '\\')) return 0;
    return 1;
}

/** @brief Verify one member's SHA-256 against the manifest and copy it into the scratch directory.
 * @return 0, or -1 (refused). */
static int rp_copy_member(rp_src_t* s, const jv_t* m)
{
    const char* name = j_s(m, "name");
    if (!rp_name_safe(name)) { rp_refuse("member name %s is not a relative path", name); return -1; }
    size_t n = 0;
    uint8_t* d = rp_src_read(s, name, &n);
    if (!d) { rp_refuse("member %s named by manifest.json is missing", name); return -1; }
    uint8_t dig[32];
    char hex[65];
    openpete_sha256_ctx c;
    openpete_sha256_init(&c);
    openpete_sha256_update(&c, d, n);
    openpete_sha256_final(&c, dig);
    openpete_sha256_hex(dig, hex);
    int rc = 0;
    if (strcmp(hex, j_s(m, "sha256")) != 0) {
        rp_refuse("member %s: SHA-256 %s, manifest.json names %s", name, hex, j_s(m, "sha256"));
        rc = -1;
    } else {
        char out[RP_PATH];
        rp_join(out, sizeof out, R.scratch, name);
        if (rp_write(out, d, n) != 0) { rp_refuse("cannot write %s", out); rc = -1; }
    }
    free(d);
    return rc;
}

/** @brief Open the bundle, parse manifest.json and copy every verified member into the scratch directory.
 * @return 0, or -1 (refused). */
static int rp_unpack(void)
{
    rp_src_t s = {0};
    s.path = R.bundle;
    if (!rp_is_dir(R.bundle)) {
        if (!mz_zip_reader_init_file(&s.z, R.bundle, 0)) {
            rp_refuse("%s is neither a bundle directory nor a readable zip", R.bundle);
            return -1;
        }
        s.is_zip = 1;
    }
    size_t n = 0;
    uint8_t* mj = rp_src_read(&s, "manifest.json", &n);
    int rc = 0;
    jparse_t P = {(const char*)mj, (const char*)mj + n, 0};
    if (!mj || j_value(&P, &R.man) != 0 || R.man.t != J_OBJ) {
        rp_refuse("%s: manifest.json is missing or does not parse", R.bundle);
        rc = -1;
    }
    if (!rc && (int)j_u(&R.man, "schema") > OPENPETE_BUGREPORT_SCHEMA) {
        rp_refuse("manifest schema %u is newer than this build's %d", j_u(&R.man, "schema"),
                  OPENPETE_BUGREPORT_SCHEMA);
        rc = -1;
    }
    char mp[RP_PATH];
    rp_join(mp, sizeof mp, R.scratch, "manifest.json");
    if (!rc && rp_write(mp, mj, n) != 0) { rp_refuse("cannot write %s", mp); rc = -1; }
    free(mj);
    const jv_t* mem = j_get(&R.man, "members");
    for (int i = 0; !rc && mem && mem->t == J_ARR && i < mem->n; i++) rc = rp_copy_member(&s, &mem->kid[i]);
    if (s.is_zip) mz_zip_reader_end(&s.z);
    return rc;
}

/* ---- the command line -------------------------------------------------- */

typedef struct {
    char** v;
    int    n, cap;
} rp_argv_t;

/** @brief Append a copy of an argument.
 * @return 0, or -1 on allocation failure. */
static int rp_arg(rp_argv_t* a, const char* s)
{
    if (a->n + 2 > a->cap) {
        const int cap = a->cap ? a->cap * 2 : 64;
        char** v = (char**)realloc(a->v, (size_t)cap * sizeof *v);
        if (!v) return -1;
        a->v = v;
        a->cap = cap;
    }
    a->v[a->n] = SDL_strdup(s);
    if (!a->v[a->n]) return -1;
    a->v[++a->n] = NULL;
    return 0;
}

/* Flags of the tester's argv the replay drops, with the values they take
 * (-1: an optional 0|1|on|off): the game and disc (the developer's --disc
 * decides, the admission checks it), the cards, mods root and config
 * (replaced by the bundle's), the boot route and movie (the replay's own),
 * the run's bounds, every output file, and every flag that only modifies an
 * output (the screenshot and record modifiers): the developer's own
 * capture flags decide the replay's outputs, and a modifier left without
 * its output can select a path no consumer brings up. Display flags
 * (--renderer, --window, --fullscreen, --vsync, --gpu, --ssaa, --aspect)
 * pass: --headless leaves the window ones inert, and the renderer ones
 * reach the developer's captures as they reached the tester's display. */
static const struct { const char* flag; int values; } k_drop[] = {
    {"--disc", 1}, {"--card1", 1}, {"--card2", 1}, {"--card-dir", 1}, {"--no-card", 0},
    {"--no-card1", 0}, {"--no-card2", 0}, {"--mods-dir", 1}, {"--config", 1}, {"--no-config", 0},
    {"--bk2", 1}, {"--bk2-frame-offset", 1}, {"--bk2-stop-on-end", 0}, {"--resume-handoff", 1},
    {"--bug-report-replay", 1}, {"--frame-limit", 1}, {"--max-runtime-seconds", 1},
    {"--screenshot-on-frame", 1}, {"--screenshot-on-tick", 1},
    {"--screenshot-intermediate-on-frame", 1}, {"--screenshot-output", 1},
    {"--screenshot-presents-start", 1}, {"--screenshot-presents-count", 1},
    {"--screenshot-native-only", 0}, {"--screenshot-overlays", -1},
    {"--screenshot-freeze-backdrop", 0},
    {"--ram-dump-on-frame", 1}, {"--ram-dump-output", 1}, {"--state-save-on-frame", 1},
    {"--state-save-output", 1}, {"--state-load-on-frame", 1}, {"--state-load-input", 1},
    {"--record", 1}, {"--record-encoder", 1}, {"--record-crf", 1}, {"--record-gop", 1},
    {"--record-pixfmt", 1}, {"--record-audio-encoder", 1}, {"--record-frames", 1},
    {"--record-cfr", 1}, {"--record-fps-overlay", 1}, {"--no-record-audio", 0},
    {"--log-file", 1}, {"--exe-path", 1}, {"--recomp-module", 1}, {"--position-log", 1},
    {"--gameplay-hash-log", 1}, {"--frame-time-log", 1}, {"--pace-log", 1}, {"--profile-out", 1},
    {"--psxtrace-verify", 1}, {"--psxtrace-anchor-pc", 1}, {"--psxtrace-anchor-hit", 1},
    {"--psxtrace-keep-going", 0}, {"--freecam-path", 1}, {"--freecam-pose", 1},
    {"--halt-at-pc", 1}, {"--halt-cpu-dump-output", 1}, {"--halt-hit", 1},
    {"--texpack-dir", 1}, {"--sfxpack-dir", 1}, {"--controller-db", 1}, {"--ingest", 1},
    {"--vibration", 1}, {"--headless", 0}, {"--game", 1},
};

/* Launch scripts of the tester's argv: flags whose host code writes guest
 * RAM once, at a point of the session it waits for (the title's induction
 * window, a live level, a feed key), and keeps its progress in host state the
 * anchor does not hold. Replayed from an anchor that precedes them they run
 * as they ran for the tester; replayed from a later anchor they would run a
 * second time, so they are dropped there. Flags that act on every tick
 * alike (--load-delay) are not launch scripts and always pass. */
static const struct { const char* flag; int values; } k_launch[] = {
    {"--skip-to-level", 1}, {"--skip-to-area", 1}, {"--skip-to-cutscene", 1},
    {"--player-pose", 1}, {"--cheats", 1},
};

/** @brief Return how many tokens a launch-script flag spans (flag and values), or 0 for another token.
 * @param a The token. */
static int rp_launch_span(const char* a)
{
    for (size_t i = 0; i < sizeof k_launch / sizeof k_launch[0]; i++)
        if (!strcmp(a, k_launch[i].flag)) return 1 + k_launch[i].values;
    return 0;
}

/** @brief Return how many tokens a dropped flag spans (flag and values), or 0 when the flag is kept.
 * @param a The token.
 * @param next The token after it, or NULL. */
static int rp_drop_span(const char* a, const char* next)
{
    if (!strcmp(a, "--bk2-record")) return (next && next[0] != '-') ? 2 : 1;
    for (size_t i = 0; i < sizeof k_drop / sizeof k_drop[0]; i++) {
        if (strcmp(a, k_drop[i].flag) != 0) continue;
        if (k_drop[i].values >= 0) return 1 + k_drop[i].values;
        const bool val = next && (!strcmp(next, "0") || !strcmp(next, "1") ||
                                  !strcmp(next, "on") || !strcmp(next, "off"));
        return val ? 2 : 1;
    }
    return 0;
}

/** @brief Remap a token whose path starts with `~` (whole or after a `key=`) into <scratch>/home.
 * @param s The token.
 * @param out Receives the remapped token.
 * @param cap Capacity of @p out; a remapped token that does not fit is kept unmapped. */
static void rp_remap(const char* s, char* out, size_t cap)
{
    const char* eq = strchr(s, '=');
    const char* v = (s[0] == '~') ? s : (eq && eq[1] == '~' ? eq + 1 : NULL);
    if (!v || (v[1] && v[1] != '/' && v[1] != '\\')) { snprintf(out, cap, "%s", s); return; }
    const int n = snprintf(out, cap, "%.*s%s/home%s", (int)(v - s), s, R.scratch, v + 1);
    if (n < 0 || (size_t)n >= cap) snprintf(out, cap, "%s", s);   /* does not fit: the token unmapped */
}

/** @brief Append the tester's arguments after argv[0] from the manifest, without the dropped flags and with `~` remapped.
 *
 * The launch-script flags (k_launch) are listed in R.launch_flags and
 * appended only under R.launch_keep.
 * @return 0, or -1 on allocation failure. */
static int rp_tester_args(rp_argv_t* a)
{
    const jv_t* av = j_get(&R.man, "argv");
    for (int i = 1; av && av->t == J_ARR && i < av->n; i++) {
        const char* t = av->kid[i].str ? av->kid[i].str : "";
        const char* nx = (i + 1 < av->n && av->kid[i + 1].str) ? av->kid[i + 1].str : NULL;
        const int span = rp_drop_span(t, nx);
        if (span) { i += span - 1; continue; }
        const int ls = rp_launch_span(t);
        for (int k = 0; k < ls && i + k < av->n; k++) {
            const size_t u = strlen(R.launch_flags);
            snprintf(R.launch_flags + u, sizeof R.launch_flags - u, "%s%s", u ? " " : "",
                     av->kid[i + k].str ? av->kid[i + k].str : "");
        }
        if (ls && !R.launch_keep) { i += ls - 1; continue; }
        char m[RP_PATH];
        rp_remap(t, m, sizeof m);
        if (rp_arg(a, m) != 0) return -1;
    }
    return 0;
}

/** @brief Copy the anchor's card of one port into <scratch>/cards and name it on the command line, or pass --no-cardN.
 * @param a The argv.
 * @param port 1 or 2.
 * @return 0, or -1 on failure (refused). */
static int rp_card_args(rp_argv_t* a, int port)
{
    char src[RP_PATH], dst[RP_PATH], name[32], flag[16];
    snprintf(name, sizeof name, "card%d.mcd", port);
    rp_join(src, sizeof src, R.scratch, name);
    if (!rp_is_file(src)) {
        snprintf(flag, sizeof flag, "--no-card%d", port);
        return rp_arg(a, flag);
    }
    for (int k = 0; k < 2; k++) {
        char s2[RP_PATH], d2[RP_PATH], leaf[64];
        snprintf(leaf, sizeof leaf, "cards/card%d.mcd%s", port, k ? ".modsave" : "");
        const int sn = snprintf(s2, sizeof s2, "%s%s", src, k ? ".modsave" : "");
        if (sn < 0 || (size_t)sn >= sizeof s2) {
            if (k == 0) { rp_refuse("card path too long: %s", src); return -1; }
            continue;   /* the sidecar's path does not fit: no sidecar */
        }
        rp_join(d2, sizeof d2, R.scratch, leaf);
        if (k == 0) snprintf(dst, sizeof dst, "%s", d2);
        size_t n = 0;
        uint8_t* d = NULL;
        if (br_read_file(s2, &d, &n) != 0) continue;
        const int rc = rp_write(d2, d, n);
        free(d);
        if (rc != 0) { rp_refuse("cannot write %s", d2); return -1; }
    }
    snprintf(flag, sizeof flag, "--card%d", port);
    return (rp_arg(a, flag) || rp_arg(a, dst)) ? -1 : 0;
}

/** @brief Append the replay's own flags: headless, cards, mods root, config, anchor and segment.
 * @return 0, or -1 on failure. */
static int rp_replay_args(rp_argv_t* a)
{
    char p[RP_PATH], src[RP_PATH], num[32];
    /* A windowed replay reaches the live present, which a headless run never
     * builds: occlusion readback and eager production differ there. */
    int rc = rp_arg(a, "--vibration") | rp_arg(a, "0");
    if (!getenv("OPENPETE_BUGREPORT_REPLAY_WINDOWED")) rc |= rp_arg(a, "--headless");
    /* No host input reaches the game: before the anchor load the pad is
     * neutral, and past the movie's last row (--bug-report-replay-beyond)
     * input_bk2_stamp stamps no row, so the pad stays neutral there too. */
    rc |= rp_arg(a, "--input-lock") | rp_arg(a, "1");
    rc |= rp_card_args(a, 1) | rp_card_args(a, 2);
    rp_join(p, sizeof p, R.scratch, "mods");
    if (rp_mkdirs(p) != 0) rc = -1;
    rc |= rp_arg(a, "--mods-dir") | rp_arg(a, p);
    char cfg[2][RP_PATH];
    rp_join(cfg[0], sizeof cfg[0], R.scratch, "config-files/exe-dir/openpete.toml");
    rp_join(cfg[1], sizeof cfg[1], R.scratch, "config-files/user/openpete.toml");
    if (rp_is_file(cfg[0]) || rp_is_file(cfg[1])) openpete_config_tier_files(cfg[0], cfg[1]);
    else rc |= rp_arg(a, "--no-config");
    /* The handoff resume deletes its file once loaded: load a copy. */
    rp_join(src, sizeof src, R.scratch, "anchor.state");
    rp_join(p, sizeof p, R.scratch, "anchor-load.state");
    size_t n = 0;
    uint8_t* d = NULL;
    if (br_read_file(src, &d, &n) != 0 || rp_write(p, d, n) != 0) rc = -1;
    free(d);
    rc |= rp_arg(a, "--resume-handoff") | rp_arg(a, p);
    rp_join(p, sizeof p, R.scratch, "segment.bk2");
    snprintf(num, sizeof num, "%u", R.anchor_consume);
    rc |= rp_arg(a, "--bk2") | rp_arg(a, p) | rp_arg(a, "--bk2-frame-offset") | rp_arg(a, num);
    return rc ? -1 : 0;
}

/** @brief Hash a file with SHA-256.
 * @param path The file.
 * @param hex Receives 64 hex digits and a NUL, "" when unreadable. */
static void rp_sha_file(const char* path, char hex[65])
{
    hex[0] = '\0';
    size_t n = 0;
    uint8_t* d = NULL;
    if (br_read_file(path, &d, &n) != 0) return;
    uint8_t dig[32];
    openpete_sha256_ctx c;
    openpete_sha256_init(&c);
    openpete_sha256_update(&c, d, n);
    openpete_sha256_final(&c, dig);
    openpete_sha256_hex(dig, hex);
    free(d);
}

/** @brief Name the bundle's recomp module on the command line when this executable is the tester's own (the primary route).
 *
 * The same executable SHA-256 means the same artifact; the tester's
 * module, compiled at first boot for the tester's CPU, is loaded as is
 * rather than rebuilt here, where a native march would give another
 * module.
 * @return 0, or -1 on allocation failure. */
static int rp_primary_args(rp_argv_t* a)
{
    char exe[RP_PATH], sha[65];
    if (openpete_exe_path(exe, sizeof exe) != 0) return 0;
    rp_sha_file(exe, sha);
    if (!sha[0] || strcmp(sha, j_s(&R.man, "exe_sha256")) != 0) return 0;
    rp_label("primary-route");
    const jv_t* mem = j_get(&R.man, "members");
    for (int i = 0; mem && mem->t == J_ARR && i < mem->n; i++) {
        const char* name = j_s(&mem->kid[i], "name");
        if (strncmp(name, "module/", 7) != 0) continue;
        char p[RP_PATH];
        rp_join(p, sizeof p, R.scratch, name);
        return (rp_arg(a, "--recomp-module") || rp_arg(a, p)) ? -1 : 0;
    }
    return 0;
}

/** @brief Append the developer's own arguments, every one after argv[0] but the --bug-report-replay pair, and report whether they name a disc.
 * @return 1 when --disc was given, 0 when not, -1 on allocation failure. */
static int rp_dev_args(rp_argv_t* a, int argc, char** argv)
{
    int disc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bug-report-replay")) { i++; continue; }
        if (!strcmp(argv[i], "--disc")) disc = 1;
        if (rp_arg(a, argv[i]) != 0) return -1;
    }
    return disc;
}

/* ---- the environment --------------------------------------------------- */

/* env.txt variables the replay does not set: the harness triggers that
 * would write a bundle, set a knob (knobs.txt carries the change and its
 * tick), take extra anchors, dump the ring, exec a handoff or end the run
 * early, the injected host input (the movie carries the
 * pad it produced), the config file the tiers replace, and the tester's
 * sandbox root (the replay sets its own). */
static const char* const k_env_drop[] = {
    "OPENPETE_BUGREPORT_AT", "OPENPETE_BUGREC_ANCHOR_AT", "OPENPETE_BUGREC_DUMP_AT",
    "OPENPETE_BUGREC_DUMP_DIR", "OPENPETE_HANDOFF_AT", "OPENPETE_FRAME_LIMIT", "OPENPETE_CONFIG",
    "OPENPETE_LOG_FILE", "OPENPETE_MOUSE_INJECT", "OPENPETE_MOUSE_INJECT_RAW",
    "OPENPETE_DEVUI_INJECT_MOUSE", "OPENPETE_BUGREPORT_KNOB_AT", "OPENPETE_SANDBOX",
};

/** @brief Report whether a variable is one of k_env_drop's harness triggers. */
static int rp_env_trigger(const char* name)
{
    for (size_t i = 0; i < sizeof k_env_drop / sizeof k_env_drop[0]; i++)
        if (!strcmp(name, k_env_drop[i])) return 1;
    return 0;
}

/** @brief Report whether an env.txt variable is set by the replay.
 *
 * OPENPETE_PLAYER_POSE is --player-pose's spelling and follows the launch
 * scripts' decision. */
static int rp_env_kept(const char* name)
{
    if (strncmp(name, "OPENPETE_", 9) != 0 || !strncmp(name, "OPENPETE_BUGREPORT_REPLAY_", 26)) return 0;
    if (!strcmp(name, "OPENPETE_PLAYER_POSE") && !R.launch_keep) return 0;
    return !rp_env_trigger(name);
}

/* Variables OPENPETE_BUGREPORT_REPLAY_ENV refuses: their consumers write
 * guest RAM, feed the pad or the CD timing, or load code that does, so the
 * replay would no longer reproduce the tester's run. A trailing '*' matches
 * a prefix. */
static const struct { const char* name; const char* why; } k_env_gameplay[] = {
    {"OPENPETE_PLAYER_POSE", "places the player"},
    {"OPENPETE_FREECAM*", "writes the camera"},
    {"OPENPETE_CAMLOCK*", "writes the camera"},
    {"OPENPETE_CUSTOM_WARP", "warps to a level"},
    {"OPENPETE_SKIP_*", "times a level skip's release"},
    {"OPENPETE_LOAD_DELAY", "sets the CD load timing"},
    {"OPENPETE_INPUT_*", "synthesizes pad input"},
    {"OPENPETE_SYNTH_PADS", "synthesizes pad input"},
    {"OPENPETE_MOUSE*", "injects mouse input"},
    {"OPENPETE_DEVUI_INJECT_MOUSE", "injects mouse input"},
    {"OPENPETE_REWIND_*", "moves the timeline"},
    {"OPENPETE_MODS", "loads mods"},
    {"OPENPETE_MODS_DIR", "loads mods"},
    {"OPENPETE_MOD_CHANNEL_MASK", "changes what mods may write"},
    {"OPENPETE_MOD_HOT_RELOAD", "reloads mods"},
    {"OPENPETE_GTE_HASH_POKE", "injects a fault"},
    {"OPENPETE_PSXTRACE_POKE", "injects a fault"},
    {"OPENPETE_S3_SCAN_POKE", "injects a fault"},
    {"OPENPETE_MODSAVE_FAULT_*", "injects a card fault"},
};

/** @brief Return why a variable is refused as gameplay-affecting, or NULL when it is not on k_env_gameplay.
 * @param name The variable. */
static const char* rp_env_gameplay(const char* name)
{
    for (size_t i = 0; i < sizeof k_env_gameplay / sizeof k_env_gameplay[0]; i++) {
        const char* g = k_env_gameplay[i].name;
        const size_t n = strlen(g);
        if (g[n - 1] == '*' ? !strncmp(name, g, n - 1) : !strcmp(name, g)) return k_env_gameplay[i].why;
    }
    return NULL;
}

#define RP_ENV_ALLOW 32

/* OPENPETE_BUGREPORT_REPLAY_ENV's entries, read before the environment is cleared. */
static struct { char name[96]; char value[RP_PATH]; } s_allow[RP_ENV_ALLOW];
static int s_allow_n;

/** @brief Read OPENPETE_BUGREPORT_REPLAY_ENV: comma-separated `NAME` (the developer's value) or `NAME=VALUE` entries.
 *
 * Refuses an entry that is not OPENPETE_*, is the replay's own, is a harness
 * trigger (k_env_drop), is on k_env_gameplay, or names an unset variable.
 * @return 0, or -1 (refused). */
static int rp_env_allow_read(void)
{
    const char* e = getenv("OPENPETE_BUGREPORT_REPLAY_ENV");
    char list[2048];
    snprintf(list, sizeof list, "%s", e ? e : "");
    s_allow_n = 0;
    for (char* tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        if (!*tok) continue;
        char* eq = strchr(tok, '=');
        if (eq) *eq = '\0';
        const char* v = eq ? eq + 1 : getenv(tok);
        const char* why = rp_env_gameplay(tok);
        if (strncmp(tok, "OPENPETE_", 9) != 0 || !strncmp(tok, "OPENPETE_BUGREPORT_REPLAY_", 26))
            why = "is not an OPENPETE_* instrument variable";
        else if (rp_env_trigger(tok)) why = "is a harness trigger the replay drops";
        else if (!v) why = "is not set";
        else if (s_allow_n == RP_ENV_ALLOW) why = "is past the list's 32 entries";
        if (why) {
            rp_refuse("OPENPETE_BUGREPORT_REPLAY_ENV: %s %s", tok, why);
            return -1;
        }
        snprintf(s_allow[s_allow_n].name, sizeof s_allow[s_allow_n].name, "%s", tok);
        snprintf(s_allow[s_allow_n].value, sizeof s_allow[s_allow_n].value, "%s", v);
        s_allow_n++;
    }
    return 0;
}

/** @brief Set or unset a variable of the C runtime's environment, the one getenv reads.
 * @param name The variable.
 * @param v Its value, or NULL to unset it. */
static void rp_setenv(const char* name, const char* v)
{
#ifdef _WIN32
    (void)_putenv_s(name, v ? v : "");
#else
    if (v) (void)setenv(name, v, 1);
    else (void)unsetenv(name);
#endif
}

/** @brief Collect the names of the OPENPETE_* variables set now, the replay's own excepted.
 * @param n Receives the count.
 * @return The names (heap; free each and the array). */
static char** rp_env_names(int* n)
{
#ifdef _WIN32
    char** env = _environ;
#else
    extern char** environ;
    char** env = environ;
#endif
    char** out = NULL;
    *n = 0;
    for (int i = 0; env && env[i]; i++) {
        const char* eq = strchr(env[i], '=');
        if (!eq || strncmp(env[i], "OPENPETE_", 9) != 0 || !strncmp(env[i], "OPENPETE_BUGREPORT_REPLAY_", 26))
            continue;
        char** o = (char**)realloc(out, (size_t)(*n + 1) * sizeof *o);
        if (!o) break;
        out = o;
        out[*n] = (char*)malloc((size_t)(eq - env[i]) + 1);
        if (!out[*n]) break;
        memcpy(out[*n], env[i], (size_t)(eq - env[i]));
        out[(*n)++][eq - env[i]] = '\0';
    }
    return out;
}

/** @brief Report whether OPENPETE_BUGREPORT_REPLAY_ENV names a variable. */
static int rp_env_allowed(const char* name)
{
    for (int i = 0; i < s_allow_n; i++)
        if (!strcmp(s_allow[i].name, name)) return 1;
    return 0;
}

/** @brief Unset this process's OPENPETE_* variables, the replay's own excepted, and print one line naming those OPENPETE_BUGREPORT_REPLAY_ENV does not forward.
 *
 * A tester's environment is not replayed from the developer's shell: the
 * replay runs on env.txt, and only OPENPETE_BUGREPORT_REPLAY_ENV forwards an
 * instrument variable. The line names what a developer set and lost. */
static void rp_env_clear(void)
{
    int nn = 0;
    char** names = rp_env_names(&nn);
    char lost[1024] = "";
    size_t k = 0;
    int nlost = 0;
    for (int i = 0; i < nn; i++) {
        if (!rp_env_allowed(names[i]) && strcmp(names[i], "OPENPETE_VIBRATION") != 0) {
            if (k < sizeof lost)
                k += (size_t)snprintf(lost + k, sizeof lost - k, "%s%s", nlost ? " " : "", names[i]);
            nlost++;
        }
        rp_setenv(names[i], NULL);
        free(names[i]);
    }
    free(names);
    if (nlost)
        fprintf(stderr, "bug-report-replay: env not forwarded from this shell (%d): %s; the replay runs on the "
                "bundle's env.txt; name an instrument variable in OPENPETE_BUGREPORT_REPLAY_ENV to forward it\n",
                nlost, lost);
}

/** @brief Make <scratch>/sandbox the process's sandbox root, so every file the session writes stays in the scratch directory.
 *
 * The log file, the hang watchdog's files, a bundle, savestates, cards,
 * screenshots and the pipeline cache resolve under the root
 * (openpete_paths_sandbox), never in the developer's directories. Set
 * after env.txt, whose OPENPETE_SANDBOX is dropped and whose
 * OPENPETE_HANG_TRACE and OPENPETE_HANG_RAM are unset here, and before
 * OPENPETE_BUGREPORT_REPLAY_ENV, so a developer can still forward either. */
static void rp_env_outputs(void)
{
    rp_setenv("OPENPETE_HANG_TRACE", NULL);
    rp_setenv("OPENPETE_HANG_RAM", NULL);
    rp_join(R.sandbox, sizeof R.sandbox, R.scratch, "sandbox");
    (void)openpete_paths_set_sandbox(R.sandbox);
    rp_setenv("OPENPETE_SANDBOX", R.sandbox);
    fprintf(stderr, "bug-report-replay: outputs under the sandbox root %s (state/openpete/logs: the log file "
            "and the hang watchdog's files; data/openpete/bugreports: a bundle the replay writes); none in "
            "the user's directories\n", R.sandbox);
}

/** @brief Unset every OPENPETE_* variable of this process except the replay's own, then set env.txt's (remapped), the scratch output paths, OPENPETE_BUGREPORT_REPLAY_ENV's and OPENPETE_VIBRATION=0.
 *
 * An allowed variable is set after env.txt, so the developer's value wins
 * over the tester's. Each is printed with its class: not on the gameplay
 * list, so the row and press-state compares are what verify it neutral.
 * @return 0, or -1 (refused by rp_env_allow_read). */
static int rp_env_apply(void)
{
    if (rp_env_allow_read() != 0) return -1;
    rp_env_clear();
    char p[RP_PATH];
    rp_join(p, sizeof p, R.scratch, "env.txt");
    size_t n = 0;
    uint8_t* d = NULL;
    if (br_read_file(p, &d, &n) == 0) {
        for (char* line = strtok((char*)d, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            if (!strcmp(line, "OPENPETE_PLAYER_POSE")) {
                const size_t u = strlen(R.launch_flags);
                snprintf(R.launch_flags + u, sizeof R.launch_flags - u, "%sOPENPETE_PLAYER_POSE=%s",
                         u ? " " : "", eq + 1);
            }
            if (!rp_env_kept(line)) continue;
            char v[RP_PATH];
            rp_remap(eq + 1, v, sizeof v);
            rp_setenv(line, v);
        }
        free(d);
    }
    rp_env_outputs();
    for (int i = 0; i < s_allow_n; i++) {
        rp_setenv(s_allow[i].name, s_allow[i].value);
        fprintf(stderr, "bug-report-replay: env kept %s=%s (instrument: not on the gameplay list; the row "
                "and press-state compares verify it)\n", s_allow[i].name, s_allow[i].value);
    }
    rp_setenv("OPENPETE_VIBRATION", "0");
    return 0;
}

/* ---- prepare ----------------------------------------------------------- */

/** @brief Decide whether the tester's launch scripts replay: only from the session's first anchor.
 *
 * The recorder takes its first anchor (seq 1) at the first admissible final
 * yield after fast boot, before any launch script has written guest RAM:
 * every level skip waits for the title's induction window, hundreds of VBLs
 * later, and placements and cheats wait for a live level. A guest state load
 * drops every anchor, so a later anchor has a higher seq. From any later
 * anchor a launch script may have run, and its progress is host state the
 * anchor does not hold. A manifest without the anchor's seq (written before
 * the field) is decided by the anchor's frame: the first anchor is taken at
 * frame 1 unless an admission refusal deferred it.
 * @param an The manifest's anchor object. */
static void rp_launch_decide(const jv_t* an)
{
    const jv_t* seq = j_get(an, "seq");
    if (seq) {
        R.launch_keep = j_u(an, "seq") == 1u;
        snprintf(R.launch_why, sizeof R.launch_why, "the anchor is %s (seq %u)",
                 R.launch_keep ? "the session's first" : "a later one", j_u(an, "seq"));
        return;
    }
    R.launch_keep = j_u(an, "frame") <= 1u;
    snprintf(R.launch_why, sizeof R.launch_why, "the manifest has no anchor seq; anchor frame %u %s",
             j_u(an, "frame"), R.launch_keep ? "is the first anchor's" : "is past the first anchor's");
}

/** @brief Read the manifest's anchor and press counters, taint counts and game.
 *
 * A bundle whose press is null was written at a fatal exit (the hang
 * watchdog, the CdSync spin guard): it is marked fatal and its end frame
 * is set from hashes.csv at admission.
 * @return 0, or -1 (refused) when the bundle holds no anchor or lacks a member the replay reads. */
static int rp_read_manifest(void)
{
    const jv_t* an = j_get(&R.man, "anchor");
    const jv_t* pr = j_get(&R.man, "press");
    if (!an || an->t != J_OBJ) {
        rp_refuse("the bundle holds no anchor (manifest anchor is null): nothing to replay from");
        return -1;
    }
    snprintf(R.trigger, sizeof R.trigger, "%s", *j_s(&R.man, "trigger") ? j_s(&R.man, "trigger") : "press");
    if (!pr || pr->t != J_OBJ) {
        if (!strcmp(R.trigger, "press")) {
            rp_refuse("the bundle's press is null and its trigger is press: no end frame to replay to");
            return -1;
        }
        R.fatal = 1;
    }
    R.anchor_frame = j_u(an, "frame");
    rp_launch_decide(an);
    R.anchor_consume = j_u(an, "consume");
    R.press_frame = R.fatal ? 0u : j_u(pr, "frame");
    R.press_feed_key = R.fatal ? 0u : j_u(pr, "feed_key");
    snprintf(R.game, sizeof R.game, "%s", j_s(&R.man, "game"));
    const jv_t* t = j_get(&R.man, "taint");
    size_t k = 0;
    R.taint[0] = '\0';
    for (int i = 0; t && t->t == J_OBJ && i < t->n && k < sizeof R.taint; i++)
        k += (size_t)snprintf(R.taint + k, sizeof R.taint - k, "%s%s=%u", i ? "," : "", t->key[i],
                              (unsigned)t->kid[i].num);
    static const char* const need[] = {"anchor.state", "press.state", "segment.bk2", "hashes.csv"};
    for (size_t i = 0; i < sizeof need / sizeof need[0]; i++) {
        if (R.fatal && !strcmp(need[i], "press.state")) continue;
        char p[RP_PATH];
        rp_join(p, sizeof p, R.scratch, need[i]);
        if (!rp_is_file(p)) { rp_refuse("the bundle has no %s", need[i]); return -1; }
    }
    return 0;
}

/** @brief Choose the scratch directory: OPENPETE_BUGREPORT_REPLAY_DIR, else the bundle's path without `.zip` plus `.replay`. */
static void rp_scratch_name(void)
{
    const char* e = getenv("OPENPETE_BUGREPORT_REPLAY_DIR");
    if (e && *e) { snprintf(R.scratch, sizeof R.scratch, "%s", e); return; }
    char b[RP_PATH];
    snprintf(b, sizeof b, "%s", R.bundle);
    size_t n = strlen(b);
    while (n > 1 && (b[n - 1] == '/' || b[n - 1] == '\\')) b[--n] = '\0';
    if (n > 4 && !SDL_strcasecmp(b + n - 4, ".zip")) b[n - 4] = '\0';
    snprintf(R.scratch, sizeof R.scratch, "%s.replay", b);
}

/** @brief Find the --bug-report-replay value in argv.
 * @return The path, or NULL when the flag is absent; "" when it has no value. */
static const char* rp_find_flag(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--bug-report-replay")) return i + 1 < argc ? argv[i + 1] : "";
    return NULL;
}

/** @brief Build the replay's argv: argv[0], the tester's, the replay's, the developer's, then the flag itself.
 * @return 0, or -1 (refused). */
static int rp_build_argv(rp_argv_t* a, int argc, char** argv)
{
    int rc = rp_arg(a, argv[0]);
    rc |= rp_tester_args(a);
    rc |= rp_replay_args(a);
    rc |= rp_primary_args(a);
    const int disc = rp_dev_args(a, argc, argv);
    if (disc < 0) rc = -1;
    rc |= rp_arg(a, "--bug-report-replay") | rp_arg(a, R.bundle);
    if (rc) { if (!R.refused) rp_refuse("cannot build the command line"); return -1; }
    if (!disc) {
        rp_refuse("pass --disc with the game's disc image (the tester's disc path is not in the bundle; "
                  "the manifest names %s, SHA-256 %s)", j_s(&R.man, "disc"), j_s(&R.man, "disc_sha256"));
        return -1;
    }
    return 0;
}

int openpete_bugreport_replay_prepare(int* argc, char*** argv)
{
    const char* path = rp_find_flag(*argc, *argv);
    if (!path) return 0;
    R.on = 1;
    if (!*path) { rp_refuse("--bug-report-replay needs a bundle path"); return OPENPETE_BUGREPORT_REPLAY_REFUSED; }
    snprintf(R.bundle, sizeof R.bundle, "%s", path);
    rp_scratch_name();
    if (rp_scratch_reset(R.scratch) != 0 || rp_unpack() != 0 || rp_read_manifest() != 0)
        return OPENPETE_BUGREPORT_REPLAY_REFUSED;
    rp_argv_t a = {0};
    if (rp_build_argv(&a, *argc, *argv) != 0) return OPENPETE_BUGREPORT_REPLAY_REFUSED;
    if (rp_env_apply() != 0) return OPENPETE_BUGREPORT_REPLAY_REFUSED;
    if (R.launch_flags[0]) {
        fprintf(stderr, "bug-report-replay: launch flags %s: %s: %s\n", R.launch_keep ? "kept" : "dropped",
                R.launch_why, R.launch_flags);
        if (strstr(R.launch_why, "no anchor seq")) rp_label("launch-inferred");
    }
    if (R.fatal)
        fprintf(stderr, "bug-report-replay: %s: %s, anchor frame %u, fatal exit (trigger %s): hash rows "
                "only, to the last row, scratch %s\n", R.bundle, R.game, R.anchor_frame, R.trigger, R.scratch);
    else
        fprintf(stderr, "bug-report-replay: %s: %s, anchor frame %u, press frame %u, scratch %s\n",
                R.bundle, R.game, R.anchor_frame, R.press_frame, R.scratch);
    fprintf(stderr, "bug-report-replay: argv:");
    for (int i = 1; i < a.n; i++) fprintf(stderr, " %s", a.v[i]);
    fprintf(stderr, "\n");
    *argc = a.n;
    *argv = a.v;
    return 0;
}

/* ---- admission --------------------------------------------------------- */

/** @brief Split one CSV line into u64 fields after the frame and vbl columns.
 * @param line The line (modified).
 * @param out Receives frame, vbl, total and @p ncols region hashes.
 * @param ncols The region column count.
 * @return 0, or -1 when the line is short. */
static int rp_csv_row(char* line, uint64_t* out, int ncols)
{
    char* save = NULL;
    char* tok = SDL_strtok_r(line, ",", &save);
    for (int k = 0; k < 3 + ncols; k++) {
        if (!tok) return -1;
        out[k] = k < 2 ? (uint64_t)strtoull(tok, NULL, 10) : (uint64_t)strtoull(tok, NULL, 16);
        tok = SDL_strtok_r(NULL, ",", &save);
    }
    return 0;
}

/** @brief Read hashes.csv: the region names of its header and its rows.
 * @return 0, or -1 (refused). */
static int rp_read_hashes(void)
{
    char p[RP_PATH];
    rp_join(p, sizeof p, R.scratch, "hashes.csv");
    size_t n = 0;
    uint8_t* d = NULL;
    if (br_read_file(p, &d, &n) != 0) { rp_refuse("cannot read %s", p); return -1; }
    char* save = NULL;
    char* hdr = SDL_strtok_r((char*)d, "\r\n", &save);
    char* hs = NULL;
    int k = 0;
    for (char* t = hdr ? SDL_strtok_r(hdr, ",", &hs) : NULL; t; t = SDL_strtok_r(NULL, ",", &hs), k++) {
        if (k < 3) continue;
        char** c = (char**)realloc(R.cols, (size_t)(R.ncols + 1) * sizeof *c);
        if (!c) break;
        R.cols = c;
        R.cols[R.ncols++] = SDL_strdup(t);
    }
    const size_t stride = 3u + (size_t)R.ncols;
    size_t cap = 0;
    for (char* line = SDL_strtok_r(NULL, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        if (R.nrows == cap) {
            cap = cap ? cap * 2 : 2048;
            uint64_t* r = (uint64_t*)realloc(R.rows, cap * stride * sizeof *r);
            if (!r) break;
            R.rows = r;
        }
        if (rp_csv_row(line, R.rows + R.nrows * stride, R.ncols) == 0) R.nrows++;
    }
    free(d);
    if (R.nrows == 0) { rp_refuse("hashes.csv holds no row"); return -1; }
    /* A fatal bundle ends at its last hash row: the last tick the tester's run completed. */
    if (R.fatal) R.press_frame = (unsigned)R.rows[(R.nrows - 1) * (3u + (size_t)R.ncols)];
    return 0;
}

/** @brief Map each region of this build's table to the bundle's column of the same name.
 *
 * A region the bundle lacks, or a bundle column this build lacks, is
 * logged; the totals are compared only when the tables are the same.
 * @return The count of names the two tables do not share. */
static int rp_map_columns(void)
{
    R.nmap = openpete_state_region_count;
    R.map = (rp_col_t*)calloc(R.nmap ? R.nmap : 1, sizeof *R.map);
    int missing = 0;
    for (size_t r = 0; R.map && r < R.nmap; r++) {
        char nm[160];
        snprintf(nm, sizeof nm, "%s", openpete_state_regions[r].name);
        for (char* c = nm; *c; c++) if (*c == ',' || *c == '\n' || *c == '\r') *c = '_';
        R.map[r].col = -1;
        for (int c = 0; c < R.ncols; c++)
            if (!strcmp(R.cols[c], nm)) { R.map[r].col = c; break; }
        if (R.map[r].col < 0) { missing++; OPL_WARN_F("bugreport", "replay: region %s is not in hashes.csv", nm); }
    }
    if ((size_t)R.ncols != R.nmap) missing += R.ncols > (int)R.nmap ? R.ncols - (int)R.nmap : 0;
    return missing;
}

/** @brief Admit the ROM: compare the ROM stamps when both sides carry one, else the disc SHA-256.
 * @param opts The options (the disc path).
 * @return 0, or -1 (refused). */
static int rp_admit_rom(const openpete_options_t* opts)
{
    const openpete_recomp_module* m = openpete_recomp_module_get();
    const char* mine = m && m->rom_sha256 ? m->rom_sha256 : "";
    const char* theirs = j_s(&R.man, "rom_stamp");
    char dsha[65] = "";
    (void)br_disc_sha(opts->disc_path, dsha);
    const int disc_same = dsha[0] && !strcmp(dsha, j_s(&R.man, "disc_sha256"));
    if (*mine && *theirs) {
        if (strcmp(mine, theirs) != 0) {
            rp_refuse("ROM stamp differs: bundle %s, this disc's carve %s", theirs, mine);
            return -1;
        }
        if (!disc_same) rp_label("cross-disc");
        return 0;
    }
    if (!disc_same) {
        rp_refuse("%s carries no ROM stamp to compare, and the disc SHA-256 differs: bundle %s (%s), "
                  "this disc %s", *theirs ? "this build" : "the bundle", j_s(&R.man, "disc_sha256"),
                  j_s(&R.man, "disc"), dsha[0] ? dsha : "(unreadable)");
        return -1;
    }
    return 0;
}

/** @brief Verify the anchor's identity by recomputing it from the manifest's stamps and admit it when it is not this build's.
 * @return 0, or -1 (refused). */
static int rp_admit_ident(void)
{
    char p[RP_PATH], have[80] = "", mine[80] = "";
    rp_join(p, sizeof p, R.scratch, "anchor.state");
    size_t n = 0;
    uint8_t* d = NULL;
    if (br_read_file(p, &d, &n) != 0 || openpete_state_image_ident(d, n, have, sizeof have) != 0) {
        free(d);
        rp_refuse("anchor.state is unreadable or shorter than a state header");
        return -1;
    }
    free(d);
    const openpete_recomp_module* m = openpete_recomp_module_get();
    (void)openpete_state_ident_from_terms(m ? m->codegen_stamp : NULL, m ? m->rom_sha256 : NULL, mine,
                                          sizeof mine);
    if (!strcmp(have, mine)) return 0;
    /* The manifest writes a missing stamp as "", the identity hashed "?". */
    const char* cg = j_s(&R.man, "codegen_stamp");
    const char* rom = j_s(&R.man, "rom_stamp");
    for (int v = 0; v < 4; v++) {
        char want[80];
        (void)openpete_state_ident_from_terms((v & 1) && !*cg ? NULL : cg, (v & 2) && !*rom ? NULL : rom,
                                              want, sizeof want);
        if (strcmp(have, want) != 0) continue;
        (void)openpete_state_admit_ident(have);
        return 0;
    }
    rp_refuse("the anchor's identity %.30s does not recompute from the manifest's stamps (codegen %s, ROM "
              "%s) under this build's state ABI: the state ABI differs", have, cg, rom);
    return -1;
}

/** @brief Label a codegen-stamp and a build-version difference. */
static void rp_labels(void)
{
    const openpete_recomp_module* m = openpete_recomp_module_get();
    const char* cg = m && m->codegen_stamp ? m->codegen_stamp : "";
    if (strcmp(cg, j_s(&R.man, "codegen_stamp")) != 0) rp_label("cross-module");
    if (strcmp(openpete_build_version(), j_s(&R.man, "build_version")) != 0) rp_label("cross-build");
}

static void rp_exit_flush(void);

int openpete_bugreport_replay_admit(const openpete_options_t* opts)
{
    if (!R.on) return 0;
    openpete_crash_set_exit_flush(OP_EXIT_FLUSH_REPLAY, rp_exit_flush);
    R.beyond = opts->bug_report_replay_beyond;
    /* A -og capture pairs with the PsyCross present that rasterized its
     * scene's OT, one tick after the key when the game submits after VSync;
     * a run stopped at the end frame leaves the provisional image, the tick
     * before. */
    if (R.beyond == 0u && opts->screenshot_frame_count > 0 && !opts->screenshot_native_only) {
        R.beyond = 1u;
        fprintf(stderr, "bug-report-replay: one tick past the end frame for the -og capture, which pairs with "
                "the PsyCross present after its key (--bug-report-replay-beyond 1)\n");
    }
    if (R.refused) return OPENPETE_BUGREPORT_REPLAY_REFUSED;
    const char* id = game_get_id();
    if (!id || strcmp(id, R.game) != 0) {
        rp_refuse("the bundle is %s; this session runs %s", R.game, id ? id : "(no game)");
        return OPENPETE_BUGREPORT_REPLAY_REFUSED;
    }
    if (!opts->disc_path || rp_admit_rom(opts) != 0 || rp_admit_ident() != 0 || rp_read_hashes() != 0)
        return OPENPETE_BUGREPORT_REPLAY_REFUSED;
    rp_labels();
    if (rp_map_columns() != 0) rp_label("cross-regions");
    openpete_state_card_remap("card1.mcd", "card2.mcd");
    /* The anchor was admitted on an empty CD queue; the fresh process
     * loads it on one too. A cold boot reaches an in-level park site in
     * the attract demo after about 3500 waits (Spyro the Dragon). */
    const char* w = getenv("OPENPETE_BUGREPORT_REPLAY_SITE_WAIT");
    openpete_handoff_resume_policy(w && *w ? (unsigned)strtoul(w, NULL, 0) : 9000u, 1);
    bk2_feed_hold(1);
    OPL_INFO_F("bugreport", "replay: admitted %s (labels: %s); anchor frame %u, press frame %u, %zu hash "
               "row(s), taint at the press %s", R.bundle, R.labels[0] ? R.labels : "none", R.anchor_frame,
               R.press_frame, R.nrows, R.taint);
    return 0;
}

/* ---- the run ----------------------------------------------------------- */

/** @brief Return the bundle's row of a frame, or NULL. */
static const uint64_t* rp_row(unsigned frame)
{
    const size_t stride = 3u + (size_t)R.ncols;
    const uint64_t first = R.rows[0];
    if (frame < first || frame - first >= R.nrows) return NULL;
    const uint64_t* r = R.rows + (size_t)(frame - first) * stride;
    if (r[0] == frame) return r;
    for (size_t i = 0; i < R.nrows; i++)
        if (R.rows[i * stride] == frame) return R.rows + i * stride;
    return NULL;
}

void openpete_bugreport_replay_tick(unsigned frame, int vbl, uint64_t total, const uint64_t* per) OP_REQUIRES(sim)
{
    if (!R.on || !R.loaded || R.done) return;
    br_knobs_replay_tick(frame);
    if (frame <= R.anchor_frame || frame > R.press_frame) return;
    const uint64_t* r = rp_row(frame);
    if (!r) return;
    R.rows_compared++;
    if (R.div_frame) return;
    size_t k = 0;
    int differ = (int)(r[1] != (uint64_t)(int64_t)vbl);
    if (differ) k += (size_t)snprintf(R.div_regions, sizeof R.div_regions, "vbl");
    for (size_t i = 0; i < R.nmap && per; i++) {
        if (R.map[i].col < 0 || r[3 + R.map[i].col] == per[i]) continue;
        differ = 1;
        if (k < sizeof R.div_regions)
            k += (size_t)snprintf(R.div_regions + k, sizeof R.div_regions - k, "%s%s", k ? "|" : "",
                                  openpete_state_regions[i].name);
    }
    if (!differ && !strstr(R.labels, "cross-regions") && r[2] != total) {
        differ = 1;
        snprintf(R.div_regions, sizeof R.div_regions, "total");
    }
    if (!differ) { R.rows_matched++; return; }
    R.div_frame = frame;
    R.div_vbl = vbl;
    OPL_ERROR_F("bugreport", "replay: tick %u (vbl %d, bundle vbl %llu) differs from the bundle in %s",
                frame, vbl, (unsigned long long)r[1], R.div_regions);
}

/** @brief Name the manifest region holding a RAM offset, or "" when none does. */
static const char* rp_ram_region(size_t off)
{
    const uint32_t addr = OPENPETE_PSX_KSEG0_BASE + (uint32_t)off;
    for (size_t r = 0; r < openpete_state_region_count; r++) {
        const OpenpeteStateRegion* g = &openpete_state_regions[r];
        if (g->kind == 0u && addr >= g->start && addr - g->start < g->length) return g->name;
    }
    return "";
}

/** @brief Compare one section pair and append a difference to the section lists.
 * @param name The section.
 * @param a The replay's bytes.
 * @param an Their count.
 * @param b The bundle's bytes.
 * @param bn Their count. */
static void rp_section_cmp(const char* name, const uint8_t* a, size_t an, const uint8_t* b, size_t bn)
{
    char one[160];
    if (an != bn) {
        snprintf(one, sizeof one, "%s:size %zu/%zu", name, an, bn);
    } else {
        size_t i = 0, nd = 0;
        while (i < an && a[i] == b[i]) i++;
        if (i == an) return;
        for (size_t k = i; k < an; k++) nd += a[k] != b[k];
        const char* rg = !strcmp(name, "ram") ? rp_ram_region(i) : "";
        snprintf(one, sizeof one, "%s+0x%zX%s%s (%zu byte(s))", name, i, *rg ? " " : "", rg, nd);
    }
    /* VRAM lies outside the gameplay-state regions; a band the game never
     * draws (the framebuffer overscan) can differ across boot routes, so
     * a VRAM difference is reported beside the verdict, not counted. */
    if (!strcmp(name, "vram")) { snprintf(R.vram_note, sizeof R.vram_note, "%s", one); return; }
    if (!R.section[0]) snprintf(R.section, sizeof R.section, "%s", one);
    const size_t k = strlen(R.section_all);
    snprintf(R.section_all + k, sizeof R.section_all - k, "%s%s", k ? "; " : "", one);
    R.sections_equal = 0;
}

/** @brief Compare a game-extra section pair as the game's own payload and the normalised save-record trailer.
 *
 * The trailer carries one row per installed mod, so its size follows the
 * build's mod set; rows that hold no identity are dropped before comparing,
 * and a difference in what remains still counts.
 * @param a The replay's section.
 * @param an Its length.
 * @param b The bundle's section.
 * @param bn Its length. */
static void rp_extra_cmp(const uint8_t* a, size_t an, const uint8_t* b, size_t bn)
{
    uint8_t *na = NULL, *nb = NULL;
    uint32_t la = 0, lb = 0;
    const uint32_t oa = openpete_modsave_block_normalise(a, (uint32_t)an, &na, &la);
    const uint32_t ob = openpete_modsave_block_normalise(b, (uint32_t)bn, &nb, &lb);
    rp_section_cmp("game-extra", a, oa, b, ob);
    if (!na || !nb) rp_section_cmp("game-extra:modsave", NULL, 0, NULL, 1);
    else rp_section_cmp("game-extra:modsave", na, la, nb, lb);
    free(na);
    free(nb);
}

/** @brief Compare the replay's press-time cards with the bundle's press-card members. */
static void rp_cards_cmp(const openpete_bugrec_anchor_t* a)
{
    for (int p = 0; p < 2; p++) {
        char path[RP_PATH], leaf[32];
        snprintf(leaf, sizeof leaf, "press-card%d.mcd", p + 1);
        rp_join(path, sizeof path, R.scratch, leaf);
        size_t n = 0;
        uint8_t* d = NULL;
        if (!a->card_loaded[p] || !a->card[p] || br_read_file(path, &d, &n) != 0) { free(d); continue; }
        rp_section_cmp(leaf, a->card[p], OPENPETE_BUGREC_CARD_BYTES, d, n);
        free(d);
    }
}

/** @brief Capture the state at the press frame and compare it with press.state section by section, the header excepted. */
static void rp_end_compare(const CPUState* cpu) OP_REQUIRES(sim)
{
    R.done = 1;
    R.sections_equal = 1;
    openpete_bugrec_anchor_t a;
    memset(&a, 0, sizeof a);
    char p[RP_PATH];
    rp_join(p, sizeof p, R.scratch, "press.state");
    size_t bn = 0;
    uint8_t* b = NULL;
    if (openpete_bugrec_capture(&a, cpu) != 0 || br_read_file(p, &b, &bn) != 0) {
        snprintf(R.section, sizeof R.section, "capture failed");
        R.sections_equal = 0;
        free(b);
        openpete_bugrec_anchor_release(&a);
        return;
    }
    (void)openpete_state_image_card_paths_set(a.state, a.state_len, "card1.mcd", "card2.mcd");
    /* The replay's own press state, for a reader to diff against press.state. */
    char mine[RP_PATH];
    rp_join(mine, sizeof mine, R.scratch, "replay-press.state");
    if (rp_write(mine, a.state, a.state_len) != 0)
        OPL_WARN_F("bugreport", "replay: cannot write %s", mine);
    openpete_state_section_t sa[16], sb[16];
    const int na = openpete_state_image_sections(a.state, a.state_len, sa, 16);
    const int nb = openpete_state_image_sections(b, bn, sb, 16);
    for (int i = 1; i < na && i < nb; i++) {
        if (!strcmp(sa[i].name, "game-extra"))
            rp_extra_cmp(a.state + sa[i].off, sa[i].len, b + sb[i].off, sb[i].len);
        else
            rp_section_cmp(sa[i].name, a.state + sa[i].off, sa[i].len, b + sb[i].off, sb[i].len);
    }
    if (na != nb) rp_section_cmp("layout", NULL, (size_t)na, NULL, (size_t)nb);
    rp_cards_cmp(&a);
    free(b);
    openpete_bugrec_anchor_release(&a);
}

/** @brief Put the anchor's card images back into the ports after the load.
 *
 * The boot before the load reads and may write the scratch cards (a
 * card probe rewrites the test frame), which the tester's timeline did
 * not do after the anchor; the load restores RAM, not the cards. */
static void rp_cards_restore(void)
{
    for (int p = 0; p < 2; p++) {
        char path[RP_PATH], leaf[32];
        snprintf(leaf, sizeof leaf, "card%d.mcd", p + 1);
        rp_join(path, sizeof path, R.scratch, leaf);
        size_t n = 0;
        uint8_t* d = NULL;
        if (br_read_file(path, &d, &n) != 0) continue;
        if (openpete_memcard_image_restore(p, d, n) != 0)
            OPL_ERROR_F("bugreport", "replay: the anchor's %s could not be restored to slot %d", leaf, p + 1);
        free(d);
    }
}

/** @brief Return the ticks the run goes on past the end frame: --bug-report-replay-beyond's, at least 1 for a fatal bundle.
 *
 * A fatal bundle's end frame is its last completed tick; the tick after it
 * is the one the tester's run did not complete, where the replay looks for
 * the same fatal exit. */
static unsigned rp_beyond(void)
{
    return R.fatal && R.beyond < 1u ? 1u : R.beyond;
}

/** @brief Take the end-state compare at the end frame (the press frame, or a fatal bundle's last hash row) and print the window the run goes on for past it. */
static void rp_reach_end(const CPUState* cpu) OP_REQUIRES(sim)
{
    if (R.fatal) {
        R.done = 1;
        R.sections_equal = 1;
        OPL_INFO_F("bugreport", "replay: fatal bundle (trigger %s): the last hash row, frame %u, is reached; "
                   "the press-section compare is skipped (no press.state)", R.trigger, R.press_frame);
    } else {
        rp_end_compare(cpu);
    }
    R.end_frame = R.press_frame + rp_beyond();
    if (R.end_frame == R.press_frame) return;
    fprintf(stderr, "bug-report-replay: beyond the recorded window: ticks %u..%u run on neutral input (the movie "
            "has ended; host input is locked), compared with nothing%s\n", R.press_frame + 1u, R.end_frame,
            R.fatal ? "; the first is the tick the tester's run did not complete" : "");
}

void openpete_bugreport_replay_step(const CPUState* cpu) OP_REQUIRES(sim)
{
    if (!R.on || R.refused || R.finished) return;
    if (!R.loaded) {
        const int r = openpete_handoff_resume_result(&R.load_site_waits, &R.load_cd_waits);
        if (r == 0) return;
        if (r < 0) {
            rp_refuse("the anchor did not load (%u park-site and %u CD-queue wait(s); the log names the "
                      "loader's refusal)", R.load_site_waits, R.load_cd_waits);
            openpete_request_shutdown();
            return;
        }
        R.loaded = 1;
        bk2_feed_hold(0);
        rp_cards_restore();
        if (br_knobs_replay_load(R.scratch, R.anchor_frame)) rp_label("knobs-partial");
        OPL_INFO_F("bugreport", "replay: anchor loaded at frame %u after %u park-site and %u CD-queue "
                   "wait(s); the segment plays from consume %u", openpete_frame, R.load_site_waits,
                   R.load_cd_waits, R.anchor_consume);
    }
    if (openpete_frame < R.press_frame) return;
    if (!R.done) rp_reach_end(cpu);
    if (openpete_frame < R.end_frame) return;
    R.finished = 1;
    openpete_request_shutdown();
}

/** @brief Name the end-state compare's outcome for the verdict line: skipped for a fatal bundle, not-reached before the press frame, else the first differing section, else equal. */
static const char* rp_sections_term(void)
{
    if (R.fatal) return "skipped";
    if (!R.done) return "not-reached";
    return R.section[0] ? R.section : "equal";
}

/** @brief Format the replay's own fatal exit for a verdict line: its reason, the tick it did not complete and, for a hang, the trip.
 * @param out Receives the fields with a leading space, "" when the run had no fatal exit.
 * @param cap Capacity of @p out. */
static void rp_fatal_fields(char* out, size_t cap)
{
    out[0] = '\0';
    if (!R.run_fatal) return;
    const int n = snprintf(out, cap, " replay-fatal=%s tick=%u", R.run_fatal, R.run_fatal_tick);
    if (R.hang_kind && n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - (size_t)n, " hang-at=0x%08X hang-kind=%s", R.hang_at, R.hang_kind);
}

/** @brief Report whether the replay's fatal exit is the bundle's, at the tick the tester's run did not complete, after identical rows. */
static int rp_reproduced(void)
{
    return R.fatal && R.run_fatal && !strcmp(R.run_fatal, R.trigger) && R.done &&
           R.run_fatal_tick == R.press_frame + 1u && R.rows_compared > 0 && !R.div_frame &&
           R.rows_matched == R.rows_compared;
}

/* The fields a verdict line carries besides its word. */
typedef struct {
    const char* lab;          /* the labels, "none" when there are none */
    char        fat[64];      /* " fatal=<trigger>" for a fatal bundle */
    const char* sec;          /* rp_sections_term */
    char        run[160];     /* rp_fatal_fields */
    char        bey[48];      /* " beyond=<run>/<asked>" when ticks past the end frame were asked */
    unsigned long long rows, ok;
} rp_line_t;

/** @brief Fill a verdict line's fields from the run's state.
 * @param L Receives the fields. */
static void rp_line_fill(rp_line_t* L)
{
    memset(L, 0, sizeof *L);
    L->lab = R.labels[0] ? R.labels : "none";
    /* A fatal bundle has no press sections: its verdict names the trigger. */
    if (R.fatal) snprintf(L->fat, sizeof L->fat, " fatal=%s", R.trigger);
    L->sec = rp_sections_term();
    rp_fatal_fields(L->run, sizeof L->run);
    const unsigned asked = rp_beyond();
    const unsigned past = R.done && openpete_frame > R.press_frame ? openpete_frame - R.press_frame : 0u;
    if (asked) snprintf(L->bey, sizeof L->bey, " beyond=%u/%u", past < asked ? past : asked, asked);
    L->rows = (unsigned long long)R.rows_compared;
    L->ok = (unsigned long long)R.rows_matched;
}

/** @brief Report whether the replay's own fatal exit fell inside the recorded window: at or before the end frame, or a fatal bundle's tick after it. */
static int rp_fatal_in_window(void)
{
    return R.run_fatal && R.run_fatal_tick <= R.press_frame + (R.fatal ? 1u : 0u);
}

/** @brief Print the DIVERGED line naming the first differing row's tick and regions, else the replay's fatal exit's tick and `fatal`, and the differing sections.
 * @param L The line's fields.
 * @return 2. */
static int rp_print_diverged(const rp_line_t* L)
{
    const int fat = !R.div_frame && rp_fatal_in_window();
    fprintf(stderr, "bug-report-replay: DIVERGED tick=%u region=%s section=%s rows=%llu matched=%llu%s%s%s "
            "labels=%s taint=%s\n", R.div_frame ? R.div_frame : fat ? R.run_fatal_tick : 0u,
            R.div_frame ? R.div_regions : fat ? "fatal" : "none", L->sec, L->rows, L->ok, L->fat, L->bey, L->run,
            L->lab, R.taint);
    if (R.section_all[0]) fprintf(stderr, "bug-report-replay: sections differing: %s\n", R.section_all);
    if (R.vram_note[0]) fprintf(stderr, "bug-report-replay: vram differs (not counted): %s\n", R.vram_note);
    return 2;
}

/** @brief Print the verdict line and return the replay's exit status.
 *
 * The verdict word is decided on the recorded window: the hash rows, the
 * press sections, and for a fatal bundle the fatal exit at the tick after
 * its last row. A fatal exit of the replay inside that window that is not
 * the bundle's makes it DIVERGED; one past it (--bug-report-replay-beyond)
 * is reported on the line and decides nothing.
 * @return 0 IDENTICAL, 2 DIVERGED or VACUOUS, OPENPETE_BUGREPORT_REPLAY_REPRODUCED, or
 *         OPENPETE_BUGREPORT_REPLAY_REFUSED. */
static int rp_verdict(void)
{
    br_knobs_replay_report();
    rp_line_t L;
    rp_line_fill(&L);
    if (R.refused) {
        fprintf(stderr, "bug-report-replay: REFUSED %s labels=%s\n", R.reason, L.lab);
        return OPENPETE_BUGREPORT_REPLAY_REFUSED;
    }
    if (rp_reproduced()) {
        fprintf(stderr, "bug-report-replay: REPRODUCED rows=%llu matched=%llu%s%s labels=%s taint=%s\n", L.rows, L.ok,
                L.fat, L.run, L.lab, R.taint);
        return OPENPETE_BUGREPORT_REPLAY_REPRODUCED;
    }
    if (R.div_frame || (R.done && !R.sections_equal) || (rp_fatal_in_window() && R.loaded))
        return rp_print_diverged(&L);
    if (!R.done || R.rows_compared == 0) {
        fprintf(stderr, "bug-report-replay: %s rows=%llu matched=%llu (the run ended at frame %u, %s %u)%s%s "
                "labels=%s taint=%s\n", R.rows_compared ? "DIVERGED" : "VACUOUS", L.rows, L.ok, openpete_frame,
                R.fatal ? "last hash row" : "press frame", R.press_frame, L.fat, L.run, L.lab, R.taint);
        return 2;
    }
    fprintf(stderr, "bug-report-replay: IDENTICAL rows=%llu sections=%s%s%s%s%s labels=%s taint=%s%s%s\n", L.rows,
            L.sec, L.fat, R.fatal ? " recurred=no" : "", L.bey, L.run, L.lab, R.taint,
            R.vram_note[0] ? " vram-differs=" : "", R.vram_note);
    return 0;
}

/** @brief The replay's slot of the fatal exit flush: record the replay's own fatal exit, print the verdict and make its status the process's.
 *
 * Runs on the exiting thread before the nested bundle is written: the hang
 * watchdog's on the game fiber's thread (or the stall monitor's, whose
 * fallback runs while the guest may still run, so the counters read can be
 * one tick stale), or the CdSync spin guard's. The tick named is the one in
 * progress: the frame counter plus one. */
static void rp_exit_flush(void)
{
    if (!R.on) return;
    const char* why = openpete_crash_exit_reason();
    R.run_fatal = why && *why ? why : "fatal";
    R.run_fatal_tick = openpete_frame + 1u;
    if (!strcmp(R.run_fatal, "hang")) R.hang_kind = openpete_hang_trip(&R.hang_at);
    const int code = rp_verdict();
    fprintf(stderr, "bug-report-replay: the fatal exit's files are under the sandbox root %s\n", R.sandbox);
    fflush(stderr);
    openpete_log_file_sync();
    openpete_crash_set_fatal_status(code);
}

int openpete_bugreport_replay_exit(int rc)
{
    if (!R.on) return rc;
    return rp_verdict();
}

