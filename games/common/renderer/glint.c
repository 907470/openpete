// games/common/renderer/glint.c — the gem-glint feeder: a handwritten routine
// (Spyro 2 func_8004AC78 / mobyfunc.s:2192-2489, Spyro 3 func_8002F098 /
// mobyfunc.s:2195-2507; the same 297 instructions but for constants and
// symbols) that draws one screen-facing four-point star per live record of a
// pool and links each star into the 3D ordering table itself, so none of its
// primitives reaches the 2D chokepoint the sprite feeder hooks. Every per-game
// address, count, stride and bound comes from game_layout.glint; the walk
// below is one body.
//
// Read from the routine (asm/mobyfunc.s):
//   * the pool is 16 records of 0x10 bytes: world XYZ at +0x00, +0x04, +0x08
//     and a packed word at +0x0C holding the remaining lifetime in byte 0, the
//     size in byte 1, a spin phase in byte 2 and a colour-table row in byte 3
//     (:2231-2247).
//   * the routine subtracts the frame's elapsed count from the lifetime byte and
//     stores it back, dropping the record when it reaches zero (:2236-2240), so
//     the post-call lifetime byte is exactly the accept verdict for this pass.
//   * the record's camera-relative offset is halved before the rotation
//     (:2252-2258), so every view-space quantity the routine computes, the
//     near/far threshold included, is in half-world units.
//   * three cull stages: the halved offset must stay inside a +/-0x7800 box
//     (:2266-2276), the rotated depth must be at least 0x80 and under 0x7800
//     (:2294-2296), and the rotated X and Y must stay inside the view pyramid
//     4|X| - 3Z < 0x100 and 3|Y| - Z < 0xC0 (:2310, :2320).
//   * the size is a triangular ramp on the remaining lifetime times the size
//     byte, and the spin phase indexes the sine table for a 2x2 rotation-scale
//     basis loaded into the GTE rotation registers (:2324-2330), so the star
//     always faces the screen.
//   * the colour-table row is three words, each carrying 0x3A in its top byte:
//     the pale centre, the spike tip and the near-black waist, in that order
//     (:2330).
//   * under half-world depth 0x2000 the routine takes its near branch
//     (:2347-2348): one bundled draw-mode mini-packet (0xE1000620, whose ABR
//     bits are 0b01, additive) at :2383, then eight POLY_G4 semi-transparent
//     quads at :2387-2417, each spanning one rim segment of a seventeen-point
//     outline (a centre plus sixteen rim points, four spikes at radius 512 and
//     three shoulder points between each pair) that the GTE transformed into
//     the scratchpad at :2366-2373. Waist corners take the dark word, the spike
//     corner the tip word and the centre corner the pale word, so the star is
//     bright at its middle and fades out along each spike.
//   * at or above that depth the far branch (:2432) emits two opaque LINE_F2
//     instead, a plain cross through four projected endpoints, colouring them
//     with the pale word raised by 0x06000000 so its 0x3A code byte becomes
//     0x40 (:2480-2487).
//   * the ordering-table bucket is (Z >> 6) - 6 entries of eight bytes off the
//     base, clamped at zero (:2338-2344), with the far branch pushed a further
//     0x100 bytes back above depth 0x4000 (:2435).
//
// The geometry is the guest's own int16 output, read back out of the polybuf,
// not a second projection. What the primitives do not carry is depth - the PSX
// orders them by ordering-table bucket - so each star's view-Z comes from
// projecting its own pool record, paired to the star through the routine's own
// bookkeeping: a near record emits exactly one draw-mode packet and a far record
// emits exactly two lines, both in pool order, and the record walk here mirrors
// the routine's own rejects to say which records were accepted in which mode.
// The pairing is used only when the predicted near and far counts both agree
// with the captured ones; otherwise every primitive of the call is left unpaired
// and the census reports it.
//
// The pairing also carries what a sub-tick present needs, since the captured
// coordinates are the tick's alone: the record's address, its position words,
// its spin phase and its own projected screen point. A replay re-projects the
// record under the staged camera at the present's alpha, translates the
// primitive by the screen displacement and rotates its corners about that
// point by the change in phase.
//
// Spyro 1's sparkle renderer is fed by a float re-derivation in
// games/spyro-1/renderer/particle.c over a different record shape (a 0x18-byte
// record carrying a speed, a fade and a style byte) and a different star (a
// nine-point outline synthesized from the stock cross endpoints), with its
// primitives serving only as its A/B oracle. What the two share - the hook
// bracket, the polybuf walk, the primitive decode and the projection helpers -
// is already shared and is reused here unchanged.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the lifetime byte it
// decrements in each record; this feeder only reads them afterwards.

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

#define GT (game_layout.glint)

#define GLINT_MAX_PRIMS 256
#define GLINT_DRMODE    0xE1u   /* GP0 draw-mode packet, one per accepted near record */
#define GLINT_QUAD      0x3Au   /* POLY_G4 semi-transparent, eight per near record */
#define GLINT_LINE      0x40u   /* LINE_F2 opaque, two per accepted far record */
#define GLINT_QUADS_PER_STAR 8
#define GLINT_LINES_PER_CROSS 2

/* One pool record the walk accepted: what the pairing hands its primitives
 * and what the sub-tick rebase re-projects it from. `addr` is the record's
 * address, which is its slot and stays its identity across ticks. */
typedef struct {
    float    depth;      /* the scene depth its primitives take */
    int      near_mode;  /* 1 = the near star branch, 0 = the far cross */
    uint32_t addr;       /* 0 on a primitive the pairing left unowned */
    uint32_t w[3];       /* its position words as the tick read them */
    uint8_t  life;       /* its lifetime byte after the routine's decrement */
    uint8_t  spin;       /* its spin-phase byte */
    double   psx, psy;   /* its own projected screen point */
} glint_rec;

typedef struct {
    ngte_prim p;
    float     depth;
    int       paired;
    int       blend;
    glint_rec src;
} glint_item;

/* The screen-space rebase of one captured primitive onto the sub-tick pose:
 * the translation of the record's projected point and the rotation of the
 * corner offsets about it. Inactive on a canonical feed, and zero-valued at
 * the end of the tick's own interval, where the feed must reproduce the
 * canonical primitive bit for bit. */
typedef struct {
    float   dx, dy;   /* the record point's screen displacement */
    double  rc, rs;   /* the corner rotation, cos/sin of the angle change */
    float   depth;    /* the depth of the re-projected record point */
    uint8_t active;   /* 1 = translate the corners and take `depth` */
    uint8_t rot;      /* 1 = rotate the corner offsets as well */
} glint_rebase;

static glint_item   s_item[GLINT_MAX_PRIMS];
static glint_rebase s_reb[GLINT_MAX_PRIMS];
static int          s_item_n;
static int          s_stars;      /* draw-mode packets the call appended */
static int          s_crosses;    /* line pairs the call appended */
static int          s_pred_near;  /* near records the pool walk predicted */
static int          s_pred_far;   /* far records the pool walk predicted */
static uint32_t     s_code_hist[256];
static uint32_t     s_tag_hist[256];
static uint32_t     s_fed_star_tris;
static uint32_t     s_fed_cross_tris;

static int glint_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= GLINT_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 0.0f;
    s_item[s_item_n].paired = 0;
    s_item[s_item_n].blend = 0;
    memset(&s_item[s_item_n].src, 0, sizeof s_item[0].src);
    memset(&s_reb[s_item_n], 0, sizeof s_reb[0]);
    s_item_n++;
    return 1;
}

/* Walk the interval the call allocated: 3-word draw-mode packets (tag length 2),
 * 9-word quads (tag length 8) and 4-word lines (tag length 3). */
static void glint_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 8, .raw_words_max = 9, .guard = GLINT_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, glint_prim_sink, NULL);
}

/* The record's camera-relative offset, halved with an arithmetic shift, which
 * floors rather than truncating toward zero (:2252-2258). Every view-space
 * quantity the routine computes from it is therefore in half-world units. */
static void glint_view_offset(const ngte_view* v, const double p[3], double vo[3]) {
    vo[0] = floor((p[0] - v->cam[0]) / 2.0);      /* the routine's IR3 */
    vo[1] = floor((v->cam[1] - p[1]) / 2.0);      /* IR1 */
    vo[2] = floor((v->cam[2] - p[2]) / 2.0);      /* IR2 */
}

/* Rotate that offset by the camera matrix with a zero translation and divide,
 * which is the RTPS the routine runs on the star's own origin: its outline
 * points are transformed about this point, so it is where the star's shared
 * centre corner and the cross's midpoint land. The SXY bound is dropped for
 * the reason the particle feeder drops it — widescreen reveal needs the true
 * X. */
static void glint_project(const ngte_view* v, const double vo[3], ngte_proj* o) {
    ngte_project_gte(v->cam_M, NULL, vo[1], vo[2], vo[0], v->ofx, v->ofy, v, 0, o);
}

/* Mirror the routine's rejects for one pool record and, on acceptance, fill in
 * what the pairing and the sub-tick rebase read from it. Returns 0 for a
 * record the routine would not have drawn. */
static int glint_accept(CPUState* cpu, const ngte_view* v, uint32_t rec,
                        glint_rec* out) {
    const uint32_t packed = cpu->read_word(rec + GT.packed_off);
    if ((packed & 0xFFu) == 0u) return 0;          /* :2236-2240 expired or free */

    uint32_t w[3];
    double p[3], vo[3];
    for (int k = 0; k < 3; k++) {
        w[k] = cpu->read_word(rec + GT.pos_off + (uint32_t)k * 4u);
        p[k] = (double)(int32_t)w[k];
    }
    glint_view_offset(v, p, vo);
    const double box = (double)GT.box;
    for (int k = 0; k < 3; k++)
        if (vo[k] >= box || vo[k] <= -box) return 0;             /* :2266-2276 */

    ngte_proj o;
    glint_project(v, vo, &o);
    if (o.m3 < (double)GT.near_z || o.m3 >= (double)GT.far_z) return 0;  /* :2294-2296 */
    if (4.0 * fabs(o.m1) - 3.0 * o.m3 - (double)GT.pyr_x >= 0.0) return 0;   /* :2310 */
    if (3.0 * fabs(o.m2) - o.m3 - (double)GT.pyr_y >= 0.0) return 0;         /* :2320 */

    out->near_mode = (o.m3 < (double)GT.near_vz);
    out->depth = openpete_native_depth_from_viewz(
        openpete_worldvz_scaled((float)o.m3, 2.0f));
    out->addr = rec;
    for (int k = 0; k < 3; k++) out->w[k] = w[k];
    out->life = (uint8_t)(packed & 0xFFu);
    out->spin = (uint8_t)((packed >> 16) & 0xFFu);
    out->psx = o.sx;
    out->psy = o.sy;
    return 1;
}

/* Re-walk the pool and mirror the routine's rejects, appending one entry per
 * record it would have drawn. Returns the count. */
static int glint_predict(CPUState* cpu, const ngte_view* v, glint_rec* out, int cap) {
    int n = 0;
    for (uint32_t s = 0; s < GT.slots; s++) {
        glint_rec r;
        if (!glint_accept(cpu, v, GT.pool + s * GT.stride, &r)) continue;
        if (n < cap) out[n] = r;
        n++;
    }
    return n;
}

/* The primitives the call appended, by class: one draw-mode packet opens a
 * near star and each consecutive pair of lines is one far cross. */
static void glint_count_prims(int* stars, int* lines) {
    *stars = 0; *lines = 0;
    for (int i = 0; i < s_item_n; i++) {
        if (s_item[i].p.code == GLINT_DRMODE) (*stars)++;
        else if (s_item[i].p.code == GLINT_LINE) (*lines)++;
    }
}

/* Leave every primitive unpaired and at the nearest depth rather than dropping
 * it: an unpaired star still draws, holds its tick pose across the sub-tick
 * presents for want of a record to re-project, and the census names the call. */
static void glint_unpair_all(void) {
    for (int i = 0; i < s_item_n; i++) {
        s_item[i].blend = s_item[i].p.stp ? 1 : 0;
        s_item[i].src.addr = 0u;
    }
}

/* Advance the prediction cursor to the next record of one branch. The pool
 * order the routine drew in is the order the predictions came out in, and the
 * two branches interleave in that order, so one cursor over the predictions
 * serves both. Returns `predicted` when that branch has no record left. */
static int glint_next_rec(const glint_rec* pred, int predicted, int r, int near_mode) {
    while (++r < predicted && pred[r].near_mode != near_mode) { }
    return r;
}

/* Attach one primitive to the record the cursor stands on. */
static void glint_bind(glint_item* it, const glint_rec* r, int blend) {
    it->blend = blend;
    it->depth = r->depth;
    it->src = *r;
    it->paired = 1;
}

/* Walk the captured primitives and the predictions together: a draw-mode
 * packet opens a near star and its ABR bits give the star's blend, the eight
 * quads until the next packet belong to it, and each consecutive pair of lines
 * is one far cross. */
static void glint_pair_walk(const glint_rec* pred, int predicted) {
    const glint_rec* cur = NULL;
    int r = -1, blend = 0, line_in_cross = 0;
    for (int i = 0; i < s_item_n; i++) {
        const uint8_t code = s_item[i].p.code;
        if (code == GLINT_DRMODE) {
            r = glint_next_rec(pred, predicted, r, 1);
            if (r >= predicted) return;
            /* The draw-mode word's semi-transparency rate, bits 5 and 6. */
            blend = ngte_abr_blend((int)((s_item[i].p.raw[1] >> 5) & 3u),
                                   NGTE_ABR_QUARTER_AS_ADD);
            cur = &pred[r];
            continue;
        }
        if (code == GLINT_LINE) {
            if (line_in_cross == 0) {
                r = glint_next_rec(pred, predicted, r, 0);
                if (r >= predicted) return;
                blend = 0;                      /* :2480 the far cross is opaque */
                cur = &pred[r];
            }
            line_in_cross = (line_in_cross + 1) % GLINT_LINES_PER_CROSS;
        } else if (!cur) {
            continue;                           /* a quad before any draw mode */
        }
        glint_bind(&s_item[i], cur, blend);
    }
}

/* Pair the call's primitives with the records that emitted them. The pairing
 * is used only when the predicted near and far counts both agree with the
 * captured ones. */
static void glint_pair_depths(CPUState* cpu, const ngte_view* v) {
    static glint_rec pred[GLINT_MAX_PRIMS];
    int lines = 0;
    glint_count_prims(&s_stars, &lines);
    s_crosses = lines / GLINT_LINES_PER_CROSS;

    const int predicted = glint_predict(cpu, v, pred, GLINT_MAX_PRIMS);
    s_pred_near = 0; s_pred_far = 0;
    for (int i = 0; i < predicted && i < GLINT_MAX_PRIMS; i++) {
        if (pred[i].near_mode) s_pred_near++; else s_pred_far++;
    }
    if (predicted > GLINT_MAX_PRIMS || s_pred_near != s_stars ||
        s_pred_far != s_crosses || lines % GLINT_LINES_PER_CROSS != 0) {
        glint_unpair_all();
        return;
    }
    glint_pair_walk(pred, predicted);
}

/* OPENPETE_GLINT selects what this feeder emits: 0 nothing, 1 the near stars
 * alone, 2 the near stars and the far crosses (the default). The guest draw
 * runs at every setting, so the A/B prices the painted pixels from one binary.
 *
 * The far cross is fed at its own projected depth, with no ordering-table pull
 * applied: the routine keys it on the same view-Z the depth comes from, so the
 * depth test reproduces the ordering the routine asked for. Where world
 * geometry stands between the camera and the gem, the cross is covered — in
 * the ordering table and under the depth test alike — and where nothing does,
 * it draws. */
static int glint_feed_mode(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_GLINT");
        v = (e && *e) ? atoi(e) : 2;
        if (v < 0) v = 0;
    }
    return v;
}

/* ---- sub-tick rebase ------------------------------------------------------
 *
 * The primitives are the guest's own int16 output, built under the tick's
 * camera from the tick's pool, so a sub-tick present that re-feeds them draws
 * every star at the pixels the tick drew: neither the camera's motion nor the
 * record's own reaches them, and neither does the spin. The rebase measures
 * the record's screen displacement as the difference of two projections — the
 * position the present shows under the staged camera, against the position the
 * tick drew under the tick's camera — moves the primitive by it, and rotates
 * the corner offsets about the projected point by the change in spin phase. At
 * the end of the tick's own interval the displaced position, the shown phase
 * and the staged camera are all the tick's, so both projections take identical
 * inputs through identical arithmetic and every term is exactly zero.
 *
 * Rotating screen offsets about the projected point is the routine's own
 * construction rather than an approximation: it loads a 2x2 rotation-scale
 * basis into the GTE rotation registers and transforms the outline about the
 * record's own translation (:2324-2330 for the star, :2480-2487 for the
 * cross), so all of a star's corners share one view Z and the perspective
 * divide is common to them. */

/* A gem's speed ceiling, in world units per tick. The table is keyed by slot
 * address, so a slot a new record respawns into inside one tick pairs across
 * two different gems and would lerp as a streak between them. The packed-word
 * test below catches a reuse that changed the size byte, the spin phase or the
 * colour row, and the lifetime test one that kept all three; this catches the
 * rest, at a distance no gem covers in a tick. */
#define GLINT_REUSE_CEILING 4096.0

/* The record's spin phase in sine-table entries, 256 to the turn: the stored
 * phase byte less four entries per remaining lifetime step (:2270-2277 masks
 * the difference to a byte and doubles it for the halfword index), so a record
 * turns four entries per tick as its lifetime counts down. */
static double glint_angle(const glint_rec* r) {
    return (double)((unsigned)(r->spin - 4u * (unsigned)r->life) & 0xFFu);
}

/* The record's displacement between the pose the tick drew and the pose this
 * present shows, in world units, with the phase the present shows. Returns 1
 * only when the previous blob is the same gem. A record that spawned this
 * tick, one whose two states are further apart than a tick's capture interval,
 * and one whose slot was reused return 0, and the primitive then holds the
 * tick's world position and phase with only the camera's motion rebasing it.
 *
 * Every byte of the packed word but the lifetime is written once by the
 * spawner and never again, so an unequal upper three bytes is a slot that took
 * a new record; the lifetime itself only ever falls while a record lives, so a
 * risen lifetime is a reuse that happened to keep them. */
static int glint_record_pair(const glint_rec* r, double alpha, double off[3],
                             double* show_ang) {
    off[0] = off[1] = off[2] = 0.0;
    *show_ang = glint_angle(r);
    const openpete_interp_obj_t* o = openpete_interp_objs_find(r->addr);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    if ((uint32_t)o->blob_len < GT.pos_off + 12u ||
        (uint32_t)o->blob_len < GT.packed_off + 4u) return 0;
    if (o->curr_ns - o->prev_ns >
        g_openpete_tick_period_ns + g_openpete_tick_period_ns / 2) return 0;
    uint32_t pp, cp;
    memcpy(&pp, o->prev + GT.packed_off, 4);
    memcpy(&cp, o->curr + GT.packed_off, 4);
    if ((pp & ~0xFFu) != (cp & ~0xFFu)) return 0;        /* the slot took a new record */
    if ((cp & 0xFFu) > (pp & 0xFFu)) return 0;           /* the lifetime rose: a reuse */
    int32_t pw[3];
    memcpy(pw, o->prev + GT.pos_off, 12);
    double d[3], sq = 0.0;
    for (int k = 0; k < 3; k++) {
        d[k] = (double)pw[k] - (double)(int32_t)r->w[k];
        sq += d[k] * d[k];
    }
    if (sq > GLINT_REUSE_CEILING * GLINT_REUSE_CEILING) return 0;
    const double rest = 1.0 - alpha;
    for (int k = 0; k < 3; k++) off[k] = d[k] * rest;
    /* The shortest arc of the 256-entry circle is the way round the record
     * turned; the phase advances monotonically, so this is the record's own
     * four-entries-per-tick step at any alpha. */
    const double ca = glint_angle(r);
    const double pa = (double)((unsigned)((uint8_t)(pp >> 16) - 4u * (unsigned)(uint8_t)(pp & 0xFFu)) & 0xFFu);
    double da = pa - ca;
    while (da > 128.0) da -= 256.0;
    while (da < -128.0) da += 256.0;
    *show_ang = ca + da * rest;
    return 1;
}

/* OPENPETE_GLINT_REBASE_DBG=1: the rebase's verdicts per run. A run whose
 * lerped count is zero rebased nothing against a previous record and measured
 * nothing; `reuse` is the subset of the snapped whose slot was found holding a
 * different gem. */
static unsigned long s_reb_stat[3];   /* 0 lerped, 1 snapped, 2 rotated */

static void glint_rebase_report(void) {
    fprintf(stderr, "[glint-rebase] lerped=%lu snapped=%lu rotated=%lu\n",
            s_reb_stat[0], s_reb_stat[1], s_reb_stat[2]);
}

static void glint_rebase_item(CPUState* cpu, const ngte_view* v, double alpha, int i) {
    glint_item* it = &s_item[i];
    glint_rebase* r = &s_reb[i];
    memset(r, 0, sizeof *r);
    if (!it->src.addr) return;                   /* an unpaired primitive */
    double off[3], show_ang = 0.0;
    const int paired = glint_record_pair(&it->src, alpha, off, &show_ang);
    s_reb_stat[paired ? 0 : 1]++;
    double p[3], vo[3];
    for (int k = 0; k < 3; k++) p[k] = (double)(int32_t)it->src.w[k] + off[k];
    glint_view_offset(v, p, vo);
    ngte_proj o;
    glint_project(v, vo, &o);
    r->dx = (float)(o.sx - it->src.psx);
    r->dy = (float)(o.sy - it->src.psy);
    r->depth = openpete_native_depth_from_viewz(
        openpete_worldvz_scaled((float)o.m3, 2.0f));
    r->active = 1;
    r->rot = ngte_table_rot(cpu, glint_angle(&it->src), show_ang, &r->rc, &r->rs)
                 ? 1u : 0u;
    s_reb_stat[2] += r->rot;
}

/* Rebase the captured set onto the pose this present shows. The staged camera
 * resolves through the sub-tick view, so building a view here reads the
 * interpolated transform the rest of the pass draws under. */
static void glint_rebase_all(CPUState* cpu, double alpha) {
    if (s_item_n <= 0) return;
    static int dbg = -1;
    if (dbg < 0) {
        const char* e = getenv("OPENPETE_GLINT_REBASE_DBG");
        dbg = (e && *e && *e != '0');
        if (dbg) atexit(glint_rebase_report);
    }
    ngte_view v;
    ngte_view_build(cpu, &v);
    for (int i = 0; i < s_item_n; i++) glint_rebase_item(cpu, &v, alpha, i);
}

/* Carry one corner through its primitive's rebase: the corner keeps its offset
 * from the record's projected point through the rotation, then the primitive
 * translates by that point's displacement. */
static void glint_rebase_xy(const glint_item* it, const glint_rebase* r,
                            float* x, float* y) {
    if (!r->active) return;
    if (r->rot) {
        const double ox = (double)*x - it->src.psx;
        const double oy = (double)*y - it->src.psy;
        *x = (float)(it->src.psx + ox * r->rc + oy * r->rs);
        *y = (float)(it->src.psy - ox * r->rs + oy * r->rc);
    }
    *x += r->dx;
    *y += r->dy;
}

static void glint_add_quad(const float x[4], const float y[4],
                           const uint32_t rgb[4], float depth, int blend) {
    static const int split[2][3] = { {0, 1, 2}, {1, 3, 2} };
    for (int s = 0; s < 2; s++) {
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            const int j = split[s][k];
            st.v[k].sx = x[j]; st.v[k].sy = y[j]; st.v[k].z = depth;
            st.v[k].u = 0.0f;  st.v[k].v = 0.0f;
            st.v[k].rgba = (rgb[j] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.blend = blend;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    }
}

/* A line primitive has no area, so it feeds as a one-pixel-wide quad across its
 * own perpendicular, the width the GPU rasterizes it at. Both endpoints carry
 * the record's rebase, so the perpendicular follows the rebased line. */
static void glint_feed_line(const glint_item* it, const glint_rebase* rb,
                            float depth, int blend) {
    const ngte_prim* p = &it->p;
    const uint32_t rgb = p->raw[1] & 0xFFFFFFu;
    const uint32_t xy0 = p->raw[2], xy1 = p->raw[3];
    float ax = (float)(int16_t)(xy0 & 0xFFFFu), ay = (float)(int16_t)(xy0 >> 16);
    float bx = (float)(int16_t)(xy1 & 0xFFFFu), by = (float)(int16_t)(xy1 >> 16);
    glint_rebase_xy(it, rb, &ax, &ay);
    glint_rebase_xy(it, rb, &bx, &by);
    const float dx = bx - ax, dy = by - ay;
    const float len = sqrtf(dx * dx + dy * dy);
    float ox = 0.5f, oy = 0.0f;
    if (len > 1e-3f) { ox = -dy / len * 0.5f; oy = dx / len * 0.5f; }
    const float qx[4] = { ax - ox, ax + ox, bx - ox, bx + ox };
    const float qy[4] = { ay - oy, ay + oy, by - oy, by + oy };
    const uint32_t cc[4] = { rgb, rgb, rgb, rgb };
    glint_add_quad(qx, qy, cc, depth, blend);
}

static void glint_feed_native(void) {
    const int mode = glint_feed_mode();
    if (mode <= 0) return;
    if (s_item_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_item_n; i++) {
        const ngte_prim* p = &s_item[i].p;
        const glint_rebase* rb = &s_reb[i];
        const float depth = rb->active ? rb->depth : s_item[i].depth;
        if (p->code == GLINT_QUAD && p->nv == 4) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_FLARE, GLINT_QUAD, 0));
            float x[4] = { p->x[0], p->x[1], p->x[2], p->x[3] };
            float y[4] = { p->y[0], p->y[1], p->y[2], p->y[3] };
            const uint32_t c[4] = { p->rgb[0], p->rgb[1], p->rgb[2], p->rgb[3] };
            for (int k = 0; k < 4; k++) glint_rebase_xy(&s_item[i], rb, &x[k], &y[k]);
            glint_add_quad(x, y, c, depth, s_item[i].blend);
            s_fed_star_tris += 2;
        } else if (p->code == GLINT_LINE && mode >= 2) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_SPARK, GLINT_LINE, 0));
            glint_feed_line(&s_item[i], rb, depth, s_item[i].blend);
            s_fed_cross_tris += 2;
        }
    }
    openpete_native_scene_set_tag(0);
}

/* OPENPETE_GLINT_CENSUS=LO-HI (or a single frame): per call in that frame range,
 * the primitives the routine appended by GPU code, the stars its draw-mode
 * packets delimit and the crosses its line pairs delimit, the near and far
 * records the pool walk predicted, the native triangles this feeder emitted from
 * each class, how many primitives were left unpaired, and the pairing's pivot
 * residual. A capture whose fed count is zero measured nothing. */
/* Structural check of one star's captured geometry: its eight quads must share
 * a single centre corner, and their remaining corners must reduce to at most the
 * sixteen rim points of the outline the routine transformed. A decode that
 * mis-slices the quad words, or a walk that straddles a primitive boundary,
 * breaks the shared centre. Returns the number of distinct rim points, or -1
 * when the centre is not shared; a small star on screen quantizes neighbouring
 * rim points onto the same pixel, so fewer than sixteen is normal. */
static int glint_star_topology_ok(int first, int count) {
    if (count != GLINT_QUADS_PER_STAR) return -1;
    float rx[GLINT_QUADS_PER_STAR * 3], ry[GLINT_QUADS_PER_STAR * 3];
    int rim = 0;
    for (int q = 0; q < count; q++) {
        const ngte_prim* p = &s_item[first + q].p;
        if (p->nv != 4) return -1;
        if (p->x[2] != s_item[first].p.x[2] || p->y[2] != s_item[first].p.y[2]) return -1;
        static const int corner[3] = { 0, 1, 3 };
        for (int k = 0; k < 3; k++) {
            const float cx = p->x[corner[k]], cy = p->y[corner[k]];
            int seen = 0;
            for (int j = 0; j < rim; j++) if (rx[j] == cx && ry[j] == cy) { seen = 1; break; }
            if (!seen && rim < (int)(sizeof rx / sizeof rx[0])) { rx[rim] = cx; ry[rim] = cy; rim++; }
        }
    }
    return rim;
}

/* Stars whose eight quads share one centre corner, and the largest distinct rim
 * count any star of the call reached. */
static void glint_stars_ok(int* ok, int* rim_max) {
    int first = -1, n = 0;
    *ok = 0; *rim_max = 0;
    for (int i = 0; i <= s_item_n; i++) {
        const int end = (i == s_item_n) || s_item[i].p.code == GLINT_DRMODE;
        if (end) {
            if (first >= 0) {
                const int rim = glint_star_topology_ok(first, n);
                if (rim >= 0) { (*ok)++; if (rim > *rim_max) *rim_max = rim; }
            }
            first = i + 1; n = 0;
        } else if (s_item[i].p.code == GLINT_QUAD && first >= 0) {
            n++;
        }
    }
}

/* The pairing's own residual: how far each paired primitive's record point
 * lands from the point the routine drew it about — a star's shared centre
 * corner, a cross's line midpoint. The sub-tick rotation pivots on the
 * projected point, so this bounds the error that pivot introduces; a residual
 * that is not a fraction of a pixel means the walk is projecting a different
 * record than the one that emitted the primitive. */
static void glint_pivot_residual(double* mean, double* worst, int* n) {
    double sum = 0.0;
    *worst = 0.0; *n = 0;
    for (int i = 0; i < s_item_n; i++) {
        const glint_item* it = &s_item[i];
        if (!it->src.addr) continue;
        double gx, gy;
        if (it->p.code == GLINT_QUAD && it->p.nv == 4) {
            if (i > 0 && s_item[i - 1].p.code == GLINT_QUAD) continue;  /* one per star */
            gx = (double)it->p.x[2]; gy = (double)it->p.y[2];
        } else if (it->p.code == GLINT_LINE) {
            const uint32_t a = it->p.raw[2], b = it->p.raw[3];
            gx = ((double)(int16_t)(a & 0xFFFFu) + (double)(int16_t)(b & 0xFFFFu)) / 2.0;
            gy = ((double)(int16_t)(a >> 16) + (double)(int16_t)(b >> 16)) / 2.0;
        } else {
            continue;
        }
        const double d = fabs(gx - it->src.psx) + fabs(gy - it->src.psy);
        sum += d;
        if (d > *worst) *worst = d;
        (*n)++;
    }
    *mean = *n ? sum / (double)*n : 0.0;
}

static void glint_census(uint32_t start, uint32_t end,
                         uint32_t star_before, uint32_t cross_before) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_GLINT_CENSUS", openpete_frame)) return;
    int unpaired = 0;
    for (int i = 0; i < s_item_n; i++)
        if (s_item[i].p.code != GLINT_DRMODE && !s_item[i].paired) unpaired++;
    fprintf(stderr, "[glint-census f=%u] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            openpete_frame, start, end, end - start, s_item_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_item_n; i++) seen[s_item[i].p.code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    int stars_ok = 0, rim_max = 0;
    glint_stars_ok(&stars_ok, &rim_max);
    double pmean, pworst; int pn;
    glint_pivot_residual(&pmean, &pworst, &pn);
    fprintf(stderr, " stars=%d centre_ok=%d rim_max=%d crosses=%d pred_near=%d pred_far=%d"
                    " fed_star_tris=%u fed_cross_tris=%u unpaired=%d"
                    " pivot:n=%d mean=%.2f worst=%.2f\n",
            s_stars, stars_ok, rim_max, s_crosses, s_pred_near, s_pred_far,
            s_fed_star_tris - star_before, s_fed_cross_tris - cross_before, unpaired,
            pn, pmean, pworst);
    /* Each star's centre corner and each cross's first endpoint, with the scene
     * depth the pairing gave it: the two quantities the feed places them by. */
    int line_i = 0;
    for (int i = 0; i < s_item_n; i++) {
        if (s_item[i].p.code == GLINT_QUAD &&
            (i == 0 || s_item[i - 1].p.code != GLINT_QUAD)) {
            fprintf(stderr, "[glint-star f=%u] centre=(%.0f,%.0f) depth=%.6f blend=%d\n",
                    openpete_frame, s_item[i].p.x[2], s_item[i].p.y[2],
                    s_item[i].depth, s_item[i].blend);
        } else if (s_item[i].p.code == GLINT_LINE) {
            if (line_i % GLINT_LINES_PER_CROSS == 0)
                fprintf(stderr, "[glint-cross f=%u] a=(%d,%d) depth=%.6f\n", openpete_frame,
                        (int)(int16_t)(s_item[i].p.raw[2] & 0xFFFFu),
                        (int)(int16_t)(s_item[i].p.raw[2] >> 16), s_item[i].depth);
            line_i++;
        }
    }
}

/* ---- hook ---------------------------------------------------------------- */

static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_SPARKLE, .prof_tag = "glint.srend", .st = &s_hb,
};

/* The world draw's call is the only one in either image, so the hook takes
 * every entry (Spyro 2 draw/func_800155A0.s:32, Spyro 3 draw/func_8001E460.s:30). */
static int on_glint_draw(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    s_item_n = 0; s_stars = 0; s_crosses = 0; s_pred_near = 0; s_pred_far = 0;
    const uint32_t star_before = s_fed_star_tris, cross_before = s_fed_cross_tris;
    if (endp > startp) {
        glint_capture_prims(cpu, startp, endp);
        ngte_view view;
        ngte_view_build(cpu, &view);
        glint_pair_depths(cpu, &view);
        if (feed && render_path_enabled(RP_SPARKLE)) glint_feed_native();
    }
    glint_census(startp, endp, star_before, cross_before);
    return hook_bracket_end(&k_bracket);
}

/* Sub-tick re-extract. The primitives are the guest's own output and are gone
 * once the polybuf is reused, so the replay re-feeds the set the canonical tick
 * captured, rebased onto the pose this present shows: the owning record's
 * screen displacement carries its primitive's corners and its depth, and the
 * change in spin phase rotates them about that point. At the end of the tick's
 * interval every term of the rebase is zero and the replay reproduces the
 * canonical present exactly, which is what the spine's identity leg
 * measures. */
void glint_reextract(CPUState* cpu, int bracketed, double alpha) {
    if (!hook_reextract_ready(RP_SPARKLE)) return;
    if (bracketed) glint_rebase_all(cpu, alpha);
    glint_feed_native();
}

/* The hook address is the layout's glint draw site, bound at boot: a static
 * initializer cannot read another TU's const. A game with no located routine
 * leaves it 0 and the registry dispatches nothing. */
static feature_dispatch_entry_t glint_dispatch[] = {
    { 0u, on_glint_draw },
    { 0u, NULL },
};

static void glint_apply_options(const openpete_options_t* opts) {
    (void)opts;
    glint_dispatch[0].addr = game_layout.glint.hook;
}

const feature_module_t glint_module = {
    .name           = "glint_native",
    .apply_options  = glint_apply_options,
    .dispatch_table = glint_dispatch,
};
