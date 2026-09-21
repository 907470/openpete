// games/common/renderer/particles.c — the particle feeder for a game whose
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
// copy of a six-class projection. What the primitives do not carry is depth —
// the PSX orders them by ordering-table bucket — so the feeder recovers each
// primitive's view-Z by projecting its own record's position, pairing
// primitives to records through the stamp byte the routine maintains
// (partsdraw.s:223 clears it when a record's processing starts, :358, :415,
// :528, :693 and :745 set it once the primitive has been appended), which
// makes the pairing the routine's own invariant rather than a match on
// geometry. Class `fixed_layer_class` and up link at a fixed front bucket
// (partsdraw.s:202) and stamp nothing, so those primitives take the near
// depth that bucket means and are paired by class alone.
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
 * (partsdraw.s:636-652 reads +0xC for the second X/Y and the high half of +0x8
 * for its Z, then stores the two screen points at polybuf +0x8 and +0x10).
 * Every other paired class carries one record point.
 *
 * `PART_ROT_CLASS` is the class whose corner extents are a rotation of a fixed
 * pair of view-space offsets around the record's own view position, all four
 * corners sharing that position's Z (partsdraw.s:497-504 reads the angle,
 * :545-566 builds the four offsets from the sine pair and :571-580 projects
 * them with VZ 0 against the record's translation). A shared Z makes the
 * perspective divide common to the four corners, so rotating their screen
 * offsets about the projected record point is that construction exactly. The
 * other class that reads an angle (partsdraw.s:225-228) rotates in world space
 * and projects each corner on its own Z, which a screen-space rotation is not,
 * so it rebases by translation alone. */
#define PART_TWO_POINT_CLASS 1
#define PART_ROT_CLASS       2

typedef struct {
    ngte_prim p;
    float     depth;
    double    vz;         /* the record point's view-Z under the tick's camera */
    double    psx, psy;   /* the record's own projected point, or the sentinel */
    double    psx2, psy2; /* the second record point of a two-point class */
    int       cls;        /* the record's class, or -1 when unpaired */
    uint32_t  rec;        /* the record that appended it, 0 when unpaired */
    uint32_t  w0, w1, w2; /* the record's position words as the tick read them */
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
} part_rebase;

static part_item   s_item[PART_MAX_PRIMS];
static part_rebase s_reb[PART_MAX_PRIMS];
static int         s_item_n;

static uint32_t s_code_hist[256];
static uint32_t s_tag_hist[256];
static uint32_t s_fed_tris;      /* native triangles the last feed emitted */

static int part_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= PART_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 0.0f;
    s_item[s_item_n].psx = s_item[s_item_n].psy = 0.0;
    s_item[s_item_n].psx2 = s_item[s_item_n].psy2 = 0.0;
    s_item[s_item_n].cls = -1;
    s_item[s_item_n].rec = 0u;
    s_item[s_item_n].w0 = s_item[s_item_n].w1 = s_item[s_item_n].w2 = 0u;
    memset(&s_reb[s_item_n], 0, sizeof s_reb[0]);
    s_reb[s_item_n].scl = 1.0;
    s_item_n++;
    return 1;
}

/* Walk the interval the routine allocated. The longest primitive it appends
 * is the ten-word POLY_FT4 (tag length 9), and the line classes need their
 * raw words because a line is not a polygon and decodes here. */
static void part_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 9, .raw_words_max = 10, .guard = PART_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, part_prim_sink, NULL);
}

/* Pair each captured primitive with the record that appended it and store the
 * record's view-Z as a scene depth. The routine appends in pool order and at
 * most one primitive per record, so walking the pool and the captured list
 * together aligns them; a record emitted when its stamp is set, or when its
 * class links at the fixed front bucket, where the routine stamps nothing and
 * always appends.
 *
 * The record position is three unsigned halves in quarter world units
 * (partsdraw.s:709-712), and the routine builds the GTE vector from it the
 * way every draw in this engine does: (camY - Y, camZ - Z, X - camX) with the
 * camera position shifted right by two, rotated by the camera matrix at
 * +0x00 with a zero translation. */
/* The pool's position words, as the record packs them: X in the low half and Y
 * in the high half of the first word, Z in the low half of the second
 * (partsdraw.s:709-712). A two-point class's second point is packed one
 * halfword on from the first: X in the high half of the second word
 * (partsdraw.s:650-651 subtracts the camera X from it), Y in the low half and Z
 * in the high half of the third (partsdraw.s:653-659 subtracts the camera Y and
 * Z from those two). `off` displaces the point in the pool's own quarter world
 * units. */
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

/* Project a pool point the way the routine does: the GTE vector is
 * (camY - Y, camZ - Z, X - camX) with the camera position shifted right by
 * two, rotated by the camera matrix at +0x00 with a zero translation. */
static void part_project(const ngte_view* v, const double p[3], ngte_proj* o) {
    const double cx = v->cam[0] / 4.0, cy = v->cam[1] / 4.0, cz = v->cam[2] / 4.0;
    ngte_project_gte(v->cam_M, NULL, cy - p[1], cz - p[2], p[0] - cx,
                     v->ofx, v->ofy, v, 0, o);
}

static void part_pair_depths(CPUState* cpu, const ngte_view* v) {
    const uint32_t base = cpu->read_word(game_layout.particles.array_ptr);
    if (base < 0x80000000u || base >= 0x80800000u) return;
    const uint32_t stride = game_layout.particles.rec_stride;
    /* The fixed front bucket is nearer than bucket 0, which the near guard in
     * openpete_native_depth_from_viewz already maps to the smallest depth the
     * pipeline carries. */
    const float front = openpete_native_depth_from_viewz(openpete_worldvz_full(1.0f));

    int i = 0;
    for (uint32_t rec = base; i < s_item_n; rec += stride) {
        const int sel = (int)(int16_t)(uint16_t)cpu->read_half(rec + game_layout.particles.sel_off);
        if (sel < 0) {
            if ((sel >> PART_CLASS_SHIFT) == PART_POOL_END) break;
            continue;                                    /* inactive slot */
        }
        const int cls = (sel >> PART_CLASS_SHIFT) & PART_CLASS_MASK;
        if ((uint32_t)cls >= game_layout.particles.fixed_layer_class) {
            s_item[i].cls = cls;
            s_item[i++].depth = front;
            continue;
        }
        if (cpu->read_byte(rec + game_layout.particles.drawn_off) == 0) continue;

        part_item* it = &s_item[i];
        it->cls = cls;
        it->rec = rec;
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
        it->depth =
            openpete_native_depth_from_viewz(openpete_worldvz_scaled((float)o.m3, 4.0f));
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
     * keeps depth 0, the nearest value, which is the fixed bucket's meaning. */
}

/* OPENPETE_PARTICLES=0 suppresses this feed alone. The guest draw still runs,
 * so the A/B prices the particles' painted pixels from one binary. */
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
 * displacement as the difference of two projections — the pool position the
 * present shows under the staged camera, against the position the tick drew
 * under the tick's camera — and moves the primitive by it. At the end of the
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

/* The record's displacement between the pose the tick drew and the pose this
 * present shows, in the pool's units, with the previous tick's angle byte.
 * Returns PART_PAIR_LERP only when the previous blob is the same particle. A
 * record that spawned this tick, one whose two states are further apart than a
 * tick's capture interval, and one whose slot was reused return otherwise, and
 * the primitive then holds the tick's pose in the world with only the camera's
 * motion rebasing it.
 *
 * The table is keyed by slot address, so a slot a new record respawns into
 * pairs two different particles; lerping across that pair draws a line from the
 * dead particle's position to the live one's. Three tests separate the reuse
 * from a continuing particle, in order of what they catch: the selector
 * halfword changing catches a reuse of a different class or type; the age byte
 * falling catches a reuse that kept both, the pool update having reset it for
 * the new record; the speed ceiling catches the rest, at a distance no particle
 * covers in a tick. */
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
    const double d[3] = {
        (double)(pw[0] & 0xFFFFu)        - (double)(cw[0] & 0xFFFFu),
        (double)((pw[0] >> 16) & 0xFFFFu) - (double)((cw[0] >> 16) & 0xFFFFu),
        (double)(pw[1] & 0xFFFFu)        - (double)(cw[1] & 0xFFFFu),
    };
    if (d[0]*d[0] + d[1]*d[1] + d[2]*d[2] > PART_REUSE_CEILING * PART_REUSE_CEILING)
        return PART_PAIR_REUSE;
    const double rest = 1.0 - alpha;
    for (int k = 0; k < 3; k++) off[k] = d[k] * rest;
    /* The angle the corner extents are built from is the top byte of the
     * second position word (partsdraw.s:497-501 shifts it right by 23 and
     * masks the halfword index to 0x1FE, which drops the low bit), and the
     * shortest arc of the 256-step circle is the way round the record turned. */
    const double pa = (double)((pw[1] >> 24) & 0xFFu);
    const double ca = (double)((cw[1] >> 24) & 0xFFu);
    double da = pa - ca;
    while (da > 128.0) da -= 256.0;
    while (da < -128.0) da += 256.0;
    *prev_ang = ca + da * rest;
    /* The size byte the corner extents are built from is byte 2 of the same
     * word (partsdraw.s:488-489 and :488C-4490 take (w1 >> 16) & 0xFF straight
     * into IR1 before the sine pair), so the extent this present shows is the
     * tick's size plus this much. */
    *size_delta = ((double)((pw[1] >> 16) & 0xFFu) - (double)((cw[1] >> 16) & 0xFFu)) * rest;
    return PART_PAIR_LERP;
}

/* The rotation carrying the corner extents from the angle the tick built them
 * at to the angle this present shows, as the cos/sin of the change. The
 * routine indexes the game's own sine table at (angle + 0x40) & 0x1FE and
 * takes the quarter-circle entry 0x80 bytes on (partsdraw.s:497-504), so the
 * extents ride the table's vector and the change is the rotation between the
 * two table vectors: exact, whatever the table's phase and sense. */
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

/* The scale carrying a primitive's corner offsets from the extent the tick
 * built to the extent this present shows. The routine builds a billboard's
 * half-extents as (size * sine) >> 10 in view space and divides all four
 * corners by the record point's own view-Z (partsdraw.s:488-4904 loads the
 * size byte and the sine pair into IR1..IR3, :4930-4934 shifts the products
 * right by 10, and :49C4 projects the four corners against one translation),
 * so the screen extent is proportional to the size byte and inversely
 * proportional to that view-Z. Both terms move inside a tick and the captured
 * corners carry neither: the scale is their ratio between the shown state and
 * the tick's.
 *
 * A two-point class's two endpoints are two record points, each translated by
 * its own displacement, so it has no offsets about a single point and holds
 * the identity.
 *
 * At the end of the tick's own interval the size delta is zero and the shown
 * view-Z is the projection of the tick's own inputs through the same
 * arithmetic, so both ratios are exactly one and the corners are untouched.
 *
 * The band is the discontinuity guard the displacement gets from
 * PART_REUSE_CEILING: a view-Z that moved a particle's extent by more than a
 * factor of four inside one tick is a camera cut or a slot reuse, where
 * holding the tick's extent is what every other term of the rebase does. */
#define PART_SCALE_MIN 0.25
#define PART_SCALE_MAX 4.0

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
    r->depth = openpete_native_depth_from_viewz(openpete_worldvz_scaled((float)o.m3, 4.0f));
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
    if (paired && it->cls == PART_ROT_CLASS) {
        const double tick_ang = (double)((it->w1 >> 24) & 0xFFu);
        r->rot = part_corner_rot(cpu, tick_ang, show_ang, &r->rc, &r->rs)
                     ? 1u : 0u;
        s_reb_stat[2][it->cls & 0xF] += r->rot;
    }
    r->dsz = (size_delta != 0.0) ? 1u : 0u;
    r->scl = part_extent_scale(it, &o, size_delta);
}

static void part_view_dbg(const char* tag, const ngte_view* v) {
    fprintf(stderr, "[part-view f=%u %s] cam=(%.2f,%.2f,%.2f) ram=(%.2f,%.2f,%.2f) "
                    "interp=%d of=(%.3f,%.3f) H=%.2f/%.2f s=%.4f F=%.4f M0=(%d,%d,%d)\n",
            openpete_frame, tag, v->cam[0], v->cam[1], v->cam[2],
            v->cam_ram[0], v->cam_ram[1], v->cam_ram[2], v->cam_interp,
            v->ofx, v->ofy, v->H_canon, v->H_eff, v->s, v->F,
            v->cam_M[0][0], v->cam_M[0][1], v->cam_M[0][2]);
}

static void part_rebase_report(void) {
    fprintf(stderr, "[part-rebase]");
    for (int c = 0; c < 16; c++)
        if (s_reb_stat[0][c] || s_reb_stat[1][c])
            fprintf(stderr, " c%d:lerp=%lu snap=%lu rot=%lu reuse=%lu", c,
                    s_reb_stat[0][c], s_reb_stat[1][c], s_reb_stat[2][c],
                    s_reb_stat[3][c]);
    fprintf(stderr, "\n");
}

/* Rebase the captured set onto the pose this present shows. The staged camera
 * resolves through the sub-tick view, so building a view here reads the
 * interpolated transform the rest of the pass draws under. */
static void part_rebase_all(CPUState* cpu, double alpha) {
    if (s_item_n <= 0) return;
    static int dbg = -1;
    if (dbg < 0) {
        const char* e = getenv("OPENPETE_PART_REBASE_DBG");
        dbg = (e && *e && *e != '0');
        s_reb_dbg = e ? atoi(e) : 0;
        if (dbg) atexit(part_rebase_report);
    }
    ngte_view v;
    ngte_view_build(cpu, &v);
    if (s_reb_dbg >= 2) part_view_dbg("staged", &v);
    for (int i = 0; i < s_item_n; i++) part_rebase_item(cpu, &v, alpha, i);
}

/* Carry one corner through its primitive's rebase: the corner keeps its offset
 * from the record point through the rotation, then the primitive translates by
 * that point's displacement. */
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

static void part_add_quad(const float x[4], const float y[4], const float u[4],
                          const float vv[4], const uint32_t rgb[4], float depth,
                          const ngte_tpage* tp, int textured, int blend) {
    static const int split[2][3] = { {0, 1, 2}, {1, 3, 2} };
    for (int s = 0; s < 2; s++) {
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            const int j = split[s][k];
            st.v[k].sx = x[j]; st.v[k].sy = y[j]; st.v[k].z = depth;
            st.v[k].u = u[j];  st.v[k].v = vv[j];
            st.v[k].rgba = (rgb[j] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = textured;
        if (textured) {
            st.tpage_x = tp->tpx; st.tpage_y = tp->tpy;
            st.clut_x  = tp->clx; st.clut_y  = tp->cly; st.bpp = tp->bpp;
        }
        st.blend = blend;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
        s_fed_tris++;
    }
}

/* A line primitive has no area, so it feeds as a one-pixel-wide quad across
 * its own perpendicular, the width the GPU rasterizes it at. */
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
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    float ox = 0.5f, oy = 0.0f;
    if (len > 1e-3f) { ox = -dy / len * 0.5f; oy = dx / len * 0.5f; }
    const float qx[4] = { ax - ox, ax + ox, bx - ox, bx + ox };
    const float qy[4] = { ay - oy, ay + oy, by - oy, by + oy };
    const float zu[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const uint32_t cc[4] = { rgb0, rgb0, rgb1, rgb1 };
    part_add_quad(qx, qy, zu, zu, cc, depth, NULL, 0, stp ? 1 : 0);
}

static void part_feed_native(void) {
    if (!part_feed_enabled()) return;
    if (s_item_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_item_n; i++) {
        const part_item* it = &s_item[i];
        const part_rebase* r = &s_reb[i];
        const ngte_prim* p = &it->p;
        const float depth = r->active ? r->depth : it->depth;
        if (p->nv == 4 || p->nv == 3) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_QUAD, (uint32_t)p->code, 0));
            ngte_tpage tp;
            int blend = p->stp ? 1 : 0;
            if (p->textured) {
                ngte_decode_tpage((unsigned)p->tpage, (unsigned)p->clut, 0u, &tp);
                if (p->stp) blend = ngte_abr_blend(tp.abr, NGTE_ABR_QUARTER_AS_ADD);
            } else {
                memset(&tp, 0, sizeof tp);
            }
            /* A triangle repeats its last corner, so one split covers both. */
            const int last = p->nv - 1;
            float x[4] = { p->x[0], p->x[1], p->x[2], p->x[last] };
            float y[4] = { p->y[0], p->y[1], p->y[2], p->y[last] };
            for (int j = 0; j < 4; j++) part_rebase_xy(it, r, 0, &x[j], &y[j]);
            const float u[4] = { (float)p->u[0], (float)p->u[1], (float)p->u[2], (float)p->u[last] };
            const float vt[4] = { (float)p->v[0], (float)p->v[1], (float)p->v[2], (float)p->v[last] };
            const uint32_t c[4] = { p->rgb[0], p->rgb[1], p->rgb[2], p->rgb[last] };
            part_add_quad(x, y, u, vt, c, depth, &tp, p->textured, blend);
        } else if ((p->code & 0xF8u) == 0x40u || (p->code & 0xF8u) == 0x50u) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_LINE, (uint32_t)p->code, 0));
            part_feed_line(it, r, depth, (p->code & 0x10u) != 0);
        }
    }
    openpete_native_scene_set_tag(0);
}

/* OPENPETE_PARTICLE_CENSUS=LO-HI (or a single frame): per call in that frame
 * range, the primitives the routine appended by GPU code, the native triangles
 * this feeder emitted from them, and the calibration the depth pairing rests
 * on — the distance between each paired record's own projected point and the
 * centroid of the primitive the pairing gave it. The routine projects the
 * record and then offsets the primitive's corners around that point, so a
 * correct pairing (and a correct camera matrix) keeps this small and a wrong
 * one scatters it across the frame. A capture whose fed count is zero measured
 * nothing. */
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
            const ngte_prim* p = &it->p;
            double gx = 0, gy = 0; int nv = 0;
            if (p->nv) {
                for (int k = 0; k < p->nv; k++) { gx += p->x[k]; gy += p->y[k]; }
                nv = p->nv;
            } else {
                const int words = (p->code & 0x10u) ? 3 : 2;   /* gouraud line carries a colour per endpoint */
                for (int k = 0; k < 2; k++) {
                    const uint32_t xy = p->raw[2 + (uint32_t)k * (uint32_t)(words - 1)];
                    gx += (double)(int16_t)(xy & 0xFFFFu);
                    gy += (double)(int16_t)(xy >> 16);
                }
                nv = 2;
            }
            gx /= nv; gy /= nv;
            const double d = fabs(gx - it->psx) + fabs(gy - it->psy);
            sum += d; if (d > worst) worst = d; n++;
        }
        if (n) fprintf(stderr, " c%d:n=%d mean=%.2f worst=%.1f", cl, n, sum / n, worst);
    }
    int unpaired = 0;
    for (int i = 0; i < s_item_n; i++) if (s_item[i].cls < 0) unpaired++;
    fprintf(stderr, " unpaired=%d\n", unpaired);
}

/* OPENPETE_PART_EXTENT_CENSUS: per present, per class, the decomposition of a
 * primitive's painted extent into the terms a sub-tick present could carry.
 * `ext` is the mean distance from the record's own projected point to the
 * primitive's furthest vertex, which is the extent the routine built from the
 * record's state under the tick's camera; `vz` is that point's view-Z, whose
 * reciprocal scales every extent; `dsz` counts the records whose size byte
 * differs between the pair's two states. A step in `ext` between two canonical
 * presents at a steady `n` is the primitive's own extent moving; a step in `n`
 * at a steady `ext` is the pool's population, which no rebase of the captured
 * set can carry. */
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

/* Sub-tick re-extract. The primitives are the guest's own output and are gone
 * once the polybuf is reused, so the replay re-feeds the set the canonical tick
 * captured, rebased onto the pose this present shows: each record point's
 * screen displacement carries its primitive's corners and its depth. At the
 * end of the tick's interval every term of the rebase is zero and the replay
 * reproduces the canonical present exactly, which is what the spine's identity
 * leg measures. */
void particles_reextract(CPUState* cpu, int bracketed, double alpha) {
    if (!hook_reextract_ready(RP_PARTICLES)) return;
    if (bracketed) part_rebase_all(cpu, alpha);
    part_extent_census(alpha);
    part_feed_native();
}

/* ---- hook ----------------------------------------------------------------- */
static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_PARTICLES, .prof_tag = "particles.srend",
    .st = &s_hb,
};

static int on_particles_draw(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    if (endp > startp) {
        part_capture_prims(cpu, startp, endp);
        const uint32_t fed_before = s_fed_tris;
        ngte_view view;
        ngte_view_build(cpu, &view);
        if (s_reb_dbg >= 2) part_view_dbg("tick", &view);
        part_pair_depths(cpu, &view);
        part_extent_census(-1.0);
        if (feed && render_path_enabled(RP_PARTICLES)) part_feed_native();
        part_census(startp, endp, fed_before);
    }
    return hook_bracket_end(&k_bracket);
}

/* The hook address is the layout's particle draw site, bound at boot: a static
 * initializer cannot read another translation unit's const. */
static feature_dispatch_entry_t particles_dispatch[] = {
    { 0u, on_particles_draw },
    { 0u, NULL },
};

static void particles_apply_options(const openpete_options_t* opts) {
    (void)opts;
    particles_dispatch[0].addr = game_layout.particles.hook;
}

const feature_module_t particles_module = {
    .name           = "particles_native",
    .apply_options  = particles_apply_options,
    .dispatch_table = particles_dispatch,
};
