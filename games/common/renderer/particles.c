// games/common/renderer/particles.c - the particle feeder for a game whose
// particle renderer links its primitives into the 3D ordering table itself.
//
// The routine (asm/partsdraw.s) is handwritten: it loads the camera matrix
// into the GTE with a zero translation, walks a pool of fixed-stride records
// and appends one primitive per drawn record to the polybuf. Six classes:
// a 4-word LINE_F2 dot, a 5-word LINE_G2 streak, and 10-word POLY_FT4
// billboards. Because it links each primitive itself, none of them reaches
// the 2D ordering-table chokepoint the sprite feeder hooks, so without this
// feeder the native scene shows no particles at all.
//
// The geometry is taken from the primitives the routine wrote, not
// re-derived: the routine's screen positions are already the GTE's int16
// output, and reading them back costs one polybuf walk instead of a second
// copy of a six-class projection. What the primitives do not carry is depth -
// the PSX orders them by ordering-table bucket - so the feeder recovers each
// primitive's view-Z by projecting its own record's position, pairing
// primitives to records through the stamp byte the routine maintains
// (partsdraw.s:223 clears it when a record's processing starts, :358, :415,
// :528, :693 and :745 set it once the record passes its screen and depth
// window) and the per-class key test some classes apply after the stamp
// (game_layout.particles.stamp_skip), which makes the pairing the routine's
// own invariant rather than a match on geometry. Class `fixed_layer_class` and up link at a fixed front bucket
// (partsdraw.s:202) and stamp nothing, so those primitives take the near
// depth that bucket means and are paired by class alone. Line numbers cite the
// Spyro 3 asm/partsdraw.s; the Spyro 2 routine is the same code two lines
// lower.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the stamp bytes and
// the polybuf cursor; this feeder only reads them afterwards.
//
// Gated by OPENPETE_PARTICLE_CENSUS (census) or a native renderer mode
// (--renderer vk; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/env_knob.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_interp_objs.h"   /* the per-record prev/curr pool snapshots */
#include "renderer/particles.h"
#include "renderer/orchestrator.h"  /* the canonical-render epoch */
#include "renderer/cut_guard.h"     /* cut_held_set_pick, cut_census_held_set */
#include "renderer/moby.h"          /* moby_native_canon_ref_ns */
#include "renderer/player_hide.h"   /* --hide-player: the player's records */

extern op_time_t g_openpete_tick_stamp;   /* the newest object-capture stamp (openpete_interp_objs.c) */
#include "game_layout.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;

/* The pool walk's own shape, held here rather than in the descriptor because
 * it is the shape of one routine: the selector's class field and the sentinel
 * that ends the pool are how the routine dispatches, not a per-game datum. */
#define PART_CLASS_SHIFT 8
#define PART_CLASS_MASK  0xF
#define PART_POOL_END    (-1)   /* selector >> 8 at the end-of-pool sentinel */

/* The record's age byte. The pool update advances it while a record lives and
 * resets it when a slot takes a new record; the draw routine never reads it,
 * which is why it is not among the layout descriptor's draw-side offsets. It is
 * the only field that separates a continuing particle from a slot reuse that
 * kept the selector's class and type, so the sub-tick pairing rests on it. */
#define PART_AGE_OFF 0x02u

#define PART_MAX_PRIMS 2048

/* The class a two-point record draws as: the routine projects a second record
 * point for it and the primitive's two endpoints are those two projections
 * (partsdraw.s:650-659 reads the high half of +0x8 for the second X and +0xC
 * for its Y and Z, then stores the two screen points at polybuf +0x8 and +0x10).
 * Every other paired class carries one record point.
 *
 * `PART_ROT_CLASS` is the class whose corner extents are a rotation of a fixed
 * pair of view-space offsets around the record's own view position, all four
 * corners sharing that position's Z (partsdraw.s:495-504 reads the angle,
 * :527-538 builds the four offsets from the sine pair and :567-593 projects
 * them with VZ 0 against the record's translation). The feed draws this
 * class's quads from corners part_rot_corners builds in float, at capture and
 * again at each sub-tick present's shown point, angle and size; the
 * screen-space rotation below serves only a primitive that is not a quad. The
 * other class that reads an angle (partsdraw.s:225-228) rotates in world space
 * and projects each corner on its own Z, which a screen-space rotation is not,
 * so it rebases by translation alone. */
#define PART_TWO_POINT_CLASS 1
#define PART_ROT_CLASS       2

typedef struct {
    ngte_prim p;
    float     depth;
    double    vz;         /* the record point's view-Z under the tick's camera */
    double    m1, m2;     /* the record point's view X and Y under the tick's camera */
    float     ex[4], ey[4]; /* PART_ROT_CLASS: the corners built in float (part_rot_corners) */
    int       exact;      /* 1 = ex/ey hold the corners the feed draws */
    double    psx, psy;   /* the record's own projected point, or the sentinel */
    double    psx2, psy2; /* the second record point of a two-point class */
    int       cls;        /* the record's class, or -1 when unpaired */
    uint32_t  rec;        /* the record that appended it, 0 when unpaired */
    uint32_t  w0, w1, w2; /* the record's position words as the tick read them */
    int       owner;      /* player_hide_particle_owner of the record that appended it */
} part_item;

/* The screen-space rebase of one captured primitive onto the sub-tick pose:
 * the translation of each record point, and the rotation of the corner offsets
 * about the first one. Inactive on a canonical feed, and zero-valued at the
 * end of the tick's own interval, where the feed must reproduce the canonical
 * primitive bit for bit. */
typedef struct {
    float dx, dy;         /* the first record point's screen displacement */
    float dx2, dy2;       /* the second's, for a two-point class */
    double rc, rs;        /* the corner rotation, cos/sin of the angle change */
    double scl;           /* the corner offsets' scale, 1.0 when the extent holds */
    float depth;          /* the depth of the re-projected record point */
    uint8_t active;       /* 1 = translate the corners and take `depth` */
    uint8_t rot;          /* 1 = rotate the corner offsets as well */
    uint8_t dsz;          /* 1 = the pair's two size bytes differ */
    uint8_t corners;      /* 1 = cx/cy are the corners, rebuilt at the shown state */
    float cx[4], cy[4];
} part_rebase;

static part_item   s_item_buf[2][PART_MAX_PRIMS];
static part_item*  s_item = s_item_buf[0];
static part_rebase s_reb[PART_MAX_PRIMS];
static int         s_item_n;
/* The canonical epoch the captured set was taken in and the object-capture
 * stamp current then (every pool state that draw read is stamped at or before
 * it), and the previous epoch's set, which a held camera cut replays
 * (particles_replay_prev). */
static uint32_t  s_item_epoch;
static op_time_t s_item_ref_ns;
static struct { part_item* item; int n; uint32_t epoch; op_time_t ref_ns; } s_item_prev = { .item = s_item_buf[1] };
/* While the previous set is replayed: its records show each pool slot's state
 * as of that set's draw, the pair's prev for a slot that rolled after
 * s_item_ref_cut and its curr otherwise. */
static int       s_item_ref_prev;
static op_time_t s_item_ref_cut;

/** @brief Exchange the captured set, its epoch and stamp for the previous epoch's. */
static void part_item_swap(void) {
    part_item* t = s_item; s_item = s_item_prev.item; s_item_prev.item = t;
    int n = s_item_n; s_item_n = s_item_prev.n; s_item_prev.n = n;
    uint32_t e = s_item_epoch; s_item_epoch = s_item_prev.epoch; s_item_prev.epoch = e;
    op_time_t r = s_item_ref_ns; s_item_ref_ns = s_item_prev.ref_ns; s_item_prev.ref_ns = r;
}

static uint32_t s_code_hist[256];
static uint32_t s_tag_hist[256];
static uint32_t s_fed_tris;      /* native triangles the last feed emitted */

/** @brief The ngte_prim_sink that appends a captured primitive to s_item.
 *
 * The new item is unpaired: depth 1 (the near guard), class -1, no record and
 * an inactive rebase with scale 1.
 * @param s The decoded primitive.
 * @return 1 when stored, 0 when s_item holds PART_MAX_PRIMS. */
static int part_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= PART_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 1.0f;   /* unpaired: the near guard (view-Z 1) */
    s_item[s_item_n].psx = s_item[s_item_n].psy = 0.0;
    s_item[s_item_n].psx2 = s_item[s_item_n].psy2 = 0.0;
    s_item[s_item_n].cls = -1;
    s_item[s_item_n].rec = 0u;
    s_item[s_item_n].w0 = s_item[s_item_n].w1 = s_item[s_item_n].w2 = 0u;
    s_item[s_item_n].owner = 0;
    s_item[s_item_n].exact = 0;
    memset(&s_reb[s_item_n], 0, sizeof s_reb[0]);
    s_reb[s_item_n].scl = 1.0;
    s_item_n++;
    return 1;
}

/** @brief Captures the primitives the routine appended to the polybuf interval [start, end) into s_item.
 *
 * Resets the captured set, then walks with ngte_walk_prims. The longest
 * primitive the routine appends is the ten-word POLY_FT4 (tag length 9), so a
 * longer tag ends the walk; every code is kept with its first ten raw words,
 * since a line is not a polygon and the line feed decodes it from those words.
 * The code and tag histograms count the stored primitives. At most
 * PART_MAX_PRIMS are kept.
 * @param cpu Reads guest RAM.
 * @param start Guest address of the first primitive.
 * @param end Guest address one past the last primitive. */
static void part_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    /* The first capture of a new canonical epoch retires the current set. */
    const uint32_t epoch = spyro_native_canon_epoch_stamp();
    if (s_item_epoch != epoch) part_item_swap();
    s_item_epoch  = epoch;
    s_item_ref_ns = g_openpete_tick_stamp;
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 9, .raw_words_max = 10, .guard = PART_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, part_prim_sink, NULL);
}

/** @brief Unpacks one record point from the pool's position words, displaced by `off`.
 *
 * The first point is X in the low half and Y in the high half of the first
 * word, Z in the low half of the second (partsdraw.s:709-712), each an unsigned
 * halfword in quarter world units. A two-point class's second point occupies
 * the next three halfwords: X in the high half of the second word
 * (partsdraw.s:650-651 subtracts the camera X from it), Y in the low half and Z
 * in the high half of the third (partsdraw.s:653-659 subtracts the camera Y and
 * Z from those two).
 * @param it The captured primitive holding the record's position words.
 * @param second Nonzero selects the second point.
 * @param off Displacement in the pool's quarter world units, or NULL for none.
 * @param p Receives the point (X, Y, Z). */
static void part_rec_point(const part_item* it, int second, const double off[3],
                           double p[3]) {
    if (second) {
        p[0] = (double)((it->w1 >> 16) & 0xFFFFu);
        p[1] = (double)(it->w2 & 0xFFFFu);
        p[2] = (double)((it->w2 >> 16) & 0xFFFFu);
    } else {
        p[0] = (double)(it->w0 & 0xFFFFu);
        p[1] = (double)((it->w0 >> 16) & 0xFFFFu);
        p[2] = (double)(it->w1 & 0xFFFFu);
    }
    if (off) { p[0] += off[0]; p[1] += off[1]; p[2] += off[2]; }
}

/** @brief Projects a pool point the way the routine does.
 *
 * The GTE vector is (camY - Y, camZ - Z, X - camX) with the camera position
 * divided by four into the pool's quarter world units, rotated by the camera
 * matrix at +0x00 with a zero translation, through the GTE-faithful projection
 * without the SXY bound.
 * @param v The view; supplies the camera matrix, position and offsets.
 * @param p The point (X, Y, Z) in quarter world units.
 * @param o Receives the projection; o->m3 is the view-Z in quarter units. */
static void part_project(const ngte_view* v, const double p[3], ngte_proj* o) {
    const double cx = v->cam[0] / 4.0, cy = v->cam[1] / 4.0, cz = v->cam[2] / 4.0;
    ngte_project_gte(v->cam_M, NULL, cy - p[1], cz - p[2], p[0] - cx,
                     v->ofx, v->ofy, v, 0, o);
}

/** @brief Builds a PART_ROT_CLASS billboard's four corners in float, as the routine builds them in integers.
 *
 * The routine moves the record point's view-space MACs, shifted left by 2,
 * into the GTE translation (partsdraw.s:529-534), sets the rotation to
 * diag(0x1000, 0xA00, 0x1000) (:567-571), and projects four corners with VZ 0
 * (RTPT at :574 and RTPS at :593): (-a2, -a3), (a3, -a2), (-a3, a2), (a2, a3),
 * where a2 and a3 are the size byte times the sine-table entry at angle + 32
 * and at angle + 96, shifted right by 10 (:495-504, :527, :537-538). The GTE
 * truncates each corner's MACs and screen position to integers, so a distant
 * billboard whose extent spans a few pixels jumps by up to a pixel on every
 * tick independent of its motion. Here every term stays fractional: the MACs
 * of `pt`, the table entry interpolated at a fractional angle
 * (ngte_sincos_f), the product without the shift's truncation, and the
 * projection without the screen rounding. The corner order is the
 * primitive's vertex order (:594-596 store the RTPT's SXY0..2, :612 the RTPS's).
 * @param cpu Reads the sine table.
 * @param v The view the corners project under.
 * @param pt The record point's projection under `v` (m1..m3 its view MACs).
 * @param size The size byte, fractional on a sub-tick present.
 * @param ang The angle byte, fractional on a sub-tick present.
 * @param x Receives the four screen X.
 * @param y Receives the four screen Y. */
static void part_rot_corners(CPUState* cpu, const ngte_view* v, const ngte_proj* pt,
                             double size, double ang, float x[4], float y[4]) {
    double s, c;
    ngte_sincos_f(cpu, ang + 32.0, &s, &c);
    const double a2 = size * s / 1024.0, a3 = size * c / 1024.0;
    const double ox[4] = { -a2, a3, -a3, a2 };
    const double oy[4] = { -a3, -a2, a2, a3 };
    for (int k = 0; k < 4; k++) {
        ngte_proj o;
        ngte_screen_gte(4.0 * pt->m1 + ox[k], 4.0 * pt->m2 + oy[k] * (2560.0 / 4096.0),
                        4.0 * pt->m3, v->ofx, v->ofy, v, 0, &o);
        x[k] = (float)o.sx;
        y[k] = (float)o.sy;
    }
}

/* Records the last pairing walk found stamped with no primitive appended. */
static int s_stamp_skipped;

/** @brief Whether a stamped record failed its class's test between the stamp and the append, so the routine appended nothing for it.
 *
 * Reproduces the routine's test from game_layout.particles.stamp_skip: the
 * record point goes through RTPS as the routine loads it (VXY0 = camera Y and
 * Z minus the record's, VZ0 = record X minus camera X, each a halfword, the
 * camera position shifted right by 2, zero translation), and the ordering key
 * (SZ3 >> 5) - size is compared against the class's key_min.
 * @param cpu Reads the record's position words and size byte.
 * @param v The tick's view; supplies the camera matrix and RAM position.
 * @param rec The stamped record's guest address.
 * @param cls The record's class, below fixed_layer_class.
 * @return 1 when the routine stamped the record and appended nothing. */
static int part_stamp_skipped(CPUState* cpu, const ngte_view* v, uint32_t rec, int cls) {
    const struct game_part_stamp_skip* t = &game_layout.particles.stamp_skip[cls];
    if (!t->on) return 0;
    const int32_t cx = (int32_t)v->cam_ram[0] >> 2;
    const int32_t cy = (int32_t)v->cam_ram[1] >> 2;
    const int32_t cz = (int32_t)v->cam_ram[2] >> 2;
    const uint32_t w0 = cpu->read_word(rec + game_layout.particles.pos_off);
    const uint32_t w1 = cpu->read_word(rec + game_layout.particles.pos_off + 4u);
    const int32_t x = (int32_t)(w0 & 0xFFFFu), y = (int32_t)(w0 >> 16);
    const int32_t z = (int32_t)(w1 & 0xFFFFu);
    static const int tr0[3] = { 0, 0, 0 };
    ngte_rtps_out o;
    ngte_rtps_int(v->cam_M, tr0, (int16_t)(cy - y), (int16_t)(cz - z), (int16_t)(x - cx),
                  v->ofx, v->ofy, (int)v->H_canon, &o);
    const int size = t->size_off ? (int)cpu->read_byte(rec + t->size_off) : 0;
    return (o.sz3 >> 5) - size < t->key_min;
}

/** @brief Pairs each captured primitive with the record that appended it and stores the record's view-Z as a scene depth.
 *
 * The routine appends in pool order and at most one primitive per record, so
 * walking the pool and the captured list together aligns them. A record
 * emitted a primitive when its stamp byte is set and it passes its class's
 * test between the stamp and the append (part_stamp_skipped), or when its
 * class links at the fixed front bucket, where the routine stamps nothing and
 * always appends; the latter take the front depth and keep no record. A paired record keeps its
 * position words, its projected point (and a two-point class's second point)
 * and its view-Z. Every primitive matched to a record, fixed-bucket ones
 * included, takes that record's player_hide_particle_owner. The walk ends at the pool's end sentinel or the last
 * captured primitive, and does nothing when the pool pointer is outside RAM.
 * @param cpu Reads the pool.
 * @param v The tick's view. */
static void part_pair_depths(CPUState* cpu, const ngte_view* v) {
    const uint32_t base = cpu->read_word(game_layout.particles.array_ptr);
    if (base < 0x80000000u || base >= 0x80800000u) return;
    const uint32_t stride = game_layout.particles.rec_stride;
    /* The fixed front bucket is nearer than bucket 0, which the near guard in
     * openpete_native_scene_viewz already maps to the smallest depth the
     * pipeline carries. */
    const float front = openpete_native_scene_viewz(openpete_worldvz_full(1.0f));

    int i = 0;
    s_stamp_skipped = 0;
    for (uint32_t rec = base; i < s_item_n; rec += stride) {
        const int sel = (int)(int16_t)(uint16_t)cpu->read_half(rec + game_layout.particles.sel_off);
        if (sel < 0) {
            if ((sel >> PART_CLASS_SHIFT) == PART_POOL_END) break;
            continue;                                    /* inactive slot */
        }
        const int cls = (sel >> PART_CLASS_SHIFT) & PART_CLASS_MASK;
        if ((uint32_t)cls >= game_layout.particles.fixed_layer_class) {
            s_item[i].cls = cls;
            s_item[i].owner = player_hide_particle_owner(cpu, rec);
            s_item[i++].depth = front;
            continue;
        }
        if (cpu->read_byte(rec + game_layout.particles.drawn_off) == 0) continue;
        if (part_stamp_skipped(cpu, v, rec, cls)) { s_stamp_skipped++; continue; }

        part_item* it = &s_item[i];
        it->cls = cls;
        it->rec = rec;
        it->owner = player_hide_particle_owner(cpu, rec);
        it->w0 = cpu->read_word(rec + game_layout.particles.pos_off);
        it->w1 = cpu->read_word(rec + game_layout.particles.pos_off + 4u);
        it->w2 = cpu->read_word(rec + game_layout.particles.pos_off + 8u);
        double p[3];
        ngte_proj o;
        part_rec_point(it, 0, NULL, p);
        part_project(v, p, &o);
        it->psx = o.sx;
        it->psy = o.sy;
        it->vz  = o.m3;
        it->m1  = o.m1;
        it->m2  = o.m2;
        if (cls == PART_ROT_CLASS && it->p.nv == 4) {
            part_rot_corners(cpu, v, &o, (double)((it->w1 >> 16) & 0xFFu),
                             (double)((it->w1 >> 24) & 0xFFu), it->ex, it->ey);
            it->exact = 1;
        }
        it->depth =
            openpete_native_scene_viewz(openpete_worldvz_scaled((float)o.m3, 4.0f));
        if (cls == PART_TWO_POINT_CLASS) {
            ngte_proj o2;
            part_rec_point(it, 1, NULL, p);
            part_project(v, p, &o2);
            it->psx2 = o2.sx;
            it->psy2 = o2.sy;
        }
        i++;
    }
    /* A polybuf that filled mid-pool ends the routine early, so fewer
     * primitives than stamped records is normal; the loop above simply stops
     * at the last captured primitive. Any primitive the loop could not reach
     * keeps the depth 1.0 (the near guard) that part_prim_sink gave it. */
}

/** @brief Whether the native particle feed runs; OPENPETE_PARTICLES starting with '0' suppresses it alone.
 *
 * The guest draw still runs, so the A/B prices the particles' painted pixels
 * from one binary. The variable is read once and latched.
 * @return 1 when the feed runs, 0 when suppressed. */
static int part_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PARTICLES"); v = !(e && *e && *e == '0'); }
    return v;
}

/* ---- sub-tick rebase ------------------------------------------------------
 *
 * The primitives are the guest's own output, built under the tick's camera
 * from the tick's pool, so a sub-tick present that re-feeds them shows every
 * particle at the pose the tick drew: neither the camera's motion nor the
 * record's own reaches them. The rebase measures each record point's screen
 * displacement as the difference of two projections - the pool position the
 * present shows under the staged camera, against the position the tick drew
 * under the tick's camera - and moves the primitive by it. At the end of the
 * tick's own interval the displaced position and the staged camera are the
 * tick's, so both projections take identical inputs through identical
 * arithmetic and the displacement is exactly zero. */

/* A particle's speed ceiling, in the pool's quarter world units per tick. The
 * table is keyed by slot address, so a slot a new record respawns into inside
 * one tick pairs across two different particles and would lerp as a streak
 * between them. The selector test below catches a reuse that changed class;
 * this catches the rest, at a distance no particle covers in a tick. */
#define PART_REUSE_CEILING 300.0

/* A pair spanning more than this is a discontinuity rather than motion. */
#define PART_PAIR_STALE_NS (g_openpete_tick_period_ns + g_openpete_tick_period_ns / 2)

/* The verdict of the pairing: the primitive is lerped only on PART_PAIR_LERP,
 * and holds its current pose otherwise. */
enum { PART_PAIR_NONE = 0, PART_PAIR_LERP = 1, PART_PAIR_REUSE = 2 };

/** @brief The record's displacement, angle and size change between the pose the tick drew and the pose this present shows.
 *
 * The pair is the record's previous and current snapshots in the interp-object
 * table; the displacement is (prev - curr) · (1 - alpha) in the pool's units.
 * The shown angle lerps the angle byte along the shortest arc of the 256-step
 * circle, and the size delta is the size byte's change scaled the same way. A
 * record with no previous snapshot (one that spawned this tick), a pair
 * further apart than PART_PAIR_STALE_NS, or a blob too short for the position
 * words returns PART_PAIR_NONE; the primitive then holds the tick's pose in the
 * world with only the camera's motion rebasing it.
 *
 * The table is keyed by slot address, so a slot a new record respawns into
 * pairs two different particles; lerping across that pair draws a line from the
 * dead particle's position to the live one's. Three tests separate the reuse
 * from a continuing particle and return PART_PAIR_REUSE, in order of what they
 * catch: the selector halfword changing catches a reuse of a different class
 * or type; the age byte falling catches a reuse that kept both, the pool update
 * having reset it for the new record; the speed ceiling PART_REUSE_CEILING
 * catches the rest, at a distance no particle covers in a tick.
 * @param it The paired captured primitive.
 * @param alpha The present's phase between the two ticks.
 * @param off Receives the displacement; zero unless the verdict is PART_PAIR_LERP.
 * @param prev_ang Receives the shown angle byte, written only on PART_PAIR_LERP.
 * @param size_delta Receives the shown size minus the tick's size; zero unless
 *        the verdict is PART_PAIR_LERP.
 * @return PART_PAIR_LERP, PART_PAIR_NONE or PART_PAIR_REUSE. */
static int part_record_pair(const part_item* it, double alpha, double off[3],
                            double* prev_ang, double* size_delta) {
    off[0] = off[1] = off[2] = 0.0;
    *size_delta = 0.0;
    const uint32_t pos = game_layout.particles.pos_off;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(it->rec);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns)
        return PART_PAIR_NONE;
    if ((uint32_t)o->blob_len < pos + 12u) return PART_PAIR_NONE;
    if (o->curr_ns - o->prev_ns > PART_PAIR_STALE_NS) return PART_PAIR_NONE;
    uint16_t psel, csel;
    memcpy(&psel, o->prev + game_layout.particles.sel_off, 2);
    memcpy(&csel, o->curr + game_layout.particles.sel_off, 2);
    if (psel != csel) return PART_PAIR_REUSE;         /* the slot took a new record */
    if (o->curr[PART_AGE_OFF] < o->prev[PART_AGE_OFF]) return PART_PAIR_REUSE;
    uint32_t pw[3], cw[3];
    memcpy(pw, o->prev + pos, 12);
    memcpy(cw, o->curr + pos, 12);
    /* A record of the previous epoch's set drew the slot's prev state when the
     * slot rolled after that draw: the displayed state is the same, and the
     * offsets are measured from prev, so they gain curr - prev. */
    const double from_prev = (s_item_ref_prev && o->curr_ns > s_item_ref_cut) ? 1.0 : 0.0;
    const double d[3] = {
        (double)(pw[0] & 0xFFFFu)        - (double)(cw[0] & 0xFFFFu),
        (double)((pw[0] >> 16) & 0xFFFFu) - (double)((cw[0] >> 16) & 0xFFFFu),
        (double)(pw[1] & 0xFFFFu)        - (double)(cw[1] & 0xFFFFu),
    };
    if (d[0]*d[0] + d[1]*d[1] + d[2]*d[2] > PART_REUSE_CEILING * PART_REUSE_CEILING)
        return PART_PAIR_REUSE;
    const double rest = 1.0 - alpha;
    for (int k = 0; k < 3; k++) off[k] = d[k] * (rest - from_prev);
    /* The angle the corner extents are built from is the top byte of the
     * second position word (partsdraw.s:497-501 shifts it right by 23 and
     * masks the halfword index to 0x1FE, which drops the low bit), and the
     * shortest arc of the 256-step circle is the way round the record turned. */
    const double pa = (double)((pw[1] >> 24) & 0xFFu);
    const double ca = (double)((cw[1] >> 24) & 0xFFu);
    double da = pa - ca;
    while (da > 128.0) da -= 256.0;
    while (da < -128.0) da += 256.0;
    *prev_ang = ca + da * rest;   /* absolute: the corner rotation measures from the record's own angle */
    /* The size byte the corner extents are built from is byte 2 of the same
     * word (partsdraw.s:495-496 takes (w1 >> 16) & 0xFF and :521 loads it into
     * IR0 ahead of the sine pair in IR1 and IR2), so the extent this present
     * shows is the tick's size plus this much. */
    *size_delta = ((double)((pw[1] >> 16) & 0xFFu) - (double)((cw[1] >> 16) & 0xFFu)) * (rest - from_prev);
    return PART_PAIR_LERP;
}

/** @brief The rotation carrying the corner extents from the angle the tick built them at to the angle this present shows.
 *
 * The result is the cos/sin of the change. The routine indexes the game's own
 * sine table at (angle + 0x40) & 0x1FE and takes the quarter-circle entry 0x80
 * bytes on (partsdraw.s:497-504), so both angles are offset by 32 entries and
 * handed to ngte_table_rot: the extents ride the table's vector and the change
 * is the rotation between the two table vectors, exact whatever the table's
 * phase and sense.
 * @param cpu Reads the game's sin/cos table.
 * @param tick_ang The angle byte the tick drew with.
 * @param show_ang The angle byte this present shows.
 * @param rc Receives the cos of the change.
 * @param rs Receives the sin of the change.
 * @return ngte_table_rot's verdict: 1 when a rotation was computed. */
static int part_corner_rot(CPUState* cpu, double tick_ang, double show_ang,
                           double* rc, double* rs) {
    return ngte_table_rot(cpu, tick_ang + 32.0, show_ang + 32.0, rc, rs);
}

/* OPENPETE_PART_REBASE_DBG=1: the rebase's verdicts per run, by class. A class
 * that only ever snaps has no pairs in the store; a run whose lerped count is
 * zero rebased nothing and measured nothing. `reuse` is the subset of the
 * snapped whose slot was found holding a different record than the pair's
 * previous blob, which is what a rising reuse count against a steady lerp count
 * separates from a pool that is simply churning. */
static unsigned long s_reb_stat[4][16];   /* 0 lerped, 1 snapped, 2 rotated, 3 reused */
static int s_reb_dbg;   /* 2 = one line per rebased primitive */

#define PART_SCALE_MIN 0.25
#define PART_SCALE_MAX 4.0

/** @brief The scale carrying a primitive's corner offsets from the tick's extent to the shown extent.
 *
 * The routine builds a billboard's half-extents as (size * sine) >> 10 in view
 * space and divides all four corners by the record point's own view-Z
 * (partsdraw.s:521-523 loads the size byte and the sine pair into IR0..IR2,
 * :527 multiplies them, :537-538 shifts the products right by 10 and :574
 * projects the four corners against one translation), so the screen extent is
 * proportional to the size byte and inversely proportional to that view-Z.
 * Both terms move inside a tick and the captured corners carry neither: the
 * scale is their ratio between the shown state and the tick's.
 *
 * A two-point class's two endpoints are two record points, each translated by
 * its own displacement, so it has no offsets about a single point and holds
 * the identity.
 *
 * At the end of the tick's own interval the size delta is zero and the shown
 * view-Z is the projection of the tick's own inputs through the same
 * arithmetic, so both ratios are exactly one and the corners are untouched.
 *
 * The band [PART_SCALE_MIN, PART_SCALE_MAX] is the discontinuity guard the
 * displacement gets from PART_REUSE_CEILING: a view-Z that moved a particle's
 * extent by more than a factor of four inside one tick is a camera cut or a
 * slot reuse, where holding the tick's extent is what every other term of the
 * rebase does.
 * @param it The captured primitive; supplies the tick's view-Z and size byte.
 * @param shown The projection of the record point the present shows; `m3` is
 *        its view-Z.
 * @param size_delta The shown size byte minus the tick's.
 * @return The scale; 1.0 for a two-point class, a non-positive view-Z, a zero
 *         tick size under a nonzero delta, or a scale outside the band. */
static double part_extent_scale(const part_item* it, const ngte_proj* shown,
                                double size_delta) {
    if (it->cls == PART_TWO_POINT_CLASS) return 1.0;
    if (!(it->vz > 0.0) || !(shown->m3 > 0.0)) return 1.0;
    double scl = it->vz / shown->m3;
    if (size_delta != 0.0) {
        const double tick_size = (double)((it->w1 >> 16) & 0xFFu);
        if (!(tick_size > 0.0)) return 1.0;
        scl *= (tick_size + size_delta) / tick_size;
    }
    if (!(scl >= PART_SCALE_MIN) || !(scl <= PART_SCALE_MAX)) return 1.0;
    return scl;
}

/** @brief Fills s_reb[i] with the rebase of captured primitive i onto the pose this present shows.
 *
 * A primitive with no record (the fixed front bucket, or unpaired) gets an
 * inactive rebase. Otherwise the record's displaced point (part_record_pair)
 * is projected under `v`, and its screen displacement from the tick's own
 * projection and its depth are stored; a two-point class does the same for its
 * second point. A lerped pair of the rotating class also gets the corner
 * rotation, and every such primitive the extent scale. A pair that is not
 * lerped holds the tick's record position, so only the camera's motion
 * displaces it. Adds to the s_reb_stat verdict counts, and at
 * OPENPETE_PART_REBASE_DBG 2 or more prints one line.
 * @param cpu Reads the sin/cos table.
 * @param v The view under the staged camera.
 * @param alpha The present's phase between the two ticks.
 * @param i The index into s_item. */
static void part_rebase_item(CPUState* cpu, const ngte_view* v, double alpha, int i) {
    part_item* it = &s_item[i];
    part_rebase* r = &s_reb[i];
    memset(r, 0, sizeof *r);
    r->scl = 1.0;
    if (it->cls < 0 || !it->rec) return;         /* the fixed front bucket */
    double off[3], show_ang = -1.0, size_delta = 0.0;
    const int verdict = part_record_pair(it, alpha, off, &show_ang, &size_delta);
    const int paired = (verdict == PART_PAIR_LERP);
    s_reb_stat[paired ? 0 : 1][it->cls & 0xF]++;
    if (verdict == PART_PAIR_REUSE) s_reb_stat[3][it->cls & 0xF]++;
    double p[3];
    ngte_proj o;
    part_rec_point(it, 0, off, p);
    part_project(v, p, &o);
    r->dx = (float)(o.sx - it->psx);
    r->dy = (float)(o.sy - it->psy);
    r->depth = openpete_native_scene_viewz(openpete_worldvz_scaled((float)o.m3, 4.0f));
    r->active = 1;
    if (it->cls == PART_TWO_POINT_CLASS) {
        ngte_proj o2;
        part_rec_point(it, 1, off, p);
        part_project(v, p, &o2);
        r->dx2 = (float)(o2.sx - it->psx2);
        r->dy2 = (float)(o2.sy - it->psy2);
    }
    if (s_reb_dbg >= 2)
        fprintf(stderr, "[part-reb f=%u rec=%08X c=%d v=%d off=(%.2f,%.2f,%.2f) "
                        "ps=(%.2f,%.2f) new=(%.2f,%.2f) d=(%.2f,%.2f) d2=(%.2f,%.2f)\n",
                openpete_frame, it->rec, it->cls, verdict, off[0], off[1], off[2],
                it->psx, it->psy, o.sx, o.sy, (double)r->dx, (double)r->dy,
                (double)r->dx2, (double)r->dy2);
    if (it->exact) {
        /* The float-built corners rebuild at the shown point, angle and size;
         * at the end of the tick's interval those are the tick's own inputs
         * and the corners equal the captured ones. */
        const double tick_ang = (double)((it->w1 >> 24) & 0xFFu);
        const double tick_size = (double)((it->w1 >> 16) & 0xFFu);
        part_rot_corners(cpu, v, &o, tick_size + size_delta, paired ? show_ang : tick_ang,
                         r->cx, r->cy);
        r->corners = 1;
        r->dsz = (size_delta != 0.0) ? 1u : 0u;
        return;
    }
    if (paired && it->cls == PART_ROT_CLASS) {
        const double tick_ang = (double)((it->w1 >> 24) & 0xFFu);
        r->rot = part_corner_rot(cpu, tick_ang, show_ang, &r->rc, &r->rs)
                     ? 1u : 0u;
        s_reb_stat[2][it->cls & 0xF] += r->rot;
    }
    r->dsz = (size_delta != 0.0) ? 1u : 0u;
    r->scl = part_extent_scale(it, &o, size_delta);
}

/** @brief Prints a view's camera, offsets, H, scale and matrix row to stderr.
 *
 * @param tag The label printed in the line.
 * @param v The view. */
static void part_view_dbg(const char* tag, const ngte_view* v) {
    fprintf(stderr, "[part-view f=%u %s] cam=(%.2f,%.2f,%.2f) ram=(%.2f,%.2f,%.2f) "
                    "interp=%d of=(%.3f,%.3f) H=%.2f/%.2f s=%.4f F=%.4f M0=(%d,%d,%d)\n",
            openpete_frame, tag, v->cam[0], v->cam[1], v->cam[2],
            v->cam_ram[0], v->cam_ram[1], v->cam_ram[2], v->cam_interp,
            v->ofx, v->ofy, v->H_canon, v->H_eff, v->s, v->F,
            v->cam_M[0][0], v->cam_M[0][1], v->cam_M[0][2]);
}

/** @brief Prints the per-class rebase verdict counts to stderr; registered to run at exit.
 *
 * A class with no lerped and no snapped primitive is omitted. */
static void part_rebase_report(void) {
    fprintf(stderr, "[part-rebase]");
    for (int c = 0; c < 16; c++)
        if (s_reb_stat[0][c] || s_reb_stat[1][c])
            fprintf(stderr, " c%d:lerp=%lu snap=%lu rot=%lu reuse=%lu", c,
                    s_reb_stat[0][c], s_reb_stat[1][c], s_reb_stat[2][c],
                    s_reb_stat[3][c]);
    fprintf(stderr, "\n");
}

/** @brief Latches OPENPETE_PART_REBASE_DBG into s_reb_dbg on the first call; nonzero registers the per-class verdict report at exit. */
static void part_dbg_latch(void) {
    static int dbg = -1;
    if (dbg >= 0) return;
    const char* e = getenv("OPENPETE_PART_REBASE_DBG");
    dbg = (e && *e && *e != '0');
    s_reb_dbg = e ? atoi(e) : 0;
    if (dbg) atexit(part_rebase_report);
}

/** @brief Rebases the captured set onto the pose this present shows.
 *
 * The staged camera resolves through the sub-tick view, so building a view
 * here reads the interpolated transform the rest of the pass draws under.
 * At OPENPETE_PART_REBASE_DBG 2 or more (part_dbg_latch) it prints the view
 * and one line per rebased primitive.
 * @param cpu Reads the staged camera and the sin/cos table.
 * @param alpha The present's phase between the two ticks. */
static void part_rebase_all(CPUState* cpu, double alpha) {
    if (s_item_n <= 0) return;
    part_dbg_latch();
    ngte_view v;
    ngte_view_build(cpu, &v);
    if (s_reb_dbg >= 2) part_view_dbg("staged", &v);
    for (int i = 0; i < s_item_n; i++) part_rebase_item(cpu, &v, alpha, i);
}

/** @brief Carries one corner through its primitive's rebase.
 *
 * When the rebase rotates or scales, the corner's offset from the first record
 * point is rotated by (rc, rs) and then scaled by scl; the corner then
 * translates by the chosen record point's screen displacement. An inactive
 * rebase leaves the corner unchanged.
 * @param it The captured primitive; supplies the first record point.
 * @param r The primitive's rebase.
 * @param second Nonzero translates by the second record point's displacement.
 * @param x The corner's screen X, updated in place.
 * @param y The corner's screen Y, updated in place. */
static void part_rebase_xy(const part_item* it, const part_rebase* r, int second,
                           float* x, float* y) {
    if (!r->active) return;
    if (r->rot || r->scl != 1.0) {
        double ox = (double)*x - it->psx;
        double oy = (double)*y - it->psy;
        if (r->rot) {
            const double rx = ox * r->rc + oy * r->rs;
            oy = -ox * r->rs + oy * r->rc;
            ox = rx;
        }
        *x = (float)(it->psx + ox * r->scl);
        *y = (float)(it->psy + oy * r->scl);
    }
    *x += second ? r->dx2 : r->dx;
    *y += second ? r->dy2 : r->dy;
}

/** @brief Feeds a LINE_F2 or LINE_G2 primitive as a one-pixel-wide quad across its own perpendicular.
 *
 * A line has no area, so the quad extends half a pixel to each side, the width
 * the GPU rasterizes it at; a line shorter than 1e-3 pixels widens along X.
 * The endpoints go through the rebase, the second by the second record point
 * for a two-point class. A gouraud line colours each endpoint from its own
 * word; the semi-transparency bit selects blend code 1.
 * @param it The captured primitive.
 * @param r Its rebase.
 * @param depth The scene depth to feed at.
 * @param gouraud Nonzero for LINE_G2. */
static void part_feed_line(const part_item* it, const part_rebase* r,
                           float depth, int gouraud) {
    const ngte_prim* p = &it->p;
    const uint32_t w1 = p->raw[1];
    const int stp = (int)((w1 >> 24) & 0x02u);
    const uint32_t rgb0 = w1 & 0xFFFFFFu;
    uint32_t xy0, xy1, rgb1;
    if (gouraud) { xy0 = p->raw[2]; rgb1 = p->raw[3] & 0xFFFFFFu; xy1 = p->raw[4]; }
    else         { xy0 = p->raw[2]; rgb1 = rgb0;                   xy1 = p->raw[3]; }
    float ax = (float)(int16_t)(xy0 & 0xFFFFu), ay = (float)(int16_t)(xy0 >> 16);
    float bx = (float)(int16_t)(xy1 & 0xFFFFu), by = (float)(int16_t)(xy1 >> 16);
    /* A two-point class's endpoints are two record points, each with its own
     * displacement; every other class offsets both from the one point. */
    const int two = (it->cls == PART_TWO_POINT_CLASS);
    part_rebase_xy(it, r, 0, &ax, &ay);
    part_rebase_xy(it, r, two, &bx, &by);
    s_fed_tris += (uint32_t)ngte_feed_flat_line(ax, ay, bx, by, rgb0, rgb1, depth, stp ? 1 : 0);
}

/* The phase of the present the next feed draws: the rebase's alpha, or -1
 * for the tick's own capture. */
static double s_feed_alpha = -1.0;

/** @brief At OPENPETE_PART_REBASE_DBG 3 or more, prints one line per fed quad of a paired record: its drawn centroid and the point the record shows.
 *
 * The shown point is the record point's projection under the present's
 * camera (the tick's own projection on a canonical feed); the drawn centroid
 * minus it is the quad's placement error, whose change between consecutive
 * presents is the motion the record does not make. The line also carries the
 * centroid of the guest's integer primitive and the record point's projection
 * under the tick's camera, whose difference calibrates the float corners
 * against the routine's own.
 * @param it The captured primitive.
 * @param r Its rebase.
 * @param x The four corner X the feed draws.
 * @param y The four corner Y the feed draws. */
static void part_track_dbg(const part_item* it, const part_rebase* r,
                           const float x[4], const float y[4]) {
    if (s_reb_dbg < 3 || !it->rec) return;
    const double gx = (x[0] + x[1] + x[2] + x[3]) / 4.0, gy = (y[0] + y[1] + y[2] + y[3]) / 4.0;
    const double ax = it->psx + (r->active ? r->dx : 0.0f);
    const double ay = it->psy + (r->active ? r->dy : 0.0f);
    const ngte_prim* p = &it->p;
    const int last = p->nv - 1;
    const double px = (p->x[0] + p->x[1] + p->x[2] + p->x[last]) / 4.0;
    const double py = (p->y[0] + p->y[1] + p->y[2] + p->y[last]) / 4.0;
    fprintf(stderr, "[part-track f=%u a=%.3f rec=%08X c=%d drawn=(%.3f,%.3f) shown=(%.3f,%.3f) "
                    "guest=(%.3f,%.3f) tick=(%.3f,%.3f)]\n",
            openpete_frame, s_feed_alpha, it->rec, it->cls, gx, gy, ax, ay, px, py,
            it->psx, it->psy);
}

/** @brief The four corners the feed draws a captured polygon at.
 *
 * In order of precedence: the corners the rebase rebuilt at the shown state,
 * the float corners built at capture, or the primitive's own corners carried
 * through the rebase. A triangle fills the unread corner 3 with its last
 * corner.
 * @param it The captured primitive, a polygon.
 * @param r Its rebase.
 * @param x Receives the four corner X.
 * @param y Receives the four corner Y. */
static void part_quad_corners(const part_item* it, const part_rebase* r, float x[4], float y[4]) {
    if (r->active && r->corners) {
        memcpy(x, r->cx, 4 * sizeof x[0]);
        memcpy(y, r->cy, 4 * sizeof y[0]);
        return;
    }
    if (it->exact) {
        memcpy(x, it->ex, 4 * sizeof x[0]);
        memcpy(y, it->ey, 4 * sizeof y[0]);
        return;
    }
    const ngte_prim* p = &it->p;
    const int last = p->nv - 1;
    const int idx[4] = { 0, 1, 2, last };
    for (int j = 0; j < 4; j++) {
        x[j] = p->x[idx[j]];
        y[j] = p->y[idx[j]];
        part_rebase_xy(it, r, 0, &x[j], &y[j]);
    }
}

/** @brief Feeds every captured primitive to the native scene: polygons as quads, lines as thin quads.
 *
 * Does nothing when the feed is suppressed or the captured set is empty.
 * Begins the scene at openpete_frame and takes each primitive's depth from its
 * rebase when active, else from its pairing. A textured polygon's blend comes
 * from its ABR when semi-transparent (rate 3 drawn additive), an untextured
 * one's is 1 when semi-transparent; corners pass through the rebase. A code in
 * the 0x40 or 0x50 line ranges goes to part_feed_line; any other code is
 * ignored. While player_hide_active holds, a primitive whose record the
 * player's or the companion's update spawned is not fed. Clears the scene
 * tag on exit. */
static void part_feed_native(void) {
    if (!part_feed_enabled()) return;
    if (s_item_n <= 0) return;
    const int hide_player = player_hide_active();
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_item_n; i++) {
        const part_item* it = &s_item[i];
        const part_rebase* r = &s_reb[i];
        const ngte_prim* p = &it->p;
        player_hide_count_data(it->owner == 1 ? PH_CAT_PLAYER_PART
                               : it->owner == 2 ? PH_CAT_SPARX_PART : PH_CAT_OTHER_PART,
                               it->owner && hide_player, p, sizeof *p);
        if (it->owner && hide_player) continue;
        const float depth = r->active ? r->depth : it->depth;
        if (p->nv == 4 || p->nv == 3) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_QUAD, (uint32_t)p->code, 0));
            ngte_tpage tp;
            const int blend = ngte_prim_tpage_blend(p, &tp);
            /* A triangle fills the unread corner 3 with its last corner. */
            const int last = p->nv - 1;
            float x[4], y[4];
            part_quad_corners(it, r, x, y);
            part_track_dbg(it, r, x, y);
            const float u[4] = { (float)p->u[0], (float)p->u[1], (float)p->u[2], (float)p->u[last] };
            const float vt[4] = { (float)p->v[0], (float)p->v[1], (float)p->v[2], (float)p->v[last] };
            const uint32_t c[4] = { p->rgb[0], p->rgb[1], p->rgb[2], p->rgb[last] };
            s_fed_tris += (uint32_t)ngte_feed_flat_quad(x, y, u, vt, c, depth,
                                                        p->textured ? &tp : NULL, blend, p->nv);
        } else if ((p->code & 0xF8u) == 0x40u || (p->code & 0xF8u) == 0x50u) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_LINE, (uint32_t)p->code, 0));
            part_feed_line(it, r, depth, (p->code & 0x10u) != 0);
        }
    }
    openpete_native_scene_set_tag(0);
}

/** @brief The mean of a captured primitive's screen vertices.
 *
 * A polygon averages its decoded vertices; a line averages its two endpoints
 * from the raw words, where a gouraud line carries a colour word before each.
 * @param p The captured primitive.
 * @param gx Receives the mean X.
 * @param gy Receives the mean Y. */
static void part_item_centroid(const ngte_prim* p, double* gx, double* gy) {
    double sx = 0, sy = 0;
    if (p->nv) {
        for (int k = 0; k < p->nv; k++) { sx += p->x[k]; sy += p->y[k]; }
        *gx = sx / p->nv; *gy = sy / p->nv;
        return;
    }
    const int words = (p->code & 0x10u) ? 3 : 2;
    for (int k = 0; k < 2; k++) {
        const uint32_t xy = p->raw[2 + (uint32_t)k * (uint32_t)(words - 1)];
        sx += (double)(int16_t)(xy & 0xFFFFu);
        sy += (double)(int16_t)(xy >> 16);
    }
    *gx = sx / 2.0; *gy = sy / 2.0;
}

/** @brief The screen point a correctly paired primitive's centroid sits at: the record's projected point, or the midpoint of the two-point class's two.
 *
 * The routine offsets a billboard's corners around the record point, and
 * draws a two-point record as the line between its two projected points
 * (partsdraw.s:683-684), whose centroid is their midpoint.
 * @param it The paired item.
 * @param ax Receives the X.
 * @param ay Receives the Y. */
static void part_item_anchor(const part_item* it, double* ax, double* ay) {
    const int two = it->cls == PART_TWO_POINT_CLASS;
    *ax = two ? (it->psx + it->psx2) / 2.0 : it->psx;
    *ay = two ? (it->psy + it->psy2) / 2.0 : it->psy;
}

/** @brief Prints the particle census line when openpete_frame lies in the OPENPETE_PARTICLE_CENSUS range.
 *
 * The range is LO-HI or a single openpete_frame value. Per call it prints the
 * primitives the routine appended by GPU code, the native triangles this
 * feeder emitted from them, and per class the calibration the depth pairing
 * rests on: the mean and worst Manhattan distance between the point each
 * paired record anchors (part_item_anchor) and the centroid of the primitive
 * the pairing gave it, followed by the unpaired count and the stamped records
 * the routine appended nothing for. A correct pairing (and a correct camera
 * matrix) keeps the distance small and a wrong one scatters it across the
 * screen. A capture whose fed count is zero measured nothing.
 * @param start Guest address of the polybuf interval's start.
 * @param end Guest address one past its end.
 * @param fed_before s_fed_tris before this capture's feed. */
static void part_census(uint32_t start, uint32_t end, uint32_t fed_before) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_PARTICLE_CENSUS", openpete_frame)) return;
    fprintf(stderr, "[part-census f=%u] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            openpete_frame, start, end, end - start, s_item_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_item_n; i++) seen[s_item[i].p.code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    fprintf(stderr, " fed_tris=%u pairs:", s_fed_tris - fed_before);
    for (int cl = 0; cl < 16; cl++) {
        int n = 0; double sum = 0, worst = 0;
        for (int i = 0; i < s_item_n; i++) {
            const part_item* it = &s_item[i];
            if (it->cls != cl || (uint32_t)cl >= game_layout.particles.fixed_layer_class) continue;
            double gx, gy, ax, ay;
            part_item_centroid(&it->p, &gx, &gy);
            part_item_anchor(it, &ax, &ay);
            const double d = fabs(gx - ax) + fabs(gy - ay);
            sum += d; if (d > worst) worst = d; n++;
        }
        if (n) fprintf(stderr, " c%d:n=%d mean=%.2f worst=%.1f", cl, n, sum / n, worst);
    }
    int unpaired = 0;
    for (int i = 0; i < s_item_n; i++) if (s_item[i].cls < 0) unpaired++;
    fprintf(stderr, " unpaired=%d stamp_skipped=%d\n", unpaired, s_stamp_skipped);
}

/** @brief The distance from the record's own projected point to the primitive's furthest vertex, in pixels.
 *
 * A polygon measures every vertex; a line measures its two endpoints from the
 * raw words, where a gouraud line carries a colour word between them.
 * @param it The captured primitive.
 * @return The largest vertex distance. */
static double part_item_extent(const part_item* it) {
    const ngte_prim* p = &it->p;
    double worst = 0;
    if (p->nv) {
        for (int k = 0; k < p->nv; k++) {
            const double dx = p->x[k] - it->psx, dy = p->y[k] - it->psy;
            const double d = sqrt(dx * dx + dy * dy);
            if (d > worst) worst = d;
        }
        return worst;
    }
    const int words = (p->code & 0x10u) ? 3 : 2;
    for (int k = 0; k < 2; k++) {
        const uint32_t xy = p->raw[2 + (uint32_t)k * (uint32_t)(words - 1)];
        const double dx = (double)(int16_t)(xy & 0xFFFFu) - it->psx;
        const double dy = (double)(int16_t)(xy >> 16) - it->psy;
        const double d = sqrt(dx * dx + dy * dy);
        if (d > worst) worst = d;
    }
    return worst;
}

/** @brief OPENPETE_PART_EXTENT_CENSUS: per present, per class, the decomposition of a primitive's painted extent into the terms a sub-tick present could carry.
 *
 * `ext` is the mean part_item_extent, which is the extent the routine built
 * from the record's state under the tick's camera; `vz` is the record point's
 * mean view-Z, whose reciprocal scales every extent; `scl` is the mean rebase
 * scale; `dsz` counts the records whose size byte differs between the pair's
 * two states. A step in `ext` between two canonical presents at a steady `n`
 * is the primitive's own extent moving; a step in `n` at a steady `ext` is the
 * pool's population, which no rebase of the captured set can carry. The
 * variable is latched on the first call.
 * @param alpha The present's phase, printed; the tick's own capture passes -1. */
static void part_extent_census(double alpha) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_PART_EXTENT_CENSUS"); s = (e && *e && *e != '0'); }
    if (!s) return;
    char buf[512]; int n = 0;
    for (int cl = 0; cl < 16; cl++) {
        int cnt = 0, dsz = 0; double ext = 0, vz = 0, scl = 0;
        for (int i = 0; i < s_item_n; i++) {
            const part_item* it = &s_item[i];
            if (it->cls != cl || !it->rec) continue;
            cnt++; ext += part_item_extent(it); vz += it->vz;
            scl += s_reb[i].active ? s_reb[i].scl : 1.0;
            if (s_reb[i].dsz) dsz++;
        }
        if (cnt) n += snprintf(buf + n, sizeof buf - (size_t)n,
                               " c%d:n=%d ext=%.2f vz=%.1f scl=%.4f dsz=%d",
                               cl, cnt, ext / cnt, vz / cnt, scl / cnt, dsz);
    }
    fprintf(stderr, "[part-ext f=%u a=%.3f prims=%d]%s\n",
            openpete_frame, alpha, s_item_n, buf);
}

/* The set a held present shows (cut_held_set_pick), or -1 outside a hold. */
static int s_item_held = -1;

int particles_replay_prev(int on) {
    if (!on) {
        if (s_item_held == CUT_HELD_PREV) part_item_swap();
        s_item_ref_prev = 0;
        s_item_held = -1;
        return 0;
    }
    if (s_item_held >= 0) return s_item_held != CUT_HELD_EMPTY;
    const cut_held_pick_t pick = cut_held_set_pick(s_item_epoch, s_item_prev.epoch);
    s_item_held = (int)pick;
    if (pick == CUT_HELD_PREV) {
        s_item_ref_cut = s_item_ref_ns;
        part_item_swap();
        s_item_ref_prev = 1;
    } else if (pick == CUT_HELD_CURR) {
        /* No capture in tick N: the moby cull's stamp is that tick's draw stamp. */
        s_item_ref_prev = moby_native_canon_ref_ns(&s_item_ref_cut);
    }
    cut_census_held_set("particles", pick, s_item_epoch, pick == CUT_HELD_EMPTY ? 0 : s_item_n);
    return pick != CUT_HELD_EMPTY;
}

/* Sub-tick re-extract of the captured particle set; see particles.h. */
void particles_reextract(CPUState* cpu, int bracketed, double alpha) {
    if (!hook_reextract_ready_held(RP_PARTICLES, s_item_held)) return;
    if (bracketed) part_rebase_all(cpu, alpha);
    s_feed_alpha = bracketed ? alpha : -1.0;
    part_extent_census(alpha);
    render_material_level_push(cpu);
    part_feed_native();
}

/* ---- hook ----------------------------------------------------------------- */
static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_PARTICLES, .prof_tag = "particles.srend",
    .st = &s_hb,
};

/** @brief The particle draw hook: runs the guest draw once, then captures and feeds its primitives.
 *
 * Through hook_bracket_run the guest routine runs unchanged and reports the
 * polybuf interval it appended. When the interval is not empty the primitives
 * are captured, the tick's view is built, each primitive is paired with the
 * pool record that appended it (part_pair_depths), the census lines print, and
 * the native scene is fed when it is live and RP_PARTICLES is enabled. Guest
 * memory is read after the draw, never written.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the bracket declines and the ordinary dispatch must run the
 *         draw, else 1: the draw has run. */
static int on_particles_draw(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    if (endp > startp) {
        part_capture_prims(cpu, startp, endp);
        const uint32_t fed_before = s_fed_tris;
        ngte_view view;
        ngte_view_build(cpu, &view);
        part_dbg_latch();
        if (s_reb_dbg >= 2) part_view_dbg("tick", &view);
        part_pair_depths(cpu, &view);
        part_extent_census(-1.0);
        render_material_level_push(cpu);
        s_feed_alpha = -1.0;
        if (feed && render_path_enabled(RP_PARTICLES)) part_feed_native();
        part_census(startp, endp, fed_before);
    }
    return hook_bracket_end(&k_bracket);
}

/* The hook address is the layout's particle draw site, bound at boot: a static
 * initializer cannot read another translation unit's const. */
static feature_dispatch_entry_t particles_dispatch[] = {
    { 0u, on_particles_draw, 0u },
    { 0u, NULL, 0u },
};

/** @brief Binds the dispatch table's hook address to the layout's particle draw site.
 *
 * @param opts Unused. */
static void particles_apply_options(const openpete_options_t* opts) {
    (void)opts;
    particles_dispatch[0].addr = game_layout.particles.hook;
}

const feature_module_t particles_module = {
    .name           = "particles_native",
    .apply_options  = particles_apply_options,
    .dispatch_table = particles_dispatch,
};
