/* core/runtime/keymap.c — the action registry, the layered binding store,
 * and the resolver. See core/include/openpete_keymap.h for what this is
 * for and why it is SDL-free.
 *
 * Deliberately free of SDL and of config.c: config.c hands bindings in as
 * (domain, action, tokens, rank, origin) and gets an error back, so the
 * resolver links into console unit tests alongside the config resolver
 * with nothing but tomlc99 in the link line. */

#include "openpete_input_log.h"
#include "openpete_keymap.h"
#include "openpete_log.h"

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

/* Drop consumed keys that are no longer physically held, so the NEXT
 * press of the same key is live again. Reads the raw input seam rather
 * than km_src_down, which would consult the consumed list and never let
 * anything clear. */
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

/* Parse ONE token. Returns 1 on success (src filled), 0 for "none",
 * -1 for a name that resolves to nothing. */
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

/* Split a value on commas and parse each piece. A scalar "C,pad:south"
 * and an array ["C", "pad:south"] are the same binding — the registry's
 * own defaults are written in the scalar form, so this keeps one code
 * path for defaults and config alike. */
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

static const char* km_domain_name(int d) {
    return d == KM_DOM_PAD  ? "pad"  : d == KM_DOM_PAD2 ? "pad2"
         : d == KM_DOM_PAD3 ? "pad3" : d == KM_DOM_PAD4 ? "pad4" : "host";
}

void openpete_keymap_init(void) {
    memset(g_bind, 0, sizeof g_bind);
    g_consumed_n = 0;
    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        const km_action_def_t* a = &g_actions[i];
        km_binding_t* b = &g_bind[i];
        b->rank = -1;
        snprintf(b->origin, sizeof b->origin, "default");

        /* KM_DEV rows resolve unbound no matter what their default says.
         * The default string is still kept in the registry (and printed
         * by --keys-report) so it reads as documentation of the key to
         * put in your own config, not as a key that is live. */
        if (a->flags & KM_DEV) {
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

/* Levenshtein-lite: the same "did you mean" affordance config.c gives
 * bare keys, so a typo'd action id is a named mistake rather than a
 * silently ignored line. */
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

/* Which contexts can be live at the same instant. MENU is disjoint from
 * GAMEPLAY and FREECAM because opening the settings menu holds the game
 * pad neutral and zeroes the freecam axes; GAMEPLAY and FREECAM are
 * disjoint because the free camera freezes Spyro, so a key that drives
 * both reaches only one of them at a time. FILLPICK overlaps both,
 * because the inspector runs with the game live underneath it. */
static int km_ctx_overlap(int a, int b) {
    if (a == KM_CTX_ALWAYS || b == KM_CTX_ALWAYS) return 1;
    if (a == b) return 1;
    if ((a == KM_CTX_GAMEPLAY && b == KM_CTX_FILLPICK) ||
        (b == KM_CTX_GAMEPLAY && a == KM_CTX_FILLPICK)) return 1;
    if ((a == KM_CTX_FREECAM  && b == KM_CTX_FILLPICK) ||
        (b == KM_CTX_FREECAM  && a == KM_CTX_FILLPICK)) return 1;
    return 0;
}

/* Overlaps that are deliberate, so a stock boot stays silent. They are
 * still COLLECTED and still printed by --keys-report — the point is that
 * a warning which fires on every launch stops being read, not that these
 * should be invisible. */
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
    return 1;
}

typedef struct { int a, b; const char* why; } km_conflict_t;
#define KM_MAX_CONFLICTS 32
static km_conflict_t g_conflicts[KM_MAX_CONFLICTS];
static int g_conflict_n;

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
    return warnings;
}

/* ------------------------------------------------------------------ */
/* live queries                                                       */
/* ------------------------------------------------------------------ */

static unsigned char g_held[OPK_ACTION_COUNT];
static unsigned char g_prev[OPK_ACTION_COUNT];
static int           g_suspend_kbd;

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
} km_resolve_t;

static const km_resolve_t KM_RESOLVE_LIVE = { NULL, 0 };

static int km_src_slot(const km_resolve_t* c, const km_source_t* s) {
    return s->device ? s->device - 1 : c->default_slot;
}

static int km_read_key(const km_resolve_t* c, int code) {
    if (g_suspend_kbd) return 0;
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

/* Raw axis value for a source, sign-corrected for "pad:-leftx" and routed to
 * the source's pad slot. One reader for both the digital-press test and the
 * analog read, so a slot can never be honoured by one and ignored by the
 * other. */
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
    default: return 0;
    }
}

static int km_action_down(const km_resolve_t* c, openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    const km_binding_t* b = &g_bind[a];
    for (int i = 0; i < b->n; i++)
        if (km_src_down(c, &b->src[i])) return 1;
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

void openpete_keymap_poll(void) {
    km_release_consumed();
    memcpy(g_prev, g_held, sizeof g_prev);
    for (int i = 0; i < OPK_ACTION_COUNT; i++)
        g_held[i] = (unsigned char)km_action_down(&KM_RESOLVE_LIVE,
                                                  (openpete_action_t)i);
}

int openpete_key_down(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? g_held[a] : 0;
}

int openpete_key_pressed(openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    return g_held[a] && !g_prev[a];
}

float openpete_key_axis(openpete_action_t a) {
    return km_action_axis(&KM_RESOLVE_LIVE, a);
}

void openpete_keymap_suspend_keyboard(int on) { g_suspend_kbd = on ? 1 : 0; }

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

/* [keys.mod.<id>] rows are stored separately: mods are discovered after
 * the config is read, so their names cannot be validated against a
 * registry at harvest time the way pad/host actions are. */
typedef struct {
    char        mod[40];
    char        name[24];
    km_source_t src[KM_MAX_SOURCES];
    int         n;
    int         rank;
} km_mod_bind_t;

#define KM_MAX_MOD_BINDS 64
static km_mod_bind_t g_modbind[KM_MAX_MOD_BINDS];
static int g_modbind_n;


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
        if (s->kind == KM_SRC_KEY) {
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
            fprintf(out, "  %-22s %-42s %-16s %s\n", id, bind, "config",
                    "mod binding override");
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

int openpete_keymap_set_mod(const char* mod_id, const char* name,
                            const char* const* tokens, int n,
                            int rank, const char* where) {
    if (!mod_id || !name) return -1;
    km_source_t src[KM_MAX_SOURCES];
    int cnt = 0;
    char dom[48];
    snprintf(dom, sizeof dom, "mod.%s", mod_id);
    if (km_parse_list(tokens, n, src, &cnt, where ? where : "config",
                      dom, name) != 0)
        return -1;

    for (int i = 0; i < g_modbind_n; i++) {
        if (strcmp(g_modbind[i].mod, mod_id) || strcmp(g_modbind[i].name, name))
            continue;
        if (rank < g_modbind[i].rank) return 0;
        memcpy(g_modbind[i].src, src, sizeof src);
        g_modbind[i].n = cnt;
        g_modbind[i].rank = rank;
        return 0;
    }
    if (g_modbind_n >= KM_MAX_MOD_BINDS) {
        OPL_WARN_F("keys", "too many [keys.mod.*] bindings; ignoring %s.%s",
                   mod_id, name);
        return 0;
    }
    km_mod_bind_t* m = &g_modbind[g_modbind_n++];
    snprintf(m->mod, sizeof m->mod, "%s", mod_id);
    snprintf(m->name, sizeof m->name, "%s", name);
    memcpy(m->src, src, sizeof src);
    m->n = cnt;
    m->rank = rank;
    return 0;
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
        if (s->kind == KM_SRC_KEY)
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
