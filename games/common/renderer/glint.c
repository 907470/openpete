// games/common/renderer/glint.c - the gem-glint feeder: a handwritten routine
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
// primitive by the screen displacement, rotates its corners about that point
// by the change in phase and scales them by the ratio of the extent the
// present shows to the extent the tick drew (the size ramp at the blended
// lifetime, over the record point's view-Z).
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

#include "openpete_fiber.h"  /* openpete_feed_key_now: instrument line labels */
#include "glint.h"
#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/env_knob.h"
#include "renderer/cut_guard.h"     /* cut_held_sets_t: the held cut's tick N-1 set */
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
    double   pvz;        /* that point's half-world view-Z */
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
    double  scl;      /* the corner-offset scale, the shown extent over the tick's */
    double  ang;      /* the spin phase this present shows, in sine-table entries */
    float   depth;    /* the depth of the re-projected record point */
    uint8_t active;   /* 1 = translate the corners and take `depth` */
    uint8_t rot;      /* 1 = rotate the corner offsets as well */
} glint_rebase;

static glint_item   s_item_buf[2][GLINT_MAX_PRIMS];
static glint_item*  s_item = s_item_buf[0];
static glint_rebase s_reb[GLINT_MAX_PRIMS];
static int          s_item_n;
/* The set the last capture of a new canonical epoch retired, which a held
 * camera cut shows when it is tick N-1's. */
static glint_item*  s_item_old = s_item_buf[1];
static int          s_item_old_n;
static int          s_stars;      /* draw-mode packets the call appended */
static int          s_crosses;    /* line pairs the call appended */
static int          s_pred_near;  /* near records the pool walk predicted */
static int          s_pred_far;   /* far records the pool walk predicted */
static uint32_t     s_code_hist[256];
static uint32_t     s_tag_hist[256];
static uint32_t     s_fed_star_tris;
static uint32_t     s_fed_cross_tris;

/** @brief Exchange the current captured set with the retired one, with their counts. */
static void glint_sets_swap(void) {
    glint_item* t = s_item; s_item = s_item_old; s_item_old = t;
    const int n = s_item_n; s_item_n = s_item_old_n; s_item_old_n = n;
}

static cut_held_sets_t s_sets = { glint_sets_swap, &s_item_n, "glint", { 0u, 0u } };

/** @brief Append one walked primitive to s_item, unpaired at the near-guard depth.
 *
 * Also zeroes the item's record and its rebase slot.
 * @param ctx Unused.
 * @param s The decoded primitive.
 * @return 1 when stored, 0 when s_item holds GLINT_MAX_PRIMS. */
static int glint_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= GLINT_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 1.0f;   /* unpaired: the near guard (view-Z 1) */
    s_item[s_item_n].paired = 0;
    s_item[s_item_n].blend = 0;
    memset(&s_item[s_item_n].src, 0, sizeof s_item[0].src);
    memset(&s_reb[s_item_n], 0, sizeof s_reb[0]);
    s_item_n++;
    return 1;
}

/** @brief Walk the polybuf interval the glint routine allocated into s_item.
 *
 * The interval holds 3-word draw-mode packets (tag length 2), 9-word quads
 * (tag length 8) and 4-word lines (tag length 3); a longer tag ends the walk.
 * At most GLINT_MAX_PRIMS prims are kept, each unpaired at the near-guard
 * depth. The code and tag histograms accumulate across calls.
 * @param cpu The guest CPU state, read only.
 * @param start First polybuf address of the routine's output.
 * @param end One past the last. */
static void glint_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 8, .raw_words_max = 9, .guard = GLINT_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, glint_prim_sink, NULL);
}

/** @brief Compute a record's camera-relative offset, halved the way the glint routine halves it.
 *
 * The routine halves with an arithmetic shift, which floors rather than
 * truncating toward zero (:2252-2258); every view-space quantity it computes
 * from the offset is therefore in half-world units. The components are ordered
 * (X - camX, camY - Y, camZ - Z), the order ngte_project_offset takes.
 * @param v The projection view; its camera position is read.
 * @param p The record's world position.
 * @param vo Receives the halved offset. */
static void glint_view_offset(const ngte_view* v, const double p[3], double vo[3]) {
    vo[0] = floor((p[0] - v->cam[0]) / 2.0);      /* the routine's IR3 */
    vo[1] = floor((v->cam[1] - p[1]) / 2.0);      /* IR1 */
    vo[2] = floor((v->cam[2] - p[2]) / 2.0);      /* IR2 */
}

/** @brief Project a halved record offset the way the glint routine projects the star's origin.
 *
 * Rotates the offset by the camera matrix with a zero translation and
 * divides, which is the RTPS the routine runs on the star's own origin: its
 * outline points are transformed about this point, so it is where the star's
 * shared centre corner and the cross's midpoint land. The SXY bound is
 * dropped, as the particle feeder drops it, because widescreen reveal needs
 * the true X.
 * @param v The projection view.
 * @param vo The halved offset from glint_view_offset.
 * @param o Receives the screen point and the half-world view-space MACs. */
static void glint_project(const ngte_view* v, const double vo[3], ngte_proj* o) {
    ngte_project_offset(v, vo, o);
}

/** @brief Mirror the glint routine's rejects for one pool record and, on acceptance, fill in what the pairing and the sub-tick rebase read from it.
 *
 * Rejects a record whose lifetime byte is 0 (:2236-2240), whose halved
 * camera-relative offset leaves the +/-GT.box box (:2266-2276), whose
 * half-world view Z is below GT.near_z or at least GT.far_z (:2294-2296), or
 * that leaves the view pyramid 4|X| - 3Z < GT.pyr_x, 3|Y| - Z < GT.pyr_y
 * (:2310, :2320). An accepted record takes the near branch when its view Z is
 * below GT.near_vz, and its scene depth is that view Z scaled back to world
 * units.
 * @param cpu The guest CPU state, read only.
 * @param v The projection view.
 * @param rec Guest address of the record.
 * @param out Receives the accepted record's branch, depth, address, position
 *        words, lifetime and spin bytes, projected point and view Z; untouched
 *        on rejection.
 * @return 1 when the routine would draw the record, else 0. */
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
    out->depth = openpete_native_scene_viewz(
        openpete_worldvz_scaled((float)o.m3, 2.0f));
    out->addr = rec;
    for (int k = 0; k < 3; k++) out->w[k] = w[k];
    out->life = (uint8_t)(packed & 0xFFu);
    out->spin = (uint8_t)((packed >> 16) & 0xFFu);
    out->psx = o.sx;
    out->psy = o.sy;
    out->pvz = o.m3;
    return 1;
}

/** @brief Re-walk the glint pool in slot order and collect every record the routine would have drawn.
 *
 * Each of the GT.slots records is tested with glint_accept. Entries past `cap`
 * are counted but not stored.
 * @param cpu The guest CPU state, read only.
 * @param v The projection view.
 * @param out Receives up to `cap` accepted records in pool order.
 * @param cap Capacity of `out`.
 * @return The number of accepted records, which may exceed `cap`. */
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

/** @brief Count the captured draw-mode packets and lines.
 *
 * One draw-mode packet opens a near star; each consecutive pair of lines is
 * one far cross.
 * @param stars Receives the number of draw-mode packets.
 * @param lines Receives the number of lines. */
static void glint_count_prims(int* stars, int* lines) {
    *stars = 0; *lines = 0;
    for (int i = 0; i < s_item_n; i++) {
        if (s_item[i].p.code == GLINT_DRMODE) (*stars)++;
        else if (s_item[i].p.code == GLINT_LINE) (*lines)++;
    }
}

/** @brief Leave every captured primitive unowned at the near-guard depth rather than dropping it.
 *
 * Sets each primitive's blend from its semi-transparency bit (1 when set, else
 * opaque) and clears its record address. An unowned primitive still draws,
 * holds its tick pose across the sub-tick presents for want of a record to
 * re-project, and counts as unpaired in the census. */
static void glint_unpair_all(void) {
    for (int i = 0; i < s_item_n; i++) {
        s_item[i].blend = s_item[i].p.stp ? 1 : 0;
        s_item[i].src.addr = 0u;
    }
}

/** @brief Advance the prediction cursor to the next record of one branch.
 *
 * The pool order the routine drew in is the order the predictions came out
 * in, and the two branches interleave in that order, so one cursor over the
 * predictions serves both.
 * @param pred The predicted records in pool order.
 * @param predicted Their count.
 * @param r The current cursor, -1 before the first record.
 * @param near_mode 1 to find the next near record, 0 the next far one.
 * @return The index of that record, or `predicted` when the branch has none
 *         left. */
static int glint_next_rec(const glint_rec* pred, int predicted, int r, int near_mode) {
    while (++r < predicted && pred[r].near_mode != near_mode) { }
    return r;
}

/** @brief Attach one captured primitive to the record that emitted it.
 *
 * Copies the record into the item, takes the record's depth, sets the blend
 * and marks the item paired.
 * @param it The captured primitive.
 * @param r The record the pairing cursor stands on.
 * @param blend The native blend mode for the primitive. */
static void glint_bind(glint_item* it, const glint_rec* r, int blend) {
    it->blend = blend;
    it->depth = r->depth;
    it->src = *r;
    it->paired = 1;
}

/** @brief Walk the captured primitives and the predicted records together and bind each primitive to its record.
 *
 * A draw-mode packet advances to the next near record and its ABR bits give
 * the star's blend; the quads until the next packet belong to that record.
 * The first line of each consecutive pair advances to the next far record,
 * opaque. A quad before any draw-mode packet stays unpaired, and the walk
 * stops when a branch runs out of records. The draw-mode packets themselves
 * are not bound.
 * @param pred The predicted records in pool order.
 * @param predicted Their count. */
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

/** @brief Pair the call's captured primitives with the pool records that emitted them.
 *
 * Counts the captured stars and crosses into s_stars and s_crosses, predicts
 * the accepted records into s_pred_near and s_pred_far, and runs
 * glint_pair_walk only when the predicted near and far counts both equal the
 * captured ones, the line count is even and the prediction fits the table;
 * otherwise every primitive is left unowned by glint_unpair_all.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view. */
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

/** @brief Read OPENPETE_GLINT, which selects what this feeder emits, latched on first use.
 *
 * 0 feeds nothing, 1 the near stars alone, 2 or more the near stars and the
 * far crosses (the default when unset or empty); a negative value reads as 0.
 * The guest draw runs at every setting, so the A/B prices the painted pixels
 * from one binary.
 *
 * The far cross is fed at its own projected depth, with no ordering-table pull
 * applied: the routine keys it on the same view-Z the depth comes from, so the
 * depth test reproduces the ordering the routine asked for. Where world
 * geometry stands between the camera and the gem, the cross is covered - in
 * the ordering table and under the depth test alike - and where nothing does,
 * it draws.
 * @return The mode, at least 0. */
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
 * record's own reaches them, and neither do the spin and the size. The rebase
 * measures the record's screen displacement as the difference of two
 * projections - the position the present shows under the staged camera,
 * against the position the tick drew under the tick's camera - moves the
 * primitive by it, rotates the corner offsets about the projected point by the
 * change in spin phase and scales them by the change in extent. At the end of
 * the tick's own interval the displaced position, the shown phase and the
 * staged camera are all the tick's, so both projections take identical inputs
 * through identical arithmetic and every term is exactly zero.
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

/** @brief The spin phase in sine-table entries, 256 to the turn, of a record at a given remaining lifetime.
 *
 * The phase is the phase byte less four entries per lifetime step (:2270-2277
 * masks the difference to a byte and doubles it for the halfword index), so a
 * record turns four entries per lifetime step as its lifetime counts down. A
 * fractional lifetime gives the phase between two steps; an integral one
 * gives the routine's own byte.
 * @param spin The record's spin-phase byte.
 * @param life The remaining lifetime, fractional on a sub-tick present.
 * @return The phase in [0, 256). */
static double glint_phase_at(uint8_t spin, double life) {
    const double a = fmod((double)spin - 4.0 * life, 256.0);
    return a < 0.0 ? a + 256.0 : a;
}

/** @brief The spin phase the routine drew a record at, in sine-table entries.
 *
 * @param r The record as the tick read it.
 * @return glint_phase_at of the record's phase byte and lifetime byte. */
static double glint_angle(const glint_rec* r) {
    return glint_phase_at(r->spin, (double)r->life);
}

/** @brief Compute a record's displacement, phase and lifetime between the pose the tick drew and the pose this present shows.
 *
 * Reads the record's previous snapshot from the interp object table and
 * blends toward it by 1 - alpha. Returns 0, leaving the tick's position,
 * phase and lifetime, for a record with no previous snapshot, a snapshot
 * blob too short for the position and packed words, snapshots more than one
 * and a half tick periods apart, a reused slot, or a displacement beyond
 * GLINT_REUSE_CEILING; the primitive then moves only with the camera.
 *
 * Every byte of the packed word but the lifetime is written once by the
 * spawner and never again, so an unequal upper three bytes is a slot that took
 * a new record; the lifetime itself only ever falls while a record lives, so a
 * risen lifetime is a reuse that happened to keep them.
 * @param r The paired record as the tick read it.
 * @param alpha The present's position in the tick interval, 1 at its end.
 * @param off Receives the world-unit offset from the tick's position.
 * @param show_ang Receives the spin phase the present shows.
 * @param show_life Receives the lifetime the present shows.
 * @return 1 when the previous snapshot is the same gem, else 0. */
static int glint_record_pair(const glint_rec* r, double alpha, double off[3],
                             double* show_ang, double* show_life) {
    off[0] = off[1] = off[2] = 0.0;
    *show_ang = glint_angle(r);
    *show_life = (double)r->life;
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
    /* The lifetime falls monotonically while the record lives (the reuse tests
     * above), so the shown lifetime is the straight blend of the two bytes.
     * The phase byte is the same in both (the packed-word test above), so the
     * shown phase is the routine's phase at the shown lifetime. */
    *show_life = (double)r->life + ((double)(pp & 0xFFu) - (double)r->life) * rest;
    *show_ang = glint_phase_at(r->spin, *show_life);
    return 1;
}

/** @brief The lifetime factor of the star's size, a triangular ramp on the remaining lifetime.
 *
 * L below 0x20 and 2 * (0x30 - L) from there, plus eight
 * (mobyfunc.s:2278-2285, Spyro 2 8004ADC4-8004ADDC; Spyro 3 8002F1E4-8002F1FC).
 * The routine multiplies it by the size byte (:2286, 8004ADE0 / 8002F200) and
 * scales the sine pair of the rotation basis by that product (:2324 and :2333,
 * 8004AE70 and 8004AE94 / 8002F290 and 8002F2B4), so the star's outline and
 * the cross's arms are proportional to it. Evaluated at a fractional lifetime
 * it is the same two line segments, continuous at 0x20.
 * @param life The remaining lifetime L.
 * @return The ramp value. */
static double glint_size_ramp(double life) {
    return (life < 32.0 ? life : 2.0 * (48.0 - life)) + 8.0;
}

/* Accepted range of the extent ratio glint_extent_scale returns; a ratio
 * outside it is a camera cut or a slot reuse and the tick's extent is held.
 * The band is the discontinuity guard GLINT_REUSE_CEILING gives the
 * displacement. */
#define GLINT_SCALE_MIN 0.25
#define GLINT_SCALE_MAX 4.0

/** @brief Whether the sub-tick replay scales a primitive's extent, latched on first use.
 *
 * OPENPETE_GLINT_EXTENT=0 holds the tick's extent on the replay, the
 * engagement leg of the sub-tick extent harness; any other value or none
 * enables the scale.
 * @return 1 when enabled, else 0. */
static int glint_extent_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLINT_EXTENT"); v = !(e && *e == '0'); }
    return v;
}

/** @brief Whether the sub-tick replay rotates a primitive to the shown spin phase, latched on first use.
 *
 * OPENPETE_GLINT_SPIN=0 holds the tick's spin phase on the replay, the
 * engagement leg of the sub-tick phase harness; any other value or none
 * enables the rotation.
 * @return 1 when enabled, else 0. */
static int glint_spin_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLINT_SPIN"); v = !(e && *e == '0'); }
    return v;
}

/** @brief The scale from the extent the tick drew a primitive at to the extent this present shows.
 *
 * Applies to the primitive's corner offsets about its record point.
 * The routine loads the size basis into the GTE rotation registers and
 * projects the outline against the record's own view-space translation, so
 * every corner of a star shares the record point's view-Z and the screen
 * extent is proportional to the size factor and inversely proportional to that
 * view-Z. The size byte is written once by the spawner, so the size factor's
 * ratio is the ramp's ratio (glint_size_ramp at the shown and the tick's
 * lifetime).
 *
 * The far cross (the branch the routine takes at half-world view-Z 0x2000 and
 * beyond, Spyro 2 8004AEBC-8004AEC0, Spyro 3 8002F2DC-8002F2E0) first
 * multiplies both basis terms by that same view-Z and shifts them right by 13
 * (Spyro 2 8004B004-8004B02C, Spyro 3 8002F424-8002F44C). The perspective
 * divide then cancels the view-Z, so a cross's screen arm length is the size
 * factor alone and its scale omits the view-Z term. A near star multiplies
 * by the ratio of the tick's record view-Z to the shown view-Z.
 *
 * Returns 1.0 with no arithmetic at the end of the tick's own interval
 * (alpha >= 1) and when OPENPETE_GLINT_EXTENT=0. A nonpositive view-Z or ramp,
 * or a ratio outside [GLINT_SCALE_MIN, GLINT_SCALE_MAX], also gives 1.0.
 * @param r The paired record as the tick read it; near_mode selects the
 *        branch the tick drew.
 * @param shown The record point projected under the pose this present shows.
 * @param show_life The lifetime this present shows.
 * @param alpha The present's position in the tick interval, 1 at its end.
 * @return The scale applied to the corner offsets. */
static double glint_extent_scale(const glint_rec* r, const ngte_proj* shown,
                                 double show_life, double alpha) {
    if (alpha >= 1.0 || !glint_extent_enabled()) return 1.0;
    double scl = 1.0;
    if (r->near_mode) {
        if (!(r->pvz > 0.0) || !(shown->m3 > 0.0)) return 1.0;
        scl = r->pvz / shown->m3;
    }
    if (show_life != (double)r->life) {
        const double tick_ramp = glint_size_ramp((double)r->life);
        const double show_ramp = glint_size_ramp(show_life);
        if (!(tick_ramp > 0.0) || !(show_ramp > 0.0)) return 1.0;
        scl *= show_ramp / tick_ramp;
    }
    if (!(scl >= GLINT_SCALE_MIN) || !(scl <= GLINT_SCALE_MAX)) return 1.0;
    return scl;
}

/* OPENPETE_GLINT_REBASE_DBG=1: the rebase's verdicts per run. A run whose
 * lerped count is zero rebased nothing against a previous record and measured
 * nothing; a snapped primitive had no usable previous record (none, a stale
 * pair, or a slot that took a different gem). */
static unsigned long s_reb_stat[3];   /* 0 lerped, 1 snapped, 2 rotated */

/** @brief Print the rebase verdict counters to stderr.
 *
 * Registered with atexit by glint_rebase_all when OPENPETE_GLINT_REBASE_DBG is
 * set. Prints how many primitives took a lerped record pair, how many
 * snapped to the tick's pose, and how many were rotated. */
static void glint_rebase_report(void) {
    fprintf(stderr, "[glint-rebase] lerped=%lu snapped=%lu rotated=%lu\n",
            s_reb_stat[0], s_reb_stat[1], s_reb_stat[2]);
}

/** @brief Compute the rebase of one captured primitive onto the pose this present shows.
 *
 * Resets the item's rebase to the identity. A primitive with no owning record
 * stays inactive. Otherwise the record's position, spin phase and lifetime at
 * this present come from glint_record_pair (the tick's own values when the
 * pair snaps), the position is re-projected under the staged view, and the
 * rebase takes the screen displacement from the record's tick projection, the
 * scene depth of the re-projected point, the corner rotation from the tick's
 * phase to the shown phase (ngte_table_rot) and the extent scale. The shown
 * phase is the tick's when OPENPETE_GLINT_SPIN=0. Updates the s_reb_stat
 * counters.
 * @param cpu The guest CPU state, read only.
 * @param v The projection view of this present.
 * @param alpha The present's position in the tick interval, 1 at its end.
 * @param i Index of the primitive in s_item and s_reb. */
static void glint_rebase_item(CPUState* cpu, const ngte_view* v, double alpha, int i) {
    glint_item* it = &s_item[i];
    glint_rebase* r = &s_reb[i];
    memset(r, 0, sizeof *r);
    r->scl = 1.0;
    if (!it->src.addr) return;                   /* an unpaired primitive */
    double off[3], show_ang = 0.0, show_life = 0.0;
    const int paired = glint_record_pair(&it->src, alpha, off, &show_ang, &show_life);
    if (!glint_spin_enabled()) show_ang = glint_angle(&it->src);
    r->ang = show_ang;
    s_reb_stat[paired ? 0 : 1]++;
    double p[3], vo[3];
    for (int k = 0; k < 3; k++) p[k] = (double)(int32_t)it->src.w[k] + off[k];
    glint_view_offset(v, p, vo);
    ngte_proj o;
    glint_project(v, vo, &o);
    r->dx = (float)(o.sx - it->src.psx);
    r->dy = (float)(o.sy - it->src.psy);
    r->depth = openpete_native_scene_viewz(
        openpete_worldvz_scaled((float)o.m3, 2.0f));
    r->active = 1;
    r->rot = ngte_table_rot(cpu, glint_angle(&it->src), show_ang, &r->rc, &r->rs)
                 ? 1u : 0u;
    s_reb_stat[2] += r->rot;
    r->scl = glint_extent_scale(&it->src, &o, show_life, alpha);
}

/** @brief Rebase every captured primitive onto the pose this present shows.
 *
 * The staged camera resolves through the sub-tick view, so building a view
 * here reads the interpolated transform the rest of the pass draws under.
 * OPENPETE_GLINT_REBASE_DBG, latched on first call, registers
 * glint_rebase_report at exit.
 * @param cpu The guest CPU state, read only.
 * @param alpha The present's position in the tick interval, 1 at its end. */
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

/** @brief Carry one corner through its primitive's rebase.
 *
 * The corner keeps its offset from the record's projected point through the
 * rotation and the extent scale, then the primitive translates by that
 * point's displacement; an inactive rebase leaves the corner unchanged.
 *
 * The routine's basis maps an outline point (x, y) to (c*x - s*y, s*x + c*y)
 * in screen space, c and s the size-scaled table entries at the phase and a
 * quarter circle on: GTE rotation row one is (c, -s, 0), row two (s, c, 0)
 * (Spyro 2 8004AEC4-8004AEE0 near, 8004B030-8004B04C far; Spyro 3
 * 8002F2E4-8002F300, 8002F450-8002F46C). The change from the tick's phase to
 * the shown phase is therefore the same-sense rotation by (rc, rs).
 * @param it The paired primitive; its record's projected point is the pivot.
 * @param r The primitive's rebase.
 * @param x The corner's screen X, rebased in place.
 * @param y The corner's screen Y, rebased in place. */
static void glint_rebase_xy(const glint_item* it, const glint_rebase* r,
                            float* x, float* y) {
    if (!r->active) return;
    if (r->rot || r->scl != 1.0) {
        double ox = (double)*x - it->src.psx;
        double oy = (double)*y - it->src.psy;
        if (r->rot) {
            const double rx = ox * r->rc - oy * r->rs;
            oy = ox * r->rs + oy * r->rc;
            ox = rx;
        }
        *x = (float)(it->src.psx + ox * r->scl);
        *y = (float)(it->src.psy + oy * r->scl);
    }
    *x += r->dx;
    *y += r->dy;
}

/** @brief Push a four-corner primitive to the native scene as two untextured triangles.
 *
 * The triangles are (0,1,2) and (1,3,2) in the WORLD group, all corners at one
 * depth, with the corner colours' low 24 bits made opaque.
 * @param x Corner screen X coordinates.
 * @param y Corner screen Y coordinates.
 * @param rgb Corner colour words.
 * @param depth The scene depth every corner takes.
 * @param blend The native blend mode. */
static void glint_add_quad(const float x[4], const float y[4],
                           const uint32_t rgb[4], float depth, int blend) {
    static const int split[2][3] = { {0, 1, 2}, {1, 3, 2} };
    for (int s = 0; s < 2; s++) {
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            const int j = split[s][k];
            st.v[k].sx = x[j]; st.v[k].sy = y[j]; st.v[k].z = depth;
            st.v[k].pvz = depth;   /* fed at one depth: W constant across the prim */
            st.v[k].u = 0.0f;  st.v[k].v = 0.0f;
            st.v[k].rgba = (rgb[j] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.blend = blend;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    }
}

/** @brief Feed one captured LINE_F2 as a one-pixel-wide quad.
 *
 * A line has no area, so it feeds as a quad extending half a pixel either
 * side along its own perpendicular, the width the GPU rasterizes it at; a
 * degenerate line takes a horizontal offset. Both endpoints carry the
 * record's rebase, so the perpendicular follows the rebased line.
 * @param it The captured line.
 * @param rb Its rebase.
 * @param depth The scene depth both triangles take.
 * @param blend The native blend mode. */
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

/** @brief Push every captured primitive to the native scene, rebased when its rebase is active.
 *
 * Does nothing when OPENPETE_GLINT selects 0 or nothing was captured. Each
 * star quad (code GLINT_QUAD) goes through glint_add_quad after the rebase
 * moves its corners; with mode 2 or more each cross line goes through
 * glint_feed_line. The depth is the rebase's when active, else the depth the
 * pairing gave. Counts the triangles fed in s_fed_star_tris and
 * s_fed_cross_tris and tags them as flare and spark particles with sel 0,
 * since neither kind carries a game particle class. */
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
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_FLARE, 0, 0));
            float x[4] = { p->x[0], p->x[1], p->x[2], p->x[3] };
            float y[4] = { p->y[0], p->y[1], p->y[2], p->y[3] };
            const uint32_t c[4] = { p->rgb[0], p->rgb[1], p->rgb[2], p->rgb[3] };
            for (int k = 0; k < 4; k++) glint_rebase_xy(&s_item[i], rb, &x[k], &y[k]);
            glint_add_quad(x, y, c, depth, s_item[i].blend);
            s_fed_star_tris += 2;
        } else if (p->code == GLINT_LINE && mode >= 2) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_SPARK, 0, 0));
            glint_feed_line(&s_item[i], rb, depth, s_item[i].blend);
            s_fed_cross_tris += 2;
        }
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Read a primitive's outline corners as the routine placed them.
 *
 * A star quad yields its three rim corners (corners 0, 1 and 3; corner 2 is
 * the shared centre); a cross line yields its two endpoints.
 * @param it The captured primitive.
 * @param cx Receives the corner X coordinates.
 * @param cy Receives the corner Y coordinates.
 * @return The number of corners, 0 for any other primitive. */
static int glint_outline(const glint_item* it, float cx[3], float cy[3]) {
    static const int rim[3] = { 0, 1, 3 };
    if (it->p.code == GLINT_QUAD && it->p.nv == 4) {
        for (int k = 0; k < 3; k++) { cx[k] = it->p.x[rim[k]]; cy[k] = it->p.y[rim[k]]; }
        return 3;
    }
    if (it->p.code != GLINT_LINE) return 0;
    for (int k = 0; k < 2; k++) {
        const uint32_t w = it->p.raw[2 + k];
        cx[k] = (float)(int16_t)(w & 0xFFFFu);
        cy[k] = (float)(int16_t)(w >> 16);
    }
    return 2;
}

/* One record's accumulator in the trace below: the extent sum, the corner
 * centroid, and the fourth-power moment of the corner offsets. */
typedef struct {
    uint32_t addr; double sum; int n; double px, py, cx, cy;
    double mre, mim;   /* sum of (dx + i*dy)^4 over the corners */
    double ph;         /* the spin phase the feed rotated the record to */
    int    life;       /* the tick's lifetime byte */
    int    near_mode;  /* the branch the tick drew the record with */
    double vz;         /* the tick's half-world view-Z of the record point */
} glint_ext_acc;

/** @brief Add one primitive's outline corners to its record's trace accumulator.
 *
 * The record's projected point and the corners are both carried through the
 * primitive's rebase. Adds each corner's distance from that point, the corner
 * coordinates, and the fourth power of each corner offset as a complex
 * number; overwrites the point, the shown spin phase, the lifetime byte, the
 * branch and the view Z with this primitive's.
 * @param it The paired primitive.
 * @param rb Its rebase; inactive leaves the tick's positions and phase.
 * @param acc The record's accumulator. */
static void glint_ext_add(const glint_item* it, const glint_rebase* rb, glint_ext_acc* acc) {
    float cx[3], cy[3];
    const int nc = glint_outline(it, cx, cy);
    const double px = it->src.psx + (rb->active ? rb->dx : 0.0f);
    const double py = it->src.psy + (rb->active ? rb->dy : 0.0f);
    acc->px = px;
    acc->py = py;
    acc->ph = rb->active ? rb->ang : glint_angle(&it->src);
    acc->life = it->src.life;
    acc->near_mode = it->src.near_mode;
    acc->vz = it->src.pvz;
    for (int k = 0; k < nc; k++) {
        glint_rebase_xy(it, rb, &cx[k], &cy[k]);
        const double dx = (double)cx[k] - px, dy = (double)cy[k] - py;
        acc->sum += hypot(dx, dy);
        acc->cx += cx[k];
        acc->cy += cy[k];
        const double re2 = dx * dx - dy * dy, im2 = 2.0 * dx * dy;
        acc->mre += re2 * re2 - im2 * im2;
        acc->mim += 2.0 * re2 * im2;
        acc->n++;
    }
}

/** @brief The drawn orientation of one record's outline, in sine-table entries (256 to the turn) modulo 64.
 *
 * The star's four spikes and the cross's four arms repeat every quarter
 * turn, so the argument of the summed fourth power of the corner offsets,
 * over four, is the outline's angle; the fourth power weights the spike tips,
 * the outermost corners, above the shoulder points between them.
 * @param a The record's accumulator.
 * @return The angle in [0, 64), or 0 when the moment is zero. */
static double glint_drawn_orientation(const glint_ext_acc* a) {
    if (a->mre == 0.0 && a->mim == 0.0) return 0.0;
    const double t = atan2(a->mim, a->mre) * (32.0 / 3.14159265358979323846);
    return fmod(t + 64.0, 64.0);
}

/** @brief Print the extent and orientation trace of every paired record this present fed.
 *
 * OPENPETE_GLINT_TRACE, latched on first call, enables it when set, non-empty
 * and not starting with '0': two lines per paired record per present that fed
 * this producer. `[glint-ext]` carries the record's address and the extent the
 * feed drew it at: the mean distance of its outline corners (a star quad's
 * three rim corners, a cross line's two endpoints) from its projected point,
 * both as the feed placed them. `[glint-rot]` carries the outline's drawn
 * orientation (glint_drawn_orientation) and the spin phase the feed asked for,
 * the routine's phase on a canonical present and the rebase's shown phase on a
 * sub-tick one. On a canonical present the corners are the routine's own
 * outline; on a sub-tick present they are the outline the rebase carried
 * there.
 * @param leg The present kind printed on each line, "canon" or "inter".
 * @param alpha The present's position in the tick interval, printed. */
static void glint_trace(const char* leg, double alpha) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_GLINT_TRACE"); on = (e && *e && *e != '0'); }
    if (!on) return;
    glint_ext_acc acc[GLINT_MAX_PRIMS];
    int nrec = 0;
    for (int i = 0; i < s_item_n; i++) {
        if (!s_item[i].src.addr) continue;
        int r = 0;
        while (r < nrec && acc[r].addr != s_item[i].src.addr) r++;
        if (r == nrec) {
            memset(&acc[r], 0, sizeof acc[r]);
            acc[r].addr = s_item[i].src.addr;
            nrec++;
        }
        glint_ext_add(&s_item[i], &s_reb[i], &acc[r]);
    }
    for (int r = 0; r < nrec; r++) {
        if (acc[r].n <= 0) continue;
        fprintf(stderr, "[glint-ext vbl=%u leg=%s alpha=%.3f] rec=%08X ext=%.4f n=%d"
                        " at=(%.2f,%.2f) c=(%.3f,%.3f)\n",
                openpete_feed_key_now(), leg, alpha, acc[r].addr, acc[r].sum / (double)acc[r].n,
                acc[r].n, acc[r].px, acc[r].py, acc[r].cx / (double)acc[r].n,
                acc[r].cy / (double)acc[r].n);
        fprintf(stderr, "[glint-rot vbl=%u leg=%s alpha=%.3f] rec=%08X ori=%.4f ph=%.4f"
                        " ext=%.4f n=%d life=%d near=%d vz=%.1f\n",
                openpete_feed_key_now(), leg, alpha, acc[r].addr, glint_drawn_orientation(&acc[r]),
                acc[r].ph, acc[r].sum / (double)acc[r].n, acc[r].n, acc[r].life,
                acc[r].near_mode, acc[r].vz);
    }
}

/** @brief Check one star's captured geometry for a shared centre corner and count its distinct rim points.
 *
 * The star must be eight four-vertex quads sharing one centre corner (corner
 * 2); their other corners are the rim points of the outline the routine
 * transformed, sixteen at most. A decode that mis-slices the quad words, or a
 * walk that straddles a primitive boundary, breaks the shared centre. A small
 * star on screen quantizes neighbouring rim points onto the same pixel, so
 * fewer than sixteen is normal.
 * @param first Index in s_item of the star's first quad.
 * @param count The number of quads the star holds.
 * @return The number of distinct rim points, or -1 when the quad count is not
 *         eight, a quad is not four-vertex, or the centre is not shared. */
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

/** @brief Count the call's stars that pass glint_star_topology_ok and the largest rim count any reached.
 *
 * A star is the run of quads after one draw-mode packet, up to the next
 * packet or the end of the capture.
 * @param ok Receives the number of passing stars.
 * @param rim_max Receives the largest distinct rim count of a passing star. */
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

/** @brief Measure how far each paired primitive's record point lands from the point the routine drew it about.
 *
 * The reference point is a star's shared centre corner, taken once per run of
 * quads, or a cross line's midpoint, taken per line; the distance is the
 * Manhattan distance in pixels. The sub-tick rotation pivots on the projected
 * point, so this bounds the error that pivot introduces; a residual that is
 * not a fraction of a pixel means the walk is projecting a different record
 * than the one that emitted the primitive.
 * @param mean Receives the mean distance, 0 when nothing was measured.
 * @param worst Receives the largest distance.
 * @param n Receives the number of points measured. */
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

/** @brief Print the per-call glint census when the VBL lies in the OPENPETE_GLINT_CENSUS range.
 *
 * OPENPETE_GLINT_CENSUS=LO-HI (or a single VBL): per call in that range, the
 * primitives the routine appended by GPU code, the stars its draw-mode packets
 * delimit and the crosses its line pairs delimit, the stars passing the
 * topology check, the near and far records the pool walk predicted, the
 * native triangles this feeder emitted from each class, how many primitives
 * were left unpaired, and the pairing's pivot residual; then one line per star
 * centre and per cross first endpoint with its paired depth. A capture whose
 * fed count is zero measured nothing.
 * @param start First polybuf address of the routine's output.
 * @param end One past the last.
 * @param star_before The fed star triangle total before this call.
 * @param cross_before The fed cross triangle total before this call. */
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

/** @brief Dispatch hook on the glint routine: run it once through the shared bracket, then capture, pair and feed its primitives.
 *
 * The world draw's call is the only one in either image, so the hook takes
 * every entry (Spyro 2 draw/func_800155A0.s:32, Spyro 3 draw/func_8001E460.s:30).
 * A taken call empties the captured set, retiring the previous epoch's set
 * first when it is the epoch's first call (cut_held_sets_capture).
 * When the routine appended anything, its primitives are captured and paired
 * under the tick's view, and fed natively with the canonical trace when the
 * scene is live and RP_SPARKLE is selected. The census runs on every taken
 * call.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the bracket declines and the ordinary dispatch must run the
 *         routine, else 1. */
static int on_glint_draw(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    cut_held_sets_capture(&s_sets);
    s_item_n = 0; s_stars = 0; s_crosses = 0; s_pred_near = 0; s_pred_far = 0;
    const uint32_t star_before = s_fed_star_tris, cross_before = s_fed_cross_tris;
    if (endp > startp) {
        glint_capture_prims(cpu, startp, endp);
        ngte_view view;
        ngte_view_build(cpu, &view);
        glint_pair_depths(cpu, &view);
        if (feed && render_path_enabled(RP_SPARKLE)) {
            render_material_level_push(cpu);
            glint_feed_native();
            glint_trace("canon", 1.0);
        }
    }
    glint_census(startp, endp, star_before, cross_before);
    return hook_bracket_end(&k_bracket);
}

/* Sub-tick re-extract; documented at the declaration in glint.h. */
void glint_reextract(CPUState* cpu, int bracketed, double alpha, int hold) {
    const int pick = cut_held_sets_enter(&s_sets, hold);
    if (hook_reextract_ready_held(RP_SPARKLE, pick)) {
        if (pick >= 0) {
            /* The staged camera is tick N-1's, the camera this set was drawn
             * under: the stars stand where that tick drew them. */
            for (int i = 0; i < s_item_n; i++) s_reb[i].active = 0;
        } else if (bracketed) {
            glint_rebase_all(cpu, alpha);
        }
        render_material_level_push(cpu);
        glint_feed_native();
        glint_trace("inter", bracketed ? alpha : 1.0);
    }
    cut_held_sets_leave(&s_sets, pick);
}

/* The hook address is the layout's glint draw site, bound at boot: a static
 * initializer cannot read another TU's const. A game with no located routine
 * leaves it 0 and the registry dispatches nothing. */
static feature_dispatch_entry_t glint_dispatch[] = {
    { 0u, on_glint_draw, 0u },
    { 0u, NULL, 0u },
};

/** @brief Bind the glint dispatch entry to the layout's routine address.
 *
 * A game whose layout locates no routine leaves the address 0.
 * @param opts Unused. */
static void glint_apply_options(const openpete_options_t* opts) {
    (void)opts;
    glint_dispatch[0].addr = game_layout.glint.hook;
}

const feature_module_t glint_module = {
    .name           = "glint_native",
    .apply_options  = glint_apply_options,
    .dispatch_table = glint_dispatch,
};
