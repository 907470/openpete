/* core/input/keymap.c — the action registry, the layered binding store,
 * and the resolver. See core/include/openpete_keymap.h for what this is
 * for and why it is SDL-free.
 *
 * Deliberately free of SDL and of config.c: config.c hands bindings in as
 * (domain, action, tokens, rank, origin) and gets an error back, so the
 * resolver links into console unit tests alongside the config resolver
 * with nothing but tomlc99 in the link line. */

#include "openpete_host_mouse.h"   /* OPENPETE_MOUSE_OWNED_* */
#include "openpete_input_log.h"
#include "openpete_keymap.h"
#include "openpete_log.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* the registry                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char*   id;
    unsigned char domain;
    unsigned char kind;
    unsigned char ctx;
    unsigned      flags;
    const char*   def;
    const char*   purpose;
} km_action_def_t;

#define KM_ROW_HOST(sym, id, k, c, f, d, p) { id, KM_DOM_HOST, k, c, f, d, p },
#define KM_ROW_PAD(sym, id, k, c, f, d, p)  { id, KM_DOM_PAD,  k, c, f, d, p },
/* Players 2-4 default to UNBOUND and carry no flags: giving them P1's
 * defaults would make every row collide with its P1 twin, and KM_REQUIRED on
 * a surface nobody has configured would warn on every stock boot. Their
 * devices are decoded through the shared controller merge, so an extra
 * player has working buttons with nothing bound here; these rows exist to
 * re-map that player, which the decode does not consult yet. */
#define KM_ROW_PAD2(sym, id, k, c, f, d, p) { id, KM_DOM_PAD2, k, c, 0u, "none", p },
#define KM_ROW_PAD3(sym, id, k, c, f, d, p) { id, KM_DOM_PAD3, k, c, 0u, "none", p },
#define KM_ROW_PAD4(sym, id, k, c, f, d, p) { id, KM_DOM_PAD4, k, c, 0u, "none", p },

static const km_action_def_t g_actions[] = {
    OPENPETE_KEYMAP_HOST_ACTIONS(KM_ROW_HOST)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ROW_PAD)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ROW_PAD2)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ROW_PAD3)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ROW_PAD4)
};

/* One row per action; the enum and this table are generated from the same
 * X-macro lists, so a mismatch is a compile error rather than a silent
 * off-by-one into the wrong binding. */
typedef char km_assert_registry_len[
    (sizeof g_actions / sizeof g_actions[0]) == OPK_ACTION_COUNT ? 1 : -1];

#define KM_ORIGIN_MAX 48

typedef struct {
    km_source_t src[KM_MAX_SOURCES];
    int         n;
    int         rank;                    /* -1 = still the built-in default */
    char        origin[KM_ORIGIN_MAX];
    char        shadowed[KM_ORIGIN_MAX]; /* previous winner, for the report */
    unsigned char unbound_by_config;     /* "none"/[] rather than KM_DEV */
} km_binding_t;

static km_binding_t g_bind[OPK_ACTION_COUNT];

/* Per-action wheel pulses and mouse motion counts of the current poll. */
static unsigned  g_pulses[OPK_ACTION_COUNT];
static long long g_delta[OPK_ACTION_COUNT];

/* ------------------------------------------------------------------ */
/* seams                                                              */
/* ------------------------------------------------------------------ */

static openpete_keymap_name_to_code_fn  s_to_code;
static openpete_keymap_code_to_name_fn  s_to_name;
static openpete_keymap_key_label_fn     s_key_label;
static openpete_keymap_input_fns        s_in;

void openpete_keymap_set_lookup(openpete_keymap_name_to_code_fn to_code,
                                openpete_keymap_code_to_name_fn to_name,
                                openpete_keymap_key_label_fn label) {
    s_to_code = to_code; s_to_name = to_name; s_key_label = label;
}

void openpete_keymap_set_input(const openpete_keymap_input_fns* fns) {
    if (fns) s_in = *fns;
    else memset(&s_in, 0, sizeof s_in);
}

/* Scancodes whose current press the event filter already acted on. Held
 * until physical release. A tiny fixed list rather than a 512-entry bitmap
 * because only ever a key or two is in flight, and the clear pass below
 * walks it every poll. */
#define KM_MAX_CONSUMED 8
static short g_consumed[KM_MAX_CONSUMED];
static int   g_consumed_n;

void openpete_keymap_consume_key(int scancode) {
    for (int i = 0; i < g_consumed_n; i++)
        if (g_consumed[i] == (short)scancode) return;
    if (g_consumed_n < KM_MAX_CONSUMED)
        g_consumed[g_consumed_n++] = (short)scancode;
}

static int km_key_consumed(int scancode) {
    for (int i = 0; i < g_consumed_n; i++)
        if (g_consumed[i] == (short)scancode) return 1;
    return 0;
}

/** @brief Drop consumed keys that are no longer physically held, so the next press of the key is live again.
 *
 * Reads the raw input seam (s_in.key_down) rather than km_src_down, which
 * consults the consumed list and would never let an entry clear. With no
 * key_down seam installed every entry clears. Called at the start of every
 * openpete_keymap_poll. */
static void km_release_consumed(void) {
    for (int i = 0; i < g_consumed_n; ) {
        int down = s_in.key_down ? s_in.key_down(g_consumed[i]) : 0;
        if (down) { i++; continue; }
        g_consumed[i] = g_consumed[--g_consumed_n];
    }
}

/* ------------------------------------------------------------------ */
/* token parsing                                                      */
/* ------------------------------------------------------------------ */

/* SDL has no mouse-button name table, so the `mouse:` vocabulary lives
 * here and the lookup seam is untouched for the two mouse kinds. */
static const struct { const char* name; unsigned char kind; short code; }
g_mouse_names[] = {
    { "left",       KM_SRC_MOUSEBTN,  KM_MOUSE_LEFT },
    { "middle",     KM_SRC_MOUSEBTN,  KM_MOUSE_MIDDLE },
    { "right",      KM_SRC_MOUSEBTN,  KM_MOUSE_RIGHT },
    { "x1",         KM_SRC_MOUSEBTN,  KM_MOUSE_X1 },
    { "x2",         KM_SRC_MOUSEBTN,  KM_MOUSE_X2 },
    { "wheel_up",   KM_SRC_MOUSEBTN,  KM_MOUSE_WHEEL_UP },
    { "wheel_down", KM_SRC_MOUSEBTN,  KM_MOUSE_WHEEL_DOWN },
    { "x",          KM_SRC_MOUSEAXIS, KM_MOUSE_X },
    { "y",          KM_SRC_MOUSEAXIS, KM_MOUSE_Y },
};

static const char* km_mouse_name(int kind, int code) {
    for (size_t i = 0; i < sizeof g_mouse_names / sizeof g_mouse_names[0]; i++)
        if (g_mouse_names[i].kind == kind && g_mouse_names[i].code == code)
            return g_mouse_names[i].name;
    return NULL;
}

static int km_is_mouse_kind(int kind) {
    return kind == KM_SRC_MOUSEBTN || kind == KM_SRC_MOUSEAXIS;
}

static int km_find(const char* domain, const char* action);
static int km_streq_ci(const char* a, const char* b);

static int km_streq_ci(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static void km_trim(char* s) {
    char* p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

/** @brief Parse one binding token into a source.
 *
 * The token is trimmed and truncated to 63 characters. An empty token or
 * "none" (any case) binds nothing. "pad:" is player 1 unqualified (device
 * 0); "pad1:".."pad4:" set device to the player number. A pad name is tried
 * as a button, then as an axis; a leading '-' inverts an axis and is an
 * error on a button. "mouse:" sources are refused in the pad domains; a
 * mouse axis binds only an action flagged KM_DELTA and never a mod
 * binding; a wheel pulse binds neither a mod binding nor an axis or
 * KM_LEVEL action. Any other name is a keyboard scancode. Every refusal is
 * logged under "keys" with where, domain and action.
 * @param tok the token text.
 * @param src receives the source on success.
 * @param where the config file or manifest named in error messages.
 * @param domain the binding domain ("host", "pad".."pad4", "mod.<id>").
 * @param action the action or mod binding name.
 * @return 1 when src is filled, 0 for an empty or "none" token, -1 for a
 *         token that names no source or is refused. */
static int km_parse_token(const char* tok, km_source_t* src, const char* where,
                          const char* domain, const char* action) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s", tok);
    km_trim(buf);
    if (!buf[0] || km_streq_ci(buf, "none")) return 0;

    memset(src, 0, sizeof *src);

    /* "pad:" is player 1; "pad2:".."pad4:" address the other host pad slots.
     * The digit is the player number, so the slot index is one less. */
    int pad_device = -1;
    const char* pad_name = NULL;
    if (strncmp(buf, "pad:", 4) == 0) {
        pad_device = 0;                      /* unqualified */
        pad_name = buf + 4;
    } else if (strncmp(buf, "pad", 3) == 0 &&
               buf[3] >= '1' && buf[3] <= '0' + OPENPETE_PAD_SLOTS &&
               buf[4] == ':') {
        pad_device = buf[3] - '0';           /* 1-based player number */
        pad_name = buf + 5;
    }

    if (pad_device >= 0) {
        const char* name = pad_name;
        int invert = 0;
        if (*name == '-') { invert = 1; name++; }
        src->device = (unsigned char)pad_device;

        /* Buttons first, then axes: SDL names them from disjoint tables,
         * and trying the button table first means "pad:start" is never
         * mistaken for an axis. */
        int code = s_to_code ? s_to_code(KM_SRC_PADBTN, name) : -1;
        if (code >= 0) {
            if (invert) {
                OPL_ERROR_F("keys", "%s: %s.%s: '-' inverts an axis, and "
                            "'%s' is a button", where, domain, action, name);
                return -1;
            }
            src->kind = KM_SRC_PADBTN;
            src->code = (short)code;
            return 1;
        }
        code = s_to_code ? s_to_code(KM_SRC_PADAXIS, name) : -1;
        if (code >= 0) {
            src->kind   = KM_SRC_PADAXIS;
            src->code   = (short)code;
            src->invert = (signed char)(invert ? -1 : 1);
            return 1;
        }
        OPL_ERROR_F("keys", "%s: %s.%s: no gamepad button or axis named '%s'",
                    where, domain, action, name);
        return -1;
    }

    /* "mouse:" sources. Accepted in the host and mod.<id> domains only: the
     * player-1 pad domain is pushed to PsyCross as gamepad codes and the
     * padN reader has no mouse member, so a mouse source there could never
     * be read. */
    if (strncmp(buf, "mouse:", 6) == 0) {
        const char* name = buf + 6;
        int invert = 0;
        if (*name == '-') { invert = 1; name++; }
        if (km_streq_ci(domain, "pad")  || km_streq_ci(domain, "pad2") ||
            km_streq_ci(domain, "pad3") || km_streq_ci(domain, "pad4")) {
            OPL_ERROR_F("keys", "%s: %s.%s: mouse sources bind host and mod "
                        "actions only; the pad domains reach the game as "
                        "gamepad codes", where, domain, action);
            return -1;
        }
        const struct { const char* name; unsigned char kind; short code; }* m = NULL;
        for (size_t i = 0; i < sizeof g_mouse_names / sizeof g_mouse_names[0]; i++)
            if (km_streq_ci(g_mouse_names[i].name, name)) {
                m = (const void*)&g_mouse_names[i];
                break;
            }
        if (!m) {
            OPL_ERROR_F("keys", "%s: %s.%s: no mouse button or axis named '%s' "
                        "(left right middle x1 x2 wheel_up wheel_down x y)",
                        where, domain, action, name);
            return -1;
        }
        const int is_mod = strncmp(domain, "mod.", 4) == 0;
        const int idx = is_mod ? -1 : km_find(domain, action);
        const unsigned flags = idx >= 0 ? g_actions[idx].flags : 0u;
        const int akind = idx >= 0 ? (int)g_actions[idx].kind : KM_DIGITAL;

        if (m->kind == KM_SRC_MOUSEAXIS) {
            /* A displacement, not a level: legal only where a consumer reads
             * openpete_key_delta, or it would bind and do nothing. */
            if (is_mod) {
                OPL_ERROR_F("keys", "%s: %s.%s: 'mouse:%s' is motion; a mod "
                            "reads motion through mouse_read, not a binding",
                            where, domain, action, name);
                return -1;
            }
            if (idx >= 0 && !(flags & KM_DELTA)) {
                OPL_ERROR_F("keys", "%s: %s.%s: 'mouse:%s' is a displacement "
                            "and %s reads a %s; only look_axis_x and "
                            "look_axis_y take mouse motion", where, domain,
                            action, name, action,
                            akind == KM_AXIS ? "level axis" : "button");
                return -1;
            }
            src->kind   = KM_SRC_MOUSEAXIS;
            src->code   = m->code;
            src->invert = (signed char)(invert ? -1 : 1);
            return 1;
        }
        if (invert) {
            OPL_ERROR_F("keys", "%s: %s.%s: '-' inverts an axis, and "
                        "'mouse:%s' is a button", where, domain, action, name);
            return -1;
        }
        if (KM_MOUSE_IS_PULSE(m->code)) {
            /* A wheel notch is a pulse with no level. binding_down is a level
             * read from a game-tick hook, and a KM_LEVEL action is read
             * through openpete_key_down; neither can ever see it. */
            if (is_mod) {
                OPL_ERROR_F("keys", "%s: %s.%s: 'mouse:%s' is a pulse and a "
                            "mod binding is read as a level", where, domain,
                            action, name);
                return -1;
            }
            if (idx >= 0 && (akind == KM_AXIS || (flags & KM_LEVEL))) {
                OPL_ERROR_F("keys", "%s: %s.%s: 'mouse:%s' is a pulse and %s "
                            "is read as a level", where, domain, action, name,
                            action);
                return -1;
            }
        }
        src->kind = KM_SRC_MOUSEBTN;
        src->code = m->code;
        return 1;
    }

    /* Bare name == keyboard scancode. The `pad:` prefix is mandatory
     * precisely so this branch never has to guess: SDL's scancode and
     * gamepad-button name tables both contain "a", "b", "back" and
     * "start". */
    {
        int code = s_to_code ? s_to_code(KM_SRC_KEY, buf) : -1;
        if (code >= 0) {
            src->kind = KM_SRC_KEY;
            src->code = (short)code;
            return 1;
        }
    }
    OPL_ERROR_F("keys", "%s: %s.%s: no key named '%s' (gamepad bindings "
                "need a pad prefix, e.g. \"pad:south\" for player 1 or "
                "\"pad2:south\" for player 2)",
                where, domain, action, buf);
    return -1;
}

/** @brief Split binding values on commas and parse each piece with km_parse_token.
 *
 * A scalar "C,pad:south" and an array ["C", "pad:south"] give the same
 * binding; the registry's defaults use the scalar form, so defaults and
 * config share this path. NULL tokens are skipped, each piece is truncated
 * to 63 characters, and sources past KM_MAX_SOURCES are dropped with a
 * warning.
 * @param tokens the values as written.
 * @param n number of tokens.
 * @param out receives up to KM_MAX_SOURCES sources.
 * @param out_n receives the number of sources parsed, including those
 *        parsed before a failure.
 * @param where the config file or manifest named in error messages.
 * @param domain the binding domain.
 * @param action the action or mod binding name.
 * @return 0, or -1 at the first piece km_parse_token rejects. */
static int km_parse_list(const char* const* tokens, int n,
                         km_source_t* out, int* out_n,
                         const char* where, const char* domain,
                         const char* action) {
    *out_n = 0;
    for (int i = 0; i < n; i++) {
        const char* s = tokens[i];
        if (!s) continue;
        while (*s) {
            const char* comma = strchr(s, ',');
            char piece[64];
            size_t len = comma ? (size_t)(comma - s) : strlen(s);
            if (len >= sizeof piece) len = sizeof piece - 1;
            memcpy(piece, s, len);
            piece[len] = '\0';

            km_source_t src;
            int r = km_parse_token(piece, &src, where, domain, action);
            if (r < 0) return -1;
            if (r > 0) {
                if (*out_n >= KM_MAX_SOURCES) {
                    OPL_WARN_F("keys", "%s: %s.%s: more than %d sources; "
                               "ignoring the rest", where, domain, action,
                               KM_MAX_SOURCES);
                } else {
                    out[(*out_n)++] = src;
                }
            }
            if (!comma) break;
            s = comma + 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* init                                                               */
/* ------------------------------------------------------------------ */

/** @brief Return the config spelling of a binding domain.
 *
 * @param d a KM_DOM_* value.
 * @return "pad", "pad2", "pad3" or "pad4" for the pad domains, "host" for
 *         any other value. */
static const char* km_domain_name(int d) {
    return d == KM_DOM_PAD  ? "pad"  : d == KM_DOM_PAD2 ? "pad2"
         : d == KM_DOM_PAD3 ? "pad3" : d == KM_DOM_PAD4 ? "pad4" : "host";
}

/** @brief Return whether a KM_DEV row's built-in default binds.
 *
 * Only a KM_DEVBUILD row binds, and only where the debug flags are compiled
 * in; the distribution presets define OPENPETE_HIDE_DEBUG_FLAGS and bind no
 * KM_DEV row.
 * @param flags the row's KM_* flags.
 * @return 1 when the default binds, 0 otherwise. */
static int km_devbuild_bound(unsigned flags) {
#ifdef OPENPETE_HIDE_DEBUG_FLAGS
    (void)flags;
    return 0;
#else
    return (flags & KM_DEVBUILD) != 0;
#endif
}

void openpete_keymap_init(void) {
    memset(g_bind, 0, sizeof g_bind);
    g_consumed_n = 0;
    memset(g_pulses, 0, sizeof g_pulses);
    memset(g_delta, 0, sizeof g_delta);
    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        const km_action_def_t* a = &g_actions[i];
        km_binding_t* b = &g_bind[i];
        b->rank = -1;
        snprintf(b->origin, sizeof b->origin, "default");

        /* KM_DEV rows resolve unbound no matter what their default says.
         * The default string is still kept in the registry (and printed
         * by --keys-report) so it reads as documentation of the key to
         * put in your own config, not as a key that is live.
         * KM_DEVBUILD exempts a row from that in a build that compiles
         * the debug flags in: there the default binds like any other, and
         * only the shipped build (OPENPETE_HIDE_DEBUG_FLAGS) withholds it. */
        if ((a->flags & KM_DEV) && !km_devbuild_bound(a->flags)) {
            b->n = 0;
            continue;
        }
        const char* def = a->def;
        if (km_parse_list(&def, 1, b->src, &b->n,
                          "built-in defaults", km_domain_name(a->domain),
                          a->id) != 0) {
            /* A default that does not parse is a build-time mistake in the
             * registry; say so loudly rather than shipping a dead key. */
            OPL_ERROR_F("keys", "built-in default for %s.%s is unparseable: %s",
                        km_domain_name(a->domain), a->id, a->def);
            b->n = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* layering                                                           */
/* ------------------------------------------------------------------ */

/** @brief Return the registry index of a domain's action.
 *
 * The domain is matched case-insensitively against "host" and "pad".."pad4";
 * the action id is matched exactly.
 * @param domain the binding domain; "mod.<id>" and unknown domains match
 *        nothing.
 * @param action the action id.
 * @return the index into g_actions, or -1. */
static int km_find(const char* domain, const char* action) {
    int dom = -1;
    if (km_streq_ci(domain, "host")) dom = KM_DOM_HOST;
    else if (km_streq_ci(domain, "pad"))  dom = KM_DOM_PAD;
    else if (km_streq_ci(domain, "pad2")) dom = KM_DOM_PAD2;
    else if (km_streq_ci(domain, "pad3")) dom = KM_DOM_PAD3;
    else if (km_streq_ci(domain, "pad4")) dom = KM_DOM_PAD4;
    if (dom < 0) return -1;
    for (int i = 0; i < OPK_ACTION_COUNT; i++)
        if (g_actions[i].domain == (unsigned char)dom &&
            strcmp(g_actions[i].id, action) == 0)
            return i;
    return -1;
}

/** @brief Return the Levenshtein distance between two strings, for km_did_you_mean.
 *
 * Gives a mistyped action id the same "did you mean" hint config.c gives
 * bare keys.
 * @param a first string.
 * @param b second string.
 * @return the edit distance, or 99 when either string is longer than 32
 *         characters. */
static int km_edit_distance(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 32 || lb > 32) return 99;
    int prev[33], cur[33];
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int d = prev[j] + 1;
            if (cur[j - 1] + 1 < d) d = cur[j - 1] + 1;
            if (prev[j - 1] + cost < d) d = prev[j - 1] + cost;
            cur[j] = d;
        }
        memcpy(prev, cur, sizeof(int) * (lb + 1));
    }
    return prev[lb];
}

static const char* km_did_you_mean(int dom, const char* action) {
    const char* best = NULL;
    int best_d = 4;   /* anything further away is not a typo */
    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        if (g_actions[i].domain != (unsigned char)dom) continue;
        int d = km_edit_distance(action, g_actions[i].id);
        if (d < best_d) { best_d = d; best = g_actions[i].id; }
    }
    return best;
}

int openpete_keymap_set(const char* domain, const char* action,
                        const char* const* tokens, int n,
                        int rank, const char* origin, const char* where) {
    if (!domain || !action) return -1;

    int idx = km_find(domain, action);
    if (idx < 0) {
        int dom = km_streq_ci(domain, "pad")  ? KM_DOM_PAD
                : km_streq_ci(domain, "pad2") ? KM_DOM_PAD2
                : km_streq_ci(domain, "pad3") ? KM_DOM_PAD3
                : km_streq_ci(domain, "pad4") ? KM_DOM_PAD4
                : km_streq_ci(domain, "host") ? KM_DOM_HOST : -1;
        if (dom < 0) {
            OPL_ERROR_F("keys", "%s: unknown [keys] namespace '%s' "
                        "(expected pad, pad2, pad3, pad4, host or mod.<id>)",
                        where ? where : "config", domain);
            return -1;
        }
        const char* hint = km_did_you_mean(dom, action);
        OPL_ERROR_F("keys", "%s: [keys.%s] unknown action '%s'%s%s%s",
                    where ? where : "config", domain, action,
                    hint ? " (did you mean '" : "", hint ? hint : "",
                    hint ? "'?)" : "");
        return -1;
    }

    km_binding_t* b = &g_bind[idx];
    /* Ties go to the later call: config.c harvests files in ascending
     * precedence, so "same rank, seen later" means the user-dir file or a
     * line further down the same table. */
    if (rank < b->rank) return 0;

    km_source_t src[KM_MAX_SOURCES];
    int cnt = 0;
    if (km_parse_list(tokens, n, src, &cnt, where ? where : "config",
                      domain, action) != 0)
        return -1;

    /* A scope REPLACES an action's source list rather than appending to
     * it. That is what lets a higher scope unbind (`= "none"`, or `= []`,
     * both of which land here with cnt == 0) something a lower scope
     * bound; append semantics would make unbinding inexpressible. */
    if (b->rank >= 0 || b->n > 0)
        snprintf(b->shadowed, sizeof b->shadowed, "%s", b->origin);
    memcpy(b->src, src, sizeof src);
    b->n = cnt;
    b->rank = rank;
    b->unbound_by_config = (cnt == 0) ? 1u : 0u;
    snprintf(b->origin, sizeof b->origin, "%s", origin ? origin : "config");
    return 0;
}

/* ------------------------------------------------------------------ */
/* resolve: conflict + reachability scans                             */
/* ------------------------------------------------------------------ */

/** @brief Return whether two input contexts can be live at the same instant.
 *
 * ALWAYS overlaps every context and each context overlaps itself. MENU is
 * disjoint from GAMEPLAY and FREECAM because opening the settings menu
 * holds the game pad neutral and zeroes the freecam axes; GAMEPLAY and
 * FREECAM are disjoint because the free camera freezes Spyro. FILLPICK
 * overlaps GAMEPLAY and FREECAM because the inspector runs with the game
 * live underneath it. openpete_keymap_resolve reports a shared source only
 * between actions whose contexts overlap.
 * @param a first KM_CTX_* value.
 * @param b second KM_CTX_* value.
 * @return 1 when the contexts overlap, 0 otherwise. */
static int km_ctx_overlap(int a, int b) {
    if (a == KM_CTX_ALWAYS || b == KM_CTX_ALWAYS) return 1;
    if (a == b) return 1;
    if ((a == KM_CTX_GAMEPLAY && b == KM_CTX_FILLPICK) ||
        (b == KM_CTX_GAMEPLAY && a == KM_CTX_FILLPICK)) return 1;
    if ((a == KM_CTX_FREECAM  && b == KM_CTX_FILLPICK) ||
        (b == KM_CTX_FREECAM  && a == KM_CTX_FILLPICK)) return 1;
    return 0;
}

/** @brief Test whether two actions sharing an input is a known, deliberate overlap.
 *
 * Two pairs are deliberate: OPK_FREECAM with OPK_PAD_L3 (Spyro 1 never
 * reads L3) and OPK_MENU_RIGHT with OPK_MENU_SELECT (a menu row is either a
 * value row or an action row). openpete_keymap_resolve() logs no warning for them, so a
 * stock boot stays silent, but still collects them, and --keys-report prints
 * them with the reason.
 * @param a First action index (OPK_*).
 * @param b Second action index, in either order.
 * @param why Receives the reason when the overlap is deliberate.
 * @return 1 when deliberate, 0 otherwise. */
static int km_overlap_is_intentional(int a, int b, const char** why) {
    int lo = a < b ? a : b, hi = a < b ? b : a;
    if (lo == OPK_FREECAM && hi == OPK_PAD_L3) {
        *why = "Spyro 1 never reads L3, so the stick click is free for the "
               "debug camera";
        return 1;
    }
    if (lo == OPK_MENU_RIGHT && hi == OPK_MENU_SELECT) {
        *why = "cross is 'increase' on a value row and 'activate' on an "
               "action row; a row is only ever one of the two";
        return 1;
    }
    return 0;
}

static int km_src_same(const km_source_t* a, const km_source_t* b) {
    if (a->kind != b->kind || a->code != b->code) return 0;
    /* Opposite polarities of one axis are the two HALVES of a stick's
     * throw, not a contested source — that is exactly how a bipolar axis
     * drives a pair of directions (menu_up on "pad:-lefty", menu_down on
     * "pad:lefty"). Only the same half of the same axis is a clash. */
    if (a->kind == KM_SRC_PADAXIS) return a->invert == b->invert;
    /* A mouse motion source delivers the whole displacement to every action
     * it is bound to whatever its sign, so mouse:x and mouse:-x contest. */
    return 1;
}

typedef struct { int a, b; const char* why; } km_conflict_t;
#define KM_MAX_CONFLICTS 32
static km_conflict_t g_conflicts[KM_MAX_CONFLICTS];
static int g_conflict_n;

/* Mod bindings are stored apart from the action registry: a mod's
 * [[binding]] names are not knowable at config time. One entry per
 * (mod, name): the mod's own `key` registers at rank 0 (origin "mod") and a
 * [keys.mod.<id>] row at its scope's rank (origin "config") replaces it.
 * The table grows on demand; `enabled` follows the mod's live state so a
 * disabled mod's defaults leave the left_bound scan. */
typedef struct {
    char        mod[40];
    char        name[24];
    km_source_t src[KM_MAX_SOURCES];
    int         n;
    int         rank;
    unsigned char is_default;   /* origin: the mod's own key, not a config row */
    unsigned char enabled;
} km_mod_bind_t;

static km_mod_bind_t* g_modbind;
static int g_modbind_n, g_modbind_cap;

static km_mod_bind_t* km_modbind_find(const char* mod_id, const char* name) {
    for (int i = 0; i < g_modbind_n; i++)
        if (!strcmp(g_modbind[i].mod, mod_id) && !strcmp(g_modbind[i].name, name))
            return &g_modbind[i];
    return NULL;
}

static km_mod_bind_t* km_modbind_add(const char* mod_id, const char* name) {
    if (g_modbind_n == g_modbind_cap) {
        int cap = g_modbind_cap ? g_modbind_cap * 2 : 32;
        km_mod_bind_t* grown = realloc(g_modbind, sizeof *grown * (size_t)cap);
        if (!grown) return NULL;
        g_modbind = grown;
        g_modbind_cap = cap;
    }
    km_mod_bind_t* m = &g_modbind[g_modbind_n++];
    memset(m, 0, sizeof *m);
    snprintf(m->mod, sizeof m->mod, "%s", mod_id);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->enabled = 1;
    return m;
}

/* Whether any host or mod binding carries mouse:left. Read on the pump
 * thread by the double-click fullscreen gate, so two presses of a
 * mouse:left-bound action cannot also flip the window. */
static _Atomic int g_left_bound;

static void km_recompute_left_bound(void) {
    int found = 0;
    for (int i = 0; i < OPK_ACTION_COUNT && !found; i++)
        for (int x = 0; x < g_bind[i].n; x++)
            if (g_bind[i].src[x].kind == KM_SRC_MOUSEBTN &&
                g_bind[i].src[x].code == KM_MOUSE_LEFT) { found = 1; break; }
    for (int i = 0; i < g_modbind_n && !found; i++)
        for (int x = 0; x < g_modbind[i].n && g_modbind[i].enabled; x++)
            if (g_modbind[i].src[x].kind == KM_SRC_MOUSEBTN &&
                g_modbind[i].src[x].code == KM_MOUSE_LEFT) { found = 1; break; }
    atomic_store_explicit(&g_left_bound, found, memory_order_relaxed);
}

int openpete_keymap_mouse_left_bound(void) {
    return atomic_load_explicit(&g_left_bound, memory_order_relaxed);
}

int openpete_keymap_resolve(void) {
    int warnings = 0;
    g_conflict_n = 0;

    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        for (int j = i + 1; j < OPK_ACTION_COUNT; j++) {
            if (!km_ctx_overlap(g_actions[i].ctx, g_actions[j].ctx)) continue;
            int shared = 0;
            for (int x = 0; x < g_bind[i].n && !shared; x++)
                for (int y = 0; y < g_bind[j].n && !shared; y++)
                    if (km_src_same(&g_bind[i].src[x], &g_bind[j].src[y]))
                        shared = 1;
            if (!shared) continue;

            const char* why = NULL;
            int intentional = km_overlap_is_intentional(i, j, &why);
            if (g_conflict_n < KM_MAX_CONFLICTS) {
                g_conflicts[g_conflict_n].a = i;
                g_conflicts[g_conflict_n].b = j;
                g_conflicts[g_conflict_n].why = intentional ? why : NULL;
                g_conflict_n++;
            }
            if (!intentional) {
                OPL_WARN_F("keys", "%s.%s and %s.%s are both bound to the "
                           "same input (%s / %s)",
                           km_domain_name(g_actions[i].domain), g_actions[i].id,
                           km_domain_name(g_actions[j].domain), g_actions[j].id,
                           g_actions[i].purpose, g_actions[j].purpose);
                warnings++;
            }
        }
    }

    /* Reachability: an action the UI depends on that ended up with NO
     * source at all. A warning rather than an error — a pad-only player
     * unbinding the keyboard half of `cross` is legitimate — and it only
     * fires when every source is gone. KM_DEV rows are exempt: being
     * unbound is their whole design. */
    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        if (!(g_actions[i].flags & KM_REQUIRED)) continue;
        if (g_actions[i].flags & KM_DEV) continue;
        if (g_bind[i].n > 0) continue;
        OPL_WARN_F("keys", "%s.%s is unbound (%s) — that control is now "
                   "unreachable", km_domain_name(g_actions[i].domain),
                   g_actions[i].id, g_actions[i].purpose);
        warnings++;
    }

    /* Mouse reachability. The settings menu owns the mouse while the MENU
     * context is live, so every mouse source on a MENU action reads neutral;
     * menu_close is exempt from that ownership (it may close what a mouse
     * button opened) and so from this clause. The menu's own gestures (left
     * and right click on a row, the wheel over a row) are consumed outside
     * the keymap, so a `menu` or `menu_close` binding on them would make
     * every row click also toggle or close the menu. */
    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        const km_action_def_t* a = &g_actions[i];
        const km_binding_t* b = &g_bind[i];
        for (int x = 0; x < b->n; x++) {
            const km_source_t* src = &b->src[x];
            if (!km_is_mouse_kind(src->kind)) continue;
            const char* nm = km_mouse_name(src->kind, src->code);
            if (a->ctx == KM_CTX_MENU && i != OPK_MENU_CLOSE) {
                OPL_WARN_F("keys", "%s.%s binds mouse:%s, which is unreachable: "
                           "the settings menu owns the mouse while it is open",
                           km_domain_name(a->domain), a->id, nm ? nm : "?");
                warnings++;
                continue;
            }
            if ((i == OPK_MENU || i == OPK_MENU_CLOSE) &&
                src->kind == KM_SRC_MOUSEBTN &&
                (src->code == KM_MOUSE_LEFT || src->code == KM_MOUSE_RIGHT ||
                 (i == OPK_MENU && KM_MOUSE_IS_PULSE(src->code)))) {
                OPL_WARN_F("keys", "%s.%s binds mouse:%s, a settings-menu "
                           "gesture: every %s would also %s the menu",
                           km_domain_name(a->domain), a->id, nm ? nm : "?",
                           KM_MOUSE_IS_PULSE(src->code) ? "wheel notch over a row"
                                                        : "row click",
                           i == OPK_MENU ? "toggle" : "close");
                warnings++;
            }
        }
    }
    km_recompute_left_bound();
    return warnings;
}

/* ------------------------------------------------------------------ */
/* live queries                                                       */
/* ------------------------------------------------------------------ */

static unsigned char g_held[OPK_ACTION_COUNT];
static unsigned char g_prev[OPK_ACTION_COUNT];
static int           g_suspend_kbd;
/* The one action that stays readable through a keyboard suspension, or
 * OPK_ACTION_COUNT for none. */
static openpete_action_t g_suspend_exempt = OPK_ACTION_COUNT;

/* Past this fraction of full throw an axis counts as a digital press —
 * generous, because the only digital-from-axis bindings are the analog
 * triggers, where a light rest of the finger should not register. */
#define KM_AXIS_PRESS  16384
/* Below this an axis reads as centred. Matches the freecam's own
 * long-standing stick deadzone so remapping a stick to an axis action
 * feels the same as the hardcoded read it replaces. */
#define KM_AXIS_DEAD    8000

/* Where a resolution reads from.
 *
 * `rd` NULL means the installed live reads — the host path, unchanged.
 * A non-NULL reader lets a caller resolve the same bindings against its own
 * snapshot, which is what keeps player 2's pad decode replayable: it is
 * decoded on the producer side from the sample being published, not from
 * whatever SDL happens to hold when a consumer later asks.
 *
 * `default_slot` serves sources with no explicit "padN:" qualifier, so an
 * unqualified "pad:" inside a [keys.pad2] block reads player 2's device. */
typedef struct {
    const openpete_keymap_reader_t* rd;
    int default_slot;
    /* The action is exempt from the settings menu's mouse ownership (OPK_MENU
     * and OPK_MENU_CLOSE, so a mouse-bound `menu` can close what it opened).
     * Never exempt from the dev overlay's. */
    int menu_exempt;
    /* The action is the one named by openpete_keymap_suspend_keyboard, so its
     * keyboard sources are read through a suspension. */
    int kbd_exempt;
} km_resolve_t;

static const km_resolve_t KM_RESOLVE_LIVE = { NULL, 0, 0, 0 };

static int km_mouse_owned(void) {
    return s_in.mouse_owned ? s_in.mouse_owned() : OPENPETE_MOUSE_OWNED_NONE;
}

/** @brief Return whether mouse sources read neutral under the current mouse owner.
 *
 * The dev overlay's ownership neutralises every action; the settings
 * menu's neutralises every action except a menu-exempt one.
 * @param owned an OPENPETE_MOUSE_OWNED_* value.
 * @param menu_exempt nonzero for OPK_MENU and OPK_MENU_CLOSE.
 * @return 1 when mouse sources read neutral, 0 otherwise. */
static int km_mouse_neutral(int owned, int menu_exempt) {
    if (owned == OPENPETE_MOUSE_OWNED_DEVUI) return 1;
    if (owned == OPENPETE_MOUSE_OWNED_MENU && !menu_exempt) return 1;
    return 0;
}

static int km_read_mouse_btn(const km_resolve_t* c, const km_source_t* s) {
    /* A pad reader resolves the replayable pad decode; the mouse never
     * routes into it. A pulse source has no level. */
    if (c->rd) return 0;
    if (KM_MOUSE_IS_PULSE(s->code)) return 0;
    if (km_mouse_neutral(km_mouse_owned(), c->menu_exempt)) return 0;
    return s_in.mouse_button ? s_in.mouse_button(s->code) : 0;
}

static int km_src_slot(const km_resolve_t* c, const km_source_t* s) {
    return s->device ? s->device - 1 : c->default_slot;
}

static int km_read_key(const km_resolve_t* c, int code) {
    if (g_suspend_kbd && !c->kbd_exempt) return 0;
    /* The event filter already acted on this press. */
    if (km_key_consumed(code)) return 0;
    if (c->rd) return c->rd->key_down ? c->rd->key_down(c->rd->ctx, code) : 0;
    return s_in.key_down ? s_in.key_down(code) : 0;
}

static int km_read_btn(const km_resolve_t* c, const km_source_t* s) {
    const int slot = km_src_slot(c, s);
    if (c->rd)
        return c->rd->pad_button ? c->rd->pad_button(c->rd->ctx, slot, s->code) : 0;
    if (slot > 0)
        return s_in.pad_button_slot ? s_in.pad_button_slot(slot, s->code) : 0;
    return s_in.pad_button ? s_in.pad_button(s->code) : 0;
}

/** @brief Return a pad axis source's raw value, sign-corrected and read from the source's pad slot.
 *
 * The slot is the source's device minus one, or the resolution's default
 * slot for an unqualified source. The caller's reader serves every slot
 * when set; otherwise slot 0 reads s_in.pad_axis and other slots
 * s_in.pad_axis_slot. The digital-press test and the analog reads share
 * this reader, so both honour the slot.
 * @param c the resolution context.
 * @param s a KM_SRC_PADAXIS source; invert < 0 negates the value.
 * @return the read value (nominally [-32768, 32767]), negated for an
 *         inverted source, or 0 when the needed read is not installed. */
static int km_src_axis_raw(const km_resolve_t* c, const km_source_t* s) {
    const int slot = km_src_slot(c, s);
    int v;
    if (c->rd) {
        if (!c->rd->pad_axis) return 0;
        v = c->rd->pad_axis(c->rd->ctx, slot, s->code);
    } else if (slot > 0) {
        if (!s_in.pad_axis_slot) return 0;
        v = s_in.pad_axis_slot(slot, s->code);
    } else {
        if (!s_in.pad_axis) return 0;
        v = s_in.pad_axis(s->code);
    }
    return v * (s->invert < 0 ? -1 : 1);
}

static int km_src_down(const km_resolve_t* c, const km_source_t* s) {
    switch (s->kind) {
    case KM_SRC_KEY:
        return km_read_key(c, s->code);
    case KM_SRC_PADBTN:
        return km_read_btn(c, s);
    case KM_SRC_PADAXIS:
        return km_src_axis_raw(c, s) > KM_AXIS_PRESS;
    case KM_SRC_MOUSEBTN:
        return km_read_mouse_btn(c, s);
    case KM_SRC_MOUSEAXIS:
        return 0;   /* a displacement has no level */
    default: return 0;
    }
}

static int km_menu_exempt(openpete_action_t a) {
    return a == OPK_MENU || a == OPK_MENU_CLOSE;
}

static int km_action_down(const km_resolve_t* c, openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    const km_binding_t* b = &g_bind[a];
    km_resolve_t cc = *c;
    cc.menu_exempt = km_menu_exempt(a);
    cc.kbd_exempt  = (a == g_suspend_exempt);
    for (int i = 0; i < b->n; i++)
        if (km_src_down(&cc, &b->src[i])) return 1;
    return 0;
}

static float km_action_axis(const km_resolve_t* c, openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0.0f;
    const km_binding_t* b = &g_bind[a];
    /* First non-neutral source wins rather than summing: with two sticks
     * bound to one axis, summing would let a centred-but-drifting pad
     * bias the other. Deterministic and order-stable. */
    for (int i = 0; i < b->n; i++) {
        const km_source_t* s = &b->src[i];
        if (s->kind == KM_SRC_PADAXIS) {
            int v = km_src_axis_raw(c, s);
            if (v > KM_AXIS_DEAD || v < -KM_AXIS_DEAD) {
                float f = (float)v / 32767.0f;
                return f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
            }
        } else if (s->kind == KM_SRC_MOUSEAXIS) {
            continue;   /* a displacement has no level to return */
        } else if (km_src_down(c, s)) {
            return 1.0f;
        }
    }
    return 0.0f;
}

int openpete_keymap_action_down_via(openpete_action_t a, int default_slot,
                                    const openpete_keymap_reader_t* rd) {
    const km_resolve_t c = { rd, default_slot };
    return km_action_down(&c, a);
}

float openpete_keymap_action_axis_via(openpete_action_t a, int default_slot,
                                      const openpete_keymap_reader_t* rd) {
    const km_resolve_t c = { rd, default_slot };
    return km_action_axis(&c, a);
}

int openpete_keymap_action_axis_raw_via(openpete_action_t a, int default_slot,
                                        const openpete_keymap_reader_t* rd) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    const km_resolve_t c = { rd, default_slot };
    const km_binding_t* b = &g_bind[a];
    /* Same first-non-neutral-source rule as the float form, returning the
     * unscaled value: a pad decode has to reproduce the game's own
     * fixed-point axis conversion, and a float round-trip through a deadzone
     * would give the extra players a centre band player 1 does not have. */
    for (int i = 0; i < b->n; i++) {
        const km_source_t* s = &b->src[i];
        if (s->kind == KM_SRC_PADAXIS) {
            int v = km_src_axis_raw(&c, s);
            if (v > KM_AXIS_DEAD || v < -KM_AXIS_DEAD) return v;
        } else if (km_src_down(&c, s)) {
            return 32767;
        }
    }
    return 0;
}

int openpete_keymap_action_bound(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? g_bind[a].n : 0;
}

/* Wheel pulses and motion deltas: one marker per total, advanced every
 * poll whether or not anything is bound, so motion made while an overlay
 * owned the mouse is dropped rather than delivered late. */
static long long g_wheel_last;
static long long g_motion_last[KM_MOUSE_AXIS_COUNT];

static long long km_floordiv_unit(long long v) {
    const long long u = OPENPETE_MOUSE_WHEEL_UNIT;
    return v >= 0 ? v / u : -((-v + u - 1) / u);
}

void openpete_keymap_poll(void) {
    km_release_consumed();
    memcpy(g_prev, g_held, sizeof g_prev);
    for (int i = 0; i < OPK_ACTION_COUNT; i++)
        g_held[i] = (unsigned char)km_action_down(&KM_RESOLVE_LIVE,
                                                  (openpete_action_t)i);

    const long long wheel = s_in.mouse_wheel_total ? s_in.mouse_wheel_total() : 0;
    const long long notches = km_floordiv_unit(wheel) - km_floordiv_unit(g_wheel_last);
    g_wheel_last = wheel;
    long long dm[KM_MOUSE_AXIS_COUNT];
    for (int ax = 0; ax < KM_MOUSE_AXIS_COUNT; ax++) {
        long long t = s_in.mouse_motion_total ? s_in.mouse_motion_total(ax) : 0;
        dm[ax] = t - g_motion_last[ax];
        g_motion_last[ax] = t;
    }
    const int owned = km_mouse_owned();

    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        g_pulses[i] = 0;
        g_delta[i]  = 0;
        const km_binding_t* b = &g_bind[i];
        if (km_mouse_neutral(owned, km_menu_exempt((openpete_action_t)i))) continue;
        for (int x = 0; x < b->n; x++) {
            const km_source_t* s = &b->src[x];
            if (s->kind == KM_SRC_MOUSEBTN) {
                if (s->code == KM_MOUSE_WHEEL_UP && notches > 0)
                    g_pulses[i] += (unsigned)notches;
                else if (s->code == KM_MOUSE_WHEEL_DOWN && notches < 0)
                    g_pulses[i] += (unsigned)(-notches);
            } else if (s->kind == KM_SRC_MOUSEAXIS) {
                g_delta[i] += dm[s->code] * (s->invert < 0 ? -1 : 1);
            }
        }
    }
}

int openpete_key_down(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? g_held[a] : 0;
}

int openpete_key_edge(openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    return g_held[a] && !g_prev[a];
}

int openpete_key_pulses(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? (int)g_pulses[a] : 0;
}

int openpete_key_pressed(openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    return (g_held[a] && !g_prev[a]) || g_pulses[a] > 0;
}

long long openpete_key_delta(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? g_delta[a] : 0;
}

void openpete_keymap_mouse_markers(long long* wheel, long long* mx, long long* my) {
    if (wheel) *wheel = g_wheel_last;
    if (mx) *mx = g_motion_last[KM_MOUSE_X];
    if (my) *my = g_motion_last[KM_MOUSE_Y];
}

float openpete_key_axis(openpete_action_t a) {
    return km_action_axis(&KM_RESOLVE_LIVE, a);
}

void openpete_keymap_suspend_keyboard(int on, openpete_action_t exempt) {
    g_suspend_kbd = on ? 1 : 0;
    g_suspend_exempt = (unsigned)exempt < OPK_ACTION_COUNT ? exempt
                                                           : OPK_ACTION_COUNT;
}

int openpete_keymap_sources(openpete_action_t a, km_source_t* out, int cap) {
    if ((unsigned)a >= OPK_ACTION_COUNT || !out) return 0;
    int n = g_bind[a].n < cap ? g_bind[a].n : cap;
    memcpy(out, g_bind[a].src, sizeof(km_source_t) * (size_t)n);
    return n;
}

const char* openpete_keymap_origin(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? g_bind[a].origin : "default";
}

int openpete_keymap_scancode_down(int scancode) {
    if (g_suspend_kbd) return 0;
    return s_in.key_down ? s_in.key_down(scancode) : 0;
}

int openpete_keymap_source_down(const km_source_t* s) {
    return s ? km_src_down(&KM_RESOLVE_LIVE, s) : 0;
}

int openpete_keymap_key_binds(openpete_action_t a, int scancode) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    const km_binding_t* b = &g_bind[a];
    for (int i = 0; i < b->n; i++)
        if (b->src[i].kind == KM_SRC_KEY && b->src[i].code == scancode)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* the report                                                         */
/* ------------------------------------------------------------------ */

static int s_report_requested;
void openpete_keymap_request_report(void) { s_report_requested = 1; }
int  openpete_keymap_report_requested(void) { return s_report_requested; }

static void km_format_binding(const km_binding_t* b, char* out, size_t cap) {
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < b->n; i++) {
        const km_source_t* s = &b->src[i];
        const char* nm = s_to_name ? s_to_name(s->kind, s->code) : NULL;
        char piece[80];
        if (km_is_mouse_kind(s->kind)) {
            const char* mn = km_mouse_name(s->kind, s->code);
            snprintf(piece, sizeof piece, "mouse:%s%s",
                     (s->kind == KM_SRC_MOUSEAXIS && s->invert < 0) ? "-" : "",
                     mn ? mn : "?");
        } else if (s->kind == KM_SRC_KEY) {
            /* Print the layout's own label next to the (positional)
             * scancode name when they differ, so a non-QWERTY user can
             * see which physical key to press without guessing. */
            const char* lbl = s_key_label ? s_key_label(s->code) : NULL;
            if (lbl && nm && strcmp(lbl, nm) != 0)
                snprintf(piece, sizeof piece, "%s [%s]", nm, lbl);
            else
                snprintf(piece, sizeof piece, "%s", nm ? nm : "?");
        } else {
            char padpfx[8];
            if (s->device) snprintf(padpfx, sizeof padpfx, "pad%d:", s->device);
            else           snprintf(padpfx, sizeof padpfx, "pad:");
            snprintf(piece, sizeof piece, "%s%s%s", padpfx,
                     (s->kind == KM_SRC_PADAXIS && s->invert < 0) ? "-" : "",
                     nm ? nm : "?");
        }
        int wrote = snprintf(out + used, cap - used, "%s%s",
                             used ? ", " : "", piece);
        if (wrote < 0 || (size_t)wrote >= cap - used) break;
        used += (size_t)wrote;
    }
    if (!b->n) snprintf(out, cap, "-");
}

void openpete_keymap_report(FILE* out) {
    if (!out) return;
    fprintf(out, "openpete keymap\n");
    fprintf(out, "  %-22s %-42s %-16s %s\n",
            "action", "binding", "from", "purpose");

    int last_domain = -1;
    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        const km_action_def_t* a = &g_actions[i];
        const km_binding_t* b = &g_bind[i];
        if ((int)a->domain != last_domain) {
            fprintf(out, "\n  [keys.%s]\n", km_domain_name(a->domain));
            last_domain = a->domain;
        }
        char bind[160];
        km_format_binding(b, bind, sizeof bind);

        /* Why a row is blank matters more than that it is: "dev" means
         * the registry withheld it, "unbound at <scope>" means a config
         * line did. */
        char from[64];
        if (!b->n && (a->flags & KM_DEV) && b->rank < 0)
            snprintf(from, sizeof from, "dev (default %s)", a->def);
        else if (b->unbound_by_config)
            snprintf(from, sizeof from, "unbound at %s", b->origin);
        else
            snprintf(from, sizeof from, "%s", b->origin);

        fprintf(out, "  %-22s %-42s %-16s %s\n",
                a->id, bind, from, a->purpose);
        if (b->shadowed[0] && strcmp(b->shadowed, b->origin) != 0)
            fprintf(out, "  %-22s   (shadowed %s)\n", "", b->shadowed);
    }

    /* Mod overrides live in their own store (mod ids are not knowable at
     * config time), but they are bindings too — leaving them out would
     * make this report an incomplete answer to "what does this install
     * respond to?". Printed after a scan of the store so an override for
     * a mod that is not installed still shows up rather than vanishing. */
    if (g_modbind_n) {
        fprintf(out, "\n  [keys.mod.*]\n");
        for (int i = 0; i < g_modbind_n; i++) {
            km_binding_t tmp;
            memset(&tmp, 0, sizeof tmp);
            memcpy(tmp.src, g_modbind[i].src, sizeof tmp.src);
            tmp.n = g_modbind[i].n;
            char bind[160];
            km_format_binding(&tmp, bind, sizeof bind);
            char id[72];
            snprintf(id, sizeof id, "%s.%s", g_modbind[i].mod, g_modbind[i].name);
            fprintf(out, "  %-22s %-42s %-16s %s\n", id, bind,
                    g_modbind[i].is_default ? "mod default" : "config",
                    g_modbind[i].is_default ? "mod binding (mod.toml key)"
                                            : "mod binding override");
        }
    }

    if (g_conflict_n) {
        fprintf(out, "\n  shared inputs:\n");
        for (int i = 0; i < g_conflict_n; i++) {
            const km_conflict_t* c = &g_conflicts[i];
            fprintf(out, "    %s.%s + %s.%s%s%s\n",
                    km_domain_name(g_actions[c->a].domain), g_actions[c->a].id,
                    km_domain_name(g_actions[c->b].domain), g_actions[c->b].id,
                    c->why ? " — intentional: " : "  <-- unintended",
                    c->why ? c->why : "");
        }
    }
    fprintf(out, "\n  Rebind in openpete.toml, e.g.\n"
                 "    [keys.host]\n"
                 "    fillpick = \"P\"      # bind a dev instrument\n"
                 "    rewind   = \"none\"   # unbind\n");
}

/* ------------------------------------------------------------------ */
/* mod bindings                                                       */
/* ------------------------------------------------------------------ */

/** @brief Store one mod binding at a rank, as a mod.toml default or a [keys.mod.<id>] override.
 *
 * The entry for (mod_id, name) is created on first use. A stored entry of
 * higher rank is kept; an equal rank is replaced by the later call. A
 * default whose key fails to parse is stored unbound, so the mod's
 * binding_down reads 0; an override that fails to parse stores nothing.
 * Every store recomputes the mouse:left scan.
 * @param mod_id the mod's id; NULL fails.
 * @param name the mod's declared binding name; NULL fails.
 * @param tokens the binding values as written.
 * @param n number of tokens.
 * @param rank the scope's precedence; 0 for a mod default.
 * @param is_default nonzero for the mod's own key.
 * @param where the config file or manifest named in error messages.
 * @return 0, or -1 on a parse failure, a NULL id or name, or allocation
 *         failure. */
static int km_set_mod(const char* mod_id, const char* name,
                      const char* const* tokens, int n, int rank,
                      int is_default, const char* where) {
    if (!mod_id || !name) return -1;
    km_source_t src[KM_MAX_SOURCES];
    int cnt = 0;
    char dom[48];
    snprintf(dom, sizeof dom, "mod.%s", mod_id);
    int parsed = km_parse_list(tokens, n, src, &cnt, where ? where : "config",
                               dom, name);
    if (parsed != 0 && !is_default) return -1;
    /* A mod key that fails to parse still registers, unbound, so the mod's
     * binding_down reads 0 the way an unbound config override does. */
    if (parsed != 0) cnt = 0;

    km_mod_bind_t* m = km_modbind_find(mod_id, name);
    if (m) {
        if (rank < m->rank) return parsed == 0 ? 0 : -1;
    } else {
        m = km_modbind_add(mod_id, name);
        if (!m) {
            OPL_WARN_F("keys", "out of memory registering %s.%s", mod_id, name);
            return -1;
        }
    }
    memcpy(m->src, src, sizeof src);
    m->n = cnt;
    m->rank = rank;
    m->is_default = (unsigned char)(is_default ? 1 : 0);
    km_recompute_left_bound();
    return parsed == 0 ? 0 : -1;
}

int openpete_keymap_set_mod(const char* mod_id, const char* name,
                            const char* const* tokens, int n,
                            int rank, const char* where) {
    return km_set_mod(mod_id, name, tokens, n, rank, 0, where);
}

int openpete_keymap_set_mod_default(const char* mod_id, const char* name,
                                    const char* key, const char* where) {
    const char* tok[1] = { key };
    return km_set_mod(mod_id, name, tok, 1, 0, 1, where);
}

void openpete_keymap_set_mod_enabled(const char* mod_id, int on) {
    if (!mod_id) return;
    for (int i = 0; i < g_modbind_n; i++)
        if (!strcmp(g_modbind[i].mod, mod_id))
            g_modbind[i].enabled = (unsigned char)(on ? 1 : 0);
    km_recompute_left_bound();
}

const char* openpete_keymap_mod_effective_key(const char* mod_id,
                                              const char* name,
                                              const char* fallback) {
    static char buf[80];
    for (int i = 0; i < g_modbind_n; i++) {
        if (strcmp(g_modbind[i].mod, mod_id) || strcmp(g_modbind[i].name, name))
            continue;
        if (g_modbind[i].n == 0) return "(unbound)";
        const km_source_t* s = &g_modbind[i].src[0];
        const char* nm = s_to_name ? s_to_name(s->kind, s->code) : NULL;
        if (km_is_mouse_kind(s->kind)) {
            const char* mn = km_mouse_name(s->kind, s->code);
            snprintf(buf, sizeof buf, "mouse:%s%s",
                     (s->kind == KM_SRC_MOUSEAXIS && s->invert < 0) ? "-" : "",
                     mn ? mn : "?");
        } else if (s->kind == KM_SRC_KEY)
            snprintf(buf, sizeof buf, "%s", nm ? nm : "?");
        else {
            char padpfx[8];
            if (s->device) snprintf(padpfx, sizeof padpfx, "pad%d:", s->device);
            else           snprintf(padpfx, sizeof padpfx, "pad:");
            snprintf(buf, sizeof buf, "%s%s%s", padpfx,
                     (s->kind == KM_SRC_PADAXIS && s->invert < 0) ? "-" : "",
                     nm ? nm : "?");
        }
        return buf;
    }
    return fallback;
}

int openpete_keymap_mod_binding(const char* mod_id, const char* name,
                                km_source_t* out, int cap, int* out_n) {
    if (!mod_id || !name) return 0;
    for (int i = 0; i < g_modbind_n; i++) {
        if (strcmp(g_modbind[i].mod, mod_id) || strcmp(g_modbind[i].name, name))
            continue;
        int n = g_modbind[i].n < cap ? g_modbind[i].n : cap;
        if (out) memcpy(out, g_modbind[i].src, sizeof(km_source_t) * (size_t)n);
        if (out_n) *out_n = n;
        return 1;
    }
    return 0;
}
