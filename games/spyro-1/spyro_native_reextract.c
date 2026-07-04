/* spyro_native_reextract.c — Spyro 1's sub-tick re-feed orchestrator.
 * See spyro_native_reextract.h for the contract. */

#include "spyro_native_reextract.h"
#include "openpete_interp.h"        /* snapshot ring query + op_now/OP_NS_PER_SEC */
#include "openpete_interp_objs.h"   /* per-object (moby) prev/curr table */
#include "openpete_native_render.h" /* register callback + scene API */
#include "openpete_psx.h"           /* openpete_psx_to_host_ptr */
#include "psx_runtime.h"            /* CPUState (read_word) */
#include "features/render_paths.h"  /* canon-drawn mask (load-window census) */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"

extern unsigned openpete_frame;

#define CAM_BYTES     0x110u
#define G_HUD_COUNT   12u
#define MOBY_STRIDE   0x58u
#define MOBY_STATE    0x48u
#define MOBY_POS      0x0Cu         /* Vector3D x,y,z (int32) */
#define SPYRO_REGION  1u            /* index in s_spyro_interp_regions (after g_Camera) */
#define SPYRO_SAVE    0x28u         /* g_Spyro bytes bracketed: pos(0x00) + per-seg anim(..0x26) */
#define FLAME_REGION  2u            /* index in s_spyro_interp_regions (after g_Spyro) */
#define FLAME_SAVE    0xD0u         /* flame bytes bracketed (covers pos + attach @ 0xB8..0xCC) */
#define FLAME_POS_OFF 0x00u         /* m_Position (origin, Vector3D int32) */
#define FLAME_CNT_OFF 0x20u         /* per-strand keyframe counter [8] (u8, += g_DeltaTime/tick) */
#define FLAME_ATT_OFF 0xB8u         /* flame-attach SHORTMATRIX (9 int16 + pad) */
#define SHADOW_REGION 3u            /* index in s_spyro_interp_regions (after g_SpyroFlame) */
#define SHADOW_SAVE   0x1Cu         /* shadow bytes bracketed (covers anchor @ 0x10..0x1C) */
#define SHADOW_POS_OFF 0x10u        /* m_Position (ground anchor, Vector3D int32) */

/* Per-moby RAM bracket save slots — restored after re-extract so the next sim
 * tick reads canonical RAM. Sized generously for any level's live moby count. */
#define MOBY_SAVE_MAX 1024
typedef struct { uint32_t addr; uint8_t bytes[MOBY_STRIDE]; } moby_save_t;

/* g_Particles pool (smoke / triangles / line particles): stride-0x20 array behind
 * a pointer; pos = X@lo16(+0x04), Y@hi16(+0x04), Z@lo16(+0x08) (int16, world/4).
 * The spine lerps each active particle's position by its per-object alpha (keyed
 * by slot address in the interp-objs table) so they animate at the present rate.
 * Only +0x04/+0x08 are bracketed (saved/restored). */
#define PART_STRIDE     0x20u
#define PART_HDR_SEL    8u           /* (hdr>>8)&0xFF: 0xFF=end, &0x80=inactive */
#define PART_POS_OFF    0x04u        /* X(lo16)/Y(hi16) word; Z(lo16) at +0x08 */
#define PART_SAVE_MAX   512
typedef struct { uint32_t addr, w1, w2; } part_save_t;  /* +0x04, +0x08 */

/* g_Camera field offsets (verified in camera.h / the feeders):
 *   +0x00 m_ProjectionMatrix (SHORTMATRIX: short[3][3]+pad)
 *   +0x14 m_ViewMatrix       (SHORTMATRIX)
 *   +0x28 m_Position         (Vector3D: int32 x,y,z) */
#define CAM_OFF_PROJ  0x00u
#define CAM_OFF_VIEW  0x14u
#define CAM_OFF_POS   0x28u

/* Nominal sim-tick period (29.97 Hz) — passed to the ring query for the
 * prev/curr pairing + dedupe verdict; the alpha is recomputed below from the
 * snapshots' measured timestamps, with the denominator capped at one tick
 * (interp_period_ns) so a stale prev after a static stretch can't stretch a
 * resume. */
#define TICK_PERIOD_NS  ((op_time_t)((OP_NS_PER_SEC * 1001) / 30000))

/* W0.6 — interp denominator, capped at ONE nominal tick. The measured
 * distinct-state gap (curr_ns - prev_ns) balloons to K ticks when an object (or
 * the camera) sits byte-static for K ticks and then RESUMES: the ring only
 * "rolls" on a change, so prev_ns stays pinned K ticks back. Dividing by that
 * gap makes the resume crawl at 1/K speed for one interval, then pop the
 * remaining (1-1/K) at the next roll — a hitchy resume on camera un-parks, an
 * enemy beginning to charge, a wait-then-move platform (invisible to the
 * oracle, which is determinism-only). Interp only ever spans one tick (1-tick
 * lag, no extrapolation), so cap it: a steady object (gap == 1 tick) is
 * unchanged; a resume sweeps the full delta over one tick. */
static double interp_period_ns(op_time_t prev_ns, op_time_t curr_ns) {
    op_time_t p = curr_ns - prev_ns;
    return (double)(p > TICK_PERIOD_NS ? TICK_PERIOD_NS : p);
}

static int16_t lerp16(int16_t a, int16_t b, double t) {
    double v = (double)a + ((double)b - (double)a) * t;
    return (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}
static int32_t lerp32(int32_t a, int32_t b, double t) {
    double v = (double)a + ((double)b - (double)a) * t;
    return (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

/* Lerp a 3x3 int16 rotation matrix (9 contiguous shorts; the 10th short is
 * pad and is left as curr). Endpoint matrices one tick apart differ by a
 * tiny rotation, so componentwise lerp (no renormalise) is sub-pixel — the
 * Q12-matrix-space interp the plan calls for at one-tick deltas. */
static void lerp_matrix(uint8_t* live, const uint8_t* prev, const uint8_t* curr,
                        uint32_t off, double t) {
    const int16_t* pp = (const int16_t*)(prev + off);
    const int16_t* pc = (const int16_t*)(curr + off);
    int16_t*       pl = (int16_t*)(live + off);
    for (int i = 0; i < 9; ++i) pl[i] = lerp16(pp[i], pc[i], t);
}
static void lerp_vec3(uint8_t* live, const uint8_t* prev, const uint8_t* curr,
                      uint32_t off, double t) {
    const int32_t* pp = (const int32_t*)(prev + off);
    const int32_t* pc = (const int32_t*)(curr + off);
    int32_t*       pl = (int32_t*)(live + off);
    for (int i = 0; i < 3; ++i) pl[i] = lerp32(pp[i], pc[i], t);
}

/* pred1 (predicted-tangent Hermite) on by default; OPENPETE_INTERP_LINEAR=1
 * forces the old linear lerp (A/B + regression escape hatch). */
static int hermite_enabled(void) {
    static int e = -1;
    if (e < 0) { const char* s = getenv("OPENPETE_INTERP_LINEAR"); e = (s && *s && *s != '0') ? 0 : 1; }
    return e;
}

/* ---- W0.3: bracket-integrity self-check --------------------------------------
 * The sub-tick present below writes lerped values INTO gameplay-readable PSX RAM
 * (the g_Camera / g_Spyro / flame / shadow spans, plus moby & particle pool
 * slots) and restores the canonical bytes before the game fiber resumes. That
 * restore discipline is exercised ONLY on sub-tick presents — which the headless
 * oracle never runs — so a missed or mis-guarded restore would silently perturb
 * gameplay state and surface only as an oracle drift days later. With
 * OPENPETE_BRACKET_CHECK set, hash the fixed DIRECT regions before any write and
 * re-check them after all restores; a mismatch means this present leaked into
 * gameplay RAM, logged on the same frame. Read-only, debug-gated, inert when
 * off. (Moby/particle pools are dynamic, so they're checked against their save
 * tables at the end of the present, next to their restore loops.) */
static int bracket_check_enabled(void) {
    static int e = -1;
    if (e < 0) { const char* s = getenv("OPENPETE_BRACKET_CHECK"); e = (s && *s && *s != '0'); }
    return e;
}
static uint64_t bc_hash(uint32_t psx_addr, uint32_t len) {
    const uint8_t* h = (const uint8_t*)openpete_psx_to_host_ptr(psx_addr);
    uint64_t x = 1469598103934665603ULL;   /* FNV-1a/64 */
    if (!h) return 0;
    for (uint32_t i = 0; i < len; ++i) { x ^= h[i]; x *= 1099511628211ULL; }
    return x;
}

/* ---- pred1: predicted-tangent cubic Hermite ----------------------------------
 * The linear lerp above is C0: velocity is constant within each 30 Hz bracket
 * and JUMPS at every tick boundary (= the 2nd difference of the sampled path).
 * That per-boundary velocity discontinuity is the perceived judder — measured
 * at ~5.5% of camera speed median, tail >100% on sharp turns, and it survives
 * any display rate (it's a 30 Hz artifact). pred1 interpolates [prev,curr] as a
 * cubic Hermite with a CENTERED start-tangent m0 = 0.5*(curr-prev2) and a
 * backward end-tangent m1 = (curr-prev). Same 1-tick latency as linear (no
 * future sample used), and it ~halves the boundary velocity jump. Needs prev2;
 * callers fall back to lerp_* when prev2 is absent or across a cut. */
static double hermite1(double prev2, double prev, double curr, double t) {
    double m0 = 0.5 * (curr - prev2);   /* centered tangent at prev */
    double m1 = (curr - prev);          /* backward tangent at curr */
    double t2 = t * t, t3 = t2 * t;
    return (2.0*t3 - 3.0*t2 + 1.0) * prev
         + (t3 - 2.0*t2 + t)       * m0
         + (-2.0*t3 + 3.0*t2)      * curr
         + (t3 - t2)               * m1;
}
static int16_t hround16(double v) { return (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5); }
static int32_t hround32(double v) { return (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5); }

static void herm_matrix(uint8_t* live, const uint8_t* prev2, const uint8_t* prev,
                        const uint8_t* curr, uint32_t off, double t) {
    const int16_t* p2 = (const int16_t*)(prev2 + off);
    const int16_t* pp = (const int16_t*)(prev  + off);
    const int16_t* pc = (const int16_t*)(curr  + off);
    int16_t*       pl = (int16_t*)(live + off);
    for (int i = 0; i < 9; ++i) pl[i] = hround16(hermite1(p2[i], pp[i], pc[i], t));
}
static void herm_vec3(uint8_t* live, const uint8_t* prev2, const uint8_t* prev,
                      const uint8_t* curr, uint32_t off, double t) {
    const int32_t* p2 = (const int32_t*)(prev2 + off);
    const int32_t* pp = (const int32_t*)(prev  + off);
    const int32_t* pc = (const int32_t*)(curr  + off);
    int32_t*       pl = (int32_t*)(live + off);
    for (int i = 0; i < 3; ++i) pl[i] = hround32(hermite1(p2[i], pp[i], pc[i], t));
}

/* ---- fractional interpolated-position table (the lerp32 1-unit floor fix) -----
 * lerp_vec3 writes the interpolated camera/object position to int32 RAM, which
 * ROUNDS to the 1-world-unit grid. Feeders that project against those positions
 * read the rounded RAM and inherit a uniform 1-unit positional floor (sub-pixel,
 * but it caps near-geometry continuity under sub-tick interpolation). For every
 * bracket we ALSO stash the SAME interpolation at DOUBLE precision (un-rounded),
 * keyed by vaddr (G_CAMERA / G_SPYRO / a moby vaddr); openpete_native_interp_pos
 * hands it back so feeders use the smooth path for their camera/object-relative
 * deltas instead of re-reading the floored RAM. Rebuilt every present, valid for
 * the duration of the feeder run (which executes inside reextract). */
/* Must cover every fpos_set caller in one present: camera + Spyro + the moby walk
 * (≤ MOBY_SAVE_MAX) + the HUD icons (G_HUD_COUNT). Undersizing silently drops the
 * fractional position for the overflow mobys → they fall back to the 1-unit RAM
 * floor (sub-pixel, not corruption), so keep this ≥ MOBY_SAVE_MAX + a little. */
#define FPOS_MAX (MOBY_SAVE_MAX + G_HUD_COUNT + 4)
static struct { uint32_t key; double p[3]; } s_fpos[FPOS_MAX];
static int s_fpos_n = 0;
/* The table is built + consumed entirely within a single sub-tick re-extract
 * present. The feeders are SHARED with the canonical 30 Hz render path, which
 * never refreshes the table — so the resolver MUST report "no entry" there (else
 * the canonical render reads stale fractional positions frozen at the last
 * sub-tick present: terrain/Spyro freeze while moby keyframe animation, a
 * separate channel, keeps going). s_fpos_active is 1 only during the re-extract
 * body; the canonical path sees it 0 and every feeder falls back to live RAM. */
static int s_fpos_active = 0;
static void fpos_reset(void) { s_fpos_n = 0; }
static void fpos_set(uint32_t key, const int32_t* prev, const int32_t* curr, double t) {
    if (s_fpos_n >= FPOS_MAX) return;
    s_fpos[s_fpos_n].key = key;
    for (int i = 0; i < 3; ++i)
        s_fpos[s_fpos_n].p[i] = (double)prev[i] + ((double)curr[i] - (double)prev[i]) * t;
    s_fpos_n++;
}
/* pred1 (Hermite) variant of fpos_set — must match herm_vec3's math so the
 * un-rounded fractional position feeders read agrees with the RAM we wrote. */
static void fpos_set_h(uint32_t key, const int32_t* prev2, const int32_t* prev,
                       const int32_t* curr, double t) {
    if (s_fpos_n >= FPOS_MAX) return;
    s_fpos[s_fpos_n].key = key;
    for (int i = 0; i < 3; ++i)
        s_fpos[s_fpos_n].p[i] = hermite1((double)prev2[i], (double)prev[i], (double)curr[i], t);
    s_fpos_n++;
}
int openpete_native_interp_pos(unsigned int key, double out[3]) {
    if (!s_fpos_active) return 0;   /* canonical render path → use live RAM */
    for (int i = 0; i < s_fpos_n; ++i) {
        if (s_fpos[i].key == (uint32_t)key) {
            out[0] = s_fpos[i].p[0]; out[1] = s_fpos[i].p[1]; out[2] = s_fpos[i].p[2];
            return 1;
        }
    }
    return 0;
}

/* ---- transient .s render-pass scratch bracket --------------------------------
 * The moby + Spyro BODY feeders re-derive geometry from PERSISTENT gamestate
 * (g_LevelMobys / g_Spyro / the models) BUT key off a small set of TRANSIENT
 * scratch the .s render pass leaves in g_SonyImage + the PSX scratchpad:
 *   - the moby REGULAR cull records (which mobys passed the cull) + the per-
 *     vertex fog-lit colour scratch                       (g_SonyImage+0x1100..)
 *   - the Spyro per-segment scratch (keyframe stream ptrs + vert counts)
 *                                                          (g_SonyImage+0x1900)
 *   - the shaded-moby (gems/props) Moby* list             (g_SonyImage+0x2400)
 *   - Spyro's projected int16 SXY (the NCLIP cull verdict) (PSX scratchpad)
 * This scratch only lives DURING the canonical render (the .s rebuilds/consumes
 * it each pass); by the time a sub-tick intermediate runs it is gone, so the
 * feeders read zeros and emit NOTHING — the mobys/Spyro vanish on every
 * intermediate and reappear on canonical frames (the flicker).
 *
 * Fix: capture each region at the canonical hook that produces it (proven live
 * there — the canonical feeder reads it at the same point), then on each
 * intermediate restore it (save-live → write-canon → run the READ-ONLY feeders
 * → restore-live, all on the host fiber). Reusing tick-N's cull verdict for the
 * sub-ticks between N and N+1 is correct: the visible set doesn't change within
 * a tick. The geometry still re-projects through the interpolated camera, so
 * mobys/Spyro move smoothly; only the (sub-pixel) cull decision is tick-N's. */
/* ---- W2.1: declarative transient-region registry --------------------------
 * One row per captured transient sub-region. Adding a region = add a backing
 * buffer + one table row (+ a capture call at its producer hook); the engine
 * below handles save-live → restore-canon → staged overlay → restore-live
 * generically. (The Fairy-Kiss 0x1D00 fix, previously three hand-edits, is
 * now literally the "fk" row.)
 *
 * producer — which canonical hook (re)fills the region; freshness is per
 *   producer (see the epoch comment below).
 * phase — SPHASE_ENTER rows restore at bracket entry (before the moby +
 *   shadow feeders). SPHASE_SEG_OVERLAY rows OVERLAP the regular-records row
 *   in PSX space and are overlaid only AFTER those feeders consumed the
 *   records (scratch_bracket_seg_restore) — reproducing the .s's temporal
 *   reuse of [0x1900,..): the moby pass fills cull records there, then the
 *   Spyro pass overwrites them with its seg scratch / Fairy-Kiss table once
 *   the records are consumed. (A single-shot restore used to clobber records
 *   >=14 at 0x1910+ — Sparx and distant chests vanished on sub-ticks.)
 *
 * Row storage is PER ROW (canon+live), so PSX-space overlap never aliases in
 * the backing store — overlap is purely a restore-ORDER concern, which the
 * phase column encodes. Live saves are all taken before any restore (one
 * coherent pre-state), so overlapping rows' saved bytes agree and leave()
 * may restore in any order. */
typedef enum { SPROD_MOBYREG = 0, SPROD_SPYRO, SPROD_SHADED, SPROD_COUNT } sprod_t;
typedef enum { SPHASE_ENTER = 0, SPHASE_SEG_OVERLAY } sphase_t;

typedef struct {
    const char* name;
    uint32_t    psx_base;    /* absolute PSX address (RAM or scratchpad) */
    uint32_t    len;
    sprod_t     producer;    /* epoch-stamp owner */
    sphase_t    phase;
    uint8_t*    canon;       /* row-owned canonical capture (len bytes) */
    uint8_t*    live;        /* row-owned live save (len bytes) */
    int         captured;    /* canon[] has ever been filled */
    int         open;        /* live[] holds this present's save */
    int         warned;      /* one-shot capture-length-mismatch warning */
} scratch_region_t;

#define SXY_BASE 0x1F800000u  /* PSX scratchpad (Spyro SXY / NCLIP verdicts) */

static uint8_t s_colour_buf[2][0x500];   /* [0]=canon  [1]=live */
static uint8_t s_recs_buf[2][0xE00];
static uint8_t s_shaded_buf[2][0x800];
static uint8_t s_sxy_buf[2][0x400];
static uint8_t s_seg_buf[2][0x100];
static uint8_t s_fk_buf[2][0x300];

static scratch_region_t s_scratch_rows[] = {
    /* per-vertex fog-lit colour scratch [0x1100,0x1600) */
    { "colour", G_SCRATCH + 0x1100u, 0x500u, SPROD_MOBYREG, SPHASE_ENTER,
      s_colour_buf[0], s_colour_buf[1], 0, 0, 0 },
    /* full REGULAR cull records [0x1600,0x2400) (<=64 @ 0x38) — captured at
     * the MOBY hook while live, BEFORE the Spyro pass reuses 0x1900 */
    { "recs",   G_SCRATCH + 0x1600u, 0xE00u, SPROD_MOBYREG, SPHASE_ENTER,
      s_recs_buf[0], s_recs_buf[1], 0, 0, 0 },
    /* shaded Moby* list + lit scratch [0x2400,0x2C00) */
    { "shaded", G_SCRATCH + 0x2400u, 0x800u, SPROD_SHADED, SPHASE_ENTER,
      s_shaded_buf[0], s_shaded_buf[1], 0, 0, 0 },
    /* Spyro's projected int16 SXY (the NCLIP cull verdict) — full 1 KB
     * hardware scratchpad */
    { "sxy",    SXY_BASE,            0x400u, SPROD_SPYRO,  SPHASE_ENTER,
      s_sxy_buf[0], s_sxy_buf[1], 0, 0, 0 },
    /* Spyro per-segment scratch [0x1900,0x1A00) — overlaps recs */
    { "seg",    G_SCRATCH + 0x1900u, 0x100u, SPROD_SPYRO,  SPHASE_SEG_OVERLAY,
      s_seg_buf[0], s_seg_buf[1], 0, 0, 0 },
    /* Fairy-Kiss pre-blend colour table [0x1D00,0x2000) — overlaps recs;
     * written by the Spyro pass AFTER the moby records, like the seg scratch.
     * When Fairy-Kiss is inactive it's harmless (build_spyro_walk reads
     * m_Colors, not 0x1D00). */
    { "fk",     G_SCRATCH + 0x1D00u, 0x300u, SPROD_SPYRO,  SPHASE_SEG_OVERLAY,
      s_fk_buf[0], s_fk_buf[1], 0, 0, 0 },
};
#define SCRATCH_ROWS ((int)(sizeof s_scratch_rows / sizeof s_scratch_rows[0]))
static int s_any_captured = 0;

/* Canonical-render EPOCH (replaces the W0.6 openpete_frame stamps). W0.6 tried
 * "capture is fresh iff its frame stamp == openpete_frame at re-extract", but
 * openpete_frame is a 60 Hz VBL counter (2 per 30 Hz game tick) that has always
 * advanced 1-2 by re-extract time — census proved match=0 on EVERY present, so
 * the whole scratch bracket was dead and the feeders silently read live scratch
 * + the unstamped SPEC snapshot (stale during pause/portal-load screens → the
 * "moby soup"). The epoch is instead bumped ONCE per canonical present, from
 * render_paths_commit — the same host-present-loop anchor that latches the
 * canon-drawn mask, guaranteed to run after the canonical render and before any
 * sub-tick re-extract. A capture is fresh iff its producer ran during the LAST
 * canonical present. No canonical present (CdRead stall) → no bump → the last
 * capture stays fresh (the world is byte-static; the run.c world-liveness gate
 * bounds long stalls).
 *
 * Freshness is PER PRODUCER: three canonical producers share the RP_MOBY path
 * bit (REGULAR func_8001F798, SPECULAR func_80020F34, SHADED/HUD/text
 * func_80022A2C), and a pause/portal-load screen runs ONLY the SHADED/text one
 * — a single shared stamp would let the text renderer revalidate the whole
 * moby replay. */
static uint32_t s_canon_epoch_now = 1;   /* stamp captures take (present in flight) */
static uint32_t s_canon_epoch_ok  = 0;   /* required stamp for a capture to be fresh */
static uint32_t s_prod_epoch[SPROD_COUNT] = {0};  /* per-producer last-capture stamp:
                                                   * MOBYREG = colour + cull records,
                                                   * SPYRO   = seg + FK + SXY,
                                                   * SHADED  = shaded list (text hook) */

void spyro_native_canon_epoch_commit(void) {
    s_canon_epoch_ok = s_canon_epoch_now;
    s_canon_epoch_now++;
}
uint32_t spyro_native_canon_epoch_stamp(void) { return s_canon_epoch_now; }
int spyro_native_canon_epoch_fresh(uint32_t stamp) { return stamp == s_canon_epoch_ok; }
int spyro_native_canon_mobyreg_fresh(void) { return s_prod_epoch[SPROD_MOBYREG] == s_canon_epoch_ok; }
int spyro_native_canon_shaded_fresh(void)  { return s_prod_epoch[SPROD_SHADED]  == s_canon_epoch_ok; }
static int scratch_prod_fresh(sprod_t p)   { return s_prod_epoch[p] == s_canon_epoch_ok; }

/* Registry engine: row lookup + capture. Captures fire at the producer hooks
 * (canonical moby/spyro/shaded render hooks, while the scratch is still live)
 * and stamp the row's producer epoch. */
static scratch_region_t* scratch_row_by_base(uint32_t psx_addr) {
    for (int i = 0; i < SCRATCH_ROWS; ++i)
        if (s_scratch_rows[i].psx_base == psx_addr) return &s_scratch_rows[i];
    return NULL;
}

static void scratch_row_capture(scratch_region_t* r) {
    const void* h = openpete_psx_to_host_ptr(r->psx_base);
    if (!h) return;
    memcpy(r->canon, h, r->len);
    r->captured = 1;
    s_any_captured = 1;
    s_prod_epoch[r->producer] = s_canon_epoch_now;
    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("OPENPETE_CANON_CAPDBG"); dbg = (e && *e && *e != '0'); }
    if (dbg && (openpete_frame % 200u) == 0) {
        uint32_t w0; memcpy(&w0, r->canon, 4);
        fprintf(stderr, "[canon-cap f=%u] row=%s base=0x%08x len=0x%x word0=%08x\n",
                openpete_frame, r->name, r->psx_base, r->len, w0);
    }
}

/* Capture a G_SCRATCH sub-range at its producer hook. `off`/`len` must name a
 * registry row (base match); an unmapped offset is a loud no-op — a new
 * capture site must come with a registry row, or its region never restores. */
void spyro_native_canon_capture_range(void* cpu_v, unsigned off, unsigned len) {
    (void)cpu_v;
    scratch_region_t* r = scratch_row_by_base(G_SCRATCH + off);
    if (!r) {
        fprintf(stderr, "[canon-cap] UNMAPPED capture offset 0x%04x (add a registry row)\n", off);
        return;
    }
    if (len != r->len && !r->warned) {
        r->warned = 1;
        fprintf(stderr, "[canon-cap] row '%s': capture len 0x%x != row len 0x%x (row len used)\n",
                r->name, len, r->len);
    }
    scratch_row_capture(r);
}

/* Capture Spyro's projected SXY scratchpad at the canonical spyro hook. */
void spyro_native_canon_capture_sxy(void* cpu_v) {
    (void)cpu_v;
    scratch_row_capture(scratch_row_by_base(SXY_BASE));
}

/* Capture the FULL regular cull-record region [0x1600,0x2400) at the MOBY hook —
 * while the records are still live, BEFORE the later Spyro pass reuses 0x1900
 * for its seg scratch (whose own row is captured later at the Spyro hook). */
void spyro_native_canon_capture_recs(void* cpu_v) {
    (void)cpu_v;
    scratch_row_capture(scratch_row_by_base(G_SCRATCH + 0x1600u));
}

/* On an intermediate: save the live scratch, overwrite with the captured canon,
 * so the read-only feeders that follow see valid tick-N render-pass inputs.
 *
 * The seg scratch [0x1900,0x1A00) is NOT restored here — it overlaps the regular
 * records (records 14+ start at 0x1910) which the .s reuses temporally. We give
 * the moby feeder the FULL records first; scratch_bracket_seg_restore then
 * overlays the seg scratch for the Spyro feeder (called between them). */
static int s_bracket_open = 0;   /* enter saved the live scratch this present */

static void scratch_bracket_enter(CPUState* cpu) {
    (void)cpu;
    {   /* OPENPETE_LOAD_CENSUS: per-producer freshness at re-extract time. */
        static int cen = -1;
        if (cen < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); cen = (e && *e && *e != '0'); }
        static unsigned last = 0xFFFFFFFFu;
        if (cen && openpete_frame != last) { last = openpete_frame;
            fprintf(stderr, "[census f=%u BRACKET] epoch_ok=%u fresh reg=%d spy=%d shd=%d\n",
                    openpete_frame, s_canon_epoch_ok,
                    spyro_native_canon_mobyreg_fresh(),
                    scratch_prod_fresh(SPROD_SPYRO),
                    spyro_native_canon_shaded_fresh()); }
    }
    if (!s_any_captured) return;
    /* Save TRUE live for ALL rows first, before any restore — one coherent
     * pre-state, so an overlapping row never saves another row's canon bytes,
     * and leave() restores everything back whether or not a canon restore
     * fired (a stale-skipped region is a saved-then-restored no-op). */
    for (int i = 0; i < SCRATCH_ROWS; ++i) {
        scratch_region_t* r = &s_scratch_rows[i];
        void* h = openpete_psx_to_host_ptr(r->psx_base);
        if (!h) continue;
        memcpy(r->live, h, r->len);
        r->open = 1;
    }
    s_bracket_open = 1;
    /* Restore canon per PRODUCER freshness: a producer that didn't run during
     * the last canonical present keeps whatever the live scratch holds (its
     * feeder pass is skipped too — moby_native_reextract checks the same
     * predicates), instead of replaying a stale capture. SEG_OVERLAY rows
     * wait for scratch_bracket_seg_restore (they overlap the records the
     * moby + shadow feeders are about to consume). */
    for (int i = 0; i < SCRATCH_ROWS; ++i) {
        scratch_region_t* r = &s_scratch_rows[i];
        if (r->phase != SPHASE_ENTER || !r->open || !r->captured) continue;
        if (!scratch_prod_fresh(r->producer)) continue;
        void* h = openpete_psx_to_host_ptr(r->psx_base);
        if (h) memcpy(h, r->canon, r->len);
    }
}

/* After the moby + shadow feeders have consumed the regular records, overlay
 * the SEG_OVERLAY rows over the (now-consumed) records at 0x1900+ — the same
 * temporal reuse the .s does — so the Spyro body feeder sees its seg scratch
 * and the Fairy-Kiss pre-blend colour table (else it reads the stale records
 * bytes and Spyro's colours flicker at >30fps during a Fairy-Kiss). */
static void scratch_bracket_seg_restore(CPUState* cpu) {
    (void)cpu;
    if (!s_bracket_open) return;
    for (int i = 0; i < SCRATCH_ROWS; ++i) {
        scratch_region_t* r = &s_scratch_rows[i];
        if (r->phase != SPHASE_SEG_OVERLAY || !r->open || !r->captured) continue;
        if (!scratch_prod_fresh(r->producer)) continue;
        void* h = openpete_psx_to_host_ptr(r->psx_base);
        if (h) memcpy(h, r->canon, r->len);
    }
}

/* Restore the live scratch before the game fiber resumes (determinism).
 * Symmetric with enter's unconditional save: every open row restores its
 * saved bytes. Overlapping rows all saved the same pre-state, so restore
 * order is irrelevant. */
static void scratch_bracket_leave(CPUState* cpu) {
    (void)cpu;
    if (!s_bracket_open) return;
    s_bracket_open = 0;
    for (int i = 0; i < SCRATCH_ROWS; ++i) {
        scratch_region_t* r = &s_scratch_rows[i];
        if (!r->open) continue;
        r->open = 0;
        void* h = openpete_psx_to_host_ptr(r->psx_base);
        if (h) memcpy(h, r->live, r->len);
    }
}

static void spyro_native_reextract(void* cpu_v) {
    CPUState* cpu = (CPUState*)cpu_v;

    /* W0.3 bracket-integrity: snapshot the fixed DIRECT gameplay regions BEFORE
     * any lerp write, so they can be re-checked after all restores at the end of
     * this present. Bracketed spans = exactly what the feeders write below. */
    const int bc_on = bracket_check_enabled();
    static const struct { uint32_t addr, len; const char* name; } bc_regs[] = {
        { G_CAMERA, CAM_BYTES,   "g_Camera"      },
        { G_SPYRO,  SPYRO_SAVE,  "g_Spyro"       },
        { G_SPYROFLAME,  FLAME_SAVE,  "g_SpyroFlame"  },
        { D_SPYROSHADOW, SHADOW_SAVE, "g_SpyroShadow" },
        { D_DRAGONSTAR,  0x2Cu,       "d_DragonStar"  },
    };
    const int bc_n = (int)(sizeof(bc_regs) / sizeof(bc_regs[0]));
    uint64_t bc_h0[5];
    if (bc_on) for (int i = 0; i < bc_n; ++i) bc_h0[i] = bc_hash(bc_regs[i].addr, bc_regs[i].len);

    /* Pair prev/curr camera snapshots (region 0 = g_Camera). Returns 0 when
     * the world is stuck (byte-identical: paused / loading / vbl busy-wait)
     * or no pairing yet (spawn) — then we present the live (curr) scene with
     * no camera bracket. */
    double qa = 0.0;
    const openpete_interp_snapshot_t *prev2 = NULL, *prev = NULL, *curr = NULL;
    int paired = openpete_interp_query3(op_now(), TICK_PERIOD_NS, &qa, &prev2, &prev, &curr);

    /* Rebuild the fractional interpolated-position table for this present (the
     * lerp32 1-unit floor fix). Empty until a bracket below populates it; feeders
     * fall back to reading the (floored) RAM for any key with no entry. Active for
     * the duration of this present only (cleared at the end) so the canonical
     * render path never reads a stale table. */
    fpos_reset();
    s_fpos_active = 1;

    /* Bracket targets (all written then restored on the host fiber). */
    uint8_t* cam = (uint8_t*)openpete_psx_to_host_ptr(G_CAMERA);
    uint8_t  cam_saved[CAM_BYTES];
    int      cam_bracketed = 0;
    uint8_t* sp = NULL; uint8_t sp_saved[SPYRO_SAVE]; int sp_bracketed = 0;
    int      sp_rot_active[3] = {0,0,0};   /* per-segment (body/head/tail) float-rot gate */
    double   sp_rot_ang[3][3] = {{0}};     /* [seg][Z,X,Y] sub-tick float byte-angles */
    uint8_t* fl = NULL; uint8_t fl_saved[FLAME_SAVE]; int fl_bracketed = 0;
    double   fl_subframe = 0.0;  /* fractional counter advance → flame sub-keyframe slide */
    uint8_t* sh = NULL; uint8_t sh_saved[SHADOW_SAVE]; int sh_bracketed = 0;
    static moby_save_t s_moby_save[MOBY_SAVE_MAX];
    int      moby_save_n = 0;
    static part_save_t s_part_save[PART_SAVE_MAX];
    int      part_save_n = 0;
    double   alpha = 0.0;

    /* Discontinuity guard: if the camera jumped this tick (level transition,
     * cutscene cut, teleport) interpolating across it produces a visible swoop.
     * Detect via the camera position delta (threshold calibrated at 5000
     * units/tick: continuous ≤1218, cuts ≥140k — game_extras_impl.c) and snap
     * (present the live curr scene, no bracket) instead. */
    int cam_cut = 0;
    if (paired && cam && prev && curr) {
        const int32_t* pp = (const int32_t*)(prev->data + prev->region_offsets[0] + CAM_OFF_POS);
        const int32_t* cp = (const int32_t*)(cam + CAM_OFF_POS);
        double dx = (double)cp[0] - pp[0], dy = (double)cp[1] - pp[1], dz = (double)cp[2] - pp[2];
        if (dx*dx + dy*dy + dz*dz > 5000.0 * 5000.0) cam_cut = 1;
        /* ROTATION cut: a stationary cut (camera spins to a new shot without moving
         * far — e.g. the dragon-rescue close-up, which flips the camera ~180° while
         * position barely changes) slips past the position threshold above. Linearly
         * lerping a large rotation matrix drives it through a near-singular midpoint
         * (determinant collapses) → severe scene warping on the intermediate presents.
         * Detect via the rotation angle between prev/curr camera matrices (orthonormal
         * Q12): cos(angle) = (trace(Rp^T·Rc) - 1)/2, and trace(Rp^T·Rc) is just the
         * Frobenius inner product of the two 3x3s / 4096^2. Snap when the camera turned
         * more than 90°/tick (cos<0 ⇒ frob<4096^2) — no smooth camera move does that
         * (it's 2700°/s); a normal fast pan stays far above the threshold (a 46° turn
         * gives frob≈2.4·4096^2), so this only fires on genuine cuts/reversals. */
        if (!cam_cut) {
            const int16_t* pm = (const int16_t*)(prev->data + prev->region_offsets[0] + CAM_OFF_PROJ);
            const int16_t* cm = (const int16_t*)(cam + CAM_OFF_PROJ);
            double frob = 0; for (int i = 0; i < 9; i++) frob += (double)pm[i] * cm[i];
            if (frob < 4096.0 * 4096.0) cam_cut = 1;   /* angle > 90° per tick */
        }
    }

    /* Moby occlusion-cull steering: suspend culling around camera cuts (the
     * few-presents-old occlusion grid is wrong everywhere across a cut) and
     * feed the measured per-tick pan speed (in screen px) so the feeder's
     * cached rects inflate with real camera motion. Unpaired presents (spawn /
     * world-stuck) get a short suspension — the camera may have moved
     * arbitrarily relative to the grid capture. */
    if (cam_cut) openpete_native_occl_suspend(12);
    if (paired && cam && prev && curr && !cam_cut) {
        /* Per-tick rotation = the max angle any ROW direction moved, from the
         * per-row NORMALIZED dot products. NOT the trace formula — the Spyro
         * camera matrix is rows-orthogonal but NOT orthonormal (the Y row
         * bakes a ~0.625 aspect scale), so acos((trace-1)/2) reads ~0.8 rad for IDENTICAL
         * matrices and would inflate every rect off the grid. */
        const int16_t* pm = (const int16_t*)(prev->data + prev->region_offsets[0] + CAM_OFF_PROJ);
        const int16_t* cm = (const int16_t*)(cam + CAM_OFF_PROJ);
        double worst = 1.0;
        for (int rw = 0; rw < 3; rw++) {
            double dp = 0, np = 0, nc = 0;
            for (int k = 0; k < 3; k++) {
                double a = pm[rw*3+k], b = cm[rw*3+k];
                dp += a * b; np += a * a; nc += b * b;
            }
            double denom = sqrt(np * nc);
            double c = denom > 0.0 ? dp / denom : 1.0;
            if (c < worst) worst = c;
        }
        if (worst > 1.0) worst = 1.0; else if (worst < -1.0) worst = -1.0;
        moby_native_set_occl_pan((float)(acos(worst) * 400.0)); /* rad/tick → px/tick (H≈288 + margin) */
    } else if (!cam_cut) {
        openpete_native_occl_suspend(2);
    }

    if (paired && cam && prev && curr && !cam_cut) {
        /* 1-tick-lag alpha: at the instant tick N completed (now≈curr.tick_ns)
         * show prev (N-1); advance to curr (N) over the next sim period. The
         * forward endpoint is the KNOWN canonical N → no extrapolation. */
        double period = interp_period_ns(prev->tick_ns, curr->tick_ns);
        alpha = (period > 0.0) ? (double)(op_now() - curr->tick_ns) / period : 1.0;
        if (alpha < 0.0) alpha = 0.0; else if (alpha > 1.0) alpha = 1.0;

        /* pred1 gate: use the cubic Hermite (centered start-tangent) only when a
         * valid prev2 exists AND the prev2→prev step is itself continuous — a cut
         * on that earlier step would make the centered tangent overshoot. The
         * DIRECT-ring feeders (camera + Spyro) share this gate; mobys/anim clocks
         * stay linear for now. Same 1-tick latency either way. */
        int use_herm = 0;
        const uint8_t* pcam2 = NULL;
        if (prev2 && hermite_enabled()) {
            pcam2 = prev2->data + prev2->region_offsets[0];
            const int32_t* p2p = (const int32_t*)(pcam2 + CAM_OFF_POS);
            const int32_t* pp  = (const int32_t*)(prev->data + prev->region_offsets[0] + CAM_OFF_POS);
            double dx = (double)pp[0]-p2p[0], dy = (double)pp[1]-p2p[1], dz = (double)pp[2]-p2p[2];
            use_herm = (dx*dx + dy*dy + dz*dz <= 5000.0 * 5000.0);
            if (use_herm) {   /* rotation cut on prev2→prev (same test as the curr guard) */
                const int16_t* p2m = (const int16_t*)(pcam2 + CAM_OFF_PROJ);
                const int16_t* pm  = (const int16_t*)(prev->data + prev->region_offsets[0] + CAM_OFF_PROJ);
                double frob = 0; for (int i = 0; i < 9; i++) frob += (double)p2m[i] * pm[i];
                if (frob < 4096.0 * 4096.0) use_herm = 0;
            }
        }

        /* (a) Camera: Q12 matrices + position (singleton, region 0). */
        const uint8_t* pcam = prev->data + prev->region_offsets[0];
        memcpy(cam_saved, cam, CAM_BYTES);        /* curr (live) */
        if (use_herm) {
            herm_matrix(cam, pcam2, pcam, cam_saved, CAM_OFF_PROJ, alpha);
            herm_matrix(cam, pcam2, pcam, cam_saved, CAM_OFF_VIEW, alpha);
            herm_vec3  (cam, pcam2, pcam, cam_saved, CAM_OFF_POS,  alpha);
            fpos_set_h(G_CAMERA,
                       (const int32_t*)(pcam2     + CAM_OFF_POS),
                       (const int32_t*)(pcam      + CAM_OFF_POS),
                       (const int32_t*)(cam_saved + CAM_OFF_POS), alpha);
        } else {
            lerp_matrix(cam, pcam, cam_saved, CAM_OFF_PROJ, alpha);
            lerp_matrix(cam, pcam, cam_saved, CAM_OFF_VIEW, alpha);
            lerp_vec3  (cam, pcam, cam_saved, CAM_OFF_POS,  alpha);
            /* Fractional camera position (un-rounded) for the floor fix — same lerp,
             * double precision. prev = pcam+POS, curr = cam_saved (the live curr we
             * saved before the lerp overwrote RAM). */
            fpos_set(G_CAMERA,
                     (const int32_t*)(pcam      + CAM_OFF_POS),
                     (const int32_t*)(cam_saved + CAM_OFF_POS), alpha);
        }
        cam_bracketed = 1;

        /* Per-present trace (OPENPETE_LAG_TRACE): the interpolated camera pos
         * presented THIS frame. Across the present sequence it must progress
         * monotonically — a sawtooth (jump forward then back) is the canonical-
         * vs-intermediate strobe (ghosting). */
        static int lt = -1;
        if (lt < 0) { const char* e = getenv("OPENPETE_LAG_TRACE"); lt = (e && *e && *e != '0'); }
        if (lt) {
            const int32_t* ip = (const int32_t*)(cam + CAM_OFF_POS);
            fprintf(stderr, "[lag-trace f=%u] a=%.3f camX=%d camZ=%d\n",
                    openpete_frame, alpha, ip[0], ip[2]);
        }

        /* (b) Spyro position (singleton, region SPYRO_REGION). */
        sp = (uint8_t*)openpete_psx_to_host_ptr(G_SPYRO);
        if (sp) {
            const uint8_t* psp = prev->data + prev->region_offsets[SPYRO_REGION];
            memcpy(sp_saved, sp, SPYRO_SAVE);          /* curr (live) */
            /* pred1 for Spyro's m_Position, gated on the camera's prev2 continuity
             * PLUS a Spyro-specific position-step check (Spyro can warp while the
             * camera holds). Falls back to the linear lerp otherwise. */
            int sp_herm = 0;
            const uint8_t* psp2 = NULL;
            if (use_herm && prev2) {
                psp2 = prev2->data + prev2->region_offsets[SPYRO_REGION];
                const int32_t* p2 = (const int32_t*)psp2;
                const int32_t* pp = (const int32_t*)psp;
                double dx=(double)pp[0]-p2[0], dy=(double)pp[1]-p2[1], dz=(double)pp[2]-p2[2];
                sp_herm = (dx*dx + dy*dy + dz*dz <= 5000.0 * 5000.0);
            }
            if (sp_herm) {
                herm_vec3(sp, psp2, psp, sp_saved, 0, alpha);   /* +0x00 m_Position */
                fpos_set_h(G_SPYRO, (const int32_t*)psp2,
                           (const int32_t*)psp, (const int32_t*)sp_saved, alpha);
            } else {
                lerp_vec3(sp, psp, sp_saved, 0, alpha);    /* +0x00 m_Position */
                /* Fractional Spyro position (un-rounded) for the floor fix. */
                fpos_set(G_SPYRO, (const int32_t*)psp, (const int32_t*)sp_saved, alpha);
            }
            /* Per-segment (body/head/tail) animation clock: lerp each
             * m_FrameProgress (0x24/0x25/0x26) when its keyframe pair
             * (frameA 0x1E/0x20/0x22, frameB 0x1F/0x21/0x23) is unchanged —
             * else snap (no blend across a knot or a transition reseed). */
            static const int frA[3] = {0x1E,0x20,0x22}, frB[3] = {0x1F,0x21,0x23},
                             prog[3] = {0x24,0x25,0x26};
            for (int s = 0; s < 3; s++) {
                if (psp[frA[s]] == sp_saved[frA[s]] && psp[frB[s]] == sp_saved[frB[s]]) {
                    int pa = psp[prog[s]], ca = sp_saved[prog[s]];
                    int la = (int)((double)pa + ((double)ca - pa) * alpha + 0.5);
                    if (la < 0) la = 0; else if (la > 255) la = 255;
                    sp[prog[s]] = (uint8_t)la;
                }
            }
            /* Body/head/tail rotation (packed 3×8-bit euler @ +0xC/+0x10/+0x14):
             * computed at FLOAT precision and handed to the Spyro euler composer
             * (spyro_native_set_rot_interp) — NOT written back to the byte euler,
             * which would re-quantize to the 256/circle (1.4°/step) grid the same
             * way the moby path did. Body (s=0) always interpolates; head/tail carry
             * a 0-sentinel ("use body") so interpolate only when both endpoints are
             * real — else leave that segment on the byte-exact path (snap). */
            for (int s = 0; s < 3; s++) {
                uint32_t off = 0x0Cu + (uint32_t)s * 4u;
                uint32_t pr, cr; memcpy(&pr, psp + off, 4); memcpy(&cr, sp_saved + off, 4);
                if (s != 0 && (pr == 0 || cr == 0)) continue;   /* sticky / transition → byte path */
                for (int k = 0; k < 3; k++) {
                    int pbk = (int)((pr >> (k*8)) & 0xFFu), cbk = (int)((cr >> (k*8)) & 0xFFu);
                    int d = cbk - pbk; if (d > 128) d -= 256; else if (d < -128) d += 256;
                    sp_rot_ang[s][k] = (double)pbk + (double)d * alpha;
                }
                sp_rot_active[s] = 1;
            }
            sp_bracketed = 1;
        }

        /* (c) Flame breath pose (singleton, region FLAME_REGION). The ribbon is
         * rigidly attached to Spyro's mouth/head: m_Position (origin) + the
         * flame-attach matrix both advance at the 30 Hz tick, so without this the
         * flame snaps to its tick-N pose while the (interpolated) Spyro it should
         * sit on moves smoothly — the 30 Hz look. Lerp both at the same alpha as
         * Spyro so the flame tracks him.
         *
         * The ribbon ANIMATION (not just the pose) is the plan's RE-RUN recipe:
         * the strip is `flameMatrix·keyframe[counter]` over a STATIC keyframe
         * template (D_FLAME_GEOM — proven byte-constant across ticks), indexed by
         * the per-strand counter FL_CNT which advances by g_DeltaTime (2..4) each
         * tick. Re-running flame_build_strip with a sub-tick-advanced counter walks
         * the template at the present rate → genuinely high-FPS flame animation
         * (NOT a lerp of two shapes). An intermediate counter is a legitimate game
         * state — it occurs naturally at g_DeltaTime=1 — and flame_build_strip is
         * fully data-driven by it, so the intermediate ribbon is faithful.
         *
         * Teleport-guard the flame's first active tick (prev pose stale from
         * before activation): snap when the origin jumps far. */
        fl = (uint8_t*)openpete_psx_to_host_ptr(G_SPYROFLAME);
        if (fl) {
            const uint8_t* pfl = prev->data + prev->region_offsets[FLAME_REGION];
            const int32_t* pp = (const int32_t*)(pfl + FLAME_POS_OFF);
            const int32_t* cp = (const int32_t*)(fl  + FLAME_POS_OFF);
            double dx = (double)cp[0]-pp[0], dy = (double)cp[1]-pp[1], dz = (double)cp[2]-pp[2];
            if (dx*dx + dy*dy + dz*dz <= 5000.0 * 5000.0) {
                memcpy(fl_saved, fl, FLAME_SAVE);                 /* curr (live) */
                lerp_vec3  (fl, pfl, fl_saved, FLAME_POS_OFF, alpha);
                lerp_matrix(fl, pfl, fl_saved, FLAME_ATT_OFF, alpha);
                /* Per-strand keyframe counter: advance fractionally so the ribbon
                 * extends/scrolls at the present rate. The counter selects the
                 * ribbon's window into the static cross-section spine (one 8-byte
                 * record per step); write the FLOOR of the interpolated counter as
                 * the window and hand the fractional part to the flame feeder as a
                 * SUB-KEYFRAME slide (record[p]→record[p+8] lerp) so the ribbon
                 * animates below the keyframe grid — as smooth as Spyro's anim
                 * lerp, not stepping one record at a time. SNAP on a large jump —
                 * the flame periodically reseeds (the counter wraps 30→0), and
                 * lerping across that collapses the ribbon through zero. |Δ|≤8
                 * separates the continuous advance/retract (g_DeltaTime 2..4) from
                 * the wrap (~30). The advance is the same for every (non-snapped)
                 * strand (all += g_DeltaTime from an integer prev), so the frac is
                 * shared; take it from the first advancing strand. */
                for (int s = 0; s < 8; s++) {
                    int pcnt = pfl[FLAME_CNT_OFF + s], ccnt = fl_saved[FLAME_CNT_OFF + s];
                    int d = ccnt - pcnt;
                    if (d > -8 && d < 8) {
                        double fadv = (double)pcnt + (double)d * alpha;
                        double ffloor = floor(fadv);
                        int v = (int)ffloor; if (v < 0) v = 0;
                        fl[FLAME_CNT_OFF + s] = (uint8_t)v;
                        if (fl_subframe == 0.0 && d > 0) fl_subframe = fadv - ffloor;
                    }   /* else keep curr (wrap/reseed — already in fl) */
                }
                fl_bracketed = 1;
            }
        }

        /* (d) Spyro drop-shadow ground anchor (singleton, region SHADOW_REGION).
         * The canonical shadow render caches the anchor (m_Position @ +0x10) each
         * tick; it tracks Spyro's ground projection but lives in a scratch global
         * the spine doesn't otherwise touch, so without this the shadow steps at
         * 30 Hz under the (interpolated) Spyro. Lerp it at the same alpha as Spyro
         * so the shadow tracks him; teleport-guard a jump (level transition / warp
         * / a frame Spyro lifts off → re-grounds). (Moby shadows need no bracket —
         * they re-derive from each moby's already-interpolated m_Position.) */
        sh = (uint8_t*)openpete_psx_to_host_ptr(D_SPYROSHADOW);
        if (sh) {
            const uint8_t* psh = prev->data + prev->region_offsets[SHADOW_REGION];
            const int32_t* pp = (const int32_t*)(psh + SHADOW_POS_OFF);
            const int32_t* cp = (const int32_t*)(sh  + SHADOW_POS_OFF);
            double dx = (double)cp[0]-pp[0], dy = (double)cp[1]-pp[1], dz = (double)cp[2]-pp[2];
            if (dx*dx + dy*dy + dz*dz <= 5000.0 * 5000.0) {
                memcpy(sh_saved, sh, SHADOW_SAVE);            /* curr (live) */
                lerp_vec3(sh, psh, sh_saved, SHADOW_POS_OFF, alpha);
                sh_bracketed = 1;
            }
        }

    }

    /* Per-moby interpolation (indirect table, keyed by Moby vaddr) — runs
     * UNCONDITIONALLY with a PER-OBJECT alpha, NOT gated on the camera bracket.
     * The DIRECT ring dedupes the WHOLE camera+Spyro snapshot, so when BOTH are
     * byte-static — the player idling in a homeworld while the dragons keep their
     * idle animation running — the ring reports paired=0 and the old camera-gated
     * moby block was skipped entirely, freezing every moby's animation/movement at
     * 30 Hz (the bug: Artisans cutscene dragons step while the camera is still).
     * Each moby instead sweeps across ITS OWN distinct-state interval
     * (o->prev_ns..o->curr_ns) — identical to the camera alpha when both advance
     * every tick (both stamped from op_now() one VBL apart, so spatial coherence
     * with a moving camera is preserved), and keeps animating when the camera is
     * static. Same per-object-cadence pattern as the HUD icons below. */
    {
        op_time_t now = op_now();
        uint8_t* base_p = (uint8_t*)openpete_psx_to_host_ptr(G_LEVELMOBYS);
        uint32_t arr = 0; if (base_p) memcpy(&arr, base_p, 4);
        if (arr >= 0x80000000u) {
            uint32_t m = arr; int guard = 0;
            while (guard++ < 4096 && moby_save_n < MOBY_SAVE_MAX) {
                uint8_t* mh = (uint8_t*)openpete_psx_to_host_ptr(m);
                if (!mh) break;
                uint8_t st = mh[MOBY_STATE];
                if (st == 0xFF) break;                /* array sentinel */
                if (st < 0x80) {                      /* alive */
                    const openpete_interp_obj_t* o = openpete_interp_objs_find(m);
                    if (o && o->have_prev) {
                        /* IDENTITY guard (slot reuse). The interp-objs table is keyed
                         * by the moby SLOT ADDRESS, so a slot a DIFFERENT moby respawns
                         * into within one tick still reads have_prev=1 and would lerp
                         * between two unrelated objects — position AND the two-pass
                         * keyframe shape (garbage when the models differ). The teleport
                         * guard below only catches FAR reuses; a near respawn (same
                         * region) slips through it. m_Class @ +0x36 (i16) is the stable
                         * type and the model is derived from it (G_MODELS[class]), so a
                         * class change is a different model → snap (present curr, no
                         * bracket). Mirrors the particle class/sel guard. (Same-class
                         * instance reuse — same model, near, a brief pose blend — is a
                         * benign residual; catching it needs a per-instance id the moby
                         * record doesn't carry.) */
                        uint16_t pcls, ccls;
                        memcpy(&pcls, o->prev + 0x36u, 2);
                        memcpy(&ccls, o->curr + 0x36u, 2);
                        if (pcls != ccls) { m += MOBY_STRIDE; continue; }

                        /* Per-object alpha from the moby's OWN distinct-roll
                         * timestamps, over a denominator capped at one tick
                         * (interp_period_ns — a stale prev after a static stretch
                         * must not stretch the resume); clamps to curr once it
                         * settles. */
                        op_time_t period_ns = o->curr_ns - o->prev_ns;
                        if (o->prev_ns != 0 && period_ns > 0) {
                            double ma = (double)(now - o->curr_ns) / interp_period_ns(o->prev_ns, o->curr_ns);
                            if (ma < 0.0) ma = 0.0; else if (ma > 1.0) ma = 1.0;
                            int32_t pp[3], cp[3];
                            memcpy(pp, o->prev + MOBY_POS, 12);
                            memcpy(cp, o->curr + MOBY_POS, 12);
                            /* per-moby teleport guard: a respawn / warp jumps far in
                             * one tick — lerping streaks it across the screen. Snap
                             * (leave at curr, don't bracket) when the jump is large. */
                            double dx = (double)cp[0]-pp[0], dy = (double)cp[1]-pp[1], dz = (double)cp[2]-pp[2];
                            if (dx*dx + dy*dy + dz*dz > 5000.0 * 5000.0) { m += MOBY_STRIDE; continue; }
                            s_moby_save[moby_save_n].addr = m;
                            memcpy(s_moby_save[moby_save_n].bytes, mh, MOBY_STRIDE);
                            moby_save_n++;
                            /* pred1: use the cubic Hermite (centered start-tangent)
                             * when this moby has a valid prev2 whose prev2->prev step
                             * is itself continuous — else the linear prev->curr lerp.
                             * Same per-object alpha + 1-tick latency either way. */
                            int m_herm = 0; int32_t p2[3];
                            if (hermite_enabled() && o->have_prev2) {
                                memcpy(p2, o->prev2 + MOBY_POS, 12);
                                double e2x=(double)pp[0]-p2[0], e2y=(double)pp[1]-p2[1], e2z=(double)pp[2]-p2[2];
                                m_herm = (e2x*e2x + e2y*e2y + e2z*e2z <= 5000.0 * 5000.0);
                            }
                            for (int k = 0; k < 3; k++) {
                                int32_t lv = m_herm ? hround32(hermite1(p2[k], pp[k], cp[k], ma))
                                                    : lerp32(pp[k], cp[k], ma);
                                memcpy(mh + MOBY_POS + k * 4, &lv, 4);
                            }
                            /* Fractional moby position (un-rounded) for the floor
                             * fix — keyed by the moby vaddr, at this moby's own
                             * per-object alpha. The body / shaded / shadow feeders
                             * look it up so the moby's own motion is continuous too
                             * (not just the camera component). */
                            if (m_herm) fpos_set_h(m, p2, pp, cp, ma);
                            else        fpos_set(m, pp, cp, ma);
                            /* Animation: NOT synthesized into RAM here anymore. The
                             * old approach wrote an interpolated m_FrameProgress (and,
                             * across a knot, an extended "continuous-phase" blend) into
                             * mh[0x3E/0x3F/0x40], which the moby decode then sampled —
                             * decoding a keyframe pair at a blend the game never
                             * rendered, stretching fast-animating mobys off-model (the
                             * same bug the Spyro player path hit). The moby decode now
                             * does a TWO-PASS shape lerp instead: moby_decode_project
                             * reconstructs the prev REAL pose (moby_prev_keyframe reads
                             * this moby's o->prev frames) and the curr REAL pose and
                             * lerps them in model space at this same per-object alpha.
                             * So we leave mh's m_Frame/m_NextFrame/m_FrameProgress at the
                             * canonical curr values (the decode reads them as the curr
                             * pose) and let the feeder handle the interpolation. */
                            /* Rotation (m_Rotation @ +0x44, Vector3D8) is NOT lerped
                             * into RAM here: a byte write would re-quantize to the
                             * 1.4°/step grid (256/circle) and the sub-tick interp
                             * couldn't subdivide below it — the residual stair-step on
                             * slow rotators (gems/dragons). Instead the moby euler
                             * composer (shaded_compose_euler) reads this moby's
                             * prev/curr snapshots and renders the rotation at FLOAT
                             * precision (table-interpolated sin/cos). Enabled for the
                             * feeder pass via moby_native_set_rot_interp below. */
                        }
                    }
                }
                m += MOBY_STRIDE;
            }
        }
    }

    /* Static 3D HUD icons (g_Hud.m_Mobys[12]) live OUTSIDE g_LevelMobys (so the
     * moby loop above misses them) and animate while the camera is dead still —
     * the inventory. So this runs UNCONDITIONALLY (not gated on the camera being
     * paired) with a PER-OBJECT alpha (the camera's alpha clamps to 1 when static,
     * which would step them). Two transitions to smooth: the m_Rotation (+0x44)
     * SPIN, and the m_Position (+0x0C) fly/slide-in when the HUD first appears.
     * Bracketed via s_moby_save (restored with the rest below). */
    {
        op_time_t now = op_now();
        for (uint32_t i = 0; i < G_HUD_COUNT && moby_save_n < MOBY_SAVE_MAX; i++) {
            uint32_t m = G_HUD_ICONS + i * MOBY_STRIDE;
            uint8_t* mh = (uint8_t*)openpete_psx_to_host_ptr(m);
            if (!mh) continue;
            const openpete_interp_obj_t* o = openpete_interp_objs_find(m);
            if (!o || !o->have_prev) continue;
            /* Per-object alpha from the icon's OWN distinct-roll timestamps:
             * sweeps 0→1 across its animation interval (independent of the camera,
             * which may be static), and clamps to curr once it stops moving
             * (curr_ns goes stale) so a settled icon doesn't wobble prev↔curr. */
            op_time_t period = o->curr_ns - o->prev_ns;
            if (o->prev_ns == 0 || period <= 0) continue;     /* no interval yet → show curr */
            double ha = (double)(now - o->curr_ns) / interp_period_ns(o->prev_ns, o->curr_ns);
            if (ha < 0.0) ha = 0.0; else if (ha > 1.0) ha = 1.0;
            s_moby_save[moby_save_n].addr = m;
            memcpy(s_moby_save[moby_save_n].bytes, mh, MOBY_STRIDE);
            moby_save_n++;
            /* Position (m_Position +0x0C): when the HUD appears the icons slide
             * down (Y, ~3-5 units/tick) into place — lerp that so the slide is as
             * smooth as the spin. Teleport-guard the one-tick appearance jump (the
             * initial placement is ~2880 units in Z/X) so it snaps into place
             * instead of streaking from a possibly-garbage spawn position. The
             * guard sits well above the slide (~5) and below the placement jump. */
            {
                int32_t pp[3], cp[3];
                memcpy(pp, o->prev + MOBY_POS, 12);
                memcpy(cp, o->curr + MOBY_POS, 12);
                double dx = (double)cp[0]-pp[0], dy = (double)cp[1]-pp[1], dz = (double)cp[2]-pp[2];
                if (dx*dx + dy*dy + dz*dz <= 512.0 * 512.0)
                    for (int k = 0; k < 3; k++) {
                        int32_t lv = lerp32(pp[k], cp[k], ha);
                        memcpy(mh + MOBY_POS + k * 4, &lv, 4);
                    }
            }
            /* Rotation (m_Rotation +0x44): NOT lerped here — the HUD icons route
             * through the moby euler composer (shaded_compose_euler), which now
             * renders rotation at float precision from their prev/curr interp-objs
             * snapshots (moby_native_set_rot_interp). A byte write here would
             * re-quantize to the 256/circle grid and be ignored by that float path. */
        }
    }

    /* Per-particle interpolation (g_Particles pool): lerp each active particle's
     * position by its OWN distinct-roll alpha so smoke / triangles / line
     * particles move at the present rate instead of stepping at 30 Hz. Keyed by
     * slot address in the interp-objs table (same store as the mobys); particles
     * spawn into inactive slots → have_prev=0 → snap (no streak from a stale slot),
     * despawns are evicted. Pure LERP of two observed states → never calls the
     * particle update's rand() (which would break gameplay 1:1), so it's safe for
     * the rand-spawned cinders too. NOT the g_Sparkles pool (rand m_Life, carved
     * out) or glows (they ride their owner moby). Teleport-guard a slot reused by a
     * new particle mid-flight. */
    {
        op_time_t now = op_now();
        /* OPENPETE_PART_INTERP_DBG: per-second census of pool-interp verdicts —
         * lerped vs each snap reason, MOVING particles (prev->curr delta > 2
         * world/4) counted separately. A moving particle that keeps snapping is
         * a 30 Hz stepper (the circling-stars class). */
        static int s_pidbg = -1;
        if (s_pidbg < 0) { const char* e = getenv("OPENPETE_PART_INTERP_DBG"); s_pidbg = (e && *e && *e != '0'); }
        static long s_pi[8]; /* 0 lerp,1 noprev,2 classchg,3 lifestall,4 far,5 lerp-moving,6 classchg-moving,7 lifestall-moving */
        uint8_t* pbase = (uint8_t*)openpete_psx_to_host_ptr(G_PARTICLES);
        uint32_t ppool = 0; if (pbase) memcpy(&ppool, pbase, 4);
        if (ppool >= 0x80000000u) {
            for (uint32_t i = 0; i < 320u && part_save_n < PART_SAVE_MAX; i++) {
                uint32_t pa = ppool + i * PART_STRIDE;
                uint8_t* ph = (uint8_t*)openpete_psx_to_host_ptr(pa);
                if (!ph) break;
                uint32_t hdr; memcpy(&hdr, ph, 4);
                uint8_t sel = (uint8_t)((hdr >> PART_HDR_SEL) & 0xFFu);
                if (sel == 0xFFu) break;            /* end of pool */
                if (sel & 0x80u) continue;          /* inactive slot */
                const openpete_interp_obj_t* o = openpete_interp_objs_find(pa);
                if (!o || !o->have_prev) { s_pi[1]++; continue; }  /* spawn → snap (curr) */
                op_time_t period = o->curr_ns - o->prev_ns;
                if (o->prev_ns == 0 || period <= 0) continue;
                double pa_alpha = (double)(now - o->curr_ns) / interp_period_ns(o->prev_ns, o->curr_ns);
                if (pa_alpha < 0.0) pa_alpha = 0.0; else if (pa_alpha > 1.0) pa_alpha = 1.0;
                uint32_t pw1, pw2, cw1, cw2;
                memcpy(&pw1, o->prev + PART_POS_OFF, 4); memcpy(&pw2, o->prev + 0x08u, 4);
                memcpy(&cw1, o->curr + PART_POS_OFF, 4); memcpy(&cw2, o->curr + 0x08u, 4);
                int pX = (int16_t)(pw1 & 0xFFFF), pY = (int16_t)(pw1 >> 16), pZ = (int16_t)(pw2 & 0xFFFF);
                int cX = (int16_t)(cw1 & 0xFFFF), cY = (int16_t)(cw1 >> 16), cZ = (int16_t)(cw2 & 0xFFFF);
                /* teleport guard: a slot reused by a new particle mid-flight jumps
                 * far in one tick — snap. Particle positions are world/4 (int16), so
                 * the threshold is in world/4 too: 1250 world/4 == 5000 full-world,
                 * matching the moby/camera/shadow guards. (The old 8000 was 32000
                 * full-world — ~6.5× looser — so a slot reused within ~32k units
                 * streaked across two different particles instead of snapping.) */
                double dX = cX - pX, dY = cY - pY, dZ = cZ - pZ;
                /* IDENTITY guard (slot reuse). The interp-objs table is keyed by slot
                 * ADDRESS, so a slot a NEW particle respawns into within one tick — a
                 * shatter/burst churning the pool faster than slots free — still reads
                 * have_prev=1 and gets lerped between TWO DIFFERENT particles: a long
                 * line streaks from the old position to the new one. The distance guard
                 * below only catches FAR reuses; a burst clustered near its emitter
                 * reuses well within it. Detect identity from the particle header word:
                 * m_Class (byte0) + sel/type (byte1) is the stable type, and m_Life
                 * (byte2) is an age counter that INCREMENTS every tick for a CONTINUING
                 * particle (measured across the run: outside spawn bursts 100% of
                 * have_prev particles have life strictly increasing; reuse — type change,
                 * life reset, or life held — appears only inside bursts). So lerp only a
                 * genuinely continuing particle; otherwise snap (present curr, no streak). */
                {
                    int moving = (dX*dX + dY*dY + dZ*dZ > 4.0);
                    uint32_t ph2, ch2; memcpy(&ph2, o->prev, 4); memcpy(&ch2, o->curr, 4);
                    if ((ph2 & 0xFFFFu) != (ch2 & 0xFFFFu)) { s_pi[2]++; if (moving) s_pi[6]++; continue; } /* m_Class/sel changed → reuse */
                    uint8_t plife = (uint8_t)(ph2 >> 16), clife = (uint8_t)(ch2 >> 16);
                    /* Life DECREASED → reset → slot reuse: snap. Life EQUAL is
                     * ambiguous: burst reuse (rare same-life coincidence) vs a
                     * PERSISTENT particle whose life byte stalls — the circling
                     * damage stars (user report 2026-07-04): 245-300 moving
                     * particles/s snapped here during the effect, stepping the
                     * orbit at 30 Hz. Lerp stalled-life particles only within a
                     * TIGHT radius (300 world/4 per tick — orbiting stars move
                     * ~50-100; burst reuses jump farther or reset life). */
                    if (clife < plife) { s_pi[3]++; if (moving) s_pi[7]++; continue; }
                    if (clife == plife && dX*dX + dY*dY + dZ*dZ > 300.0*300.0)
                        { s_pi[3]++; if (moving) s_pi[7]++; continue; }
                    s_pi[0]++; if (moving) s_pi[5]++;
                }
                if (dX*dX + dY*dY + dZ*dZ > 1250.0 * 1250.0) { s_pi[4]++; continue; }
                uint32_t live1, live2;
                memcpy(&live1, ph + PART_POS_OFF, 4); memcpy(&live2, ph + 0x08u, 4);
                s_part_save[part_save_n].addr = pa;
                s_part_save[part_save_n].w1 = live1;
                s_part_save[part_save_n].w2 = live2;
                part_save_n++;
                int16_t lX = lerp16((int16_t)pX, (int16_t)cX, pa_alpha);
                int16_t lY = lerp16((int16_t)pY, (int16_t)cY, pa_alpha);
                int16_t lZ = lerp16((int16_t)pZ, (int16_t)cZ, pa_alpha);
                uint32_t nw1 = (uint32_t)(uint16_t)lX | ((uint32_t)(uint16_t)lY << 16);
                uint32_t nw2 = (live2 & 0xFFFF0000u) | (uint32_t)(uint16_t)lZ;
                memcpy(ph + PART_POS_OFF, &nw1, 4);
                memcpy(ph + 0x08u, &nw2, 4);
            }
        }
        if (s_pidbg) {
            static op_time_t t0 = 0;
            if (t0 == 0) t0 = now;
            if (now - t0 >= OP_NS_PER_SEC) {
                fprintf(stderr, "[part-interp f=%u] lerp=%ld (moving=%ld) noprev=%ld classchg=%ld (mov=%ld) lifestall=%ld (mov=%ld) far=%ld\n",
                        openpete_frame, s_pi[0], s_pi[5], s_pi[1], s_pi[2], s_pi[6], s_pi[3], s_pi[7], s_pi[4]);
                memset(s_pi, 0, sizeof s_pi);
                t0 = now;
            }
        }
    }

    /* Dragon-star record (D_DRAGONSTAR — the circling damage stars): a single
     * gamestate record (pos +0x04, per-effect rotation SHORTMATRIX +0x10, spoke
     * size +0x24, colour byte +0x2B) that func_8002F3E4 animates once per tick;
     * the feeder re-reads it every present, so un-bracketed the stars circle at
     * 30 Hz regardless of --render-fps (user report, particle-circling-stars.bk2
     * f1850). Lerp prev→curr at the record's own distinct-roll alpha — position
     * (int32), matrix (componentwise int16, then the two USED column vectors
     * — [r][0]/[r][2], the spoke basis — renormalized to Q12 so the chord lerp
     * can't shrink the ring), size and colour. Guards: active in BOTH snapshots
     * (re-trigger → snap), teleport (>5000 world units), rotation >90°/tick
     * (column-0 cosine < 0 → snap; lerp would arc the wrong way). Bracketed:
     * saved here, restored with the other brackets below. */
    uint8_t* ds_host = NULL; int ds_bracketed = 0; static uint8_t ds_saved[0x2C];
    {
        ds_host = (uint8_t*)openpete_psx_to_host_ptr(D_DRAGONSTAR);
        uint32_t gate = 0; if (ds_host) memcpy(&gate, ds_host, 4);
        const openpete_interp_obj_t* o = (gate != 0) ? openpete_interp_objs_find(D_DRAGONSTAR) : NULL;
        if (o && o->have_prev && o->prev_ns != 0 && o->curr_ns > o->prev_ns) {
            uint32_t pg, cg; memcpy(&pg, o->prev, 4); memcpy(&cg, o->curr, 4);
            int32_t pp[3], cp[3];
            memcpy(pp, o->prev + 0x04, 12); memcpy(cp, o->curr + 0x04, 12);
            double dx = (double)cp[0]-pp[0], dy = (double)cp[1]-pp[1], dz = (double)cp[2]-pp[2];
            int16_t pm[9], cm[9];
            memcpy(pm, o->prev + 0x10, 18); memcpy(cm, o->curr + 0x10, 18);
            /* column-0 rotation magnitude between snapshots (Q12 dot) */
            double c0dot = ((double)pm[0]*cm[0] + (double)pm[3]*cm[3] + (double)pm[6]*cm[6]) / (4096.0*4096.0);
            if (pg != 0 && cg != 0 &&
                dx*dx + dy*dy + dz*dz <= 5000.0*5000.0 &&
                c0dot > 0.0) {
                double a2 = (double)(op_now() - o->curr_ns) / interp_period_ns(o->prev_ns, o->curr_ns);
                if (a2 < 0.0) a2 = 0.0; else if (a2 > 1.0) a2 = 1.0;
                memcpy(ds_saved, ds_host, 0x2C); ds_bracketed = 1;
                for (int k = 0; k < 3; k++) {
                    int32_t v = pp[k] + (int32_t)llround(((double)cp[k] - pp[k]) * a2);
                    memcpy(ds_host + 0x04 + 4*k, &v, 4);
                }
                double lm[9];
                for (int j = 0; j < 9; j++) lm[j] = (double)pm[j] + ((double)cm[j] - pm[j]) * a2;
                for (int col = 0; col <= 2; col += 2) {   /* the used spoke basis columns */
                    double n = sqrt(lm[0+col]*lm[0+col] + lm[3+col]*lm[3+col] + lm[6+col]*lm[6+col]);
                    if (n > 1.0) {
                        double s = 4096.0 / n;
                        lm[0+col] *= s; lm[3+col] *= s; lm[6+col] *= s;
                    }
                }
                for (int j = 0; j < 9; j++) {
                    int16_t v16 = (int16_t)llround(lm[j]);
                    memcpy(ds_host + 0x10 + 2*j, &v16, 2);
                }
                int32_t psz, csz; memcpy(&psz, o->prev + 0x24, 4); memcpy(&csz, o->curr + 0x24, 4);
                int32_t lsz = psz + (int32_t)llround(((double)csz - psz) * a2);
                memcpy(ds_host + 0x24, &lsz, 4);
                int pc = o->prev[0x2B], ccol = o->curr[0x2B];
                ds_host[0x2B] = (uint8_t)(pc + (int)llround(((double)ccol - pc) * a2));
            }
        }
    }

    /* The 2D-UI overlay (fade + cutscene letterbox + menu panel + the 2D-UI
     * sprites/lines) is snapshotted by openpete_native_scene_capture_overlay() in
     * run.c's CANONICAL present block — right after the game render produced it,
     * before THIS rebuild wipes it. Capturing here instead would also run on every
     * SUB-TICK, where s_scene already holds the previous present's RE-APPLIED carry
     * set, and re-capturing it accumulates the carry set unbounded (2D-UI cloning).
     * The reextract only RE-APPLIES it (below), on every present. */

    /* Restore tick-N's transient .s render-pass scratch (cull records, colour,
     * shaded list, Spyro seg + SXY) so the read-only body feeders below find
     * valid inputs — without it they emit nothing and mobys/Spyro flicker. */
    scratch_bracket_enter(cpu);

    /* Sub-tick keyframe-blend for Spyro: pass the alpha + whether interpolation
     * is active (camera bracketed). The feeder builds a blend-interpolated seg
     * scratch so Spyro's animation is as smooth as its movement. */
    spyro_native_set_anim_interp(alpha, cam_bracketed);
    /* Spyro body/head/tail rotation at float precision (same fix as the mobys):
     * the per-segment float byte-angles computed above feed spyro_compose_euler so
     * Spyro's turn/sway renders below the 8-bit euler grid. Inactive segments
     * (head/tail sticky/transition, or not bracketed) fall back to byte-exact. */
    spyro_native_set_rot_interp(sp_bracketed ? sp_rot_active : NULL, &sp_rot_ang[0][0]);
    /* Flame sub-keyframe slide: the fractional part of the counter advance (the
     * integer part is already written to FL_CNT). The flame feeder lerps each
     * cross-section between adjacent spine records by this, so the ribbon animates
     * below the keyframe grid. 0 when not bracketed (exact keyframes). */
    flame_native_set_subframe(fl_subframe);
    /* Moby rotation: render at float precision (table-interpolated sin/cos over the
     * per-moby prev/curr euler snapshots) for this feeder pass, so slow rotators
     * (gems, idle dragons, HUD icons) don't stair-step on the 1.4° byte grid.
     * Per-object self-gated inside the composer; reset after the feeders so the
     * canonical inline render stays byte-exact. */
    moby_native_set_rot_interp(1);

    /* Rebuild the scene against the (now interpolated) gamestate. scene_begin
     * resets s_scene without bumping the frame id, so the feeders' own
     * begin_frame() calls no-op (no history snapshot / no double-reset). */
    openpete_native_scene_begin();
    uint32_t bg = cpu->read_word(G_CYCLORAMA + 0x10) & 0xFFFFFF;
    openpete_native_scene_set_bg(bg | 0xFF000000u);   /* fallback bg if cyclorama is off */

    /* Re-extract each feeder in canonical scene order. Camera-direct feeders
     * (terrain, Spyro) + the re-derived mobys interpolate via the brackets; sky
     * reproduces tick-N (far backdrop). Every feeder runs → nothing vanishes. */
    const int pt_on = openpete_phase_timers_on();   /* per-feeder phase timing */
    uint64_t pt_t0 = 0, pt_t1;
    transition_native_reextract(cpu);     /* level-transition / balloonist cross-fade backdrop (backmost) */
    cyclorama_native_reextract(cpu);      /* sky dome + background */
    portal_native_reextract(cpu, alpha, cam_bracketed); /* homeworld portal previews (dome matrix lerped prev→curr) */
    if (pt_on) pt_t0 = openpete_phase_now_us();
    terrain_native_reextract(cpu);        /* interpolated camera → smooth terrain */
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_REX_TERRAIN, pt_t1 - pt_t0); pt_t0 = pt_t1; }
    moby_native_reextract(cpu);           /* re-derived: interpolated camera + lerped position */
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_REX_MOBY, pt_t1 - pt_t0); pt_t0 = pt_t1; }
    shadow_native_reextract(cpu);         /* drop-shadows under mobys + Spyro */
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_REX_SHADOW, pt_t1 - pt_t0); }
    /* The moby + shadow feeders have now consumed the FULL regular cull records;
     * overlay the Spyro seg scratch over the (consumed) records 14+ at 0x1900 —
     * the .s's own temporal reuse — so the Spyro body feeder sees its seg scratch
     * without record 14+ mobys (Sparx, distant chests) being clobbered earlier. */
    scratch_bracket_seg_restore(cpu);
    if (pt_on) pt_t0 = openpete_phase_now_us();
    spyro_player_native_reextract(cpu);   /* interpolated camera + lerped position */
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_REX_SPYRO, pt_t1 - pt_t0); }
    particle_native_reextract(cpu);       /* particles / glows / sparkles (Sparx) */
    flame_native_reextract(cpu);          /* fire breath (self-gated on m_IsFlameActive) */
    tracer_native_reextract(cpu);          /* additive tracer/beam (replay snapshot, interp camera) */
    flame_native_set_subframe(0.0);       /* reset: the canonical inline render uses exact keyframes */
    moby_native_set_rot_interp(0);        /* reset: canonical inline render uses byte-exact rotation */
    spyro_native_set_rot_interp(NULL, NULL); /* reset: canonical inline render uses byte-exact rotation */

    /* Re-apply the 2D-UI overlay (letterbox + menu panel held; fade interpolated
     * by the spine alpha) on top of the re-derived 3D scene. */
    openpete_native_scene_apply_overlay(alpha);

    /* Restore the transient render-pass scratch (disjoint from the position
     * bracket regions below — order irrelevant). */
    scratch_bracket_leave(cpu);

    /* Restore canonical RAM before the game fiber resumes (the present below
     * reads only s_scene). Order is irrelevant — the regions are disjoint. */
    for (int i = 0; i < moby_save_n; i++) {
        uint8_t* mh = (uint8_t*)openpete_psx_to_host_ptr(s_moby_save[i].addr);
        if (mh) memcpy(mh, s_moby_save[i].bytes, MOBY_STRIDE);
    }
    for (int i = 0; i < part_save_n; i++) {
        uint8_t* ph = (uint8_t*)openpete_psx_to_host_ptr(s_part_save[i].addr);
        if (ph) { memcpy(ph + PART_POS_OFF, &s_part_save[i].w1, 4);
                  memcpy(ph + 0x08u,        &s_part_save[i].w2, 4); }
    }
    if (ds_bracketed && ds_host) memcpy(ds_host, ds_saved, 0x2C);
    if (sh_bracketed)  memcpy(sh, sh_saved, SHADOW_SAVE);
    if (fl_bracketed)  memcpy(fl, fl_saved, FLAME_SAVE);
    if (sp_bracketed)  memcpy(sp, sp_saved, SPYRO_SAVE);
    if (cam_bracketed) memcpy(cam, cam_saved, CAM_BYTES);

    /* W0.3 bracket-integrity: every lerp write above must be undone — verify the
     * present left gameplay RAM byte-identical to how it found it. A mismatch is
     * a restore bug (the class the headless oracle can't see); log it same-frame.
     * The moby/particle pool checks guard their restore loops against future
     * regressions (a slot dropped from the loop, a stray post-restore write). */
    /* Load-window census (OPENPETE_LOAD_CENSUS=1): one line per re-extract with
     * the inputs that decide what the intermediates rebuild — the canon-drawn
     * mask (which paths the last canonical render actually painted), the
     * camera-ring pairing, and the cut flag. Correlate with the MOBY-RE and
     * SCENE lines to attribute "mobys visible during a level transition". */
    {
        static int s_census = -1;
        if (s_census < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); s_census = (e && *e && *e != '0'); }
        if (s_census) {
            uint32_t m = 0;
            for (int p = 0; p < RP_COUNT; p++) if (render_path_canon_drawn((render_path_t)p)) m |= 1u << p;
            fprintf(stderr, "[census f=%u RE] canon_mask=%02x (cyc|spy|moby|ter|shdw|flm|part|trc) paired=%d cam_cut=%d alpha=%.2f mobys=%d\n",
                    openpete_frame, m, paired, cam_cut, alpha, moby_save_n);
        }
    }
    if (bc_on) {
        for (int i = 0; i < bc_n; ++i) {
            if (bc_hash(bc_regs[i].addr, bc_regs[i].len) != bc_h0[i])
                fprintf(stderr, "[bracket-check f=%u] RESTORE MISMATCH region=%s: "
                        "sub-tick present perturbed gameplay RAM\n",
                        openpete_frame, bc_regs[i].name);
        }
        for (int i = 0; i < moby_save_n; ++i) {
            const uint8_t* mh = (const uint8_t*)openpete_psx_to_host_ptr(s_moby_save[i].addr);
            if (mh && memcmp(mh, s_moby_save[i].bytes, MOBY_STRIDE) != 0)
                fprintf(stderr, "[bracket-check f=%u] RESTORE MISMATCH moby@%08x\n",
                        openpete_frame, s_moby_save[i].addr);
        }
        for (int i = 0; i < part_save_n; ++i) {
            const uint8_t* ph = (const uint8_t*)openpete_psx_to_host_ptr(s_part_save[i].addr);
            if (ph) {
                uint32_t w1, w2;
                memcpy(&w1, ph + PART_POS_OFF, 4);
                memcpy(&w2, ph + 0x08u, 4);
                if (w1 != s_part_save[i].w1 || w2 != s_part_save[i].w2)
                    fprintf(stderr, "[bracket-check f=%u] RESTORE MISMATCH particle@%08x\n",
                            openpete_frame, s_part_save[i].addr);
            }
        }
    }

    s_fpos_active = 0;   /* leave the resolver inert for the canonical render path */

    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("OPENPETE_REEXTRACT_DBG"); dbg = (e && *e && *e != '0'); }
    if (dbg) {
        static unsigned n = 0, last_frame = 0; static double amin = 2.0, amax = -1.0;
        if (cam_bracketed) { if (alpha < amin) amin = alpha; if (alpha > amax) amax = alpha; }
        n++;
        if (openpete_frame != last_frame && (openpete_frame % 120u) == 0) {
            last_frame = openpete_frame;
            fprintf(stderr, "[reextract f=%u] intermediates=%u paired=%d bracketed=%d mobys=%d "
                    "alpha=[%.2f..%.2f] captures=%llu dedupe=%llu\n",
                    openpete_frame, n, paired, cam_bracketed, moby_save_n,
                    amin <= 1.0 ? amin : -1.0, amax,
                    (unsigned long long)openpete_interp_total_captures(),
                    (unsigned long long)openpete_interp_total_dedupe_skips());
            amin = 2.0; amax = -1.0;
        }
    }
}

void spyro_native_reextract_init(void) {
    openpete_native_register_reextract(spyro_native_reextract);
}
