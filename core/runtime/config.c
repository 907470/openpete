/*
 * core/runtime/config.c — openpete.toml: load, resolve, materialize.
 *
 * The precedence chain and the reason flattening is required (rather than
 * splicing every layer and letting each consumer's own CLI-beats-env rule
 * decide) are documented in openpete_config.h. Read that first.
 *
 * Runs before the logger is configured — --log-level and --log-format are
 * themselves config knobs — so diagnostics here go straight to stderr,
 * the same convention options.c uses.
 *
 * Deliberately SDL-free: the exe directory is resolved with the platform
 * call directly (the same thing SDL_GetBasePath does, and the same thing
 * disc_library.cpp already does) so the resolver can be linked into a
 * standalone unit test without dragging SDL into the link line.
 */

#include "openpete_config.h"
#include "config_schema.h"

#include "../../third_party/tomlc99/toml.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define OPCFG_MKDIR(p) _mkdir(p)
#define OPCFG_SEP '\\'
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#define OPCFG_MKDIR(p) mkdir((p), 0777)
#define OPCFG_SEP '/'
#endif

/* ------------------------------------------------------------------ */
/* tiers                                                              */
/* ------------------------------------------------------------------ */

enum {
    TIER_LEGACY_ENV = 1,
    TIER_LEGACY_OPTS,
    TIER_GLOBAL,
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

static void path_join(char *out, size_t cap, const char *dir, const char *leaf) {
    size_t n = strlen(dir);
    if (n && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
        snprintf(out, cap, "%s%s", dir, leaf);
    else
        snprintf(out, cap, "%s%c%s", dir, OPCFG_SEP, leaf);
}

static int exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    char buf[CFG_PATH_MAX];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof buf);
    if (n == 0 || n >= sizeof buf) return -1;
    char *slash = strrchr(buf, '\\');
    if (!slash) slash = strrchr(buf, '/');
    if (!slash) return -1;
    *slash = '\0';
    snprintf(out, cap, "%s", buf);
    return 0;
#else
    char buf[CFG_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char *slash = strrchr(buf, '/');
    if (!slash) return -1;
    *slash = '\0';
    snprintf(out, cap, "%s", buf);
    return 0;
#endif
}

/* $XDG_CONFIG_HOME/openpete (or %APPDATA%\openpete), created as needed.
 * Mirrors savestate.c's resolve_state_dir, but under config rather than
 * state: savestates and memory cards are data, this is preference. */
static int user_config_dir(char *out, size_t cap) {
    const char *base = NULL;
    char fallback[CFG_PATH_MAX];
#ifdef _WIN32
    base = getenv("APPDATA");
    if (!base || !*base) {
        const char *up = getenv("USERPROFILE");
        if (!up || !*up) return -1;
        snprintf(fallback, sizeof fallback, "%s\\AppData\\Roaming", up);
        base = fallback;
    }
#else
    base = getenv("XDG_CONFIG_HOME");
    if (!base || !*base) {
        const char *home = getenv("HOME");
        if (!home || !*home) return -1;
        snprintf(fallback, sizeof fallback, "%s/.config", home);
        base = fallback;
    }
#endif
    (void)OPCFG_MKDIR(base);
    path_join(out, cap, base, "openpete");
    (void)OPCFG_MKDIR(out);
    return 0;
}

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ */
/* contribution table                                                 */
/* ------------------------------------------------------------------ */

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

/* True when `a` and `b` name the same setting: the same schema knob, or —
 * for entries with no schema row — the same literal key. */
static int same_setting(const contrib_t *a, const contrib_t *b) {
    if (a->knob >= 0 || b->knob >= 0) return a->knob == b->knob;
    return a->spelling == b->spelling && !strcmp(a->key, b->key);
}

/* ------------------------------------------------------------------ */
/* value normalization                                                */
/* ------------------------------------------------------------------ */

/* Render one TOML scalar into the flag vocabulary. Returns 0 on success,
 * -1 on a type the config never accepts (arrays, tables, timestamps).
 * `is_bool` reports a bare true/false, which the [args] spelling reads as
 * "emit the flag" / "suppress the flag" rather than as a value. */
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

/* Levenshtein-lite: enough to say "did you mean" for a one-character slip
 * or a dash/underscore mixup, which is what config typos actually are. */
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

/* The one top-level word that is a directive rather than a knob.
 *
 * The sub-table namespaces (env, args, game, disc, saved) need no
 * reserving: this walk only ever looks at scalars, and TOML cannot spell
 * `disc` as both a scalar and a table in one document. So a global
 * `disc = "path/to.cue"` is the path knob and `[disc.SCUS-94228]` is the
 * scope namespace, with the file itself deciding which one it meant. */
static int is_directive_key(const char *key) {
    return !strcmp(key, "portable");
}

/* Bare keys of `tab` — the friendly, validated spelling. */
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

/* [<scope>.env] — becomes environment variables. Free-form on purpose:
 * the ~150 debug OPENPETE_* vars have no schema rows and shouldn't need
 * any to be settable from a file. */
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

/* [<scope>.args] — keys are flags verbatim. A boolean value means the
 * flag takes none: true emits it, false suppresses whatever a lower tier
 * asked for (the only way to un-set an inherited valueless flag). Flags
 * with a schema row know their own arity, so a boolean there is just the
 * 0/1 the flag expects. */
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

/* One scope = its bare keys, then its [env] and [args] sub-tables. */
static int harvest_scope(const toml_table_t *tab, int tier, int file,
                         const char *origin) {
    char where[CFG_PATH_MAX + 64];
    snprintf(where, sizeof where, "%s [%s]", s_file_path[file], origin);

    if (harvest_base(tab, tier, file, origin, where, 0) != 0) return -1;

    const toml_table_t *env = toml_table_in(tab, "env");
    if (env && harvest_env(env, tier, file, origin, where) != 0) return -1;

    const toml_table_t *args = toml_table_in(tab, "args");
    if (args && harvest_args(args, tier, file, origin, where) != 0) return -1;
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

    /* A portable install keeps its settings next to the exe. */
    toml_raw_t p = toml_raw_in(doc, "portable");
    int b = 0;
    if (p && toml_rtob(p, &b) == 0 && b) s_write_file = 0;
    return 1;
}

/* Global scope: the document's own bare keys (minus the reserved
 * sub-tables) plus top-level [env] / [args]. */
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

/* [disc.<sha>] may be written as any unambiguous >=8-char prefix, git
 * style. An ambiguous one is an error: silently picking a side would make
 * the config mean different things as the library grows. */
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

/* The managed zone. Read like any other layer; written by the menus. */
static int harvest_saved(int file) {
    toml_table_t *doc = s_doc[file];
    if (!doc) return 0;
    const toml_table_t *tab = toml_table_in(doc, "saved");
    if (!tab) return 0;

    char where[CFG_PATH_MAX + 32];
    snprintf(where, sizeof where, "%s [saved]", s_file_path[file]);
    if (harvest_base(tab, TIER_SAVED, file, "saved", where, 0) != 0) return -1;

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
/* legacy openpete.opts / openpete.env                                */
/* ------------------------------------------------------------------ */

/* Every openpete flag starts with "--", so a following token that doesn't
 * is this flag's value. That keeps negative numbers ("--bk2-frame-offset
 * -100") working, which an "any leading dash" rule would break. */
static int token_is_flag(const char *t) { return t && t[0] == '-' && t[1] == '-'; }

static int legacy_opts_load(const char *dir) {
    char path[CFG_PATH_MAX];
    path_join(path, sizeof path, dir, "openpete.opts");
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fprintf(stderr, "openpete: config: %s is deprecated — run "
                    "--convert-config to move it into openpete.toml\n", path);

    char line[4096];
    char *toks[128];
    int ntok = 0;
    while (fgets(line, sizeof line, f) && ntok < 128) {
        char *p = line;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) break;
            if (*p == '#') { while (*p && *p != '\n') p++; continue; }
            char *start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
            if (*p) *p++ = '\0';
            if (ntok < 128) {
                char *dup = (char *)malloc(strlen(start) + 1);
                if (!dup) { fclose(f); return -1; }
                strcpy(dup, start);
                toks[ntok++] = dup;
            }
        }
    }
    fclose(f);

    for (int i = 0; i < ntok; i++) {
        const char *flag = toks[i];
        if (!token_is_flag(flag)) {
            fprintf(stderr, "openpete: config: %s: stray token '%s' (expected a "
                            "--flag)\n", path, flag);
            return -1;
        }
        int ki = cfg_schema_find_cli(flag);
        const cfg_knob_t *k = (ki >= 0) ? &cfg_schema(NULL)[ki] : NULL;
        int is_alias = (k && k->alias && !strcmp(k->alias, flag));
        int takes_value = !(k && (k->flag_valueless || is_alias));

        const char *val = NULL;
        if (takes_value && i + 1 < ntok && !token_is_flag(toks[i + 1]))
            val = toks[++i];

        contrib_t *c = contrib_new();
        if (!c) return -1;
        c->knob = ki;
        c->rank = RANK(TIER_LEGACY_OPTS, 0, SP_ARGS);
        c->spelling = SP_ARGS;
        snprintf(c->key, sizeof c->key, "%s", flag);
        snprintf(c->origin, sizeof c->origin, "openpete.opts");
        if (is_alias) {
            c->has_value = 1;
            snprintf(c->value, sizeof c->value, "%s", k->alias_value);
        } else if (val) {
            c->has_value = 1;
            snprintf(c->value, sizeof c->value, "%s", val);
        } else {
            c->valueless = 1;
        }
    }
    return 1;
}

static int legacy_env_load(const char *dir) {
    static const char *const names[] = { "openpete.env", ".env" };
    char path[CFG_PATH_MAX];
    FILE *f = NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !f; i++) {
        path_join(path, sizeof path, dir, names[i]);
        f = fopen(path, "r");
    }
    if (!f) return 0;

    fprintf(stderr, "openpete: config: %s is deprecated — run "
                    "--convert-config to move it into openpete.toml\n", path);

    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n' || *p == '\r') continue;
        if (!strncmp(p, "export ", 7)) p += 7;
        char *eq = strchr(p, '=');
        if (!eq || eq == p) continue;
        char *ke = eq;
        while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        *ke = '\0';
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *ve = v + strlen(v);
        while (ve > v && (ve[-1] == '\n' || ve[-1] == '\r' ||
                          ve[-1] == ' '  || ve[-1] == '\t')) ve--;
        *ve = '\0';
        if (ve - v >= 2 && (v[0] == '"' || v[0] == '\'') && ve[-1] == v[0]) {
            ve[-1] = '\0'; v++;
        }

        /* Refuse rather than truncate: a silently shortened variable name
         * would set a DIFFERENT variable, which is worse than not setting
         * one. Nothing real comes close to these bounds. */
        size_t klen = strlen(p), vlen = strlen(v);
        if (klen >= sizeof ((contrib_t *)0)->key ||
            vlen >= sizeof ((contrib_t *)0)->value) {
            fprintf(stderr, "openpete: config: %s: skipping over-long entry "
                            "'%.32s...'\n", path, p);
            continue;
        }

        contrib_t *c = contrib_new();
        if (!c) { fclose(f); return -1; }
        c->knob = cfg_schema_find_env(p);
        c->rank = RANK(TIER_LEGACY_ENV, 0, SP_ENV);
        c->spelling = SP_ENV;
        c->has_value = 1;
        memcpy(c->key, p, klen + 1);
        memcpy(c->value, v, vlen + 1);
        snprintf(c->origin, sizeof c->origin, "openpete.env");
    }
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ */
/* launch tiers                                                       */
/* ------------------------------------------------------------------ */

/* Flags already present in argv (from the bk2 sidecar or typed) and env
 * vars already exported: these outrank every file tier, so their only
 * effect here is to suppress the file's copy. Recorded as contributions
 * so one comparison decides everything. */
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

/* Must run BEFORE any [env] tier is applied, or the file's own variables
 * would read back as if the user had exported them. */
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

/* True when some higher-ranked contribution supplies the same setting.
 * Ties cannot happen: two entries at the same rank would be the same key
 * in the same table, which TOML rejects. */
static int is_shadowed(const contrib_t *c) {
    for (int i = 0; i < s_contrib_n; i++) {
        const contrib_t *o = &s_contrib[i];
        if (o == c) continue;
        if (!same_setting(c, o)) continue;
        if (o->rank > c->rank) return 1;
    }
    return 0;
}

/* A config-sourced flag also stands down when a higher tier supplied a
 * flag it cannot coexist with. */
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

/* The flattening pass. Every surviving contribution is materialized in
 * ITS OWN spelling — an [env] winner becomes a setenv, a bare key or an
 * [args] winner becomes argv tokens — because the two vocabularies are
 * not interchangeable (OPENPETE_PRESENT_MODE says "immediate", --vsync
 * says 0). Exactly one spelling per setting reaches the parser, so no
 * consumer's own CLI-beats-env rule can re-order the tiers. */
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

/* Best guess at the image this launch will boot, from everything ranked
 * below the [disc.*] scopes. Only used to pick those scopes. */
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

/* Clear every resolver static. Only the unit test calls this: a process
 * resolves its config exactly once, but the test walks dozens of layer
 * stacks and each has to start from nothing. */
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
}

int openpete_config_apply(int argc, char **argv, int typed_argc,
                          int *out_argc, char ***out_argv) {
    const char *explicit_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-config")) s_no_config = 1;
        else if (!strcmp(argv[i], "--config-report")) s_report_requested = 1;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) explicit_path = argv[i + 1];
        /* Converting is a file operation, not a launch: reading the legacy
         * files here would warn about them twice and apply settings to a
         * process whose only job is to rewrite them. */
        else if (!strcmp(argv[i], "--convert-config")) s_no_config = 1;
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
    int have_exedir = (exe_dir(exedir, sizeof exedir) == 0);

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
                path_join(s_file_path[0], CFG_PATH_MAX, exedir, "openpete.toml");
            char userdir[CFG_PATH_MAX];
            if (user_config_dir(userdir, sizeof userdir) == 0)
                path_join(s_file_path[1], CFG_PATH_MAX, userdir, "openpete.toml");
            s_file_n = 2;
        }

        /* Legacy files sit below every toml tier: the toml is the
         * intentional config, these are on their way out. */
        if (!explicit_path && have_exedir) {
            if (legacy_env_load(exedir) < 0) return -1;
            if (legacy_opts_load(exedir) < 0) return -1;
        }

        for (int f = 0; f < s_file_n; f++) {
            if (!s_file_path[f][0]) continue;
            if (load_toml(f) < 0) return -1;
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

/* A saved value is written as the type its knob declares, so the file
 * round-trips through the reader unchanged (`fullscreen = true`, not
 * `fullscreen = "1"`). */
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

/* ------------------------------------------------------------------ */
/* --convert-config                                                   */
/* ------------------------------------------------------------------ */

static int rename_bak(const char *path) {
    char bak[CFG_PATH_MAX + 8];
    snprintf(bak, sizeof bak, "%s.bak", path);
    remove(bak);
    return rename(path, bak);
}

int openpete_config_convert_legacy(void) {
    char dir[CFG_PATH_MAX];
    if (exe_dir(dir, sizeof dir) != 0) {
        fprintf(stderr, "openpete: --convert-config: cannot locate the executable\n");
        return 3;
    }
    char opts_path[CFG_PATH_MAX], env_path[CFG_PATH_MAX], out_path[CFG_PATH_MAX];
    path_join(opts_path, sizeof opts_path, dir, "openpete.opts");
    path_join(env_path, sizeof env_path, dir, "openpete.env");
    path_join(out_path, sizeof out_path, dir, "openpete.toml");

    int have_opts = file_exists(opts_path);
    int have_env  = file_exists(env_path);
    if (!have_opts && !have_env) {
        fprintf(stderr, "openpete: --convert-config: nothing to convert "
                        "(no openpete.opts or openpete.env next to the exe)\n");
        return 0;
    }
    if (file_exists(out_path)) {
        fprintf(stderr, "openpete: --convert-config: %s already exists — move it "
                        "aside first (this would overwrite it)\n", out_path);
        return 3;
    }

    /* Reuse the loaders: they already know the two syntaxes, and reading
     * through them means the conversion cannot disagree with the runtime
     * about what the legacy files said. */
    s_contrib_n = 0;
    if (have_env  && legacy_env_load(dir)  < 0) return 3;
    if (have_opts && legacy_opts_load(dir) < 0) return 3;

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "openpete: --convert-config: cannot write %s: %s\n",
                out_path, strerror(errno));
        return 3;
    }
    fprintf(f, "# openpete.toml — converted from openpete.opts / openpete.env.\n"
               "# Both legacy files mapped onto the global scope: their flags\n"
               "# and variables mean exactly what they meant before.\n");

    int wrote_env = 0, wrote_args = 0;
    for (int i = 0; i < s_contrib_n; i++) {
        if (s_contrib[i].spelling != SP_ENV) continue;
        if (!wrote_env) { fprintf(f, "\n[env]\n"); wrote_env = 1; }
        fprintf(f, "%s = \"%s\"\n", s_contrib[i].key, s_contrib[i].value);
    }
    for (int i = 0; i < s_contrib_n; i++) {
        const contrib_t *c = &s_contrib[i];
        if (c->spelling != SP_ARGS) continue;
        if (!wrote_args) { fprintf(f, "\n[args]\n"); wrote_args = 1; }
        if (c->valueless) fprintf(f, "\"%s\" = true\n", c->key);
        else              fprintf(f, "\"%s\" = \"%s\"\n", c->key, c->value);
    }
    fclose(f);

    if (have_opts) rename_bak(opts_path);
    if (have_env)  rename_bak(env_path);

    fprintf(stderr, "openpete: --convert-config: wrote %s\n", out_path);
    if (have_opts) fprintf(stderr, "openpete:   openpete.opts -> openpete.opts.bak\n");
    if (have_env)  fprintf(stderr, "openpete:   openpete.env  -> openpete.env.bak\n");
    fprintf(stderr, "openpete: verify with --config-report before deleting the .bak files\n");
    s_contrib_n = 0;
    return 0;
}
