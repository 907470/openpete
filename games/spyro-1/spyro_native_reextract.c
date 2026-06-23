/* spyro_native_reextract.c — Spyro 1's sub-tick re-feed orchestrator.
 * See spyro_native_reextract.h for the contract. */

#include "spyro_native_reextract.h"
#include "openpete_interp.h"        /* snapshot ring query + op_now/OP_NS_PER_SEC */
#include "openpete_interp_objs.h"   /* per-object (moby) prev/curr table */
#include "openpete_native_render.h" /* register callback + scene API */
#include "openpete_psx.h"           /* openpete_psx_to_host_ptr */
#include "psx_runtime.h"            /* CPUState (read_word) */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;

#define G_CAMERA      0x80076DD0u
#define CAM_BYTES     0x110u
#define G_CYCLORAMA   0x80078A40u
#define G_SPYRO       0x80078A58u   /* m_Position @ +0x00 */
#define G_LEVELMOBYS  0x80075828u   /* Moby* → head of the stride-0x58 array */
#define G_HUD_ICONS   0x80077FECu   /* g_Hud.m_Mobys[12] — static 3D HUD icons (spin) */
#define G_HUD_COUNT   12u
#define MOBY_STRIDE   0x58u
#define MOBY_STATE    0x48u
#define MOBY_POS      0x0Cu         /* Vector3D x,y,z (int32) */
#define SPYRO_REGION  1u            /* index in s_spyro_interp_regions (after g_Camera) */
#define SPYRO_SAVE    0x28u         /* g_Spyro bytes bracketed: pos(0x00) + per-seg anim(..0x26) */
#define G_FLAME       0x800786C8u   /* g_SpyroFlame: m_Position @ +0x00, attach matrix @ +0xB8 */
#define FLAME_REGION  2u            /* index in s_spyro_interp_regions (after g_Spyro) */
#define FLAME_SAVE    0xD0u         /* flame bytes bracketed (covers pos + attach @ 0xB8..0xCC) */
#define FLAME_POS_OFF 0x00u         /* m_Position (origin, Vector3D int32) */
#define FLAME_CNT_OFF 0x20u         /* per-strand keyframe counter [8] (u8, += g_DeltaTime/tick) */
#define FLAME_ATT_OFF 0xB8u         /* flame-attach SHORTMATRIX (9 int16 + pad) */
#define G_SHADOW      0x8007AA10u   /* SpyroShadow: ground anchor m_Position @ +0x10 */
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
#define G_PARTICLES     0x80075824u  /* Particle[] base POINTER (deref) */
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

/* Nominal sim-tick period (29.97 Hz) — passed to the ring query only to get
 * the prev/curr pairing + dedupe verdict; the actual alpha is recomputed
 * below from the snapshots' measured timestamps. */
#define TICK_PERIOD_NS  ((op_time_t)((OP_NS_PER_SEC * 1001) / 30000))

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
#define DSONY            0x8006FCF4u          /* g_SonyImage scratch base */
#define DSONY_LO         0x1000u              /* captured span low offset (incl) */
#define DSONY_HI         0x2C00u              /* captured span high offset (excl) */
#define DSONY_SPAN       (DSONY_HI - DSONY_LO)
#define SXY_BASE         0x1F800000u          /* PSX hardware scratchpad (Spyro SXY) */
#define SXY_WORDS        0x100u               /* full 1 KB scratchpad */

/* Captured sub-regions are filled at their producer hooks (colour+records at the
 * moby hook, seg at the Spyro hook, shaded at the shaded hook) into s_canon_dsony
 * by offset; the explicit COLOUR/RECS/SEG/SHADED constants below drive the
 * staged restore. The regular records are ALSO captured in full into s_canon_recs
 * (see the overlap note there). */
static uint8_t  s_canon_dsony[DSONY_SPAN], s_live_dsony[DSONY_SPAN];
static uint32_t s_canon_sxy[SXY_WORDS],    s_live_sxy[SXY_WORDS];
static int      s_canon_scratch_valid = 0;

/* Explicit scratch sub-regions (offsets from DSONY). The REGULAR cull-record
 * region [0x1600,0x2400) and the Spyro per-segment scratch [0x1900,0x1A00)
 * OVERLAP — the .s reuses 0x1900 temporally (the moby pass fills the records
 * there, then the Spyro pass overwrites them with its seg scratch once the
 * records are consumed). The reextract reproduces that ordering: it restores the
 * FULL records for the moby feeder, then overlays the seg scratch for the Spyro
 * feeder (scratch_bracket_seg_restore). The old single-shot restore captured the
 * records only up to 0x1900 and then wrote the seg scratch over records at queue
 * index >=14 (they start at 0x1910), so any moby parked there — Sparx, distant
 * chests — was clobbered and vanished on sub-tick presents. */
#define COLOUR_OFF  0x1100u
#define COLOUR_LEN  0x500u    /* [0x1100,0x1600) per-vertex fog-lit colour scratch */
#define RECS_OFF    0x1600u
#define RECS_LEN    0xE00u    /* [0x1600,0x2400) full REGULAR cull records (≤64 @ 0x38) */
#define SEG_OFF     0x1900u
#define SEG_LEN     0x100u    /* [0x1900,0x1A00) Spyro per-segment scratch (overlaps recs) */
#define SHADED_OFF  0x2400u
#define SHADED_LEN  0x800u    /* [0x2400,0x2C00) shaded Moby* list + lit scratch */
static uint8_t  s_canon_recs[RECS_LEN], s_live_recs[RECS_LEN];
static int      s_canon_recs_valid = 0;

/* Capture a DSONY sub-range at its producer hook (called from the canonical
 * moby/spyro render hooks while the scratch is still live). */
void spyro_native_canon_capture_range(void* cpu_v, unsigned off, unsigned len) {
    (void)cpu_v;
    if (off < DSONY_LO || off + len > DSONY_HI) return;
    const void* h = openpete_psx_to_host_ptr(DSONY + off);
    if (!h) return;
    memcpy(s_canon_dsony + (off - DSONY_LO), h, len);
    s_canon_scratch_valid = 1;
    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("OPENPETE_CANON_CAPDBG"); dbg = (e && *e && *e != '0'); }
    if (dbg && (openpete_frame % 200u) == 0) {
        uint32_t w0; memcpy(&w0, s_canon_dsony + (off - DSONY_LO), 4);
        fprintf(stderr, "[canon-cap f=%u] off=0x%04x len=0x%x word0=%08x\n", openpete_frame, off, len, w0);
    }
}

/* Capture Spyro's projected SXY scratchpad at the canonical spyro hook. */
void spyro_native_canon_capture_sxy(void* cpu_v) {
    CPUState* cpu = (CPUState*)cpu_v;
    for (uint32_t i = 0; i < SXY_WORDS; ++i) s_canon_sxy[i] = cpu->read_word(SXY_BASE + i * 4u);
    s_canon_scratch_valid = 1;
}

/* Capture the FULL regular cull-record region [0x1600,0x2400) at the MOBY hook —
 * while the records are still live, BEFORE the later Spyro pass reuses 0x1900 for
 * its seg scratch. Stored separately from s_canon_dsony (whose seg sub-range is
 * captured later at the Spyro hook and would otherwise overwrite records 14+).
 * Restored for the moby feeder by scratch_bracket_enter. */
void spyro_native_canon_capture_recs(void* cpu_v) {
    (void)cpu_v;
    const void* h = openpete_psx_to_host_ptr(DSONY + RECS_OFF);
    if (!h) return;
    memcpy(s_canon_recs, h, RECS_LEN);
    s_canon_recs_valid = 1;
}

/* On an intermediate: save the live scratch, overwrite with the captured canon,
 * so the read-only feeders that follow see valid tick-N render-pass inputs.
 *
 * The seg scratch [0x1900,0x1A00) is NOT restored here — it overlaps the regular
 * records (records 14+ start at 0x1910) which the .s reuses temporally. We give
 * the moby feeder the FULL records first; scratch_bracket_seg_restore then
 * overlays the seg scratch for the Spyro feeder (called between them). */
static void scratch_bracket_enter(CPUState* cpu) {
    if (!s_canon_scratch_valid) return;
    void* hc = openpete_psx_to_host_ptr(DSONY + COLOUR_OFF);
    void* hr = openpete_psx_to_host_ptr(DSONY + RECS_OFF);
    void* hs = openpete_psx_to_host_ptr(DSONY + SHADED_OFF);
    /* Save TRUE live (before any restore). The records region [0x1600,0x2400)
     * spans the seg sub-range, so this one save/restore covers the seg too. */
    if (hc) memcpy(s_live_dsony + (COLOUR_OFF - DSONY_LO), hc, COLOUR_LEN);
    if (hr) memcpy(s_live_recs, hr, RECS_LEN);
    if (hs) memcpy(s_live_dsony + (SHADED_OFF - DSONY_LO), hs, SHADED_LEN);
    /* Restore canon for the MOBY + shadow feeders: colour, FULL records, shaded. */
    if (hc) memcpy(hc, s_canon_dsony + (COLOUR_OFF - DSONY_LO), COLOUR_LEN);
    if (hr && s_canon_recs_valid) memcpy(hr, s_canon_recs, RECS_LEN);
    if (hs) memcpy(hs, s_canon_dsony + (SHADED_OFF - DSONY_LO), SHADED_LEN);
    for (uint32_t i = 0; i < SXY_WORDS; ++i) {
        s_live_sxy[i] = cpu->read_word(SXY_BASE + i * 4u);
        cpu->write_word(SXY_BASE + i * 4u, s_canon_sxy[i]);
    }
}

/* After the moby + shadow feeders have consumed the regular records, overlay the
 * Spyro per-segment scratch over the (now-consumed) records 14+ at 0x1900 — the
 * same temporal reuse the .s does — so the Spyro body feeder sees its seg scratch. */
static void scratch_bracket_seg_restore(CPUState* cpu) {
    (void)cpu;
    if (!s_canon_scratch_valid) return;
    void* h = openpete_psx_to_host_ptr(DSONY + SEG_OFF);
    if (h) memcpy(h, s_canon_dsony + (SEG_OFF - DSONY_LO), SEG_LEN);
}

/* Restore the live scratch before the game fiber resumes (determinism). The
 * records-region restore [0x1600,0x2400) covers the seg sub-range too. */
static void scratch_bracket_leave(CPUState* cpu) {
    if (!s_canon_scratch_valid) return;
    void* hc = openpete_psx_to_host_ptr(DSONY + COLOUR_OFF);
    void* hr = openpete_psx_to_host_ptr(DSONY + RECS_OFF);
    void* hs = openpete_psx_to_host_ptr(DSONY + SHADED_OFF);
    if (hc) memcpy(hc, s_live_dsony + (COLOUR_OFF - DSONY_LO), COLOUR_LEN);
    if (hr) memcpy(hr, s_live_recs, RECS_LEN);
    if (hs) memcpy(hs, s_live_dsony + (SHADED_OFF - DSONY_LO), SHADED_LEN);
    for (uint32_t i = 0; i < SXY_WORDS; ++i)
        cpu->write_word(SXY_BASE + i * 4u, s_live_sxy[i]);
}

static void spyro_native_reextract(void* cpu_v) {
    CPUState* cpu = (CPUState*)cpu_v;

    /* Pair prev/curr camera snapshots (region 0 = g_Camera). Returns 0 when
     * the world is stuck (byte-identical: paused / loading / vbl busy-wait)
     * or no pairing yet (spawn) — then we present the live (curr) scene with
     * no camera bracket. */
    double qa = 0.0;
    const openpete_interp_snapshot_t *prev = NULL, *curr = NULL;
    int paired = openpete_interp_query(op_now(), TICK_PERIOD_NS, &qa, &prev, &curr);

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
    }

    if (paired && cam && prev && curr && !cam_cut) {
        /* 1-tick-lag alpha: at the instant tick N completed (now≈curr.tick_ns)
         * show prev (N-1); advance to curr (N) over the next sim period. The
         * forward endpoint is the KNOWN canonical N → no extrapolation. */
        double period = (double)(curr->tick_ns - prev->tick_ns);
        alpha = (period > 0.0) ? (double)(op_now() - curr->tick_ns) / period : 1.0;
        if (alpha < 0.0) alpha = 0.0; else if (alpha > 1.0) alpha = 1.0;

        /* (a) Camera: Q12 matrices + position (singleton, region 0). */
        const uint8_t* pcam = prev->data + prev->region_offsets[0];
        memcpy(cam_saved, cam, CAM_BYTES);        /* curr (live) */
        lerp_matrix(cam, pcam, cam_saved, CAM_OFF_PROJ, alpha);
        lerp_matrix(cam, pcam, cam_saved, CAM_OFF_VIEW, alpha);
        lerp_vec3  (cam, pcam, cam_saved, CAM_OFF_POS,  alpha);
        cam_bracketed = 1;
        /* Fractional camera position (un-rounded) for the floor fix — same lerp,
         * double precision. prev = pcam+POS, curr = cam_saved (the live curr we
         * saved before the lerp overwrote RAM). */
        fpos_set(G_CAMERA,
                 (const int32_t*)(pcam      + CAM_OFF_POS),
                 (const int32_t*)(cam_saved + CAM_OFF_POS), alpha);

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
            lerp_vec3(sp, psp, sp_saved, 0, alpha);    /* +0x00 m_Position */
            /* Fractional Spyro position (un-rounded) for the floor fix. */
            fpos_set(G_SPYRO, (const int32_t*)psp, (const int32_t*)sp_saved, alpha);
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
        fl = (uint8_t*)openpete_psx_to_host_ptr(G_FLAME);
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
        sh = (uint8_t*)openpete_psx_to_host_ptr(G_SHADOW);
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
                        /* Per-object alpha from the moby's OWN distinct-roll
                         * timestamps (sweeps across the static ticks between
                         * distinct states; clamps to curr once it settles). */
                        op_time_t period_ns = o->curr_ns - o->prev_ns;
                        if (o->prev_ns != 0 && period_ns > 0) {
                            double ma = (double)(now - o->curr_ns) / (double)period_ns;
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
                            for (int k = 0; k < 3; k++) {
                                int32_t lv = lerp32(pp[k], cp[k], ma);
                                memcpy(mh + MOBY_POS + k * 4, &lv, 4);
                            }
                            /* Fractional moby position (un-rounded) for the floor
                             * fix — keyed by the moby vaddr, at this moby's own
                             * per-object alpha. The body / shaded / shadow feeders
                             * look it up so the moby's own motion is continuous too
                             * (not just the camera component). */
                            fpos_set(m, pp, cp, ma);
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
            double ha = (double)(now - o->curr_ns) / (double)period;
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
                if (!o || !o->have_prev) continue;  /* spawn → snap (curr) */
                op_time_t period = o->curr_ns - o->prev_ns;
                if (o->prev_ns == 0 || period <= 0) continue;
                double pa_alpha = (double)(now - o->curr_ns) / (double)period;
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
                if (dX*dX + dY*dY + dZ*dZ > 1250.0 * 1250.0) continue;
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
    cyclorama_native_reextract(cpu);      /* sky dome + background */
    portal_native_reextract(cpu, alpha, cam_bracketed); /* homeworld portal previews (dome matrix lerped prev→curr) */
    terrain_native_reextract(cpu);        /* interpolated camera → smooth terrain */
    moby_native_reextract(cpu);           /* re-derived: interpolated camera + lerped position */
    shadow_native_reextract(cpu);         /* drop-shadows under mobys + Spyro */
    /* The moby + shadow feeders have now consumed the FULL regular cull records;
     * overlay the Spyro seg scratch over the (consumed) records 14+ at 0x1900 —
     * the .s's own temporal reuse — so the Spyro body feeder sees its seg scratch
     * without record 14+ mobys (Sparx, distant chests) being clobbered earlier. */
    scratch_bracket_seg_restore(cpu);
    spyro_player_native_reextract(cpu);   /* interpolated camera + lerped position */
    particle_native_reextract(cpu);       /* particles / glows / sparkles (Sparx) */
    flame_native_reextract(cpu);          /* fire breath (self-gated on m_IsFlameActive) */
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
    if (sh_bracketed)  memcpy(sh, sh_saved, SHADOW_SAVE);
    if (fl_bracketed)  memcpy(fl, fl_saved, FLAME_SAVE);
    if (sp_bracketed)  memcpy(sp, sp_saved, SPYRO_SAVE);
    if (cam_bracketed) memcpy(cam, cam_saved, CAM_BYTES);

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
