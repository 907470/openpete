// games/common/renderer/glow.c — the glow-ring feeder: a handwritten routine
// (Spyro 2 func_8004A8D4 / mobyfunc.s:1936-2146, Spyro 3 func_8002ECA8 /
// mobyfunc.s:1920-2166; the same 208 instructions but for constants and
// symbols) that draws one screen-facing polygon per live record of a pool and
// links each fan into the 3D ordering table itself, so none of its primitives
// reaches the 2D chokepoint the sprite feeder hooks. Every per-game address,
// count, stride and bound comes from game_layout.glow; the walk below is one
// body.
//
// STATIC ASM FINDINGS:
//   * the pool is 16 records of 0x14 bytes: world XYZ at +0x00, the primitive
//     colour at +0x0C, and a packed word at +0x10 holding the shape index, the
//     size, a rotation phase and a signed ordering-table bias (:1928-1929,
//     :1946-1951, :2002-2003).
//   * the shape comes from a table of 64-byte entries indexed by the shape byte
//     (:2008-2012): sixteen packed direction pairs, then sixteen halfwords of
//     vertex-index pairs ended by a zero, so a record draws up to sixteen
//     triangles fanned from its centre.
//   * the rotation is a 2x2 basis built from the sine table at the camera's own
//     yaw plus the record's phase, loaded into the GTE's rotation registers
//     (:2043-2058), so the polygon always faces the screen.
//   * each record emits one bundled draw-mode mini-packet (0xE1000620, whose
//     ABR bits are 0b01, additive) followed by its triangles, all chained into
//     one ordering-table entry (:2098-2110, :2170-2178).
//   * the triangles are POLY_G3 semi-transparent with the record's colour at
//     the centre vertex and black at the two rim vertices, so the ring is
//     brightest at its middle and fades out (:2110-2126).
//
// The geometry is the guest's own int16 output, read back out of the polybuf,
// not a second projection. What the primitives do not carry is depth — the PSX
// orders them by ordering-table bucket — so each fan's view-Z comes from
// projecting its own pool record, paired to the fan through the routine's own
// bookkeeping: each accepted record emits exactly one draw-mode packet, so the
// draw-mode packets delimit the fans in pool order, and the record walk here
// mirrors the routine's own rejects to say which records were accepted. The
// pairing is used only when the two counts agree; otherwise every fan of the
// call is left unpaired and the census reports it.
//
// Spyro 1's own glow renderer is fed by a float re-derivation in
// games/spyro-1/renderer/particle.c: its records carry a moby-position pointer
// and its ring is a perspective-scaled radius, a different routine over a
// different record shape. What the two share — the hook bracket, the polybuf
// walk, the primitive decode and the projection helpers — is already shared and
// is reused here unchanged.
//
// Out of scope: the same routine reached from the pause screen's own draw,
// which stays an existing carve-out; this hook is the one call the world draw
// makes.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the phase byte it
// decrements in each record; this feeder only reads them afterwards.

#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/env_knob.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "game_layout.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;

#define GW              (game_layout.glow)
#define GLOW_SHAPE_MASK 0xFFu

#define GLOW_MAX_PRIMS  512
#define GLOW_DRMODE     0xE1u   /* GP0 draw-mode packet, one per accepted record */
#define GLOW_TRI        0x32u   /* POLY_G3 semi-transparent */

typedef struct {
    ngte_prim p;
    float     depth;
    int       paired;
    int       blend;
} glow_item;

static glow_item s_item[GLOW_MAX_PRIMS];
static int       s_item_n;
static int       s_groups;        /* draw-mode packets the call appended */
static int       s_accepted;      /* records the pool walk predicted */
static uint32_t  s_code_hist[256];
static uint32_t  s_tag_hist[256];
static uint32_t  s_fed_tris;

static int glow_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= GLOW_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 0.0f;
    s_item[s_item_n].paired = 0;
    s_item[s_item_n].blend = 0;
    s_item_n++;
    return 1;
}

/* Walk the interval the call allocated: 3-word draw-mode packets (tag length 2)
 * and 7-word triangles (tag length 6). */
static void glow_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 6, .raw_words_max = 7, .guard = GLOW_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, glow_prim_sink, NULL);
}

/* One accepted record's view-Z and its blend-independent verdict. */
typedef struct { float depth; } glow_rec;

/* Re-walk the pool and mirror the routine's rejects, appending one depth per
 * record it would have drawn. Returns the count. */
static int glow_predict(CPUState* cpu, const ngte_view* v, glow_rec* out, int cap) {
    int n = 0;
    const double cx = v->cam[0], cy = v->cam[1], cz = v->cam[2];
    /* The fixed front entry is nearer than bucket 0, which the near guard in
     * openpete_native_depth_from_viewz already maps to the smallest depth. */
    const float front = openpete_native_depth_from_viewz(openpete_worldvz_full(1.0f));

    const double box = (double)GW.box;
    for (uint32_t s = 0; s < GW.slots; s++) {
        const uint32_t rec = GW.pool + s * GW.stride;
        const uint32_t packed = cpu->read_word(rec + GW.packed_off);
        if ((packed & GLOW_SHAPE_MASK) == 0u) continue;       /* :1932 free slot */
        const int bias = (int)(int8_t)(uint8_t)((packed >> 24) & 0xFFu);

        const double px = (double)(int32_t)cpu->read_word(rec + GW.pos_off + 0u);
        const double py = (double)(int32_t)cpu->read_word(rec + GW.pos_off + 4u);
        const double pz = (double)(int32_t)cpu->read_word(rec + GW.pos_off + 8u);
        const double vx = px - cx;            /* the routine's IR3 */
        const double vy = cy - py;            /* IR1 */
        const double vz = cz - pz;            /* IR2 */
        /* :1970-1988 drops the record when it leaves the camera-relative box. */
        if (vx <= -box || vx >= box || vy <= -box || vy >= box ||
            vz <= -box || vz >= box) continue;

        double m1, m2, m3;
        ngte_mac(v->cam_M, vy, vz, vx, &m1, &m2, &m3);
        /* :1994-2000 the screen-extent and depth window the routine culls on. */
        if (m3 < (double)GW.near_z) continue;
        if (m3 >= (double)GW.far_z) continue;
        if (4.0 * fabs(m1) - 3.0 * m3 - (double)GW.pyr_x >= 0.0) continue;
        if (3.0 * fabs(m2) - m3 - (double)GW.pyr_y >= 0.0) continue;

        if (n < cap) {
            const int d = (int)((uint32_t)(int32_t)m3 >> 7) - bias;
            out[n].depth = (d < 0 && GW.front_bias >= 0 && bias == GW.front_bias)
                ? front
                : openpete_native_depth_from_viewz(openpete_worldvz_full((float)m3));
        }
        n++;
    }
    return n;
}

/* Attach each captured primitive to its fan: a draw-mode packet opens one, its
 * ABR bits give the fan's blend, and the triangles until the next draw-mode
 * packet belong to it. */
static void glow_pair_depths(CPUState* cpu, const ngte_view* v) {
    static glow_rec pred[GLOW_MAX_PRIMS];
    s_groups = 0;
    for (int i = 0; i < s_item_n; i++)
        if (s_item[i].p.code == GLOW_DRMODE) s_groups++;
    s_accepted = glow_predict(cpu, v, pred, GLOW_MAX_PRIMS);
    const int pair = (s_accepted == s_groups && s_groups <= GLOW_MAX_PRIMS);

    int g = -1, blend = 0;
    float depth = 0.0f;
    for (int i = 0; i < s_item_n; i++) {
        if (s_item[i].p.code == GLOW_DRMODE) {
            g++;
            /* The draw-mode word's semi-transparency rate, bits 5 and 6. */
            blend = ngte_abr_blend((int)((s_item[i].p.raw[1] >> 5) & 3u),
                                   NGTE_ABR_QUARTER_AS_ADD);
            depth = pair ? pred[g].depth : 0.0f;
            continue;
        }
        if (g < 0) continue;                  /* a triangle before any draw mode */
        s_item[i].blend = blend;
        s_item[i].depth = depth;
        s_item[i].paired = pair;
    }
}

/* OPENPETE_GLOW=0 suppresses this feed alone. The guest draw still runs, so the
 * A/B prices the rings' painted pixels from one binary. */
static int glow_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLOW"); v = !(e && *e && *e == '0'); }
    return v;
}

static void glow_feed_native(void) {
    if (!glow_feed_enabled()) return;
    if (s_item_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_GLOW, GLOW_TRI, 0));
    for (int i = 0; i < s_item_n; i++) {
        const ngte_prim* p = &s_item[i].p;
        if (p->nv != 3) continue;
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = p->x[k]; st.v[k].sy = p->y[k]; st.v[k].z = s_item[i].depth;
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (p->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.blend = s_item[i].blend;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
        s_fed_tris++;
    }
    openpete_native_scene_set_tag(0);
}

/* OPENPETE_GLOW_CENSUS=LO-HI (or a single frame): per call in that frame range,
 * the primitives the routine appended by GPU code, the native triangles this
 * feeder emitted from them, the fans the draw-mode packets delimit, the records
 * the pool walk predicted, and how many primitives were left unpaired. A
 * capture whose fed count is zero measured nothing. */
static void glow_census(uint32_t start, uint32_t end, uint32_t fed_before) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_GLOW_CENSUS", openpete_frame)) return;
    int unpaired = 0, tris = 0;
    for (int i = 0; i < s_item_n; i++) {
        if (s_item[i].p.code == GLOW_DRMODE) continue;
        tris++;
        if (!s_item[i].paired) unpaired++;
    }
    fprintf(stderr, "[glow-census f=%u] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            openpete_frame, start, end, end - start, s_item_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_item_n; i++) seen[s_item[i].p.code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    fprintf(stderr, " tris=%d fans=%d accepted=%d fed_tris=%u unpaired=%d\n",
            tris, s_groups, s_accepted, s_fed_tris - fed_before, unpaired);
}

/* ---- hook ---------------------------------------------------------------- */

static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_GLOW, .prof_tag = "glow.srend", .st = &s_hb,
};

static int on_glow_draw(CPUState* cpu, uint32_t addr) {
    /* A game whose image holds a second call site over a frozen frame names
     * the world draw's return address; every other caller is refused. */
    if (GW.ra_guard && cpu->ra != GW.ra_guard) return 0;
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    s_item_n = 0; s_groups = 0; s_accepted = 0;
    const uint32_t fed_before = s_fed_tris;
    if (endp > startp) {
        glow_capture_prims(cpu, startp, endp);
        ngte_view view;
        ngte_view_build(cpu, &view);
        glow_pair_depths(cpu, &view);
        if (feed && render_path_enabled(RP_GLOW)) glow_feed_native();
    }
    glow_census(startp, endp, fed_before);
    return hook_bracket_end(&k_bracket);
}

/* Sub-tick re-extract. The primitives are the guest's own output and are gone
 * once the polybuf is reused, so the replay re-feeds the set the canonical tick
 * captured, at the depths paired then. At alpha 1 it reproduces the canonical
 * present exactly, which is what the spine's identity leg measures. */
void glow_reextract(CPUState* cpu) {
    (void)cpu;
    if (!hook_reextract_ready(RP_GLOW)) return;
    glow_feed_native();
}

/* The hook address is the layout's glow draw site, bound at boot: a static
 * initializer cannot read another TU's const. A game with no located routine
 * leaves it 0 and the registry dispatches nothing. */
static feature_dispatch_entry_t glow_dispatch[] = {
    { 0u, on_glow_draw },
    { 0u, NULL },
};

static void glow_apply_options(const openpete_options_t* opts) {
    (void)opts;
    glow_dispatch[0].addr = game_layout.glow.hook;
}

const feature_module_t glow_module = {
    .name           = "glow_native",
    .apply_options  = glow_apply_options,
    .dispatch_table = glow_dispatch,
};
