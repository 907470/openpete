/* mod_material_resolve_test - the material resolver (core/mods/mod_material_resolve.c)
 * against stub loader hooks.
 *
 * Each case registers materials for stub mods (id, enabled-list position,
 * manifest api pin), drives the extraction, bake and draw-time entry
 * points, and asserts the outcome the resolver produces. The cases record
 * the current rules: one material per draw; the moby extraction walk down
 * the priority order, where a refine's decline passes the moby to the next
 * matching material and the walk's outcome is stamped into the tag; one
 * moby predicate (level, contract, identity) at extraction and draw time;
 * refine registrations and retunes refused where a refine cannot take
 * effect; refine outcomes asked once per tick and replayed on the tick's
 * re-extracts; a same-mod tie kept by the first registration below api
 * pin 13 and by the later one from it; terrain refines and declaratives in
 * one enabled-list ranking on the bake and the walked path, one terrain
 * predicate (level and contract) on both paths, terrain refine outcomes
 * computed on the sim thread and read by the compose from the view, and the
 * level read from the installed view at draw time. Every stub mod carries
 * its api pin, so a rule keyed on the pin changes only the cases whose mods
 * sit on the changed side of it.
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
    if (fn && !mod_mat_refine_valid(m->id, &sel, contract)) return -1;
    return mod_mat_append(m->id, &sel, -1, 0, contract, fn, m->load_index, m->api, 1u);
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
/** @brief The moby tag field of a declarative landing on material @p m. */
#define DECL(m) (OPENPETE_MATERIAL_SLOT_DECL | (m))

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

/** @brief A later refine's decline falls through to the earlier declarative on the class. */
static void case_f1(void) {
    begin("Refine decline falls through to an earlier declarative");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mb = reg(b, sel_moby(5), SCENE, rf_decline);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == DECL(ma), "slot %#x", s);
    CHECK(s_calls == 1, "calls %d", s_calls);
    CHECK(resolve(moby_tag(5, s), SCENE) == ma, "declined instance resolves");
    CHECK(mod_mat_census_get(mb, MOD_MAT_CENSUS_DECLINES) == 1, "census declines");
    CHECK(mod_mat_census_get(mb, MOD_MAT_CENSUS_DECLINE_SHADOWS) == 0, "census decline shadows");
    CHECK(mod_mat_census_get(mb, MOD_MAT_CENSUS_FALLTHROUGHS) == 1, "census fallthroughs");
    CHECK(mod_mat_census_get(ma, MOD_MAT_CENSUS_EXTRACT_WINS) == 1, "census landing");
    CHECK(s_guard_max == 1 && s_guard_depth == 0 && s_guard_owner && strcmp(s_guard_owner, "fx-b") == 0,
          "guard balance");
    mod_mat_census_force(0);
    OUTCOME("class 5, B declines: the walk lands on A (DECL|%d), resolve A, refine calls 1, 1 fall-through", ma);

    begin("Refine accept wins over an earlier declarative");
    a = mod("fx-a", 0, 13);
    b = mod("fx-b", 1, 13);
    reg(a, sel_moby(5), SCENE, NULL);
    const int mb2 = reg(b, sel_moby(5), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int s2 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s2 == 1, "slot %d", s2);
    CHECK(resolve(moby_tag(5, s2), SCENE) == mb2, "accepted instance");
    OUTCOME("class 5, B accepts: slot 1, resolve B");
}

/** @brief A later declarative keyed to another level does not take the moby; the earlier refine is asked and claims it. */
static void case_f1a_level(void) {
    begin("Level-keyed declarative over a refine");
    stub_mod_t* a = mod("fx-r", 0, 13);
    stub_mod_t* b = mod("fx-d", 1, 13);
    const int mr = reg(a, sel_moby(5), SCENE, rf_accept);
    openpete_mod_material_selector_t d = sel_moby(5);
    d.level = 9;
    const int md = reg(b, d, SCENE, NULL);
    openpete_mods_set_level(3);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == 1, "slot %d", s);
    CHECK(s_calls == 1, "calls %d", s_calls);
    CHECK(resolve(moby_tag(5, s), SCENE) == mr, "resolve");
    openpete_mods_set_level(9);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s9 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s9 == DECL(md) && s_calls == 0, "level 9 slot %#x calls %d", s9, s_calls);
    OUTCOME("level 3, D keyed to 9: the refine is asked and claims (slot 1); level 9: D lands, 0 calls");
}

/** @brief A DECAL-contract refine on channel -1 is refused, and a DECAL declarative on channel -1 never takes a moby. */
static void case_f1a_decal(void) {
    begin("DECAL-contract refine on channel -1");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-decal", 1, 13);
    const int ma = reg(a, sel_moby(-1), SCENE, NULL);
    const int mb = reg(b, openpete_mod_material_sel_any(), DECAL, rf_accept);
    CHECK(mb == -1, "refused, got %d", mb);
    reg(b, openpete_mod_material_sel_any(), DECAL, NULL);
    reg(b, sel_moby(999), SCENE, rf_accept);   /* a live refine, so the walk runs */
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == DECL(ma) && s_calls == 0, "slot %#x calls %d", s, s_calls);
    CHECK(resolve(moby_tag(5, s), SCENE) == ma, "scene resolve");
    OUTCOME("DECAL refine refused; with a DECAL declarative on channel -1 the moby lands on A");
}

/** @brief A later channel-wide refine accepting only class 7 passes class 5 to an earlier class-5 declarative. */
static void case_f2(void) {
    begin("Channel-wide refine falls through to an earlier class declarative");
    stub_mod_t* a = mod("fx-a", 0, 12);
    stub_mod_t* b = mod("fx-wild", 1, 11);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mb = reg(b, sel_moby(-1), SCENE, rf_class7);
    openpete_mods_material_frame_reset(1);
    const int s5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    const int s7 = openpete_mods_material_moby_slot(NULL, 7, 0x80010100u);
    CHECK(s5 == DECL(ma), "class 5 slot %#x", s5);
    CHECK(resolve(moby_tag(5, s5), SCENE) == ma, "class 5 resolve");
    CHECK(resolve(moby_tag(7, s7), SCENE) == mb, "class 7 resolve");
    CHECK(resolve(moby_tag(5, 0), SCENE) == ma, "class 5 untagged");
    OUTCOME("class 5: A (the wild refine's decline falls through); class 7: wild refine");
}

/** @brief A later declarative outranks an earlier terrain refine on its texture, at bake and walked; an earlier declarative does not. */
static void case_f3(void) {
    begin("Terrain refine under a later declarative");
    stub_mod_t* a = mod("fx-tr", 0, 13);
    stub_mod_t* b = mod("fx-td", 1, 13);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 3;
    const int mr = reg(a, r, SCENE, rf_accept);
    const int md = reg(b, r, SCENE, NULL);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_terrain_bucket(3, 0, 0) == md + 1, "bake bucket");
    const int s = openpete_mods_material_terrain_tex_slot(NULL, 3);
    CHECK(s == 1, "walked slot %d", s);
    CHECK(resolve(ter_tag(3, 0, 0, s), SCENE) == md, "walked resolve");
    CHECK(resolve(ter_tag(3, 0, 0, 0), SCENE) == md, "slotless tag");
    CHECK(mod_mat_census_get(mr, MOD_MAT_CENSUS_OUTRANKED) == 1, "census outranked");
    mod_mat_census_force(0);
    OUTCOME("texture 3: bake bucket = the later declarative, walked = the later declarative; refine counted outranked");

    begin("Terrain refine over an earlier declarative");
    b = mod("fx-td", 0, 13);
    a = mod("fx-tr", 1, 13);
    const int md2 = reg(b, r, SCENE, NULL);
    const int mr2 = reg(a, r, SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_terrain_bucket(3, 0, 0) == mr2 + 1, "bake bucket");
    const int s2 = openpete_mods_material_terrain_tex_slot(NULL, 3);
    CHECK(resolve(ter_tag(3, 0, 0, s2), SCENE) == mr2, "walked resolve");
    (void)md2;
    OUTCOME("texture 3: bake bucket = the later refine, walked = the later refine");
}

/** @brief A walked terrain refine's decline ranks against the tag's declarative winner, per env key. */
static void case_terrain_decline(void) {
    begin("Walked terrain decline under and over a declarative");
    stub_mod_t* a = mod("fx-tr", 0, 13);
    stub_mod_t* b = mod("fx-tenv", 1, 13);
    stub_mod_t* c = mod("fx-tr2", 2, 13);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN), e = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 3;
    e.texture = 3;
    e.flags = OP_SEL_ENV_ANIM;
    reg(a, r, SCENE, rf_decline);
    const int me = reg(b, e, SCENE, NULL);
    openpete_mods_material_frame_reset(1);
    const int s = openpete_mods_material_terrain_tex_slot(NULL, 3);
    CHECK(s == OPENPETE_MATERIAL_TERRAIN_STOCK, "walked decline %#x", s);
    CHECK(resolve(ter_tag(3, 1, 0, s), SCENE) == me, "env face: the later declarative");
    CHECK(resolve(ter_tag(3, 0, 0, s), SCENE) == -1, "plain face: the decline");
    CHECK(openpete_mods_material_terrain_bucket(3, 1, 0) == me + 1, "bake env face");
    reg(c, r, SCENE, rf_decline);
    openpete_mods_material_frame_reset(1);
    const int s2 = openpete_mods_material_terrain_tex_slot(NULL, 3);
    CHECK(resolve(ter_tag(3, 1, 0, s2), SCENE) == -1, "env face under a later declining refine");
    OUTCOME("decline: env face draws the later env declarative, plain face stock; a later declining refine draws stock on both");
}

/** @brief A same-mod tie keeps the first registration, manifest row against C and C against C. */
static void case_f4(void) {
    begin("Same-mod tie, manifest row then C registration (pin 13)");
    stub_mod_t* a = mod("fx-pair", 0, 13);
    reg(a, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    const int mc = reg(a, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    CHECK(resolve(chan_tag(OP_CHAN_PLAYER, 0), SCENE) == mc, "player");
    OUTCOME("player: the C registration (later registration) wins");

    begin("Same-mod tie, manifest row then C registration (pin 12)");
    a = mod("fx-pair12", 0, 12);
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

    begin("Same-mod tie, C against C on flame ident 0 (pin 13)");
    a = mod("fx-gifts13", 0, 13);
    reg(a, f, SCENE, NULL);
    const int m2 = reg(a, f, SCENE, NULL);
    CHECK(resolve(chan_tag(OP_CHAN_FLAME, 0), SCENE) == m2, "flame");
    OUTCOME("flame ident 0: the later registration wins");
}

/** @brief OP_SEL_HP_TIER on a moby selector: the one moby predicate rejects it at extraction and at draw time. */
static void case_hp_tier_moby(void) {
    begin("OP_SEL_HP_TIER on a moby selector");
    stub_mod_t* a = mod("fx-hp", 0, 13);
    stub_mod_t* b = mod("fx-other", 1, 13);
    openpete_mod_material_selector_t s = sel_moby(-1);
    s.flags = OP_SEL_HP_TIER;
    const int ma = reg(a, s, SCENE, NULL);
    const int mb = reg(b, sel_moby(99), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int sl = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(sl == OPENPETE_MATERIAL_SLOT_STOCK, "slot %#x", sl);
    CHECK(mod_mat_census_get(ma, MOD_MAT_CENSUS_EXTRACT_WINS) == 0, "no extract win");
    CHECK(resolve(moby_tag(5, sl), SCENE) == -1, "stamped");
    CHECK(resolve(moby_tag(5, 0), SCENE) == -1, "draw-time");
    (void)mb;
    OUTCOME("extraction: no candidate (STOCK); draw-time resolve of an unstamped tag: stock");
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
    openpete_mods_material_frame_reset(1);
    const int sl = openpete_mods_material_moby_slot(NULL, 508, 0x80010000u);
    CHECK(sl == DECL(ma), "slot %#x", sl);
    CHECK(resolve(moby_tag(508, sl), SCENE) == ma, "draw-time");
    CHECK(resolve(moby_tag(508, 0), SCENE) == ma, "unstamped");
    OUTCOME("class 508: the walk lands on it (DECL); draw-time resolve: it");
}

/** @brief A level-keyed terrain refine draws on both paths only in its own level. */
static void case_f3a(void) {
    begin("Level-keyed terrain refine");
    stub_mod_t* a = mod("fx-tlev", 0, 13);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 4;
    r.level = 9;
    const int mr = reg(a, r, SCENE, rf_accept);
    openpete_mods_set_level(3);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    CHECK(openpete_mods_material_terrain_bucket(4, 0, 0) == 0, "bake");
    const int s = openpete_mods_material_terrain_tex_slot(NULL, 4);
    CHECK(s == 0 && s_calls == 0, "walked slot %d calls %d", s, s_calls);
    CHECK(resolve(ter_tag(4, 0, 0, s), SCENE) == -1, "walked resolve");
    openpete_mods_set_level(9);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_terrain_bucket(4, 0, 0) == mr + 1, "bake level 9");
    const int s9 = openpete_mods_material_terrain_tex_slot(NULL, 4);
    CHECK(resolve(ter_tag(4, 0, 0, s9), SCENE) == mr, "walked level 9");
    OUTCOME("level 3: bake stock, walked stock (no call); level 9: both refine");

    begin("DECAL-contract terrain refine on the walked path");
    a = mod("fx-tdecal", 0, 13);
    r.level = -1;
    reg(a, r, DECAL, rf_accept);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    CHECK(openpete_mods_material_terrain_tex_slot(NULL, 4) == 0 && s_calls == 0, "walked");
    CHECK(openpete_mods_material_terrain_bucket(4, 0, 0) == 0, "bake");
    OUTCOME("a DECAL terrain refine is asked on neither path");
}

/** @brief Refines that cannot take effect are refused at registration and at retune, before a retune is parked. */
static void case_d3a(void) {
    begin("Refines that cannot take effect are refused");
    stub_mod_t* a = mod("fx-bad", 0, 13);
    s_calls = 0;
    CHECK(reg(a, sel_ch(OP_CHAN_FLAME), SCENE, rf_accept) == -1, "flame refine");
    CHECK(reg(a, sel_ch(OP_CHAN_SHADOW), DECAL, rf_accept) == -1, "shadow DECAL refine");
    CHECK(reg(a, sel_moby(-1), DECAL, rf_accept) == -1, "moby DECAL refine");
    CHECK(reg(a, sel_ch(OP_CHAN_TERRAIN), SCENE, rf_accept) == -1, "unkeyed terrain refine");
    CHECK(mod_mat_count() == 0, "table %d", mod_mat_count());
    openpete_mod_material_selector_t t = sel_ch(OP_CHAN_TERRAIN), u = sel_ch(OP_CHAN_TERRAIN);
    t.texture = 3;
    u.flags = OP_SEL_UNTEXTURED;
    CHECK(reg(a, t, SCENE, rf_accept) >= 0, "texture-keyed terrain refine");
    CHECK(reg(a, u, SCENE, rf_accept) >= 0, "untextured-band terrain refine");
    CHECK(reg(a, sel_ch(-1), SCENE, rf_accept) >= 0, "channel -1 refine");
    CHECK(reg(a, sel_ch(OP_CHAN_FLAME), SCENE, NULL) >= 0, "flame declarative");
    const int mm = reg(a, sel_moby(5), SCENE, rf_accept);
    CHECK(mm >= 0, "moby refine");
    openpete_mod_material_selector_t f = sel_ch(OP_CHAN_FLAME);
    CHECK(mod_mat_set_selector("fx-bad", mm, &f) == -1, "retune to flame");
    openpete_mods_in_present = 1;
    openpete_mod_material_selector_t un = sel_ch(OP_CHAN_TERRAIN);
    CHECK(mod_mat_set_selector("fx-bad", mm, &un) == -1 && !mod_mat_pending(), "in-present retune not parked");
    openpete_mods_in_present = 0;
    CHECK(mod_mat_at(mm)->sel.channel == OP_CHAN_MOBY && mod_mat_at(mm)->sel.moby_class == 5, "old selector in force");
    openpete_mods_material_frame_reset(1);
    const int sl = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(sl > 0 && sl < OPENPETE_MATERIAL_SLOT_DECL, "moby refine still claims, slot %#x", sl);
    OUTCOME("refused: flame, shadow DECAL, moby DECAL, unkeyed terrain refines and both retunes; "
            "kept: keyed terrain, channel -1 and moby refines, the old selector after a refused retune");
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
    openpete_mods_material_frame_reset(1);
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_view_set(&v);
    CHECK(resolve(moby_tag(5, 0), SCENE) == md, "before");
    openpete_mods_set_level(4);
    CHECK(resolve(moby_tag(5, 0), SCENE) == md, "after");
    openpete_mods_material_view_set(NULL);
    CHECK(resolve(moby_tag(5, 0), SCENE) == -1, "live table after");
    OUTCOME("view captured in level 3: resolves the material before and after a push of level 4; the live table: stock");
}

/** @brief F3b: terrain refine outcomes are computed on the sim thread; the compose reads the view or this present's memo and calls nothing. */
static void case_f3b_terrain_refine(void) {
    begin("F3b terrain refine outcomes on the sim thread");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-tr", 0, 13);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN), u = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 6;
    u.flags = OP_SEL_UNTEXTURED;
    const int mr = reg(a, r, SCENE, rf_accept);
    const int mu = reg(a, u, SCENE, rf_decline);
    reg(a, sel_ch(-1), SCENE, rf_accept);   /* unkeyed: never asked on terrain */
    mod_mat_census_force(1);
    CHECK(openpete_mods_material_terrain_bucket(6, 0, 0) == mr + 1, "bake textured");
    CHECK(openpete_mods_material_terrain_bucket(-1, 0, 0) == mu + 1, "bake untextured");
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    openpete_mods_material_terrain_refine_present();
    CHECK(s_calls == 2, "sim-thread calls %d", s_calls);
    const int sr = mod_mat_terrain_bucket_slot(mr);
    CHECK(sr == 1, "memo slot %d", sr);
    CHECK(mod_mat_terrain_bucket_slot(mu) == OPENPETE_MATERIAL_TERRAIN_STOCK, "declined");
    CHECK(openpete_mods_material_terrain_tex_slot(NULL, 6) == sr && s_calls == 2, "walk reuses the memo");
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_frame_reset(1);   /* the sim thread opens the next tick's scene */
    s_calls = 0;
    openpete_mods_material_view_set(&v);
    CHECK(mod_mat_terrain_bucket_slot(mr) == sr, "view slot");
    CHECK(resolve(ter_tag(6, 0, 0, sr), SCENE) == mr, "walked tag through the view");
    openpete_mods_material_view_set(NULL);
    CHECK(mod_mat_terrain_bucket_slot(mr) == OPENPETE_MATERIAL_TERRAIN_STOCK, "stale memo");
    CHECK(mod_mat_census_get(mr, MOD_MAT_CENSUS_STALE_MEMO) == 1, "stale memo counted");
    CHECK(s_calls == 0, "compose calls %d", s_calls);
    CHECK(mod_mat_census_view_resolves() >= 1, "view resolves counted");
    mod_mat_census_force(0);
    OUTCOME("2 refine calls on the sim thread; the compose binds the view's outcome, a stale memo draws stock; 0 compose calls");
}

/** @brief The sim-thread refine pass asks no outranked terrain refine: one a later declarative takes on every face, or a lower refine on the same key. */
static void case_refine_present_outranked(void) {
    begin("Sim-thread refine pass skips outranked refines");
    stub_mod_t* a = mod("fx-tr", 0, 13);
    stub_mod_t* b = mod("fx-td", 1, 13);
    stub_mod_t* c = mod("fx-tr2", 2, 13);
    openpete_mod_material_selector_t r3 = sel_ch(OP_CHAN_TERRAIN), r5 = sel_ch(OP_CHAN_TERRAIN);
    r3.texture = 3;
    r5.texture = 5;
    reg(a, r3, SCENE, rf_accept);              /* outranked by b on every face of texture 3 */
    const int md = reg(b, r3, SCENE, NULL);
    reg(a, r5, SCENE, rf_accept);              /* outranked by c's refine on texture 5 */
    const int mc = reg(c, r5, SCENE, rf_accept);
    CHECK(openpete_mods_material_terrain_bucket(3, 0, 0) == md + 1, "texture 3 bake");
    CHECK(openpete_mods_material_terrain_bucket(3, 1, 1) == md + 1, "texture 3 env lp bake");
    CHECK(openpete_mods_material_terrain_bucket(5, 0, 0) == mc + 1, "texture 5 bake");
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    openpete_mods_material_terrain_refine_present();
    CHECK(s_calls == 1, "calls %d", s_calls);
    CHECK(mod_mat_terrain_bucket_slot(mc) == 1, "the winning refine has an outcome");
    OUTCOME("1 call: the texture-5 winner; the refine under the later declarative and the lower texture-5 refine get 0");
}

/** @brief Host-loop mode (no view installed): a frame reset strands an earlier present's slot, and a reused slot names another instance's block. */
static void case_host_loop(void) {
    begin("Host-loop mode, no view installed");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-tint", 0, 5);
    const int mr = reg(a, sel_moby(-1), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == 1, "slot %d", s);
    CHECK(resolve(moby_tag(5, s), SCENE) == mr, "same present");
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_frame_reset(1);
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

/** @brief Postfx collect with more passes than the cap keeps the highest-priority ones, lowest first. */
static void case_postfx_cap(void) {
    begin("Postfx collect, 20 passes, cap 15");
    mod_postfx_rank_t v[20];
    for (int i = 0; i < 20; i++) { v[i].index = 19 - i; v[i].load_index = 19 - i; v[i].order = 0; }
    const int n = mod_postfx_rank(v, 20, 15);
    CHECK(n == 15, "kept %d", n);
    int ok = 1;
    for (int i = 0; i < n; i++) ok &= v[i].load_index == i + 5;
    CHECK(ok, "kept the 15 highest load indices in ascending order");
    mod_postfx_rank_t w[3] = {{2, 1, 0}, {0, 1, 5}, {1, 1, 0}};
    CHECK(mod_postfx_rank(w, 3, 15) == 3 && w[0].index == 1 && w[1].index == 2 && w[2].index == 0,
          "ties by order then index");
    OUTCOME("kept load 5..19 in run order, dropped 0..4 (the lowest priority); same load: order, then index");
}

/** @brief Narrow-before-wide exclusions in one mod (sky-selector): the first same-mod registration wins at api 12, the later one at api 13. */
static void case_exclude(int api) {
    char name[96];
    snprintf(name, sizeof name, "narrow-before-wide exclusions in one mod (pin %d)", api);
    begin(name);
    stub_mod_t* a = mod("fx-exclude", 0, api);
    stub_mod_t* b = mod("fx-other", 1, 13);
    const int mx = reg(a, sel_moby(83), SCENE, NULL);
    const int mw = reg(a, sel_moby(-1), SCENE, NULL);
    reg(b, sel_moby(999), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int s = openpete_mods_material_moby_slot(NULL, 83, 0x80010000u);
    CHECK(s == DECL(api >= 13 ? mw : mx), "slot %#x", s);
    CHECK(resolve(moby_tag(83, s), SCENE) == (api >= 13 ? mw : mx), "excluded class");
    CHECK(resolve(moby_tag(99, 0), SCENE) == mw, "other class");
    OUTCOME("class 83: %s; class 99: the wide material",
            api >= 13 ? "the wide material (later registration)" : "the exclusion (first registration)");
}

/** @brief An accept-all channel-wide refine (moby-instance-tint) claims every class over earlier materials. */
static void case_accept_all(void) {
    begin("Accept-all channel-wide refine");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-tint", 1, 5);
    reg(a, sel_moby(5), SCENE, NULL);
    const int mt = reg(b, sel_moby(-1), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
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
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_terrain_bucket(-1, 0, 0) == mu + 1, "bake");
    CHECK(resolve(ter_tag(0, 0, 0, 0), SCENE) == mt, "walked");
    OUTCOME("bake: the untextured-band refine; walked (textured, Tiledef bits 0): the textured declarative");
}

/** @brief The walk runs down the priority order: each decline passes the moby on, a refine below the landing is never asked, an exhausted walk draws stock. */
static void case_walk_chain(void) {
    begin("Walk down the priority order");
    stub_mod_t* r = mod("fx-low", 0, 11);
    stub_mod_t* a = mod("fx-a", 1, 12);
    stub_mod_t* b = mod("fx-b", 2, 8);
    stub_mod_t* c = mod("fx-c", 3, 13);
    const int mr = reg(r, sel_moby(-1), SCENE, rf_accept);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mb = reg(b, sel_moby(-1), SCENE, rf_decline);
    const int mc = reg(c, sel_moby(-1), SCENE, rf_class7);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s5 == DECL(ma) && s_calls == 2, "class 5 slot %#x calls %d", s5, s_calls);
    CHECK(s_guard_owner && strcmp(s_guard_owner, "fx-b") == 0 && s_guard_depth == 0, "last asked B");
    CHECK(mod_mat_census_get(mc, MOD_MAT_CENSUS_FALLTHROUGHS) == 1 &&
          mod_mat_census_get(mb, MOD_MAT_CENSUS_FALLTHROUGHS) == 1, "fall-throughs");
    CHECK(mod_mat_census_get(mr, MOD_MAT_CENSUS_REFINE_CALLS) == 0, "refine below the landing not asked");
    s_calls = 0;
    const int s6 = openpete_mods_material_moby_slot(NULL, 6, 0x80020000u);
    CHECK(s6 > 0 && s6 < OPENPETE_MATERIAL_SLOT_DECL && s_calls == 3, "class 6 slot %#x calls %d", s6, s_calls);
    CHECK(resolve(moby_tag(6, s6), SCENE) == mr, "class 6 lands on the lowest refine");
    const int s7 = openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    CHECK(resolve(moby_tag(7, s7), SCENE) == mc, "class 7 claimed by the top refine");
    mod_mat_enable(mr, 0);
    openpete_mods_material_frame_reset(1);
    const int s8 = openpete_mods_material_moby_slot(NULL, 8, 0x80040000u);
    CHECK(s8 == OPENPETE_MATERIAL_SLOT_STOCK, "exhausted %#x", s8);
    CHECK(resolve(moby_tag(8, s8), SCENE) == -1, "exhausted resolves stock");
    mod_mat_census_force(0);
    OUTCOME("class 5: C, B decline, A lands (2 calls, the refine under A not asked); class 6: C, B decline, "
            "the lowest refine claims; class 7: C claims; every material declining: STOCK");
}

/** @brief A declarative landing binds its static block and stops drawing when its material, owner or hold changes before the draw. */
static void case_landing_state(void) {
    begin("Declarative landing at draw time");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const float blk[4] = {1, 0, 0, 1};
    CHECK(mod_mat_set_params(ma, blk, sizeof blk) == 0, "params");
    reg(b, sel_moby(-1), SCENE, rf_decline);
    openpete_mods_material_frame_reset(1);
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == DECL(ma), "slot %#x", s);
    int slot = -1;
    CHECK(mod_mat_resolve_index(moby_tag(5, s), SCENE, &slot) == ma && slot == 0, "static block, slot %d", slot);
    CHECK(mod_mat_view_static_slot(ma) > 0, "static slot interned");
    CHECK(resolve(moby_tag(5, s), DECAL) == -1, "other contract");
    mod_mat_enable(ma, 0);
    CHECK(resolve(moby_tag(5, s), SCENE) == -1, "user-disabled after the stamp");
    mod_mat_enable(ma, 1);
    a->enabled = 0;
    CHECK(resolve(moby_tag(5, s), SCENE) == -1, "owner disabled after the stamp");
    a->enabled = 1;
    const char* held[1] = {"fx-a"};
    openpete_mods_hold_owners(held, 1);
    CHECK(resolve(moby_tag(5, s), SCENE) == -1, "held owner");
    openpete_mods_hold_owners(NULL, 0);
    CHECK(resolve(moby_tag(5, s), SCENE) == ma, "released");
    OUTCOME("DECL|%d binds the static block; user disable, owner disable and hold each draw stock", ma);
}

/** @brief One moby predicate: identity is the class, a level key matches its own level, OP_SEL_TEXTURED accepts, and a channel -1 refine takes mobys. */
static void case_moby_predicate(void) {
    begin("One moby predicate: identity, level, textured, channel -1");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    openpete_mod_material_selector_t id5 = sel_moby(-1), lev = sel_moby(-1);
    id5.ident = 5;
    lev.level = 4;
    lev.flags = OP_SEL_TEXTURED;
    const int mi = reg(a, id5, SCENE, NULL);
    const int ml = reg(b, lev, SCENE, NULL);
    const int mw = reg(b, sel_ch(-1), SCENE, rf_class7);
    openpete_mods_set_level(3);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_moby_slot(NULL, 5, 0x80010000u) == DECL(mi), "ident 5 on class 5");
    CHECK(openpete_mods_material_moby_slot(NULL, 6, 0x80020000u) == OPENPETE_MATERIAL_SLOT_STOCK, "class 6");
    CHECK(resolve(moby_tag(5, 0), SCENE) == mi && resolve(moby_tag(6, 0), SCENE) == -1, "draw-time");
    const int s7 = openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    CHECK(resolve(moby_tag(7, s7), SCENE) == mw, "channel -1 refine claims class 7");
    openpete_mods_set_level(4);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_moby_slot(NULL, 6, 0x80020000u) == DECL(ml), "level 4, textured key");
    OUTCOME("ident 5 takes class 5 only; a level-4 textured declarative takes mobys in level 4; "
            "a channel -1 refine claims on the moby channel");
}

/** @brief Refine outcomes are asked once per tick: a re-extract of the tick replays them, a new tick or a table change asks again. */
static void case_tick_memo(void) {
    begin("Refine outcomes kept for the tick");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mb = reg(b, sel_moby(-1), SCENE, rf_class7);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 6;
    const int mt = reg(b, r, SCENE, rf_accept);
    CHECK(openpete_mods_material_terrain_bucket(6, 0, 0) == mt + 1, "bake");
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    openpete_mods_material_terrain_refine_present();
    const int s5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    const int s7 = openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    CHECK(s_calls == 3 && s5 == DECL(ma) && s7 == 2, "tick open: calls %d slots %#x %#x", s_calls, s5, s7);
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_frame_reset(0);   /* a re-extract of the same tick */
    openpete_mods_material_terrain_refine_present();
    const int r7 = openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    const int r5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s_calls == 3, "replay calls %d", s_calls);
    CHECK(r5 == s5 && r7 == s7 && resolve(moby_tag(7, r7), SCENE) == mb, "replayed %#x %#x", r5, r7);
    CHECK(mod_mat_terrain_bucket_slot(mt) == 1 && mod_mat_census_memo_replays() == 3, "terrain replayed, %u",
          mod_mat_census_memo_replays());
    uint32_t len = 0;
    const void* blk = openpete_mods_material_slot_uniforms(r7, &len);
    CHECK(blk && len == 16 && memcmp(blk, v.slots[s7].bytes, 16) == 0, "replayed block");
    mod_mat_enable(ma, 1);                   /* a table change voids the memo */
    openpete_mods_material_frame_reset(0);
    openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    CHECK(s_calls == 4, "after a table change calls %d", s_calls);
    openpete_mods_material_frame_reset(1);
    openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    CHECK(s_calls == 5, "new tick calls %d", s_calls);
    mod_mat_census_force(0);
    OUTCOME("tick open: 3 calls; re-extract: 0 calls, the same outcomes re-interned; "
            "a table change or a new tick asks again");
}

/** @brief A refine claim that finds the slot table full draws stock for that present and is not kept for the tick. */
static void case_slot_table_full(void) {
    begin("Slot table full");
    stub_mod_t* a = mod("fx-tint", 0, 13);
    reg(a, sel_moby(-1), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    int last = 0;
    for (int i = 0; i < MOD_MATERIAL_SLOTS - 1; i++)
        last = openpete_mods_material_moby_slot(NULL, 5, 0x80100000u + (uint32_t)i * 0x100u);
    CHECK(last == MOD_MATERIAL_SLOTS - 1, "last slot %d", last);
    s_calls = 0;
    const int over = openpete_mods_material_moby_slot(NULL, 5, 0x80900000u);
    CHECK(over == OPENPETE_MATERIAL_SLOT_STOCK && s_calls == 1, "full: %#x", over);
    openpete_mods_material_frame_reset(0);
    const int again = openpete_mods_material_moby_slot(NULL, 5, 0x80900000u);
    CHECK(again == 1 && s_calls == 2, "asked again on the re-extract: slot %d calls %d", again, s_calls);
    OUTCOME("the 128th distinct block draws stock; the re-extract asks again and gets slot 1");
}

int main(void) {
    case_f1();
    case_f1a_level();
    case_f1a_decal();
    case_f2();
    case_f3();
    case_terrain_decline();
    case_f4();
    case_hp_tier_moby();
    case_textured_moby();
    case_f3a();
    case_d3a();
    case_f3b_view_level();
    case_f3b_terrain_refine();
    case_refine_present_outranked();
    case_host_loop();
    case_level_change_bake();
    case_postfx_cap();
    case_exclude(12);
    case_exclude(13);
    case_accept_all();
    case_level_retune();
    case_cross_mod_decl();
    case_imported_sector();
    case_walk_chain();
    case_landing_state();
    case_moby_predicate();
    case_tick_memo();
    case_slot_table_full();
    CHECK(s_split == 1, "tag split armed while materials are live");
    printf("%s: %d checks\n", s_fail ? "FAIL" : "PASS", s_checks);
    return s_fail;
}
