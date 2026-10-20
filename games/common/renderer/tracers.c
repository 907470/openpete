// games/common/renderer/tracers.c - the tracer ribbon feeders: two handwritten
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
//   * the point-ring ribbon (asm/mobyfunc.s:2940-3181, Spyro 3 offsets). Walks
//     a 0x20-slot array of records (Spyro 3: 0x6C bytes; Spyro 2: 0x30 bytes),
//     each holding a circular buffer of packed unsigned XYZ halves (Spyro 3:
//     16 entries; Spyro 2: 6) plus a head index, a point count, a signed width
//     byte and a colour-plus-command word (Spyro 3: at +0x64 and
//     +0x68). It RTPS-projects every live point of a record into the
//     scratchpad, then walks consecutive pairs and appends one 13-word
//     POLY_GT4 per segment: the two projected
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
// them. What the primitives do not carry is depth - the PSX orders them by
// ordering-table bucket - so each one's view-Z is recovered from the producer's
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
// are the geometry. What the two share - the hook bracket, the polybuf walk,
// the primitive decode and the projection helpers - is already shared and is
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
#include "renderer/cut_guard.h"     /* cut_held_sets_t: the held cut's tick N-1 set */
#include "renderer/tracers.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_interp_objs.h"   /* the per-record prev/curr snapshots */
#include "openpete_psx.h"           /* openpete_psx_to_host_span */
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

/* Descriptor shape of the indexed mesh (asm/mobyfunc.s:3217-3218, 3277-3279). */
#define MSH_VERTS        0x00u
#define MSH_INDICES      0x08u
#define MSH_SHIFT        0x10u

/* The scratchpad table both routines project into holds 8 bytes per vertex:
 * the screen XY word and the MAC3-plus-outcode word (:3196-3200, :3218-3221). */
#define SPAD_MAX_VERTS   128

#define TR_MAX_PRIMS     512    /* primitives one call may contribute */
#define TR_MAX_TICK      2048   /* primitives one tick may contribute */
#define TR_MAX_LISTS     8      /* mesh vertex lists one tick may draw */

/* Where one corner of a primitive came from: the point the routine projected
 * to place it, as the sub-tick rebase needs it back. `key` names the point's
 * previous state in the keyed store - the ribbon record, or the mesh vertex's
 * own address - and is 0 on a corner with no source. `age` is the ribbon
 * point's place counted back from the record's head. `w` is the point in full
 * world units as the tick drew it and (sx, sy) its screen position under the
 * tick's camera, projected by the same arithmetic the rebase projects the
 * shown point with. */
typedef struct {
    uint32_t key;
    int      age;
    int32_t  w[3];
    double   sx, sy;
    int      ok;       /* the point stood in front of the near floor */
} tr_corner;

enum { TR_SRC_NONE = 0, TR_SRC_RIBBON, TR_SRC_MESH };

typedef struct {
    ngte_prim p;
    float     depth;
    int       paired;
    int       src;             /* TR_SRC_*: which producer's point each corner holds */
    tr_corner c[4];
    float     dx[4], dy[4];    /* the sub-tick screen displacement of each corner */
} tr_item;

static tr_item  s_item[TR_MAX_PRIMS];
static int      s_item_n;

/* The tick's whole set, replayed by the sub-tick leg: nine calls contribute to
 * one canonical present and the polybuf is reused before the next one. The
 * set the last capture of a new canonical epoch retired is kept beside it,
 * which a held camera cut shows when it is tick N-1's. */
static tr_item  s_tick_buf[2][TR_MAX_TICK];
static tr_item* s_tick = s_tick_buf[0];
static int      s_tick_n;
static tr_item* s_tick_old = s_tick_buf[1];
static int      s_tick_old_n;

/** @brief Exchange the tick's held set with the retired one, with their counts. */
static void tr_sets_swap(void) {
    tr_item* t = s_tick; s_tick = s_tick_old; s_tick_old = t;
    const int n = s_tick_n; s_tick_n = s_tick_old_n; s_tick_old_n = n;
}

static cut_held_sets_t s_sets = { tr_sets_swap, &s_tick_n, "tracers", { 0u, 0u } };

/* The mesh vertex lists the most recent drawing frame read: address and
 * vertex count. The set stands until the next frame that draws a mesh, so
 * every capture between two draws sees it. */
static uint32_t s_list_addr[TR_MAX_LISTS];
static uint32_t s_list_n[TR_MAX_LISTS];
static int      s_lists;
static unsigned s_list_frame = 0xFFFFFFFFu;

static uint32_t s_code_hist[256];
static uint32_t s_tag_hist[256];
static uint32_t s_fed_tris;
static int      s_last_predicted;   /* segments the record walk predicted */
static int      s_live;             /* records holding a live pointer */
static int      s_boxed;            /* of those, records the box test dropped */

/** @brief Stores one decoded primitive as an unpaired item in the call's item set.
 *
 * The item starts at depth 1 with no source and zeroed corners and
 * displacements.
 * @param ctx Unused.
 * @param s The primitive.
 * @return 1 when stored, 0 when the set holds TR_MAX_PRIMS items. */
static int tr_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= TR_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 1.0f;   /* unpaired: the near guard (view-Z 1) */
    s_item[s_item_n].paired = 0;
    s_item[s_item_n].src = TR_SRC_NONE;
    memset(s_item[s_item_n].c, 0, sizeof s_item[s_item_n].c);
    memset(s_item[s_item_n].dx, 0, sizeof s_item[s_item_n].dx);
    memset(s_item[s_item_n].dy, 0, sizeof s_item[s_item_n].dy);
    s_item_n++;
    return 1;
}

/** @brief Decodes the primitives a routine call appended to the polybuf into the call's item set.
 *
 * Both routines append one 13-word primitive at a time (tag length 12), so
 * the walk admits tags up to 12 words and at most TR_MAX_PRIMS primitives.
 * Each primitive becomes an unpaired item at the near-guard depth; the code
 * and tag histograms count every packet walked.
 * @param cpu The guest CPU state.
 * @param start The polybuf cursor before the call.
 * @param end The polybuf cursor after the call. */
static void tr_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 12, .raw_words_max = 13, .guard = TR_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, tr_prim_sink, NULL);
}

/* ---- corner sources --------------------------------------------------------
 *
 * The rebase needs, per corner, the point the routine placed it at. Both
 * routines load the camera-relative offset (x - cx, cy - y, cz - z) as
 * (VZ0, VX0, VY0) (ribbon :2974-2999, mesh :3223-3237); a uniform scale of that
 * vector, the ribbon's halving or the mesh's right shift, leaves the
 * perspective divide's quotient unchanged, so both project here in full world
 * units through one function. */

/* The view-Z below which a point has no stable screen position: the ribbon
 * drops a segment whose two ends average under 128 in its halved units
 * (:3152-3154), which is 256 in full world units. */
#define TR_NEAR_M3 256.0

/** @brief Projects a world point to the screen under a view, in full world units.
 *
 * The camera-relative offset is (x - cx, cy - y, cz - z), the vector both
 * routines load as (VZ0, VX0, VY0).
 * @param v The view.
 * @param p The world point.
 * @param o Receives the projection. */
static void tr_project_world(const ngte_view* v, const double p[3], ngte_proj* o) {
    const double vo[3] = { p[0] - v->cam[0], v->cam[1] - p[1], v->cam[2] - p[2] };
    ngte_project_offset(v, vo, o);
}

/** @brief Fills a corner's source record and its screen position under the tick's view.
 *
 * Sets ok when the point's view-Z is at or past TR_NEAR_M3.
 * @param c The corner, written.
 * @param v The tick's view.
 * @param key The previous-state key: the ribbon record or the vertex address.
 * @param age The ribbon point's place counted back from the head; 0 for a mesh
 *        vertex.
 * @param w The point in full world units. */
static void tr_corner_set(tr_corner* c, const ngte_view* v, uint32_t key, int age,
                          const int32_t w[3]) {
    c->key = key;
    c->age = age;
    memcpy(c->w, w, sizeof c->w);
    const double p[3] = { (double)w[0], (double)w[1], (double)w[2] };
    ngte_proj o;
    tr_project_world(v, p, &o);
    c->sx = o.sx;
    c->sy = o.sy;
    c->ok = o.m3 >= TR_NEAR_M3;
}

/* ---- ribbon leg: predict the segments the record walk emitted -------------- */

typedef struct { int32_t oc; int32_t sz3; double m3; tr_corner c; } rib_pt;

/* One predicted segment: its depth and the two points it joins, the head-side
 * one first. The routine writes corners 0 and 2 about the head-side point and
 * corners 1 and 3 about the next one (:3125-3141). */
typedef struct { float depth; tr_corner a, b; } rib_seg;

/** @brief Computes the ribbon routine's outcode word for a screen position (mobyfunc.s:2488-2508).
 *
 * The packed screen coordinate (y in the high half) is shifted left five
 * and gains one low bit per screen edge the point falls outside: 1 for a
 * packed word at or below zero, 2 for y at or past 240, 4 for x at or below
 * zero, 8 for x at or past 512. Only the low four bits are tested, and only
 * as an AND across a record's points or across a segment's two ends.
 * @param sx The screen x.
 * @param sy The screen y.
 * @return The outcode word. */
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

/** @brief Computes the GTE SZ3 of a MAC3 value with sf = 1.
 *
 * @param m3 The MAC3 value.
 * @return MAC3 floored and saturated to 0..65535. */
static int32_t rib_sz3(double m3) {
    double z = floor(m3);
    if (z < 0.0) return 0;
    if (z > 65535.0) return 65535;
    return (int32_t)z;
}

/** @brief Decodes the point a given number of places back from the head of a ribbon record's ring.
 *
 * The record image is guest RAM or a keyed-store blob. The packed word at
 * TR.packed_off holds the head index (byte 0) and the point count (byte 1);
 * the point's three unsigned halves are shifted left one as the routine
 * loads them, which gives half world units. An age past the tail is
 * clamped to the tail.
 * @param rec The record image.
 * @param len The image's length in bytes.
 * @param age Places back from the head; 0 is the head.
 * @param hw Receives the point in half world units.
 * @return The ring's point count, or 0 when the image is too short or the
 *         packed word holds no walkable ring (count below 2 or above
 *         TR.ring_n, or head not below count). */
static int rib_ring_point(const uint8_t* rec, uint32_t len, int age, int32_t hw[3]) {
    if (len < TR.packed_off + 4u) return 0;
    uint32_t packed;
    memcpy(&packed, rec + TR.packed_off, 4);
    const int head  = (int)(packed & 0xFFu);
    const int count = (int)((packed >> 8) & 0xFFu);
    if (count < 2 || count > TR.ring_n || head >= count) return 0;
    if (age >= count) age = count - 1;
    const int idx = ((head - age) % count + count) % count;
    const uint32_t off = TR.points_off + (uint32_t)idx * 6u;
    if (off + 6u > len) return 0;
    uint16_t h[3];
    memcpy(h, rec + off, 6);
    for (int k = 0; k < 3; k++) hw[k] = (int32_t)((uint32_t)h[k] << 1);
    return count;
}

/** @brief Projects one ribbon record's points head first, mirroring the routine's per-record rejects.
 *
 * The rejects are any point outside the camera-relative box (:2408-2437,
 * counted in s_boxed), every point off one common screen edge (:2496) and
 * the head's SZ3 at or past TR.far_sz (:2540). Each point's outcode, SZ3,
 * MAC3 and rebase corner (keyed by the record, aged by its place from the
 * head) are written to pt.
 * @param cpu Unused.
 * @param v The tick's view.
 * @param rec The record's guest address.
 * @param cam The camera position shifted right one, as the routine reads it.
 * @param pt Receives the projected points, head first.
 * @return The point count, or 0 when the routine drops the record. */
static int rib_project_record(CPUState* cpu, const ngte_view* v, uint32_t rec,
                              const int32_t cam[3], rib_pt* pt) {
    (void)cpu;
    const uint8_t* host = (const uint8_t*)openpete_psx_to_host_span(rec, TR.stride);
    if (!host) return 0;
    int32_t hw[3];
    const int count = rib_ring_point(host, TR.stride, 0, hw);
    if (count == 0) return 0;
    int32_t and_oc = -1;
    for (int k = 0; k < count; k++) {
        rib_ring_point(host, TR.stride, k, hw);
        const int32_t vx = hw[0] - cam[0];   /* the routine's IR3 */
        const int32_t vy = cam[1] - hw[1];   /* IR1 */
        const int32_t vz = cam[2] - hw[2];   /* IR2 */
        if (vx <= -TR.box || vx >= TR.box || vy <= -TR.box || vy >= TR.box ||
            vz <= -TR.box || vz >= TR.box) { s_boxed++; return 0; }
        ngte_proj o;
        ngte_project_gte(v->cam_M, NULL, (double)vy, (double)vz, (double)vx,
                         v->ofx, v->ofy, v, 0, &o);
        pt[k].m3  = o.m3;
        pt[k].sz3 = rib_sz3(o.m3);
        pt[k].oc  = rib_outcode((int)lrint(o.sx), (int)lrint(o.sy));
        and_oc &= pt[k].oc;
        const int32_t w[3] = { hw[0] * 2, hw[1] * 2, hw[2] * 2 };
        tr_corner_set(&pt[k].c, v, rec, k, w);
    }
    if ((and_oc & 0xF) > 0) return 0;
    if (pt[0].sz3 - TR.far_sz >= 0) return 0;
    return count;
}

/** @brief Re-walks the ribbon record array and predicts every segment the routine emitted, in emission order.
 *
 * Per live record (counted in s_live) the points are projected by
 * rib_project_record; a consecutive pair is dropped when its two outcodes
 * share an edge bit (:2652) or its ordering-table key is negative (:2692).
 * Each kept segment carries its view-Z from the pair's mean MAC3 and its
 * two rebase corners, head side first.
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @param out Receives the segments.
 * @param cap The capacity of out; segments past it are counted, not written.
 * @return The count of predicted segments. */
static int rib_predict(CPUState* cpu, const ngte_view* v, rib_seg* out, int cap) {
    int n = 0;
    s_live = 0; s_boxed = 0;
    /* The camera position the routine reads, shifted right one like its own
     * `srl` so the box test compares the same values. */
    const int32_t cam[3] = {
        (int32_t)(cpu->read_word(game_layout.cam.position + 0u) >> 1),
        (int32_t)(cpu->read_word(game_layout.cam.position + 4u) >> 1),
        (int32_t)(cpu->read_word(game_layout.cam.position + 8u) >> 1),
    };
    static rib_pt pt[RIB_POINT_MAX];
    for (uint32_t s = 0; s < TR.slots; s++) {
        const uint32_t rec = TR.ring + s * TR.stride;
        if (cpu->read_word(rec + RIB_LIVE) == 0) continue;
        s_live++;
        const int count = rib_project_record(cpu, v, rec, cam, pt);
        for (int k = 0; k + 1 < count; k++) {
            if ((pt[k].oc & pt[k + 1].oc & 0xF) > 0) continue;        /* :2652 */
            const int32_t key = ((pt[k].sz3 + pt[k + 1].sz3) >> 7) - 2;
            if (key < 0) continue;                                    /* :2692 */
            if (n < cap) {
                /* The record's halves are quarter world units and the routine
                 * halves the whole vector, so its MAC3 is half the world value. */
                const double mz = (pt[k].m3 + pt[k + 1].m3) * 0.5;
                out[n].depth = openpete_native_scene_viewz(
                    openpete_worldvz_scaled((float)mz, 2.0f));
                out[n].a = pt[k].c;
                out[n].b = pt[k + 1].c;
            }
            n++;
        }
    }
    return n;
}

/** @brief Pairs the captured ribbon primitives with the record walk's predicted segments.
 *
 * Runs rib_predict and records its count in s_last_predicted. When the count
 * equals the captured primitive count, each item takes its segment's depth,
 * marks itself paired and takes the ribbon source with its corners 0 and 2
 * from the head-side point and 1 and 3 from the next. On a mismatch every
 * item stays unpaired at the near-guard depth.
 * @param cpu The guest CPU state.
 * @param v The tick's view. */
static void rib_pair_depths(CPUState* cpu, const ngte_view* v) {
    static rib_seg pred[TR_MAX_PRIMS];
    const int n = rib_predict(cpu, v, pred, TR_MAX_PRIMS);
    s_last_predicted = n;
    if (n != s_item_n || n > TR_MAX_PRIMS) return;   /* every primitive stays unpaired */
    for (int i = 0; i < n; i++) {
        tr_item* it = &s_item[i];
        it->depth = pred[i].depth;
        it->paired = 1;
        it->src = TR_SRC_RIBBON;
        it->c[0] = it->c[2] = pred[i].a;
        it->c[1] = it->c[3] = pred[i].b;
    }
}

/* ---- mesh leg: replay the emit test out of the routine's own scratchpad ---- */

/** @brief Records a mesh vertex list the draw read, for the per-object capture.
 *
 * The list ends at the routine's -1 terminator (:3223-3225), bounded by the
 * scratchpad table's SPAD_MAX_VERTS. The first note on a new VBL empties
 * the set; a repeated address, an address outside KSEG0 RAM and a list past
 * TR_MAX_LISTS are ignored. The capture runs on every VBL and the draw once
 * per tick, so the set is replaced by the next VBL that draws, not consumed
 * by the capture: a vertex missing from one capture is evicted from the
 * store and pairs as a spawn.
 * @param cpu The guest CPU state.
 * @param vlist The vertex list's guest address. */
static void msh_note_list(CPUState* cpu, uint32_t vlist) {
    if (s_list_frame != openpete_frame) { s_list_frame = openpete_frame; s_lists = 0; }
    if (vlist < 0x80000000u || vlist >= 0x80800000u) return;
    for (int i = 0; i < s_lists; i++) if (s_list_addr[i] == vlist) return;
    if (s_lists >= TR_MAX_LISTS) return;
    uint32_t n = 0;
    while (n < SPAD_MAX_VERTS && cpu->read_word(vlist + n * 12u) != 0xFFFFFFFFu) n++;
    s_list_addr[s_lists] = vlist;
    s_list_n[s_lists] = n;
    s_lists++;
}

int tracers_mesh_lists(uint32_t* addr, uint32_t* nverts, int max) {
    int n = 0;
    for (int i = 0; i < s_lists && n < max; i++, n++) {
        addr[n] = s_list_addr[i];
        nverts[n] = s_list_n[i];
    }
    return n;
}

/** @brief Sets a mesh item's four rebase corners from the vertices its index entry names.
 *
 * Corner k is the entry's index byte k (:3358-3378 store them at +0x08,
 * +0x14, +0x20, +0x2C); the vertex is the 12-byte world XYZ triple at that
 * index of the descriptor's vertex list, keyed by its address.
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @param it The item.
 * @param vlist The vertex list's guest address.
 * @param off The four scratchpad offsets (index * 8) of the entry. */
static void msh_set_corners(CPUState* cpu, const ngte_view* v, tr_item* it,
                            uint32_t vlist, const uint32_t off[4]) {
    it->src = TR_SRC_MESH;
    for (int k = 0; k < 4; k++) {
        const uint32_t va = vlist + (off[k] >> 3) * 12u;
        const int32_t w[3] = {
            (int32_t)cpu->read_word(va + 0u),
            (int32_t)cpu->read_word(va + 4u),
            (int32_t)cpu->read_word(va + 8u),
        };
        tr_corner_set(&it->c[k], v, va, 0, w);
    }
}

/** @brief Pairs the captured mesh primitives with the entries the routine emitted, in order.
 *
 * Replays the routine's emit test over the descriptor's index list against
 * the scratchpad table it left: an entry is skipped on a common outcode edge
 * or a key outside 0..0x7FF, otherwise the next item takes the depth from
 * the entry's summed SZ and the descriptor's shift and is marked paired. When
 * the vertex list is a valid KSEG0 RAM address the list is noted and each
 * paired item gets its four vertex corners. The walk ends at the -1
 * terminator or when every item is paired, and returns with the remaining
 * items unpaired on an index outside the scratchpad table or an index list
 * outside KSEG0 RAM.
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @param desc The mesh descriptor's guest address. */
static void msh_pair_depths(CPUState* cpu, const ngte_view* v, uint32_t desc) {
    const uint32_t ilist = cpu->read_word(desc + MSH_INDICES);
    if (ilist < 0x80000000u || ilist >= 0x80800000u) return;
    const uint32_t vlist = cpu->read_word(desc + MSH_VERTS);
    const int have_verts = vlist >= 0x80000000u && vlist < 0x80800000u;
    if (have_verts) msh_note_list(cpu, vlist);
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
        if (have_verts) msh_set_corners(cpu, v, &s_item[i], vlist, off);
        i++;
    }
}

/* ---- feed ---------------------------------------------------------------- */

/** @brief Reports whether the tracer feed is enabled; OPENPETE_TRACERS=0 disables it.
 *
 * Only this feed is suppressed; the guest draws still run, so an A/B
 * measures the ribbons' painted pixels from one binary. Latched on the
 * first call.
 * @return 0 when OPENPETE_TRACERS starts with '0', else 1. */
static int tr_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TRACERS"); v = !(e && *e && *e == '0'); }
    return v;
}

/* Whether the held set carries a sub-tick displacement. Clear on every
 * canonical feed and at the end of the tick's interval, where the replay emits
 * the captured corners verbatim. */
static int s_reb_active;

/** @brief Feeds one item to the native scene as two world triangles.
 *
 * A quad is split into (0,1,2) and (1,3,2); a triangle repeats its last
 * corner. Every vertex takes the item's single depth. While the rebase is
 * active each corner is moved by its screen displacement. Only 3 and 4
 * vertex primitives are fed. Counts the triangles fed.
 * @param it The item. */
static void tr_add_prim(const tr_item* it) {
    static const int split[2][3] = { {0, 1, 2}, {1, 3, 2} };
    const ngte_prim* p = &it->p;
    const float depth = it->depth;
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
            if (s_reb_active) { st.v[k].sx += it->dx[j]; st.v[k].sy += it->dy[j]; }
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

/** @brief Feeds a set of items to the native scene under the tracer tag.
 *
 * Does nothing when OPENPETE_TRACERS disables the feed or the set is empty.
 * @param set The items.
 * @param n The item count. */
static void tr_feed_native(const tr_item* set, int n) {
    if (!tr_feed_enabled()) return;
    if (n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_TRACER(0));
    for (int i = 0; i < n; i++) tr_add_prim(&set[i]);
    openpete_native_scene_set_tag(0);
}

/** @brief Appends the call's items to the tick's held set for the sub-tick replay.
 *
 * The first call of a canonical epoch retires the previous epoch's set
 * (cut_held_sets_capture) and empties the held one, since several calls
 * contribute to one canonical present; items past TR_MAX_TICK are dropped.
 * Clears the rebase so a canonical feed emits the captured corners.
 * @return The index of the call's first item in the held set. */
static int tr_hold(void) {
    if (cut_held_sets_capture(&s_sets)) s_tick_n = 0;
    const int first = s_tick_n;
    for (int i = 0; i < s_item_n && s_tick_n < TR_MAX_TICK; i++) s_tick[s_tick_n++] = s_item[i];
    s_reb_active = 0;
    return first;
}

/** @brief Prints one stderr line per hook call inside the VBL range OPENPETE_TRACER_CENSUS names (LO-HI or one VBL).
 *
 * The line carries the polybuf interval, the primitives captured by GPU
 * code, the native triangles fed from them, the segments the record walk
 * predicted, the unpaired primitives, the live and boxed record counts and
 * the held set's size. A capture whose fed count is zero measured nothing.
 * @param leg The producer's name, "ribbon" or "mesh".
 * @param start The polybuf cursor before the call.
 * @param end The polybuf cursor after the call.
 * @param fed_before s_fed_tris before the call. */
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
/** @brief Hook-bracket pre step of the mesh leg: saves the descriptor argument in a0.
 *
 * The routine reuses a0, so the value is read before the guest draw runs.
 * @param cpu The guest CPU state.
 * @param feed Unused.
 * @param verify Unused. */
static void msh_pre(CPUState* cpu, int feed, int verify) {
    (void)feed; (void)verify;
    s_msh_desc = cpu->a0;
}
static const hook_bracket_desc k_msh = {
    .path = RP_TRACER, .prof_tag = "tracer3.srend", .pre = msh_pre, .st = &s_hb_msh,
};

/** @brief Dispatch hook of the ribbon routine: brackets the guest draw and feeds its primitives.
 *
 * After the draw, decodes the appended primitives, pairs them with the
 * predicted segments, holds them for the sub-tick replay and, when the
 * bracket asks for a feed and the tracer path is enabled, feeds them to the
 * native scene. Reads guest RAM only.
 * @param cpu The guest CPU state.
 * @param addr The routine's guest address.
 * @return 0 when the bracket declines the call, else the bracket's end
 *         result. */
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
        render_material_level_push(cpu);
        if (feed && render_path_enabled(RP_TRACER))
            tr_feed_native(&s_tick[first], s_tick_n - first);
    }
    tr_census("ribbon", startp, endp, fed_before);
    return hook_bracket_end(&k_rib);
}

/** @brief Dispatch hook of the mesh routine: brackets the guest draw and feeds its primitives.
 *
 * As on_ribbon, with the depths and corners paired from the descriptor read
 * before the draw (s_msh_desc); a call that appended no primitives does no
 * capture.
 * @param cpu The guest CPU state.
 * @param addr The routine's guest address.
 * @return 0 when the bracket declines the call, else the bracket's end
 *         result. */
static int on_mesh(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_msh, &startp, &endp, &feed, &verify))
        return 0;
    s_last_predicted = 0;
    s_item_n = 0;
    const uint32_t fed_before = s_fed_tris;
    if (endp > startp) {
        tr_capture_prims(cpu, startp, endp);
        ngte_view view;
        ngte_view_build(cpu, &view);
        msh_pair_depths(cpu, &view, s_msh_desc);
        const int first = tr_hold();
        render_material_level_push(cpu);
        if (feed && render_path_enabled(RP_TRACER))
            tr_feed_native(&s_tick[first], s_tick_n - first);
    }
    tr_census("mesh", startp, endp, fed_before);
    return hook_bracket_end(&k_msh);
}

/* ---- sub-tick rebase ------------------------------------------------------
 *
 * The primitives are the guest's own int16 output, built under the tick's
 * camera from the tick's records, so a present that re-feeds them draws every
 * ribbon where the tick drew it: neither the camera's motion nor the records'
 * own reaches it. The rebase moves each corner by the screen displacement of
 * the point the routine placed it about: that point as the present shows it,
 * projected under the staged camera, against the point as the tick drew it,
 * projected under the tick's camera by the same function. The shown point is
 * the lerp of the previous tick's point and the tick's, paired by the record's
 * slot and the point's place counted back from the ring's head, so at alpha 0
 * the ribbon lies along the previous tick's ring and the head sweeps to the
 * tick's head over the interval; a ring that grew this tick holds its new tail
 * on the previous tail. A mesh corner pairs its own vertex by address. A point
 * with no usable previous state holds the tick's world position, and the
 * camera's motion alone rebases it. At the end of the tick's interval the
 * replay emits the captured corners verbatim.
 *
 * Out of scope, each its own channel of the same record: the ribbon's width,
 * which the routine tapers in screen units from the record's width byte, its
 * DPCS fade by the head's depth, and the primitives' depth, which the replay
 * keeps from the canonical pairing. */

/* A point's speed ceiling, in full world units per tick. The store is keyed by
 * slot address, so a slot a new record respawns into inside one tick pairs
 * across two different ribbons; the owner test catches a reuse by another
 * owner and this one a reuse by the same owner, at a distance no point covers
 * in a tick. */
#define TR_REUSE_CEILING 4096.0

/* A pair spanning more than this is a discontinuity rather than motion. */
#define TR_PAIR_STALE_NS (g_openpete_tick_period_ns + g_openpete_tick_period_ns / 2)

static unsigned long s_reb_stat[3][2]; /* corners per TR_SRC_*: 0 lerped, 1 held */
static double        s_reb_worldmax;  /* the largest per-tick world step paired */
static double        s_reb_screenmax; /* the largest screen displacement applied */

/** @brief Looks up a keyed-store record that holds a usable previous and current state.
 *
 * Refuses a record with no previous state, timestamps that are not
 * increasing, a blob shorter than need, or a pair spanning more than
 * TR_PAIR_STALE_NS.
 * @param key The record's key.
 * @param need The minimum blob length in bytes.
 * @return The record, or NULL when refused. */
static const openpete_interp_obj_t* tr_pair(uint32_t key, uint32_t need) {
    const openpete_interp_obj_t* o = openpete_interp_objs_find(key);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return NULL;
    if ((uint32_t)o->blob_len < need) return NULL;
    if (o->curr_ns - o->prev_ns > TR_PAIR_STALE_NS) return NULL;
    return o;
}

/** @brief Looks up a corner's point in the previous tick's keyed-store state, in full world units.
 *
 * A mesh corner reads its vertex's previous triple; a ribbon corner decodes
 * the point of the same age from the record's previous image.
 * @param it The item holding the corner.
 * @param c The corner.
 * @param w Receives the previous point.
 * @return 1 when found; 0 for a record or vertex with no previous state, a
 *         pair spanning more than TR_PAIR_STALE_NS, a ribbon slot whose
 *         live pointer changed, or a ring that does not decode. */
static int tr_prev_point(const tr_item* it, const tr_corner* c, int32_t w[3]) {
    if (it->src == TR_SRC_MESH) {
        const openpete_interp_obj_t* o = tr_pair(c->key, 12u);
        if (!o) return 0;
        memcpy(w, o->prev, 12);
        return 1;
    }
    const openpete_interp_obj_t* o = tr_pair(c->key, TR.packed_off + 4u);
    if (!o || memcmp(o->prev + RIB_LIVE, o->curr + RIB_LIVE, 4) != 0) return 0;
    int32_t hw[3];
    if (!rib_ring_point(o->prev, o->blob_len, c->age, hw)) return 0;
    for (int k = 0; k < 3; k++) w[k] = hw[k] * 2;
    return 1;
}

/** @brief Computes one corner's sub-tick screen displacement.
 *
 * The shown point is the tick's point moved toward its previous point by
 * (1 - alpha); a previous point farther than TR_REUSE_CEILING is not used
 * and the point stays at the tick's. The displacement is the screen
 * position of the shown point under the view minus the corner's screen
 * position under the tick's camera. It is zero for a corner with no key or
 * whose tick point was behind the near floor, and for a shown point behind
 * the near floor. Updates the rebase statistics.
 * @param it The item, its dx and dy written.
 * @param k The corner index, 0..3.
 * @param v The present's view.
 * @param alpha The present's position in the tick interval. */
static void tr_rebase_corner(tr_item* it, int k, const ngte_view* v, double alpha) {
    const tr_corner* c = &it->c[k];
    it->dx[k] = 0.0f;
    it->dy[k] = 0.0f;
    if (!c->key || !c->ok) return;
    double p[3] = { (double)c->w[0], (double)c->w[1], (double)c->w[2] };
    int32_t pw[3];
    int lerped = 0;
    if (tr_prev_point(it, c, pw)) {
        double d[3], sq = 0.0;
        for (int j = 0; j < 3; j++) { d[j] = (double)pw[j] - p[j]; sq += d[j] * d[j]; }
        if (sq <= TR_REUSE_CEILING * TR_REUSE_CEILING) {
            for (int j = 0; j < 3; j++) p[j] += d[j] * (1.0 - alpha);
            if (sqrt(sq) > s_reb_worldmax) s_reb_worldmax = sqrt(sq);
            lerped = 1;
        }
    }
    s_reb_stat[it->src][lerped ? 0 : 1]++;
    ngte_proj o;
    tr_project_world(v, p, &o);
    /* A shown point behind the near floor has no screen position to move to;
     * the corner holds the tick's. */
    if (o.m3 < TR_NEAR_M3) return;
    it->dx[k] = (float)(o.sx - c->sx);
    it->dy[k] = (float)(o.sy - c->sy);
    const double m = fmax(fabs((double)it->dx[k]), fabs((double)it->dy[k]));
    if (m > s_reb_screenmax) s_reb_screenmax = m;
}

/** @brief Reports whether the sub-tick rebase is enabled; OPENPETE_TRACERS_INTERP=0 disables it.
 *
 * Disabled, the replay stays at the tick's screen positions, the
 * engagement leg of the sub-tick harness. Latched on the first call.
 * @return 0 when OPENPETE_TRACERS_INTERP starts with '0', else 1. */
static int tr_interp_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TRACERS_INTERP"); v = !(e && *e && *e == '0'); }
    return v;
}

/** @brief Prints the rebase's run totals to stderr, registered with atexit under OPENPETE_TRACER_REBASE_DBG.
 *
 * The line carries the lerped and held corner counts per producer and the
 * largest world step paired and screen displacement applied. A run whose
 * lerped count is zero rebased nothing against a previous point. */
static void tr_rebase_report(void) {
    fprintf(stderr, "[tracer-rebase] ribbon lerped=%lu held=%lu mesh lerped=%lu held=%lu "
            "worldmax=%.3f screenmax=%.3f\n",
            s_reb_stat[TR_SRC_RIBBON][0], s_reb_stat[TR_SRC_RIBBON][1],
            s_reb_stat[TR_SRC_MESH][0], s_reb_stat[TR_SRC_MESH][1],
            s_reb_worldmax, s_reb_screenmax);
}

/** @brief Computes every held corner's sub-tick screen displacement for the pose this present shows.
 *
 * The staged camera resolves through the sub-tick view, so the view built
 * here is the interpolated transform the rest of the pass draws under. At
 * alpha 1 or above, with an empty set or with the rebase disabled, the
 * rebase stays off and the replay emits the captured corners. The first
 * call past those conditions latches OPENPETE_TRACER_REBASE_DBG and
 * registers tr_rebase_report.
 * @param cpu The guest CPU state.
 * @param alpha The present's position in the tick interval, 0..1. */
static void tr_rebase_all(CPUState* cpu, double alpha) {
    s_reb_active = 0;
    if (alpha >= 1.0 || s_tick_n <= 0 || !tr_interp_enabled()) return;
    static int dbg = -1;
    if (dbg < 0) {
        const char* e = getenv("OPENPETE_TRACER_REBASE_DBG");
        dbg = (e && *e && *e != '0');
        if (dbg) atexit(tr_rebase_report);
    }
    ngte_view v;
    ngte_view_build(cpu, &v);
    for (int i = 0; i < s_tick_n; i++)
        for (int k = 0; k < 4; k++) tr_rebase_corner(&s_tick[i], k, &v, alpha);
    s_reb_active = 1;
}

/* Called from each game's reextract.c on a sub-tick present. */
void tracers_reextract(CPUState* cpu, int bracketed, double alpha, int hold) {
    const int pick = cut_held_sets_enter(&s_sets, hold);
    if (hook_reextract_ready_held(RP_TRACER, pick)) {
        /* On a held cut the staged camera is tick N-1's, the camera the shown
         * set was drawn under: alpha 1 emits its captured corners. */
        tr_rebase_all(cpu, (bracketed && pick < 0) ? alpha : 1.0);
        render_material_level_push(cpu);
        tr_feed_native(s_tick, s_tick_n);
        s_reb_active = 0;
    }
    cut_held_sets_leave(&s_sets, pick);
}

/* The hook addresses are the layout's two tracer draw sites, bound at boot: a
 * static initializer cannot read another TU's const. A game with no located
 * routine leaves either 0 and the registry dispatches nothing for it. */
static feature_dispatch_entry_t tracers_dispatch[] = {
    { 0u, on_ribbon },
    { 0u, on_mesh },
    { 0u, NULL },
};

/** @brief Binds the two dispatch rows to the layout's ribbon and mesh hook addresses.
 *
 * @param opts Unused. */
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
