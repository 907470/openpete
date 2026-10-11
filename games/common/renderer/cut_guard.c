/* games/common/renderer/cut_guard.c - see cut_guard.h. */
#include "renderer/cut_guard.h"

#include "renderer/interp_lerp.h"
#include "renderer/interp_regions_gen.h"
#include "renderer/moby.h"          /* moby_native_canon_drawn_mobys */
#include "renderer/moby_game.h"     /* moby_game.interp_key */
#include "renderer/orchestrator.h"   /* spyro_native_canon_epoch_fresh */
#include "openpete_interp_objs.h"   /* g_openpete_tick_period_ns */
#include "openpete_psx.h"           /* openpete_psx_to_host_span */
#include "game_layout.h"
#include "openpete_pace_log.h"
#include "openpete_fiber.h"         /* openpete_frame */
#include "spyro_overlay_table.h"    /* the resident overlay, for the props and attach markers */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A position step above this in one tick is a jump, not a move: measured
 * continuous motion reaches 1218 units/tick and a cut 140k. */
#define CUT_POS_LIMIT   5000.0
/* Row-dot sum of two Q12 rotation matrices, in units of 4096^2: the sum is
 * 1 + 2*cos(angle) for an orthonormal pair, so below one unit the camera turned
 * more than 90 degrees in one tick. No smooth camera move does that (it is
 * 2700 degrees/s); a 46-degree pan reads about 2.4. */
#define CUT_ROT_LIMIT   (4096.0 * 4096.0)
/* Presents this far past the forward endpoint are replaying a pair the sim
 * stopped advancing: there is no interval left to hold. */
#define CUT_PAIR_STALE_TICKS 4.0

int cut_census_on(void) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_CUT_CENSUS"); s_on = (e && *e && *e != '0'); }
    return s_on;
}

static long s_cut_hold[CUT_KINDS], s_cut_snap[CUT_KINDS];
/* The current snapshot's stamp while this present holds a camera cut
 * (cut_camera_hold), else 0. */
static op_time_t s_held_ns;

void cut_census_note(int kind, uint32_t addr, int held, const char* why,
                     const int32_t* prev_pos, const int32_t* shown_pos) {
    static const char* const names[CUT_KINDS] = { "camcut", "class", "teleport", "heldcut" };
    if (kind < 0 || kind >= CUT_KINDS) return;
    if (held) s_cut_hold[kind]++; else s_cut_snap[kind]++;
    if (!cut_census_on()) return;
    static const int32_t zero[3] = { 0, 0, 0 };
    const int32_t* pp = prev_pos ? prev_pos : zero;
    const int32_t* sp = shown_pos ? shown_pos : zero;
    double dx = (double)sp[0]-pp[0], dy = (double)sp[1]-pp[1], dz = (double)sp[2]-pp[2];
    fprintf(stderr, "[cut-census vbl=%u] kind=%s obj=%08x verdict=%s why=%s "
                    "prev=(%d,%d,%d) shown=(%d,%d,%d) D=%.3f "
                    "holds=%ld/%ld/%ld/%ld snaps=%ld/%ld/%ld/%ld\n",
            openpete_frame, names[kind], addr, held ? "hold" : "snap", why,
            pp[0], pp[1], pp[2], sp[0], sp[1], sp[2], sqrt(dx*dx + dy*dy + dz*dz),
            s_cut_hold[0], s_cut_hold[1], s_cut_hold[2], s_cut_hold[3],
            s_cut_snap[0], s_cut_snap[1], s_cut_snap[2], s_cut_snap[3]);
}

cut_held_pick_t cut_held_set_pick(uint32_t curr_epoch, uint32_t prev_epoch) {
    if (spyro_native_canon_epoch_fresh(curr_epoch))
        return spyro_native_canon_epoch_fresh(prev_epoch + 1u) ? CUT_HELD_PREV : CUT_HELD_EMPTY;
    return spyro_native_canon_epoch_fresh(curr_epoch + 1u) ? CUT_HELD_CURR : CUT_HELD_EMPTY;
}

/** @brief Exchange a producer's two sets and their epochs.
 *
 * @param s The producer's sets. */
static void cut_held_sets_exchange(cut_held_sets_t* s) {
    s->swap();
    const uint32_t e = s->epoch[0];
    s->epoch[0] = s->epoch[1];
    s->epoch[1] = e;
}

int cut_held_sets_capture(cut_held_sets_t* s) {
    const uint32_t epoch = spyro_native_canon_epoch_stamp();
    if (s->epoch[0] == epoch) return 0;
    s->swap();
    s->epoch[1] = s->epoch[0];
    s->epoch[0] = epoch;
    return 1;
}

int cut_held_sets_enter(cut_held_sets_t* s, int hold) {
    if (!hold) return -1;
    const cut_held_pick_t pick = cut_held_set_pick(s->epoch[0], s->epoch[1]);
    if (pick == CUT_HELD_PREV) cut_held_sets_exchange(s);
    cut_census_held_set(s->kind, pick, s->epoch[0], pick == CUT_HELD_EMPTY ? 0 : *s->n);
    return (int)pick;
}

void cut_held_sets_leave(cut_held_sets_t* s, int pick) {
    if (pick == CUT_HELD_PREV) cut_held_sets_exchange(s);
}

void cut_census_held_set(const char* kind, cut_held_pick_t pick, unsigned epoch, int n) {
    if (!cut_census_on()) return;
    static const char* const k_set[] = { "empty", "curr", "prev" };
    fprintf(stderr, "[cut-census vbl=%u] held-queue kind=%s set=%s epoch=%u n=%d\n",
            openpete_frame, kind, k_set[pick], epoch, n);
}

void cut_census_camera(const int32_t* pos, const int16_t* mat,
                       double alpha, int cam_cut) {
    if (!cut_census_on() || !pos || !mat) return;
    fprintf(stderr, "[cut-census vbl=%u] shown=camera a=%.3f cut=%d pos=(%d,%d,%d) "
                    "row0=(%d,%d,%d)\n",
            openpete_frame, alpha, cam_cut, pos[0], pos[1], pos[2],
            mat[0], mat[1], mat[2]);
}

const char* cut_camera_reason(const uint8_t* pcam, const uint8_t* ccam,
                              uint32_t pos_off, uint32_t mat_off,
                              const void* pgs, const void* cgs,
                              cut_cam_metrics_t* out) {
    cut_cam_metrics_t m = { 0.0, 0.0, 0u, 0u, 0 };
    const char* why = NULL;
    if (pcam && ccam) {
        const int32_t* pp = (const int32_t*)(pcam + pos_off);
        const int32_t* cp = (const int32_t*)(ccam + pos_off);
        double dx = (double)cp[0] - pp[0], dy = (double)cp[1] - pp[1], dz = (double)cp[2] - pp[2];
        m.d2 = dx*dx + dy*dy + dz*dz;
        const int16_t* pm = (const int16_t*)(pcam + mat_off);
        const int16_t* cm = (const int16_t*)(ccam + mat_off);
        for (int i = 0; i < 9; i++) m.frob += (double)pm[i] * (double)cm[i];
        /* Rotation: linearly lerping a large rotation drives the matrix through
         * a near-singular midpoint, which warps the whole scene on the
         * intermediate presents. */
        if (m.d2 > CUT_POS_LIMIT * CUT_POS_LIMIT) why = "pos";
        else if (m.frob < CUT_ROT_LIMIT) why = "rot";
    }
    /* The two ticks came from different simulation regimes (gameplay to level
     * transition, transition to load, cutscene in or out). The camera re-targets
     * on such an edge by less than either threshold above, and a lerp presents
     * the chord of that swing; the regimes are not one motion, so the pair is a
     * cut by definition. */
    if (pgs && cgs) {
        memcpy(&m.pgs, pgs, 4);
        memcpy(&m.cgs, cgs, 4);
        if (m.pgs != m.cgs) { m.gs_edge = 1; if (!why) why = "gs-edge"; }
    }
    if (out) *out = m;
    return why;
}

/** @brief The distance bound cut_pos_jump applies: OPENPETE_MOBY_JUMP_BOUND when set (read once), else game_layout.moby.jump_bound.
 *
 * The variable is a same-binary A/B of the bound; 0 disables it.
 * @return The bound in world units. */
static double cut_jump_bound(void) {
    static long s = -2;
    if (s == -2) { const char* e = getenv("OPENPETE_MOBY_JUMP_BOUND"); s = (e && *e) ? strtol(e, NULL, 0) : -1; }
    return s >= 0 ? (double)s : (double)game_layout.moby.jump_bound;
}

/** @brief Whether the placement markers apply: OPENPETE_MOBY_JUMP_MARKERS, default on, read once.
 *
 * 0 leaves the cutscene sampler's rule and the distance bound alone, for a
 * same-binary A/B of the markers.
 * @return Nonzero when the markers apply. */
static int cut_jump_markers_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_JUMP_MARKERS"); s = (e && *e) ? (*e != '0') : 1; }
    return s;
}

/** @brief Whether a moby step matches a row of the game's placement table.
 *
 * A row matches on tick N's class halfword, tick N's state byte, and tick
 * N-1's state byte unless the row takes any (0xFF).
 * @param prev_rec Tick N-1's moby record.
 * @param curr_rec Tick N's moby record.
 * @return 1 when a row matches, else 0. */
static int cut_place_marked(const uint8_t* prev_rec, const uint8_t* curr_rec) {
    const game_moby_place_t* t = game_layout.moby.place;
    if (!t) return 0;
    uint16_t cls;
    memcpy(&cls, curr_rec + game_layout.moby.class_off, 2);
    const uint8_t sp = prev_rec[game_layout.moby.state_off];
    const uint8_t sc = curr_rec[game_layout.moby.state_off];
    for (uint32_t i = 0; i < game_layout.moby.place_n; i++)
        if (t[i].cls == cls && t[i].state_curr == sc &&
            (t[i].state_prev == 0xFFu || t[i].state_prev == sp)) return 1;
    return 0;
}

/** @brief One interp ring pair's snapshots: the newest pair (prev, curr), or the pair before it (prev2, prev).
 *
 * @param older 0 for the newest pair, 1 for the pair before it.
 * @param a Receives the earlier snapshot.
 * @param b Receives the later snapshot.
 * @return 1 when the ring holds that pair, else 0. */
static int cut_ring_pair(int older, const openpete_interp_snapshot_t** a,
                         const openpete_interp_snapshot_t** b) {
    double al = 0.0;
    const openpete_interp_snapshot_t *p2 = NULL, *pv = NULL, *cu = NULL;
    if (!openpete_interp_query3(openpete_interp_curr_tick_ns(), g_openpete_tick_period_ns,
                                &al, &p2, &pv, &cu) || !pv || !cu) return 0;
    if (older) { if (!p2) return 0; cu = pv; pv = p2; }
    *a = pv; *b = cu;
    return 1;
}

/** @brief Read the 4-byte word of ring row `row` at byte `off` from one snapshot.
 *
 * @param s The snapshot.
 * @param row The row index.
 * @param off Byte offset within the row.
 * @return The word. */
static uint32_t cut_snap_word(const openpete_interp_snapshot_t* s, int row, uint32_t off) {
    uint32_t v;
    memcpy(&v, s->data + s->region_offsets[row] + off, 4);
    return v;
}

/** @brief Whether a regime marker (game_layout.moby.regime) holds on one interp ring pair.
 *
 * A GAME_REGIME_EITHER row tests both snapshots of the pair; a
 * GAME_REGIME_BEFORE row tests the snapshot before the pair's earlier one,
 * which the ring holds only for its newest pair.
 * @param older 0 tests the newest pair (prev, curr); 1 tests the pair before
 *        it (prev2, prev).
 * @return 1 when a row's value range holds, else 0 (also when the game names
 *         none, a row is unregistered, or the ring has no such pair). */
static int cut_regime(int older) {
    const game_moby_regime_t* t = game_layout.moby.regime;
    if (!t) return 0;
    double al = 0.0;
    const openpete_interp_snapshot_t *p2 = NULL, *pv = NULL, *cu = NULL;
    if (!openpete_interp_query3(openpete_interp_curr_tick_ns(), g_openpete_tick_period_ns,
                                &al, &p2, &pv, &cu) || !pv || !cu) return 0;
    if (older) { if (!p2) return 0; cu = pv; pv = p2; p2 = NULL; }
    for (uint32_t i = 0; i < game_layout.moby.regime_n; i++) {
        const int row = game_interp_row_index(t[i].row);
        const openpete_interp_snapshot_t* tested[2] = { pv, cu };
        if (t[i].snaps == GAME_REGIME_BEFORE) { tested[0] = p2; tested[1] = NULL; }
        for (int k = 0; row >= 0 && k < 2; k++) {
            if (!tested[k]) continue;
            const uint32_t v = cut_snap_word(tested[k], row, 0);
            if (v >= t[i].lo && v <= t[i].hi) return 1;
        }
    }
    return 0;
}

/** @brief Whether a director placement marker (game_layout.moby.director) names this moby on one interp ring pair.
 *
 * A row with a state row matches when that row goes state_prev -> state_curr
 * across the pair and the later snapshot's pointer word names `addr`; a row
 * with no state row (a pointer edge) matches when the pointer word names
 * `addr` in the later snapshot and not in the earlier one.
 * @param addr The moby's guest address.
 * @param older 0 tests the newest pair (prev, curr); 1 tests the pair before
 *        it (prev2, prev).
 * @return 1 when a row matches, else 0 (also when a row's ring rows are
 *         unregistered or the ring has no such pair). */
static int cut_director_placed(uint32_t addr, int older) {
    const game_moby_director_t* t = game_layout.moby.director;
    if (!t) return 0;
    const openpete_interp_snapshot_t *pv = NULL, *cu = NULL;
    if (!cut_ring_pair(older, &pv, &cu)) return 0;
    for (uint32_t i = 0; i < game_layout.moby.director_n; i++) {
        const int mrow = game_interp_row_index(t[i].moby_row);
        const int srow = t[i].state_row ? game_interp_row_index(t[i].state_row) : -1;
        if (mrow < 0 || (t[i].state_row && srow < 0)) continue;
        if (cut_snap_word(cu, mrow, t[i].moby_off) != addr) continue;
        if (srow < 0 ? cut_snap_word(pv, mrow, t[i].moby_off) != addr
                     : (cut_snap_word(pv, srow, 0) == t[i].state_prev &&
                        cut_snap_word(cu, srow, 0) == t[i].state_curr)) return 1;
    }
    return 0;
}

/** @brief Whether the resident overlay's name starts with `prefix`.
 *
 * @param prefix The prefix; NULL matches any overlay.
 * @return 1 on a match, else 0 (also when no overlay is resident). */
static int cut_overlay_is(const char* prefix) {
    if (!prefix) return 1;
    const spyro_overlay_entry_t* ov = spyro_overlay_find(spyro_get_active_overlay_magic());
    return ov && ov->name && strncmp(ov->name, prefix, strlen(prefix)) == 0;
}

/** @brief The guest address of a moby record's props block.
 *
 * @param rec The moby record.
 * @return The props pointer word (moby_game.props_off). */
static uint32_t cut_rec_props(const uint8_t* rec) {
    uint32_t p;
    memcpy(&p, rec + moby_game.props_off, 4);
    return p;
}

/** @brief The props bytes the capture takes for a moby of class `cls` in the resident overlay: through the last word a props or attach marker row reads.
 *
 * @param cls The moby's class halfword.
 * @return The byte count; 0 when no row names the class in this overlay. */
static uint32_t cut_props_span(uint16_t cls) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < game_layout.moby.props_mark_n; i++) {
        const game_moby_props_t* r = &game_layout.moby.props_mark[i];
        if (r->cls != cls || !cut_overlay_is(r->overlay)) continue;
        const uint32_t e = (uint32_t)(r->word_off > r->base_off ? r->word_off : r->base_off) + 4u;
        if (e > n) n = e;
    }
    for (uint32_t i = 0; i < game_layout.moby.attach_n; i++) {
        const game_moby_attach_t* r = &game_layout.moby.attach[i];
        if (r->cls == cls && cut_overlay_is(r->overlay) && r->parent_off + 4u > n)
            n = r->parent_off + 4u;
    }
    return n;
}

void cut_props_capture(const uint8_t* rec) {
    if (!game_layout.moby.props_mark_n && !game_layout.moby.attach_n) return;
    uint16_t cls;
    memcpy(&cls, rec + game_layout.moby.class_off, 2);
    const uint32_t n = cut_props_span(cls);
    const uint32_t props = n ? cut_rec_props(rec) : 0u;
    if (props < 0x80000000u) return;
    const void* host = openpete_psx_to_host_span(props, n);
    if (host) openpete_interp_objs_capture(props, host, n);
}

/** @brief An object's step that ends on the tick stamp `t`: (prev, curr) when its curr rolled at `t`, (prev2, prev) when its prev did.
 *
 * The store keeps the last distinct state and evicts an object not seen on a
 * capture, so a paired object's earlier blob is its state on the tick
 * before `t`.
 * @param o The object.
 * @param t The roll stamp the step ends on.
 * @param a Receives the earlier blob.
 * @param b Receives the later blob.
 * @return 1 when the object holds such a step, else 0. */
static int cut_obj_step_at(const openpete_interp_obj_t* o, op_time_t t,
                           const uint8_t** a, const uint8_t** b) {
    if (!o || !o->have_prev) return 0;
    if (o->curr_ns == t) { *a = o->prev; *b = o->curr; return 1; }
    if (o->prev_ns == t && o->have_prev2) { *a = o->prev2; *b = o->prev; return 1; }
    return 0;
}

/** @brief The roll stamp a moby's step ends on: its curr roll for the newest pair, its prev roll for the pair before it.
 *
 * @param addr The moby's guest address.
 * @param older 0 for the newest pair, 1 for the pair before it.
 * @param t Receives the stamp.
 * @return 1 when the moby is tracked with that pair, else 0. */
static int cut_step_stamp(uint32_t addr, int older, op_time_t* t) {
    const openpete_interp_obj_t* mo = openpete_interp_objs_find(addr);
    if (!mo || !mo->have_prev || (older && !mo->have_prev2)) return 0;
    *t = older ? mo->prev_ns : mo->curr_ns;
    return 1;
}

/** @brief One moby step's props-block pair: the props object's step ending on the tick the moby's step does.
 *
 * The props object is keyed by the props address the later record names.
 * @param addr The moby's guest address.
 * @param curr_rec The later moby record.
 * @param older 0 for the moby's newest pair, 1 for the pair before it.
 * @param a Receives the earlier props blob.
 * @param b Receives the later props blob.
 * @return The blob length, or 0 when the props block did not change on that tick. */
static uint32_t cut_props_pair(uint32_t addr, const uint8_t* curr_rec, int older,
                               const uint8_t** a, const uint8_t** b) {
    op_time_t t;
    const openpete_interp_obj_t* po = openpete_interp_objs_find(cut_rec_props(curr_rec));
    if (!cut_step_stamp(addr, older, &t) || !cut_obj_step_at(po, t, a, b)) return 0;
    return po->blob_len;
}

/** @brief Read a 4-byte word at `off` from a props blob of `len` bytes.
 *
 * @param blob The blob.
 * @param len Its length.
 * @param off Byte offset.
 * @param v Receives the word.
 * @return 1 when the word lies inside the blob, else 0. */
static int cut_blob_word(const uint8_t* blob, uint32_t len, uint32_t off, uint32_t* v) {
    if (off + 4u > len) return 0;
    memcpy(v, blob + off, 4);
    return 1;
}

/** @brief Whether a props-word marker (game_layout.moby.props_mark) marks a moby step as a placement.
 *
 * A row matches the later record's class in the resident overlay when its
 * props word goes 0 -> nonzero on the step's tick and, with a base word,
 * the earlier record's position component equals that word.
 * @param addr The moby's guest address.
 * @param prev_rec The earlier record.
 * @param curr_rec The later record.
 * @param older 0 for the moby's newest pair, 1 for the pair before it.
 * @return 1 when a row matches, else 0. */
static int cut_props_marked(uint32_t addr, const uint8_t* prev_rec, const uint8_t* curr_rec,
                            int older) {
    if (!game_layout.moby.props_mark_n) return 0;
    uint16_t cls;
    memcpy(&cls, curr_rec + game_layout.moby.class_off, 2);
    const uint8_t *a = NULL, *b = NULL;
    uint32_t len = 0;
    for (uint32_t i = 0; i < game_layout.moby.props_mark_n; i++) {
        const game_moby_props_t* r = &game_layout.moby.props_mark[i];
        if (r->cls != cls || !cut_overlay_is(r->overlay)) continue;
        if (!len && !(len = cut_props_pair(addr, curr_rec, older, &a, &b))) return 0;
        uint32_t wa, wb, base;
        if (!cut_blob_word(a, len, r->word_off, &wa) || !cut_blob_word(b, len, r->word_off, &wb) ||
            wa != 0u || wb == 0u) continue;
        int32_t pos;
        memcpy(&pos, prev_rec + game_layout.moby.pos_off + 4u * r->base_axis, 4);
        if (!r->base_off ||
            (cut_blob_word(a, len, r->base_off, &base) && (uint32_t)pos == base)) return 1;
    }
    return 0;
}

static uint32_t cut_moby_array_head(void);
static int cut_obj_jump_at(uint32_t addr, const uint8_t* prev_rec, const uint8_t* curr_rec,
                           int older, int depth, const char** why);

/** @brief Whether an attachment marker (game_layout.moby.attach) carries a jump of the moby's parent to it.
 *
 * The parent is the moby-array record whose index the props word names; its
 * step must lie on the same tick as the moby's, and its own verdict decides.
 * @param addr The moby's guest address.
 * @param curr_rec The later record.
 * @param older 0 for the moby's newest pair, 1 for the pair before it.
 * @param depth Attachment levels already followed; the chain stops at 4.
 * @return 1 when the parent's step is a jump, else 0. */
static int cut_attach_jump(uint32_t addr, const uint8_t* curr_rec, int older, int depth) {
    if (!game_layout.moby.attach_n || depth >= 4) return 0;
    uint16_t cls;
    memcpy(&cls, curr_rec + game_layout.moby.class_off, 2);
    for (uint32_t i = 0; i < game_layout.moby.attach_n; i++) {
        const game_moby_attach_t* r = &game_layout.moby.attach[i];
        if (r->cls != cls || !cut_overlay_is(r->overlay)) continue;
        const openpete_interp_obj_t* po = openpete_interp_objs_find(cut_rec_props(curr_rec));
        const uint32_t arr = cut_moby_array_head();
        uint32_t idx;
        op_time_t t;
        if (!po || !arr || !cut_blob_word(po->curr, po->blob_len, r->parent_off, &idx) ||
            idx >= 4096u || !cut_step_stamp(addr, older, &t)) continue;
        const uint32_t paddr = arr + idx * game_layout.moby.array_stride;
        const uint8_t *p0 = NULL, *p1 = NULL;
        if (paddr == addr || !cut_obj_step_at(openpete_interp_objs_find(paddr), t, &p0, &p1)) continue;
        if (memcmp(p0 + game_layout.moby.class_off, p1 + game_layout.moby.class_off, 2) != 0 ||
            cut_obj_jump_at(paddr, p0, p1, older, depth + 1, NULL)) return 1;
    }
    return 0;
}

/** @brief Whether a per-moby game marker (placement table, director, props word or attachment) marks one step as a placement.
 *
 * @param addr The moby's guest address.
 * @param prev_rec The earlier record.
 * @param curr_rec The later record.
 * @param older 0 for the newest pair, 1 for the pair before it.
 * @param depth Attachment levels already followed.
 * @param why Receives the matching marker's name.
 * @return 1 when a marker matches, else 0. */
static int cut_moby_marked(uint32_t addr, const uint8_t* prev_rec, const uint8_t* curr_rec,
                           int older, int depth, const char** why) {
    if (cut_place_marked(prev_rec, curr_rec)) { *why = "table"; return 1; }
    if (cut_director_placed(addr, older)) { *why = "director"; return 1; }
    if (cut_props_marked(addr, prev_rec, curr_rec, older)) { *why = "props"; return 1; }
    if (cut_attach_jump(addr, curr_rec, older, depth)) { *why = "attach"; return 1; }
    return 0;
}

int cut_pos_jump(const int32_t* prev_pos, const int32_t* curr_pos) {
    const double lim = cut_jump_bound();
    if (lim <= 0.0) return 0;
    const double dx = (double)curr_pos[0] - prev_pos[0];
    const double dy = (double)curr_pos[1] - prev_pos[1];
    const double dz = (double)curr_pos[2] - prev_pos[2];
    return dx*dx + dy*dy + dz*dz > lim * lim;
}

/** @brief The jump verdict of one moby step between two consecutive ring snapshots, and the rule that decided (cut_obj_jump_reason).
 *
 * @param addr The moby's guest address.
 * @param prev_rec The earlier record.
 * @param curr_rec The later record.
 * @param older 0 when the records are the ring's newest pair, 1 when they are
 *        the pair before it; selects the snapshots the ring-row rules read.
 * @param depth Attachment levels already followed (cut_attach_jump).
 * @param why Receives the deciding rule's name when non-NULL.
 * @return 1 for a jump, else 0. */
static int cut_obj_jump_at(uint32_t addr, const uint8_t* prev_rec, const uint8_t* curr_rec,
                           int older, int depth, const char** why) {
    const char* w = "none";
    int v = moby_game.cutpath_jump ? moby_game.cutpath_jump(addr, prev_rec, curr_rec) : -1;
    if (v >= 0) {
        w = "cutpath";
    } else if (cut_jump_markers_on() && cut_moby_marked(addr, prev_rec, curr_rec, older, depth, &w)) {
        v = 1;
    } else if (cut_jump_markers_on() && cut_regime(older)) {
        v = 1; w = "regime";
    } else {
        int32_t pp[3], cp[3];
        memcpy(pp, prev_rec + game_layout.moby.pos_off, 12);
        memcpy(cp, curr_rec + game_layout.moby.pos_off, 12);
        v = cut_pos_jump(pp, cp);
        w = v ? "bound" : "none";
    }
    if (why) *why = w;
    return v;
}

int cut_obj_jump_reason(uint32_t addr, const uint8_t* prev_rec, const uint8_t* curr_rec,
                        const char** why) {
    return cut_obj_jump_at(addr, prev_rec, curr_rec, 0, 0, why);
}

int cut_obj_jump(uint32_t addr, const uint8_t* prev_rec, const uint8_t* curr_rec) {
    return cut_obj_jump_reason(addr, prev_rec, curr_rec, NULL);
}

int cut_obj_prev_jump(uint32_t addr, const uint8_t* prev2_rec, const uint8_t* prev_rec) {
    uint16_t c2, c1;
    memcpy(&c2, prev2_rec + game_layout.moby.class_off, 2);
    memcpy(&c1, prev_rec + game_layout.moby.class_off, 2);
    return c2 != c1 || cut_obj_jump_at(addr, prev2_rec, prev_rec, 1, 0, NULL);
}

void cut_jump_census_capture(uint32_t key) {
    static long s_min = -2;
    if (s_min == -2) { const char* e = getenv("OPENPETE_JUMP_CENSUS"); s_min = (e && *e) ? strtol(e, NULL, 0) : -1; }
    if (s_min < 0) return;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(key);
    if (!o || !o->have_prev || o->curr_ns != openpete_interp_tick_stamp()) return;
    int32_t pp[3], cp[3];
    memcpy(pp, o->prev + game_layout.moby.pos_off, 12);
    memcpy(cp, o->curr + game_layout.moby.pos_off, 12);
    const double dx = (double)cp[0] - pp[0], dy = (double)cp[1] - pp[1], dz = (double)cp[2] - pp[2];
    const double d = sqrt(dx*dx + dy*dy + dz*dz);
    if (d < (double)s_min) return;
    uint16_t pc, cc;
    memcpy(&pc, o->prev + game_layout.moby.class_off, 2);
    memcpy(&cc, o->curr + game_layout.moby.class_off, 2);
    const char* why = "class";
    const int v = (pc != cc) ? 1 : cut_obj_jump_reason(key, o->prev, o->curr, &why);
    /* Each rule's own verdict, so one run scores every combination of the
     * markers and the bound. */
    const int cutpath = moby_game.cutpath_jump ? moby_game.cutpath_jump(key, o->prev, o->curr) : -1;
    const char* mw = NULL;
    const int marked = cut_moby_marked(key, o->prev, o->curr, 0, 0, &mw);
    fprintf(stderr, "[jump-census vbl=%u] obj=%08x cls=%03x st=%02x->%02x d=%.1f "
                    "cutpath=%d table=%d load=%d bound=%.0f verdict=%s why=%s\n",
            openpete_feed_key_now(), key, (unsigned)cc, o->prev[game_layout.moby.state_off],
            o->curr[game_layout.moby.state_off], d, cutpath,
            marked, cut_regime(0), cut_jump_bound(),
            v ? "jump" : "lerp", why);
}

/** @brief Whether the held-cut rule applies: OPENPETE_CUT_HOLD_ALL, default on, read once.
 *
 * 0 leaves a held camera cut's records on their own clocks, for a same-binary
 * A/B.
 * @return Nonzero when the rule applies. */
static int cut_hold_all_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_CUT_HOLD_ALL"); s = (e && *e) ? (*e != '0') : 1; }
    return s;
}

int cut_held_rolled(const openpete_interp_obj_t* o) {
    if (!s_held_ns || !o || !o->have_prev) return -1;
    return o->curr_ns + g_openpete_tick_period_ns / 2 > s_held_ns ? 1 : 0;
}

/* The present's per-object verdicts, in decision order. */
#define CUT_OBJ_MAX 256
static struct { uint32_t addr; int held; } s_obj[CUT_OBJ_MAX];
static int s_obj_n;
static int s_obj_open;
/* The present's camera cut showed tick N (cut_set_camera_snap). */
static int s_cam_snap;

void cut_obj_present_begin(void) { s_obj_n = 0; s_obj_open = 1; }
void cut_obj_present_end(void)   { s_obj_n = 0; s_obj_open = 0; s_held_ns = 0; }
void cut_set_camera_snap(int snap) { s_cam_snap = snap ? 1 : 0; }

int cut_obj_held(uint32_t addr) {
    if (!s_obj_open) return -1;
    for (int i = 0; i < s_obj_n; i++)
        if (s_obj[i].addr == addr) return s_obj[i].held;
    return -1;
}

/** @brief Note one object verdict in the census and record it in the present's set.
 *
 * @param kind CUT_CLASS or CUT_TELEPORT.
 * @param addr The object's guest address.
 * @param held 1 for a hold, 0 for a snap.
 * @param why The verdict's reason.
 * @param pp Tick N-1's position.
 * @param cp Tick N's position.
 * @return held. */
static int cut_obj_decide(int kind, uint32_t addr, int held, const char* why,
                          const int32_t* pp, const int32_t* cp) {
    cut_census_note(kind, addr, held, why, pp, held ? pp : cp);
    if (s_obj_open && s_obj_n < CUT_OBJ_MAX) {
        s_obj[s_obj_n].addr = addr;
        s_obj[s_obj_n].held = held;
        s_obj_n++;
    }
    return held;
}

int cut_obj_hold(int kind, uint32_t addr, const openpete_interp_obj_t* o,
                 uint32_t pos_off, double alpha,
                 int (*stage)(uint32_t addr, const openpete_interp_obj_t* o)) {
    int32_t pp[3], cp[3];
    memcpy(pp, o->prev + pos_off, 12);
    memcpy(cp, o->curr + pos_off, 12);
    const char* why = NULL;
    if (!cut_pair_is_tick(o))          why = "stale";
    else if (alpha >= 1.0)             why = "alpha1";
    else if (s_cam_snap)               why = "camsnap";
    else if (stage && !stage(addr, o)) why = "unstaged";
    return cut_obj_decide(kind, addr, why == NULL, why ? why : "hold", pp, cp);
}

int cut_pair_is_tick(const openpete_interp_obj_t* o) {
    if (!o || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    /* The capture rolls only on a change, so prev_ns is when prev was first
     * captured; prev_seen_ns is the last capture that still observed it. */
    const op_time_t prev_at = o->prev_seen_ns > o->prev_ns ? o->prev_seen_ns : o->prev_ns;
    return o->curr_ns > prev_at &&
           (double)(o->curr_ns - prev_at) <= 1.5 * (double)g_openpete_tick_period_ns;
}

/** @brief Whether the canonical tick drew this moby record.
 *
 * The cull writes its verdict into the record at game_layout.moby.drawn_off;
 * a game that names no such field counts every record as drawn.
 * @param mh Host pointer to the moby record.
 * @return Nonzero when the record was drawn. */
static int cut_rec_drawn(const uint8_t* mh) {
    const uint32_t off = game_layout.moby.drawn_off;
    return off ? (mh[off] != 0) : 1;
}

/** @brief Whether one moby record can show tick N-1, for cut_scene_holdable.
 *
 * The record is looked up in the interp table by moby_game.interp_key (the
 * guest address when the game names none). It refuses when it has no entry or
 * no previous state, or when its pair is not one tick (cut_pair_is_tick); a
 * record whose class field differs between prev and curr passes. With the
 * census armed, every refusal prints the record, and an accepted pair whose
 * roll stamps are more than 1.5 ticks apart prints as a seen pair.
 * @param m Guest address of the record.
 * @param mh Host pointer to the record.
 * @return 1 when the record can hold, 0 when it refuses. */
static int cut_rec_holdable(uint32_t m, const uint8_t* mh) {
    const uint32_t key = moby_game.interp_key ? moby_game.interp_key(m) : m;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(key);
    if (!o || !o->have_prev) {
        if (cut_census_on()) {
            uint16_t cls = 0;
            if (game_layout.moby.class_off) memcpy(&cls, mh + game_layout.moby.class_off, 2);
            fprintf(stderr, "[cut-census vbl=%u] scene=noprev rec=%08x key=%08x cls=%03x obj=%d\n",
                    openpete_frame, m, key, (unsigned)cls, o != NULL);
        }
        return 0;
    }
    if (game_layout.moby.class_off) {
        uint16_t pc, cc;
        memcpy(&pc, o->prev + game_layout.moby.class_off, 2);
        memcpy(&cc, o->curr + game_layout.moby.class_off, 2);
        if (pc != cc) return 1;
    }
    if (cut_pair_is_tick(o)) {
        /* Engagement of the seen stamp: a pair whose roll stamps are further
         * apart than a tick, accepted because prev was observed up to curr. */
        if (cut_census_on() &&
            (double)(o->curr_ns - o->prev_ns) > 1.5 * (double)g_openpete_tick_period_ns)
            fprintf(stderr, "[cut-census vbl=%u] scene=seenpair rec=%08x prev_ns=%lld "
                            "prev_seen_ns=%lld curr_ns=%lld\n",
                    openpete_frame, m, (long long)o->prev_ns,
                    (long long)o->prev_seen_ns, (long long)o->curr_ns);
        return 1;
    }
    if (cut_census_on())
        fprintf(stderr, "[cut-census vbl=%u] scene=stalepair rec=%08x key=%08x "
                        "prev_ns=%lld prev_seen_ns=%lld curr_ns=%lld\n",
                openpete_frame, m, key, (long long)o->prev_ns,
                (long long)o->prev_seen_ns, (long long)o->curr_ns);
    return 0;
}

/** @brief The guest address of the moby array's first element.
 *
 * game_layout.moby.array_head is the array itself when array_direct is set,
 * else the address of the pointer variable that holds it, which is read.
 * @return The address, or 0 when it is below 0x80000000 (the game names no
 *         array, or the pointer is unset). */
static uint32_t cut_moby_array_head(void) {
    uint32_t arr = game_layout.moby.array_head;
    if (!game_layout.moby.array_direct) {
        const void* hp = openpete_psx_to_host_span(arr, 4u);
        arr = 0;
        if (hp) memcpy(&arr, hp, 4);
    }
    return arr >= 0x80000000u ? arr : 0;
}

/** @brief Count the drawn live records of the moby array that cannot show tick N-1.
 *
 * Walks the array from its head up to the state sentinel (at most 4096
 * records), skipping dormant records (state 0x80 and above) and records the
 * cull did not draw; a record refuses when cut_rec_holdable rejects it. The
 * walk stops at the first refusal unless the census is armed, which counts
 * them all.
 * @return The number of refusing records; 0 when the game names no array. */
static int cut_array_refusals(void) {
    const uint32_t stride = game_layout.moby.array_stride;
    const uint32_t soff   = game_layout.moby.state_off;
    const int all = cut_census_on();
    int n = 0;
    uint32_t m = stride ? cut_moby_array_head() : 0;
    for (int g = 0; m && g < 4096 && (all || n == 0); g++, m += stride) {
        const uint8_t* mh = (const uint8_t*)openpete_psx_to_host_span(m, stride);
        if (!mh || mh[soff] == (uint8_t)game_layout.moby.state_term) break;
        if (mh[soff] >= 0x80) continue;          /* dormant: not drawn */
        if (cut_rec_drawn(mh) && !cut_rec_holdable(m, mh)) n++;
    }
    return n;
}

/** @brief Count the records the canonical render drew from outside the moby array (a relocated actor) that cannot show tick N-1.
 *
 * The replay draws these too, and the cull drew each by definition, so no
 * drawn test applies; dormant records are skipped. Counts refusals as
 * cut_array_refusals does.
 * @return The number of refusing records. */
static int cut_drawn_set_refusals(void) {
    const uint32_t soff = game_layout.moby.state_off;
    const uint32_t* drawn = NULL;
    const int all = cut_census_on();
    const uint32_t rec_bytes = game_layout.moby.array_stride ? game_layout.moby.array_stride : soff + 1u;
    const int cnt = moby_native_canon_drawn_mobys(&drawn);
    int n = 0;
    for (int i = 0; drawn && i < cnt && (all || n == 0); i++) {
        const uint8_t* mh = (const uint8_t*)openpete_psx_to_host_span(drawn[i], rec_bytes);
        if (!mh || mh[soff] >= 0x80) continue;
        if (!cut_rec_holdable(drawn[i], mh)) n++;
    }
    return n;
}

/** @brief Count the records of the previous epoch's queues, which a held cut replays, that cannot show tick N-1.
 *
 * Dormant records are skipped. A record whose object has not rolled since the
 * current queue's cull stamp shows its snapshot curr, which is the state the
 * previous cull drew, so it holds without a pair; every other record is
 * judged by cut_rec_holdable. Counts refusals as cut_array_refusals does.
 * @return The number of refusing records; 0 when the previous queue is not
 *         usable (the hold then replays the current queue, which the other
 *         two counts cover). */
static int cut_prev_queue_refusals(void) {
    static uint32_t mobys[1024];
    const uint32_t soff = game_layout.moby.state_off;
    const uint32_t rec_bytes = game_layout.moby.array_stride ? game_layout.moby.array_stride : soff + 1u;
    const int all = cut_census_on();
    op_time_t ref_ns = 0;
    const int cnt = moby_native_prev_queue_mobys(mobys, 1024, &ref_ns);
    int n = 0;
    for (int i = 0; i < cnt && (all || n == 0); i++) {
        const uint8_t* mh = (const uint8_t*)openpete_psx_to_host_span(mobys[i], rec_bytes);
        if (!mh || mh[soff] >= 0x80) continue;
        const uint32_t key = moby_game.interp_key ? moby_game.interp_key(mobys[i]) : mobys[i];
        const openpete_interp_obj_t* o = openpete_interp_objs_find(key);
        if (o && o->curr_ns <= ref_ns) continue;
        if (!cut_rec_holdable(mobys[i], mh)) n++;
    }
    return n;
}

int cut_scene_holdable(void) {
    if (!cut_census_on())
        return cut_array_refusals() == 0 && cut_drawn_set_refusals() == 0 &&
               cut_prev_queue_refusals() == 0;
    const int na = cut_array_refusals(), nd = cut_drawn_set_refusals(), np = cut_prev_queue_refusals();
    fprintf(stderr, "[cut-census vbl=%u] scene refusals array=%d drawn=%d prevq=%d\n",
            openpete_frame, na, nd, np);
    return na == 0 && nd == 0 && np == 0;
}

int cut_camera_hold(double period_ns, op_time_t curr_tick_ns, int gs_edge) {
    double a = (period_ns > 0.0)
             ? (double)(openpete_interp_present_now() - curr_tick_ns) / period_ns : 1.0;
    if (openpete_interp_curr_is_stale()) a = 1.0;
    const char* why = (a < 1.0) ? (cut_scene_holdable() ? "hold" : "scene") : "stale";
    const int hold = (why[0] == 'h');
    s_held_ns = (hold && cut_hold_all_on()) ? curr_tick_ns : 0;
    cut_census_note(CUT_CAM, gs_edge ? 1u : 0u, hold, why, NULL, NULL);
    return hold;
}

/** @brief A snapshot's pointer to one registered region's bytes.
 *
 * @param s The snapshot; NULL yields NULL.
 * @param region The region index; negative (the game registered no such row)
 *        yields NULL.
 * @return Pointer into s->data at the region's offset, or NULL. */
static const uint8_t* snap_region(const openpete_interp_snapshot_t* s, int region) {
    return (s && region >= 0) ? s->data + s->region_offsets[region] : NULL;
}

/** @brief The interval length a camera pair interpolates over.
 *
 * The measured gap between the two snapshots' tick stamps, replaced by one
 * nominal tick period when it exceeds CUT_PAIR_STALE_TICKS periods: the gap
 * balloons to K ticks when the camera sits byte-static for K ticks and then
 * resumes, which would make the resume crawl at 1/K speed for one interval.
 * A missing snapshot or a non-increasing stamp also yields the nominal period.
 * @param prev The earlier snapshot.
 * @param curr The later snapshot.
 * @return The period in nanoseconds. */
static double cut_period_ns(const openpete_interp_snapshot_t* prev,
                            const openpete_interp_snapshot_t* curr) {
    const double nominal = (double)g_openpete_tick_period_ns;
    if (!prev || !curr || curr->tick_ns <= prev->tick_ns) return nominal;
    const double p = (double)(curr->tick_ns - prev->tick_ns);
    return (p > CUT_PAIR_STALE_TICKS * nominal) ? nominal : p;
}

void cut_locate_camera(const cut_cam_desc_t* desc) {
    if (!desc) return;
    double qa = 0.0;
    const openpete_interp_snapshot_t *p2 = NULL, *pv = NULL, *cu = NULL;
    if (!openpete_interp_query3(openpete_interp_present_now(),
                                g_openpete_tick_period_ns, &qa, &p2, &pv, &cu))
        return;
    const uint8_t* pcam = snap_region(pv, desc->cam_region);
    const uint8_t* ccam = snap_region(cu, desc->cam_region);
    if (!pcam || !ccam) return;
    cut_cam_metrics_t m;
    const char* why = cut_camera_reason(pcam, ccam, desc->pos_off, desc->mat_off,
                                        snap_region(pv, desc->gs_region),
                                        snap_region(cu, desc->gs_region), &m);
    if (!why) return;
    const int32_t* pp = (const int32_t*)(pcam + desc->pos_off);
    const int32_t* cp = (const int32_t*)(ccam + desc->pos_off);
    fprintf(stderr, "[cut-census vbl=%u] locate=camcut why=%s |d|=%.0f frob=%.2f "
                    "gs=%u->%u ptick=%u ctick=%u prev=(%d,%d,%d) curr=(%d,%d,%d)\n",
            openpete_frame, why, sqrt(m.d2), m.frob / CUT_ROT_LIMIT,
            m.pgs, m.cgs, pv->tick, cu->tick,
            pp[0], pp[1], pp[2], cp[0], cp[1], cp[2]);
}

void cam_bracket_run(uint8_t* cam, const openpete_interp_snapshot_t* prev,
                     const openpete_interp_snapshot_t* curr,
                     const cut_cam_desc_t* desc, uint32_t view_off,
                     cam_bracket_t* out) {
    cam_bracket_t r = { 0, 0, 0, 0.0, NULL, { 0.0, 0.0, 0.0 } };
    s_cam_snap = 0;
    s_held_ns = 0;
    const uint8_t* pcam = snap_region(prev, desc->cam_region);
    const uint8_t* ccam = snap_region(curr, desc->cam_region);
    if (!cam || !pcam || !ccam) { if (out) *out = r; return; }

    cut_cam_metrics_t m;
    r.cut = cut_camera_reason(pcam, ccam, desc->pos_off, desc->mat_off,
                              snap_region(prev, desc->gs_region),
                              snap_region(curr, desc->gs_region), &m) != NULL;
    const double period = cut_period_ns(prev, curr);
    /* A cut the ring is still replaying long after the fact has no open
     * interval: it shows the head, as it did before the hold existed. */
    const double pair_age = (double)((int64_t)openpete_interp_present_now()
                                     - (int64_t)curr->tick_ns)
                          / (double)g_openpete_tick_period_ns;
    r.hold = r.cut && pair_age <= CUT_PAIR_STALE_TICKS &&
             cut_camera_hold(period, curr->tick_ns, m.gs_edge);
    if (r.cut && !r.hold) { s_cam_snap = 1; if (out) *out = r; return; }

    r.alpha = (double)(openpete_interp_present_now() - curr->tick_ns) / period;
    /* The registered regions stopped changing longer ago than the pair's own
     * cadence: prev to curr describes a motion that has ended, and the head is
     * the state this present shows. */
    if (openpete_interp_curr_is_stale()) r.alpha = 1.0;
    /* Slightly negative rather than clamped at 0: the canonical present fires a
     * few ms before its tick's grid stamp, and clamping delivers that fraction
     * of all motion as tick-rate jumps. */
    if (r.alpha < -0.35) r.alpha = -0.35; else if (r.alpha > 1.0) r.alpha = 1.0;
    /* A held cut shows prev for the whole interval; a negative alpha would
     * extrapolate backwards through the cut, which is the motion that does not
     * exist. */
    if (r.hold) r.alpha = 0.0;

    lerp_matrix(cam, pcam, ccam, desc->mat_off, r.alpha);
    lerp_matrix(cam, pcam, ccam, view_off,      r.alpha);
    lerp_vec3  (cam, pcam, ccam, desc->pos_off, r.alpha);
    const int32_t* pp = (const int32_t*)(pcam + desc->pos_off);
    const int32_t* cp = (const int32_t*)(ccam + desc->pos_off);
    for (int i = 0; i < 3; i++)
        r.fpos[i] = (double)pp[i] + ((double)cp[i] - (double)pp[i]) * r.alpha;
    r.tickcam   = ccam;
    r.bracketed = 1;
    openpete_pace_note_alpha(r.alpha, 1);
    if (out) *out = r;
}
