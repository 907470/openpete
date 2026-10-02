// core/diag/log.c - JSON & plain-text structured logger.
//
// Output goes to stderr so stdout stays usable for stand-alone messages
// (e.g. the smoke-test exit line). Per-subsystem level filters are parsed
// once at init and stored as a small linked list:
//   "cd:debug,gpu:trace,*:info"  ->  cd->DEBUG, gpu->TRACE, default->INFO

#include "openpete_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct level_rule {
    char prefix[32];          /* subsystem prefix, or "*" for default */
    openpete_log_level_t lvl;
    struct level_rule* next;
} level_rule_t;

static openpete_log_format_t g_fmt = OPENPETE_LOG_PLAIN;
static level_rule_t* g_rules = NULL;
static openpete_log_level_t g_default = OPL_INFO;
static char g_spec[128];   /* last applied level spec, for display */

#if defined(_WIN32)
# include <sys/time.h>
# include <io.h>        /* _isatty, _fileno */
#else
# include <sys/time.h>
# include <unistd.h>    /* isatty, fileno */
#endif

/* ANSI colour for the plain-text log, only when stderr is an interactive
 * terminal (piped/redirected output - grep, files, the oracle - stays clean). */
static int g_tty = -1;

/* Pinned by the stderr mirror before it replaces fd 2 with a pipe. */
void openpete_log_pin_stderr_tty(int is_tty) { g_tty = is_tty ? 1 : 0; }

/** @brief Return whether stderr is a terminal, testing once and caching the answer unless it was pinned. */
static int stderr_is_tty(void) {
    if (g_tty < 0) {
#if defined(_WIN32)
        g_tty = _isatty(_fileno(stderr)) ? 1 : 0;
#else
        g_tty = isatty(fileno(stderr)) ? 1 : 0;
#endif
    }
    return g_tty;
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

static double g_t0 = 0.0;

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

/** @brief Free every per-subsystem level rule and empty the rule list; the default level is untouched. */
static void clear_rules(void) {
    level_rule_t* r = g_rules;
    while (r) { level_rule_t* n = r->next; free(r); r = n; }
    g_rules = NULL;
}

/** @brief Apply a comma-separated "subsystem:level" spec to the rule list and the default level.
 *
 * Segments longer than 63 bytes are truncated and subsystem names longer than
 * 31 bytes are truncated. A segment without a colon or with an unknown level
 * is skipped. "*" sets the default level; any other name is pushed at the
 * head of the rule list, so a later segment for the same name wins. A NULL or
 * empty spec changes nothing. */
static void parse_spec(const char* spec) {
    if (!spec || !*spec) return;
    const char* p = spec;
    while (*p) {
        const char* comma = strchr(p, ',');
        size_t seg_len = comma ? (size_t)(comma - p) : strlen(p);
        char seg[64];
        if (seg_len >= sizeof(seg)) seg_len = sizeof(seg) - 1;
        memcpy(seg, p, seg_len);
        seg[seg_len] = '\0';

        char* colon = strchr(seg, ':');
        if (colon) {
            *colon = '\0';
            const char* prefix = seg;
            const char* lvl_s  = colon + 1;
            openpete_log_level_t lvl;
            if (parse_level(lvl_s, &lvl) == 0) {
                if (strcmp(prefix, "*") == 0) {
                    g_default = lvl;
                } else {
                    level_rule_t* r = (level_rule_t*)calloc(1, sizeof(*r));
                    if (r) {
                        size_t n = strlen(prefix);
                        if (n >= sizeof(r->prefix)) n = sizeof(r->prefix) - 1;
                        memcpy(r->prefix, prefix, n);
                        r->prefix[n] = '\0';
                        r->lvl = lvl;
                        r->next = g_rules;
                        g_rules = r;
                    }
                }
            }
        }
        if (!comma) break;
        p = comma + 1;
    }
}

/** @brief Return the level filter for subsystem.
 *
 * The rule whose stored name equals the subsystem exactly wins, the most
 * recently added of equal names first; otherwise the default level.
 * @param subsystem Subsystem name, or NULL for the default. */
static openpete_log_level_t lookup_level(const char* subsystem) {
    if (!subsystem) return g_default;
    for (level_rule_t* r = g_rules; r; r = r->next) {
        if (strcmp(r->prefix, subsystem) == 0) return r->lvl;
    }
    return g_default;
}

void openpete_log_init(openpete_log_format_t fmt, const char* level_spec) {
    g_fmt = fmt;
    /* Anchor the timeline once. main() initialises the logger before the
     * recomp module is attached (so a first-boot build failure is logged
     * with real timestamps) and openpete_run() initialises again; a second
     * reset would rewind every timestamp mid-file. */
    if (g_t0 == 0.0) g_t0 = monotonic_seconds();
    clear_rules();
    g_default = OPL_INFO;

    /* Env var first, then explicit spec overrides. */
    const char* env = getenv("OPENPETE_LOG");
    if (env) parse_spec(env);
    if (level_spec) parse_spec(level_spec);
    {
        const char* eff = level_spec ? level_spec : env;
        snprintf(g_spec, sizeof g_spec, "%s", eff ? eff : "");
    }
}

/* Live re-configuration from the knob registry; see openpete_log.h. */
void openpete_log_set_format(openpete_log_format_t fmt) { g_fmt = fmt; }
openpete_log_format_t openpete_log_get_format(void) { return g_fmt; }

void openpete_log_set_spec(const char* spec) {
    clear_rules();
    g_default = OPL_INFO;
    parse_spec(spec);
    snprintf(g_spec, sizeof g_spec, "%s", spec ? spec : "");
}

const char* openpete_log_get_spec(void) { return g_spec; }

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

void openpete_logf(openpete_log_level_t level, const char* subsystem,
                   const char* fmt, ...) {
    if (level < lookup_level(subsystem ? subsystem : "")) return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    double t = monotonic_seconds() - g_t0;

    if (g_fmt == OPENPETE_LOG_JSON) {
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
