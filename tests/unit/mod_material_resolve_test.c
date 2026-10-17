/* mod_material_resolve_test - the material resolver (core/mods/mod_material_resolve.c)
 * against stub loader hooks.
 *
 * Each case registers materials for stub mods (id, enabled-list position,
 * manifest api pin), drives the extraction, bake and draw-time entry
 * points, and asserts the outcome the resolver produces. The cases record
 * the current rules: one material per draw, a refine decline drawn stock,
 * a same-mod tie kept by the first registration, a terrain refine ranked
 * above every declarative, the extraction scan without the level,
 * contract and identity tests, and the level read live at draw time.
 * Every stub mod carries its api pin, so a rule keyed on the pin changes
 * only the cases whose mods sit on the changed side of it.
 *
 * Exit 0 when every check holds, 1 otherwise. */
#include "mod_material_resolve.h"
#include "openpete_native_render.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Loader and renderer stubs. */

OPENPETE_THREAD_LOCAL int openpete_mods_in_present;

/** @brief One stub mod: id, enabled-list position, manifest api pin, live toggle. */
typedef struct {
    const char* id;
    int load_index;
    int api;
    int enabled;
} stub_mod_t;

static stub_mod_t s_mods[8];
static int s_mod_n;
static int s_guard_depth, s_guard_max;
static const char* s_guard_owner;
static int s_split;

/** @brief Stub: a mod's live-toggle state; unknown ids are disabled. */
int openpete_mods_is_enabled(const char* id) {
    for (int i = 0; i < s_mod_n; i++)
        if (strcmp(s_mods[i].id, id) == 0) return s_mods[i].enabled;
    return 0;
}
/** @brief Stub: enter a guarded stretch owned by a mod, recording the owner and the nesting depth. */
void openpete_mods_guard_enter(const char* mod_id) {
    s_guard_owner = mod_id;
    if (++s_guard_depth > s_guard_max) s_guard_max = s_guard_depth;
}
/** @brief Stub: leave the guarded stretch. */
void openpete_mods_guard_leave(void) { s_guard_depth--; }
/** @brief Stub: the mod toggle epoch, constant here. */
unsigned openpete_mods_toggle_epoch(void) { return 0; }
/** @brief Stub: record the batcher tag-split arm state. */
void openpete_native_set_tag_split(int on) { s_split = on; }

/* ------------------------------------------------------------------ */
/* Harness. */

static int s_fail, s_checks;
#define CHECK(cond, ...) do { s_checks++; if (!(cond)) { s_fail = 1; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

/** @brief Start a case: empty the table, the stub mods and the census, and print its name. */
static void begin(const char* name) {
    mod_mat_reset();
    mod_mat_census_reset();
    s_mod_n = 0;
    s_guard_depth = s_guard_max = 0;
    openpete_mods_in_present = 0;
    printf("case %s\n", name);
}
/** @brief Print one recorded outcome line of the current case. */
#define OUTCOME(...) do { printf("  "); printf(__VA_ARGS__); printf("\n"); } while (0)

/** @brief Add an enabled stub mod.
 *
 * @param id Mod id (string literal).
 * @param load Enabled-list position, later higher.
 * @param api Manifest api pin.
 * @return The stub. */
static stub_mod_t* mod(const char* id, int load, int api) {
    stub_mod_t* m = &s_mods[s_mod_n++];
    m->id = id; m->load_index = load; m->api = api; m->enabled = 1;
    return m;
}

/** @brief Register a material for a stub mod through the same validation and append the engine uses.
 *
 * @param m Owner.
 * @param sel Selector.
 * @param contract OPENPETE_MATERIAL_CONTRACT_*.
 * @param fn Refine callback, or NULL for a declarative material.
 * @return The handle, or -1 when refused. */
static int reg(const stub_mod_t* m, openpete_mod_material_selector_t sel, int contract,
               openpete_mod_material_fn fn) {
    if (!mod_mat_sel_valid(m->id, &sel)) return -1;
    return mod_mat_append(m->id, &sel, -1, 0, contract, fn, m->load_index, 1u);
}
/** @brief A selector on one channel, every other key open. */
static openpete_mod_material_selector_t sel_ch(int ch) {
    openpete_mod_material_selector_t s = openpete_mod_material_sel_any();
    s.channel = ch;
    return s;
}
/** @brief A moby selector keyed to one class (-1 = every class). */
static openpete_mod_material_selector_t sel_moby(int cls) {
    openpete_mod_material_selector_t s = sel_ch(OP_CHAN_MOBY);
    s.moby_class = cls;
    return s;
}

/** @brief A moby provenance tag: path nibble 1, class [15:0], slot [27:16]. */
static uint32_t moby_tag(int cls, int slot) {
    return (1u << 28) | ((uint32_t)slot << 16) | ((uint32_t)cls & 0xFFFFu);
}
/** @brief A terrain provenance tag: path nibble 2, env bit 27, LP bit 26, untextured bit 25, slot [24:16], Tiledef id [15:9].
 *
 * @param tex Tiledef id, or -1 for an untextured face. */
static uint32_t ter_tag(int tex, int env, int lp, int slot) {
    uint32_t t = (2u << 28) | ((uint32_t)slot << 16);
    if (env) t |= 0x08000000u;
    if (lp) t |= 0x04000000u;
    if (tex < 0) t |= 0x02000000u;
    else t |= ((uint32_t)tex & 0x7Fu) << 9;
    return t;
}
/** @brief A provenance tag of a non-moby, non-terrain channel: the channel nibble and the identity. */
static uint32_t chan_tag(int ch, int ident) { return ((uint32_t)ch << 28) | ((uint32_t)ident & 0xFFFFu); }

/** @brief Resolve a tag for a contract and return the material index, or -1 for stock. */
static int resolve(uint32_t tag, int contract) {
    int slot = 0;
    return mod_mat_resolve_index(tag, contract, &slot);
}
#define SCENE OPENPETE_MATERIAL_CONTRACT_SCENE
#define DECAL OPENPETE_MATERIAL_CONTRACT_DECAL

/* Refine callbacks. Each counts its calls; a claim writes a 16-byte block
 * derived from the key, so distinct instances intern distinct slots. */
static int s_calls;
/** @brief Refine: claim every instance with a block from its address or texture. */
static int rf_accept(const openpete_mod_material_key_t* k, void* out, uint32_t cap) {
    s_calls++;
    if (cap < 16) return -1;
    memset(out, 0, 16);
    if (k->channel == OP_CHAN_MOBY) memcpy(out, &k->u.moby.vaddr, sizeof k->u.moby.vaddr);
    else memcpy(out, &k->u.terrain.texture, sizeof k->u.terrain.texture);
    return 16;
}
/** @brief Refine: decline every instance. */
static int rf_decline(const openpete_mod_material_key_t* k, void* out, uint32_t cap) {
    (void)k; (void)out; (void)cap;
    s_calls++;
    return -1;
}
/** @brief Refine: claim only class 7 mobys (the channel-wide refine that accepts its own instances). */
static int rf_class7(const openpete_mod_material_key_t* k, void* out, uint32_t cap) {
    s_calls++;
    if (k->channel != OP_CHAN_MOBY || k->u.moby.moby_class != 7 || cap < 16) return -1;
    memset(out, 7, 16);
    return 16;
}

/* ------------------------------------------------------------------ */
/* Cases. Each OUTCOME line is the recorded current result. */

/** @brief A later refine's decline draws stock; the earlier declarative on the class is not consulted. */
static void case_f1(void) {
    begin("Refine decline draws stock over an earlier declarative");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mb = reg(b, sel_moby(5), SCENE, rf_decline);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset();
    s_calls = 0;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == OPENPETE_MATERIAL_SLOT_STOCK, "slot %#x", s);
    CHECK(s_calls == 1, "calls %d", s_calls);
    CHECK(resolve(moby_tag(5, s), SCENE) == -1, "declined instance resolves");
    CHECK(mod_mat_census_get(mb, MOD_MAT_CENSUS_DECLINES) == 1, "census declines");
    CHECK(mod_mat_census_get(mb, MOD_MAT_CENSUS_DECLINE_SHADOWS) == 1, "census decline shadows");
    CHECK(mod_mat_census_get(mb, MOD_MAT_CENSUS_FALLTHROUGHS) == 0, "census fallthroughs");
    CHECK(s_guard_max == 1 && s_guard_depth == 0 && s_guard_owner && strcmp(s_guard_owner, "fx-b") == 0,
          "guard balance");
    mod_mat_census_force(0);
    OUTCOME("class 5, B declines: slot STOCK, resolve stock (A=%d not consulted), refine calls 1", ma);

    begin("Refine accept wins over an earlier declarative");
    a = mod("fx-a", 0, 13);
    b = mod("fx-b", 1, 13);
    reg(a, sel_moby(5), SCENE, NULL);
    const int mb2 = reg(b, sel_moby(5), SCENE, rf_accept);
    openpete_mods_material_frame_reset();
    const int s2 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s2 == 1, "slot %d", s2);
    CHECK(resolve(moby_tag(5, s2), SCENE) == mb2, "accepted instance");
    OUTCOME("class 5, B accepts: slot 1, resolve B");
}

/** @brief A later level-keyed declarative wins extraction, fails the draw-time level test, and the refine is never asked. */
static void case_f1a_level(void) {
    begin("Level-keyed declarative over a refine");
    stub_mod_t* a = mod("fx-r", 0, 13);
    stub_mod_t* b = mod("fx-d", 1, 13);
    reg(a, sel_moby(5), SCENE, rf_accept);
    openpete_mod_material_selector_t d = sel_moby(5);
    d.level = 9;
    reg(b, d, SCENE, NULL);
    openpete_mods_set_level(3);
    openpete_mods_material_frame_reset();
    s_calls = 0;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == 0, "slot %d", s);
    CHECK(s_calls == 0, "calls %d", s_calls);
    CHECK(resolve(moby_tag(5, s), SCENE) == -1, "resolve");
    OUTCOME("level 3, D keyed to 9: extraction slot 0 (D won), refine calls 0, resolve stock");
}

/** @brief A DECAL-contract refine on channel -1 wins moby extraction and the SCENE draw goes stock. */
static void case_f1a_decal(void) {
    begin("DECAL-contract refine on channel -1");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-decal", 1, 13);
    reg(a, sel_moby(-1), SCENE, NULL);
    const int mb = reg(b, openpete_mod_material_sel_any(), DECAL, rf_accept);
    CHECK(mb >= 0, "registered");
    openpete_mods_material_frame_reset();
    s_calls = 0;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == 1 && s_calls == 1, "slot %d calls %d", s, s_calls);
    CHECK(resolve(moby_tag(5, s), SCENE) == -1, "scene resolve");
    OUTCOME("DECAL refine wins extraction (slot 1, 1 call); SCENE draw resolves stock, A not consulted");
}

/** @brief A later channel-wide refine accepting only class 7 shadows an earlier class-5 declarative. */
static void case_f2(void) {
    begin("Channel-wide refine shadows an earlier class declarative");
    stub_mod_t* a = mod("fx-a", 0, 12);
    stub_mod_t* b = mod("fx-wild", 1, 11);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mb = reg(b, sel_moby(-1), SCENE, rf_class7);
    openpete_mods_material_frame_reset();
    const int s5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    const int s7 = openpete_mods_material_moby_slot(NULL, 7, 0x80010100u);
    CHECK(s5 == OPENPETE_MATERIAL_SLOT_STOCK, "class 5 slot %#x", s5);
    CHECK(resolve(moby_tag(5, s5), SCENE) == -1, "class 5 resolve");
    CHECK(resolve(moby_tag(7, s7), SCENE) == mb, "class 7 resolve");
    CHECK(resolve(moby_tag(5, 0), SCENE) == ma, "class 5 untagged");
    OUTCOME("class 5: STOCK (A shadowed); class 7: wild refine; A reached only by a tag that skipped extraction");
}

/** @brief A terrain refine outranks a later declarative on its texture, at bake and walked. */
static void case_f3(void) {
    begin("Terrain refine outranks a later declarative");
    stub_mod_t* a = mod("fx-tr", 0, 13);
    stub_mod_t* b = mod("fx-td", 1, 13);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 3;
    const int mr = reg(a, r, SCENE, rf_accept);
    const int md = reg(b, r, SCENE, NULL);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset();
    CHECK(openpete_mods_material_terrain_bucket(3, 0, 0) == mr + 1, "bake bucket");
    const int s = openpete_mods_material_terrain_tex_slot(NULL, 3);
    CHECK(s == 1, "walked slot %d", s);
    CHECK(resolve(ter_tag(3, 0, 0, s), SCENE) == mr, "walked resolve");
    CHECK(resolve(ter_tag(3, 0, 0, 0), SCENE) == md, "slotless tag");
    CHECK(mod_mat_census_get(mr, MOD_MAT_CENSUS_OUTRANKED) == 1, "census outranked");
    mod_mat_census_force(0);
    OUTCOME("texture 3: bake bucket = refine, walked = refine; census counts the refine as outranked");
}

/** @brief A same-mod tie keeps the first registration, manifest row against C and C against C. */
static void case_f4(void) {
    begin("Same-mod tie, manifest row then C registration (pin 13)");
    stub_mod_t* a = mod("fx-pair", 0, 13);
    const int mm = reg(a, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    reg(a, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    CHECK(resolve(chan_tag(OP_CHAN_PLAYER, 0), SCENE) == mm, "player");
    OUTCOME("player: the manifest row (first registration) wins");

    begin("Same-mod tie, C against C on flame ident 0 (pin 11)");
    a = mod("fx-gifts", 0, 11);
    openpete_mod_material_selector_t f = sel_ch(OP_CHAN_FLAME);
    f.ident = 0;
    const int m1 = reg(a, f, SCENE, NULL);
    reg(a, f, SCENE, NULL);
    CHECK(resolve(chan_tag(OP_CHAN_FLAME, 0), SCENE) == m1, "flame");
    OUTCOME("flame ident 0: the first registration wins");
}

/** @brief OP_SEL_HP_TIER on a moby selector: extraction rejects it, the draw-time scan accepts it. */
static void case_hp_tier_moby(void) {
    begin("OP_SEL_HP_TIER on a moby selector");
    stub_mod_t* a = mod("fx-hp", 0, 13);
    stub_mod_t* b = mod("fx-other", 1, 13);
    openpete_mod_material_selector_t s = sel_moby(-1);
    s.flags = OP_SEL_HP_TIER;
    const int ma = reg(a, s, SCENE, NULL);
    const int mb = reg(b, sel_moby(99), SCENE, rf_accept);
    openpete_mods_material_frame_reset();
    const int sl = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(sl == 0, "slot %d", sl);
    CHECK(mod_mat_census_get(ma, MOD_MAT_CENSUS_EXTRACT_WINS) == 0, "no extract win");
    CHECK(resolve(moby_tag(5, sl), SCENE) == ma, "draw-time");
    (void)mb;
    OUTCOME("extraction: no candidate (slot 0); draw-time resolve: the HP-tier material");
}

/** @brief Spyro64's OP_SEL_TEXTURED moby selector matches at extraction and at draw time. */
static void case_textured_moby(void) {
    begin("OP_SEL_TEXTURED moby selector (spyro64)");
    stub_mod_t* a = mod("fx-s64", 0, 12);
    stub_mod_t* b = mod("fx-other", 1, 13);
    openpete_mod_material_selector_t s = sel_moby(508);
    s.flags = OP_SEL_TEXTURED;
    const int ma = reg(a, s, SCENE, NULL);
    reg(b, sel_moby(99), SCENE, rf_accept);
    openpete_mods_material_frame_reset();
    const int sl = openpete_mods_material_moby_slot(NULL, 508, 0x80010000u);
    CHECK(sl == 0, "slot %d", sl);
    CHECK(resolve(moby_tag(508, sl), SCENE) == ma, "draw-time");
    OUTCOME("class 508: extraction picks it (declarative, slot 0); draw-time resolve: it");
}

/** @brief A level-keyed terrain refine draws on the walked path in every level and buckets only in its own. */
static void case_f3a(void) {
    begin("Level-keyed terrain refine");
    stub_mod_t* a = mod("fx-tlev", 0, 13);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 4;
    r.level = 9;
    const int mr = reg(a, r, SCENE, rf_accept);
    openpete_mods_set_level(3);
    openpete_mods_material_frame_reset();
    CHECK(openpete_mods_material_terrain_bucket(4, 0, 0) == 0, "bake");
    const int s = openpete_mods_material_terrain_tex_slot(NULL, 4);
    CHECK(s == 1, "walked slot %d", s);
    CHECK(resolve(ter_tag(4, 0, 0, s), SCENE) == mr, "walked resolve");
    openpete_mods_set_level(9);
    CHECK(openpete_mods_material_terrain_bucket(4, 0, 0) == mr + 1, "bake level 9");
    OUTCOME("level 3: bake stock, walked refine; level 9: both refine");
}

/** @brief Refines that cannot take effect register today and stay inert. */
static void case_d3a(void) {
    begin("Refines that cannot take effect");
    stub_mod_t* a = mod("fx-bad", 0, 13);
    s_calls = 0;
    const int mf = reg(a, sel_ch(OP_CHAN_FLAME), SCENE, rf_accept);
    const int md = reg(a, sel_ch(OP_CHAN_SHADOW), DECAL, rf_accept);
    const int mt = reg(a, sel_ch(OP_CHAN_TERRAIN), SCENE, rf_accept);
    CHECK(mf >= 0 && md >= 0 && mt >= 0, "registered %d %d %d", mf, md, mt);
    openpete_mods_material_frame_reset();
    CHECK(resolve(chan_tag(OP_CHAN_FLAME, 0), SCENE) == -1, "flame");
    CHECK(resolve(chan_tag(OP_CHAN_SHADOW, 0), DECAL) == -1, "shadow");
    CHECK(openpete_mods_material_terrain_bucket(3, 0, 0) == 0, "bake textured");
    CHECK(openpete_mods_material_terrain_bucket(-1, 0, 0) == 0, "bake untextured");
    CHECK(openpete_mods_material_terrain_tex_slot(NULL, 3) == 0, "walked textured");
    CHECK(openpete_mods_material_terrain_untex_slot(NULL) == 0, "walked untextured");
    CHECK(s_calls == 0, "calls %d", s_calls);
    OUTCOME("flame refine, DECAL refine, unkeyed terrain refine: all register, none draws, 0 calls");
}

/** @brief F3b: the draw-time level is read live, so a level push after the view capture changes a captured scene's resolve. */
static void case_f3b_view_level(void) {
    begin("F3b view-captured scene against a live level change");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-lev", 0, 13);
    openpete_mod_material_selector_t d = sel_moby(5);
    d.level = 3;
    const int md = reg(a, d, SCENE, NULL);
    openpete_mods_set_level(3);
    openpete_mods_material_frame_reset();
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_view_set(&v);
    CHECK(resolve(moby_tag(5, 0), SCENE) == md, "before");
    openpete_mods_set_level(4);
    CHECK(resolve(moby_tag(5, 0), SCENE) == -1, "after");
    openpete_mods_material_view_set(NULL);
    OUTCOME("view captured in level 3: resolves the material; after a push of level 4: stock");
}

/** @brief Host-loop mode (no view installed): a frame reset strands an earlier present's slot, and a reused slot names another instance's block. */
static void case_host_loop(void) {
    begin("Host-loop mode, no view installed");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-tint", 0, 5);
    const int mr = reg(a, sel_moby(-1), SCENE, rf_accept);
    openpete_mods_material_frame_reset();
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == 1, "slot %d", s);
    CHECK(resolve(moby_tag(5, s), SCENE) == mr, "same present");
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_frame_reset();
    CHECK(resolve(moby_tag(5, s), SCENE) == -1, "after reset, live table");
    openpete_mods_material_view_set(&v);
    CHECK(resolve(moby_tag(5, s), SCENE) == mr, "after reset, captured view");
    openpete_mods_material_view_set(NULL);
    const int s2 = openpete_mods_material_moby_slot(NULL, 6, 0x80020000u);
    uint32_t len = 0;
    const unsigned char* bytes = (const unsigned char*)openpete_mods_material_slot_uniforms(s, &len);
    CHECK(s2 == s && bytes && memcmp(bytes, v.slots[s].bytes, 16) != 0, "slot reused with other bytes");
    CHECK(resolve(moby_tag(5, s), SCENE) == mr, "stale tag resolves");
    OUTCOME("after a reset the old tag draws stock from the live table, the material from the view;"
            " once the slot is re-minted the old tag binds the new instance's block");
}

/** @brief A level-keyed terrain declarative buckets against the previous level until the game pushes the new one. */
static void case_level_change_bake(void) {
    begin("Level-keyed terrain declarative, CACHE=1, first present after a level change");
    stub_mod_t* a = mod("fx-tdlev", 0, 13);
    openpete_mod_material_selector_t d = sel_ch(OP_CHAN_TERRAIN);
    d.level = 9;
    const int md = reg(a, d, SCENE, NULL);
    openpete_mods_set_level(3);
    const unsigned e1 = openpete_mods_material_terrain_epoch();
    CHECK(openpete_mods_material_terrain_bucket(5, 0, 0) == 0, "level 3");
    /* The guest has entered level 9; the bake runs before the push. */
    CHECK(openpete_mods_material_terrain_bucket(5, 0, 0) == 0, "before push");
    openpete_mods_set_level(9);
    CHECK(openpete_mods_material_terrain_epoch() > e1, "epoch moved");
    CHECK(openpete_mods_material_terrain_bucket(5, 0, 0) == md + 1, "after push");
    OUTCOME("bake before the push: stock; push moves the epoch; bake after: the material");
}

/** @brief Postfx collect with more passes than the cap keeps the lowest-priority ones. */
static void case_postfx_cap(void) {
    begin("Postfx collect, 20 passes, cap 15");
    mod_postfx_rank_t v[20];
    for (int i = 0; i < 20; i++) { v[i].index = 19 - i; v[i].load_index = 19 - i; v[i].order = 0; }
    const int n = mod_postfx_rank(v, 20, 15);
    CHECK(n == 15, "kept %d", n);
    int ok = 1;
    for (int i = 0; i < n; i++) ok &= v[i].load_index == i;
    CHECK(ok, "kept the 15 lowest load indices in ascending order");
    mod_postfx_rank_t w[3] = {{2, 1, 0}, {0, 1, 5}, {1, 1, 0}};
    CHECK(mod_postfx_rank(w, 3, 15) == 3 && w[0].index == 1 && w[1].index == 2 && w[2].index == 0,
          "ties by order then index");
    OUTCOME("kept load 0..14, dropped 15..19 (the highest priority); same load: order, then index");
}

/** @brief Narrow-before-wide exclusions in one mod (sky-selector): the first same-mod registration wins, at api 12 and api 13. */
static void case_exclude(int api) {
    char name[96];
    snprintf(name, sizeof name, "narrow-before-wide exclusions in one mod (pin %d)", api);
    begin(name);
    stub_mod_t* a = mod("fx-exclude", 0, api);
    stub_mod_t* b = mod("fx-other", 1, 13);
    const int mx = reg(a, sel_moby(83), SCENE, NULL);
    const int mw = reg(a, sel_moby(-1), SCENE, NULL);
    reg(b, sel_moby(999), SCENE, rf_accept);
    openpete_mods_material_frame_reset();
    const int s = openpete_mods_material_moby_slot(NULL, 83, 0x80010000u);
    CHECK(s == 0, "slot %d", s);
    CHECK(resolve(moby_tag(83, s), SCENE) == mx, "excluded class");
    CHECK(resolve(moby_tag(99, 0), SCENE) == mw, "other class");
    OUTCOME("class 83: the exclusion (first registration); class 99: the wide material");
}

/** @brief An accept-all channel-wide refine (moby-instance-tint) claims every class over earlier materials. */
static void case_accept_all(void) {
    begin("Accept-all channel-wide refine");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-tint", 1, 5);
    reg(a, sel_moby(5), SCENE, NULL);
    const int mt = reg(b, sel_moby(-1), SCENE, rf_accept);
    openpete_mods_material_frame_reset();
    const int s5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    const int s77 = openpete_mods_material_moby_slot(NULL, 77, 0x80010100u);
    CHECK(resolve(moby_tag(5, s5), SCENE) == mt && resolve(moby_tag(77, s77), SCENE) == mt, "both");
    CHECK(s5 != s77, "distinct slots per instance");
    OUTCOME("every class resolves to the refine, one slot per distinct block");
}

/** @brief A level-keyed selector retuned on a level change (sky-selector): live outside a present, parked inside one. */
static void case_level_retune(void) {
    begin("Level-keyed selector retuned on a level change");
    stub_mod_t* a = mod("fx-fog", 0, 11);
    openpete_mod_material_selector_t d = sel_moby(-1);
    d.level = 3;
    const int md = reg(a, d, SCENE, NULL);
    openpete_mods_set_level(3);
    CHECK(resolve(moby_tag(5, 0), SCENE) == md, "level 3");
    openpete_mods_set_level(4);
    CHECK(resolve(moby_tag(5, 0), SCENE) == -1, "level 4 before retune");
    d.level = 4;
    CHECK(mod_mat_set_selector("fx-fog", md, &d) == 0, "retune");
    CHECK(resolve(moby_tag(5, 0), SCENE) == md, "level 4 after retune");
    openpete_mods_in_present = 1;
    d.level = 5;
    CHECK(mod_mat_set_selector("fx-fog", md, &d) == 0 && mod_mat_pending(), "parked");
    openpete_mods_in_present = 0;
    openpete_mods_set_level(5);
    CHECK(resolve(moby_tag(5, 0), SCENE) == -1, "level 5 parked");
    mod_mat_apply_pending();
    CHECK(resolve(moby_tag(5, 0), SCENE) == md, "level 5 applied");
    CHECK(mod_mat_set_selector("fx-other", md, &d) == -1, "foreign retune refused");
    OUTCOME("retune outside a present applies at once; inside one it waits for apply_pending");
}

/** @brief Cross-mod declarative order on flame, sky and player: the later-listed mod wins each shared key. */
static void case_cross_mod_decl(void) {
    begin("Cross-mod declarative order on flame, sky, player");
    stub_mod_t* a = mod("fx-broad", 0, 13);
    stub_mod_t* b = mod("fx-pair", 1, 13);
    openpete_mod_material_selector_t f0 = sel_ch(OP_CHAN_FLAME), f1 = sel_ch(OP_CHAN_FLAME);
    f0.ident = 0; f1.ident = 1;
    reg(a, f0, SCENE, NULL);
    reg(a, sel_ch(OP_CHAN_SKY), SCENE, NULL);
    reg(a, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    const int af1 = reg(a, f1, SCENE, NULL);
    const int bf0 = reg(b, f0, SCENE, NULL);
    const int bsk = reg(b, sel_ch(OP_CHAN_SKY), SCENE, NULL);
    const int bpl = reg(b, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    CHECK(resolve(chan_tag(OP_CHAN_FLAME, 0), SCENE) == bf0, "flame 0");
    CHECK(resolve(chan_tag(OP_CHAN_SKY, 0), SCENE) == bsk, "sky");
    CHECK(resolve(chan_tag(OP_CHAN_PLAYER, 0), SCENE) == bpl, "player");
    CHECK(resolve(chan_tag(OP_CHAN_FLAME, 1), SCENE) == af1, "flame 1");
    OUTCOME("flame 0, sky, player: the later mod; flame 1 (only the earlier mod): the earlier mod");
}

/** @brief Imported-sector terrain faces: the bake's texid -1 reads as untextured, the walked tag as textured with no texid. */
static void case_imported_sector(void) {
    begin("Imported-sector terrain faces (texid -1)");
    stub_mod_t* a = mod("fx-untex", 0, 13);
    stub_mod_t* b = mod("fx-tex", 1, 13);
    openpete_mod_material_selector_t u = sel_ch(OP_CHAN_TERRAIN), t = sel_ch(OP_CHAN_TERRAIN);
    u.flags = OP_SEL_UNTEXTURED;
    t.flags = OP_SEL_TEXTURED;
    const int mu = reg(a, u, SCENE, rf_accept);
    const int mt = reg(b, t, SCENE, NULL);
    openpete_mods_material_frame_reset();
    CHECK(openpete_mods_material_terrain_bucket(-1, 0, 0) == mu + 1, "bake");
    CHECK(resolve(ter_tag(0, 0, 0, 0), SCENE) == mt, "walked");
    OUTCOME("bake: the untextured-band refine; walked (textured, Tiledef bits 0): the textured declarative");
}

int main(void) {
    case_f1();
    case_f1a_level();
    case_f1a_decal();
    case_f2();
    case_f3();
    case_f4();
    case_hp_tier_moby();
    case_textured_moby();
    case_f3a();
    case_d3a();
    case_f3b_view_level();
    case_host_loop();
    case_level_change_bake();
    case_postfx_cap();
    case_exclude(12);
    case_exclude(13);
    case_accept_all();
    case_level_retune();
    case_cross_mod_decl();
    case_imported_sector();
    CHECK(s_split == 1, "tag split armed while materials are live");
    printf("%s: %d checks\n", s_fail ? "FAIL" : "PASS", s_checks);
    return s_fail;
}
