/*
 * core/config/config.c - openpete.toml: load, resolve, materialize.
 *
 * The precedence chain and the reason flattening is required (rather than
 * splicing every layer and letting each consumer's own CLI-beats-env rule
 * decide) are documented in openpete_config.h. Read that first.
 *
 * Runs before the logger is configured - --log-level and --log-format are
 * themselves config knobs - so diagnostics here go straight to stderr,
 * the same convention options.c uses.
 *
 * Deliberately SDL-free: directories come from openpete_paths.h, which
 * calls the platform directly, so the resolver can be linked into a
 * standalone unit test without dragging SDL into the link line.
 */

#include "openpete_config.h"
#include "config_schema.h"
#include "openpete_keymap.h"
#include "openpete_paths.h"
#include "openpete_atomic_file.h"

#include "../../third_party/tomlc99/toml.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <limits.h>
#endif

/* ------------------------------------------------------------------ */
/* tiers                                                              */
/* ------------------------------------------------------------------ */

enum {
    TIER_GLOBAL = 1,
    TIER_GAME,
    TIER_DISC_SERIAL,
    TIER_DISC_SHA,
    TIER_SAVED,
    TIER_REAL_ENV,
    TIER_SIDECAR,
    TIER_CLI
};

/* Spelling, the minor sort key: notation never outranks scope. */
enum { SP_BASE = 0, SP_ENV = 1, SP_ARGS = 2 };

/* Rank packs (tier, file, spelling) into one comparable integer. `file`
 * is 0 for the exe-dir file and 1 for the user-dir one: your personal
 * file beats the shipped file at the same scope. */
#define RANK(tier, file, spelling) ((tier) * 1000 + (file) * 10 + (spelling))

#define CFG_KEY_MAX    96
#define CFG_VALUE_MAX  512
#define CFG_ORIGIN_MAX 96
#define CFG_MAX_CONTRIB 512
#define CFG_MAX_TOKENS  256
#define CFG_MAX_SAVED   64
#define CFG_PATH_MAX    2048

typedef struct {
    int  knob;                       /* schema index, or -1 */
    int  rank;
    unsigned char spelling;
    unsigned char suppress;          /* [args] "--flag" = false */
    unsigned char valueless;         /* unregistered flag, bool true */
    unsigned char has_value;
    char key[CFG_KEY_MAX];           /* flag or env var (unregistered rows) */
    char value[CFG_VALUE_MAX];
    char origin[CFG_ORIGIN_MAX];     /* "game.spyro-1.args" */
} contrib_t;

static contrib_t s_contrib[CFG_MAX_CONTRIB];
static int       s_contrib_n;

/* Emitted argv tokens, and the origin label of each (so an unknown-flag
 * error can name the config file instead of a flag nobody typed). */
static char *s_tokens[CFG_MAX_TOKENS];
static char  s_token_origin[CFG_MAX_TOKENS][CFG_ORIGIN_MAX];
static int   s_token_n;

/* Per-knob decision, for --config-report and the overlay badges. */
typedef struct {
    int  knob;
    char value[CFG_VALUE_MAX];
    char origin[CFG_ORIGIN_MAX];
    char pin[CFG_KEY_MAX + 2];   /* launch-tier spelling ("$VAR"), "" if none */
    int  pinned;
    int  shadowed;               /* how many lower layers it beat */
    char shadow[192];
} decision_t;

static decision_t s_decision[128];
static int        s_decision_n;

/* The managed [saved] zone, round-tripped through memory. */
typedef struct { char id[CFG_KEY_MAX]; char value[CFG_VALUE_MAX]; } saved_t;
static saved_t s_saved[CFG_MAX_SAVED];
static int     s_saved_n;

/* The zone's [saved.keys.<domain>] rows, round-tripped the same way: every
 * row of the write target is copied at harvest (pad2..pad4 and mod.<id>
 * included), so a flush rewrites the rows the binding page does not edit. */
#define CFG_MAX_SAVED_KEYS 128
#define CFG_SAVED_KEY_TOKS (KM_MAX_SOURCES * 2)
typedef struct {
    char domain[CFG_KEY_MAX];                    /* "pad", "host", "mod.<id>" */
    char action[CFG_KEY_MAX];
    char tok[CFG_SAVED_KEY_TOKS][CFG_KEY_MAX];
    int  n;
} saved_key_t;
static saved_key_t s_saved_keys[CFG_MAX_SAVED_KEYS];
static int         s_saved_keys_n;

static char s_file_path[2][CFG_PATH_MAX];   /* [0] exe dir, [1] user dir */
static int  s_file_present[2];
static int  s_file_n;
static int  s_write_file = 1;               /* index of the save target */
static char s_save_path[CFG_PATH_MAX];

static int  s_no_config;
static int  s_report_requested;
static char s_scope_game[64];
static char s_scope_serial[64];
static char s_scope_sha[80];

/* The game-neutral stage (openpete_config_apply_neutral) resolves the
 * tiers that hold no [game.*] or [disc.*] scope and writes nothing to the
 * process: its [env] winners are kept here and read through
 * openpete_config_env. The full stage (openpete_config_apply) runs once,
 * after the game is identified, and is the only one that calls setenv. */
static int  s_neutral;
#define CFG_MAX_PENDING_ENV 128
typedef struct { char key[CFG_KEY_MAX]; char value[CFG_VALUE_MAX]; } pending_env_t;
static pending_env_t s_pending_env[CFG_MAX_PENDING_ENV];
static int           s_pending_env_n;

/* Identification result, set before the full stage: the disc image whose
 * [disc.*] scopes apply, and the label of the source that named the game. */
static char s_ident_disc[CFG_PATH_MAX];
static char s_ident_source[32];
static int  s_materializations;

/* Contributions the full stage refused: a device-creation knob of class
 * CFG_DEVICE_FIXED or the `disc` identification input, set in a game or
 * disc scope. Listed by --config-report as not applied. */
#define CFG_MAX_NOT_APPLIED 32
typedef struct {
    char key[CFG_KEY_MAX];
    char value[CFG_VALUE_MAX];
    char origin[CFG_ORIGIN_MAX];
    const char *why;
} not_applied_t;
static not_applied_t s_not_applied[CFG_MAX_NOT_APPLIED];
static int           s_not_applied_n;

/* Hashes of the diagnostics already printed. Both stages harvest the
 * global and user scopes, so each warning about them would print twice. */
#define CFG_MAX_DIAG 256
static unsigned long long s_diag_hash[CFG_MAX_DIAG];
static int                s_diag_n;

/** @brief Print a resolver diagnostic to stderr, once per distinct text.
 *
 * The text is formatted, hashed with FNV-1a 64, and skipped when the same
 * hash was printed before; past CFG_MAX_DIAG distinct texts every
 * diagnostic prints. */
static void cfg_diag_v(const char *fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    unsigned long long h = 1469598103934665603ULL;
    for (const char *p = buf; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    for (int i = 0; i < s_diag_n; i++)
        if (s_diag_hash[i] == h) return;
    if (s_diag_n < CFG_MAX_DIAG) s_diag_hash[s_diag_n++] = h;
    fputs(buf, stderr);
}

/** @brief Print a resolver diagnostic to stderr unless the same text was printed before (cfg_diag_v). */
static void cfg_diag(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cfg_diag_v(fmt, ap);
    va_end(ap);
}

void openpete_config_diag(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cfg_diag_v(fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* paths                                                              */
/* ------------------------------------------------------------------ */

/** @brief Whether `p` opens for reading in binary mode. */
static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ */
/* contribution table                                                 */
/* ------------------------------------------------------------------ */

/** @brief Append a zeroed contribution with no schema knob (`knob` = -1).
 *
 * Returns NULL, after printing to stderr, once the table holds
 * CFG_MAX_CONTRIB entries. */
static contrib_t *contrib_new(void) {
    if (s_contrib_n >= CFG_MAX_CONTRIB) {
        cfg_diag("openpete: config: too many entries (max %d)\n",
                CFG_MAX_CONTRIB);
        return NULL;
    }
    contrib_t *c = &s_contrib[s_contrib_n++];
    memset(c, 0, sizeof *c);
    c->knob = -1;
    return c;
}

/** @brief Whether `a` and `b` name the same setting.
 *
 * When either entry has a schema knob the two match on the knob index; two
 * entries with no schema row match on the same spelling and literal key. */
static int same_setting(const contrib_t *a, const contrib_t *b) {
    if (a->knob >= 0 || b->knob >= 0) return a->knob == b->knob;
    return a->spelling == b->spelling && !strcmp(a->key, b->key);
}

/* ------------------------------------------------------------------ */
/* value normalization                                                */
/* ------------------------------------------------------------------ */

/** @brief Render one TOML scalar into `out` in the flag vocabulary.
 *
 * Tries bool, string, integer and float in that order. A bool is written
 * "1"/"0" and sets `*is_bool` with its value in `*bool_val`, which the
 * [args] spelling reads as emitting or suppressing the flag rather than as
 * a value. A float is written with %g ("0.5", "100"). Returns 0, or -1 for
 * a NULL `raw` (an array or table) or a timestamp. */
static int toml_scalar_to_string(toml_raw_t raw, char *out, size_t cap,
                                 int *is_bool, int *bool_val) {
    *is_bool = 0;
    *bool_val = 0;
    if (!raw) return -1;

    int b = 0;
    if (toml_rtob(raw, &b) == 0) {
        *is_bool = 1;
        *bool_val = b;
        snprintf(out, cap, "%d", b ? 1 : 0);
        return 0;
    }
    char *s = NULL;
    if (toml_rtos(raw, &s) == 0) {
        snprintf(out, cap, "%s", s ? s : "");
        free(s);
        return 0;
    }
    int64_t i = 0;
    if (toml_rtoi(raw, &i) == 0) {
        snprintf(out, cap, "%lld", (long long)i);
        return 0;
    }
    double d = 0;
    if (toml_rtod(raw, &d) == 0) {
        /* %g keeps 0.5 as "0.5" and 100 as "100" - the spellings a user
         * would have typed on the command line. */
        snprintf(out, cap, "%g", d);
        return 0;
    }
    return -1;
}

/** @brief Whether `v` is an accepted spelling of enum knob `k`.
 *
 * Returns 1 for a knob that is not CFG_ENUM or has no vocabulary, else 1 only
 * when `v` equals a vocabulary entry exactly (case-sensitive). */
static int enum_ok(const cfg_knob_t *k, const char *v) {
    if (k->type != CFG_ENUM || !k->vocab) return 1;
    for (int i = 0; k->vocab[i]; i++)
        if (!strcmp(k->vocab[i], v)) return 1;
    return 0;
}

/** @brief Write the vocabulary of `k` into `out` as `a|b|c`.
 *
 * Writes an empty string for a knob without a vocabulary. The text is truncated
 * to `cap` - 1 characters; `cap` must be at least 1. */
static void enum_vocab_str(const cfg_knob_t *k, char *out, size_t cap) {
    out[0] = '\0';
    if (!k->vocab) return;
    for (int i = 0; k->vocab[i]; i++) {
        if (i) strncat(out, "|", cap - strlen(out) - 1);
        strncat(out, k->vocab[i], cap - strlen(out) - 1);
    }
}

/** @brief Whether a bare key's value has the type its knob takes, printing the refusal when not.
 *
 * A bool knob takes a TOML boolean or 0, 1, on, off; an enum knob a word of
 * its vocabulary.
 * @param k The knob.
 * @param val The value as text.
 * @param is_bool 1 when the value was a TOML boolean.
 * @param where The file and table, for the message.
 * @param key The key, for the message.
 * @return 1 when the type matches, else 0 after the message. */
static int base_value_typed(const cfg_knob_t *k, const char *val, int is_bool,
                            const char *where, const char *key) {
    if (k->type == CFG_BOOL && !is_bool &&
        strcmp(val, "0") && strcmp(val, "1") &&
        strcmp(val, "on") && strcmp(val, "off")) {
        cfg_diag("openpete: config: %s: '%s' expects true/false, "
                 "got: %s\n", where, key, val);
        return 0;
    }
    if (!enum_ok(k, val)) {
        char vocab[128];
        enum_vocab_str(k, vocab, sizeof vocab);
        cfg_diag("openpete: config: %s: '%s' expects %s, got: %s\n",
                where, key, vocab, val);
        return 0;
    }
    return 1;
}

/** @brief Answer a value refused by type: --config-report fails on it; a boot warns and drops it, so the knob keeps its default.
 *
 * A stored setting that stops the game from booting is worse than the
 * default; the report is how a user checks a file, so it refuses.
 * @param where The file and table.
 * @param key The key.
 * @param val The refused value.
 * @return -1 under --config-report, else 0 after the warning. */
static int value_refused(const char *where, const char *key, const char *val) {
    if (s_report_requested) return -1;
    cfg_diag("openpete: config: %s: %s = \"%s\" is not accepted; "
             "keeping the default\n", where, key, val);
    return 0;
}

/** @brief Answer a [keys] or [args] row refused for its name or binding: --config-report fails on it; a boot warns and ignores the row.
 *
 * The reason is already printed by the caller or the keymap.
 * @param where The file and table.
 * @param table The sub-table, such as "keys.host" or "args".
 * @param key The row's key.
 * @return -1 under --config-report, else 0 after the warning. */
static int row_refused(const char *where, const char *table, const char *key) {
    if (s_report_requested) return -1;
    cfg_diag("openpete: config: %s: [%s] '%s' ignored\n", where, table, key);
    return 0;
}

/** @brief The schema knob id closest to an unknown config key, or NULL.
 *
 * Scores each id by the characters that match `key` position by position up
 * to the shorter length, with '-' and '_' equal, and returns the highest
 * scorer whose score is at least half the id's length. Catches a substituted
 * character, a truncation and a dash/underscore mixup; an inserted or
 * deleted character shifts every later position and is not caught. */
static const char *did_you_mean(const char *key) {
    size_t n;
    const cfg_knob_t *sch = cfg_schema(&n);
    const char *best = NULL;
    int best_score = 0;
    for (size_t i = 0; i < n; i++) {
        int score = 0;
        const char *a = key, *b = sch[i].id;
        while (*a && *b) {
            char ca = (*a == '-') ? '_' : *a;
            char cb = (*b == '-') ? '_' : *b;
            if (ca == cb) score++;
            a++; b++;
        }
        int len = (int)strlen(sch[i].id);
        if (score > best_score && score * 2 >= len) {
            best_score = score;
            best = sch[i].id;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* harvesting one scope out of a parsed document                      */
/* ------------------------------------------------------------------ */

/** @brief Whether `key` is `portable`, the one top-level word that is a directive rather than a knob.
 *
 * The sub-table namespaces (env, args, game, disc, saved) need no
 * reserving: this walk only ever looks at scalars, and TOML cannot spell
 * `disc` as both a scalar and a table in one document. So a global
 * `disc = "path/to.cue"` is the path knob and `[disc.SCUS-94228]` is the
 * scope namespace, with the file itself deciding which one it meant. */
static int is_directive_key(const char *key) {
    return !strcmp(key, "portable");
}

/* Keys that were renamed after files could already contain them.
 *
 * openpete writes these files itself (SAVE in the M overlay), so a rename
 * with no mapping turns the previous release's saved settings into a hard
 * "unknown key" boot failure. The bool -> vocabulary pair carries the value
 * across too: `show_fps` was two-state and `perf_stats` is three, and true
 * means the level the old key could actually select. */
typedef struct {
    const char *old_key;
    const char *new_key;
    const char *on;    /* NULL when the value needs no translation */
    const char *off;
} cfg_renamed_t;

static const cfg_renamed_t g_renamed[] = {
    { "show_fps", "perf_stats", "fps", "off" },
};

/** @brief The g_renamed row whose old key is `key`, or NULL. */
static const cfg_renamed_t *renamed_for(const char *key) {
    for (size_t i = 0; i < sizeof g_renamed / sizeof g_renamed[0]; i++)
        if (!strcmp(g_renamed[i].old_key, key)) return &g_renamed[i];
    return NULL;
}

/* Keys whose knob changed from a bool to an enum after files could hold the
 * bool: a true spelling is the enum's unset state and contributes nothing,
 * a false one maps to `off`. `fog` was the on/off distance-fog row; on meant
 * fog following the LOD mode, which is the unset fog mode. */
typedef struct {
    const char *key;
    const char *off;   /* the enum value a false spelling maps to */
} cfg_legacy_bool_t;

static const cfg_legacy_bool_t g_legacy_bool[] = {
    { "fog", "off" },
};

/** @brief Translate a former bool key's old spelling into its enum knob's vocabulary.
 *
 * For a g_legacy_bool key whose value is a TOML boolean or one of 1, 0, on,
 * off, true, false: a false value is rewritten to the row's `off` and
 * `*is_bool` cleared; a true value asks the caller to record nothing. Any
 * other key or value is left as it is.
 * @param key The bare key.
 * @param val The value as text, rewritten in place.
 * @param cap Size of `val`.
 * @param is_bool In: 1 when the value was a TOML boolean; cleared on a rewrite.
 * @param bool_val The TOML boolean's value.
 * @return 1 when the key contributes nothing (the unset state), else 0. */
static int legacy_bool_value(const char *key, char *val, size_t cap,
                             int *is_bool, int bool_val) {
    for (size_t i = 0; i < sizeof g_legacy_bool / sizeof g_legacy_bool[0]; i++) {
        if (strcmp(g_legacy_bool[i].key, key)) continue;
        int truth = -1;
        if (*is_bool) truth = bool_val;
        else if (!strcmp(val, "1") || !strcmp(val, "on") || !strcmp(val, "true")) truth = 1;
        else if (!strcmp(val, "0") || !strcmp(val, "off") || !strcmp(val, "false")) truth = 0;
        if (truth < 0) return 0;
        if (truth) return 1;
        snprintf(val, cap, "%s", g_legacy_bool[i].off);
        *is_bool = 0;
        return 0;
    }
    return 0;
}

/** @brief Truthiness of a renamed bool key's value.
 *
 * A TOML bool gives its own value; a string value is true when it is "1",
 * "on" or "true". */
static int renamed_truth(const char *val, int is_bool, int bool_val) {
    if (is_bool) return bool_val;
    return !strcmp(val, "1") || !strcmp(val, "on") || !strcmp(val, "true");
}

/** @brief Record one bare key of `tab` as a contribution at (tier, file).
 *
 * A sub-table is skipped (its caller harvests it). A renamed key is mapped
 * to its new name with a warning, and its value to the new knob's
 * vocabulary when the rename carries one. A former bool key's old value is
 * translated by legacy_bool_value, its true spelling recording nothing.
 * An unknown key (named with a
 * did_you_mean hint) is ignored with a warning at boot and is an error
 * under --config-report. A non-scalar value and a value of a TOML type the
 * config does not accept are errors printed with `where`; a non-boolean
 * value for a bool knob and an enum value outside the knob's vocabulary
 * are refused through value_refused.
 * @return 0, or -1 on an error. */
static int harvest_base_row(const toml_table_t *tab, const char *key, int tier,
                            int file, const char *origin, const char *where) {
    toml_raw_t raw = toml_raw_in(tab, key);
    if (!raw) {
        /* A sub-table ([env]/[args]/[saved]) or an array. Sub-tables are
         * harvested by the caller; anything else is a mistake worth
         * naming, because TOML also lands a stray bare key here when it
         * is written after a sub-table header. */
        if (toml_table_in(tab, key)) return 0;
        cfg_diag("openpete: config: %s: '%s' must be a scalar "
                 "(arrays are not config values)\n", where, key);
        return -1;
    }

    const cfg_renamed_t *ren = renamed_for(key);
    if (ren) {
        cfg_diag("openpete: config: %s: '%s' has been renamed to "
                 "'%s'; using it\n", where, key, ren->new_key);
        key = ren->new_key;
    }

    int ki = cfg_schema_find_id(key);
    if (ki < 0) {
        const char *hint = did_you_mean(key);
        cfg_diag("openpete: config: %s: unknown key '%s'%s%s%s%s\n",
                where, key,
                hint ? " (did you mean '" : "", hint ? hint : "",
                hint ? "'?)" : "", s_report_requested ? "" : "; ignored");
        return s_report_requested ? -1 : 0;
    }
    const cfg_knob_t *k = &cfg_schema(NULL)[ki];

    char val[CFG_VALUE_MAX];
    int is_bool = 0, bool_val = 0;
    if (toml_scalar_to_string(raw, val, sizeof val, &is_bool, &bool_val) != 0) {
        cfg_diag("openpete: config: %s: '%s' has a value this "
                 "config cannot use\n", where, key);
        return -1;
    }
    if (ren && ren->on) {
        /* Translate BEFORE the type checks below: the value in the file
         * is spelled for the old knob's type, not the new one's. */
        snprintf(val, sizeof val, "%s",
                 renamed_truth(val, is_bool, bool_val) ? ren->on : ren->off);
        is_bool = 0;
    }
    if (legacy_bool_value(key, val, sizeof val, &is_bool, bool_val)) return 0;
    if (!base_value_typed(k, val, is_bool, where, key))
        return value_refused(where, key, val);

    contrib_t *c = contrib_new();
    if (!c) return -1;
    c->knob = ki;
    c->rank = RANK(tier, file, SP_BASE);
    c->spelling = SP_BASE;
    c->has_value = 1;
    snprintf(c->key, sizeof c->key, "%s", key);
    snprintf(c->value, sizeof c->value, "%s", val);
    snprintf(c->origin, sizeof c->origin, "%s", origin);
    return 0;
}

/** @brief Record the bare keys of `tab`, the validated spelling, as contributions at (tier, file).
 *
 * Each key goes through harvest_base_row; `skip_directives` skips
 * `portable`. Returns 0, or -1 on the first error. */
static int harvest_base(const toml_table_t *tab, int tier, int file,
                        const char *origin, const char *where,
                        int skip_directives) {
    for (int i = 0;; i++) {
        const char *key = toml_key_in(tab, i);
        if (!key) break;
        if (skip_directives && is_directive_key(key)) continue;
        if (harvest_base_row(tab, key, tier, file, origin, where) != 0) return -1;
    }
    return 0;
}

/** @brief Record the rows of a [<scope>.env] table, which materialize as environment variables.
 *
 * Free-form: a variable needs no schema row, so the debug OPENPETE_* vars
 * can be set from a file; a row whose variable a schema knob declares
 * carries that knob. A name without the OPENPETE_ prefix is warned about
 * and kept. A non-scalar value or a value of a TOML type the config does
 * not accept is an error. Returns 0, or -1 on the first error. */
static int harvest_env(const toml_table_t *tab, int tier, int file,
                       const char *origin, const char *where) {
    for (int i = 0;; i++) {
        const char *key = toml_key_in(tab, i);
        if (!key) break;
        toml_raw_t raw = toml_raw_in(tab, key);
        if (!raw) {
            cfg_diag("openpete: config: %s: [env] '%s' must be a scalar\n",
                    where, key);
            return -1;
        }
        char val[CFG_VALUE_MAX];
        int is_bool = 0, bool_val = 0;
        if (toml_scalar_to_string(raw, val, sizeof val, &is_bool, &bool_val) != 0) {
            cfg_diag("openpete: config: %s: [env] '%s' has a value this "
                     "config cannot use\n", where, key);
            return -1;
        }
        if (strncmp(key, "OPENPETE_", 9) != 0)
            cfg_diag("openpete: config: %s: [env] '%s' is not an "
                     "OPENPETE_* variable (applied anyway)\n", where, key);

        contrib_t *c = contrib_new();
        if (!c) return -1;
        c->knob = cfg_schema_find_env(key);
        c->rank = RANK(tier, file, SP_ENV);
        c->spelling = SP_ENV;
        c->has_value = 1;
        snprintf(c->key, sizeof c->key, "%s", key);
        snprintf(c->value, sizeof c->value, "%s", val);
        snprintf(c->origin, sizeof c->origin, "%s.env", origin);
    }
    return 0;
}

/** @brief Record one row of a [<scope>.args] table, whose keys are flags verbatim.
 *
 * A key must start with "--". On an unregistered or valueless flag a
 * boolean value means the flag takes none: true emits it, false suppresses
 * whatever a lower tier asked for (the only way to un-set an inherited
 * valueless flag). On a schema alias flag ("--no-lod") true stands for the
 * alias's fixed value and false suppresses. On any other schema flag a
 * boolean is the 0/1 the flag expects, and an enum value outside the knob's
 * vocabulary is refused through value_refused. A key that is not a flag is
 * refused through row_refused. Returns 0, or -1 on an error. */
static int harvest_args_row(const toml_table_t *tab, const char *key, int tier,
                            int file, const char *origin, const char *where) {
    toml_raw_t raw = toml_raw_in(tab, key);
    if (!raw) {
        cfg_diag("openpete: config: %s: [args] '%s' must be a scalar\n",
                where, key);
        return -1;
    }
    if (strncmp(key, "--", 2) != 0) {
        cfg_diag("openpete: config: %s: [args] key '%s' is not a flag "
                 "(write it exactly as on the command line, e.g. "
                 "\"--lod\")\n", where, key);
        return row_refused(where, "args", key);
    }
    char val[CFG_VALUE_MAX];
    int is_bool = 0, bool_val = 0;
    if (toml_scalar_to_string(raw, val, sizeof val, &is_bool, &bool_val) != 0) {
        cfg_diag("openpete: config: %s: [args] '%s' has a value this "
                 "config cannot use\n", where, key);
        return -1;
    }

    int ki = cfg_schema_find_cli(key);
    const cfg_knob_t *k = (ki >= 0) ? &cfg_schema(NULL)[ki] : NULL;

    contrib_t *c = contrib_new();
    if (!c) return -1;
    c->knob = ki;
    c->rank = RANK(tier, file, SP_ARGS);
    c->spelling = SP_ARGS;
    snprintf(c->key, sizeof c->key, "%s", key);
    snprintf(c->origin, sizeof c->origin, "%s.args", origin);

    if (k && k->alias && !strcmp(k->alias, key)) {
        /* "--no-lod" = true means the alias's fixed value. */
        if (is_bool && !bool_val) { c->suppress = 1; return 0; }
        c->has_value = 1;
        snprintf(c->value, sizeof c->value, "%s", k->alias_value);
        return 0;
    }
    if (is_bool && (!k || k->flag_valueless)) {
        if (!bool_val) { c->suppress = 1; return 0; }
        c->valueless = 1;
        return 0;
    }
    if (k && !enum_ok(k, val)) {
        char vocab[128];
        enum_vocab_str(k, vocab, sizeof vocab);
        cfg_diag("openpete: config: %s: [args] '%s' expects %s, got: %s\n",
                where, key, vocab, val);
        s_contrib_n--;   /* drop the refused contribution, the last one added */
        return value_refused(where, key, val);
    }
    c->has_value = 1;
    snprintf(c->value, sizeof c->value, "%s", val);
    return 0;
}

/** @brief Record every row of an [args] table through harvest_args_row.
 *
 * Returns 0, or -1 on the first error. */
static int harvest_args(const toml_table_t *tab, int tier, int file,
                        const char *origin, const char *where) {
    for (int i = 0;; i++) {
        const char *key = toml_key_in(tab, i);
        if (!key) break;
        if (harvest_args_row(tab, key, tier, file, origin, where) != 0) return -1;
    }
    return 0;
}

/** @brief row_refused for a row of the [keys.<domain>] table. */
static int keys_row_refused(const char *where, const char *domain, const char *key) {
    char table[32];
    snprintf(table, sizeof table, "keys.%s", domain);
    return row_refused(where, table, key);
}

/** @brief Forward one row of a [<scope>.keys.<domain>] table to the keymap at `rank`.
 *
 * Input bindings. Unlike knobs, these do not
 * flow through the contribution table: the keymap owns its own layered
 * store (it merges per ACTION and replaces within one, which the knob
 * resolver's single-winner-per-key model cannot express), so this walk
 * just forwards each row with the scope's rank and lets keymap.c decide.
 *
 * A value is a string or an array of strings; arrays are legal here only
 * (harvest_base rejects them). An array longer than 2 * KM_MAX_SOURCES is
 * truncated; an empty array means "none". A row the keymap refuses (an
 * unknown action, which it names with a did_you_mean hint, or a binding it
 * cannot parse) goes through keys_row_refused. Returns 0, or -1 on a
 * malformed row or a refusal under --config-report. */
static int harvest_keys_row(const toml_table_t *tab, const char *domain,
                            const char *key, int rank, const char *origin,
                            const char *where) {
    const toml_array_t *arr = toml_array_in(tab, key);
    if (arr) {
        const char *toks[KM_MAX_SOURCES * 2];
        char bufs[KM_MAX_SOURCES * 2][CFG_KEY_MAX];
        int n = toml_array_nelem(arr);
        if (n > (int)(sizeof toks / sizeof toks[0]))
            n = (int)(sizeof toks / sizeof toks[0]);
        for (int j = 0; j < n; j++) {
            toml_datum_t d = toml_string_at(arr, j);
            if (!d.ok) {
                cfg_diag("openpete: config: %s: [keys.%s] '%s' "
                         "array must hold strings\n",
                        where, domain, key);
                return -1;
            }
            snprintf(bufs[j], CFG_KEY_MAX, "%s", d.u.s);
            free(d.u.s);
            toks[j] = bufs[j];
        }
        /* n == 0 is an empty array: the same statement as "none". */
        if (openpete_keymap_set(domain, key, toks, n, rank, origin,
                                where) != 0 &&
            keys_row_refused(where, domain, key) != 0)
            return -1;
        return 0;
    }

    toml_raw_t raw = toml_raw_in(tab, key);
    if (!raw) {
        cfg_diag("openpete: config: %s: [keys.%s] '%s' must be a "
                 "string or an array of strings\n",
                where, domain, key);
        return -1;
    }
    char val[CFG_VALUE_MAX];
    int is_bool = 0, bool_val = 0;
    if (toml_scalar_to_string(raw, val, sizeof val, &is_bool, &bool_val) != 0) {
        cfg_diag("openpete: config: %s: [keys.%s] '%s' has a value "
                 "this config cannot use\n", where, domain, key);
        return -1;
    }
    const char *tok = val;
    if (openpete_keymap_set(domain, key, &tok, 1, rank, origin, where) != 0 &&
        keys_row_refused(where, domain, key) != 0)
        return -1;
    return 0;
}

/** @brief Forward every row of a [<scope>.keys.<domain>] table through harvest_keys_row.
 *
 * Returns 0, or -1 on the first error. */
static int harvest_keys_domain(const toml_table_t *tab, const char *domain,
                               int rank, const char *origin,
                               const char *where) {
    for (int i = 0;; i++) {
        const char *key = toml_key_in(tab, i);
        if (!key) break;
        if (harvest_keys_row(tab, domain, key, rank, origin, where) != 0) return -1;
    }
    return 0;
}

/** @brief Forward the [keys.mod.<id>] tables to openpete_keymap_set_mod by name, without validation.
 *
 * A mod entry that is not a table, or a binding that is not a scalar string,
 * is an error printed with `where`. Returns 0, or -1 on the first error. */
static int harvest_keys_mods(const toml_table_t *keys, int rank, const char *where) {
    const toml_table_t *mods = toml_table_in(keys, "mod");
    if (!mods) return 0;
    for (int i = 0;; i++) {
        const char *mod_id = toml_key_in(mods, i);
        if (!mod_id) break;
        const toml_table_t *mt = toml_table_in(mods, mod_id);
        if (!mt) {
            cfg_diag("openpete: config: %s: [keys.mod] '%s' must be "
                     "a table of binding names\n", where, mod_id);
            return -1;
        }
        for (int j = 0;; j++) {
            const char *name = toml_key_in(mt, j);
            if (!name) break;
            toml_raw_t raw = toml_raw_in(mt, name);
            char val[CFG_VALUE_MAX];
            int is_bool = 0, bool_val = 0;
            if (!raw ||
                toml_scalar_to_string(raw, val, sizeof val,
                                      &is_bool, &bool_val) != 0) {
                cfg_diag("openpete: config: %s: [keys.mod.%s] '%s' "
                         "must be a string\n", where, mod_id, name);
                return -1;
            }
            const char *tok = val;
            if (openpete_keymap_set_mod(mod_id, name, &tok, 1, rank,
                                        where) != 0)
                return -1;
        }
    }
    return 0;
}

/** @brief Name every sub-table of `keys` outside pad, pad2..pad4, host and mod, refusing each through row_refused.
 *
 * Returns 0, or -1 when a refusal fails under --config-report. */
static int keys_unknown_tables(const toml_table_t *keys, const char *where) {
    /* Anything else under [keys] is a typo. Silence here is how the
     * scoping got lost for [args]/[env] in the docs - name it instead. */
    for (int i = 0;; i++) {
        const char *key = toml_key_in(keys, i);
        if (!key) break;
        if (!strcmp(key, "pad") || !strcmp(key, "pad2") ||
            !strcmp(key, "pad3") || !strcmp(key, "pad4") ||
            !strcmp(key, "host") || !strcmp(key, "mod")) continue;
        cfg_diag("openpete: config: %s: unknown [keys.%s] - expected "
                 "pad, pad2, pad3, pad4, host or mod.<id>\n", where, key);
        if (row_refused(where, "keys", key) != 0) return -1;
    }
    return 0;
}

/** @brief Record the bindings of a [keys] table: the pad, pad2, pad3, pad4 and host domains and the [keys.mod.<id>] tables.
 *
 * Each binding is handed to the keymap at rank RANK(tier, file, SP_BASE), since
 * [keys] has no [env] or [args] twin. A domain binding goes through
 * openpete_keymap_set, which validates it; a [keys.mod.<id>] binding goes
 * through openpete_keymap_set_mod by name without validation, because mods are
 * discovered after the config is read. A binding value must be a scalar. Any
 * other sub-table of `keys` is named with the accepted set and refused
 * through row_refused. Errors print
 * to stderr with `where`. Returns 0, or -1 on the first error. */
static int harvest_keys(const toml_table_t *keys, int tier, int file,
                        const char *origin, const char *where) {
    /* No consumer reads a binding before the game is identified, so the
     * game-neutral stage leaves the keymap to the full stage. */
    if (s_neutral) return 0;
    /* Keys have no [env]/[args] twin, so the spelling sub-sort never
     * applies and every binding ranks at SP_BASE for its scope. */
    const int rank = RANK(tier, file, SP_BASE);

    static const char *const domains[] = { "pad", "pad2", "pad3", "pad4", "host" };
    for (size_t d = 0; d < sizeof domains / sizeof domains[0]; d++) {
        const toml_table_t *tab = toml_table_in(keys, domains[d]);
        if (tab && harvest_keys_domain(tab, domains[d], rank, origin, where) != 0)
            return -1;
    }

    /* [keys.mod.<id>] - one more level, and the ids are not knowable
     * here (mods are discovered after the config is read), so these are
     * forwarded by name without validation. */
    if (harvest_keys_mods(keys, rank, where) != 0) return -1;
    return keys_unknown_tables(keys, where);
}

/** @brief Harvest one named scope table: its bare keys, then its [env], [args] and [keys] sub-tables.
 *
 * Serves [game.<id>], [disc.<serial>] and [disc.<sha>] through
 * harvest_named. The global scope (harvest_global) repeats the same four
 * walks with the `portable` directive skipped; [saved] (harvest_saved)
 * harvests only its bare keys and [keys]. A sub-table added here is
 * therefore added to harvest_global too. `origin` labels each
 * contribution and error. Returns 0, or -1 on the first error. */
static int harvest_scope(const toml_table_t *tab, int tier, int file,
                         const char *origin) {
    char where[CFG_PATH_MAX + 64];
    snprintf(where, sizeof where, "%s [%s]", s_file_path[file], origin);

    if (harvest_base(tab, tier, file, origin, where, 0) != 0) return -1;

    const toml_table_t *env = toml_table_in(tab, "env");
    if (env && harvest_env(env, tier, file, origin, where) != 0) return -1;

    const toml_table_t *args = toml_table_in(tab, "args");
    if (args && harvest_args(args, tier, file, origin, where) != 0) return -1;

    const toml_table_t *keys = toml_table_in(tab, "keys");
    if (keys && harvest_keys(keys, tier, file, origin, where) != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* file loading                                                       */
/* ------------------------------------------------------------------ */

static toml_table_t *s_doc[2];

/** @brief Parse config file number `file` into s_doc and apply its `portable` directive.
 *
 * A file that cannot be opened is the normal absent case. On success marks the
 * file present and, when the file carries a boolean top-level `portable`, sets
 * the path layout with openpete_paths_set_portable. Returns 1 when loaded, 0
 * when absent, -1 after printing the parse error to stderr. */
static int load_toml(int file) {
    const char *path = s_file_path[file];
    FILE *f = fopen(path, "r");
    if (!f) return 0;                       /* absent is the normal case */
    char err[256];
    toml_table_t *doc = toml_parse_file(f, err, sizeof err);
    fclose(f);
    if (!doc) {
        cfg_diag("openpete: config: %s: %s\n", path, err);
        return -1;
    }
    s_doc[file] = doc;
    s_file_present[file] = 1;

    /* A portable install keeps everything it writes - this file's [saved]
     * zone, memory cards, savestates - next to the exe. Files load in
     * ascending precedence, so a user-dir directive beats the shipped one,
     * matching how the rest of the layering resolves. */
    toml_raw_t p = toml_raw_in(doc, "portable");
    int b = 0;
    if (p && toml_rtob(p, &b) == 0) openpete_paths_set_portable(b);
    return 1;
}

/** @brief Harvest the global scope of loaded file `file`: its top-level bare keys and [env], [args] and [keys].
 *
 * The `portable` directive is skipped; sub-tables are not bare keys.
 * Returns 0 when the file is not loaded or harvests cleanly, -1 on the
 * first error. */
static int harvest_global(int file) {
    toml_table_t *doc = s_doc[file];
    if (!doc) return 0;

    char where[CFG_PATH_MAX + 32];
    snprintf(where, sizeof where, "%s (global)", s_file_path[file]);

    if (harvest_base(doc, TIER_GLOBAL, file, "global", where, 1) != 0) return -1;

    const toml_table_t *env = toml_table_in(doc, "env");
    if (env && harvest_env(env, TIER_GLOBAL, file, "global", where) != 0) return -1;
    const toml_table_t *args = toml_table_in(doc, "args");
    if (args && harvest_args(args, TIER_GLOBAL, file, "global", where) != 0) return -1;
    const toml_table_t *keys = toml_table_in(doc, "keys");
    if (keys && harvest_keys(keys, TIER_GLOBAL, file, "global", where) != 0) return -1;
    return 0;
}

/** @brief Harvest the [<ns>.<name>] scope table of file `file` at `tier`.
 *
 * The contribution origin is `<origin_prefix>.<name>`. Returns 0 when the file
 * is not loaded, `name` is NULL or empty, or the table is absent; otherwise the
 * result of harvest_scope (0, or -1 on the first error). */
static int harvest_named(int file, const char *ns, const char *name,
                         int tier, const char *origin_prefix) {
    toml_table_t *doc = s_doc[file];
    if (!doc || !name || !*name) return 0;
    const toml_table_t *nstab = toml_table_in(doc, ns);
    if (!nstab) return 0;
    const toml_table_t *tab = toml_table_in(nstab, name);
    if (!tab) return 0;
    char origin[CFG_ORIGIN_MAX];
    snprintf(origin, sizeof origin, "%s.%s", origin_prefix, name);
    return harvest_scope(tab, tier, file, origin);
}

/** @brief Harvest the [disc.<sha>] scope of file `file` that matches the running image's hash.
 *
 * The key may be any prefix of the hash of at least 8 characters, git
 * style; a shorter key is a serial and is skipped. Two matching keys are
 * an error: picking one would make the config mean different things as
 * the library grows. Returns 0 when nothing matches or the match harvests
 * cleanly, -1 on an ambiguity or a harvest error. */
static int harvest_disc_sha(int file) {
    toml_table_t *doc = s_doc[file];
    if (!doc || !s_scope_sha[0]) return 0;
    const toml_table_t *nstab = toml_table_in(doc, "disc");
    if (!nstab) return 0;

    const char *match = NULL;
    for (int i = 0;; i++) {
        const char *key = toml_key_in(nstab, i);
        if (!key) break;
        size_t klen = strlen(key);
        if (klen < 8) continue;                       /* a serial, not a hash */
        if (strncmp(key, s_scope_sha, klen) != 0) continue;
        if (match) {
            cfg_diag("openpete: config: %s: [disc.%s] and [disc.%s] both "
                     "match the running image - use a longer prefix\n",
                    s_file_path[file], match, key);
            return -1;
        }
        match = key;
    }
    if (!match) return 0;
    return harvest_named(file, "disc", match, TIER_DISC_SHA, "disc");
}

/** @brief Find the in-memory [saved.keys] row of (domain, action).
 *
 * @return its index, or -1. */
static int saved_key_find(const char *domain, const char *action) {
    for (int i = 0; i < s_saved_keys_n; i++)
        if (!strcmp(s_saved_keys[i].domain, domain) &&
            !strcmp(s_saved_keys[i].action, action))
            return i;
    return -1;
}

/** @brief Store a [saved.keys] row in memory, replacing an existing row of the same (domain, action).
 *
 * @param domain "host", "pad".."pad4" or "mod.<id>".
 * @param action the action id or mod binding name.
 * @param toks the binding tokens; at most CFG_SAVED_KEY_TOKS are kept.
 * @param n their count; 0 stores an unbinding row.
 * @return 0, or -1 for a full table or an over-long name. */
static int saved_key_put(const char *domain, const char *action,
                         const char *const *toks, int n) {
    if (strlen(domain) >= CFG_KEY_MAX || strlen(action) >= CFG_KEY_MAX) return -1;
    int i = saved_key_find(domain, action);
    if (i < 0) {
        if (s_saved_keys_n >= CFG_MAX_SAVED_KEYS) return -1;
        i = s_saved_keys_n++;
        snprintf(s_saved_keys[i].domain, CFG_KEY_MAX, "%s", domain);
        snprintf(s_saved_keys[i].action, CFG_KEY_MAX, "%s", action);
    }
    saved_key_t *k = &s_saved_keys[i];
    k->n = 0;
    for (int j = 0; j < n && k->n < CFG_SAVED_KEY_TOKS; j++)
        snprintf(k->tok[k->n++], CFG_KEY_MAX, "%s", toks[j] ? toks[j] : "");
    return 0;
}

/** @brief Copy every row of one [saved.keys.<domain>] table into memory: a string or an array of strings. */
static void saved_keys_copy_table(const toml_table_t *tab, const char *domain) {
    for (int i = 0;; i++) {
        const char *key = toml_key_in(tab, i);
        if (!key) break;
        const char *toks[CFG_SAVED_KEY_TOKS];
        char bufs[CFG_SAVED_KEY_TOKS][CFG_KEY_MAX];
        int n = 0;
        const toml_array_t *arr = toml_array_in(tab, key);
        if (arr) {
            const int m = toml_array_nelem(arr);
            for (int j = 0; j < m && n < CFG_SAVED_KEY_TOKS; j++) {
                toml_datum_t d = toml_string_at(arr, j);
                if (!d.ok) continue;
                const int len = snprintf(bufs[n], CFG_KEY_MAX, "%s", d.u.s);
                free(d.u.s);
                if (len >= CFG_KEY_MAX) continue;   /* no key name is this long */
                toks[n] = bufs[n];
                n++;
            }
        } else {
            toml_raw_t raw = toml_raw_in(tab, key);
            char val[CFG_VALUE_MAX];
            int ib = 0, bv = 0;
            if (!raw || toml_scalar_to_string(raw, val, sizeof val, &ib, &bv) != 0)
                continue;
            if (snprintf(bufs[0], CFG_KEY_MAX, "%s", val) >= CFG_KEY_MAX)
                continue;   /* no key name is this long */
            toks[0] = bufs[0];
            n = 1;
        }
        (void)saved_key_put(domain, key, toks, n);
    }
}

/** @brief Copy the write target's [saved.keys.*] rows into memory, the mod.<id> tables included. */
static void saved_keys_copy(const toml_table_t *keys) {
    static const char *const domains[] = { "pad", "pad2", "pad3", "pad4", "host" };
    for (size_t d = 0; d < sizeof domains / sizeof domains[0]; d++) {
        const toml_table_t *tab = toml_table_in(keys, domains[d]);
        if (tab) saved_keys_copy_table(tab, domains[d]);
    }
    const toml_table_t *mods = toml_table_in(keys, "mod");
    for (int i = 0; mods; i++) {
        const char *id = toml_key_in(mods, i);
        if (!id) break;
        const toml_table_t *mt = toml_table_in(mods, id);
        char dom[CFG_KEY_MAX];
        snprintf(dom, sizeof dom, "mod.%s", id);
        if (mt) saved_keys_copy_table(mt, dom);
    }
}

/** @brief Harvest the managed [saved] zone of file `file`: its bare keys and [saved.keys.*].
 *
 * Read like any other layer; written by the menus through
 * openpete_config_save_flush. From the write target the zone's scalars are
 * also copied into the in-memory saved table, so a later save rewrites the
 * whole zone. A [saved] zone in a file ranked above the write target is
 * warned about. Returns 0, or -1 on a harvest error. */
static int harvest_saved(int file) {
    toml_table_t *doc = s_doc[file];
    if (!doc) return 0;
    const toml_table_t *tab = toml_table_in(doc, "saved");
    if (!tab) return 0;

    char where[CFG_PATH_MAX + 32];
    snprintf(where, sizeof where, "%s [saved]", s_file_path[file]);
    if (harvest_base(tab, TIER_SAVED, file, "saved", where, 0) != 0) return -1;

    /* [saved.keys.*] reads like any other scope, so the ladder has no
     * hole at its top. The binding page writes it through the settings
     * writer (openpete_config_save_binding); the write target's rows are
     * copied below so a flush rewrites every row it did not edit. Anything
     * hand-written below the managed marker is regenerated on the next
     * save, so a hand-edited binding belongs in a normal scope (global,
     * [game.*], [disc.*]). */
    const toml_table_t *keys = toml_table_in(tab, "keys");
    if (keys && harvest_keys(keys, TIER_SAVED, file, "saved", where) != 0)
        return -1;
    if (keys && file == s_write_file) saved_keys_copy(keys);

    /* A managed zone in a file that outranks the write target would beat
     * everything the menus save from here on - the settings screen would
     * appear to do nothing. Ranks are fixed by file (user-dir beats
     * exe-dir), so this can only happen when the layout puts the target in
     * the lower-ranked file; say so rather than let saves vanish. */
    if (file > s_write_file)
        cfg_diag("openpete: config: %s has a [saved] section that "
                 "outranks the save target %s - settings saved from "
                 "the menus will not take effect until it is removed\n",
                s_file_path[file], s_file_path[s_write_file]);

    /* Keep a copy so a later save rewrites the whole zone rather than
     * dropping the knobs this session didn't touch. */
    if (file == s_write_file) {
        for (int i = 0;; i++) {
            const char *key = toml_key_in(tab, i);
            if (!key) break;
            toml_raw_t raw = toml_raw_in(tab, key);
            if (!raw || s_saved_n >= CFG_MAX_SAVED) continue;
            char val[CFG_VALUE_MAX];
            int ib = 0, bv = 0;
            if (toml_scalar_to_string(raw, val, sizeof val, &ib, &bv) != 0) continue;
            snprintf(s_saved[s_saved_n].id, CFG_KEY_MAX, "%s", key);
            snprintf(s_saved[s_saved_n].value, CFG_VALUE_MAX, "%s", val);
            s_saved_n++;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* argv tokenizing                                                    */
/* ------------------------------------------------------------------ */

/** @brief Whether `t` is non-NULL and starts with "--".
 *
 * Every openpete flag starts with "--", so a following token that does not
 * is the flag's value. Negative numbers ("--bk2-frame-offset -100") stay
 * values, which an any-leading-dash rule would break. */
static int token_is_flag(const char *t) { return t && t[0] == '-' && t[1] == '-'; }

/* ------------------------------------------------------------------ */
/* retired sidecars                                                   */
/* ------------------------------------------------------------------ */

/** @brief Print a stderr notice for each openpete.opts or openpete.env in `dir`.
 *
 * Those files (an argv splice and a setenv pass) are not read; the notice
 * names each one present and points at openpete.toml. */
static void warn_retired_sidecars(const char *dir) {
    static const char *const names[] = { "openpete.opts", "openpete.env" };
    char path[CFG_PATH_MAX];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        openpete_paths_join(path, sizeof path, dir, names[i]);
        if (!file_exists(path)) continue;
        cfg_diag("openpete: config: %s is no longer read; put these "
                 "settings in openpete.toml\n", path);
    }
}

/* ------------------------------------------------------------------ */
/* launch tiers                                                       */
/* ------------------------------------------------------------------ */

/** @brief Record every "--" token in argv as a launch-tier contribution.
 *
 * Tokens at `typed_from` and later rank as TIER_CLI (origin "cli"), earlier
 * ones as TIER_SIDECAR ("bk2 sidecar"). The contribution's value is the
 * next token when that is not a flag, else "". These tiers outrank every
 * file tier and are already in argv, so the contribution only shadows the
 * files' copies of the same setting. */
static void scan_launch(int argc, char **argv, int typed_from) {
    for (int i = 1; i < argc; i++) {
        if (!token_is_flag(argv[i])) continue;
        int ki = cfg_schema_find_cli(argv[i]);
        int tier = (i >= typed_from) ? TIER_CLI : TIER_SIDECAR;

        contrib_t *c = contrib_new();
        if (!c) return;
        c->knob = ki;
        c->rank = RANK(tier, 0, SP_ARGS);
        c->spelling = SP_ARGS;
        snprintf(c->key, sizeof c->key, "%s", argv[i]);
        snprintf(c->origin, sizeof c->origin, "%s",
                 tier == TIER_CLI ? "cli" : "bk2 sidecar");
        /* The value is irrelevant: argv already carries it. */
        c->has_value = 1;
        snprintf(c->value, sizeof c->value, "%s",
                 (i + 1 < argc && !token_is_flag(argv[i + 1])) ? argv[i + 1] : "");
    }
}

/** @brief Record one set env var as a TIER_REAL_ENV contribution of schema knob `knob` (-1 for none).
 *
 * @param knob The schema index, or -1.
 * @param var The variable.
 * @param v Its value. */
static void real_env_contrib(int knob, const char *var, const char *v) {
    contrib_t *c = contrib_new();
    if (!c) return;
    c->knob = knob;
    c->rank = RANK(TIER_REAL_ENV, 0, SP_ENV);
    c->spelling = SP_ENV;
    c->has_value = 1;
    snprintf(c->key, sizeof c->key, "%s", var);
    snprintf(c->value, sizeof c->value, "%s", v);
    snprintf(c->origin, sizeof c->origin, "env");
}

/** @brief Record every set schema env var and every set device-creation env var as a TIER_REAL_ENV contribution.
 *
 * Runs before any [env] tier is applied, or the files' own variables would
 * read back as exported by the user. A device-creation variable with no
 * schema row is recorded so openpete_config_device_value sees the real
 * environment without a getenv of its own. */
static void scan_real_env(void) {
    size_t n;
    const cfg_knob_t *sch = cfg_schema(&n);
    for (size_t i = 0; i < n; i++) {
        if (!sch[i].env) continue;
        const char *v = getenv(sch[i].env);
        if (v) real_env_contrib((int)i, sch[i].env, v);
    }
    size_t dn;
    const char *const *dev = cfg_schema_device_spellings(&dn);
    for (size_t i = 0; i < dn; i++) {
        if (strncmp(dev[i], "OPENPETE_", 9) != 0) continue;
        if (cfg_schema_find_env(dev[i]) >= 0) continue;   /* recorded above */
        const char *v = getenv(dev[i]);
        if (v) real_env_contrib(-1, dev[i], v);
    }
}

/* ------------------------------------------------------------------ */
/* materialization                                                    */
/* ------------------------------------------------------------------ */

/** @brief Set env var `k` to `v` (NULL as "") only when `k` is unset.
 *
 * The real environment wins over a config-file value. Returns the
 * setenv/_putenv_s result, or 0 when `k` was already set on Windows. */
static int portable_setenv_soft(const char *k, const char *v) {
#ifdef _WIN32
    if (getenv(k)) return 0;              /* the real environment wins */
    return _putenv_s(k, v ? v : "");
#else
    return setenv(k, v ? v : "", 0);
#endif
}

/** @brief Materialize one env winner: setenv in the full stage, the pending table in the game-neutral stage.
 *
 * Both keep the soft rule: a variable the environment already carries is
 * not replaced. A full pending table drops the variable with a diagnostic.
 * @param k The variable.
 * @param v Its value. */
static void cfg_setenv(const char *k, const char *v) {
    if (!s_neutral) { (void)portable_setenv_soft(k, v); return; }
    if (getenv(k)) return;
    for (int i = 0; i < s_pending_env_n; i++)
        if (!strcmp(s_pending_env[i].key, k)) {
            snprintf(s_pending_env[i].value, sizeof s_pending_env[i].value, "%s", v ? v : "");
            return;
        }
    if (s_pending_env_n >= CFG_MAX_PENDING_ENV) {
        cfg_diag("openpete: config: too many [env] entries (max %d)\n", CFG_MAX_PENDING_ENV);
        return;
    }
    pending_env_t *e = &s_pending_env[s_pending_env_n++];
    snprintf(e->key, sizeof e->key, "%s", k);
    snprintf(e->value, sizeof e->value, "%s", v ? v : "");
}

/** @brief Copy `s` into a new malloc block, or return NULL when the allocation fails.
 *
 * The copy is never freed: emitted tokens live for the process. */
static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;                              /* retained for process lifetime */
}

/** @brief Append one spliced argv token, remembering its origin label.
 *
 * Copies `tok` with dup_str and stores `origin` beside it for
 * openpete_config_token_source. Once CFG_MAX_TOKENS tokens exist, or when the
 * copy fails, the token is dropped (the limit case prints to stderr) and
 * nothing reports the loss to the caller. */
static void emit_token(const char *tok, const char *origin) {
    if (s_token_n >= CFG_MAX_TOKENS) {
        cfg_diag("openpete: config: too many flags to splice (max %d)\n",
                CFG_MAX_TOKENS);
        return;
    }
    char *d = dup_str(tok);
    if (!d) return;
    snprintf(s_token_origin[s_token_n], CFG_ORIGIN_MAX, "%s", origin);
    s_tokens[s_token_n++] = d;
}

/** @brief The decision record of schema knob `knob`, created zeroed on first use.
 *
 * Returns NULL when the table already holds every slot of s_decision. */
static decision_t *decision_for(int knob) {
    for (int i = 0; i < s_decision_n; i++)
        if (s_decision[i].knob == knob) return &s_decision[i];
    if (s_decision_n >= (int)(sizeof s_decision / sizeof s_decision[0]))
        return NULL;
    decision_t *d = &s_decision[s_decision_n++];
    memset(d, 0, sizeof *d);
    d->knob = knob;
    return d;
}

/** @brief Whether a higher-ranked contribution supplies the same setting as `c`.
 *
 * An equal rank does not shadow. Equal ranks arise from two spellings of
 * one knob in one table (an alias flag beside its flag, a renamed key
 * beside its new name) and from a flag repeated in argv. */
static int is_shadowed(const contrib_t *c) {
    for (int i = 0; i < s_contrib_n; i++) {
        const contrib_t *o = &s_contrib[i];
        if (o == c) continue;
        if (!same_setting(c, o)) continue;
        if (o->rank > c->rank) return 1;
    }
    return 0;
}

/** @brief Whether an unregistered [args] flag stands down for a flag it cannot coexist with.
 *
 * True when a higher-ranked, unsuppressed flag of the same
 * cfg_schema_mutex_group is present. Flags with a schema row resolve
 * through same_setting instead and always return 0 here, as do other
 * spellings. */
static int mutex_blocked(const contrib_t *c) {
    if (c->spelling != SP_ARGS || c->knob >= 0) return 0;
    const char *const *grp = cfg_schema_mutex_group(c->key);
    if (!grp) return 0;
    for (int g = 0; grp[g]; g++) {
        if (!strcmp(grp[g], c->key)) continue;
        for (int i = 0; i < s_contrib_n; i++) {
            const contrib_t *o = &s_contrib[i];
            if (o->spelling == SP_ARGS && !strcmp(o->key, grp[g]) &&
                o->rank > c->rank && !o->suppress)
                return 1;
        }
    }
    return 0;
}

/** @brief Store the winning contribution `win` as its knob's decision record.
 *
 * Does nothing for a contribution without a schema knob. Copies the value and
 * origin, marks the record pinned when `win` ranks at the real-environment tier
 * or above (keeping the spelling: `$VAR` for an env winner, else the key), and
 * appends `origin(value)` of every lower-ranked contribution of the same
 * setting to the shadow list while 32 bytes of room remain, counting each in
 * `shadowed`. A second winner of equal rank overwrites the value and origin,
 * and appends the same lower contributions again. */
static void record_decision(const contrib_t *win) {
    if (win->knob < 0) return;
    decision_t *d = decision_for(win->knob);
    if (!d) return;
    snprintf(d->value, sizeof d->value, "%s", win->value);
    snprintf(d->origin, sizeof d->origin, "%s", win->origin);
    d->pinned = (win->rank >= RANK(TIER_REAL_ENV, 0, 0));
    if (d->pinned) {
        if (win->spelling == SP_ENV)
            snprintf(d->pin, sizeof d->pin, "$%s", win->key);
        else
            snprintf(d->pin, sizeof d->pin, "%s", win->key);
    }
    for (int i = 0; i < s_contrib_n; i++) {
        const contrib_t *o = &s_contrib[i];
        if (o == win || !same_setting(win, o) || o->rank >= win->rank) continue;
        d->shadowed++;
        size_t used = strlen(d->shadow);
        if (used + 32 < sizeof d->shadow)
            snprintf(d->shadow + used, sizeof d->shadow - used, "%s%s(%s)",
                     used ? ", " : "", o->origin, o->value);
    }
}

/** @brief The flattening pass: materialize each unshadowed contribution in its own spelling.
 *
 * An [env] winner becomes a setenv, a bare key or an [args] winner becomes
 * argv tokens, because the two vocabularies are not interchangeable
 * (OPENPETE_PRESENT_MODE says "immediate", --vsync says 0). Exactly one
 * spelling per setting reaches the parser, so no consumer's own
 * CLI-beats-env rule can re-order the tiers. Every unshadowed schema
 * contribution records its decision. Launch-tier winners are already in
 * argv or the environment and emit nothing, nor do suppressed and
 * mutex-blocked flags. A bare key of an env-only knob is set as its
 * variable, and a false bool is left unset because some consumers test
 * presence only. setenv never replaces a variable the environment already
 * carries. In the game-neutral stage the variables go to the pending table
 * (cfg_setenv) and the process environment is not written. */
static void materialize(void) {
    for (int i = 0; i < s_contrib_n; i++) {
        contrib_t *c = &s_contrib[i];
        if (is_shadowed(c)) continue;

        record_decision(c);

        /* Launch tiers are already in argv / the environment. */
        if (c->rank >= RANK(TIER_REAL_ENV, 0, 0)) continue;
        if (c->suppress) continue;
        if (mutex_blocked(c)) continue;

        const cfg_knob_t *k = (c->knob >= 0) ? &cfg_schema(NULL)[c->knob] : NULL;

        if (c->spelling == SP_ENV) {
            cfg_setenv(c->key, c->value);
            continue;
        }

        /* A bare key for an env-only knob has to go out as its variable.
         * False becomes an ABSENT variable rather than "0": several of
         * these consumers only test for presence. */
        if (k && !k->cli) {
            if (k->type == CFG_BOOL &&
                (!strcmp(c->value, "0") || !strcmp(c->value, "off")))
                continue;
            cfg_setenv(k->env, c->value);
            continue;
        }

        const char *flag = k ? k->cli : c->key;
        if (c->valueless) {
            emit_token(flag, c->origin);
        } else if (k && k->flag_valueless) {
            if (strcmp(c->value, "0") && strcmp(c->value, "off"))
                emit_token(flag, c->origin);
        } else if (c->has_value) {
            emit_token(flag, c->origin);
            emit_token(c->value, c->origin);
        }
    }
}

/* ------------------------------------------------------------------ */
/* disc identity                                                      */
/* ------------------------------------------------------------------ */

static openpete_disc_identity_fn s_disc_identity;

void openpete_config_set_disc_identity_fn(openpete_disc_identity_fn fn) {
    s_disc_identity = fn;
}

void openpete_config_set_identified_disc(const char *disc_path) {
    snprintf(s_ident_disc, sizeof s_ident_disc, "%s", disc_path ? disc_path : "");
}

void openpete_config_set_identity_source(const char *source) {
    snprintf(s_ident_source, sizeof s_ident_source, "%s", source ? source : "");
}

/* ------------------------------------------------------------------ */
/* game and disc scopes on the device-creation set                    */
/* ------------------------------------------------------------------ */

/** @brief Whether contribution `c` was harvested from a [game.*] or [disc.*] scope. */
static int scoped_tier(const contrib_t *c) {
    const int tier = c->rank / 1000;
    return tier == TIER_GAME || tier == TIER_DISC_SERIAL || tier == TIER_DISC_SHA;
}

/** @brief The device-creation class of contribution `c`, from its own spelling or its knob's.
 *
 * An [env] or [args] contribution is classed by its key; a bare key, or any
 * spelling of a schema knob, by the stronger class of the knob's flag and
 * env var, since the two spellings are one setting. */
static cfg_device_t contrib_device_class(const contrib_t *c) {
    cfg_device_t cls = cfg_schema_device_class(c->key);
    if (c->knob >= 0) {
        const cfg_knob_t *k = &cfg_schema(NULL)[c->knob];
        cfg_device_t a = cfg_schema_device_class(k->cli);
        cfg_device_t b = cfg_schema_device_class(k->env);
        if (a > cls) cls = a;
        if (b > cls) cls = b;
    }
    return cls;
}

/** @brief Move every game or disc scope contribution the full stage may not apply to the not-applied list.
 *
 * Two kinds: a CFG_DEVICE_FIXED knob, which the device was created without,
 * and the `disc` knob, an identification input that a scope selected by
 * the identified game or disc cannot name. CFG_DEVICE_LIVE contributions
 * stay and win as usual; the consumer applies them after creation. */
static void drop_scoped_unapplied(void) {
    const int disc_knob = cfg_schema_find_id("disc");
    int o = 0;
    for (int i = 0; i < s_contrib_n; i++) {
        const contrib_t *c = &s_contrib[i];
        const char *why = NULL;
        if (scoped_tier(c)) {
            if (contrib_device_class(c) == CFG_DEVICE_FIXED)
                why = "device-creation knob";
            else if (disc_knob >= 0 && c->knob == disc_knob)
                why = "identification input";
        }
        if (!why) {
            if (o != i) s_contrib[o] = s_contrib[i];
            o++;
            continue;
        }
        if (s_not_applied_n < CFG_MAX_NOT_APPLIED) {
            not_applied_t *n = &s_not_applied[s_not_applied_n++];
            snprintf(n->key, sizeof n->key, "%s",
                     c->knob >= 0 ? cfg_schema(NULL)[c->knob].id : c->key);
            snprintf(n->value, sizeof n->value, "%s", c->value);
            snprintf(n->origin, sizeof n->origin, "%s", c->origin);
            n->why = why;
        }
    }
    s_contrib_n = o;
}

/* ------------------------------------------------------------------ */
/* entry point                                                        */
/* ------------------------------------------------------------------ */

int openpete_config_report_requested(void) { return s_report_requested; }

const char *openpete_config_token_source(const char *token) {
    for (int i = 0; i < s_token_n; i++)
        if (s_tokens[i] == token)
            return s_token_origin[i];
    return NULL;
}

/** @brief Clear one resolution's state: documents, files, contributions, tokens, decisions, scopes and latches.
 *
 * Keeps the game id, the disc-identity hook, the identified disc and source,
 * the diagnostics already printed and the materialization count, which span
 * both stages. The spliced tokens are not freed. */
static void reset_resolution(void) {
    for (int f = 0; f < 2; f++) {
        if (s_doc[f]) toml_free(s_doc[f]);
        s_doc[f] = NULL;
        s_file_path[f][0] = '\0';
        s_file_present[f] = 0;
    }
    s_contrib_n = s_token_n = s_decision_n = s_saved_n = 0;
    s_saved_keys_n = 0;
    s_pending_env_n = s_not_applied_n = 0;
    s_file_n = 0;
    s_write_file = 1;
    s_save_path[0] = s_scope_serial[0] = s_scope_sha[0] = '\0';
    s_no_config = s_report_requested = 0;
    openpete_paths_set_portable(-1);   /* drop the last run's directive */
}

void openpete_config_reset(void) {
    reset_resolution();
    s_disc_identity = NULL;
    s_ident_disc[0] = s_ident_source[0] = '\0';
    s_diag_n = 0;
    s_materializations = 0;
}

/** @brief Read the launch tiers and both files, and harvest the global scope of each.
 *
 * Scans argv for --no-config, --config-report, --keys-report and --config
 * (OPENPETE_CONFIG when --config is absent; "none" disables the files),
 * captures the real environment, records the launch tiers, loads the files
 * and settles the save target from the `portable` directive.
 * @return 0, or -1 on an error already printed. */
static int harvest_launch_and_global(int argc, char **argv, int typed_argc) {
    const char *explicit_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-config")) s_no_config = 1;
        else if (!strcmp(argv[i], "--config-report")) s_report_requested = 1;
        else if (!strcmp(argv[i], "--keys-report")) openpete_keymap_request_report();
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) explicit_path = argv[i + 1];
    }
    if (!explicit_path) {
        const char *e = getenv("OPENPETE_CONFIG");
        if (e && *e) explicit_path = e;
    }
    if (explicit_path && !strcmp(explicit_path, "none")) {
        s_no_config = 1;
        explicit_path = NULL;
    }

    /* The environment as the user actually exported it - captured before a
     * single [env] line is applied, so tier 8 can be told apart from the
     * file's own variables. The game-neutral stage writes nothing, so the
     * full stage captures the same environment. */
    scan_real_env();

    /* argv already carries the bk2 sidecar (spliced by the caller), whose
     * tokens sit left of the typed ones - so the typed arguments are the
     * last typed_argc-1 entries. */
    int typed_from = argc - (typed_argc - 1);
    if (typed_from < 1) typed_from = 1;
    scan_launch(argc, argv, typed_from);

    if (s_no_config) return 0;

    char exedir[CFG_PATH_MAX] = "";
    int have_exedir = (openpete_exe_dir(exedir, sizeof exedir) == 0);

    if (explicit_path) {
        snprintf(s_file_path[0], CFG_PATH_MAX, "%s", explicit_path);
        s_file_n = 1;
        s_write_file = 0;
        if (!file_exists(explicit_path)) {
            cfg_diag("openpete: --config %s: no such file\n", explicit_path);
            return -1;
        }
    } else {
        if (have_exedir)
            openpete_paths_join(s_file_path[0], CFG_PATH_MAX, exedir, "openpete.toml");
        /* Named, not created: the per-user file is read whether or not
         * it is the write target, and a portable install must not leave
         * an empty directory behind in AppData just for having looked. */
        char userdir[CFG_PATH_MAX];
        if (openpete_paths_user_config_dir(userdir, sizeof userdir, 0) == 0)
            openpete_paths_join(s_file_path[1], CFG_PATH_MAX, userdir, "openpete.toml");
        s_file_n = 2;
    }

    if (!explicit_path && have_exedir) warn_retired_sidecars(exedir);

    for (int f = 0; f < s_file_n; f++) {
        if (!s_file_path[f][0]) continue;
        if (load_toml(f) < 0) return -1;
    }

    /* Both files are now parsed, so the `portable` directive (if any)
     * has landed and the layout is decided. --config pins its own file
     * as the target and is not up for a vote. */
    if (!explicit_path) {
        s_write_file = openpete_paths_portable() ? 0 : 1;
        if (s_write_file == 1 && !s_neutral) {
            /* The write target has to exist before the [saved] flush;
             * naming it above deliberately did not create it. */
            char userdir[CFG_PATH_MAX];
            (void)openpete_paths_user_config_dir(userdir, sizeof userdir, 1);
        }
    }

    for (int f = 0; f < s_file_n; f++)
        if (s_doc[f] && harvest_global(f) != 0) return -1;
    return 0;
}

/** @brief Harvest the identified game's [game.<id>] scope and the identified disc's [disc.*] scopes.
 *
 * The disc-identity hook names the identified disc's serial and hash
 * (cheap by contract: a serial read or a library entry's recorded hash).
 * @return 0, or -1 on an error already printed. */
static int harvest_identified_scopes(void) {
    for (int f = 0; f < s_file_n; f++)
        if (s_doc[f] && harvest_named(f, "game", s_scope_game, TIER_GAME, "game") != 0)
            return -1;
    if (s_disc_identity)
        (void)s_disc_identity(s_ident_disc[0] ? s_ident_disc : NULL,
                              s_scope_serial, sizeof s_scope_serial,
                              s_scope_sha, sizeof s_scope_sha);
    for (int f = 0; f < s_file_n; f++) {
        if (!s_doc[f]) continue;
        if (harvest_named(f, "disc", s_scope_serial, TIER_DISC_SERIAL, "disc") != 0)
            return -1;
        if (harvest_disc_sha(f) != 0) return -1;
    }
    return 0;
}

/** @brief Run one resolution stage: harvest, resolve and materialize.
 *
 * The game-neutral stage (s_neutral) harvests the launch tiers, the global
 * scopes and [saved]; the full stage adds the identified game's and disc's
 * scopes, drops the scoped values it may not apply, resolves the keymap,
 * and is the stage that writes the environment.
 * @return As openpete_config_apply. */
static int resolve_stage(int argc, char **argv, int typed_argc,
                         int *out_argc, char ***out_argv) {
    reset_resolution();

    /* Seed the keymap from its registry defaults before any [keys] table
     * is read, so config rows layer on top of a populated store rather
     * than defining it. The game-neutral stage reads no [keys] table. */
    if (!s_neutral) {
        /* The binding page compares against the layers below [saved] and
         * below the game scopes; every [keys] row is ranked against these. */
        openpete_keymap_set_layer_bounds(RANK(TIER_SAVED, 0, SP_BASE),
                                         RANK(TIER_GAME, 0, SP_BASE));
        openpete_keymap_init();
    }

    if (harvest_launch_and_global(argc, argv, typed_argc) != 0) return -1;

    if (!s_no_config) {
        if (!s_neutral && harvest_identified_scopes() != 0) return -1;
        for (int f = 0; f < s_file_n; f++)
            if (s_doc[f] && harvest_saved(f) != 0) return -1;
    }

    if (s_write_file >= 0 && s_file_path[s_write_file][0])
        snprintf(s_save_path, sizeof s_save_path, "%s", s_file_path[s_write_file]);

    if (!s_neutral) {
        drop_scoped_unapplied();
        /* Every [keys] layer is in; pick winners and run the conflict +
         * reachability scans. Never fails a boot - a clash is a warning,
         * and an unreachable control is the user's call to make. */
        (void)openpete_keymap_resolve();
    }

    materialize();
    if (!s_neutral) s_materializations++;

    if (s_token_n == 0) return 0;

    int new_argc = argc + s_token_n;
    char **new_argv = (char **)malloc(sizeof(char *) * (size_t)(new_argc + 1));
    if (!new_argv) return -1;
    int o = 0;
    new_argv[o++] = argv[0];
    for (int i = 0; i < s_token_n; i++) new_argv[o++] = s_tokens[i];
    for (int i = 1; i < argc; i++)      new_argv[o++] = argv[i];
    new_argv[o] = NULL;

    *out_argc = new_argc;
    *out_argv = new_argv;
    return 1;
}

int openpete_config_apply_neutral(int argc, char **argv, int typed_argc,
                                  int *out_argc, char ***out_argv) {
    s_neutral = 1;
    const int rc = resolve_stage(argc, argv, typed_argc, out_argc, out_argv);
    s_neutral = 0;
    return rc;
}

int openpete_config_apply(int argc, char **argv, int typed_argc,
                          int *out_argc, char ***out_argv) {
    s_neutral = 0;
    return resolve_stage(argc, argv, typed_argc, out_argc, out_argv);
}

int openpete_config_materializations(void) { return s_materializations; }

const char *openpete_config_env(const char *name) {
    if (!name) return NULL;
    const char *v = getenv(name);
    if (v) return v;
    for (int i = 0; i < s_pending_env_n; i++)
        if (!strcmp(s_pending_env[i].key, name)) return s_pending_env[i].value;
    return NULL;
}

const char *openpete_config_device_value(const char *spelling) {
    if (!spelling) return NULL;
    const int is_env = !strncmp(spelling, "OPENPETE_", 9);
    /* The contribution that names this spelling, to find its setting. */
    const contrib_t *probe = NULL;
    contrib_t synth;
    for (int i = 0; i < s_contrib_n && !probe; i++)
        if (!strcmp(s_contrib[i].key, spelling)) probe = &s_contrib[i];
    if (!probe) {
        memset(&synth, 0, sizeof synth);
        synth.knob = is_env ? cfg_schema_find_env(spelling) : cfg_schema_find_cli(spelling);
        synth.spelling = is_env ? SP_ENV : SP_ARGS;
        snprintf(synth.key, sizeof synth.key, "%s", spelling);
        probe = &synth;
    }
    /* The setting's winner over the tiers without a game or disc scope. */
    const contrib_t *win = NULL;
    for (int i = 0; i < s_contrib_n; i++) {
        const contrib_t *c = &s_contrib[i];
        if (c->suppress || scoped_tier(c) || !same_setting(probe, c)) continue;
        if (!win || c->rank > win->rank) win = c;
    }
    if (!win) return NULL;
    /* Two spellings of one knob share a winner; it answers only its own. */
    if (is_env != (win->spelling == SP_ENV)) return NULL;
    return win->value;
}

void openpete_config_set_game_id(const char *id) {
    snprintf(s_scope_game, sizeof s_scope_game, "%s", id ? id : "");
}

/* ------------------------------------------------------------------ */
/* report                                                             */
/* ------------------------------------------------------------------ */

/** @brief Print the layout and every directory openpete writes, as this install resolves them.
 *
 * The knob table never shows these: they are directories, not knobs, and
 * the answer moves with the layout. Savestates are kept per game, under the
 * identified game's id; the other state leaves are shared by every game.
 * Nothing is created.
 * @param out The report stream. */
static void report_layout(FILE *out) {
    static const struct { const char *leaf; int per_game; } kState[] = {
        { "cards", 0 }, { "states", 1 }, { "logs", 0 },
    };
    static const char *const kData[] = { "library", "mods", "assets", "bk2", "screenshots" };
    char dir[CFG_PATH_MAX];
    fprintf(out, "  layout: %s\n",
            openpete_paths_portable() ? "portable (next to the exe)" : "per-user directories");
    for (size_t i = 0; i < sizeof kState / sizeof kState[0]; i++)
        if (openpete_paths_game_state_dir(kState[i].leaf, kState[i].per_game ? s_scope_game : NULL,
                                          dir, sizeof dir, 0) == 0)
            fprintf(out, "  %-8s %s\n", kState[i].leaf, dir);
    for (size_t i = 0; i < sizeof kData / sizeof kData[0]; i++)
        if (openpete_paths_data_dir(kData[i], dir, sizeof dir, 0) == 0)
            fprintf(out, "  %-8s %s\n", kData[i], dir);
}

void openpete_config_report(FILE *out) {
    if (!out) out = stderr;
    fprintf(out, "openpete config\n");
    if (s_no_config) {
        fprintf(out, "  files: (--no-config: none read)\n");
    } else {
        fprintf(out, "  files:");
        int any = 0;
        for (int f = 0; f < s_file_n; f++) {
            if (!s_file_path[f][0]) continue;
            fprintf(out, "%s %s%s", any ? "," : "", s_file_path[f],
                    s_file_present[f] ? "" : " (absent)");
            any = 1;
        }
        if (!any) fprintf(out, " (none)");
        fprintf(out, "\n");
    }
    fprintf(out, "  scope: game=%s serial=%s sha=%s\n",
            s_scope_game[0] ? s_scope_game : "-",
            s_scope_serial[0] ? s_scope_serial : "-",
            s_scope_sha[0] ? s_scope_sha : "-");
    fprintf(out, "  identified by: %s\n", s_ident_source[0] ? s_ident_source : "-");
    fprintf(out, "  save target: %s\n", s_save_path[0] ? s_save_path : "(none)");

    report_layout(out);

    fprintf(out, "\n  %-22s %-14s %-26s %s\n", "knob", "value", "won at", "shadowed");
    for (int i = 0; i < s_decision_n; i++) {
        const decision_t *d = &s_decision[i];
        const cfg_knob_t *k = &cfg_schema(NULL)[d->knob];
        fprintf(out, "  %-22s %-14s %-26s %s\n", k->id,
                d->value[0] ? d->value : "(set)", d->origin,
                d->shadow[0] ? d->shadow : "-");
    }

    /* Everything with no schema row: raw flags and the debug env vars. */
    int printed_hdr = 0;
    for (int i = 0; i < s_contrib_n; i++) {
        const contrib_t *c = &s_contrib[i];
        if (c->knob >= 0 || is_shadowed(c)) continue;
        if (c->rank >= RANK(TIER_REAL_ENV, 0, 0)) continue;
        if (!printed_hdr) { fprintf(out, "\n  unregistered:\n"); printed_hdr = 1; }
        fprintf(out, "    %-30s %-14s %s\n", c->key,
                c->suppress ? "(suppressed)" : (c->value[0] ? c->value : "(flag)"),
                c->origin);
    }

    /* Scoped values of the device-creation set and of the identification
     * input: harvested, then refused, so the file's line is accounted for. */
    if (s_not_applied_n) {
        fprintf(out, "\n  not applied (set in a game or disc scope):\n");
        for (int i = 0; i < s_not_applied_n; i++)
            fprintf(out, "    %-30s %-14s %-26s %s\n", s_not_applied[i].key,
                    s_not_applied[i].value, s_not_applied[i].origin,
                    s_not_applied[i].why);
    }
    fprintf(out, "\n");
}

/* ------------------------------------------------------------------ */
/* provenance                                                         */
/* ------------------------------------------------------------------ */

/** @brief The recorded decision for knob id `knob_id`, or NULL when the id is unknown or no layer set it. */
static const decision_t *decision_lookup(const char *knob_id) {
    int ki = cfg_schema_find_id(knob_id);
    if (ki < 0) return NULL;
    for (int i = 0; i < s_decision_n; i++)
        if (s_decision[i].knob == ki) return &s_decision[i];
    return NULL;
}

const char *openpete_config_knob_layer(const char *knob_id) {
    const decision_t *d = decision_lookup(knob_id);
    return d ? d->origin : "default";
}

int openpete_config_knob_pinned(const char *knob_id) {
    const decision_t *d = decision_lookup(knob_id);
    return d ? d->pinned : 0;
}

const char *openpete_config_knob_pin_spelling(const char *knob_id) {
    const decision_t *d = decision_lookup(knob_id);
    return (d && d->pinned && d->pin[0]) ? d->pin : NULL;
}

/* ------------------------------------------------------------------ */
/* the managed [saved] zone                                           */
/* ------------------------------------------------------------------ */

#define CFG_MANAGED_MARKER \
    "# >>> openpete managed settings - written by the in-game menus <<<"
#define CFG_MANAGED_NOTE \
    "# Anything below this line is REGENERATED on save; edit above it.\n"

const char *openpete_config_save_path(void) {
    return s_save_path[0] ? s_save_path : "(none)";
}

int openpete_config_save_knob(const char *knob_id, const char *value) {
    if (!knob_id || !value) return -1;
    if (cfg_schema_find_id(knob_id) < 0) return -1;
    for (int i = 0; i < s_saved_n; i++) {
        if (strcmp(s_saved[i].id, knob_id)) continue;
        snprintf(s_saved[i].value, CFG_VALUE_MAX, "%s", value);
        return 0;
    }
    if (s_saved_n >= CFG_MAX_SAVED) return -1;
    snprintf(s_saved[s_saved_n].id, CFG_KEY_MAX, "%s", knob_id);
    snprintf(s_saved[s_saved_n].value, CFG_VALUE_MAX, "%s", value);
    s_saved_n++;
    return 0;
}

int openpete_config_unsave_knob(const char *knob_id) {
    if (!knob_id) return 0;
    for (int i = 0; i < s_saved_n; i++) {
        if (strcmp(s_saved[i].id, knob_id)) continue;
        memmove(&s_saved[i], &s_saved[i + 1],
                (size_t)(s_saved_n - i - 1) * sizeof s_saved[0]);
        s_saved_n--;
        return 1;
    }
    return 0;
}

int openpete_config_save_binding(const char *domain, const char *action,
                                 const char *const *toks, int n) {
    if (!domain || !action || n < 0 || (n && !toks)) return -1;
    return saved_key_put(domain, action, toks, n);
}

int openpete_config_unsave_binding(const char *domain, const char *action) {
    if (!domain || !action) return 0;
    const int i = saved_key_find(domain, action);
    if (i < 0) return 0;
    memmove(&s_saved_keys[i], &s_saved_keys[i + 1],
            (size_t)(s_saved_keys_n - i - 1) * sizeof s_saved_keys[0]);
    s_saved_keys_n--;
    return 1;
}

int openpete_config_knob_value_below_saved(const char *knob_id, char *buf,
                                           size_t cap) {
    const int ki = cfg_schema_find_id(knob_id);
    if (ki < 0 || !buf || cap == 0) return -1;
    const contrib_t *win = NULL;
    for (int i = 0; i < s_contrib_n; i++) {
        const contrib_t *c = &s_contrib[i];
        if (c->knob != ki) continue;
        /* A game or disc scope sits between the global tier and [saved]:
         * with the key removed the scope would decide this game's value,
         * so no global comparison can stand in for it. */
        if (scoped_tier(c)) return -1;
        if (c->rank / 1000 > TIER_GLOBAL) continue;
        if (!win || c->rank > win->rank) win = c;
    }
    if (!win) return 0;
    /* [env] spells the variable's vocabulary and [args] = false withholds
     * the flag; neither is a value in the CLI's vocabulary. */
    if (win->spelling == SP_ENV || win->suppress || win->valueless ||
        !win->has_value)
        return -1;
    snprintf(buf, cap, "%s", win->value);
    return 1;
}

int openpete_config_has_save_target(void) { return s_save_path[0] != '\0'; }

/** @brief Write a [saved] value and newline as the TOML type its knob declares.
 *
 * A bool is `false` for "0" or "off" and `true` otherwise; an int or float
 * is written bare, "0" when empty; every other type is a quoted string. The
 * file round-trips through the reader (`fullscreen = true`, not
 * `fullscreen = "1"`). The string is written without escaping. */
static void write_saved_value(FILE *f, const cfg_knob_t *k, const char *v) {
    switch (k->type) {
    case CFG_BOOL:
        fprintf(f, "%s\n", (strcmp(v, "0") && strcmp(v, "off")) ? "true" : "false");
        break;
    case CFG_INT:
    case CFG_FLOAT:
        fprintf(f, "%s\n", (v && *v) ? v : "0");
        break;
    default:
        fprintf(f, "\"%s\"\n", v ? v : "");
        break;
    }
}

/** @brief Write a TOML basic string, escaping backslash and quote (SDL names the backslash key "\\"). */
static void write_toml_string(FILE *f, const char *v) {
    fputc('"', f);
    for (const char *p = v ? v : ""; *p; p++) {
        if (*p == '\\' || *p == '"') fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

/** @brief Write a TOML key: bare when it is all letters, digits, '_' and '-', else quoted. */
static void write_toml_key(FILE *f, const char *k) {
    int bare = *k != '\0';
    for (const char *p = k; *p && bare; p++)
        bare = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
    if (bare) fputs(k, f);
    else      write_toml_string(f, k);
}

/** @brief Write one [saved.keys] row: an array of strings, or for a mod.<id> row one string with the tokens joined by ',' ("none" when empty). */
static void write_saved_key_row(FILE *out, const saved_key_t *k) {
    write_toml_key(out, k->action);
    fputs(" = ", out);
    if (!strncmp(k->domain, "mod.", 4)) {
        char joined[CFG_VALUE_MAX] = "";
        for (int t = 0; t < k->n; t++) {
            if (t) strncat(joined, ",", sizeof joined - strlen(joined) - 1);
            strncat(joined, k->tok[t], sizeof joined - strlen(joined) - 1);
        }
        write_toml_string(out, k->n ? joined : "none");
    } else {
        fputc('[', out);
        for (int t = 0; t < k->n; t++) {
            if (t) fputs(", ", out);
            write_toml_string(out, k->tok[t]);
        }
        fputc(']', out);
    }
    fputc('\n', out);
}

/** @brief Write the [saved.keys.<domain>] tables, one per domain in first-seen order.
 *
 * A host or pad row is an array of strings. A mod.<id> row is one string,
 * the tokens joined by ',', since the [keys.mod.<id>] reader takes a
 * string only. */
static void write_saved_keys(FILE *out) {
    for (int i = 0; i < s_saved_keys_n; i++) {
        int seen = 0;
        for (int j = 0; j < i && !seen; j++)
            seen = !strcmp(s_saved_keys[j].domain, s_saved_keys[i].domain);
        if (seen) continue;
        const char *dom = s_saved_keys[i].domain;
        fputs("[saved.keys.", out);
        if (!strncmp(dom, "mod.", 4)) { fputs("mod.", out); write_toml_key(out, dom + 4); }
        else                          fputs(dom, out);
        fputs("]\n", out);
        for (int j = i; j < s_saved_keys_n; j++)
            if (!strcmp(s_saved_keys[j].domain, dom)) write_saved_key_row(out, &s_saved_keys[j]);
    }
}

/** @brief Return whether a span of config text holds a `[saved` table header at the start of a line. */
static int text_has_saved_header(const char *text, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && (text[j] == ' ' || text[j] == '\t')) j++;
        if (len - j >= 6 && !strncmp(text + j, "[saved", 6) &&
            (j + 6 == len || text[j + 6] == ']' || text[j + 6] == '.' ||
             text[j + 6] == ' ' || text[j + 6] == '\t'))
            return 1;
        while (i < len && text[i] != '\n') i++;
        i++;
    }
    return 0;
}

int openpete_config_save_flush(void) {
    if (!s_save_path[0]) {
        fprintf(stderr, "openpete: config: nowhere to save (no writable config path)\n");
        return -1;
    }

    /* Copy everything above the marker through byte for byte: comments,
     * ordering and any table the reader doesn't know about all survive.
     * Only the zone below is ours to rewrite. */
    char *head = NULL;
    size_t head_len = 0;
    FILE *in = fopen(s_save_path, "rb");
    if (in) {
        fseek(in, 0, SEEK_END);
        long sz = ftell(in);
        rewind(in);
        if (sz > 0) {
            head = (char *)malloc((size_t)sz + 1);
            if (!head) { fclose(in); return -1; }
            size_t got = fread(head, 1, (size_t)sz, in);
            head[got] = '\0';
            char *marker = strstr(head, CFG_MANAGED_MARKER);
            head_len = marker ? (size_t)(marker - head) : got;
        }
        fclose(in);
    }

    /* The reader cannot tell [saved] rows above the marker from the zone's,
     * so writing the zone under a hand-written [saved] table would define
     * the table twice and fail the next parse. */
    if (head && text_has_saved_header(head, head_len)) {
        fprintf(stderr, "openpete: config: %s has a [saved] table above the "
                "managed marker; not saving (move it below the marker or "
                "delete it)\n", s_save_path);
        free(head);
        return -1;
    }

    op_atomic_file_t af;
    if (op_atomic_file_open(&af, s_save_path) != 0) {
        fprintf(stderr, "openpete: config: cannot write beside %s\n", s_save_path);
        free(head);
        return -1;
    }
    FILE *out = af.f;
    if (head && head_len) fwrite(head, 1, head_len, out);
    if (head_len && head[head_len - 1] != '\n') fputc('\n', out);
    free(head);

    fprintf(out, "%s\n", CFG_MANAGED_MARKER);
    fputs(CFG_MANAGED_NOTE, out);
    if (s_saved_n) {
        fprintf(out, "[saved]\n");
        for (int i = 0; i < s_saved_n; i++) {
            int ki = cfg_schema_find_id(s_saved[i].id);
            if (ki < 0) continue;
            fprintf(out, "%s = ", s_saved[i].id);
            write_saved_value(out, &cfg_schema(NULL)[ki], s_saved[i].value);
        }
    }
    write_saved_keys(out);
    /* The commit checks every write above (the stream's error flag), the
     * flush, sync and close, and replaces the file in one rename; on any
     * failure the previous file stays whole. */
    if (op_atomic_file_commit(&af) != 0) {
        fprintf(stderr, "openpete: config: cannot replace %s; previous file kept\n",
                s_save_path);
        return -1;
    }
    fprintf(stderr, "openpete: config: saved %d setting(s) and %d binding(s) to %s\n",
            s_saved_n, s_saved_keys_n, s_save_path);
    return 0;
}

int openpete_config_reset_saved(void) {
    s_saved_n = 0;
    s_saved_keys_n = 0;
    return openpete_config_save_flush();
}

