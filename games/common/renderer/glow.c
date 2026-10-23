// games/common/renderer/glow.c - the glow-ring feeder: a handwritten routine
// (Spyro 2 func_8004A8D4 / mobyfunc.s:1936-2146, Spyro 3 func_8002ECA8 /
// mobyfunc.s:1920-2166; the same 208 instructions but for constants and
// symbols) that draws one screen-facing polygon per live record of a pool and
// links each fan into the 3D ordering table itself, so none of its primitives
// reaches the 2D chokepoint the sprite feeder hooks. Every per-game address,
// count, stride and bound comes from game_layout.glow; the walk below is one
// body.
//
// Read from the routine (asm/mobyfunc.s):
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
// not a second projection. What the primitives do not carry is depth - the PSX
// orders them by ordering-table bucket - so each fan's depth comes from the key
// its own pool record links at (glow_record_depth), paired to the fan through
// the routine's own bookkeeping: each accepted record emits exactly one draw-mode packet, so the
// draw-mode packets delimit the fans in pool order, and the record walk here
// mirrors the routine's own rejects to say which records were accepted. The
// pairing is used only when the two counts agree; otherwise every fan of the
// call is left unpaired and the census reports it.
//
// The routine's lateral cull is the 4:3 frame plus a fixed margin. In a wider
// view a record it dropped on that test alone has its fan rebuilt from the
// record, the shape table and the camera (glow_wide_reveal), and drawn unless
// the fan has left the view's screen-X window.
//
// Spyro 1's own glow renderer is fed by a float re-derivation in
// games/spyro-1/renderer/particle.c: its records carry a moby-position pointer
// and its ring is a perspective-scaled radius, a different routine over a
// different record shape. What the two share - the hook bracket, the polybuf
// walk, the primitive decode and the projection helpers - is already shared and
// is reused here unchanged.
//
// Out of scope: the same routine reached from the pause screen's own draw,
// which stays an existing carve-out; this hook is the one call the world draw
// makes.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the phase byte it
// decrements in each record; this feeder only reads them afterwards.

#include "glow.h"
#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/env_knob.h"
#include "renderer/cut_guard.h"     /* cut_held_sets_t: the held cut's tick N-1 set */
#include "renderer/player_hide.h"   /* --hide-player: the companion's ring */
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

#define GW              (game_layout.glow)
#define GLOW_SHAPE_MASK 0xFFu

#define GLOW_MAX_PRIMS  512
#define GLOW_DRMODE     0xE1u   /* GP0 draw-mode packet, one per accepted record */
#define GLOW_TRI        0x32u   /* POLY_G3 semi-transparent */

/* One accepted record, as the fan the routine built from it needs it back:
 * the depth its primitives take, the slot address the keyed store holds its
 * previous copy under, the position words the tick read, and the screen point
 * the tick's camera put that position at. The routine loads a 2x2 rotate-scale
 * basis whose third row is zero and translates the shape's outline about the
 * record's own rotated position (Spyro 2 mobyfunc.s:2026-2034, Spyro 3
 * :2030-2038), so every corner of a fan shares that position's view Z and one
 * perspective divide: the fan's whole screen displacement is that point's. */
typedef struct {
    float    depth;
    uint32_t addr;
    int32_t  w[3];
    double   psx, psy;
} glow_rec;

typedef struct {
    ngte_prim p;
    float     depth;
    int       paired;
    int       blend;
    float     dx, dy;     /* the sub-tick screen displacement of its fan */
    glow_rec  src;
    int       sparx;      /* its record is the companion's (player_hide_sparx_glow) */
} glow_item;

static glow_item  s_item_buf[2][GLOW_MAX_PRIMS];
static glow_item* s_item = s_item_buf[0];
static int        s_item_n;
/* The set the last capture of a new canonical epoch retired, which a held
 * camera cut shows when it is tick N-1's. */
static glow_item* s_item_old = s_item_buf[1];
static int        s_item_old_n;
static int       s_groups;        /* draw-mode packets the call appended */
static int       s_accepted;      /* records the pool walk predicted */
static uint32_t  s_code_hist[256];
static uint32_t  s_tag_hist[256];
static uint32_t  s_fed_tris;

/** @brief Exchange the current captured set with the retired one, with their counts. */
static void glow_sets_swap(void) {
    glow_item* t = s_item; s_item = s_item_old; s_item_old = t;
    const int n = s_item_n; s_item_n = s_item_old_n; s_item_old_n = n;
}

static cut_held_sets_t s_sets = { glow_sets_swap, &s_item_n, "glow", { 0u, 0u }, 0u };

/** @brief Append one walked primitive to s_item, unpaired at the near-guard depth with no displacement.
 *
 * Also zeroes the item's record.
 * @param ctx Unused.
 * @param s The decoded primitive.
 * @return 1 when stored, 0 when s_item holds GLOW_MAX_PRIMS. */
static int glow_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= GLOW_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 1.0f;   /* unpaired: the near guard (view-Z 1) */
    s_item[s_item_n].paired = 0;
    s_item[s_item_n].blend = 0;
    s_item[s_item_n].dx = 0.0f;
    s_item[s_item_n].dy = 0.0f;
    memset(&s_item[s_item_n].src, 0, sizeof s_item[s_item_n].src);
    s_item[s_item_n].sparx = 0;
    s_item_n++;
    return 1;
}

/** @brief Walk the polybuf interval the glow routine allocated into s_item.
 *
 * The interval holds 3-word draw-mode packets (tag length 2) and 7-word
 * triangles (tag length 6); a longer tag ends the walk. At most GLOW_MAX_PRIMS
 * prims are kept, each unpaired at the near-guard depth with no displacement.
 * The code and tag histograms accumulate across calls.
 * @param cpu The guest CPU state, read only.
 * @param start First polybuf address of the routine's output.
 * @param end One past the last. */
static void glow_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 6, .raw_words_max = 7, .guard = GLOW_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, glow_prim_sink, NULL);
}

/** @brief The fan's native depth from the ordering-table key the routine links it at.
 *
 * The key is (MAC3 >> 7) - bias (Spyro 2 mobyfunc.s:2048-2049, Spyro 3 :2052-2053). A
 * negative key links at bucket 0 (Spyro 2 :2050-2052); on a game with the
 * front-entry branch, a record whose bias byte is front_bias links one entry
 * before bucket 0 instead (Spyro 3 :2054-2059), which is drawn after the whole
 * table and so takes the nearest depth. The key plus the owning moby's depth
 * offset converts to view-Z on the moby feeders' scale, so the fan sorts against
 * that moby's faces as the table sorts them: over every face of its own slot and
 * beyond, under every nearer one. With the allocator's bias of 4 and an owner
 * offset of 4 the two pulls cancel and the fan sits at the front edge of its
 * own view-Z slot. Against a surface whose native depth carries no authored
 * pull (terrain), the table puts the fan bias slots nearer than this depth
 * does. The fan is additive on a pipeline that writes no depth
 * (native_render.c gouraud_add_desc), so this depth decides only which opaque
 * pixels it adds onto; it occludes nothing.
 * @param m3 The record's view-space MAC3.
 * @param bias The record's signed ordering-table bias byte.
 * @return The scene depth the fan's triangles take. */
static float glow_record_depth(double m3, int bias) {
    int key = (int)((uint32_t)(int32_t)m3 >> 7) - bias;
    if (key < 0) {
        if (GW.front_bias >= 0 && bias == GW.front_bias)
            return openpete_native_scene_viewz(openpete_worldvz_full(1.0f));
        key = 0;
    }
    return openpete_native_scene_viewz(openpete_worldvz_owner_slot(key + GW.owner_offset));
}

/* The routine's verdict on one pool record, in the order it tests them. */
typedef enum {
    GLOW_V_FREE, GLOW_V_BOX, GLOW_V_NEAR, GLOW_V_FAR, GLOW_V_PYR_X, GLOW_V_PYR_Y,
    GLOW_V_ACCEPT,
} glow_verdict;

static const char* const k_glow_verdict_name[] = {
    "free", "box", "near", "far", "pyr_x", "pyr_y", "accept",
};

/* One pool record as the routine's cull reads it: the packed word, the
 * position words, the camera-relative offset (X - camX, camY - Y, camZ - Z)
 * and its rotation by the camera matrix. */
typedef struct {
    uint32_t addr;
    uint32_t packed;
    int32_t  w[3];
    double   off[3];
    double   m1, m2, m3;
} glow_src;

/** @brief Read one pool record and apply the routine's rejects to it in the routine's order.
 *
 * A zero shape byte is a free slot (:1932); the camera-relative offset must
 * stay inside the +/-GW.box box (:1970-1988); the rotated depth must be at
 * least GW.near_z and under GW.far_z; the rotated point must stay inside the
 * view pyramid 4|X| - 3Z < GW.pyr_x and 3|Y| - Z < GW.pyr_y (:1994-2000). The
 * pyramid is the 4:3 frame widened by a margin of GW.pyr_x / 4 view units
 * laterally.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view.
 * @param s The pool slot.
 * @param r Receives the record; the offset and rotation only past the free test.
 * @return The routine's verdict. */
static glow_verdict glow_classify(CPUState* cpu, const ngte_view* v, uint32_t s, glow_src* r) {
    r->addr = GW.pool + s * GW.stride;
    r->packed = cpu->read_word(r->addr + GW.packed_off);
    if ((r->packed & GLOW_SHAPE_MASK) == 0u) return GLOW_V_FREE;
    for (uint32_t k = 0; k < 3u; k++)
        r->w[k] = (int32_t)cpu->read_word(r->addr + GW.pos_off + k * 4u);
    r->off[0] = (double)r->w[0] - v->cam[0];   /* the routine's IR3 */
    r->off[1] = v->cam[1] - (double)r->w[1];   /* IR1 */
    r->off[2] = v->cam[2] - (double)r->w[2];   /* IR2 */
    const double box = (double)GW.box;
    for (int k = 0; k < 3; k++)
        if (r->off[k] <= -box || r->off[k] >= box) return GLOW_V_BOX;
    ngte_mac(v->cam_M, r->off[1], r->off[2], r->off[0], &r->m1, &r->m2, &r->m3);
    if (r->m3 < (double)GW.near_z) return GLOW_V_NEAR;
    if (r->m3 >= (double)GW.far_z) return GLOW_V_FAR;
    if (4.0 * fabs(r->m1) - 3.0 * r->m3 - (double)GW.pyr_x >= 0.0) return GLOW_V_PYR_X;
    if (3.0 * fabs(r->m2) - r->m3 - (double)GW.pyr_y >= 0.0) return GLOW_V_PYR_Y;
    return GLOW_V_ACCEPT;
}

/** @brief Fill the pairing record of one record the cull admitted.
 *
 * Stores its depth from glow_record_depth, its address, its position words
 * and its centre's screen point under the view.
 * @param v The projection view.
 * @param r The record from glow_classify.
 * @param out Receives the pairing record. */
static void glow_rec_fill(const ngte_view* v, const glow_src* r, glow_rec* out) {
    const int bias = (int)(int8_t)(uint8_t)((r->packed >> 24) & 0xFFu);
    out->depth = glow_record_depth(r->m3, bias);
    out->addr = r->addr;
    for (int k = 0; k < 3; k++) out->w[k] = r->w[k];
    ngte_proj o;
    ngte_project_offset(v, r->off, &o);
    out->psx = o.sx;
    out->psy = o.sy;
}

/** @brief Re-walk the glow pool in slot order and collect every record the routine would have drawn.
 *
 * A record is collected when glow_classify accepts it. Entries past `cap`
 * are counted but not stored.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view.
 * @param out Receives up to `cap` accepted records in pool order.
 * @param cap Capacity of `out`.
 * @return The number of accepted records, which may exceed `cap`. */
static int glow_predict(CPUState* cpu, const ngte_view* v, glow_rec* out, int cap) {
    int n = 0;
    for (uint32_t s = 0; s < GW.slots; s++) {
        glow_src r;
        if (glow_classify(cpu, v, s, &r) != GLOW_V_ACCEPT) continue;
        if (n < cap) glow_rec_fill(v, &r, &out[n]);
        n++;
    }
    return n;
}

/** @brief Attach each captured primitive to its fan and, when the counts agree, to the record that emitted it.
 *
 * A draw-mode packet opens a fan and its ABR bits give the fan's blend; the
 * triangles until the next draw-mode packet belong to it. Counts the packets
 * into s_groups and the predicted records into s_accepted; only when the two
 * are equal does each triangle take its record's depth and copy, otherwise
 * every triangle keeps the blend at the near-guard depth, unpaired. A
 * triangle before any draw-mode packet is left as captured.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view. */
static void glow_pair_depths(CPUState* cpu, const ngte_view* v) {
    static glow_rec pred[GLOW_MAX_PRIMS];
    s_groups = 0;
    for (int i = 0; i < s_item_n; i++)
        if (s_item[i].p.code == GLOW_DRMODE) s_groups++;
    s_accepted = glow_predict(cpu, v, pred, GLOW_MAX_PRIMS);
    const int pair = (s_accepted == s_groups && s_groups <= GLOW_MAX_PRIMS);

    int g = -1, blend = 0;
    float depth = 1.0f;   /* unpaired: the near guard (view-Z 1) */
    for (int i = 0; i < s_item_n; i++) {
        if (s_item[i].p.code == GLOW_DRMODE) {
            g++;
            /* The draw-mode word's semi-transparency rate, bits 5 and 6. */
            blend = ngte_abr_blend((int)((s_item[i].p.raw[1] >> 5) & 3u),
                                   NGTE_ABR_QUARTER_AS_ADD);
            depth = pair ? pred[g].depth : 1.0f;
            continue;
        }
        if (g < 0) continue;                  /* a triangle before any draw mode */
        s_item[i].blend = blend;
        s_item[i].depth = depth;
        s_item[i].paired = pair;
        if (pair) s_item[i].src = pred[g];
    }
}

/** @brief Mark every paired triangle whose record is the companion's glow record.
 *
 * The companion's record is the one its moby tag points at
 * (player_hide_sparx_glow); an unpaired triangle has no record and stays
 * unmarked.
 * @param cpu The guest CPU state, read only. */
static void glow_mark_sparx(CPUState* cpu) {
    const uint32_t rec = player_hide_sparx_glow(cpu);
    for (int i = 0; i < s_item_n; i++)
        s_item[i].sparx = rec != 0u && s_item[i].paired && s_item[i].src.addr == rec;
}

/* ---- the widened view's fans ---------------------------------------------
 *
 * The routine's lateral pyramid test is the 4:3 frame plus a fixed margin, so
 * in a wider view (Hor+ factor F above 1) it drops a record whose fan still
 * lies inside the revealed side band; the routine then emits no primitive for
 * it. Such a record keeps the routine's own depth window and vertical test,
 * and its fan is rebuilt here from the record and the camera alone, the way
 * the routine builds it:
 *   * the shape entry is shape_table + (shape byte - 1) * 64: sixteen
 *     halfwords each packing a signed rim direction (X in the low byte, Y in
 *     the high byte), then up to sixteen vertex-index halfwords (rim A in bits
 *     8-11, rim B in bits 0-3) ended by a zero (Spyro 3 mobyfunc.s:2013-2018,
 *     :2122-2143).
 *   * the angle is ((camera yaw >> 4) + phase) & 0xFF, where the phase is
 *     packed byte 2 as the routine leaves it (it decrements the byte before
 *     any cull, :1981-1982); the sine table gives s at the angle and c a
 *     quarter turn on (+0x80 bytes), and with c' = size * c / 64 and
 *     s' = size * s / 64 the rim direction (x, y) moves the centre by
 *     ((-1.5 c' x + 1.5 s' y) / 4096, (s' x + c' y) / 4096) in view X and Y
 *     at the centre's own view Z (:2062-2106).
 *   * each triangle is (centre, rim A, rim B), the centre in the record's
 *     colour word and both rims black, additive (:2110-2143).
 * The fan is admitted unless every vertex lies left of the view's screen-X
 * bound xlo or every vertex at or right of xhi: it is culled only when it has
 * left the view. A record the routine accepted is never rebuilt; its fan is
 * the routine's own. */

#define GLOW_RIM_N  16
#define GLOW_TRI_N  16

/* One rebuilt fan in screen space, under the view it was built for. */
typedef struct {
    int      n;                         /* triangles */
    float    x[GLOW_TRI_N][3], y[GLOW_TRI_N][3];
    uint32_t rgb;                       /* the centre colour, low 24 bits */
    float    xmin, xmax;
} glow_fan;

static int s_wide_admit;   /* fans the widened view rebuilt and fed this call */
static int s_wide_cull;    /* fans the widened view rebuilt and culled this call */

/** @brief Rebuild the fan the routine draws for one record, projected under the view.
 *
 * Reads the shape entry, the camera yaw and the sine table as the routine
 * does (see the section comment) and projects every rim point through
 * ngte_screen_gte with no SXY bound, so the fan keeps its true X in a widened
 * view. The routine's RTPS also multiplies a stale VZ0 by a rotation element
 * that is 0 or -1; that term is dropped (at most 8 view units).
 * @param cpu The guest CPU state, read only.
 * @param v The projection view.
 * @param r The record from glow_classify, past its depth window.
 * @param f Receives the fan; f->n = 0 when the layout names no shape table or
 *        sine table. */
static void glow_fan_build(CPUState* cpu, const ngte_view* v, const glow_src* r, glow_fan* f) {
    f->n = 0;
    f->xmin = 1e30f; f->xmax = -1e30f;
    const uint32_t tbl = game_layout.sin_table;
    if (!GW.shape_table || !tbl) return;
    const uint32_t entry = GW.shape_table + ((r->packed & GLOW_SHAPE_MASK) - 1u) * 64u;
    const int yaw = (int16_t)cpu->read_half(game_layout.cam.base + GW.cam_yaw_off) >> 4;
    const uint32_t ang = (uint32_t)(yaw + (int)((r->packed >> 16) & 0xFFu)) & 0xFFu;
    const double size = (double)((r->packed >> 8) & 0xFFu);
    const double c = size * (double)(int16_t)cpu->read_half(tbl + 0x80u + ang * 2u) / 64.0;
    const double sn = size * (double)(int16_t)cpu->read_half(tbl + ang * 2u) / 64.0;
    double rx[GLOW_RIM_N], ry[GLOW_RIM_N];
    for (uint32_t j = 0; j < GLOW_RIM_N; j++) {
        const uint16_t h = cpu->read_half(entry + j * 2u);
        const double x = (double)(int8_t)(h & 0xFFu), y = (double)(int8_t)(h >> 8);
        ngte_proj o;
        ngte_screen_gte(r->m1 + (-1.5 * c * x + 1.5 * sn * y) / 4096.0,
                        r->m2 + (sn * x + c * y) / 4096.0, r->m3,
                        v->ofx, v->ofy, v, 0, &o);
        rx[j] = o.sx; ry[j] = o.sy;
    }
    ngte_proj ctr;
    ngte_screen_gte(r->m1, r->m2, r->m3, v->ofx, v->ofy, v, 0, &ctr);
    f->rgb = cpu->read_word(r->addr + 0x0Cu) & 0xFFFFFFu;
    for (uint32_t k = 0; k < GLOW_TRI_N; k++) {
        const uint16_t h = cpu->read_half(entry + 0x20u + k * 2u);
        if (h == 0u) break;
        const uint32_t a = (h >> 8) & 0xFu, b = h & 0xFu;
        const double xs[3] = { ctr.sx, rx[a], rx[b] }, ys[3] = { ctr.sy, ry[a], ry[b] };
        for (int i = 0; i < 3; i++) {
            f->x[f->n][i] = (float)xs[i];
            f->y[f->n][i] = (float)ys[i];
            if (f->x[f->n][i] < f->xmin) f->xmin = f->x[f->n][i];
            if (f->x[f->n][i] > f->xmax) f->xmax = f->x[f->n][i];
        }
        f->n++;
    }
}

/** @brief Append a rebuilt fan to s_item as paired additive triangles owned by its record.
 *
 * Stops at GLOW_MAX_PRIMS items.
 * @param v The projection view.
 * @param r The record the fan was built from.
 * @param f The fan. */
static void glow_fan_append(const ngte_view* v, const glow_src* r, const glow_fan* f) {
    glow_rec rec;
    glow_rec_fill(v, r, &rec);
    const int blend = ngte_abr_blend(1, NGTE_ABR_QUARTER_AS_ADD);   /* 0xE1000620: ABR 0b01 */
    for (int t = 0; t < f->n && s_item_n < GLOW_MAX_PRIMS; t++) {
        glow_item* it = &s_item[s_item_n++];
        memset(it, 0, sizeof *it);
        it->p.code = GLOW_TRI;
        it->p.nv = 3;
        it->p.gouraud = 1;
        it->p.stp = 1;
        for (int i = 0; i < 3; i++) { it->p.x[i] = f->x[t][i]; it->p.y[i] = f->y[t][i]; }
        it->p.rgb[0] = f->rgb;
        it->depth = rec.depth;
        it->paired = 1;
        it->blend = blend;
        it->src = rec;
    }
}

/** @brief Whether the per-record cull log is on for this VBL (OPENPETE_GLOW_CULL_LOG=LO-HI).
 *
 * The range is in openpete_frame units, as every census range is.
 * @return 1 when on, else 0. */
static int glow_cull_log_on(void) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    return env_frame_range_active(&s_range, "OPENPETE_GLOW_CULL_LOG", openpete_frame);
}

/** @brief The largest vertex distance between a rebuilt fan and the routine's own fan for the same record.
 *
 * The routine's fan is the triangles after the `group`-th draw-mode packet of
 * s_item, compared triangle by triangle in order; the calibration of
 * glow_fan_build against the primitives the routine emitted.
 * @param f The rebuilt fan.
 * @param group The record's draw-mode packet ordinal in s_item.
 * @param ntri Receives the routine's triangle count for the record.
 * @return The largest per-axis vertex distance in screen pixels, or -1 when
 *         the group is absent. */
static double glow_fan_cal(const glow_fan* f, int group, int* ntri) {
    int g = -1, t = 0;
    double worst = -1.0;
    *ntri = 0;
    for (int i = 0; i < s_item_n; i++) {
        const ngte_prim* p = &s_item[i].p;
        if (p->code == GLOW_DRMODE) { g++; continue; }
        if (g != group || p->nv != 3) continue;
        (*ntri)++;
        if (worst < 0.0) worst = 0.0;
        if (t >= f->n) { worst = 1e9; continue; }
        for (int k = 0; k < 3; k++) {
            const double dx = fabs((double)p->x[k] - (double)f->x[t][k]);
            const double dy = fabs((double)p->y[k] - (double)f->y[t][k]);
            if (dx > worst) worst = dx;
            if (dy > worst) worst = dy;
        }
        t++;
    }
    return worst;
}

/** @brief Whether the routine skips a live record before its cull in its front-entry-only state.
 *
 * True when the layout names the branch, the game state word equals
 * GW.solo_state, the flag word is non-zero and the record's bias byte is not
 * GW.front_bias.
 * @param cpu The guest CPU state, read only.
 * @param packed The record's packed word.
 * @return 1 when the routine draws nothing for the record, else 0. */
static int glow_solo_skips(CPUState* cpu, uint32_t packed) {
    if (!GW.solo_flag) return 0;
    if (cpu->read_word(GW.solo_state_addr) != GW.solo_state) return 0;
    if (cpu->read_word(GW.solo_flag) == 0u) return 0;
    return (int)(int8_t)(uint8_t)((packed >> 24) & 0xFFu) != GW.front_bias;
}

/** @brief Print one record's line of the cull log.
 *
 * The line holds the routine's verdict, the verdict this view draws the
 * record under, its centre's screen point, its rebuilt fan's screen-X extent
 * and the view's screen-X window. For a record the routine accepted, when the
 * fans paired, it also holds the rebuilt and the routine's triangle counts and
 * their largest vertex distance (glow_fan_cal).
 * @param v The tick's projection view.
 * @param slot The pool slot.
 * @param g The routine's verdict.
 * @param view The verdict this view draws the record under.
 * @param r The record.
 * @param f The rebuilt fan; n = 0 when none was built.
 * @param accepted The record's ordinal among the routine's accepted records. */
static void glow_cull_log_line(const ngte_view* v, uint32_t slot, glow_verdict g, const char* view,
                               const glow_src* r, const glow_fan* f, int accepted) {
    ngte_proj o;
    ngte_project_offset(v, r->off, &o);
    int ntri = 0;
    const double cal = (g == GLOW_V_ACCEPT && s_accepted == s_groups)
                           ? glow_fan_cal(f, accepted, &ntri) : -1.0;
    fprintf(stderr, "[glow-cull f=%u] slot=%u routine=%s view=%s F=%.4f sx=%.1f sy=%.1f m3=%.0f"
                    " fan_x=[%.1f,%.1f] window=[%.1f,%.1f] cal_tris=%d/%d cal_maxerr=%.2f\n",
            openpete_frame, slot, k_glow_verdict_name[g], view, (double)v->F, o.sx, o.sy, r->m3,
            (double)f->xmin, (double)f->xmax, (double)v->xlo, (double)v->xhi, f->n, ntri, cal);
}

/** @brief Draw a record the routine's lateral test dropped when its rebuilt fan is still inside the view's screen-X window.
 *
 * The fan is culled when every vertex lies left of xlo or every vertex at or
 * right of xhi, else appended (glow_fan_append); s_wide_cull or s_wide_admit
 * counts the outcome. A fan with no triangles is neither.
 * @param v The tick's projection view.
 * @param r The record.
 * @param f Its rebuilt fan.
 * @return The verdict name the cull log prints. */
static const char* glow_wide_admit(const ngte_view* v, const glow_src* r, const glow_fan* f) {
    if (f->n <= 0) return "reject";
    if (f->xmax < v->xlo || f->xmin >= v->xhi) { s_wide_cull++; return "wide_cull"; }
    glow_fan_append(v, r, f);
    s_wide_admit++;
    return "wide_admit";
}

/** @brief Rebuild and feed-queue the fans the routine's lateral test dropped but the widened view shows.
 *
 * Runs only when the view is wider than 4:3 (F above 1) or the cull log is
 * on. Every record the routine rejected on its lateral pyramid test alone,
 * and did not skip in its front-entry-only state (glow_solo_skips), is
 * rebuilt (glow_fan_build) and offered to glow_wide_admit; at F of at most 1
 * nothing is appended. With OPENPETE_GLOW_CULL_LOG in range, each live record
 * past the routine's box test prints a log line (glow_cull_log_line), and a
 * record the routine accepted is also rebuilt so the line can compare it with
 * the routine's own fan.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view. */
static void glow_wide_reveal(CPUState* cpu, const ngte_view* v) {
    s_wide_admit = 0; s_wide_cull = 0;
    const int wide = v->F > 1.0f, log = glow_cull_log_on();
    if (!wide && !log) return;
    int accepted = -1;
    for (uint32_t s = 0; s < GW.slots; s++) {
        glow_src r;
        const glow_verdict g = glow_classify(cpu, v, s, &r);
        if (g == GLOW_V_ACCEPT) accepted++;
        if (g == GLOW_V_FREE || glow_solo_skips(cpu, r.packed)) continue;
        const char* view = g == GLOW_V_ACCEPT ? "accept" : "reject";
        glow_fan f = { .n = 0, .xmin = 0.0f, .xmax = 0.0f };
        if (g == GLOW_V_PYR_X || (log && g == GLOW_V_ACCEPT)) glow_fan_build(cpu, v, &r, &f);
        if (wide && g == GLOW_V_PYR_X) view = glow_wide_admit(v, &r, &f);
        if (log && g >= GLOW_V_NEAR) glow_cull_log_line(v, s, g, view, &r, &f, accepted);
    }
}

/** @brief Whether this feeder emits the glow fans, latched on first use.
 *
 * OPENPETE_GLOW with a value starting with '0' suppresses this feed alone.
 * The guest draw still runs, so the A/B prices the rings' painted pixels from
 * one binary.
 * @return 1 when enabled, else 0. */
static int glow_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLOW"); v = !(e && *e && *e == '0'); }
    return v;
}

/** @brief Push every captured triangle to the native scene, shifted by its fan's displacement.
 *
 * Does nothing when OPENPETE_GLOW suppresses the feed or nothing was
 * captured. Each three-vertex primitive becomes one untextured WORLD triangle
 * at its item's depth (also used as the constant W), moved by the item's dx and
 * dy, with the item's blend and the vertex colours made opaque. Draw-mode
 * packets are skipped, and so is a triangle of the companion's ring while the
 * player is hidden (player_hide_active). Counts the triangles in s_fed_tris and tags them as glow
 * particles with sel 0, since a glow ring carries no game particle class. */
static void glow_feed_native(void) {
    if (!glow_feed_enabled()) return;
    if (s_item_n <= 0) return;
    const int hide_player = player_hide_active();
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_GLOW, 0, 0));
    for (int i = 0; i < s_item_n; i++) {
        const ngte_prim* p = &s_item[i].p;
        if (p->nv != 3) continue;
        const int sparx = s_item[i].sparx;
        player_hide_count_data(sparx ? PH_CAT_SPARX_GLOW : PH_CAT_OTHER_GLOW,
                               sparx && hide_player, p, sizeof *p);
        if (sparx && hide_player) continue;
        /* The fan is flat in view-Z, so any constant W interpolates its
         * colour screen-linearly: the slot view-Z serves as W. */
        static const float zu[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float x[4] = { (float)p->x[0] + s_item[i].dx, (float)p->x[1] + s_item[i].dx,
                             (float)p->x[2] + s_item[i].dx, 0.0f };
        const float y[4] = { (float)p->y[0] + s_item[i].dy, (float)p->y[1] + s_item[i].dy,
                             (float)p->y[2] + s_item[i].dy, 0.0f };
        const uint32_t rgb[4] = { p->rgb[0], p->rgb[1], p->rgb[2], 0u };
        s_fed_tris += (uint32_t)ngte_feed_flat_quad(x, y, zu, zu, rgb, s_item[i].depth,
                                                    NULL, s_item[i].blend, 3);
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Print the per-call glow census when the VBL lies in the OPENPETE_GLOW_CENSUS range.
 *
 * OPENPETE_GLOW_CENSUS=LO-HI (or a single VBL): per call in that range, the
 * primitives the routine appended by GPU code, the triangles among them and
 * the widened view's rebuilt ones, the fans the draw-mode packets delimit,
 * the records the pool walk predicted, the fans the widened view rebuilt and
 * fed or culled, the native triangles this feeder emitted, and how many
 * triangles were left unpaired. A capture whose fed count is zero measured nothing.
 * @param start First polybuf address of the routine's output.
 * @param end One past the last.
 * @param fed_before The fed triangle total before this call. */
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
    fprintf(stderr, " tris=%d fans=%d accepted=%d wide_admit=%d wide_cull=%d fed_tris=%u unpaired=%d\n",
            tris, s_groups, s_accepted, s_wide_admit, s_wide_cull, s_fed_tris - fed_before, unpaired);
}

/* ---- hook ---------------------------------------------------------------- */

static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_GLOW, .prof_tag = "glow.srend", .st = &s_hb,
};

/** @brief Dispatch hook on the glow routine: run it once, then capture, pair, widen and feed its fans.
 *
 * Runs through the shared bracket. Refuses (returns 0, the ordinary dispatch runs) a call whose return
 * address differs from GW.ra_guard when the layout names one. A taken call
 * empties the captured set, retiring the previous epoch's set first when it
 * is the epoch's first call (cut_held_sets_capture). When the routine
 * appended anything, its primitives are captured and paired under the tick's
 * view. In a view wider than 4:3, or with the cull log on, the fans the
 * routine's lateral test dropped are rebuilt (glow_wide_reveal) whether or
 * not it appended anything. The set is fed natively when it holds anything,
 * the scene is live and RP_GLOW is selected. The census runs on every taken
 * call.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the call is refused or the bracket declines, else 1 (the
 *         routine has already run). */
static int on_glow_draw(CPUState* cpu, uint32_t addr) {
    /* A game whose image holds a second call site over a frozen frame names
     * the world draw's return address; every other caller is refused. */
    if (GW.ra_guard && cpu->ra != GW.ra_guard) return 0;
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    cut_held_sets_capture(&s_sets);
    s_item_n = 0; s_groups = 0; s_accepted = 0; s_wide_admit = 0; s_wide_cull = 0;
    const uint32_t fed_before = s_fed_tris;
    /* A view wider than 4:3 can show a fan the routine emitted nothing for,
     * so it walks the pool even after an empty call. */
    if (endp > startp || openpete_native_widescreen_factor() > 1.0f || glow_cull_log_on()) {
        ngte_view view;
        ngte_view_build(cpu, &view);
        if (endp > startp) {
            glow_capture_prims(cpu, startp, endp);
            glow_pair_depths(cpu, &view);
        }
        glow_wide_reveal(cpu, &view);
        if (s_item_n > 0) {
            glow_mark_sparx(cpu);
            render_material_level_push(cpu);
            if (feed && render_path_enabled(RP_GLOW)) glow_feed_native();
        }
    }
    glow_census(startp, endp, fed_before);
    return hook_bracket_end(&k_bracket);
}

/* ---- sub-tick rebase ------------------------------------------------------
 *
 * The primitives are the guest's own int16 output, built under the tick's
 * camera from the tick's pool, so a present that re-feeds them draws every
 * ring at the pixels the tick drew: neither the camera's motion nor the
 * record's reaches them. The rebase measures the record's screen displacement
 * as the difference of two projections - the position the present shows under
 * the staged camera, against the position the tick drew under the tick's
 * camera - and moves the fan by it. Every corner of a fan shares the record's
 * view Z and its perspective divide, so one displacement carries the fan. At
 * the end of the tick's own interval the shown position and the staged camera
 * are both the tick's, so the two projections take identical inputs through
 * identical arithmetic and the displacement is exactly zero.
 *
 * Out of scope, each its own channel of the same record: the ring's spin,
 * which the routine takes from the camera's yaw plus a phase byte it
 * decrements by two table entries per draw (Spyro 2 mobyfunc.s:1970-1973), and
 * the ring's size, which scales with the record's view Z. */

/* A record's speed ceiling, in world units per tick. The keyed store is keyed
 * by slot address, so a slot a new record respawns into inside one tick pairs
 * across two different rings and would lerp as a streak between them. The
 * packed-word test below catches a reuse that changed the shape, the size or
 * the ordering bias; this catches one that kept all three, at a distance no
 * record covers in a tick. */
#define GLOW_REUSE_CEILING 4096.0

/* The packed word's stable bytes: the shape index, the size and the signed
 * ordering-table bias, all written once by the spawner. Byte 2 is the spin
 * phase, which the routine rewrites on every draw, so it is masked out of the
 * identity test. */
#define GLOW_PACKED_STABLE 0xFF00FFFFu

static unsigned long s_reb_stat[2];   /* 0 lerped, 1 snapped */
static double        s_reb_worldmax;  /* the largest per-tick world step paired */
static double        s_reb_screenmax; /* the largest screen displacement applied */

/** @brief Compute a record's world displacement between the pose the tick drew and the pose this present shows.
 *
 * Reads the record's previous snapshot from the interp object table and
 * blends toward it by 1 - alpha, recording the largest paired step in
 * s_reb_worldmax. Returns 0 with a zero offset for a record with no previous
 * snapshot, a snapshot blob too short for the position and packed words,
 * snapshots more than one and a half tick periods apart, packed words whose
 * GLOW_PACKED_STABLE bytes differ, or a displacement beyond
 * GLOW_REUSE_CEILING; the fan then moves only with the camera.
 * @param r The paired record as the tick read it.
 * @param alpha The present's position in the tick interval, 1 at its end.
 * @param off Receives the world-unit offset from the tick's position.
 * @return 1 when the previous snapshot is the same record, else 0. */
static int glow_record_offset(const glow_rec* r, double alpha, double off[3]) {
    off[0] = off[1] = off[2] = 0.0;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(r->addr);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    if ((uint32_t)o->blob_len < GW.pos_off + 12u ||
        (uint32_t)o->blob_len < GW.packed_off + 4u) return 0;
    if (o->curr_ns - o->prev_ns >
        g_openpete_tick_period_ns + g_openpete_tick_period_ns / 2) return 0;
    uint32_t pp, cp;
    memcpy(&pp, o->prev + GW.packed_off, 4);
    memcpy(&cp, o->curr + GW.packed_off, 4);
    if ((pp & GLOW_PACKED_STABLE) != (cp & GLOW_PACKED_STABLE)) return 0;
    int32_t pw[3];
    memcpy(pw, o->prev + GW.pos_off, 12);
    double d[3], sq = 0.0;
    for (int k = 0; k < 3; k++) {
        d[k] = (double)pw[k] - (double)r->w[k];
        sq += d[k] * d[k];
    }
    if (sq > GLOW_REUSE_CEILING * GLOW_REUSE_CEILING) return 0;
    if (sqrt(sq) > s_reb_worldmax) s_reb_worldmax = sqrt(sq);
    const double rest = 1.0 - alpha;
    for (int k = 0; k < 3; k++) off[k] = d[k] * rest;
    return 1;
}

/** @brief Print the rebase's verdict counts and largest steps for the run, registered at exit.
 *
 * Registered by glow_rebase_all when OPENPETE_GLOW_REBASE_DBG is set,
 * non-empty and not starting with '0'. A run whose lerped count is zero
 * rebased nothing against a previous record and measured nothing. */
static void glow_rebase_report(void) {
    fprintf(stderr, "[glow-rebase] lerped=%lu snapped=%lu worldmax=%.3f screenmax=%.3f\n",
            s_reb_stat[0], s_reb_stat[1], s_reb_worldmax, s_reb_screenmax);
}

/** @brief Rebase every captured fan onto the screen position this present shows.
 *
 * The staged camera resolves through the sub-tick view, so building a view
 * here reads the interpolated transform the rest of the pass draws under.
 * Each paired primitive takes the displacement of its record's projected
 * point; an unpaired one, or one whose shown position falls below the
 * routine's near plane, keeps a zero displacement.
 * @param cpu The guest CPU state, read only.
 * @param alpha The present's position in the tick interval, 1 at its end. */
static void glow_rebase_all(CPUState* cpu, double alpha) {
    if (s_item_n <= 0) return;
    static int dbg = -1;
    if (dbg < 0) {
        const char* e = getenv("OPENPETE_GLOW_REBASE_DBG");
        dbg = (e && *e && *e != '0');
        if (dbg) atexit(glow_rebase_report);
    }
    ngte_view v;
    ngte_view_build(cpu, &v);
    for (int i = 0; i < s_item_n; i++) {
        glow_item* it = &s_item[i];
        it->dx = 0.0f;
        it->dy = 0.0f;
        if (!it->src.addr) continue;              /* an unpaired primitive */
        double off[3];
        s_reb_stat[glow_record_offset(&it->src, alpha, off) ? 0 : 1]++;
        const double p[3] = { (double)it->src.w[0] + off[0],
                              (double)it->src.w[1] + off[1],
                              (double)it->src.w[2] + off[2] };
        const double vo[3] = { p[0] - v.cam[0], v.cam[1] - p[1], v.cam[2] - p[2] };
        ngte_proj o;
        ngte_project_offset(&v, vo, &o);
        /* A record whose shown position crossed the routine's own near plane
         * has no screen point to displace to; the fan holds the tick's. */
        if (o.m3 < (double)GW.near_z) continue;
        it->dx = (float)(o.sx - it->src.psx);
        it->dy = (float)(o.sy - it->src.psy);
        const double m = fabs((double)it->dx) > fabs((double)it->dy)
                             ? fabs((double)it->dx) : fabs((double)it->dy);
        if (m > s_reb_screenmax) s_reb_screenmax = m;
    }
}

/* Sub-tick re-extract; see glow.h. */
void glow_reextract(CPUState* cpu, int bracketed, double alpha, int hold) {
    const int pick = cut_held_sets_enter(&s_sets, hold);
    if (hook_reextract_ready_held(RP_GLOW, pick)) {
        if (pick >= 0) {
            /* The staged camera is tick N-1's, the camera this set was drawn
             * under: the fans stand where that tick drew them. */
            for (int i = 0; i < s_item_n; i++) { s_item[i].dx = 0.0f; s_item[i].dy = 0.0f; }
        } else if (bracketed) {
            glow_rebase_all(cpu, alpha);
        }
        render_material_level_push(cpu);
        glow_feed_native();
    }
    cut_held_sets_leave(&s_sets, pick);
}

/* The hook address is the layout's glow draw site, bound at boot: a static
 * initializer cannot read another TU's const. A game with no located routine
 * leaves it 0 and the registry dispatches nothing. */
static feature_dispatch_entry_t glow_dispatch[] = {
    { 0u, on_glow_draw, 0u },
    { 0u, NULL, 0u },
};

/** @brief Bind the glow dispatch entry to the layout's routine address.
 *
 * A game whose layout locates no routine leaves the address 0.
 * @param opts Unused. */
static void glow_apply_options(const openpete_options_t* opts) {
    (void)opts;
    glow_dispatch[0].addr = game_layout.glow.hook;
}

const feature_module_t glow_module = {
    .name           = "glow_native",
    .apply_options  = glow_apply_options,
    .dispatch_table = glow_dispatch,
};
