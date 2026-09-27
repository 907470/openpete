/* games/common/renderer/cut_guard.c — see cut_guard.h. */
#include "renderer/cut_guard.h"

#include "renderer/interp_lerp.h"
#include "renderer/interp_regions_gen.h"
#include "renderer/moby.h"          /* moby_native_canon_drawn_mobys */
#include "renderer/moby_game.h"     /* moby_game.interp_key */
#include "openpete_interp_objs.h"   /* g_openpete_tick_period_ns */
#include "openpete_psx.h"           /* openpete_psx_to_host_span */
#include "game_layout.h"
#include "openpete_pace_log.h"
#include "openpete_fiber.h"         /* openpete_frame */

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

void cut_census_note(int kind, uint32_t addr, int held, const char* why,
                     const int32_t* prev_pos, const int32_t* shown_pos) {
    static const char* const names[CUT_KINDS] = { "camcut", "class", "teleport" };
    if (kind < 0 || kind >= CUT_KINDS) return;
    if (held) s_cut_hold[kind]++; else s_cut_snap[kind]++;
    if (!cut_census_on()) return;
    static const int32_t zero[3] = { 0, 0, 0 };
    const int32_t* pp = prev_pos ? prev_pos : zero;
    const int32_t* sp = shown_pos ? shown_pos : zero;
    double dx = (double)sp[0]-pp[0], dy = (double)sp[1]-pp[1], dz = (double)sp[2]-pp[2];
    fprintf(stderr, "[cut-census vbl=%u] kind=%s obj=%08x verdict=%s why=%s "
                    "prev=(%d,%d,%d) shown=(%d,%d,%d) D=%.3f "
                    "holds=%ld/%ld/%ld snaps=%ld/%ld/%ld\n",
            openpete_frame, names[kind], addr, held ? "hold" : "snap", why,
            pp[0], pp[1], pp[2], sp[0], sp[1], sp[2], sqrt(dx*dx + dy*dy + dz*dz),
            s_cut_hold[0], s_cut_hold[1], s_cut_hold[2],
            s_cut_snap[0], s_cut_snap[1], s_cut_snap[2]);
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
 * @return The address, or 0 when it is not a KSEG0 pointer (the game names no
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

int cut_scene_holdable(void) {
    if (!cut_census_on()) return cut_array_refusals() == 0 && cut_drawn_set_refusals() == 0;
    const int na = cut_array_refusals(), nd = cut_drawn_set_refusals();
    fprintf(stderr, "[cut-census vbl=%u] scene refusals array=%d drawn=%d\n",
            openpete_frame, na, nd);
    return na == 0 && nd == 0;
}

int cut_camera_hold(double period_ns, op_time_t curr_tick_ns, int gs_edge) {
    double a = (period_ns > 0.0)
             ? (double)(openpete_interp_present_now() - curr_tick_ns) / period_ns : 1.0;
    if (openpete_interp_curr_is_stale()) a = 1.0;
    const char* why = (a < 1.0) ? (cut_scene_holdable() ? "hold" : "scene") : "stale";
    const int hold = (why[0] == 'h');
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
    if (r.cut && !r.hold) { if (out) *out = r; return; }

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
