// core/diag/log.c - JSON & plain-text structured logger.
//
// Output goes to stderr so stdout stays usable for stand-alone messages
// (e.g. the smoke-test exit line). Per-subsystem level filters are parsed
// from a spec into a rule array:
//   "cd:debug,gpu:trace,*:info"  ->  cd->DEBUG, gpu->TRACE, default->INFO
//
// The whole configuration (format, rules, default level, spec string and
// timestamp origin) is one immutable snapshot behind an atomic pointer.
// openpete_log_init, openpete_log_set_spec and openpete_log_set_format
// build a new snapshot and publish it with a release compare-and-swap;
// every reader takes one acquire load and sees one consistent snapshot.
// Any thread may log while another reconfigures (the devui knob rows run
// inside a present, on the present thread under eager production). A
// replaced snapshot is never freed: a reader may still hold it, no
// quiescent point exists to reclaim it, and openpete_log_get_spec hands its
// spec string to callers. Each publish links the snapshot it replaced, so
// the retired chain stays reachable; reconfiguration is a rare human
// action and the chain grows by one small allocation per change.

#include "openpete_log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct level_rule {
    char prefix[32];          /* subsystem name; "" matches a NULL subsystem */
    openpete_log_level_t lvl;
} level_rule_t;

typedef struct log_snapshot {
    const struct log_snapshot* retired;  /* snapshot this one replaced, kept reachable */
    openpete_log_format_t fmt;
    openpete_log_level_t dflt;           /* level of the "*" rule, OPL_INFO when unset */
    double t0;                           /* monotonic origin of the timestamps, 0.0 before init */
    char spec[128];                      /* last applied level spec, for display */
    size_t n_rules;
    level_rule_t rules[];                /* in spec order: the last match wins */
} log_snapshot_t;

/* The configuration in force before the first openpete_log_init. */
static const log_snapshot_t k_boot_snapshot = {
    NULL, OPENPETE_LOG_PLAIN, OPL_INFO, 0.0, "", 0
};

static _Atomic(const log_snapshot_t*) g_snap = &k_boot_snapshot;

/** @brief Return the snapshot in force, with acquire ordering so its contents are visible. */
static const log_snapshot_t* snapshot_load(void) {
    return atomic_load_explicit(&g_snap, memory_order_acquire);
}

#if defined(_WIN32)
# include <sys/time.h>
# include <io.h>        /* _isatty, _fileno */
#else
# include <sys/time.h>
# include <unistd.h>    /* isatty, fileno */
#endif

/* ANSI colour for the plain-text log, only when stderr is an interactive
 * terminal (piped/redirected output - grep, files, the oracle - stays clean).
 * -1 until tested or pinned, then 0 or 1. */
static atomic_int g_tty = -1;

/* Pinned by the stderr mirror before it replaces fd 2 with a pipe. */
void openpete_log_pin_stderr_tty(int is_tty) {
    atomic_store_explicit(&g_tty, is_tty ? 1 : 0, memory_order_relaxed);
}

/** @brief Return whether stderr is a terminal, testing once and caching the answer unless it was pinned.
 *
 * Racing first callers each test, and the first answer stored wins; a pin
 * stored before or after overrides it. */
static int stderr_is_tty(void) {
    int v = atomic_load_explicit(&g_tty, memory_order_relaxed);
    if (v >= 0) return v;
#if defined(_WIN32)
    int tested = _isatty(_fileno(stderr)) ? 1 : 0;
#else
    int tested = isatty(fileno(stderr)) ? 1 : 0;
#endif
    int expected = -1;
    if (atomic_compare_exchange_strong_explicit(&g_tty, &expected, tested,
                                                memory_order_relaxed,
                                                memory_order_relaxed))
        return tested;
    return expected;
}

/** @brief Return the ANSI escape that colours a line of level l, "" for info. */
static const char* level_color(openpete_log_level_t l) {
    switch (l) {
        case OPL_TRACE: return "\033[90m";    /* grey   */
        case OPL_DEBUG: return "\033[36m";    /* cyan   */
        case OPL_INFO:  return "";            /* default */
        case OPL_WARN:  return "\033[33m";    /* yellow */
        case OPL_ERROR: return "\033[1;31m";  /* bold red */
    }
    return "";
}

/** @brief Return CLOCK_MONOTONIC in seconds, 0.0 when the clock read fails. */
static double monotonic_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/** @brief Return the lowercase name of level l, or a placeholder outside the enum. */
static const char* level_name(openpete_log_level_t l) {
    switch (l) {
        case OPL_TRACE: return "trace";
        case OPL_DEBUG: return "debug";
        case OPL_INFO:  return "info";
        case OPL_WARN:  return "warn";
        case OPL_ERROR: return "error";
    }
    return "?";
}

/** @brief Parse a lowercase level name (trace, debug, info, warn, error) into out.
 *
 * @return 0 on a match, -1 for any other text, with out untouched. */
static int parse_level(const char* s, openpete_log_level_t* out) {
    if (strcmp(s, "trace") == 0) { *out = OPL_TRACE; return 0; }
    if (strcmp(s, "debug") == 0) { *out = OPL_DEBUG; return 0; }
    if (strcmp(s, "info")  == 0) { *out = OPL_INFO;  return 0; }
    if (strcmp(s, "warn")  == 0) { *out = OPL_WARN;  return 0; }
    if (strcmp(s, "error") == 0) { *out = OPL_ERROR; return 0; }
    return -1;
}

/* A configuration under construction, private to the building thread. */
typedef struct {
    openpete_log_level_t dflt;
    level_rule_t* rules;
    size_t n_rules, cap;
    int oom;                  /* a rule allocation failed; the rule was dropped */
} spec_build_t;

/** @brief Append a rule for prefix at lvl, truncating the name to 31 bytes; on allocation failure drop it. */
static void build_push_rule(spec_build_t* b, const char* prefix,
                            openpete_log_level_t lvl) {
    if (b->n_rules == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8;
        level_rule_t* r = (level_rule_t*)realloc(b->rules, cap * sizeof(*r));
        if (!r) { b->oom = 1; return; }
        b->rules = r;
        b->cap = cap;
    }
    level_rule_t* r = &b->rules[b->n_rules++];
    size_t n = strlen(prefix);
    if (n >= sizeof(r->prefix)) n = sizeof(r->prefix) - 1;
    memcpy(r->prefix, prefix, n);
    r->prefix[n] = '\0';
    r->lvl = lvl;
}

/** @brief Apply one "subsystem:level" segment to b; a segment without a colon or with an unknown level is skipped. */
static void build_apply_segment(spec_build_t* b, char* seg) {
    char* colon = strchr(seg, ':');
    if (!colon) return;
    *colon = '\0';
    openpete_log_level_t lvl;
    if (parse_level(colon + 1, &lvl) != 0) return;
    if (strcmp(seg, "*") == 0) b->dflt = lvl;
    else build_push_rule(b, seg, lvl);
}

/** @brief Apply a comma-separated "subsystem:level" spec to b.
 *
 * Segments longer than 63 bytes are truncated and subsystem names longer than
 * 31 bytes are truncated. "*" sets the default level; any other name appends
 * a rule, and lookup_level takes the last rule of a name, so a later segment
 * for the same name wins. A NULL or empty spec changes nothing. */
static void build_parse_spec(spec_build_t* b, const char* spec) {
    if (!spec || !*spec) return;
    const char* p = spec;
    while (*p) {
        const char* comma = strchr(p, ',');
        size_t seg_len = comma ? (size_t)(comma - p) : strlen(p);
        char seg[64];
        if (seg_len >= sizeof(seg)) seg_len = sizeof(seg) - 1;
        memcpy(seg, p, seg_len);
        seg[seg_len] = '\0';
        build_apply_segment(b, seg);
        if (!comma) break;
        p = comma + 1;
    }
}

/** @brief Allocate a snapshot carrying b's rules and default level, with the other fields zeroed.
 *
 * @return The snapshot, or NULL when the allocation fails. */
static log_snapshot_t* snapshot_from_build(const spec_build_t* b) {
    log_snapshot_t* s = (log_snapshot_t*)calloc(
        1, sizeof(*s) + b->n_rules * sizeof(level_rule_t));
    if (!s) return NULL;
    s->dflt = b->dflt;
    s->n_rules = b->n_rules;
    if (b->n_rules) memcpy(s->rules, b->rules, b->n_rules * sizeof(level_rule_t));
    return s;
}

/** @brief Allocate a copy of src, rules and spec included.
 *
 * @return The copy, or NULL when the allocation fails. */
static log_snapshot_t* snapshot_clone(const log_snapshot_t* src) {
    size_t sz = sizeof(*src) + src->n_rules * sizeof(level_rule_t);
    log_snapshot_t* s = (log_snapshot_t*)malloc(sz);
    if (s) memcpy(s, src, sz);
    return s;
}

/** @brief Publish next in place of cur, linking cur as retired.
 *
 * @return 1 when published; 0 when another thread published first, in which
 *         case next is freed and the caller rebuilds from the new snapshot. */
static int snapshot_publish(const log_snapshot_t* cur, log_snapshot_t* next) {
    next->retired = cur;
    if (atomic_compare_exchange_strong_explicit(&g_snap, &cur, next,
                                                memory_order_release,
                                                memory_order_relaxed))
        return 1;
    free(next);
    return 0;
}

/** @brief Return the level filter for subsystem under snapshot s.
 *
 * The rule whose stored name equals the subsystem exactly wins, the most
 * recently added of equal names first; otherwise the default level.
 * @param s Snapshot in force.
 * @param subsystem Subsystem name, or NULL for the default. */
static openpete_log_level_t lookup_level(const log_snapshot_t* s,
                                         const char* subsystem) {
    if (!subsystem) return s->dflt;
    for (size_t i = s->n_rules; i-- > 0;) {
        if (strcmp(s->rules[i].prefix, subsystem) == 0) return s->rules[i].lvl;
    }
    return s->dflt;
}

/** @brief Build and publish a snapshot of fmt and the rules of the given specs layered in order.
 *
 * Rules start empty with the default OPL_INFO; first then second are parsed.
 * The timestamp origin is carried over from the snapshot in force; with
 * anchor_t0 set and that origin still 0.0 it is taken now instead. The spec
 * string is display. A failed snapshot allocation leaves the configuration
 * in force unchanged.
 * @param fmt Output format, or -1 to keep the format in force.
 * @param anchor_t0 Nonzero to take the timestamp origin if none is set.
 * @param first Spec parsed first, or NULL.
 * @param second Spec parsed second, or NULL.
 * @param display Spec string read back by openpete_log_get_spec, or NULL for "". */
static void apply_config(int fmt, int anchor_t0, const char* first,
                         const char* second, const char* display) {
    spec_build_t b = { OPL_INFO, NULL, 0, 0, 0 };
    build_parse_spec(&b, first);
    build_parse_spec(&b, second);
    for (;;) {
        const log_snapshot_t* cur = snapshot_load();
        log_snapshot_t* next = snapshot_from_build(&b);
        if (!next) break;
        next->fmt = fmt < 0 ? cur->fmt : (openpete_log_format_t)fmt;
        next->t0 = (cur->t0 == 0.0 && anchor_t0) ? monotonic_seconds() : cur->t0;
        snprintf(next->spec, sizeof next->spec, "%s", display ? display : "");
        if (snapshot_publish(cur, next)) break;
    }
    free(b.rules);
}

void openpete_log_init(openpete_log_format_t fmt, const char* level_spec) {
    /* Env var first, then explicit spec overrides. main() initialises the
     * logger before the recomp module is attached (so a first-boot build
     * failure is logged with real timestamps) and openpete_run() initialises
     * again; the timestamp origin is anchored by the first call only, since
     * a second reset would rewind every timestamp mid-file. */
    const char* env = getenv("OPENPETE_LOG");
    apply_config((int)fmt, 1, env, level_spec, level_spec ? level_spec : env);
}

/* Live re-configuration from the knob registry; see openpete_log.h. */
void openpete_log_set_format(openpete_log_format_t fmt) {
    for (;;) {
        const log_snapshot_t* cur = snapshot_load();
        log_snapshot_t* next = snapshot_clone(cur);
        if (!next) return;
        next->fmt = fmt;
        if (snapshot_publish(cur, next)) return;
    }
}

openpete_log_format_t openpete_log_get_format(void) { return snapshot_load()->fmt; }

void openpete_log_set_spec(const char* spec) {
    apply_config(-1, 0, spec, NULL, spec);
}

const char* openpete_log_get_spec(void) { return snapshot_load()->spec; }

/** @brief Write s to f as a quoted JSON string literal.
 *
 * Escapes the double quote and the backslash, writes newline, carriage
 * return and tab as their two-character escapes and any other byte below
 * 0x20 as a six-character u00XX escape; every other byte, UTF-8 included,
 * passes through unchanged. */
static void emit_json_string(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n",  f); break;
            case '\r': fputs("\\r",  f); break;
            case '\t': fputs("\\t",  f); break;
            default:
                if (c < 0x20) fprintf(f, "\\u%04x", c);
                else fputc(c, f);
        }
    }
    fputc('"', f);
}

/* The calling thread's capture buffer (openpete_log_capture_begin): while
 * on, every line of level info or above lands here instead of stderr. */
static _Thread_local struct { char* b; size_t n, cap; int on, oom; } t_cap;

/** @brief Append text to the calling thread's capture buffer, growing it; an allocation failure stops the capture's growth. */
static void capture_put(const char* s, size_t n) {
    if (t_cap.oom) return;
    if (t_cap.n + n + 1 > t_cap.cap) {
        size_t cap = t_cap.cap ? t_cap.cap * 2 : 16384;
        while (cap < t_cap.n + n + 1) cap *= 2;
        char* g = (char*)realloc(t_cap.b, cap);
        if (!g) { t_cap.oom = 1; return; }
        t_cap.b = g;
        t_cap.cap = cap;
    }
    memcpy(t_cap.b + t_cap.n, s, n);
    t_cap.n += n;
    t_cap.b[t_cap.n] = '\0';
}

void openpete_log_capture_begin(void) {
    free(t_cap.b);
    t_cap.b = NULL;
    t_cap.n = t_cap.cap = 0;
    t_cap.oom = 0;
    t_cap.on = 1;
    capture_put("", 0);
}

char* openpete_log_capture_end(size_t* len) {
    char* b = t_cap.b;
    if (len) *len = t_cap.n;
    t_cap.b = NULL;
    t_cap.n = t_cap.cap = 0;
    t_cap.on = 0;
    return b;
}

void openpete_log_rawf(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (t_cap.on) {
        capture_put(msg, n < 0 ? 0 : ((size_t)n < sizeof msg ? (size_t)n : sizeof msg - 1));
        return;
    }
    fputs(msg, stderr);
    fflush(stderr);
}

void openpete_logf(openpete_log_level_t level, const char* subsystem,
                   const char* fmt, ...) {
    const log_snapshot_t* snap = snapshot_load();
    if (t_cap.on ? level < OPL_INFO : level < lookup_level(snap, subsystem ? subsystem : ""))
        return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    double t = monotonic_seconds() - snap->t0;
    if (t_cap.on) {
        char line[1200];
        const int n = snprintf(line, sizeof line, "%8.4f [%-5s] %s: %s\n", t, level_name(level),
                               subsystem ? subsystem : "-", msg);
        capture_put(line, n < 0 ? 0 : ((size_t)n < sizeof line ? (size_t)n : sizeof line - 1));
        return;
    }

    if (snap->fmt == OPENPETE_LOG_JSON) {
        fprintf(stderr, "{\"t\":%.4f,\"level\":\"%s\",\"subsystem\":",
                t, level_name(level));
        emit_json_string(stderr, subsystem ? subsystem : "");
        fputs(",\"msg\":", stderr);
        emit_json_string(stderr, msg);
        fputs("}\n", stderr);
    } else {
        const char* col = stderr_is_tty() ? level_color(level) : "";
        const char* rst = (col && *col) ? "\033[0m" : "";
        fprintf(stderr, "%s%8.4f [%-5s] %s: %s%s\n",
                col, t, level_name(level), subsystem ? subsystem : "-", msg, rst);
    }
    fflush(stderr);
}
