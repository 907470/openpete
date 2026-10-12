/* tests/unit/keymap_test.c - the keymap registry, parser and resolver.
 *
 * SDL-free: keymap.c reaches SDL only through the lookup/input seams, so
 * this test installs a tiny fake name table and a fake input state. That
 * keeps the console-test link line free of SDL (see the note at the top
 * of tests/unit/CMakeLists.txt) and, more usefully, lets the test drive
 * held/released input directly to exercise the edge latch. */

#include "openpete_host_mouse.h"  /* OPENPETE_MOUSE_OWNED_*, wheel unit */
#include "openpete_input_log.h"   /* OPENPETE_PAD_SLOTS */
#include "openpete_keymap.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

/* ---- fake SDL ------------------------------------------------------- */

/* Just enough names to spell every default in the registry, plus a few
 * for the parser tests. Codes are arbitrary but stable. */
static const struct { int kind; const char* name; int code; } g_names[] = {
    { KM_SRC_KEY, "A", 1 },  { KM_SRC_KEY, "B", 2 },  { KM_SRC_KEY, "C", 3 },
    { KM_SRC_KEY, "D", 4 },  { KM_SRC_KEY, "F", 5 },  { KM_SRC_KEY, "G", 6 },
    { KM_SRC_KEY, "H", 7 },  { KM_SRC_KEY, "I", 8 },  { KM_SRC_KEY, "L", 9 },
    { KM_SRC_KEY, "M", 10 }, { KM_SRC_KEY, "N", 11 }, { KM_SRC_KEY, "O", 12 },
    { KM_SRC_KEY, "P", 13 }, { KM_SRC_KEY, "S", 14 }, { KM_SRC_KEY, "T", 15 },
    { KM_SRC_KEY, "U", 16 }, { KM_SRC_KEY, "V", 17 }, { KM_SRC_KEY, "W", 18 },
    { KM_SRC_KEY, "X", 19 }, { KM_SRC_KEY, "Y", 20 }, { KM_SRC_KEY, "Z", 21 },
    { KM_SRC_KEY, "1", 22 }, { KM_SRC_KEY, "2", 23 }, { KM_SRC_KEY, "3", 24 },
    { KM_SRC_KEY, "F7", 25 }, { KM_SRC_KEY, "F8", 26 },
    { KM_SRC_KEY, "Up", 27 }, { KM_SRC_KEY, "Down", 28 },
    { KM_SRC_KEY, "Left", 29 }, { KM_SRC_KEY, "Right", 30 },
    { KM_SRC_KEY, "Space", 31 }, { KM_SRC_KEY, "Return", 32 },
    { KM_SRC_KEY, "Tab", 33 },   { KM_SRC_KEY, "Backspace", 34 },
    { KM_SRC_KEY, "Left Shift", 35 },  { KM_SRC_KEY, "Right Shift", 36 },
    { KM_SRC_KEY, "Left Ctrl", 37 },   { KM_SRC_KEY, "Right Ctrl", 38 },
    { KM_SRC_KEY, "[", 39 },  { KM_SRC_KEY, "]", 40 },
    { KM_SRC_KEY, "F5", 41 }, { KM_SRC_KEY, "F6", 42 },
    { KM_SRC_KEY, "F10", 43 }, { KM_SRC_KEY, "Escape", 44 },
    { KM_SRC_KEY, "F9", 45 },  { KM_SRC_KEY, "F12", 46 },

    { KM_SRC_KEY, ",", 47 },   { KM_SRC_KEY, "\\", 48 },
    { KM_SRC_KEY, "E", 49 },   { KM_SRC_KEY, "Q", 50 },

    /* Pad codes in SDL3's ranges, which the capture scans. */
    { KM_SRC_PADBTN, "south", 0 },  { KM_SRC_PADBTN, "east", 1 },
    { KM_SRC_PADBTN, "west", 2 },   { KM_SRC_PADBTN, "north", 3 },
    { KM_SRC_PADBTN, "back", 4 },   { KM_SRC_PADBTN, "guide", 5 },
    { KM_SRC_PADBTN, "start", 6 },
    { KM_SRC_PADBTN, "leftstick", 7 },
    { KM_SRC_PADBTN, "rightstick", 8 },
    { KM_SRC_PADBTN, "leftshoulder", 9 },
    { KM_SRC_PADBTN, "rightshoulder", 10 },
    { KM_SRC_PADBTN, "dpup", 11 },   { KM_SRC_PADBTN, "dpdown", 12 },
    { KM_SRC_PADBTN, "dpleft", 13 }, { KM_SRC_PADBTN, "dpright", 14 },
    { KM_SRC_PADBTN, "misc1", 15 },

    { KM_SRC_PADAXIS, "leftx", 0 },  { KM_SRC_PADAXIS, "lefty", 1 },
    { KM_SRC_PADAXIS, "rightx", 2 }, { KM_SRC_PADAXIS, "righty", 3 },
    { KM_SRC_PADAXIS, "lefttrigger", 4 },
    { KM_SRC_PADAXIS, "righttrigger", 5 },
};

static int fake_to_code(int kind, const char* name) {
    for (size_t i = 0; i < sizeof g_names / sizeof g_names[0]; i++)
        if (g_names[i].kind == kind && strcmp(g_names[i].name, name) == 0)
            return g_names[i].code;
    return -1;
}

static const char* fake_to_name(int kind, int code) {
    for (size_t i = 0; i < sizeof g_names / sizeof g_names[0]; i++)
        if (g_names[i].kind == kind && g_names[i].code == code)
            return g_names[i].name;
    return NULL;
}

static unsigned char g_keys[256];
static unsigned char g_btns[256];
static int           g_axes[256];

static int fake_key(int c)  { return (c >= 0 && c < 256) ? g_keys[c] : 0; }
static int fake_btn(int c)  { return (c >= 0 && c < 256) ? g_btns[c] : 0; }
static int fake_axis(int c) { return (c >= 0 && c < 256) ? g_axes[c] : 0; }

/* The fake mouse channel: a button mask, running totals, and an owner. */
static unsigned  g_mouse_mask;
static long long g_mouse_motion[2];
static long long g_mouse_wheel;
static int       g_mouse_owned;

static int       fake_mouse_btn(int c)   { return (g_mouse_mask >> c) & 1u; }
static long long fake_mouse_motion(int a) { return (a >= 0 && a < 2) ? g_mouse_motion[a] : 0; }
static long long fake_mouse_wheel(void)   { return g_mouse_wheel; }
static int       fake_mouse_owned(void)   { return g_mouse_owned; }

/* The fake clock the hold sources read. */
static unsigned long long g_now_ms;
static unsigned long long fake_now_ms(void) { return g_now_ms; }

/* The fake pointer the capture prompt's rect test reads. */
static float g_mouse_x, g_mouse_y;
static int fake_mouse_pos(float* x, float* y) { *x = g_mouse_x; *y = g_mouse_y; return 1; }

static void fake_reset(void) {
    memset(g_keys, 0, sizeof g_keys);
    memset(g_btns, 0, sizeof g_btns);
    memset(g_axes, 0, sizeof g_axes);
    g_mouse_mask = 0;
    memset(g_mouse_motion, 0, sizeof g_mouse_motion);
    g_mouse_wheel = 0;
    g_mouse_owned = OPENPETE_MOUSE_OWNED_NONE;
    g_now_ms = 1000;
    g_mouse_x = g_mouse_y = 0.0f;
}

static void install_fakes(void) {
    static const openpete_keymap_input_fns fns = {
        fake_key, fake_btn, fake_axis, NULL, NULL,
        fake_mouse_btn, fake_mouse_motion, fake_mouse_wheel, fake_mouse_owned,
        fake_now_ms, fake_mouse_pos
    };
    openpete_keymap_set_lookup(fake_to_code, fake_to_name, NULL);
    openpete_keymap_set_input(&fns);
}

/* ---- helpers -------------------------------------------------------- */

static int set1(const char* dom, const char* act, const char* val,
                int rank, const char* origin) {
    const char* toks[1] = { val };
    return openpete_keymap_set(dom, act, toks, 1, rank, origin, "test");
}

static int n_sources(openpete_action_t a) {
    km_source_t s[KM_MAX_SOURCES];
    return openpete_keymap_sources(a, s, KM_MAX_SOURCES);
}

static int has_source(openpete_action_t a, int kind, int code) {
    km_source_t s[KM_MAX_SOURCES];
    int n = openpete_keymap_sources(a, s, KM_MAX_SOURCES);
    for (int i = 0; i < n; i++)
        if (s[i].kind == kind && s[i].code == code) return 1;
    return 0;
}

/* ---- tests ---------------------------------------------------------- */

/* Every registry default must parse. This is the test that catches a
 * typo'd default string, which would otherwise ship as a dead key. */
static void test_defaults_parse(void) {
    openpete_keymap_init();

    CHECK(has_source(OPK_MENU, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "Escape")),
          "menu should default to Escape");
    CHECK(has_source(OPK_PAD_CROSS, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "C")),
          "pad.cross should default to C");
    CHECK(has_source(OPK_PAD_CROSS, KM_SRC_PADBTN,
                     fake_to_code(KM_SRC_PADBTN, "south")),
          "pad.cross should also default to pad:south");
    CHECK(n_sources(OPK_PAD_CROSS) == 2, "pad.cross should have 2 sources");

    /* An inverted axis keeps the axis code and flips the sign. */
    km_source_t s[KM_MAX_SOURCES];
    int n = openpete_keymap_sources(OPK_FLY_AXIS_Y, s, KM_MAX_SOURCES);
    CHECK(n == 1 && s[0].kind == KM_SRC_PADAXIS &&
          s[0].code == fake_to_code(KM_SRC_PADAXIS, "lefty") && s[0].invert < 0,
          "fly_axis_y should be an inverted lefty");
}

/* KM_DEV rows resolve unbound with no config, so a shipped build has no
 * live dev instruments. This is the "don't ship the P shortcut" case. */
static void test_dev_unbound_by_default(void) {
    openpete_keymap_init();
    CHECK(n_sources(OPK_FILLPICK) == 0, "fillpick must be unbound by default");
    CHECK(n_sources(OPK_CAMLOCK) == 0, "camlock must be unbound");
    CHECK(n_sources(OPK_FILLPICK_FILL) == 0, "fillpick_fill must be unbound");

    /* ...and a config can bind one back. */
    CHECK(set1("host", "fillpick", "P", 100, "global") == 0, "bind fillpick");
    CHECK(has_source(OPK_FILLPICK, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "P")),
          "fillpick should be bindable from config");
}

/* devui, interp_toggle and lod_toggle are player controls, not KM_DEV
 * rows: each defaults to its key; devui also to a hold of Back. */
static void test_overlay_toggles_bound_by_default(void) {
    openpete_keymap_init();
    CHECK(n_sources(OPK_DEVUI) == 2 &&
          has_source(OPK_DEVUI, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "M")),
          "devui should default to M and a pad source");
    CHECK(n_sources(OPK_INTERP_TOGGLE) == 1 &&
          has_source(OPK_INTERP_TOGGLE, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "I")),
          "interp_toggle should default to I");
    CHECK(n_sources(OPK_LOD_TOGGLE) == 1 &&
          has_source(OPK_LOD_TOGGLE, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "L")),
          "lod_toggle should default to L");
    CHECK(n_sources(OPK_FREECAM) == 1 &&
          has_source(OPK_FREECAM, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "F")),
          "freecam should default to F alone");
}

/* Player 2 is a valid namespace but defaults to unbound - giving it P1's
 * keys would make every row collide with its P1 twin. */
static void test_pad2_unbound(void) {
    openpete_keymap_init();
    CHECK(n_sources(OPK_PAD2_CROSS) == 0, "pad2.cross must default unbound");
    CHECK(set1("pad2", "cross", "pad:south", 100, "global") == 0,
          "pad2 should still accept a binding");
}

/* ---- per-slot reads (players 2-4) ------------------------------------ */

/* Fake pad state addressed by slot, plus a record of which slot was asked:
 * a routing bug that always reads slot 0 still returns plausible values, so
 * the slot the resolver ASKED for is the thing under test. */
static unsigned char g_slot_btns[OPENPETE_PAD_SLOTS][256];
static int           g_slot_axes[OPENPETE_PAD_SLOTS][256];
static int           g_last_slot_asked;

static int slot_btn(void* ctx, int slot, int code) {
    (void)ctx;
    g_last_slot_asked = slot;
    return (slot >= 0 && slot < OPENPETE_PAD_SLOTS && code >= 0 && code < 256)
         ? g_slot_btns[slot][code] : 0;
}
static int slot_axis(void* ctx, int slot, int code) {
    (void)ctx;
    g_last_slot_asked = slot;
    return (slot >= 0 && slot < OPENPETE_PAD_SLOTS && code >= 0 && code < 256)
         ? g_slot_axes[slot][code] : 0;
}
static int slot_key(void* ctx, int c) { (void)ctx; return fake_key(c); }

static const openpete_keymap_reader_t g_slot_reader = {
    slot_key, slot_btn, slot_axis, NULL
};

static void slot_reset(void) {
    memset(g_slot_btns, 0, sizeof g_slot_btns);
    memset(g_slot_axes, 0, sizeof g_slot_axes);
    g_last_slot_asked = -1;
}

/* "pad2:" .. "pad4:" record a 1-based player; bare "pad:" stays unqualified,
 * which is what lets it mean "this block's player" inside [keys.padN] and
 * "player 1" everywhere else. */
static void test_pad_device_qualifier(void) {
    openpete_keymap_init();
    km_source_t s[KM_MAX_SOURCES];

    CHECK(set1("pad2", "cross", "pad2:south", 100, "global") == 0, "bind pad2:south");
    CHECK(openpete_keymap_sources(OPK_PAD2_CROSS, s, KM_MAX_SOURCES) == 1 &&
          s[0].device == 2, "pad2: must record player 2");

    CHECK(set1("pad3", "cross", "pad4:south", 100, "global") == 0, "bind pad4:south");
    CHECK(openpete_keymap_sources(OPK_PAD3_CROSS, s, KM_MAX_SOURCES) == 1 &&
          s[0].device == 4, "pad4: must record player 4");

    CHECK(set1("pad2", "circle", "pad:east", 100, "global") == 0, "bind pad:east");
    CHECK(openpete_keymap_sources(OPK_PAD2_CIRCLE, s, KM_MAX_SOURCES) == 1 &&
          s[0].device == 0, "bare pad: must stay unqualified");
}

/* An unqualified source reads the slot the caller names, so a [keys.pad2] row
 * can be written "pad:south" without every row needing a "pad2:" prefix. */
static void test_pad_reader_default_slot(void) {
    openpete_keymap_init();
    fake_reset();
    slot_reset();
    const int south = fake_to_code(KM_SRC_PADBTN, "south");

    CHECK(set1("pad2", "cross", "pad:south", 100, "global") == 0, "bind pad2.cross");

    g_slot_btns[1][south] = 1;    /* player 2's device only */
    CHECK(openpete_keymap_action_down_via(OPK_PAD2_CROSS, 1, &g_slot_reader) == 1,
          "default_slot 1 must read player 2's device");
    CHECK(g_last_slot_asked == 1, "resolver must ask for slot 1");

    g_slot_btns[1][south] = 0;
    g_slot_btns[0][south] = 1;    /* player 1 pressing must not reach player 2 */
    CHECK(openpete_keymap_action_down_via(OPK_PAD2_CROSS, 1, &g_slot_reader) == 0,
          "player 1's button must not press player 2's cross");
}

/* An explicit qualifier wins over the caller's default, so a slot can be
 * cross-wired on purpose. */
static void test_pad_explicit_qualifier_wins(void) {
    openpete_keymap_init();
    fake_reset();
    slot_reset();
    const int south = fake_to_code(KM_SRC_PADBTN, "south");

    CHECK(set1("pad2", "cross", "pad3:south", 100, "global") == 0, "bind pad3:south");
    g_slot_btns[2][south] = 1;
    CHECK(openpete_keymap_action_down_via(OPK_PAD2_CROSS, 1, &g_slot_reader) == 1,
          "explicit pad3: must read slot 2 despite default_slot 1");
    CHECK(g_last_slot_asked == 2, "resolver must ask for slot 2");
}

/* The raw axis form keeps the sign of "pad:-leftx" and routes to the same
 * slot, so a re-mapped stick cannot pick up a deadzone or a flipped sign the
 * default-mapped one does not have. */
static void test_pad_reader_axis(void) {
    openpete_keymap_init();
    fake_reset();
    slot_reset();
    const int leftx = fake_to_code(KM_SRC_PADAXIS, "leftx");

    CHECK(set1("pad2", "left_x", "pad:-leftx", 100, "global") == 0, "bind pad2.left_x");
    g_slot_axes[1][leftx] = 20000;
    CHECK(openpete_keymap_action_axis_raw_via(OPK_PAD2_LEFT_X, 1, &g_slot_reader)
              == -20000, "raw axis must invert and read slot 1");
    CHECK(g_last_slot_asked == 1, "axis resolver must ask for slot 1");
}

/* A qualified source with no slot-aware reader installed must read as
 * not-held rather than falling back to player 1, or a mistyped prefix
 * quietly controls someone else. */
static void test_pad_qualified_without_reader(void) {
    openpete_keymap_init();
    fake_reset();
    const int south = fake_to_code(KM_SRC_PADBTN, "south");

    CHECK(set1("host", "menu", "pad3:south", 100, "global") == 0, "bind pad3:south");
    g_btns[south] = 1;            /* player 1's device is pressing */
    openpete_keymap_poll();
    CHECK(openpete_key_down(OPK_MENU) == 0,
          "pad3: must not fall back to player 1's device");
}

/* A higher scope replaces the action's whole list; "none" and an empty
 * array both unbind. */
static void test_unbind(void) {
    openpete_keymap_init();
    CHECK(n_sources(OPK_REWIND) == 1, "rewind starts bound");

    CHECK(set1("host", "rewind", "none", 100, "global") == 0, "unbind rewind");
    CHECK(n_sources(OPK_REWIND) == 0, "\"none\" must unbind");

    /* An empty array is the same statement. */
    openpete_keymap_init();
    CHECK(openpete_keymap_set("host", "rewind", NULL, 0, 100, "global",
                              "test") == 0, "empty array accepted");
    CHECK(n_sources(OPK_REWIND) == 0, "[] must unbind");

    /* Unbinding at a HIGHER scope beats a lower scope's binding... */
    openpete_keymap_init();
    CHECK(set1("host", "teleport", "T", 100, "global") == 0, "bind at global");
    CHECK(set1("host", "teleport", "none", 200, "game.spyro-1") == 0,
          "unbind at game scope");
    CHECK(n_sources(OPK_TELEPORT) == 0, "higher scope's unbind must win");

    /* ...and a lower scope cannot resurrect it. */
    CHECK(set1("host", "teleport", "T", 50, "legacy") == 0, "low scope bind");
    CHECK(n_sources(OPK_TELEPORT) == 0, "lower scope must not override");
}

/* Within an action a scope REPLACES rather than appends - otherwise
 * unbinding could never be expressed. */
static void test_replace_not_append(void) {
    openpete_keymap_init();
    CHECK(n_sources(OPK_PAD_CROSS) == 2, "cross starts with key + pad");
    CHECK(set1("pad", "cross", "B", 100, "global") == 0, "rebind cross");
    CHECK(n_sources(OPK_PAD_CROSS) == 1, "rebinding replaces the whole list");
    CHECK(has_source(OPK_PAD_CROSS, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "B")),
          "cross should now be B");
    CHECK(!has_source(OPK_PAD_CROSS, KM_SRC_PADBTN,
                      fake_to_code(KM_SRC_PADBTN, "south")),
          "the old pad source must be gone");
}

/* Merge is per ACTION: naming one leaves the others alone. This is what
 * would break if a scope were ever made to replace a whole table. */
static void test_per_action_merge(void) {
    openpete_keymap_init();
    CHECK(set1("pad", "cross", "B", 100, "game.spyro-1") == 0, "one action");
    CHECK(n_sources(OPK_PAD_CIRCLE) == 2, "circle must keep its default");
    CHECK(n_sources(OPK_PAD_SQUARE) == 2, "square must keep its default");
    CHECK(strcmp(openpete_keymap_origin(OPK_PAD_CIRCLE), "default") == 0,
          "untouched actions stay at 'default'");
    CHECK(strcmp(openpete_keymap_origin(OPK_PAD_CROSS), "game.spyro-1") == 0,
          "the touched action records its scope");
}

/* Rank ordering, and the "ties go to the later call" rule that makes the
 * user-dir file beat the shipped one at the same tier. */
static void test_rank_order(void) {
    openpete_keymap_init();
    CHECK(set1("host", "menu", "O", 300, "disc.SCUS-94228") == 0, "high");
    CHECK(set1("host", "menu", "M", 100, "global") == 0, "low, ignored");
    CHECK(has_source(OPK_MENU, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "O")),
          "the higher rank must win regardless of call order");

    CHECK(set1("host", "menu", "N", 300, "disc.SCUS-94228 (user)") == 0, "tie");
    CHECK(has_source(OPK_MENU, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "N")),
          "an equal rank seen later must win");
}

static void test_parse_errors(void) {
    openpete_keymap_init();
    CHECK(set1("host", "menu", "Nonexistent Key", 100, "g") != 0,
          "an unknown key name must be an error");
    CHECK(set1("host", "menu", "pad:nosuchbutton", 100, "g") != 0,
          "an unknown pad name must be an error");
    CHECK(set1("host", "nosuchaction", "O", 100, "g") != 0,
          "an unknown action must be an error");
    CHECK(set1("nosuchdomain", "menu", "O", 100, "g") != 0,
          "an unknown namespace must be an error");
    /* '-' inverts an axis; on a button it is a mistake worth naming. */
    CHECK(set1("host", "menu", "pad:-south", 100, "g") != 0,
          "inverting a button must be an error");
    /* A failed parse must not have clobbered the previous binding. */
    CHECK(has_source(OPK_MENU, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "Escape")),
          "a rejected value must leave the old binding intact");
}

/* Scalar "C,pad:south" and array ["C","pad:south"] are one binding. */
static void test_scalar_and_array_equivalent(void) {
    openpete_keymap_init();
    CHECK(set1("host", "teleport", "C,pad:south", 100, "g") == 0, "scalar");
    int a = n_sources(OPK_TELEPORT);

    openpete_keymap_init();
    const char* toks[2] = { "C", "pad:south" };
    CHECK(openpete_keymap_set("host", "teleport", toks, 2, 100, "g",
                              "test") == 0, "array");
    CHECK(n_sources(OPK_TELEPORT) == a && a == 2,
          "the two spellings must produce the same binding");
}

/* A stock config must resolve with zero warnings, or the warning becomes
 * wallpaper nobody reads. */
static void test_stock_resolve_is_clean(void) {
    openpete_keymap_init();
    int w = openpete_keymap_resolve();
    CHECK(w == 0, "stock defaults must resolve with no warnings (got %d)", w);
}

static void test_conflict_detection(void) {
    /* Two ALWAYS actions on one key: a real clash. */
    openpete_keymap_init();
    CHECK(set1("host", "teleport", "F", 100, "g") == 0, "teleport -> F");
    CHECK(openpete_keymap_resolve() > 0,
          "two ALWAYS actions on F must be reported");

    /* Disjoint contexts sharing a key: not a clash. The pad's left-stick
     * keys (GAMEPLAY) and the freecam's fly keys (FREECAM) have shared
     * WASD since long before the keymap existed. */
    openpete_keymap_init();
    CHECK(has_source(OPK_PAD_STICK_UP, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "W")),
          "pad.stick_up is W");
    CHECK(has_source(OPK_FLY_FORWARD, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "W")),
          "host.fly_forward is W");
    CHECK(openpete_keymap_resolve() == 0,
          "context-disjoint sharing must stay quiet");
}

/* KM_REQUIRED warns only when EVERY source is gone - a pad-only player
 * unbinding the keyboard half of cross is legitimate. */
static void test_required_warns_only_when_fully_unbound(void) {
    openpete_keymap_init();
    CHECK(set1("pad", "cross", "pad:south", 100, "g") == 0, "pad-only cross");
    CHECK(openpete_keymap_resolve() == 0,
          "dropping one of two sources must not warn");

    openpete_keymap_init();
    CHECK(set1("pad", "cross", "none", 100, "g") == 0, "unbind cross");
    CHECK(openpete_keymap_resolve() > 0,
          "unbinding a required action entirely must warn");
}

/* The edge latch: one press = one pressed(), held stays down, and an
 * unbound action never fires. */
static void test_edge_latch(void) {
    openpete_keymap_init();
    fake_reset();
    int o = fake_to_code(KM_SRC_KEY, "Escape");

    openpete_keymap_poll();
    CHECK(!openpete_key_pressed(OPK_MENU), "no press before the key is held");

    g_keys[o] = 1;
    openpete_keymap_poll();
    CHECK(openpete_key_pressed(OPK_MENU), "first poll while held = a press");
    CHECK(openpete_key_down(OPK_MENU), "and it reads as down");

    openpete_keymap_poll();
    CHECK(!openpete_key_pressed(OPK_MENU), "a held key must not re-fire");
    CHECK(openpete_key_down(OPK_MENU), "but it is still down");

    g_keys[o] = 0;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_MENU), "released");
    g_keys[o] = 1;
    openpete_keymap_poll();
    CHECK(openpete_key_pressed(OPK_MENU), "re-press fires again");

    /* An unbound action can never produce an edge - no stuck latch. */
    fake_reset();
    openpete_keymap_poll();
    openpete_keymap_poll();
    CHECK(!openpete_key_pressed(OPK_FILLPICK), "unbound: never pressed");
    CHECK(!openpete_key_down(OPK_FILLPICK), "unbound: never down");
}

/* Suspending the keyboard must leave PAD sources live: mouse-only devui
 * interaction is meant to leave the game playable. */
static void test_suspend_is_keyboard_only(void) {
    openpete_keymap_init();
    fake_reset();
    CHECK(set1("host", "freecam", "F,pad:leftstick", 100, "g") == 0,
          "freecam -> F + pad:leftstick");
    g_keys[fake_to_code(KM_SRC_KEY, "F")]           = 1;
    g_btns[fake_to_code(KM_SRC_PADBTN, "leftstick")] = 1;

    openpete_keymap_suspend_keyboard(1, NULL, 0);
    openpete_keymap_poll();
    CHECK(openpete_key_down(OPK_FREECAM),
          "a pad source must survive a keyboard suspend");

    g_btns[fake_to_code(KM_SRC_PADBTN, "leftstick")] = 0;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_FREECAM),
          "the keyboard source must be inert while suspended");

    openpete_keymap_suspend_keyboard(0, NULL, 0);
    openpete_keymap_poll();
    CHECK(openpete_key_down(OPK_FREECAM), "and live again after resume");
}

/* The pad suspension: live pad sources go neutral except for the exempt
 * set; keyboard sources and the pad decode's reader stay live. */
static int rd_btn(void* ctx, int slot, int b) { (void)ctx; (void)slot; return fake_btn(b); }

static void test_suspend_pad_exempt_set(void) {
    openpete_keymap_init();
    fake_reset();
    CHECK(set1("host", "savestate_save", "1,pad:misc1", 100, "g") == 0,
          "savestate_save -> 1 + pad:misc1");
    const int misc1 = fake_to_code(KM_SRC_PADBTN, "misc1");
    const int south = fake_to_code(KM_SRC_PADBTN, "south");
    g_btns[misc1] = 1;
    g_btns[south] = 1;
    const openpete_action_t ex[] = { OPK_DEVUI, OPK_DEVUI_ACTIVATE };
    openpete_keymap_suspend_pad(1, ex, 2);
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_STATE_SAVE), "a pad hotkey is inert under the pad suspension");
    CHECK(openpete_key_down(OPK_DEVUI_ACTIVATE), "an exempt action reads its pad source");
    openpete_keymap_reader_t rd;
    memset(&rd, 0, sizeof rd);
    rd.pad_button = rd_btn;
    CHECK(openpete_keymap_action_down_via(OPK_PAD_CROSS, 0, &rd),
          "the pad decode's reader is not suspended");
    g_btns[misc1] = 0;
    g_keys[fake_to_code(KM_SRC_KEY, "1")] = 1;
    openpete_keymap_poll();
    CHECK(openpete_key_down(OPK_STATE_SAVE), "a key source survives the pad suspension");
    openpete_keymap_suspend_pad(0, NULL, 0);
    g_keys[fake_to_code(KM_SRC_KEY, "1")] = 0;
    g_btns[misc1] = 1;
    openpete_keymap_poll();
    CHECK(openpete_key_down(OPK_STATE_SAVE), "and the pad source is live after resume");

    /* The keyboard exemption is a set too. */
    fake_reset();
    CHECK(set1("host", "devui_up", "Up", 100, "g") == 0, "devui_up -> Up");
    g_keys[fake_to_code(KM_SRC_KEY, "M")]  = 1;
    g_keys[fake_to_code(KM_SRC_KEY, "Up")] = 1;
    g_keys[fake_to_code(KM_SRC_KEY, "F")]  = 1;
    const openpete_action_t kex[] = { OPK_DEVUI, OPK_DEVUI_UP };
    openpete_keymap_suspend_keyboard(1, kex, 2);
    openpete_keymap_poll();
    CHECK(openpete_key_down(OPK_DEVUI) && openpete_key_down(OPK_DEVUI_UP),
          "both members of the keyboard exempt set read");
    CHECK(!openpete_key_down(OPK_FREECAM), "a non-member is suspended");
    openpete_keymap_suspend_keyboard(0, NULL, 0);
}

/* A hold: source reads held only after KM_HOLD_MS of unbroken hold, fires
 * one press edge, and resets on release. */
static void test_hold_source(void) {
    openpete_keymap_init();
    fake_reset();
    const int back = fake_to_code(KM_SRC_PADBTN, "back");
    CHECK(has_source(OPK_DEVUI, KM_SRC_PADBTN, back), "devui defaults to a Back source");
    km_source_t src[KM_MAX_SOURCES];
    int n = openpete_keymap_sources(OPK_DEVUI, src, KM_MAX_SOURCES);
    int holds = 0;
    for (int i = 0; i < n; i++) holds += src[i].hold;
    CHECK(holds == 1, "the Back source is a hold source");

    g_btns[back] = 1;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_DEVUI), "a fresh hold reads not held");
    g_now_ms += KM_HOLD_MS - 1;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_DEVUI), "one ms short of the hold reads not held");
    g_now_ms += 1;
    openpete_keymap_poll();
    CHECK(openpete_key_pressed(OPK_DEVUI), "the hold fires one press edge");
    openpete_keymap_poll();
    CHECK(openpete_key_down(OPK_DEVUI) && !openpete_key_pressed(OPK_DEVUI),
          "and stays down without re-firing");
    g_btns[back] = 0;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_DEVUI), "a release ends it");
    g_btns[back] = 1;
    g_now_ms += 10000;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_DEVUI), "a new hold starts its own timer");

    /* The pad decode never sees a hold, and select still reads Back. */
    openpete_keymap_reader_t rd;
    memset(&rd, 0, sizeof rd);
    rd.pad_button = rd_btn;
    CHECK(openpete_keymap_action_down_via(OPK_PAD_SELECT, 0, &rd), "select reads Back");

    /* Refusals: the pad domains, an axis action, a wheel, a nested hold. */
    CHECK(set1("pad", "select", "hold:pad:back", 100, "g") < 0, "hold refused in [keys.pad]");
    CHECK(set1("host", "fly_axis_x", "hold:pad:south", 100, "g") < 0, "hold refused on an axis action");
    CHECK(set1("host", "pause", "hold:mouse:wheel_up", 100, "g") < 0, "hold refused on a wheel pulse");
    CHECK(set1("host", "pause", "hold:hold:pad:south", 100, "g") < 0, "hold refused nested");
    CHECK(set1("host", "pause", "hold:F7", 100, "g") == 0, "hold accepted on a key");

    char lbl[128];
    openpete_keymap_init();
    openpete_keymap_action_label(OPK_DEVUI, lbl, sizeof lbl);
    CHECK(strstr(lbl, "(hold)") != NULL, "the player label marks the hold: %s", lbl);
}

/* KM_CTX_DEVUI shares the pad with GAMEPLAY and the menu without a report:
 * the contexts are never live together. */
static void test_devui_ctx_disjoint(void) {
    openpete_keymap_init();
    CHECK(has_source(OPK_DEVUI_ACTIVATE, KM_SRC_PADBTN, fake_to_code(KM_SRC_PADBTN, "south")),
          "devui_activate is pad:south, as cross is");
    CHECK(openpete_keymap_resolve() == 0, "devui navigation beside the PSX pad stays quiet");
    CHECK(set1("host", "pause", "pad:dpup", 100, "g") == 0, "pause -> pad:dpup");
    CHECK(openpete_keymap_resolve() > 0, "an ALWAYS action on devui_up's source is reported");
}

static void test_axis(void) {
    openpete_keymap_init();
    fake_reset();
    int lx = fake_to_code(KM_SRC_PADAXIS, "leftx");
    int ly = fake_to_code(KM_SRC_PADAXIS, "lefty");

    CHECK(openpete_key_axis(OPK_FLY_AXIS_X) == 0.0f, "centred reads 0");

    g_axes[lx] = 4000;   /* inside the deadzone */
    CHECK(openpete_key_axis(OPK_FLY_AXIS_X) == 0.0f, "deadzone reads 0");

    g_axes[lx] = 32767;
    CHECK(openpete_key_axis(OPK_FLY_AXIS_X) > 0.99f, "full throw reads +1");

    /* fly_axis_y is "pad:-lefty": stick up (negative on SDL) = forward. */
    g_axes[ly] = -32767;
    CHECK(openpete_key_axis(OPK_FLY_AXIS_Y) > 0.99f,
          "an inverted axis must flip the sign");

    /* A digital source on an axis action reads as full throw, so a
     * keyboard binding on an axis still works. */
    openpete_keymap_init();
    fake_reset();
    CHECK(set1("host", "fly_axis_x", "D", 100, "g") == 0, "key on an axis");
    g_keys[fake_to_code(KM_SRC_KEY, "D")] = 1;
    CHECK(openpete_key_axis(OPK_FLY_AXIS_X) > 0.99f,
          "a held key on an axis action reads as full throw");
}

/* Axis-as-digital: the analog triggers bound to speed_up/speed_down must
 * only register past a real pull, not a resting finger. */
static void test_axis_as_digital(void) {
    openpete_keymap_init();
    fake_reset();
    int rt = fake_to_code(KM_SRC_PADAXIS, "righttrigger");

    g_axes[rt] = 4000;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_SPEED_UP), "a light trigger must not press");

    g_axes[rt] = 30000;
    openpete_keymap_poll();
    CHECK(openpete_key_down(OPK_SPEED_UP), "a pulled trigger presses");
}

/* Quit is a binding like any other: unbound by default, bindable, and
 * unbindable again. The event filter asks the keymap by scancode (it sees
 * one key event at a time and cannot use the polled latch), so that query
 * is what these assert. */
static void test_quit_binding(void) {
    int esc = fake_to_code(KM_SRC_KEY, "Escape");
    int f10 = fake_to_code(KM_SRC_KEY, "F10");

    openpete_keymap_init();
    CHECK(!openpete_keymap_key_binds(OPK_QUIT, esc),
          "quit must default to unbound");
    /* An unbound quit must NOT warn: the window close button still quits. */
    CHECK(openpete_keymap_resolve() == 0,
          "an unbound quit must not warn (it is not KM_REQUIRED)");

    CHECK(set1("host", "quit", "Escape", 100, "global") == 0, "bind quit");
    CHECK(openpete_keymap_key_binds(OPK_QUIT, esc), "quit should now be Escape");
    CHECK(set1("host", "quit", "none", 200, "global") == 0, "unbind quit");
    CHECK(!openpete_keymap_key_binds(OPK_QUIT, esc),
          "quit = \"none\" must stop Escape quitting");

    openpete_keymap_init();
    CHECK(set1("host", "quit", "Escape", 100, "global") == 0, "bind quit");
    CHECK(set1("host", "quit", "F10", 200, "global") == 0, "rebind quit");
    CHECK(openpete_keymap_key_binds(OPK_QUIT, f10), "quit should now be F10");
    CHECK(!openpete_keymap_key_binds(OPK_QUIT, esc),
          "rebinding quit must release Escape");

    /* The query is by scancode and must not match other actions' keys. */
    CHECK(!openpete_keymap_key_binds(OPK_MENU, f10),
          "menu is not bound to F10");
}

/* The event filter and the polled bindings are two paths onto one
 * physical press: the filter swallows the EVENT, but the poll reads
 * SDL_GetKeyboardState and still sees the key held. With `menu` bound to
 * the key the filter dismisses on, that closed the menu and reopened it
 * in the same press. Consuming the key is the fix. */
static void test_consume_key(void) {
    openpete_keymap_init();
    fake_reset();
    /* The default binds menu on the dismiss key. */
    int esc = fake_to_code(KM_SRC_KEY, "Escape");

    /* Press 1, menu closed: the filter does nothing (nothing to dismiss)
     * and the poll's rising edge opens the menu. */
    g_keys[esc] = 1;
    openpete_keymap_poll();
    CHECK(openpete_key_pressed(OPK_MENU), "press 1 opens the menu");

    g_keys[esc] = 0;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_MENU), "released between presses");

    /* Press 2, menu now OPEN. This is the reported bug: the filter
     * dismisses and swallows the EVENT, but the poll still sees a fresh
     * rising edge in SDL's key state and toggles the menu straight back
     * open. The consume is what has to stop it - and note this only
     * reproduces on a fresh press, which is why the edge matters. */
    g_keys[esc] = 1;
    openpete_keymap_consume_key(esc);      /* what the filter does */
    openpete_keymap_poll();
    CHECK(!openpete_key_pressed(OPK_MENU),
          "a consumed press must not re-fire and reopen the menu");
    CHECK(!openpete_key_down(OPK_MENU), "a consumed key reads as not held");

    /* Still held, several polls later: still suppressed. */
    openpete_keymap_poll();
    openpete_keymap_poll();
    CHECK(!openpete_key_pressed(OPK_MENU), "still suppressed while held");

    /* Release, then press again: live once more, so dismiss and toggle
     * compose into normal open/close behaviour. */
    g_keys[esc] = 0;
    openpete_keymap_poll();
    g_keys[esc] = 1;
    openpete_keymap_poll();
    CHECK(openpete_key_pressed(OPK_MENU),
          "a fresh press after release must fire again");

    /* Consuming one key must not deafen another. */
    openpete_keymap_init();
    fake_reset();
    openpete_keymap_consume_key(fake_to_code(KM_SRC_KEY, "O"));
    g_keys[fake_to_code(KM_SRC_KEY, "F7")] = 1;
    openpete_keymap_poll();
    CHECK(openpete_key_pressed(OPK_PAUSE),
          "consuming one key must not suppress others");
}

static void test_mod_bindings(void) {
    openpete_keymap_init();
    km_source_t s[KM_MAX_SOURCES];
    int n = 0;
    CHECK(!openpete_keymap_mod_binding("supercharge", "warp", s, 4, &n),
          "an unconfigured mod binding must fall through to the mod's own");

    const char* tok[1] = { "F6" };
    CHECK(openpete_keymap_set_mod("supercharge", "warp", tok, 1, 100,
                                  "test") == 0, "set a mod binding");
    CHECK(openpete_keymap_mod_binding("supercharge", "warp", s, 4, &n),
          "a configured mod binding must be found");
    CHECK(n == 1 && s[0].code == fake_to_code(KM_SRC_KEY, "F6"),
          "and carry the configured key");
}

/* ---- mouse sources ---------------------------------------------------- */

static int set_mod1(const char* mod, const char* name, const char* val) {
    const char* toks[1] = { val };
    return openpete_keymap_set_mod(mod, name, toks, 1, 100, "test");
}

/* Parse acceptance and rejection by domain and by action class. */
static void test_mouse_parse(void) {
    openpete_keymap_init();
    CHECK(set1("host", "teleport", "mouse:right", 100, "g") == 0,
          "mouse:right on a host digital action");
    CHECK(has_source(OPK_TELEPORT, KM_SRC_MOUSEBTN, KM_MOUSE_RIGHT),
          "and it lands as KM_SRC_MOUSEBTN right");
    CHECK(set1("host", "look_axis_x", "pad:rightx,mouse:x", 100, "g") == 0,
          "mouse:x alongside a stick on a KM_DELTA action");
    CHECK(has_source(OPK_LOOK_AXIS_X, KM_SRC_MOUSEAXIS, KM_MOUSE_X),
          "and it lands as KM_SRC_MOUSEAXIS x");
    CHECK(set1("host", "look_axis_y", "mouse:-y", 100, "g") == 0, "mouse:-y");
    {
        km_source_t src[KM_MAX_SOURCES];
        int n = openpete_keymap_sources(OPK_LOOK_AXIS_Y, src, KM_MAX_SOURCES);
        CHECK(n == 1 && src[0].kind == KM_SRC_MOUSEAXIS && src[0].invert < 0,
              "mouse:-y carries the invert");
    }
    CHECK(set1("host", "speed_up", "],mouse:wheel_up", 100, "g") == 0,
          "a wheel pulse on a digital action");
    CHECK(set_mod1("probe", "fire", "mouse:right") == 0,
          "mouse:right on a mod binding");
    CHECK(set_mod1("probe", "go", "pad:north") == 0,
          "pad:north on a mod binding");

    /* Rejections. */
    CHECK(set1("pad", "cross", "mouse:left", 100, "g") != 0,
          "the pad domain must reject a mouse source");
    CHECK(set1("pad2", "cross", "mouse:left", 100, "g") != 0, "pad2 too");
    CHECK(set1("pad3", "cross", "mouse:left", 100, "g") != 0, "pad3 too");
    CHECK(set1("pad4", "cross", "mouse:left", 100, "g") != 0, "pad4 too");
    CHECK(set1("host", "teleport", "mouse:x", 100, "g") != 0,
          "mouse motion on a digital action must be an error");
    CHECK(set1("host", "fly_axis_x", "mouse:x", 100, "g") != 0,
          "mouse motion on an axis without a delta consumer must be an error");
    CHECK(set1("host", "menu_close", "mouse:wheel_up", 100, "g") != 0,
          "a pulse on a KM_LEVEL action must be an error");
    CHECK(set1("host", "fly_forward", "mouse:wheel_down", 100, "g") != 0,
          "a pulse on a level-read freecam key must be an error");
    CHECK(set1("host", "fly_axis_x", "mouse:wheel_up", 100, "g") != 0,
          "a pulse on an axis action must be an error");
    CHECK(set_mod1("probe", "fire", "mouse:wheel_up") != 0,
          "a pulse on a mod binding must be an error");
    CHECK(set_mod1("probe", "fire", "mouse:x") != 0,
          "motion on a mod binding must be an error");
    CHECK(set1("host", "teleport", "mouse:-left", 100, "g") != 0,
          "inverting a mouse button must be an error");
    CHECK(set1("host", "teleport", "mouse:nosuch", 100, "g") != 0,
          "an unknown mouse name must be an error");
    CHECK(has_source(OPK_TELEPORT, KM_SRC_MOUSEBTN, KM_MOUSE_RIGHT),
          "a rejected value must leave the old binding intact");
}

/* Resolve-time clauses: the MENU-context unreachable warning, the menu
 * gesture warning, the one-device clash, and the left_bound predicate. */
static void test_mouse_resolve(void) {
    openpete_keymap_init();
    CHECK(set1("host", "menu_up", "Up,mouse:x1", 100, "g") == 0, "menu_up+x1");
    CHECK(openpete_keymap_resolve() == 1,
          "a mouse source on a MENU action is unreachable and warns once");

    openpete_keymap_init();
    CHECK(set1("host", "menu_close", "pad:start,mouse:middle", 100, "g") == 0,
          "menu_close+middle");
    CHECK(openpete_keymap_resolve() == 0,
          "menu_close is exempt from the unreachable clause");

    openpete_keymap_init();
    CHECK(set1("host", "menu", "O,mouse:left", 100, "g") == 0, "menu+left");
    CHECK(openpete_keymap_resolve() == 1,
          "menu on mouse:left is a menu gesture and warns");
    CHECK(openpete_keymap_mouse_left_bound(),
          "left_bound is true with a mouse:left source");

    openpete_keymap_init();
    CHECK(set1("host", "menu", "O,mouse:wheel_up", 100, "g") == 0, "menu+wheel");
    CHECK(openpete_keymap_resolve() == 1,
          "menu on a wheel source is a menu gesture and warns");

    openpete_keymap_init();
    CHECK(set1("host", "menu", "O,mouse:middle", 100, "g") == 0, "menu+middle");
    CHECK(openpete_keymap_resolve() == 0,
          "menu on mouse:middle is reachable and not a gesture");
    CHECK(!openpete_keymap_mouse_left_bound(),
          "left_bound is false without a mouse:left source");

    openpete_keymap_init();
    CHECK(set1("host", "teleport", "mouse:left", 100, "g") == 0, "teleport left");
    CHECK(set1("host", "pause", "mouse:left", 100, "g") == 0, "pause left");
    CHECK(openpete_keymap_resolve() == 1,
          "mouse:left bound twice in one context is a clash");

    /* A mod binding counts toward left_bound, and is seen when registered
     * after resolve. */
    openpete_keymap_init();
    CHECK(openpete_keymap_resolve() == 0, "stock");
    CHECK(!openpete_keymap_mouse_left_bound(), "stock: no mouse:left");
    CHECK(set_mod1("probe", "fire", "mouse:left") == 0, "mod left");
    CHECK(openpete_keymap_mouse_left_bound(),
          "a mod binding registered after resolve sets left_bound");
    CHECK(set_mod1("probe", "fire", "F6") == 0, "mod rebound");
    CHECK(!openpete_keymap_mouse_left_bound(),
          "and rebinding it clears left_bound");
}

/* Buttons are levels through the arbitrated view; pulses count notches
 * per poll; deltas sum signed counts with the invert applied. */
static void test_mouse_live(void) {
    openpete_keymap_init();
    fake_reset();
    CHECK(set1("host", "teleport", "mouse:right", 100, "g") == 0, "teleport right");
    CHECK(set1("host", "speed_up", "],mouse:wheel_up", 100, "g") == 0, "speed_up");
    CHECK(set1("host", "speed_down", "[,mouse:wheel_down", 100, "g") == 0, "speed_down");
    CHECK(set1("host", "look_axis_x", "mouse:x", 100, "g") == 0, "look x");
    CHECK(set1("host", "look_axis_y", "mouse:-y", 100, "g") == 0, "look -y");
    openpete_keymap_poll();

    /* Button level and edge. */
    g_mouse_mask = OPENPETE_MOUSE_RIGHT;
    openpete_keymap_poll();
    CHECK(openpete_key_down(OPK_TELEPORT), "right held reads down");
    CHECK(openpete_key_pressed(OPK_TELEPORT), "and pressed on its first poll");
    openpete_keymap_poll();
    CHECK(!openpete_key_pressed(OPK_TELEPORT), "held: no second press");
    g_mouse_mask = 0;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_TELEPORT), "released");

    /* Two notches in one poll: two pulses, one press. */
    g_mouse_wheel += 2 * OPENPETE_MOUSE_WHEEL_UNIT;
    openpete_keymap_poll();
    CHECK(openpete_key_pulses(OPK_SPEED_UP) == 2, "two notches -> two pulses (got %d)",
          openpete_key_pulses(OPK_SPEED_UP));
    CHECK(openpete_key_pressed(OPK_SPEED_UP), "pulses read as pressed");
    CHECK(!openpete_key_edge(OPK_SPEED_UP), "but not as a held edge");
    CHECK(openpete_key_pulses(OPK_SPEED_DOWN) == 0, "the other direction is quiet");
    /* Consecutive polls: one notch each. */
    g_mouse_wheel -= OPENPETE_MOUSE_WHEEL_UNIT;
    openpete_keymap_poll();
    CHECK(openpete_key_pulses(OPK_SPEED_DOWN) == 1, "one notch down");
    g_mouse_wheel -= OPENPETE_MOUSE_WHEEL_UNIT;
    openpete_keymap_poll();
    CHECK(openpete_key_pulses(OPK_SPEED_DOWN) == 1, "and another on the next poll");
    openpete_keymap_poll();
    CHECK(openpete_key_pulses(OPK_SPEED_DOWN) == 0 && !openpete_key_pressed(OPK_SPEED_DOWN),
          "no notch: no pulse, no press");
    /* Fractional travel accumulates across polls. */
    g_mouse_wheel += OPENPETE_MOUSE_WHEEL_UNIT / 2;
    openpete_keymap_poll();
    CHECK(openpete_key_pulses(OPK_SPEED_UP) == 0, "half a notch is not a pulse");
    g_mouse_wheel += OPENPETE_MOUSE_WHEEL_UNIT / 2;
    openpete_keymap_poll();
    CHECK(openpete_key_pulses(OPK_SPEED_UP) == 1, "two halves make one notch");
    /* The key edge and a pulse in one poll are both there for a consumer
     * stepping edge + pulses; pressed still reads once. */
    g_keys[fake_to_code(KM_SRC_KEY, "]")] = 1;
    g_mouse_wheel += OPENPETE_MOUSE_WHEEL_UNIT;
    openpete_keymap_poll();
    CHECK(openpete_key_edge(OPK_SPEED_UP) == 1 && openpete_key_pulses(OPK_SPEED_UP) == 1,
          "edge and pulse in one poll");
    CHECK(openpete_key_pressed(OPK_SPEED_UP) == 1, "pressed is one, not two");
    g_keys[fake_to_code(KM_SRC_KEY, "]")] = 0;

    /* Deltas: signed counts since the previous poll, invert applied. */
    g_mouse_motion[0] += 17; g_mouse_motion[1] += -5;
    openpete_keymap_poll();
    CHECK(openpete_key_delta(OPK_LOOK_AXIS_X) == 17, "x delta 17 (got %lld)",
          openpete_key_delta(OPK_LOOK_AXIS_X));
    CHECK(openpete_key_delta(OPK_LOOK_AXIS_Y) == 5, "mouse:-y negates (got %lld)",
          openpete_key_delta(OPK_LOOK_AXIS_Y));
    openpete_keymap_poll();
    CHECK(openpete_key_delta(OPK_LOOK_AXIS_X) == 0, "no motion: zero delta");
    CHECK(openpete_key_axis(OPK_LOOK_AXIS_X) == 0.0f,
          "a displacement has no level: the axis read stays centred");
}

/* The arbitrated view: neutral while an overlay owns the mouse, markers
 * snapped so nothing made under it is delivered when it closes; menu and
 * menu_close exempt from the menu's ownership only. */
static void test_mouse_owned(void) {
    openpete_keymap_init();
    fake_reset();
    CHECK(set1("host", "teleport", "mouse:right", 100, "g") == 0, "teleport right");
    CHECK(set1("host", "menu", "O,mouse:middle", 100, "g") == 0, "menu middle");
    CHECK(set1("host", "speed_up", "mouse:wheel_up", 100, "g") == 0, "speed_up");
    CHECK(set1("host", "look_axis_x", "mouse:x", 100, "g") == 0, "look x");
    openpete_keymap_poll();

    g_mouse_owned = OPENPETE_MOUSE_OWNED_MENU;
    g_mouse_mask = OPENPETE_MOUSE_RIGHT | OPENPETE_MOUSE_MIDDLE;
    g_mouse_wheel += 3 * OPENPETE_MOUSE_WHEEL_UNIT;
    g_mouse_motion[0] += 40;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_TELEPORT), "menu owns: a button reads neutral");
    CHECK(openpete_key_down(OPK_MENU), "menu is exempt from the menu's ownership");
    CHECK(openpete_key_pulses(OPK_SPEED_UP) == 0, "menu owns: no pulses");
    CHECK(openpete_key_delta(OPK_LOOK_AXIS_X) == 0, "menu owns: no delta");
    {
        long long w, mx, my;
        openpete_keymap_mouse_markers(&w, &mx, &my);
        CHECK(w == g_mouse_wheel && mx == g_mouse_motion[0],
              "the markers advanced to the totals under the owner");
    }
    g_mouse_owned = OPENPETE_MOUSE_OWNED_NONE;
    openpete_keymap_poll();
    CHECK(openpete_key_pulses(OPK_SPEED_UP) == 0 && openpete_key_delta(OPK_LOOK_AXIS_X) == 0,
          "motion made under the owner is never delivered late");
    CHECK(openpete_key_down(OPK_TELEPORT), "released ownership: the held button reads");

    g_mouse_owned = OPENPETE_MOUSE_OWNED_DEVUI;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_MENU), "menu is not exempt from the overlay's ownership");
    CHECK(!openpete_key_down(OPK_TELEPORT), "nor is anything else");
}

/* Mouse sources never route into a pad reader, and the formatter prints
 * them with the mouse: prefix. */
static void test_mouse_reader_and_format(void) {
    openpete_keymap_init();
    fake_reset();
    CHECK(set1("host", "teleport", "mouse:right", 100, "g") == 0, "teleport right");
    g_mouse_mask = OPENPETE_MOUSE_RIGHT;
    openpete_keymap_poll();
    openpete_keymap_reader_t rd = { NULL, NULL, NULL, NULL };
    CHECK(!openpete_keymap_action_down_via(OPK_TELEPORT, 0, &rd),
          "a pad reader never sees a mouse source");
    CHECK(openpete_keymap_action_down_via(OPK_TELEPORT, 0, NULL),
          "the live path does");
    CHECK(set_mod1("probe", "fire", "mouse:right") == 0, "mod right");
    CHECK(strcmp(openpete_keymap_mod_effective_key("probe", "fire", "Q"),
                 "mouse:right") == 0, "the effective-key formatter spells mouse:");
}

/* A mod's own key registers at rank 0 under mod.<id>; a config row replaces
 * it whatever the order; a key that fails to parse registers unbound; a
 * disabled mod's mouse:left leaves the double-click scan. */
static void test_mod_defaults(void) {
    openpete_keymap_init();
    km_source_t s[KM_MAX_SOURCES];
    int n = -1;
    CHECK(openpete_keymap_set_mod_default("dflt", "go", "pad:north", "m.toml") == 0,
          "a mod default parses pad:north under mod.<id>");
    CHECK(openpete_keymap_mod_binding("dflt", "go", s, 4, &n) && n == 1 &&
          s[0].kind == KM_SRC_PADBTN, "and is found as the binding");
    CHECK(openpete_keymap_set_mod_default("dflt", "bad", "mouse:wheel_up", "m.toml") != 0,
          "a pulse key is a parse error");
    CHECK(openpete_keymap_mod_binding("dflt", "bad", s, 4, &n) && n == 0,
          "and registers unbound");

    /* Override before default (the boot order): the default does not win. */
    CHECK(set_mod1("dflt", "fire", "F6") == 0, "config override first");
    CHECK(openpete_keymap_set_mod_default("dflt", "fire", "mouse:left", "m.toml") == 0,
          "default after");
    CHECK(openpete_keymap_mod_binding("dflt", "fire", s, 4, &n) && n == 1 &&
          s[0].kind == KM_SRC_KEY, "the override stands");
    CHECK(!openpete_keymap_mouse_left_bound(), "the shadowed default does not bind left");
    /* Default then override. */
    CHECK(openpete_keymap_set_mod_default("dflt", "warp", "mouse:left", "m.toml") == 0,
          "default warp");
    CHECK(openpete_keymap_mouse_left_bound(), "a mouse:left default binds left");
    openpete_keymap_set_mod_enabled("dflt", 0);
    CHECK(!openpete_keymap_mouse_left_bound(), "a disabled mod's default leaves the scan");
    openpete_keymap_set_mod_enabled("dflt", 1);
    CHECK(openpete_keymap_mouse_left_bound(), "and re-enters on enable");
    CHECK(set_mod1("dflt", "warp", "F5") == 0, "override warp");
    CHECK(openpete_keymap_mod_binding("dflt", "warp", s, 4, &n) && n == 1 &&
          s[0].kind == KM_SRC_KEY, "the override replaces the default");
    CHECK(!openpete_keymap_mouse_left_bound(), "and left is unbound again");
    CHECK(strcmp(openpete_keymap_mod_effective_key("dflt", "go", "?"), "pad:north") == 0,
          "the effective key of a default is its parsed spelling");
}


/* ---- binding editor ------------------------------------------------- */

static int K(const char* n)  { return fake_to_code(KM_SRC_KEY, n); }
static int PB(const char* n) { return fake_to_code(KM_SRC_PADBTN, n); }
static int PA(const char* n) { return fake_to_code(KM_SRC_PADAXIS, n); }

/* A fresh store with the fakes released and the saved layer's bounds set. */
static void editor_reset(void) {
    fake_reset();
    openpete_keymap_set_layer_bounds(5000, 2000);
    openpete_keymap_init();
    (void)openpete_keymap_resolve();
    openpete_keymap_poll();
    openpete_keymap_poll();
}

static openpete_keymap_edit_t g_edit;

/* Take the result of the capture that just ended; KM_EDIT_NONE when none. */
static int take(void) {
    memset(&g_edit, 0, sizeof g_edit);
    return openpete_keymap_capture_take_result(&g_edit) ? g_edit.kind : KM_EDIT_NONE;
}

/* Begin a capture and poll once so it arms. */
static int begin(openpete_action_t a, int slot, int hold) {
    int rc = openpete_keymap_capture_begin(a, slot, hold, &g_edit);
    openpete_keymap_poll();
    return rc;
}

/* Press and release one fake input across the capture: candidate, then commit. */
static int tap_key(const char* name) {
    g_keys[K(name)] = 1; openpete_keymap_poll();
    g_keys[K(name)] = 0; openpete_keymap_poll();
    return take();
}
static int tap_btn(const char* name) {
    g_btns[PB(name)] = 1; openpete_keymap_poll();
    g_btns[PB(name)] = 0; openpete_keymap_poll();
    return take();
}

/* Whether an action's binding is exactly the listed tokens, in order. */
static int binding_is(openpete_action_t a, const char* want) {
    km_source_t s[KM_MAX_SOURCES];
    int n = openpete_keymap_sources(a, s, KM_MAX_SOURCES);
    char got[256] = "";
    for (int i = 0; i < n; i++) {
        char t[64];
        openpete_keymap_source_token(&s[i], t, sizeof t);
        strcat(got, i ? "," : "");
        strcat(got, t);
    }
    if (strcmp(got, want) != 0) printf("  binding: '%s' want '%s'\n", got, want);
    return strcmp(got, want) == 0;
}

/* The keys report, for the shared-input list. */
static int report_has(const char* needle) {
    FILE* f = tmpfile();
    if (!f) return 0;
    openpete_keymap_report(f);
    rewind(f);
    char line[512];
    int hit = 0;
    while (!hit && fgets(line, sizeof line, f)) hit = strstr(line, needle) != NULL;
    fclose(f);
    return hit;
}

static void test_capture_commit_on_release(void) {
    editor_reset();
    CHECK(begin(OPK_SCREENSHOT, 0, 0) == 0, "capture begins");
    CHECK(KM_CAPWORD_STATE(openpete_keymap_capture_word()) == KM_CAP_ARMED, "armed");
    CHECK(openpete_keymap_capture_active(), "active while capturing");
    g_keys[K("Q")] = 1; openpete_keymap_poll();
    CHECK(KM_CAPWORD_STATE(openpete_keymap_capture_word()) == KM_CAP_CANDIDATE,
          "the press is the candidate");
    CHECK(take() == KM_EDIT_NONE, "no commit while the candidate is held");
    CHECK(!openpete_key_down(OPK_SCREENSHOT), "nothing reads held during a capture");
    g_keys[K("Q")] = 0; openpete_keymap_poll();
    CHECK(take() == KM_EDIT_COMMITTED, "the release commits");
    CHECK(binding_is(OPK_SCREENSHOT, "Q"), "screenshot = Q");
    CHECK(!openpete_key_pressed(OPK_SCREENSHOT), "no press edge on commit");
    CHECK(openpete_keymap_has_saved(OPK_SCREENSHOT), "the edit is a saved binding");
    CHECK(!openpete_keymap_capture_active(), "inactive once nothing is latched");
}

static void test_capture_wheel_and_ignored_motion(void) {
    editor_reset();
    begin(OPK_PAUSE, -1, 0);
    /* Motion and a stick throw on a digital action are ignored. */
    g_mouse_motion[0] += 500;
    g_axes[PA("leftx")] = 30000;
    openpete_keymap_poll();
    CHECK(KM_CAPWORD_STATE(openpete_keymap_capture_word()) == KM_CAP_ARMED,
          "motion and a stick throw leave the capture armed");
    CHECK(take() == KM_EDIT_NONE, "nothing committed");
    g_axes[PA("leftx")] = 0;
    openpete_keymap_poll();
    g_mouse_wheel += OPENPETE_MOUSE_WHEEL_UNIT;
    openpete_keymap_poll();
    CHECK(take() == KM_EDIT_COMMITTED, "a wheel notch commits on its pulse");
    CHECK(binding_is(OPK_PAUSE, "F7,mouse:wheel_up"), "pause gains wheel_up");
}

static void test_capture_latch(void) {
    editor_reset();
    begin(OPK_SCREENSHOT, 0, 0);
    g_keys[K("Q")] = 1; openpete_keymap_poll();
    g_keys[K("F7")] = 1; g_btns[PB("west")] = 1; openpete_keymap_poll();
    g_keys[K("Q")] = 0; openpete_keymap_poll();
    CHECK(take() == KM_EDIT_COMMITTED, "commit with other inputs held");
    CHECK(openpete_keymap_capture_active(), "active while latched inputs are held");
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_PAUSE), "F7 held through the commit reads consumed");
    CHECK(!openpete_keymap_action_down_via(OPK_PAD_SQUARE, 0, NULL), "west reads consumed");
    g_keys[K("F7")] = 0; g_btns[PB("west")] = 0; openpete_keymap_poll();
    CHECK(!openpete_keymap_capture_active(), "inactive once released");
    g_keys[K("F7")] = 1; openpete_keymap_poll();
    CHECK(openpete_key_pressed(OPK_PAUSE), "a fresh press is live");

    /* Nine inputs held at commit all read consumed. */
    editor_reset();
    begin(OPK_SCREENSHOT, 0, 0);
    g_keys[K("Q")] = 1; openpete_keymap_poll();
    const char* held[] = { "1", "2", "3", "F7", "F8", "Tab", "T", "I", "L" };
    for (int i = 0; i < 9; i++) g_keys[K(held[i])] = 1;
    openpete_keymap_poll();
    g_keys[K("Q")] = 0; openpete_keymap_poll();
    CHECK(take() == KM_EDIT_COMMITTED, "commit with nine held");
    openpete_keymap_poll();
    int live = 0;
    for (int i = 0; i < 9; i++) {
        km_source_t s = { KM_SRC_KEY, 0, (short)K(held[i]), 0, 0 };
        live += openpete_keymap_source_down(&s);
    }
    CHECK(live == 0, "nine held keys read consumed (%d live)", live);
    CHECK(openpete_keymap_capture_active(), "still active");
    for (int i = 0; i < 9; i++) g_keys[K(held[i])] = 0;
    openpete_keymap_poll();
    CHECK(!openpete_keymap_capture_active(), "inactive after all nine release");
}

static void test_capture_cancels(void) {
    /* Escape: the event handler's cancel. Escape held at a poll before the
     * cancel is drained is no candidate and reads consumed. */
    editor_reset();
    begin(OPK_PAD_CROSS, 1, 0);
    g_keys[K("Escape")] = 1; openpete_keymap_poll();
    CHECK(KM_CAPWORD_STATE(openpete_keymap_capture_word()) == KM_CAP_ARMED,
          "Escape is not a candidate");
    openpete_keymap_capture_cancel("escape");
    CHECK(take() == KM_EDIT_CANCELLED && !strcmp(g_edit.reason, "escape"), "escape cancels");
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_MENU), "Escape reads consumed after the cancel");
    g_keys[K("Escape")] = 0; openpete_keymap_poll();
    CHECK(binding_is(OPK_PAD_CROSS, "C,pad:south"), "cross unchanged");

    /* Timeout, and a stick throw on a digital action does not stop it. */
    editor_reset();
    begin(OPK_PAD_CROSS, 1, 0);
    g_axes[PA("leftx")] = 30000;
    g_now_ms += KM_CAPTURE_TIMEOUT_MS + 1;
    openpete_keymap_poll();
    CHECK(take() == KM_EDIT_CANCELLED && !strcmp(g_edit.reason, "timeout"),
          "the timeout cancels with a stick thrown");
    openpete_keymap_poll();
    CHECK(openpete_keymap_capture_active(), "the stick held past the cancel stays latched");
    CHECK(openpete_keymap_action_axis_raw_via(OPK_FLY_AXIS_X, 0, NULL) == 0,
          "the stick reads consumed");
    g_axes[PA("leftx")] = 10000;   /* under KM_AXIS_PRESS */
    openpete_keymap_poll();
    CHECK(!openpete_keymap_capture_active(), "released under KM_AXIS_PRESS");

    /* A held candidate stops the timeout. */
    editor_reset();
    begin(OPK_PAD_CROSS, 1, 0);
    g_btns[PB("east")] = 1; openpete_keymap_poll();
    g_now_ms += KM_CAPTURE_TIMEOUT_MS + 1;
    openpete_keymap_poll();
    CHECK(take() == KM_EDIT_NONE, "no timeout while the candidate is held");
    g_btns[PB("east")] = 0; openpete_keymap_poll();
    CHECK(take() == KM_EDIT_COMMITTED, "its release commits");

    /* A click outside the prompt cancels; inside it is a candidate. */
    editor_reset();
    openpete_keymap_capture_set_rect(100, 100, 200, 200);
    begin(OPK_PAUSE, -1, 0);
    g_mouse_x = 10; g_mouse_y = 10;
    g_mouse_mask = 1u << KM_MOUSE_LEFT; openpete_keymap_poll();
    CHECK(take() == KM_EDIT_CANCELLED, "a click outside cancels");
    g_mouse_mask = 0; openpete_keymap_poll();
    begin(OPK_PAUSE, -1, 0);
    g_mouse_x = 150; g_mouse_y = 150;
    g_mouse_mask = 1u << KM_MOUSE_MIDDLE; openpete_keymap_poll();
    g_mouse_mask = 0; openpete_keymap_poll();
    CHECK(take() == KM_EDIT_COMMITTED, "a click inside is a candidate");
    CHECK(binding_is(OPK_PAUSE, "F7,mouse:middle"), "pause gains mouse:middle");
    openpete_keymap_capture_set_rect(0, 0, 0, 0);

    /* A pad button held through an Escape cancel stays consumed. */
    editor_reset();
    begin(OPK_PAD_CROSS, 1, 0);
    g_btns[PB("east")] = 1; openpete_keymap_poll();
    openpete_keymap_capture_cancel("escape");
    CHECK(take() == KM_EDIT_CANCELLED, "cancel with the candidate held");
    openpete_keymap_poll();
    CHECK(openpete_keymap_capture_active(), "active until east is released");
    g_btns[PB("east")] = 0; openpete_keymap_poll();
    CHECK(!openpete_keymap_capture_active(), "released");
}

static void test_capture_refusals(void) {
    editor_reset();
    CHECK(openpete_keymap_capture_begin(OPK_PAD_CROSS, -1, 1, &g_edit) != 0,
          "hold on a pad action refused at begin");
    begin(OPK_PAD_CROSS, -1, 0);
    CHECK(tap_key("Q") == KM_EDIT_REFUSED, "a second key on a pad action refused: %s",
          g_edit.reason);
    CHECK(binding_is(OPK_PAD_CROSS, "C,pad:south"), "cross unchanged");

    CHECK(openpete_keymap_set_live(OPK_PAUSE, NULL, 0) == 0, "clear pause");
    for (int i = 0; i < KM_MAX_SOURCES; i++) {
        begin(OPK_PAUSE, -1, 0);
        const char* k[] = { "1", "2", "3", "T" };
        (void)tap_key(k[i]);
    }
    CHECK(openpete_keymap_capture_begin(OPK_PAUSE, -1, 0, &g_edit) != 0,
          "a fifth source refused");

    editor_reset();
    begin(OPK_PAD_STICK_UP, -1, 0);
    CHECK(tap_btn("south") == KM_EDIT_REFUSED, "a pad source on stick_up refused");
    begin(OPK_PAD_LEFT_X, 0, 0);
    CHECK(tap_btn("south") == KM_EDIT_REFUSED, "a button on left_x refused");
    begin(OPK_SCREENSHOT, 0, 0);
    CHECK(tap_key(",") == KM_EDIT_REFUSED, "',' does not round-trip: %s", g_edit.reason);
    begin(OPK_SCREENSHOT, 0, 0);
    CHECK(tap_key("\\") == KM_EDIT_COMMITTED, "'\\' round-trips");
    CHECK(binding_is(OPK_SCREENSHOT, "\\"), "screenshot = backslash");
    begin(OPK_TURBO, -1, 0);
    g_mouse_wheel += OPENPETE_MOUSE_WHEEL_UNIT; openpete_keymap_poll();
    CHECK(take() == KM_EDIT_REFUSED, "a wheel notch on a level action refused");
}

static void test_capture_swaps(void) {
    /* Two ALWAYS hotkeys swap keys. */
    editor_reset();
    begin(OPK_SCREENSHOT, 0, 0);
    CHECK(tap_key("F7") == KM_EDIT_COMMITTED, "F7 onto screenshot");
    CHECK(binding_is(OPK_SCREENSHOT, "F7") && binding_is(OPK_PAUSE, "F12"),
          "screenshot and pause swap");
    CHECK(g_edit.n_changed == 2 && g_edit.changed[1].action == OPK_PAUSE &&
          !strcmp(g_edit.changed[1].old_tok, "F7") &&
          !strcmp(g_edit.changed[1].new_tok, "F12"), "the swap names pause F7 -> F12");

    /* WASD shared across GAMEPLAY and FREECAM is no conflict. */
    editor_reset();
    km_source_t w = { KM_SRC_KEY, 0, (short)K("W"), 0, 0 };
    CHECK(openpete_keymap_conflicts(OPK_FLY_FORWARD, &w, NULL, 0) == 0,
          "W on fly_forward does not conflict with stick_up");

    /* pad:east onto cross's pad slot conflicts with circle only. */
    editor_reset();
    km_source_t east = { KM_SRC_PADBTN, 0, (short)PB("east"), 0, 0 };
    openpete_action_t out[8];
    CHECK(openpete_keymap_conflicts(OPK_PAD_CROSS, &east, out, 8) == 1 &&
          out[0] == OPK_PAD_CIRCLE, "pad:east on cross conflicts with circle only");
    begin(OPK_PAD_CROSS, 1, 0);
    CHECK(tap_btn("east") == KM_EDIT_COMMITTED, "east onto cross");
    CHECK(binding_is(OPK_PAD_CROSS, "C,pad:east") && binding_is(OPK_PAD_CIRCLE, "V,pad:south"),
          "cross pad:east, circle pad:south");
    CHECK(g_edit.pad_changed, "a pad action changed");

    /* V onto cross's key slot. */
    editor_reset();
    begin(OPK_PAD_CROSS, 0, 0);
    CHECK(tap_key("V") == KM_EDIT_COMMITTED, "V onto cross");
    CHECK(binding_is(OPK_PAD_CROSS, "V,pad:south") && binding_is(OPK_PAD_CIRCLE, "C,pad:east"),
          "cross V, circle C");
}

static void test_capture_v22(void) {
    /* A plain controller button onto an in-play hotkey is refused. */
    editor_reset();
    begin(OPK_SCREENSHOT, -1, 0);
    CHECK(tap_btn("south") == KM_EDIT_REFUSED, "plain pad:south onto screenshot refused");
    CHECK(strstr(g_edit.reason, "only as a hold") != NULL, "the V22 reason: %s", g_edit.reason);
    CHECK(binding_is(OPK_PAD_CROSS, "C,pad:south"), "cross unchanged");

    /* The hold form is admitted and cross keeps the button. */
    editor_reset();
    begin(OPK_SCREENSHOT, -1, 1);
    CHECK(tap_btn("south") == KM_EDIT_COMMITTED, "hold:pad:south onto screenshot: %s",
          g_edit.reason);
    CHECK(binding_is(OPK_SCREENSHOT, "F12,hold:pad:south"), "screenshot gains the hold");
    CHECK(binding_is(OPK_PAD_CROSS, "C,pad:south"), "cross keeps pad:south");
    CHECK(openpete_keymap_resolve() == 0, "resolve warns about no pair");

    /* Keys follow the swap, plain or held. */
    editor_reset();
    begin(OPK_SCREENSHOT, 0, 0);
    CHECK(tap_key("C") == KM_EDIT_COMMITTED, "C onto screenshot");
    CHECK(binding_is(OPK_PAD_CROSS, "F12,pad:south"), "cross takes F12");
    CHECK(g_edit.n_changed == 2 && g_edit.changed[1].action == OPK_PAD_CROSS,
          "the swap names cross (its toast)");
    editor_reset();
    begin(OPK_SCREENSHOT, -1, 1);
    CHECK(tap_key("C") == KM_EDIT_COMMITTED, "hold:C onto screenshot");
    CHECK(binding_is(OPK_PAD_CROSS, "pad:south"), "cross loses C on an add");

    /* pad:start onto cross: menu's hold is no conflict. */
    editor_reset();
    km_source_t start = { KM_SRC_PADBTN, 0, (short)PB("start"), 0, 0 };
    openpete_action_t out[8];
    const int n = openpete_keymap_conflicts(OPK_PAD_CROSS, &start, out, 8);
    int has_menu = 0;
    for (int i = 0; i < n && i < 8; i++) has_menu |= out[i] == OPK_MENU;
    CHECK(!has_menu, "pad:start on cross does not conflict with menu");

    /* plain pad:start onto menu is refused. */
    editor_reset();
    begin(OPK_MENU, -1, 0);
    CHECK(tap_btn("start") == KM_EDIT_REFUSED, "plain pad:start onto menu refused");
    CHECK(binding_is(OPK_MENU, "Escape,hold:pad:start"), "menu unchanged");

    /* hold:pad:back onto screenshot strips devui's only pad source. */
    editor_reset();
    begin(OPK_SCREENSHOT, -1, 1);
    CHECK(tap_btn("back") == KM_EDIT_REFUSED, "hold:pad:back onto screenshot refused");
    CHECK(strstr(g_edit.reason, "devui") != NULL, "names devui: %s", g_edit.reason);
    /* hold:pad:start onto screenshot strips menu's. */
    begin(OPK_SCREENSHOT, -1, 1);
    CHECK(tap_btn("start") == KM_EDIT_REFUSED, "hold:pad:start onto screenshot refused");
    CHECK(strstr(g_edit.reason, "menu") != NULL, "names menu: %s", g_edit.reason);

    /* Remove refused on menu's last pad source. */
    editor_reset();
    CHECK(openpete_keymap_remove_source(OPK_MENU, 1, &g_edit) != 0,
          "removing menu's last pad source refused");
    CHECK(openpete_keymap_remove_source(OPK_MENU, 0, &g_edit) == 0, "removing Escape allowed");
}

static void test_resolve_hold_rule(void) {
    editor_reset();
    CHECK(openpete_keymap_resolve() == 0, "a stock resolve logs no warning");
    CHECK(report_has("host.menu + pad.start - intentional: a tap reaches the game"),
          "menu/pad.start collected with the hold reason");
    CHECK(report_has("host.devui + pad.select - intentional: a tap reaches the game"),
          "devui/pad.select collected with the hold reason");

    /* A plain pad:back on a host action is a real pair. */
    editor_reset();
    CHECK(set1("host", "screenshot", "pad:back", 100, "global") == 0, "screenshot pad:back");
    CHECK(openpete_keymap_resolve() > 0, "devui hold:pad:back + screenshot pad:back warns");
    CHECK(report_has("host.devui + host.screenshot  <-- unintended"), "reported as a pair");
}

static void test_layers_and_restore(void) {
    editor_reset();
    CHECK(set1("host", "pause", "P", 1000, "global") == 0, "global P");
    CHECK(set1("host", "pause", "O", 3000, "game.spyro-1") == 0, "game O");
    CHECK(set1("host", "pause", "U", 5000, "saved") == 0, "saved U");
    CHECK(binding_is(OPK_PAUSE, "U") && openpete_keymap_has_saved(OPK_PAUSE), "saved wins");
    km_source_t g[KM_MAX_SOURCES];
    int scoped = 0;
    CHECK(openpete_keymap_global_layer(OPK_PAUSE, g, KM_MAX_SOURCES, &scoped) == 1 &&
          g[0].code == K("P") && scoped, "global layer P, game-scoped");
    char origin[48];
    CHECK(openpete_keymap_below_saved(OPK_PAUSE, g, KM_MAX_SOURCES, origin, sizeof origin) == 1 &&
          g[0].code == K("O") && !strcmp(origin, "game.spyro-1"), "below saved: game O");
    CHECK(openpete_keymap_restore(OPK_PAUSE, &g_edit) == 0, "restore");
    CHECK(binding_is(OPK_PAUSE, "O") && !openpete_keymap_has_saved(OPK_PAUSE),
          "restore returns to the game layer and clears has-saved");
}

/* A hold of a controller button onto an in-play hotkey leaves the settings
 * menu's and the overlay's navigation sources alone, and with the menu's
 * pad suspension a held Cross in the menu fires no hold hotkey. */
static void test_hold_leaves_overlay_nav(void) {
    editor_reset();
    begin(OPK_SCREENSHOT, -1, 1);
    CHECK(tap_btn("south") == KM_EDIT_COMMITTED, "hold:pad:south onto screenshot");
    CHECK(g_edit.n_changed == 1, "no action swapped (%d changed)", g_edit.n_changed);
    CHECK(binding_is(OPK_MENU_SELECT, "Return,pad:south"), "menu_select untouched");
    CHECK(binding_is(OPK_MENU_RIGHT, "Right,pad:dpright,pad:leftx,pad:south"),
          "menu_right untouched");
    CHECK(binding_is(OPK_DEVUI_ACTIVATE, "pad:south"), "devui_activate untouched");
    CHECK(!openpete_keymap_has_saved(OPK_MENU_SELECT) &&
          !openpete_keymap_has_saved(OPK_DEVUI_ACTIVATE), "no saved navigation rows");
    CHECK(openpete_keymap_resolve() == 0, "no warning");
    CHECK(report_has("host.menu_select + host.screenshot - intentional: the overlay"),
          "the navigation pair carries its reason");

    static const struct { const char* btn; openpete_action_t nav; const char* want; } k[] = {
        { "east",          OPK_MENU_CLOSE,   "pad:start,pad:east" },
        { "east",          OPK_DEVUI_CANCEL, "pad:east" },
        { "west",          OPK_DEVUI_MENU,   "pad:west" },
        { "leftshoulder",  OPK_DEVUI_SLOWER, "pad:leftshoulder" },
        { "rightshoulder", OPK_DEVUI_FASTER, "pad:rightshoulder" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        editor_reset();
        begin(OPK_SCREENSHOT, -1, 1);
        CHECK(tap_btn(k[i].btn) == KM_EDIT_COMMITTED, "hold:pad:%s onto screenshot: %s",
              k[i].btn, g_edit.reason);
        CHECK(binding_is(k[i].nav, k[i].want), "navigation keeps pad:%s", k[i].btn);
    }

    /* Menu open: the run loop suspends live pad sources outside the menu. */
    editor_reset();
    begin(OPK_PAUSE, -1, 1);
    CHECK(tap_btn("south") == KM_EDIT_COMMITTED, "hold:pad:south onto pause");
    CHECK(set1("host", "screenshot", "hold:pad:south", 6000, "saved") == 0,
          "screenshot holds south too (two hotkeys on one hold report a pair)");
    int n = 0;
    const openpete_action_t* ex = openpete_keymap_menu_pad_exempt(&n);
    openpete_keymap_suspend_pad(1, ex, n);
    g_btns[PB("south")] = 1;
    int fired = 0, nav = 0;
    for (int i = 0; i < 20; i++) {
        g_now_ms += 100;
        openpete_keymap_poll();
        fired += openpete_key_pressed(OPK_PAUSE) + openpete_key_pressed(OPK_SCREENSHOT);
        nav += openpete_key_down(OPK_MENU_SELECT);
    }
    CHECK(fired == 0, "a held Cross in the menu fires no hold hotkey (%d)", fired);
    CHECK(nav > 0, "menu_select reads the held Cross");
    g_btns[PB("south")] = 0;
    openpete_keymap_suspend_pad(0, NULL, 0);
    openpete_keymap_poll();
    g_btns[PB("south")] = 1;
    for (int i = 0; i < 7; i++) { g_now_ms += 100; openpete_keymap_poll();
        fired += openpete_key_pressed(OPK_PAUSE) + openpete_key_pressed(OPK_SCREENSHOT); }
    CHECK(fired == 2, "outside the menu the hold fires pause and screenshot once (%d)", fired);
    g_btns[PB("south")] = 0;
    openpete_keymap_poll();
}

int main(void) {
    install_fakes();

    test_defaults_parse();
    test_dev_unbound_by_default();
    test_pad2_unbound();
    test_pad_device_qualifier();
    test_pad_reader_default_slot();
    test_pad_explicit_qualifier_wins();
    test_pad_reader_axis();
    test_pad_qualified_without_reader();
    test_unbind();
    test_replace_not_append();
    test_per_action_merge();
    test_rank_order();
    test_parse_errors();
    test_scalar_and_array_equivalent();
    test_stock_resolve_is_clean();
    test_conflict_detection();
    test_required_warns_only_when_fully_unbound();
    test_edge_latch();
    test_suspend_is_keyboard_only();
    test_suspend_pad_exempt_set();
    test_hold_source();
    test_devui_ctx_disjoint();
    test_axis();
    test_axis_as_digital();
    test_quit_binding();
    test_overlay_toggles_bound_by_default();
    test_consume_key();
    test_mod_bindings();
    test_mouse_parse();
    test_mouse_resolve();
    test_mouse_live();
    test_mouse_owned();
    test_mouse_reader_and_format();
    test_mod_defaults();
    test_capture_commit_on_release();
    test_capture_wheel_and_ignored_motion();
    test_capture_latch();
    test_capture_cancels();
    test_capture_refusals();
    test_capture_swaps();
    test_capture_v22();
    test_resolve_hold_rule();
    test_layers_and_restore();
    test_hold_leaves_overlay_nav();

    if (g_fail) {
        printf("keymap_test: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("keymap_test: all checks passed\n");
    return 0;
}
