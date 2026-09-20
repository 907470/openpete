// games/common/renderer/zap.c, the feeder for Zoe's zap: a handwritten routine
// (Spyro 2 func_8004B194 / mobyfunc.s:2548-2905, Spyro 3 func_8002F5B4 /
// mobyfunc.s:2548-2936) that draws one ragged textured ribbon per live record
// of a pool. The records are spawned by the level overlays' Zoe update through
// the spawner beside it (Spyro 3 func_8002F540 / asm/mobyfunc.s:2511-2544,
// Spyro 2 func_8004B120 / mobyfunc.s:2510-2543), which is why the pool is empty
// everywhere Zoe is not zapping. The routine links every triangle into the 3D
// ordering table itself, so none of its primitives reaches the 2D chokepoint
// the sprite feeder hooks. Spyro
// 3's routine carries seven instructions Spyro 2's lacks, a game-state test
// that suppresses the per-present re-randomize; everything this feeder reads is
// in the 358 instructions the two share. Every per-game address, count, stride
// and bound comes from game_layout.zap; the walk below is one body.
//
// Read from the routine (asm/mobyfunc.s):
//   * the pool is 8 records of 0x30 bytes: the head world XYZ at +0x00, the
//     tail world XYZ at +0x0C, the primitive colour word at +0x18 and a packed
//     word at +0x1C holding the segment count in byte 0, the jitter shift in
//     byte 1 and a re-randomize flag in the upper halfword (:2588-2593). One
//     stored random halfword per interior point follows at +0x20. A zero count
//     byte is a free slot; the spawner fills the halfwords from rand and forces
//     the colour word's top byte to 0x26, POLY_FT3 semi-transparent
//     (:2512-2543).
//   * the camera matrix comes from G_CAMERA+0x00 with the GTE translation
//     registers zeroed (:2564-2578), so every MAC the routine computes is a
//     plain rotation of a camera-relative offset in full-world units.
//   * the record is dropped unless each component of the head's camera-relative
//     offset stays inside a +/-0x5400 box (:2608-2628); the tail is not tested.
//   * the routine lays down count+1 points from the head to the tail by
//     repeated addition of the integer-divided step (:2638-2648, :2652-2657),
//     displacing each interior point by the three signed 8-bit fields packed in
//     that point's stored random halfword, arithmetic-shifted right by the
//     jitter shift (:2658-2672). With the shift non-zero and the game not in
//     state 4 it draws a fresh rand, stores it back over the halfword and adds
//     its fields at half that scale as well (:2673-2685). Both displacements
//     are bounded by 192 world units against the 0x5400 box, so the depth this
//     feeder derives from the undisplaced chain is the routine's own chain to
//     within that bound.
//   * every point is then widened into a pair offset perpendicular to the local
//     path direction by about a sixteenth of the distance between its
//     neighbours (:2693-2745); the head and the tail are written as coincident
//     pairs instead (:2747-2766), which is why the first and last segment emit
//     one triangle and every interior segment two.
//   * each of the pair entries is RTPSed into the scratchpad as {packed screen
//     XY, SZ3 in the upper half and a four-bit outcode in the lower}
//     (:2773-2803); a record whose every entry shares an outcode bit lies
//     wholly off one screen edge and is dropped (:2804-2806).
//   * the record's depth key is the SZ3 of its middle point (:2821-2827): at
//     0x5000 and beyond the record is dropped, and from 0x4000 up a DPCS fades
//     the record colour toward black by (SZ3 - 0x4000) / 4096 (:2828-2836), so
//     the colour word this feeder reads back already carries that fade.
//   * per segment the four corner outcodes are ANDed and the segment skipped
//     when any bit survives (:2843-2848), then the ordering-table bucket is
//     ((sum of the four SZ3) >> 9) - 2 entries of eight bytes off the base
//     (:2849-2860), so both triangles of a segment sit at one depth.
//
// The geometry is the guest's own int16 output, read back out of the polybuf,
// not a second projection. What the primitives do not carry is depth - the PSX
// orders them by ordering-table bucket - so each segment's view-Z is read back
// out of the entry table the routine left in the scratchpad and paired to the
// primitives by replaying the routine's own emit loop over that table: its
// per-segment outcode AND, its four-corner SZ3 sum and its two coincidence
// tests, in its own order. Re-deriving the chain instead cannot reach the same
// verdicts, because each interior point carries a random displacement the
// routine draws and overwrites inside the call. The routine rewrites the table
// once per record it projects, so the replay is used only when exactly one
// record reached that stage and only when its triangle count equals the
// captured one; otherwise every primitive of the call is left unpaired and the
// census reports it.
//
// This is not a second branch of the gem-glint family in glint.c: that pool is
// 16 records of 0x10 bytes drawn as a screen-facing star of eight POLY_G4 over
// a halved camera offset, this one is 8 records of 0x30 bytes drawn as a
// textured POLY_FT3 strip between two world points in full-world units, and the
// two share no record field, no primitive class and no reject. What they do
// share - the hook bracket, the polybuf walk, the primitive decode and the
// projection helpers - is already shared and is reused here unchanged.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the random halfwords
// and the alternating jitter shift it stores into each record; this feeder only
// reads them afterwards.

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

#define ZAP (game_layout.zap)

#define ZAP_MAX_PRIMS 512
#define ZAP_MAX_SEG   128

typedef struct {
    ngte_prim p;
    float     depth;
    int       paired;
} zap_item;

static zap_item  s_item[ZAP_MAX_PRIMS];
static int      s_item_n;
static float    s_pred[ZAP_MAX_PRIMS];   /* one depth per predicted triangle */
static int      s_pred_n;
static int      s_live;                 /* records with a non-zero count byte */
static int      s_accepted;             /* 1 when the replay accounted for the call */
static int      s_reached;              /* records that reached the projection pass */
static uint32_t s_code_hist[256];
static uint32_t s_tag_hist[256];
static uint32_t s_fed_tris;

/* The tick's captured set, replayed by every sub-tick present of the tick. */
typedef struct { ngte_prim p; float depth; } zap_held;
static zap_held  s_tick[ZAP_MAX_PRIMS];
static int      s_tick_n;
static unsigned s_tick_frame = ~0u;

static int zap_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= ZAP_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 0.0f;
    s_item[s_item_n].paired = 0;
    s_item_n++;
    return 1;
}

/* Walk the interval the call allocated: 8-word POLY_FT3 (tag length 7). */
static void zap_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 7, .raw_words_max = 8, .guard = ZAP_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, zap_prim_sink, NULL);
}

/* Identify the record whose projected chain the scratchpad still holds. The
 * routine overwrites the entry table once per record it projects, so the
 * replay below is that record's only while exactly one record got that far:
 * the two tests before the projection are the count byte and the head's
 * +/-0x5400 box, and both read the record alone. Returns that record's segment
 * count, or 0 when none or more than one reached the projection. */
static int zap_projected_record(CPUState* cpu, const ngte_view* v) {
    int reached = 0, n_reached = 0;
    s_live = 0;
    const double box = (double)ZAP.box;
    for (uint32_t s = 0; s < ZAP.slots; s++) {
        const uint32_t rec = ZAP.pool + s * ZAP.stride;
        const int n = (int)(cpu->read_word(rec + ZAP.packed_off) & 0xFFu);
        if (n == 0) continue;                          /* :2590 free slot */
        s_live++;
        if (n < 1 || n > ZAP_MAX_SEG) return 0;
        /* :2608-2628 the head's camera-relative offset against the box, in the
         * integer staged camera the routine itself subtracts. */
        const double hx = (double)(int32_t)cpu->read_word(rec + ZAP.head_off + 0u) - v->cam_ram[0];
        const double hy = v->cam_ram[1] - (double)(int32_t)cpu->read_word(rec + ZAP.head_off + 4u);
        const double hz = v->cam_ram[2] - (double)(int32_t)cpu->read_word(rec + ZAP.head_off + 8u);
        if (fabs(hx) >= box || fabs(hy) >= box || fabs(hz) >= box) continue;
        reached++;
        n_reached = n;
    }
    s_reached = reached;
    return reached == 1 ? n_reached : 0;
}

/* Replay the routine's emit loop out of the entry table it just wrote, and
 * append one depth per triangle it emitted, in emit order.
 *
 * The table holds two entries of eight bytes per chain point, {packed screen
 * XY, SZ3 in the upper half and the four-bit outcode in the lower}, and the
 * routine reads four consecutive entries per segment (:2839-2848). Everything
 * the emit loop decides is in those words, so nothing here re-projects: the
 * per-segment outcode AND, the four-corner SZ3 sum the ordering-table bucket is
 * keyed on, and the two coincidence tests that drop one triangle of a segment
 * are the routine's own comparisons on its own words. Its projection pass stops
 * one entry short of the table it filled (:2884-2906 against :2757-2766), so
 * the last segment's fourth corner still carries the pre-projection word; the
 * replay reads it exactly as the routine does rather than substituting a
 * projected value, since the primitive the routine emitted is the one the
 * pairing has to explain.
 *
 * Returns the triangle count, or -1 when the record was dropped whole. */
static int zap_replay(CPUState* cpu, int n) {
    const uint32_t base = ZAP.spad;          /* :2771 the widened entry table */
    s_pred_n = 0;

    /* :2804-2806 the record is dropped when every projected entry shares an
     * outcode bit; the projection pass covers entries 0 to 2n. */
    int off = 0xF;
    for (int e = 0; e <= 2 * n; e++) off &= (int)(cpu->read_word(base + (uint32_t)e * 8u + 4u) & 0xFu);
    if (off != 0) return -1;
    /* :2821-2827 and the middle point's signed SZ3 halfword decides the far
     * reject; the routine addresses it as the halfword at offset 6 of the
     * table's midpoint. */
    const int mid_sz = (int)(int16_t)(uint16_t)(cpu->read_word(base + (uint32_t)n * 8u + 4u) >> 16);
    if (mid_sz - ZAP.far_sz >= 0) return -1;

    for (int j = 1; j <= n; j++) {
        uint32_t xy[4], w1[4];
        for (int k = 0; k < 4; k++) {
            const uint32_t e = base + (uint32_t)(2 * j - 2 + k) * 8u;
            xy[k] = cpu->read_word(e);
            w1[k] = cpu->read_word(e + 4u);
        }
        int oc = 0xF;
        for (int k = 0; k < 4; k++) oc &= (int)(w1[k] & 0xFu);
        if (oc != 0) continue;                         /* :2843-2848 */
        /* :2849-2860 the ordering-table index is the four corners' SZ3 sum
         * shifted down by nine and pulled two buckets nearer; a segment whose
         * index falls below the table's base is dropped, which is what takes
         * out a segment straddling the camera plane, where the projection
         * clamped its near corners' SZ3 to zero. */
        uint32_t sum = 0;
        for (int k = 0; k < 4; k++) sum += w1[k] >> 16;
        if ((int)(sum >> 9) - 2 < 0) continue;
        const float d = openpete_native_depth_from_viewz(
            openpete_worldvz_full((float)sum * 0.25f));
        /* :2863 the first triangle is dropped when this point's two entries
         * coincide, :2890 the second when the previous point's do. */
        if (xy[2] != xy[3]) { if (s_pred_n >= ZAP_MAX_PRIMS) return -1; s_pred[s_pred_n++] = d; }
        if (xy[0] != xy[1]) { if (s_pred_n >= ZAP_MAX_PRIMS) return -1; s_pred[s_pred_n++] = d; }
    }
    return s_pred_n;
}

/* Attach each captured primitive to the segment that emitted it: the replay
 * walks the routine's own entry table in the order the routine emitted from it,
 * so one cursor serves the call. The depths are taken only when the replay
 * accounts for exactly the primitives the polybuf carries. */
static void zap_pair_depths(CPUState* cpu, const ngte_view* v) {
    const int n = zap_projected_record(cpu, v);
    s_pred_n = 0;
    if (n <= 0) return;
    const int predicted = zap_replay(cpu, n);
    if (predicted != s_item_n) return;   /* every primitive stays unpaired */
    s_accepted = 1;
    for (int i = 0; i < s_item_n; i++) {
        s_item[i].depth = s_pred[i];
        s_item[i].paired = 1;
    }
}

/* ---- feed ---------------------------------------------------------------- */

/* OPENPETE_ZAP=0 suppresses this feed alone. The guest draw still runs, so
 * the A/B prices the ribbons' painted pixels from one binary. */
static int zap_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_ZAP"); v = !(e && *e && *e == '0'); }
    return v;
}

static void zap_add_prim(const ngte_prim* p, float depth) {
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
        if (p->nv == 3) break;            /* a triangle is one scene triangle */
    }
}

static void zap_feed_set(const zap_held* set, int n) {
    if (!zap_feed_enabled()) return;
    if (n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_TRACER(0));
    for (int i = 0; i < n; i++) zap_add_prim(&set[i].p, set[i].depth);
    openpete_native_scene_set_tag(0);
}

/* Hold the call's primitives for the sub-tick replay. The tick's first hook
 * clears the set; this routine is called once per tick. */
static void zap_hold(void) {
    if (s_tick_frame != openpete_frame) { s_tick_frame = openpete_frame; s_tick_n = 0; }
    for (int i = 0; i < s_item_n && s_tick_n < ZAP_MAX_PRIMS; i++) {
        s_tick[s_tick_n].p = s_item[i].p;
        s_tick[s_tick_n].depth = s_item[i].depth;
        s_tick_n++;
    }
}

/* OPENPETE_ZAP_CENSUS=LO-HI (or a single frame): per call in that frame
 * range, the primitives the routine appended by GPU code, the pool records that
 * were live and that the reject mirror accepted, the triangles the segment walk
 * predicted, the native triangles this feeder emitted and how many primitives
 * were left unpaired. A capture whose fed count is zero measured nothing. */
static void zap_census(uint32_t start, uint32_t end, uint32_t fed_before) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_ZAP_CENSUS", openpete_frame)) return;
    int unpaired = 0;
    for (int i = 0; i < s_item_n; i++) if (!s_item[i].paired) unpaired++;
    fprintf(stderr, "[zap-census f=%u] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            openpete_frame, start, end, end - start, s_item_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_item_n; i++) seen[s_item[i].p.code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    fprintf(stderr, " live=%d reached=%d accepted=%d predicted=%d fed_tris=%u unpaired=%d\n",
            s_live, s_reached, s_accepted, s_pred_n, s_fed_tris - fed_before, unpaired);
    /* Each triangle's first corner and the scene depth the pairing gave it:
     * the two quantities the feed places it by. */
    for (int i = 0; i < s_item_n; i++)
        fprintf(stderr, "[zap-tri f=%u] %d a=(%.0f,%.0f) b=(%.0f,%.0f) c=(%.0f,%.0f)"
                        " rgb=%06X uv=(%d,%d) tp=%d depth=%.6f\n",
                openpete_frame, i, s_item[i].p.x[0], s_item[i].p.y[0],
                s_item[i].p.x[1], s_item[i].p.y[1], s_item[i].p.x[2], s_item[i].p.y[2],
                s_item[i].p.rgb[0] & 0xFFFFFFu, s_item[i].p.u[0], s_item[i].p.v[0],
                s_item[i].p.tpage, s_item[i].depth);
}

/* ---- hook ---------------------------------------------------------------- */

static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_TRACER, .prof_tag = "zap.srend", .st = &s_hb,
};

/* The world draw's call is the only one in either image, so the hook takes
 * every entry (Spyro 2 draw/func_800155A0.s:34, Spyro 3 draw/func_8001E460.s:32). */
static int on_zap_draw(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    s_item_n = 0; s_pred_n = 0; s_live = 0; s_reached = 0; s_accepted = 0;
    const uint32_t fed_before = s_fed_tris;
    if (s_tick_frame != openpete_frame) { s_tick_frame = openpete_frame; s_tick_n = 0; }
    if (endp > startp) {
        zap_capture_prims(cpu, startp, endp);
        ngte_view view;
        ngte_view_build(cpu, &view);
        zap_pair_depths(cpu, &view);
        zap_hold();
        if (feed && render_path_enabled(RP_TRACER)) zap_feed_set(s_tick, s_tick_n);
    }
    zap_census(startp, endp, fed_before);
    return hook_bracket_end(&k_bracket);
}

/* Sub-tick re-extract. The primitives are the guest's own output and are gone
 * once the polybuf is reused, so the replay re-feeds the set the canonical tick
 * captured, at the depths paired then. At alpha 1 it reproduces the canonical
 * present exactly, which is what the spine's identity leg measures. */
void zap_reextract(CPUState* cpu) {
    (void)cpu;
    if (!hook_reextract_ready(RP_TRACER)) return;
    zap_feed_set(s_tick, s_tick_n);
}

/* The hook address is the layout's zap draw site, bound at boot: a
 * static initializer cannot read another TU's const. A game with no located
 * routine leaves it 0 and the registry dispatches nothing. */
static feature_dispatch_entry_t zap_dispatch[] = {
    { 0u, on_zap_draw },
    { 0u, NULL },
};

static void zap_apply_options(const openpete_options_t* opts) {
    (void)opts;
    zap_dispatch[0].addr = game_layout.zap.hook;
}

const feature_module_t zap_module = {
    .name           = "zap_native",
    .apply_options  = zap_apply_options,
    .dispatch_table = zap_dispatch,
};
