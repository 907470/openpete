/* tests/unit/keymap_test.c — the keymap registry, parser and resolver.
 *
 * SDL-free: keymap.c reaches SDL only through the lookup/input seams, so
 * this test installs a tiny fake name table and a fake input state. That
 * keeps the console-test link line free of SDL (see the note at the top
 * of tests/unit/CMakeLists.txt) and, more usefully, lets the test drive
 * held/released input directly to exercise the edge latch. */

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

    { KM_SRC_PADBTN, "south", 100 }, { KM_SRC_PADBTN, "east", 101 },
    { KM_SRC_PADBTN, "west", 102 },  { KM_SRC_PADBTN, "north", 103 },
    { KM_SRC_PADBTN, "start", 104 }, { KM_SRC_PADBTN, "back", 105 },
    { KM_SRC_PADBTN, "leftshoulder", 106 },
    { KM_SRC_PADBTN, "rightshoulder", 107 },
    { KM_SRC_PADBTN, "leftstick", 108 },
    { KM_SRC_PADBTN, "rightstick", 109 },
    { KM_SRC_PADBTN, "dpup", 110 },   { KM_SRC_PADBTN, "dpdown", 111 },
    { KM_SRC_PADBTN, "dpleft", 112 }, { KM_SRC_PADBTN, "dpright", 113 },

    { KM_SRC_PADAXIS, "leftx", 200 },  { KM_SRC_PADAXIS, "lefty", 201 },
    { KM_SRC_PADAXIS, "rightx", 202 }, { KM_SRC_PADAXIS, "righty", 203 },
    { KM_SRC_PADAXIS, "lefttrigger", 204 },
    { KM_SRC_PADAXIS, "righttrigger", 205 },
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

static void fake_reset(void) {
    memset(g_keys, 0, sizeof g_keys);
    memset(g_btns, 0, sizeof g_btns);
    memset(g_axes, 0, sizeof g_axes);
}

static void install_fakes(void) {
    static const openpete_keymap_input_fns fns = { fake_key, fake_btn, fake_axis };
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

    CHECK(has_source(OPK_MENU, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "O")),
          "menu should default to O");
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
    CHECK(n_sources(OPK_DEVUI) == 0, "devui must be unbound by default");
    CHECK(n_sources(OPK_INTERP_TOGGLE) == 0, "interp_toggle must be unbound");
    CHECK(n_sources(OPK_LOD_TOGGLE) == 0, "lod_toggle must be unbound");
    CHECK(n_sources(OPK_CAMLOCK) == 0, "camlock must be unbound");
    CHECK(n_sources(OPK_FILLPICK_FILL) == 0, "fillpick_fill must be unbound");

    /* ...and a config can bind one back. */
    CHECK(set1("host", "fillpick", "P", 100, "global") == 0, "bind fillpick");
    CHECK(has_source(OPK_FILLPICK, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "P")),
          "fillpick should be bindable from config");
}

/* Player 2 is a valid namespace but defaults to unbound — giving it P1's
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

/* Within an action a scope REPLACES rather than appends — otherwise
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
    CHECK(has_source(OPK_MENU, KM_SRC_KEY, fake_to_code(KM_SRC_KEY, "O")),
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

/* KM_REQUIRED warns only when EVERY source is gone — a pad-only player
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
    int o = fake_to_code(KM_SRC_KEY, "O");

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

    /* An unbound action can never produce an edge — no stuck latch. */
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
    g_keys[fake_to_code(KM_SRC_KEY, "F")]           = 1;
    g_btns[fake_to_code(KM_SRC_PADBTN, "leftstick")] = 1;

    openpete_keymap_suspend_keyboard(1);
    openpete_keymap_poll();
    CHECK(openpete_key_down(OPK_FREECAM),
          "a pad source must survive a keyboard suspend");

    g_btns[fake_to_code(KM_SRC_PADBTN, "leftstick")] = 0;
    openpete_keymap_poll();
    CHECK(!openpete_key_down(OPK_FREECAM),
          "the keyboard source must be inert while suspended");

    openpete_keymap_suspend_keyboard(0);
    openpete_keymap_poll();
    CHECK(openpete_key_down(OPK_FREECAM), "and live again after resume");
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

/* Quit is a binding like any other, so it can be turned off. The event
 * filter asks the keymap by scancode (it sees one key event at a time and
 * cannot use the polled latch), so that query is what these assert. */
static void test_quit_binding(void) {
    int esc = fake_to_code(KM_SRC_KEY, "Escape");
    int f10 = fake_to_code(KM_SRC_KEY, "F10");

    openpete_keymap_init();
    CHECK(openpete_keymap_key_binds(OPK_QUIT, esc),
          "quit must default to Escape");

    CHECK(set1("host", "quit", "none", 100, "global") == 0, "unbind quit");
    CHECK(!openpete_keymap_key_binds(OPK_QUIT, esc),
          "quit = \"none\" must stop Escape quitting");
    /* Unbinding quit must NOT warn — it is a supported choice, and the
     * window close button still quits. */
    CHECK(openpete_keymap_resolve() == 0,
          "unbinding quit must not warn (it is not KM_REQUIRED)");

    openpete_keymap_init();
    CHECK(set1("host", "quit", "F10", 100, "global") == 0, "rebind quit");
    CHECK(openpete_keymap_key_binds(OPK_QUIT, f10), "quit should now be F10");
    CHECK(!openpete_keymap_key_binds(OPK_QUIT, esc),
          "rebinding quit must release Escape");

    /* The query is by scancode and must not match other actions' keys. */
    CHECK(!openpete_keymap_key_binds(OPK_MENU, esc),
          "menu is not bound to Escape");
}

/* The event filter and the polled bindings are two paths onto one
 * physical press: the filter swallows the EVENT, but the poll reads
 * SDL_GetKeyboardState and still sees the key held. With `menu` bound to
 * the key the filter dismisses on, that closed the menu and reopened it
 * in the same press. Consuming the key is the fix. */
static void test_consume_key(void) {
    openpete_keymap_init();
    fake_reset();
    /* Reproduce the reported config: menu on the dismiss key. */
    CHECK(set1("host", "menu", "Escape", 100, "global") == 0, "menu -> Escape");
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
     * open. The consume is what has to stop it — and note this only
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
    test_axis();
    test_axis_as_digital();
    test_quit_binding();
    test_consume_key();
    test_mod_bindings();

    if (g_fail) {
        printf("keymap_test: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("keymap_test: all checks passed\n");
    return 0;
}
