// games/common/renderer/tracers.c — the tracer ribbon feeders: two handwritten
// routines that build textured gouraud quad strips and link them into the 3D
// ordering table themselves, so none of their primitives reaches the 2D
// chokepoint the sprite feeder hooks. Both are the same routine in each game
// (ribbon: Spyro 2 func_8004B72C / mobyfunc.s:2936-3157, Spyro 3 func_8002FB68
// / mobyfunc.s:2940-3181, 217 of 222 instructions; mesh: Spyro 2 func_8004BAA4
// / mobyfunc.s:3180-3402, Spyro 3 func_8002FEE0 / mobyfunc.s:3182-3418, the
// same arithmetic under a different load-delay schedule). Every per-game
// address, count, stride and bound comes from game_layout.tracers; the walks
// below are one body.
//
// Two producers, one capture:
//
//   * the point-ring ribbon (asm/mobyfunc.s:2940-3181). Walks a 0x20-slot array
//     of 0x6C-byte records, each holding a 16-entry circular buffer of packed
//     unsigned XYZ halves plus a head index, a point count and a signed width
//     byte at +0x64 and a colour-plus-command word at +0x68. It RTPS-projects
//     every live point of a record into the scratchpad, then walks consecutive
//     pairs and appends one 13-word POLY_GT4 per segment: the two projected
//     points each expanded by the record's half-width, which tapers linearly to
//     zero along the ribbon, and DPCS-faded from the record's colour. The width
//     rides in the high half of a packed screen coordinate while it is positive
//     and in the low half while it is negative, so the ribbon expands vertically
//     or horizontally by the sign of that byte.
//   * the indexed mesh (asm/mobyfunc.s:3182-3418). Takes one of eight 0x14-byte
//     descriptors: a vertex list of world XYZ triples ended by -1, a colour
//     table, an index list of 8-byte entries ended by -1, a pair of texture
//     words, and a right-shift applied to every camera-relative coordinate. It
//     projects the vertex list into the scratchpad as {screen XY, MAC3 and
//     outcode} pairs and appends one POLY_GT4 per index entry. It also stores
//     the highest ordering-table bucket it touched, so running its body twice
//     would move a value the next draw reads: the shared bracket runs the guest
//     draw exactly once.
//
// The geometry is the guest's own int16 output, read back out of the polybuf,
// not a second projection: the primitives are the routine's only product and
// re-deriving the ribbon's width law and DPCS fade would be a second copy of
// them. What the primitives do not carry is depth — the PSX orders them by
// ordering-table bucket — so each one's view-Z is recovered from the producer's
// own record:
//
//   * the mesh leg replays the routine's own emit test out of the scratchpad
//     vertex table the routine just wrote, which still holds this call's
//     vertices: the pairing is then the routine's own arithmetic on its own
//     values, and the depth is the MAC3 average the routine bucketed by.
//   * the ribbon leg re-walks the record array and mirrors the routine's per
//     record rejects (the camera-relative box test, the shared-outcode test and
//     the near-depth test) to predict how many segments each record emitted.
//     The prediction is accepted only when the total equals the number of
//     primitives captured; otherwise every primitive of the call is left
//     unpaired and the census reports it.
//
// Spyro 1's tracer.c is not made a client of this: its beam is re-derived in
// float from tracer-point gamestate with its own ribbon-width and colour laws
// and its primitives are only its A/B oracle, while these routines' primitives
// are the geometry. What the two share — the hook bracket, the polybuf walk,
// the primitive decode and the projection helpers — is already shared and is
// reused here unchanged.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the polybuf cursor
// and the ordering-table top; this feeder only reads them afterwards.

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

#define TR (game_layout.tracers)

/* Record shape of the point-ring ribbon (asm/mobyfunc.s:2940-2947, 3096-3100
 * in the Spyro 3 image): the live pointer at +0x00, zero = free slot. */
#define RIB_LIVE         0x00u
#define RIB_POINT_MAX    256

/* Descriptor shape of the indexed mesh (asm/mobyfunc.s:3225-3246). */
#define MSH_INDICES      0x08u
#define MSH_SHIFT        0x10u

/* The scratchpad table both routines project into holds 8 bytes per vertex:
 * the screen XY word and the MAC3-plus-outcode word (:3196-3200, :3218-3221). */
#define SPAD_MAX_VERTS   128

#define TR_MAX_PRIMS     512    /* primitives one call may contribute */
#define TR_MAX_TICK      2048   /* primitives one tick may contribute */

typedef struct {
    ngte_prim p;
    float     depth;
    int       paired;
} tr_item;

static tr_item  s_item[TR_MAX_PRIMS];
static int      s_item_n;

/* The tick's whole set, replayed by the sub-tick leg: nine calls contribute to
 * one canonical present and the polybuf is reused before the next one. */
typedef struct { ngte_prim p; float depth; } tr_held;
static tr_held  s_tick[TR_MAX_TICK];
static int      s_tick_n;
static unsigned s_tick_frame = 0xFFFFFFFFu;

static uint32_t s_code_hist[256];
static uint32_t s_tag_hist[256];
static uint32_t s_fed_tris;
static int      s_last_predicted;   /* segments the record walk predicted */
static int      s_live;             /* records holding a live pointer */
static int      s_boxed;            /* of those, records the box test dropped */

static int tr_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= TR_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 1.0f;   /* unpaired: the near guard (view-Z 1) */
    s_item[s_item_n].paired = 0;
    s_item_n++;
    return 1;
}

/* Walk the interval the call allocated. Both routines append one 13-word
 * primitive at a time (tag length 12). */
static void tr_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 12, .raw_words_max = 13, .guard = TR_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, tr_prim_sink, NULL);
}

/* ---- ribbon leg: predict the segments the record walk emitted -------------- */

typedef struct { int32_t oc; int32_t sz3; double m3; } rib_pt;

/* The routine's own outcode word (:2488-2508): the packed screen coordinate
 * shifted left five with one bit per frame edge the point falls outside. Only
 * the low four bits are tested, and only as an AND across a record's points or
 * across a segment's two ends. */
static int32_t rib_outcode(int sx, int sy) {
    const int32_t xy = (int32_t)(((uint32_t)(sy & 0xFFFF) << 16) | ((uint32_t)sx & 0xFFFFu));
    int32_t oc = (int32_t)((uint32_t)xy << 5);
    int32_t t = xy;
    if (!(t > 0)) oc += 1;
    t = xy - 0x00F00000;
    if (!(t < 0)) oc += 2;
    t = (int32_t)((uint32_t)xy << 16);
    if (!(t > 0)) oc += 4;
    t = t - 0x02000000;
    if (!(t < 0)) oc += 8;
    return oc;
}

/* SZ3 with sf = 1: MAC3 saturated into an unsigned 16-bit value. */
static int32_t rib_sz3(double m3) {
    double z = floor(m3);
    if (z < 0.0) return 0;
    if (z > 65535.0) return 65535;
    return (int32_t)z;
}

/* Re-walk the record array and mirror the routine's rejects, appending one
 * predicted depth per segment it would have emitted. Returns the count. */
static int rib_predict(CPUState* cpu, const ngte_view* v, float* out, int cap) {
    int n = 0;
    s_live = 0; s_boxed = 0;
    /* The camera position the routine reads, shifted right one like its own
     * `srl` so the box test below compares the same values. */
    const uint32_t cx = cpu->read_word(game_layout.cam.position + 0u) >> 1;
    const uint32_t cy = cpu->read_word(game_layout.cam.position + 4u) >> 1;
    const uint32_t cz = cpu->read_word(game_layout.cam.position + 8u) >> 1;

    static rib_pt pt[RIB_POINT_MAX];
    for (uint32_t s = 0; s < TR.slots; s++) {
        const uint32_t rec = TR.ring + s * TR.stride;
        if (cpu->read_word(rec + RIB_LIVE) == 0) continue;
        s_live++;
        const uint32_t packed = cpu->read_word(rec + TR.packed_off);
        const int head  = (int)(packed & 0xFFu);
        const int count = (int)((packed >> 8) & 0xFFu);
        if (count < 2 || count > TR.ring_n || head >= count) continue;

        int rejected = 0;
        int32_t and_oc = -1;
        for (int k = 0; k < count && !rejected; k++) {
            const int idx = ((head - k) % count + count) % count;
            const uint32_t p = rec + TR.points_off + (uint32_t)idx * 6u;
            const int32_t px = (int32_t)((uint32_t)cpu->read_half(p + 0u) << 1);
            const int32_t py = (int32_t)((uint32_t)cpu->read_half(p + 2u) << 1);
            const int32_t pz = (int32_t)((uint32_t)cpu->read_half(p + 4u) << 1);
            const int32_t vx = px - (int32_t)cx;   /* the routine's IR3 */
            const int32_t vy = (int32_t)cy - py;   /* IR1 */
            const int32_t vz = (int32_t)cz - pz;   /* IR2 */
            /* :2408-2437 drops the whole record when any point leaves the box. */
            if (vx <= -TR.box || vx >= TR.box || vy <= -TR.box || vy >= TR.box ||
                vz <= -TR.box || vz >= TR.box) { rejected = 1; break; }
            ngte_proj o;
            ngte_project_gte(v->cam_M, NULL, (double)vy, (double)vz, (double)vx,
                             v->ofx, v->ofy, v, 0, &o);
            pt[k].m3  = o.m3;
            pt[k].sz3 = rib_sz3(o.m3);
            pt[k].oc  = rib_outcode((int)lrint(o.sx), (int)lrint(o.sy));
            and_oc &= pt[k].oc;
        }
        if (rejected) { s_boxed++; continue; }
        if ((and_oc & 0xF) > 0) continue;            /* :2496 every point off one edge */
        if (pt[0].sz3 - TR.far_sz >= 0) continue;    /* :2540 the whole record too far */

        for (int k = 0; k + 1 < count; k++) {
            if ((pt[k].oc & pt[k + 1].oc & 0xF) > 0) continue;        /* :2652 */
            const int32_t key = ((pt[k].sz3 + pt[k + 1].sz3) >> 7) - 2;
            if (key < 0) continue;                                    /* :2692 */
            if (n < cap) {
                /* The record's halves are quarter world units and the routine
                 * halves the whole vector, so its MAC3 is half the world value. */
                const double mz = (pt[k].m3 + pt[k + 1].m3) * 0.5;
                out[n] = openpete_native_scene_viewz(
                    openpete_worldvz_scaled((float)mz, 2.0f));
            }
            n++;
        }
    }
    return n;
}

static void rib_pair_depths(CPUState* cpu, const ngte_view* v) {
    static float pred[TR_MAX_PRIMS];
    const int n = rib_predict(cpu, v, pred, TR_MAX_PRIMS);
    s_last_predicted = n;
    if (n != s_item_n || n > TR_MAX_PRIMS) return;   /* every primitive stays unpaired */
    for (int i = 0; i < n; i++) {
        s_item[i].depth = pred[i];
        s_item[i].paired = 1;
    }
}

/* ---- mesh leg: replay the emit test out of the routine's own scratchpad ---- */

static void msh_pair_depths(CPUState* cpu, uint32_t desc) {
    const uint32_t ilist = cpu->read_word(desc + MSH_INDICES);
    if (ilist < 0x80000000u || ilist >= 0x80800000u) return;
    const int shift = (int)cpu->read_word(desc + MSH_SHIFT);
    const float mul = (shift >= 0 && shift < 24) ? (float)(1u << (unsigned)shift) : 1.0f;

    int i = 0;
    for (uint32_t e = ilist; i < s_item_n; e += 8u) {
        const uint32_t w0 = cpu->read_word(e);
        if (w0 == 0xFFFFFFFFu) break;
        /* :3236-3251 four 8-bit vertex indices, each scaled to its 8-byte
         * scratchpad slot before the table read. */
        const uint32_t off[4] = {
            (w0 & 0xFFu) << 3,
            ((uint32_t)((int32_t)w0 >> 5)) & 0x7F8u,
            ((uint32_t)((int32_t)w0 >> 13)) & 0x7F8u,
            ((uint32_t)((int32_t)w0 >> 21)) & 0x7F8u,
        };
        int32_t and_oc = -1, zsum = 0;
        for (int k = 0; k < 4; k++) {
            if (off[k] >= SPAD_MAX_VERTS * 8u) return;   /* outside the table */
            const int32_t w1 = (int32_t)cpu->read_word(TR.spad + off[k] + 4u);
            and_oc &= w1;
            zsum += w1 >> 16;                            /* :3241 sra 16 */
        }
        if ((and_oc & 0xF) > 0) continue;                /* :3256 */
        int32_t key = zsum >> 9;
        key = (int32_t)((uint32_t)key << (unsigned)(shift & 31));
        if (key < 0 || key - 0x7FF > 0) continue;        /* :3262-3264 */
        s_item[i].depth = openpete_native_scene_viewz(
            openpete_worldvz_scaled((float)zsum * 0.25f, mul));
        s_item[i].paired = 1;
        i++;
    }
}

/* ---- feed ---------------------------------------------------------------- */

/* OPENPETE_TRACERS=0 suppresses this feed alone. The guest draws still run, so
 * the A/B prices the ribbons' painted pixels from one binary. */
static int tr_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TRACERS"); v = !(e && *e && *e == '0'); }
    return v;
}

static void tr_add_prim(const ngte_prim* p, float depth) {
    static const int split[2][3] = { {0, 1, 2}, {1, 3, 2} };
    if (p->nv != 3 && p->nv != 4) return;
    ngte_tpage tp;
    int blend = p->stp ? 1 : 0;
    if (p->textured) {
        ngte_decode_tpage((unsigned)p->tpage, (unsigned)p->clut, 0u, &tp);
        if (p->stp) blend = ngte_abr_blend(tp.abr, NGTE_ABR_QUARTER_AS_ADD);
    } else {
        memset(&tp, 0, sizeof tp);
    }
    const int last = p->nv - 1;           /* a triangle repeats its last corner */
    for (int s = 0; s < 2; s++) {
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            const int j = split[s][k] == 3 ? last : split[s][k];
            st.v[k].sx = p->x[j]; st.v[k].sy = p->y[j]; st.v[k].z = depth;
            st.v[k].pvz = depth;   /* fed at one depth: W constant across the prim */
            st.v[k].u = (float)p->u[j]; st.v[k].v = (float)p->v[j];
            st.v[k].rgba = (p->rgb[j] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = p->textured;
        if (p->textured) {
            st.tpage_x = tp.tpx; st.tpage_y = tp.tpy;
            st.clut_x  = tp.clx; st.clut_y  = tp.cly; st.bpp = tp.bpp;
        }
        st.blend = blend;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
        s_fed_tris++;
    }
}

static void tr_feed_native(const tr_held* set, int n) {
    if (!tr_feed_enabled()) return;
    if (n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_TRACER(0));
    for (int i = 0; i < n; i++) tr_add_prim(&set[i].p, set[i].depth);
    openpete_native_scene_set_tag(0);
}

/* Hold the call's primitives for the sub-tick replay and return where they
 * start in the held set. The tick's first hook clears it: nine calls
 * contribute to one canonical present. */
static int tr_hold(void) {
    if (s_tick_frame != openpete_frame) { s_tick_frame = openpete_frame; s_tick_n = 0; }
    const int first = s_tick_n;
    for (int i = 0; i < s_item_n && s_tick_n < TR_MAX_TICK; i++) {
        s_tick[s_tick_n].p = s_item[i].p;
        s_tick[s_tick_n].depth = s_item[i].depth;
        s_tick_n++;
    }
    return first;
}

/* OPENPETE_TRACER_CENSUS=LO-HI (or a single frame): per call in that frame
 * range, the primitives the routine appended by GPU code, the native triangles
 * this feeder emitted from them, how many primitives the record walk predicted,
 * and how many were left unpaired. A capture whose fed count is zero measured
 * nothing. */
static void tr_census(const char* leg, uint32_t start, uint32_t end, uint32_t fed_before) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_TRACER_CENSUS", openpete_frame)) return;
    int unpaired = 0;
    for (int i = 0; i < s_item_n; i++) if (!s_item[i].paired) unpaired++;
    fprintf(stderr, "[tracer-census f=%u %s] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            openpete_frame, leg, start, end, end - start, s_item_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_item_n; i++) seen[s_item[i].p.code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    fprintf(stderr, " fed_tris=%u predicted=%d unpaired=%d live=%d boxed=%d tick=%d\n",
            s_fed_tris - fed_before, s_last_predicted, unpaired, s_live, s_boxed, s_tick_n);
}

/* ---- hooks ---------------------------------------------------------------- */

static hook_bracket_state s_hb_rib;
static const hook_bracket_desc k_rib = {
    .path = RP_TRACER, .prof_tag = "tracer3.srend", .st = &s_hb_rib,
};
static hook_bracket_state s_hb_msh;
/* The mesh leg's descriptor argument, read before the guest draw runs: the
 * routine spills and reuses every saved register, so a0 is gone afterwards. */
static uint32_t s_msh_desc;
static void msh_pre(CPUState* cpu, int feed, int verify) {
    (void)feed; (void)verify;
    s_msh_desc = cpu->a0;
}
static const hook_bracket_desc k_msh = {
    .path = RP_TRACER, .prof_tag = "tracer3.srend", .pre = msh_pre, .st = &s_hb_msh,
};

static int on_ribbon(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_rib, &startp, &endp, &feed, &verify))
        return 0;
    s_last_predicted = 0;
    s_item_n = 0;
    const uint32_t fed_before = s_fed_tris;
    {
        if (endp > startp) tr_capture_prims(cpu, startp, endp);
        ngte_view view;
        ngte_view_build(cpu, &view);
        rib_pair_depths(cpu, &view);
        const int first = tr_hold();
        if (feed && render_path_enabled(RP_TRACER))
            tr_feed_native(&s_tick[first], s_tick_n - first);
    }
    tr_census("ribbon", startp, endp, fed_before);
    return hook_bracket_end(&k_rib);
}

static int on_mesh(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_msh, &startp, &endp, &feed, &verify))
        return 0;
    s_last_predicted = 0;
    s_item_n = 0;
    const uint32_t fed_before = s_fed_tris;
    if (endp > startp) {
        tr_capture_prims(cpu, startp, endp);
        msh_pair_depths(cpu, s_msh_desc);
        const int first = tr_hold();
        if (feed && render_path_enabled(RP_TRACER))
            tr_feed_native(&s_tick[first], s_tick_n - first);
    }
    tr_census("mesh", startp, endp, fed_before);
    return hook_bracket_end(&k_msh);
}

/* Sub-tick re-extract. The primitives are the guest's own output and are gone
 * once the polybuf is reused, so the replay re-feeds the set the canonical tick
 * captured, at the depths paired then. At alpha 1 it reproduces the canonical
 * present exactly, which is what the spine's identity leg measures. */
void tracers_reextract(CPUState* cpu) {
    (void)cpu;
    if (!hook_reextract_ready(RP_TRACER)) return;
    tr_feed_native(s_tick, s_tick_n);
}

/* The hook addresses are the layout's two tracer draw sites, bound at boot: a
 * static initializer cannot read another TU's const. A game with no located
 * routine leaves either 0 and the registry dispatches nothing for it. */
static feature_dispatch_entry_t tracers_dispatch[] = {
    { 0u, on_ribbon },
    { 0u, on_mesh },
    { 0u, NULL },
};

static void tracers_apply_options(const openpete_options_t* opts) {
    (void)opts;
    tracers_dispatch[0].addr = game_layout.tracers.ribbon_hook;
    tracers_dispatch[1].addr = game_layout.tracers.mesh_hook;
}

const feature_module_t tracers_module = {
    .name           = "tracers_native",
    .apply_options  = tracers_apply_options,
    .dispatch_table = tracers_dispatch,
};
