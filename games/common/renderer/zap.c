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

#include "zap.h"
#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/env_knob.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_interp_objs.h"   /* the per-record prev/curr pool snapshots */
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

/* Where a triangle corner's chain point projects: the midpoint of the two
 * entries the routine widened that point into, which is the point the corner
 * is offset from by the ribbon's half-width. `ok` is 0 for a corner of the
 * head or the tail, which the routine writes as a coincident pair and so
 * carries no width. */
typedef struct {
    float   x[3], y[3];
    uint8_t ok[3];
    uint8_t pt[3];   /* the chain point's index, 0 at the head */
} zap_pivot;

typedef struct {
    ngte_prim p;
    float     depth;
    int       paired;
    zap_pivot pv;
} zap_item;

static zap_item  s_item[ZAP_MAX_PRIMS];
static int      s_item_n;
static float    s_pred[ZAP_MAX_PRIMS];   /* one depth per predicted triangle */
static zap_pivot s_pred_pv[ZAP_MAX_PRIMS]; /* its corners' chain points */
static int      s_pred_n;
static int      s_live;                 /* records with a non-zero count byte */
static int      s_accepted;             /* 1 when the replay accounted for the call */
static int      s_reached;              /* records that reached the projection pass */
static uint32_t s_code_hist[256];
static uint32_t s_tag_hist[256];
static uint32_t s_fed_tris;

/* The record the pairing accepted, and where the routine's own projection puts
 * its head and tail on screen at the tick. The ribbon runs from one to the
 * other, so these two points are what a sub-tick present has to move it by.
 * `s_rec` is 0 when the call paired no record. */
static uint32_t s_rec;
static uint32_t s_head_w[3], s_tail_w[3];
static ngte_proj s_head_p, s_tail_p;
static int      s_seg_n;                     /* the record's segment count */
static double   s_hw_tick[ZAP_MAX_SEG + 1];  /* each chain point's screen half-width */

/* The sub-tick rebase: each end's screen displacement and re-projected view-Z.
 * Inactive on a canonical feed and at the end of the tick's own interval,
 * where the feed must reproduce the canonical primitive bit for bit. */
static struct {
    double dhx, dhy, dtx, dty;   /* head and tail screen displacement */
    double head_m3, tail_m3;     /* their re-projected view-Z */
    double hw_scale[ZAP_MAX_SEG + 1];  /* each chain point's half-width, shown over the tick's */
    int    active;
} s_reb;

/* The tick's captured set, replayed by every sub-tick present of the tick. */
typedef struct { ngte_prim p; float depth; zap_pivot pv; } zap_held;
static zap_held  s_tick[ZAP_MAX_PRIMS];
static int      s_tick_n;
static unsigned s_tick_frame = ~0u;

/** @brief Stores one decoded primitive in s_item as an unpaired item at the near-guard depth 1.
 *
 * @param ctx Unused.
 * @param s The decoded primitive.
 * @return 1 when stored, 0 when s_item holds ZAP_MAX_PRIMS items. */
static int zap_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= ZAP_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 1.0f;   /* unpaired: the near guard (view-Z 1) */
    s_item[s_item_n].paired = 0;
    memset(&s_item[s_item_n].pv, 0, sizeof s_item[0].pv);
    s_item_n++;
    return 1;
}

/** @brief Decodes every primitive in the polybuf interval the zap routine's call allocated into s_item.
 *
 * The routine emits 8-word POLY_FT3 (tag length 7); the walk admits tags up
 * to that length, up to ZAP_MAX_PRIMS primitives, and fills the code and tag
 * histograms. Every captured primitive starts unpaired at depth 1.
 * @param cpu The guest CPU state.
 * @param start The polybuf cursor before the call.
 * @param end The polybuf cursor after the call. */
static void zap_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 7, .raw_words_max = 8, .guard = ZAP_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, zap_prim_sink, NULL);
}

/** @brief Identifies the record whose projected chain the scratchpad entry table still holds.
 *
 * The routine overwrites the entry table once per record it projects, so the
 * replay is that record's only while exactly one record got that far: the two
 * tests before the projection are the count byte and the head's +/-0x5400
 * box, and both read the record alone. Sets s_live to the records with a
 * non-zero count byte and s_reached to those inside the box. A live record
 * whose count exceeds ZAP_MAX_SEG aborts the search.
 * @param cpu The guest CPU state.
 * @param v The view whose staged camera the box test subtracts.
 * @param out_rec Receives the record's address when exactly one reached the projection.
 * @return That record's segment count, or 0 when none or more than one reached the projection. */
static int zap_projected_record(CPUState* cpu, const ngte_view* v, uint32_t* out_rec) {
    int reached = 0, n_reached = 0;
    uint32_t rec_reached = 0u;
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
        rec_reached = rec;
    }
    s_reached = reached;
    if (reached != 1) return 0;
    *out_rec = rec_reached;
    return n_reached;
}

/** @brief Reads a record's head and tail world points.
 *
 * The head is at ZAP.head_off and the tail twelve bytes on, both three
 * signed words in full world units, returned as raw words.
 * @param cpu The guest CPU state.
 * @param rec The record's address.
 * @param head_w Receives the head's three words.
 * @param tail_w Receives the tail's three words. */
static void zap_rec_points(CPUState* cpu, uint32_t rec,
                           uint32_t head_w[3], uint32_t tail_w[3]) {
    for (int k = 0; k < 3; k++) {
        head_w[k] = cpu->read_word(rec + ZAP.head_off + (uint32_t)k * 4u);
        tail_w[k] = cpu->read_word(rec + ZAP.head_off + 12u + (uint32_t)k * 4u);
    }
}

/** @brief Projects one record world point through the routine's own arithmetic.
 *
 * The camera-relative offset the routine subtracts (:2564-2578 zero the GTE
 * translation, so every MAC is a plain rotation of a camera-relative offset
 * in full world units) is rotated and divided by ngte_project_offset.
 * @param v The view to project under.
 * @param w The point's three world words.
 * @param off A displacement in full world units, or NULL for none.
 * @param o Receives the projection. */
static void zap_project_point(const ngte_view* v, const uint32_t w[3],
                              const double off[3], ngte_proj* o) {
    double q[3];
    for (int k = 0; k < 3; k++) q[k] = (double)(int32_t)w[k] + (off ? off[k] : 0.0);
    const double rel[3] = { q[0] - v->cam[0], v->cam[1] - q[1], v->cam[2] - q[2] };
    ngte_project_offset(v, rel, o);
}

/** @brief Computes the screen half-width of chain point i of n with the head and tail world points displaced.
 *
 * The routine lays the undisplaced point at head + i * step (:2638-2657) and
 * widens it in camera-relative world units, before the projection, from the
 * difference d between its two neighbours (:2693-2745): a vertical offset
 * of max(|dx|, |dy|) + min(|dx|, |dy|) / 2 and a horizontal offset of |dz|
 * along world X where |dx| < |dy| and along world Y otherwise, both shifted
 * right by four, the pair's two entries sitting at the point plus and minus
 * that vector. The neighbours are jittered points (:2658-2685), so the world
 * offset is the tick's and is held: it is built from the tick's undisplaced
 * head-to-tail vector, of which the step is a fixed fraction, and only the
 * point it is carried on is displaced. What varies is the offset's
 * projection, its view-Z and its orientation to the camera.
 * @param v The view to project under.
 * @param oh The head's displacement in full world units.
 * @param ot The tail's displacement in full world units.
 * @param i The chain point's index, 0 at the head.
 * @param n The segment count.
 * @return Half the screen distance between the two entries' projections, or
 *         0 when either lies behind the camera. */
static double zap_half_width(const ngte_view* v, const double oh[3], const double ot[3],
                             int i, int n) {
    double d[3], p[3], o[3], q[3];
    const double f = (double)i / (double)n;
    for (int k = 0; k < 3; k++) {
        const double h = (double)(int32_t)s_head_w[k];
        d[k] = (double)(int32_t)s_tail_w[k] - h;
        p[k] = h + oh[k] + (d[k] + ot[k] - oh[k]) * f;
    }
    /* The neighbours' difference is two steps of d / n. */
    const double sc = 2.0 / (double)n / 16.0;
    const double ax = fabs(d[0]) * sc, ay = fabs(d[1]) * sc, az = fabs(d[2]) * sc;
    o[0] = (ax < ay) ? az : 0.0;
    o[1] = (ax < ay) ? 0.0 : -az;
    o[2] = -(fmax(ax, ay) + fmin(ax, ay) * 0.5);
    ngte_proj a, b;
    for (int k = 0; k < 3; k++) q[k] = p[k] + o[k];
    const double ra[3] = { q[0] - v->cam[0], v->cam[1] - q[1], v->cam[2] - q[2] };
    for (int k = 0; k < 3; k++) q[k] = p[k] - o[k];
    const double rb[3] = { q[0] - v->cam[0], v->cam[1] - q[1], v->cam[2] - q[2] };
    ngte_project_offset(v, ra, &a);
    ngte_project_offset(v, rb, &b);
    if (!(a.m3 > 0.0) || !(b.m3 > 0.0)) return 0.0;
    return 0.5 * hypot(a.sx - b.sx, a.sy - b.sy);
}

/* The band is the discontinuity guard ZAP_REUSE_CEILING gives the
 * displacement: a half-width that changed by more than a factor of four
 * inside one tick is a camera cut or a slot reuse, where the replay holds the
 * tick's width as every other term of the rebase does. */
#define ZAP_SCALE_MIN 0.25
#define ZAP_SCALE_MAX 4.0

/** @brief Reports whether the sub-tick replay scales the ribbon's width, latched from OPENPETE_ZAP_WIDTH on first call.
 *
 * OPENPETE_ZAP_WIDTH=0 holds the tick's width on the replay, the engagement
 * leg of the sub-tick width harness; any other value or none enables it.
 * @return 1 when enabled, else 0. */
static int zap_width_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_ZAP_WIDTH"); v = !(e && *e == '0'); }
    return v;
}

/** @brief Computes each interior chain point's half-width scale into s_reb.hw_scale.
 *
 * The scale is the half-width this present shows over the one the tick drew,
 * both from zap_half_width. The head, the tail, a point whose tick half-width
 * is not positive, and a scale outside [ZAP_SCALE_MIN, ZAP_SCALE_MAX] (which
 * covers a point projecting behind the camera, whose half-width is 0) hold
 * the tick's width at scale 1, as does every point when zap_width_enabled is 0.
 * @param v The view this present shows.
 * @param oh The head's displacement in full world units.
 * @param ot The tail's displacement in full world units. */
static void zap_width_scales(const ngte_view* v, const double oh[3], const double ot[3]) {
    const int on = zap_width_enabled();
    for (int i = 0; i <= s_seg_n; i++) {
        s_reb.hw_scale[i] = 1.0;
        if (!on || i == 0 || i == s_seg_n || !(s_hw_tick[i] > 0.0)) continue;
        const double scl = zap_half_width(v, oh, ot, i, s_seg_n) / s_hw_tick[i];
        if (scl >= ZAP_SCALE_MIN && scl <= ZAP_SCALE_MAX) s_reb.hw_scale[i] = scl;
    }
}

/** @brief Computes the midpoint of two packed screen-XY entries.
 *
 * @param a The first entry (x in the low signed halfword, y in the high).
 * @param b The second entry.
 * @param x Receives the midpoint's x.
 * @param y Receives the midpoint's y. */
static void zap_entry_mid(uint32_t a, uint32_t b, float* x, float* y) {
    *x = ((float)(int16_t)(a & 0xFFFFu) + (float)(int16_t)(b & 0xFFFFu)) * 0.5f;
    *y = ((float)(int16_t)(a >> 16) + (float)(int16_t)(b >> 16)) * 0.5f;
}

/** @brief Appends one predicted triangle of segment j with its depth and its corners' chain points.
 *
 * Segment j joins points j-1 and j of n. Its four entries are point j-1's
 * pair (0, 1) and point j's pair (2, 3); the first triangle's corners are
 * entries 0, 2, 3 and the second's 0, 1, 3 (the stores after :2863 and
 * :2890), so each corner's pivot is the midpoint of its own point's pair.
 * Points 0 and n are the coincident head and tail pairs (:2747-2766); the
 * tail's second entry is also the one the projection pass leaves
 * unprojected, so neither end's corners take a pivot.
 * @param depth The segment's scene depth.
 * @param xy The segment's four packed screen-XY entries.
 * @param second 0 for the first triangle, 1 for the second.
 * @param j The segment index, 1 to n.
 * @param n The record's segment count.
 * @return 1 when appended, 0 when the table is full. */
static int zap_replay_emit(float depth, const uint32_t xy[4], int second, int j, int n) {
    if (s_pred_n >= ZAP_MAX_PRIMS) return 0;
    static const int ent[2][3] = { {0, 2, 3}, {0, 1, 3} };
    zap_pivot* pv = &s_pred_pv[s_pred_n];
    for (int k = 0; k < 3; k++) {
        const int e = ent[second][k];
        const int point = (e < 2) ? j - 1 : j;
        pv->ok[k] = (uint8_t)(point > 0 && point < n);
        pv->pt[k] = (uint8_t)point;
        const int pair = (e < 2) ? 0 : 2;
        zap_entry_mid(xy[pair], xy[pair + 1], &pv->x[k], &pv->y[k]);
    }
    s_pred[s_pred_n++] = depth;
    return 1;
}

/** @brief Replays the routine's emit loop out of the entry table it just wrote, appending one depth per emitted triangle in emit order.
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
 * @param cpu The guest CPU state.
 * @param n The record's segment count.
 * @return The triangle count, or -1 when the record was dropped whole or the
 *         prediction table filled. */
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
        const float d = openpete_native_scene_viewz(
            openpete_worldvz_full((float)sum * 0.25f));
        /* :2863 the first triangle is dropped when this point's two entries
         * coincide, :2890 the second when the previous point's do. */
        if (xy[2] != xy[3] && !zap_replay_emit(d, xy, 0, j, n)) return -1;
        if (xy[0] != xy[1] && !zap_replay_emit(d, xy, 1, j, n)) return -1;
    }
    return s_pred_n;
}

/** @brief Attaches each captured primitive to the segment that emitted it.
 *
 * The replay walks the routine's own entry table in the order the routine
 * emitted from it, so one cursor serves the call. Before the replay it
 * records the projected record's head and tail, their tick projections and
 * every chain point's tick half-width for the sub-tick rebase. The depths
 * and pivots are taken, and s_accepted set, only when the replay accounts
 * for exactly the primitives the polybuf carries.
 * @param cpu The guest CPU state.
 * @param v The tick's view. */
static void zap_pair_depths(CPUState* cpu, const ngte_view* v) {
    uint32_t rec = 0u;
    const int n = zap_projected_record(cpu, v, &rec);
    s_pred_n = 0;
    s_rec = 0u;
    if (n <= 0) return;
    zap_rec_points(cpu, rec, s_head_w, s_tail_w);
    zap_project_point(v, s_head_w, NULL, &s_head_p);
    zap_project_point(v, s_tail_w, NULL, &s_tail_p);
    s_rec = rec;
    s_seg_n = n;
    static const double k_zero[3] = { 0.0, 0.0, 0.0 };
    for (int i = 0; i <= n; i++) s_hw_tick[i] = zap_half_width(v, k_zero, k_zero, i, n);
    const int predicted = zap_replay(cpu, n);
    if (predicted != s_item_n) return;   /* every primitive stays unpaired */
    s_accepted = 1;
    for (int i = 0; i < s_item_n; i++) {
        s_item[i].depth = s_pred[i];
        s_item[i].paired = 1;
        s_item[i].pv = s_pred_pv[i];
    }
}

/* ---- feed ---------------------------------------------------------------- */

/** @brief Reports whether the zap feed is enabled, latched from OPENPETE_ZAP on first call.
 *
 * OPENPETE_ZAP=0 suppresses this feed alone. The guest draw still runs, so
 * an A/B prices the ribbons' painted pixels from one binary.
 * @return 0 when OPENPETE_ZAP starts with '0', else 1. */
static int zap_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_ZAP"); v = !(e && *e && *e == '0'); }
    return v;
}

/** @brief Computes where a corner sits along the ribbon, as its parameter on the tick's head-to-tail screen segment.
 *
 * The corner is projected onto the segment from s_head_p to s_tail_p and the
 * parameter clamped to 0..1. The chain the routine lays down runs from the
 * head to the tail by repeated addition of one step (:2638-2657), so this
 * parameter is that chain's own, exact at the two ends and linear between
 * them.
 * @param x The corner's screen x.
 * @param y The corner's screen y.
 * @return The parameter in 0..1; 0 when the two ends project within 1e-9
 *         squared pixels of each other, so the corner takes the head's
 *         displacement whole. */
static double zap_corner_pos(float x, float y) {
    const double ax = s_head_p.sx, ay = s_head_p.sy;
    const double dx = s_tail_p.sx - ax, dy = s_tail_p.sy - ay;
    const double n = dx * dx + dy * dy;
    if (n < 1e-9) return 0.0;
    const double t = (((double)x - ax) * dx + ((double)y - ay) * dy) / n;
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

/** @brief Scales one widened corner's offset from its chain point by that point's half-width scale.
 *
 * The chain point then moves with the corner through zap_rebase_xy. A corner
 * without a pivot, an inactive rebase, or a scale of 1 leaves it untouched.
 * @param x The corner's screen x, updated in place.
 * @param y The corner's screen y, updated in place.
 * @param pv The triangle's corner pivots.
 * @param k The corner index. */
static void zap_widen_xy(float* x, float* y, const zap_pivot* pv, int k) {
    if (!s_reb.active || !pv->ok[k]) return;
    const double scl = s_reb.hw_scale[pv->pt[k]];
    if (scl == 1.0) return;
    *x = (float)((double)pv->x[k] + ((double)*x - (double)pv->x[k]) * scl);
    *y = (float)((double)pv->y[k] + ((double)*y - (double)pv->y[k]) * scl);
}

/** @brief Moves one corner by the two ribbon ends' screen displacement blended at its own position along the ribbon.
 *
 * The rebase is skipped when inactive, so the canonical feed emits the
 * captured corner untouched; the position is reported either way.
 * @param x The corner's screen x, updated in place.
 * @param y The corner's screen y, updated in place.
 * @param pos Receives the corner's position along the ribbon in [0, 1], or NULL. */
static void zap_rebase_xy(float* x, float* y, double* pos) {
    const double t = zap_corner_pos(*x, *y);
    if (pos) *pos = t;
    if (!s_reb.active) return;
    *x = (float)((double)*x + s_reb.dhx + (s_reb.dtx - s_reb.dhx) * t);
    *y = (float)((double)*y + s_reb.dhy + (s_reb.dty - s_reb.dhy) * t);
}

/* The fed ribbon's half-width: the mean distance of every widened corner from
 * its own chain point, both as the feed placed them, measured across the
 * head-to-tail line the feed drew the ribbon along so that the ribbon's
 * stretch along its own length does not enter it. Accumulated per present for
 * OPENPETE_ZAP_TRACE. */
static double s_hw_sum;
static int    s_hw_n;

/** @brief Adds one widened corner's distance from its chain point to the fed half-width mean.
 *
 * The distance is measured perpendicular to the head-to-tail line the feed
 * drew, with the chain point rebased like the corner. A corner without a
 * pivot, or a ribbon of zero length, adds nothing.
 * @param cx The fed corner's screen x.
 * @param cy The fed corner's screen y.
 * @param pv The triangle's corner pivots.
 * @param k The corner index, 0 to 2. */
static void zap_width_note(float cx, float cy, const zap_pivot* pv, int k) {
    if (!pv->ok[k]) return;
    float mx = pv->x[k], my = pv->y[k];
    zap_rebase_xy(&mx, &my, NULL);
    const double ux = (s_tail_p.sx + s_reb.dtx) - (s_head_p.sx + s_reb.dhx);
    const double uy = (s_tail_p.sy + s_reb.dty) - (s_head_p.sy + s_reb.dhy);
    const double len = hypot(ux, uy);
    if (!(len > 0.0)) return;
    s_hw_sum += fabs(((double)cx - (double)mx) * uy - ((double)cy - (double)my) * ux) / len;
    s_hw_n++;
}

/** @brief Feeds one held ribbon primitive to the native scene as one or two world triangles.
 *
 * The corners are widened and rebased first. The depth is the held pairing
 * depth, or with the rebase active the view-Z blended between the head and
 * tail at the mean position of the corners. A quad splits into corners
 * {0,1,2} and {1,3,2}; a triangle feeds once. A primitive that is neither
 * is skipped.
 * @param h The held primitive with its depth and pivots. */
static void zap_add_prim(const zap_held* h) {
    static const int split[2][3] = { {0, 1, 2}, {1, 3, 2} };
    const ngte_prim* p = &h->p;
    float depth = h->depth;
    if (p->nv != 3 && p->nv != 4) return;
    /* Rebase the corners first: the depth the triangle takes is the view-Z of
     * its own mean position along the ribbon, which is the same quantity the
     * routine keys its ordering-table bucket on (:2849-2860, the four corners'
     * mean SZ3). */
    float px[4], py[4];
    double pos_sum = 0.0;
    for (int k = 0; k < p->nv; k++) {
        px[k] = p->x[k]; py[k] = p->y[k];
        double t = 0.0;
        if (k < 3) zap_widen_xy(&px[k], &py[k], &h->pv, k);
        zap_rebase_xy(&px[k], &py[k], &t);
        pos_sum += t;
        if (k < 3) zap_width_note(px[k], py[k], &h->pv, k);
    }
    if (s_reb.active) {
        const double tm = pos_sum / (double)p->nv;
        const double vz = s_reb.head_m3 + (s_reb.tail_m3 - s_reb.head_m3) * tm;
        depth = openpete_native_scene_viewz(openpete_worldvz_full((float)vz));
    }
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
            st.v[k].sx = px[j]; st.v[k].sy = py[j]; st.v[k].z = depth;
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
        if (p->nv == 3) break;            /* a triangle is one scene triangle */
    }
}

/** @brief Feeds a held set of ribbon primitives to the native scene under the tracer tag.
 *
 * A no-op when OPENPETE_ZAP is 0 or n is not positive. Opens the frame's
 * scene and resets the fed half-width accumulators first.
 * @param set The held primitives.
 * @param n The count of primitives. */
static void zap_feed_set(const zap_held* set, int n) {
    if (!zap_feed_enabled()) return;
    if (n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_TRACER(0));
    s_hw_sum = 0.0; s_hw_n = 0;
    for (int i = 0; i < n; i++) zap_add_prim(&set[i]);
    openpete_native_scene_set_tag(0);
}

/** @brief Prints one OPENPETE_ZAP_TRACE line for a present that fed this producer.
 *
 * Enabled by OPENPETE_ZAP_TRACE set to a value other than 0, latched on first
 * call. The line carries the live record count, the accepted record, the
 * screen points the feed placed the ribbon's two ends at and the mean fed
 * width. On a canonical present those points are the routine's own
 * projections of the record's head and tail; on a sub-tick present they are
 * those two points moved by the rebase. A present whose live count is zero
 * fed nothing and measured nothing.
 * @param leg The present's leg name ("canon" or "inter").
 * @param alpha The present's sub-tick alpha. */
static void zap_trace(const char* leg, double alpha) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_ZAP_TRACE"); on = (e && *e && *e != '0'); }
    if (!on) return;
    fprintf(stderr, "[zap-screen vbl=%u leg=%s alpha=%.3f] live=%d rec=%08X "
                    "head=(%.2f,%.2f) tail=(%.2f,%.2f) width=%.3f wn=%d\n",
            openpete_frame, leg, alpha, s_live, s_rec,
            s_head_p.sx + s_reb.dhx, s_head_p.sy + s_reb.dhy,
            s_tail_p.sx + s_reb.dtx, s_tail_p.sy + s_reb.dty,
            s_hw_n ? 2.0 * s_hw_sum / (double)s_hw_n : 0.0, s_hw_n);
}

/** @brief Appends the call's captured primitives to the held set the sub-tick replay re-feeds.
 *
 * The held set is cleared when openpete_frame differs from the frame it was
 * filled on, and holds at most ZAP_MAX_PRIMS primitives. */
static void zap_hold(void) {
    if (s_tick_frame != openpete_frame) { s_tick_frame = openpete_frame; s_tick_n = 0; }
    for (int i = 0; i < s_item_n && s_tick_n < ZAP_MAX_PRIMS; i++) {
        s_tick[s_tick_n].p = s_item[i].p;
        s_tick[s_tick_n].depth = s_item[i].depth;
        s_tick[s_tick_n].pv = s_item[i].pv;
        s_tick_n++;
    }
}

/** @brief Prints the per-call zap census for VBLs inside the OPENPETE_ZAP_CENSUS range.
 *
 * OPENPETE_ZAP_CENSUS=LO-HI, or a single VBL. One line per call: the
 * polybuf interval, the primitives captured by GPU code, the live records,
 * the records that reached the projection, whether the replay was accepted,
 * the triangles it predicted, the native triangles fed since fed_before and
 * the primitives left unpaired. Then one line per primitive with its three
 * corners, colour, first UV, page and paired depth. A capture whose fed
 * count is zero measured nothing.
 * @param start The polybuf cursor before the call.
 * @param end The polybuf cursor after the call.
 * @param fed_before s_fed_tris before the call. */
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

/** @brief Hooks the zap routine: runs it once through the shared bracket, then captures, pairs, holds and feeds its primitives.
 *
 * The world draw's call is the only one in either image, so the hook takes
 * every entry (Spyro 2 draw/func_800155A0.s:34, Spyro 3
 * draw/func_8001E460.s:32). The polybuf interval the call allocated is
 * decoded, paired to the replayed segment depths, and held for the tick's
 * sub-tick replay; with the native scene live and RP_TRACER enabled the held
 * set is fed unrebased. The census runs on every bracketed call.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the bracket declines (the ordinary dispatch runs the
 *         routine), else hook_bracket_end's result. */
static int on_zap_draw(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    s_item_n = 0; s_pred_n = 0; s_live = 0; s_reached = 0; s_accepted = 0;
    memset(&s_reb, 0, sizeof s_reb);   /* the canonical feed emits the captured corners */
    const uint32_t fed_before = s_fed_tris;
    if (s_tick_frame != openpete_frame) { s_tick_frame = openpete_frame; s_tick_n = 0; }
    if (endp > startp) {
        zap_capture_prims(cpu, startp, endp);
        ngte_view view;
        ngte_view_build(cpu, &view);
        zap_pair_depths(cpu, &view);
        zap_hold();
        if (feed && render_path_enabled(RP_TRACER)) {
            zap_feed_set(s_tick, s_tick_n);
            zap_trace("canon", 1.0);
        }
    }
    zap_census(startp, endp, fed_before);
    return hook_bracket_end(&k_bracket);
}

/* ---- sub-tick rebase ------------------------------------------------------
 *
 * The triangles are the guest's own int16 output, built under the tick's
 * camera from the tick's record, so a sub-tick present that re-feeds them
 * draws the ribbon where the tick drew it: neither the camera's motion nor the
 * record's own reaches it. The rebase measures each end's screen displacement
 * as the difference of two projections - the world point the present shows
 * under the staged camera, against the point the tick drew under the tick's
 * camera - and moves every corner by the pair blended at its own position
 * along the chain. At the end of the tick's interval the displaced points and
 * the staged camera are the tick's, so both projections take identical inputs
 * through identical arithmetic and every term is exactly zero; the replay then
 * emits the captured corners verbatim instead, which makes that exactness
 * structural rather than a matter of float agreement. */

/* A beam end's speed ceiling, in full world units per tick. The table is keyed
 * by slot address, so a slot a new record respawns into inside one tick pairs
 * across two different beams and would lerp as a sweep between them. The
 * segment-count test below catches a reuse that changed the chain's length;
 * this catches the rest, at a distance no end covers in a tick while staying
 * inside the +/-0x5400 box the routine admits it from. */
#define ZAP_REUSE_CEILING 8192.0

/* A pair spanning more than this is a discontinuity rather than motion. */
#define ZAP_PAIR_STALE_NS (g_openpete_tick_period_ns + g_openpete_tick_period_ns / 2)

/** @brief Computes the record's head and tail displacement between the pose the tick drew and the pose this present shows.
 *
 * The displacement is (previous - current) * (1 - alpha) in full world units,
 * read from the interpolation snapshot pair keyed by the record's address.
 * With no usable previous record (a beam that spawned this tick, a pair
 * spanning more than ZAP_PAIR_STALE_NS, a changed segment count, or an end
 * that moved more than ZAP_REUSE_CEILING) both offsets are zero, and the
 * ribbon holds its tick pose in the world with only the camera's motion
 * rebasing it.
 * @param alpha The present's sub-tick alpha.
 * @param off_head Receives the head's displacement.
 * @param off_tail Receives the tail's displacement.
 * @return 1 when the displacement came from a usable pair, else 0. */
static int zap_record_pair(double alpha, double off_head[3], double off_tail[3]) {
    for (int k = 0; k < 3; k++) off_head[k] = off_tail[k] = 0.0;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(s_rec);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    if ((uint32_t)o->blob_len < ZAP.head_off + 24u ||
        (uint32_t)o->blob_len < ZAP.packed_off + 4u) return 0;
    if (o->curr_ns - o->prev_ns > ZAP_PAIR_STALE_NS) return 0;
    uint32_t pp, cp;
    memcpy(&pp, o->prev + ZAP.packed_off, 4);
    memcpy(&cp, o->curr + ZAP.packed_off, 4);
    if ((pp & 0xFFu) != (cp & 0xFFu)) return 0;      /* the chain changed length */
    int32_t ph[3], pt[3];
    memcpy(ph, o->prev + ZAP.head_off, 12);
    memcpy(pt, o->prev + ZAP.head_off + 12u, 12);
    const double rest = 1.0 - alpha;
    double sq_h = 0.0, sq_t = 0.0;
    for (int k = 0; k < 3; k++) {
        const double dh = (double)ph[k] - (double)(int32_t)s_head_w[k];
        const double dt = (double)pt[k] - (double)(int32_t)s_tail_w[k];
        sq_h += dh * dh; sq_t += dt * dt;
        off_head[k] = dh * rest;
        off_tail[k] = dt * rest;
    }
    if (sq_h > ZAP_REUSE_CEILING * ZAP_REUSE_CEILING ||
        sq_t > ZAP_REUSE_CEILING * ZAP_REUSE_CEILING) {
        for (int k = 0; k < 3; k++) off_head[k] = off_tail[k] = 0.0;
        return 0;
    }
    return 1;
}

/** @brief Rebases the held set onto the pose this present shows.
 *
 * The staged camera resolves through the sub-tick view, so building a view
 * here reads the interpolated transform the rest of the pass draws under.
 * The rebase stays inactive with no accepted record, an empty held set, or
 * alpha at or above 1.
 * @param cpu The guest CPU state.
 * @param alpha The present's sub-tick alpha. */
static void zap_rebase_all(CPUState* cpu, double alpha) {
    memset(&s_reb, 0, sizeof s_reb);
    if (!s_rec || s_tick_n <= 0) return;
    /* At the end of the tick's own interval the replay must reproduce the
     * canonical present exactly, so it emits the captured corners and the
     * paired depths verbatim rather than a re-projection that agrees with them
     * only to the last bit. */
    if (alpha >= 1.0) return;
    double off_head[3], off_tail[3];
    zap_record_pair(alpha, off_head, off_tail);
    ngte_view v;
    ngte_view_build(cpu, &v);
    ngte_proj h, t;
    zap_project_point(&v, s_head_w, off_head, &h);
    zap_project_point(&v, s_tail_w, off_tail, &t);
    s_reb.dhx = h.sx - s_head_p.sx;
    s_reb.dhy = h.sy - s_head_p.sy;
    s_reb.dtx = t.sx - s_tail_p.sx;
    s_reb.dty = t.sy - s_tail_p.sy;
    s_reb.head_m3 = h.m3;
    s_reb.tail_m3 = t.m3;
    zap_width_scales(&v, off_head, off_tail);
    s_reb.active = 1;
}

/* Sub-tick re-extract. The primitives are the guest's own output and are gone
 * once the polybuf is reused, so the replay re-feeds the set the canonical tick
 * captured, rebased onto the pose this present shows: the record's head and
 * tail carry the ribbon's two ends, every corner between them moves by the
 * pair blended at its own position along the chain, and each widened corner's
 * offset from its chain point scales by the change in half-width. The jitter is not
 * re-derived - the routine draws and stores a fresh random halfword per point
 * inside its own call (:2673-2685), and this replay re-feeds the tick's
 * primitives, so a sub-tick present shows the tick's jitter rather than a new
 * one. The rebase runs only inside the tick's interval; at its end the replay
 * emits the captured corners and the paired depths verbatim and so reproduces
 * the canonical present exactly, which is what the spine's identity leg
 * measures. */
void zap_reextract(CPUState* cpu, int bracketed, double alpha) {
    if (!hook_reextract_ready(RP_TRACER)) return;
    if (bracketed) zap_rebase_all(cpu, alpha);
    zap_feed_set(s_tick, s_tick_n);
    zap_trace("inter", bracketed ? alpha : 1.0);
}

/* The hook address is the layout's zap draw site, bound at boot: a
 * static initializer cannot read another TU's const. A game with no located
 * routine leaves it 0 and the registry dispatches nothing. */
static feature_dispatch_entry_t zap_dispatch[] = {
    { 0u, on_zap_draw },
    { 0u, NULL },
};

/// @brief Binds the module's dispatch entry to the layout's zap draw address.
static void zap_apply_options(const openpete_options_t* opts) {
    (void)opts;
    zap_dispatch[0].addr = game_layout.zap.hook;
}

const feature_module_t zap_module = {
    .name           = "zap_native",
    .apply_options  = zap_apply_options,
    .dispatch_table = zap_dispatch,
};
