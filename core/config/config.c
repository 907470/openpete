/*
 * core/config/config.c — openpete.toml: load, resolve, materialize.
 *
 * The precedence chain and the reason flattening is required (rather than
 * splicing every layer and letting each consumer's own CLI-beats-env rule
 * decide) are documented in openpete_config.h. Read that first.
 *
 * Runs before the logger is configured — --log-level and --log-format are
 * themselves config knobs — so diagnostics here go straight to stderr,
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

#include "../../third_party/tomlc99/toml.h"

#include <errno.h>
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
        fprintf(stderr, "openpete: config: too many entries (max %d)\n",
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
        /* %g keeps 0.5 as "0.5" and 100 as "100" — the spellings a user
         * would have typed on the command line. */
        snprintf(out, cap, "%g", d);
        return 0;
    }
    return -1;
}

static int enum_ok(const cfg_knob_t *k, const char *v) {
    if (k->type != CFG_ENUM || !k->vocab) return 1;
    for (int i = 0; k->vocab[i]; i++)
        if (!strcmp(k->vocab[i], v)) return 1;
    return 0;
}

static void enum_vocab_str(const cfg_knob_t *k, char *out, size_t cap) {
    out[0] = '\0';
    if (!k->vocab) return;
    for (int i = 0; k->vocab[i]; i++) {
        if (i) strncat(out, "|", cap - strlen(out) - 1);
        strncat(out, k->vocab[i], cap - strlen(out) - 1);
    }
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

static const cfg_renamed_t *renamed_for(const char *key) {
    for (size_t i = 0; i < sizeof g_renamed / sizeof g_renamed[0]; i++)
        if (!strcmp(g_renamed[i].old_key, key)) return &g_renamed[i];
    return NULL;
}

/** @brief Truthiness of a renamed bool key's value.
 *
 * A TOML bool gives its own value; a string value is true when it is "1",
 * "on" or "true". */
static int renamed_truth(const char *val, int is_bool, int bool_val) {
    if (is_bool) return bool_val;
    return !strcmp(val, "1") || !strcmp(val, "on") || !strcmp(val, "true");
}

/** @brief Record the bare keys of `tab`, the validated spelling, as contributions at (tier, file).
 *
 * Sub-tables are skipped (their caller harvests them); `skip_directives`
 * also skips `portable`. A renamed key is mapped to its new name with a
 * warning, and its value to the new knob's vocabulary when the rename
 * carries one. A
 * non-scalar value, an unknown key (named with a did_you_mean hint), a
 * value of a TOML type the config does not accept, a non-boolean value for
 * a bool knob and an enum value outside the knob's vocabulary are errors
 * printed with `where`. Returns 0, or -1 on the first error. */
static int harvest_base(const toml_table_t *tab, int tier, int file,
                        const char *origin, const char *where,
                        int skip_directives) {
    for (int i = 0;; i++) {
        const char *key = toml_key_in(tab, i);
        if (!key) break;
        if (skip_directives && is_directive_key(key)) continue;

        toml_raw_t raw = toml_raw_in(tab, key);
        if (!raw) {
            /* A sub-table ([env]/[args]/[saved]) or an array. Sub-tables are
             * harvested by the caller; anything else is a mistake worth
             * naming, because TOML also lands a stray bare key here when it
             * is written after a sub-table header. */
            if (toml_table_in(tab, key)) continue;
            fprintf(stderr, "openpete: config: %s: '%s' must be a scalar "
                            "(arrays are not config values)\n", where, key);
            return -1;
        }

        const cfg_renamed_t *ren = renamed_for(key);
        if (ren) {
            fprintf(stderr, "openpete: config: %s: '%s' has been renamed to "
                            "'%s'; using it\n", where, key, ren->new_key);
            key = ren->new_key;
        }

        int ki = cfg_schema_find_id(key);
        if (ki < 0) {
            const char *hint = did_you_mean(key);
            fprintf(stderr, "openpete: config: %s: unknown key '%s'%s%s%s\n",
                    where, key,
                    hint ? " (did you mean '" : "", hint ? hint : "",
                    hint ? "'?)" : "");
            return -1;
        }
        const cfg_knob_t *k = &cfg_schema(NULL)[ki];

        char val[CFG_VALUE_MAX];
        int is_bool = 0, bool_val = 0;
        if (toml_scalar_to_string(raw, val, sizeof val, &is_bool, &bool_val) != 0) {
            fprintf(stderr, "openpete: config: %s: '%s' has a value this "
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
        if (k->type == CFG_BOOL && !is_bool &&
            strcmp(val, "0") && strcmp(val, "1") &&
            strcmp(val, "on") && strcmp(val, "off")) {
            fprintf(stderr, "openpete: config: %s: '%s' expects true/false, "
                            "got: %s\n", where, key, val);
            return -1;
        }
        if (!enum_ok(k, val)) {
            char vocab[128];
            enum_vocab_str(k, vocab, sizeof vocab);
            fprintf(stderr, "openpete: config: %s: '%s' expects %s, got: %s\n",
                    where, key, vocab, val);
            return -1;
        }

        contrib_t *c = contrib_new();
        if (!c) return -1;
        c->knob = ki;
        c->rank = RANK(tier, file, SP_BASE);
        c->spelling = SP_BASE;
        c->has_value = 1;
        snprintf(c->key, sizeof c->key, "%s", key);
        snprintf(c->value, sizeof c->value, "%s", val);
        snprintf(c->origin, sizeof c->origin, "%s", origin);
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
            fprintf(stderr, "openpete: config: %s: [env] '%s' must be a scalar\n",
                    where, key);
            return -1;
        }
        char val[CFG_VALUE_MAX];
        int is_bool = 0, bool_val = 0;
        if (toml_scalar_to_string(raw, val, sizeof val, &is_bool, &bool_val) != 0) {
            fprintf(stderr, "openpete: config: %s: [env] '%s' has a value this "
                            "config cannot use\n", where, key);
            return -1;
        }
        if (strncmp(key, "OPENPETE_", 9) != 0)
            fprintf(stderr, "openpete: config: %s: [env] '%s' is not an "
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

/** @brief Record the rows of a [<scope>.args] table, whose keys are flags verbatim.
 *
 * A key must start with "--". On an unregistered or valueless flag a
 * boolean value means the flag takes none: true emits it, false suppresses
 * whatever a lower tier asked for (the only way to un-set an inherited
 * valueless flag). On a schema alias flag ("--no-lod") true stands for the
 * alias's fixed value and false suppresses. On any other schema flag a
 * boolean is the 0/1 the flag expects, and an enum value outside the knob's
 * vocabulary is an error. Returns 0, or -1 on the first error. */
static int harvest_args(const toml_table_t *tab, int tier, int file,
                        const char *origin, const char *where) {
    for (int i = 0;; i++) {
        const char *key = toml_key_in(tab, i);
        if (!key) break;
        toml_raw_t raw = toml_raw_in(tab, key);
        if (!raw) {
            fprintf(stderr, "openpete: config: %s: [args] '%s' must be a scalar\n",
                    where, key);
            return -1;
        }
        if (strncmp(key, "--", 2) != 0) {
            fprintf(stderr, "openpete: config: %s: [args] key '%s' is not a flag "
                            "(write it exactly as on the command line, e.g. "
                            "\"--lod\")\n", where, key);
            return -1;
        }
        char val[CFG_VALUE_MAX];
        int is_bool = 0, bool_val = 0;
        if (toml_scalar_to_string(raw, val, sizeof val, &is_bool, &bool_val) != 0) {
            fprintf(stderr, "openpete: config: %s: [args] '%s' has a value this "
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
            if (is_bool && !bool_val) { c->suppress = 1; continue; }
            c->has_value = 1;
            snprintf(c->value, sizeof c->value, "%s", k->alias_value);
            continue;
        }
        if (is_bool && (!k || k->flag_valueless)) {
            if (!bool_val) { c->suppress = 1; continue; }
            c->valueless = 1;
            continue;
        }
        if (k && !enum_ok(k, val)) {
            char vocab[128];
            enum_vocab_str(k, vocab, sizeof vocab);
            fprintf(stderr, "openpete: config: %s: [args] '%s' expects %s, got: %s\n",
                    where, key, vocab, val);
            return -1;
        }
        c->has_value = 1;
        snprintf(c->value, sizeof c->value, "%s", val);
    }
    return 0;
}

/** @brief Forward the rows of a [<scope>.keys.<domain>] table to the keymap at `rank`.
 *
 * Input bindings. Unlike knobs, these do not
 * flow through the contribution table: the keymap owns its own layered
 * store (it merges per ACTION and replaces within one, which the knob
 * resolver's single-winner-per-key model cannot express), so this walk
 * just forwards each row with the scope's rank and lets keymap.c decide.
 *
 * A value is a string or an array of strings; arrays are legal here only
 * (harvest_base rejects them). An array longer than 2 * KM_MAX_SOURCES is
 * truncated; an empty array means "none". Returns 0, or -1 on a malformed
 * row or a keymap refusal. */
static int harvest_keys_domain(const toml_table_t *tab, const char *domain,
                               int rank, const char *origin,
                               const char *where) {
    for (int i = 0;; i++) {
        const char *key = toml_key_in(tab, i);
        if (!key) break;

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
                    fprintf(stderr, "openpete: config: %s: [keys.%s] '%s' "
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
                                    where) != 0)
                return -1;
            continue;
        }

        toml_raw_t raw = toml_raw_in(tab, key);
        if (!raw) {
            fprintf(stderr, "openpete: config: %s: [keys.%s] '%s' must be a "
                            "string or an array of strings\n",
                    where, domain, key);
            return -1;
        }
        char val[CFG_VALUE_MAX];
        int is_bool = 0, bool_val = 0;
        if (toml_scalar_to_string(raw, val, sizeof val, &is_bool, &bool_val) != 0) {
            fprintf(stderr, "openpete: config: %s: [keys.%s] '%s' has a value "
                            "this config cannot use\n", where, domain, key);
            return -1;
        }
        const char *tok = val;
        if (openpete_keymap_set(domain, key, &tok, 1, rank, origin, where) != 0)
            return -1;
    }
    return 0;
}

static int harvest_keys(const toml_table_t *keys, int tier, int file,
                        const char *origin, const char *where) {
    /* Keys have no [env]/[args] twin, so the spelling sub-sort never
     * applies and every binding ranks at SP_BASE for its scope. */
    const int rank = RANK(tier, file, SP_BASE);

    static const char *const domains[] = { "pad", "pad2", "pad3", "pad4", "host" };
    for (size_t d = 0; d < sizeof domains / sizeof domains[0]; d++) {
        const toml_table_t *tab = toml_table_in(keys, domains[d]);
        if (tab && harvest_keys_domain(tab, domains[d], rank, origin, where) != 0)
            return -1;
    }

    /* [keys.mod.<id>] — one more level, and the ids are not knowable
     * here (mods are discovered after the config is read), so these are
     * forwarded by name without validation. */
    const toml_table_t *mods = toml_table_in(keys, "mod");
    if (mods) {
        for (int i = 0;; i++) {
            const char *mod_id = toml_key_in(mods, i);
            if (!mod_id) break;
            const toml_table_t *mt = toml_table_in(mods, mod_id);
            if (!mt) {
                fprintf(stderr, "openpete: config: %s: [keys.mod] '%s' must be "
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
                    fprintf(stderr, "openpete: config: %s: [keys.mod.%s] '%s' "
                                    "must be a string\n", where, mod_id, name);
                    return -1;
                }
                const char *tok = val;
                if (openpete_keymap_set_mod(mod_id, name, &tok, 1, rank,
                                            where) != 0)
                    return -1;
            }
        }
    }

    /* Anything else under [keys] is a typo. Silence here is how the
     * scoping got lost for [args]/[env] in the docs — name it instead. */
    for (int i = 0;; i++) {
        const char *key = toml_key_in(keys, i);
        if (!key) break;
        if (!strcmp(key, "pad") || !strcmp(key, "pad2") ||
            !strcmp(key, "pad3") || !strcmp(key, "pad4") ||
            !strcmp(key, "host") || !strcmp(key, "mod")) continue;
        fprintf(stderr, "openpete: config: %s: unknown [keys.%s] — expected "
                        "pad, pad2, pad3, pad4, host or mod.<id>\n", where, key);
        return -1;
    }
    return 0;
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

static int load_toml(int file) {
    const char *path = s_file_path[file];
    FILE *f = fopen(path, "r");
    if (!f) return 0;                       /* absent is the normal case */
    char err[256];
    toml_table_t *doc = toml_parse_file(f, err, sizeof err);
    fclose(f);
    if (!doc) {
        fprintf(stderr, "openpete: config: %s: %s\n", path, err);
        return -1;
    }
    s_doc[file] = doc;
    s_file_present[file] = 1;

    /* A portable install keeps everything it writes — this file's [saved]
     * zone, memory cards, savestates — next to the exe. Files load in
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
            fprintf(stderr, "openpete: config: %s: [disc.%s] and [disc.%s] both "
                            "match the running image — use a longer prefix\n",
                    s_file_path[file], match, key);
            return -1;
        }
        match = key;
    }
    if (!match) return 0;
    return harvest_named(file, "disc", match, TIER_DISC_SHA, "disc");
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
     * hole at its top. Nothing WRITES it yet — the managed-zone writer is
     * knob-id keyed and there is no rebind UI — and note that anything
     * hand-written below the managed marker is regenerated away on the
     * next menu save, so a hand-edited binding belongs in a normal scope
     * (global, [game.*], [disc.*]) rather than here. */
    const toml_table_t *keys = toml_table_in(tab, "keys");
    if (keys && harvest_keys(keys, TIER_SAVED, file, "saved", where) != 0)
        return -1;

    /* A managed zone in a file that outranks the write target would beat
     * everything the menus save from here on — the settings screen would
     * appear to do nothing. Ranks are fixed by file (user-dir beats
     * exe-dir), so this can only happen when the layout puts the target in
     * the lower-ranked file; say so rather than let saves vanish. */
    if (file > s_write_file)
        fprintf(stderr, "openpete: config: %s has a [saved] section that "
                        "outranks the save target %s — settings saved from "
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
        fprintf(stderr, "openpete: config: %s is no longer read; put these "
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

/** @brief Record every set schema env var as a TIER_REAL_ENV contribution.
 *
 * Runs before any [env] tier is applied, or the files' own variables would
 * read back as exported by the user. */
static void scan_real_env(void) {
    size_t n;
    const cfg_knob_t *sch = cfg_schema(&n);
    for (size_t i = 0; i < n; i++) {
        if (!sch[i].env) continue;
        const char *v = getenv(sch[i].env);
        if (!v) continue;
        contrib_t *c = contrib_new();
        if (!c) return;
        c->knob = (int)i;
        c->rank = RANK(TIER_REAL_ENV, 0, SP_ENV);
        c->spelling = SP_ENV;
        c->has_value = 1;
        snprintf(c->key, sizeof c->key, "%s", sch[i].env);
        snprintf(c->value, sizeof c->value, "%s", v);
        snprintf(c->origin, sizeof c->origin, "env");
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

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;                              /* retained for process lifetime */
}

static void emit_token(const char *tok, const char *origin) {
    if (s_token_n >= CFG_MAX_TOKENS) {
        fprintf(stderr, "openpete: config: too many flags to splice (max %d)\n",
                CFG_MAX_TOKENS);
        return;
    }
    char *d = dup_str(tok);
    if (!d) return;
    snprintf(s_token_origin[s_token_n], CFG_ORIGIN_MAX, "%s", origin);
    s_tokens[s_token_n++] = d;
}

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
 * carries. */
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
            portable_setenv_soft(c->key, c->value);
            continue;
        }

        /* A bare key for an env-only knob has to go out as its variable.
         * False becomes an ABSENT variable rather than "0": several of
         * these consumers only test for presence. */
        if (k && !k->cli) {
            if (k->type == CFG_BOOL &&
                (!strcmp(c->value, "0") || !strcmp(c->value, "off")))
                continue;
            portable_setenv_soft(k->env, c->value);
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

/** @brief Best guess at the disc image this launch boots, used only to pick the [disc.*] scopes.
 *
 * The last --disc or --ingest value in argv, else the value of the
 * highest-ranked `disc` contribution harvested so far (the tiers below the
 * [disc.*] scopes). A bare image path in argv is not considered. Returns a
 * static buffer, or NULL when neither source names an image. */
static const char *provisional_disc(int argc, char **argv) {
    static char path[CFG_PATH_MAX];
    path[0] = '\0';

    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--disc") || !strcmp(argv[i], "--ingest"))
            snprintf(path, sizeof path, "%s", argv[i + 1]);
    if (path[0]) return path;

    int best = -1, best_rank = -1;
    int disc_knob = cfg_schema_find_id("disc");
    for (int i = 0; i < s_contrib_n; i++) {
        if (s_contrib[i].knob != disc_knob) continue;
        if (s_contrib[i].rank <= best_rank) continue;
        best = i; best_rank = s_contrib[i].rank;
    }
    if (best >= 0) snprintf(path, sizeof path, "%s", s_contrib[best].value);
    return path[0] ? path : NULL;
}

/* ------------------------------------------------------------------ */
/* entry point                                                        */
/* ------------------------------------------------------------------ */

int openpete_config_report_requested(void) { return s_report_requested; }

const char *openpete_config_token_source(const char *token) {
    for (int i = 0; i < s_token_n; i++)
        if (s_tokens[i] == token || !strcmp(s_tokens[i], token))
            return s_token_origin[i];
    return NULL;
}

/** @brief Clear the resolver's state so the next openpete_config_apply starts from nothing.
 *
 * Only the unit test calls this: a process resolves its config once, and
 * the test walks many layer stacks. Frees the parsed documents and clears
 * the file paths, the contribution, token, decision and saved tables, the
 * save target, the disc scopes, the --no-config and --config-report
 * latches and the disc-identity hook, and drops the portable directive.
 * The game id and the keymap are kept; the spliced tokens are not freed. */
void openpete_config_reset_for_test(void) {
    for (int f = 0; f < 2; f++) {
        if (s_doc[f]) toml_free(s_doc[f]);
        s_doc[f] = NULL;
        s_file_path[f][0] = '\0';
        s_file_present[f] = 0;
    }
    s_contrib_n = s_token_n = s_decision_n = s_saved_n = 0;
    s_file_n = 0;
    s_write_file = 1;
    s_save_path[0] = s_scope_serial[0] = s_scope_sha[0] = '\0';
    s_no_config = s_report_requested = 0;
    s_disc_identity = NULL;
    openpete_paths_set_portable(-1);   /* drop the last run's directive */
}

int openpete_config_apply(int argc, char **argv, int typed_argc,
                          int *out_argc, char ***out_argv) {
    const char *explicit_path = NULL;

    /* Seed the keymap from its registry defaults before any [keys] table
     * is read, so config rows layer on top of a populated store rather
     * than defining it. Idempotent, and a no-op for a caller that never
     * uses the keymap. */
    openpete_keymap_init();

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

    /* The environment as the user actually exported it — captured before a
     * single [env] line is applied, so tier 8 can be told apart from the
     * file's own variables. */
    scan_real_env();

    /* argv already carries the bk2 sidecar (spliced by the caller), whose
     * tokens sit left of the typed ones — so the typed arguments are the
     * last typed_argc-1 entries. */
    int typed_from = argc - (typed_argc - 1);
    if (typed_from < 1) typed_from = 1;
    scan_launch(argc, argv, typed_from);

    char exedir[CFG_PATH_MAX] = "";
    int have_exedir = (openpete_exe_dir(exedir, sizeof exedir) == 0);

    if (!s_no_config) {
        if (explicit_path) {
            snprintf(s_file_path[0], CFG_PATH_MAX, "%s", explicit_path);
            s_file_n = 1;
            s_write_file = 0;
            if (!file_exists(explicit_path)) {
                fprintf(stderr, "openpete: --config %s: no such file\n", explicit_path);
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
            if (s_write_file == 1) {
                /* The write target has to exist before the [saved] flush;
                 * naming it above deliberately did not create it. */
                char userdir[CFG_PATH_MAX];
                (void)openpete_paths_user_config_dir(userdir, sizeof userdir, 1);
            }
        }

        for (int f = 0; f < s_file_n; f++) {
            if (!s_doc[f]) continue;
            if (harvest_global(f) != 0) return -1;
            if (harvest_named(f, "game", s_scope_game, TIER_GAME, "game") != 0) return -1;
        }
    }

    /* Now that the low tiers are in, we know which image will boot, which
     * is what selects the [disc.*] scopes. Cheap by contract: a serial read
     * or a library entry's recorded hash, never an image hash. */
    if (!s_no_config) {
        const char *disc = provisional_disc(argc, argv);
        if (s_disc_identity)
            (void)s_disc_identity(disc, s_scope_serial, sizeof s_scope_serial,
                                  s_scope_sha, sizeof s_scope_sha);
        for (int f = 0; f < s_file_n; f++) {
            if (!s_doc[f]) continue;
            if (harvest_named(f, "disc", s_scope_serial, TIER_DISC_SERIAL, "disc") != 0)
                return -1;
            if (harvest_disc_sha(f) != 0) return -1;
            if (harvest_saved(f) != 0) return -1;
        }
    }

    if (s_write_file >= 0 && s_file_path[s_write_file][0])
        snprintf(s_save_path, sizeof s_save_path, "%s", s_file_path[s_write_file]);

    /* Every [keys] layer is in; pick winners and run the conflict +
     * reachability scans. Never fails a boot — a clash is a warning, and
     * an unreachable control is the user's call to make. */
    (void)openpete_keymap_resolve();

    materialize();

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

void openpete_config_set_game_id(const char *id) {
    snprintf(s_scope_game, sizeof s_scope_game, "%s", id ? id : "");
}

/* ------------------------------------------------------------------ */
/* report                                                             */
/* ------------------------------------------------------------------ */

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
    fprintf(out, "  save target: %s\n", s_save_path[0] ? s_save_path : "(none)");

    /* Where the things openpete writes go, which the knob table below
     * never shows: they are directories, not knobs. Worth printing in
     * full because the answer moves with the layout — "where did my disc
     * library go?" should be answerable without reading the source. */
    {
        char dir[CFG_PATH_MAX];
        fprintf(out, "  layout: %s\n",
                openpete_paths_portable() ? "portable (next to the exe)"
                                          : "per-user directories");
        static const char* const kState[] = { "cards", "states", "logs" };
        static const char* const kData[]  = { "library", "mods", "assets", "bk2",
                                             "screenshots" };
        for (size_t i = 0; i < sizeof kState / sizeof kState[0]; i++)
            if (openpete_paths_state_dir(kState[i], dir, sizeof dir, 0) == 0)
                fprintf(out, "  %-8s %s\n", kState[i], dir);
        for (size_t i = 0; i < sizeof kData / sizeof kData[0]; i++)
            if (openpete_paths_data_dir(kData[i], dir, sizeof dir, 0) == 0)
                fprintf(out, "  %-8s %s\n", kData[i], dir);
    }

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

    char tmp[CFG_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_save_path);
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        fprintf(stderr, "openpete: config: cannot write %s: %s\n",
                tmp, strerror(errno));
        free(head);
        return -1;
    }
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
    fclose(out);

    remove(s_save_path);                  /* Windows rename won't clobber */
    if (rename(tmp, s_save_path) != 0) {
        fprintf(stderr, "openpete: config: cannot replace %s: %s\n",
                s_save_path, strerror(errno));
        return -1;
    }
    fprintf(stderr, "openpete: config: saved %d setting(s) to %s\n",
            s_saved_n, s_save_path);
    return 0;
}

int openpete_config_reset_saved(void) {
    s_saved_n = 0;
    return openpete_config_save_flush();
}

