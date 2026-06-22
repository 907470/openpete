/* spyro_native_reextract.c — Spyro 1's sub-tick re-feed orchestrator.
 * See spyro_native_reextract.h for the contract. */

#include "spyro_native_reextract.h"
#include "openpete_interp.h"        /* snapshot ring query + op_now/OP_NS_PER_SEC */
#include "openpete_interp_objs.h"   /* per-object (moby) prev/curr table */
#include "openpete_native_render.h" /* register callback + scene API */
#include "openpete_psx.h"           /* openpete_psx_to_host_ptr */
#include "psx_runtime.h"            /* CPUState (read_word) */

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

/* Per-moby RAM bracket save slots — restored after re-extract so the next sim
 * tick reads canonical RAM. Sized generously for any level's live moby count. */
#define MOBY_SAVE_MAX 1024
typedef struct { uint32_t addr; uint8_t bytes[MOBY_STRIDE]; } moby_save_t;

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

/* Lerp a packed 3×8-bit euler word (Spyro rotation: Z=bits0-7, X=8-15,
 * Y=16-23 — see spyro_compose_euler). Shortest-arc per byte (256/circle);
 * byte 3 preserved from curr. */
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

/* Captured sub-ranges (offset from DSONY, length). Disjoint; each filled at its
 * producer hook. Records sit in [0x1100,0x1900) with the colour scratch; the
 * seg scratch is 0x1900; the shaded list is 0x2400. */
static const struct { uint32_t off, len; } s_canon_ranges[] = {
    { 0x1100u, 0x800u },   /* colour scratch + REGULAR cull records (moby regular hook) */
    { 0x1900u, 0x100u },   /* Spyro per-segment scratch (spyro hook) */
    { 0x2400u, 0x800u },   /* shaded Moby* list + lit scratch (shaded hook) */
};
static uint8_t  s_canon_dsony[DSONY_SPAN], s_live_dsony[DSONY_SPAN];
static uint32_t s_canon_sxy[SXY_WORDS],    s_live_sxy[SXY_WORDS];
static int      s_canon_scratch_valid = 0;

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

/* On an intermediate: save the live scratch, overwrite with the captured canon,
 * so the read-only feeders that follow see valid tick-N render-pass inputs. */
static void scratch_bracket_enter(CPUState* cpu) {
    if (!s_canon_scratch_valid) return;
    for (size_t r = 0; r < sizeof(s_canon_ranges)/sizeof(s_canon_ranges[0]); ++r) {
        uint32_t off = s_canon_ranges[r].off, len = s_canon_ranges[r].len;
        void* h = openpete_psx_to_host_ptr(DSONY + off);
        if (!h) continue;
        memcpy(s_live_dsony + (off - DSONY_LO), h, len);            /* save live */
        memcpy(h, s_canon_dsony + (off - DSONY_LO), len);           /* write canon */
    }
    for (uint32_t i = 0; i < SXY_WORDS; ++i) {
        s_live_sxy[i] = cpu->read_word(SXY_BASE + i * 4u);
        cpu->write_word(SXY_BASE + i * 4u, s_canon_sxy[i]);
    }
}

/* Restore the live scratch before the game fiber resumes (determinism). */
static void scratch_bracket_leave(CPUState* cpu) {
    if (!s_canon_scratch_valid) return;
    for (size_t r = 0; r < sizeof(s_canon_ranges)/sizeof(s_canon_ranges[0]); ++r) {
        uint32_t off = s_canon_ranges[r].off, len = s_canon_ranges[r].len;
        void* h = openpete_psx_to_host_ptr(DSONY + off);
        if (h) memcpy(h, s_live_dsony + (off - DSONY_LO), len);
    }
    for (uint32_t i = 0; i < SXY_WORDS; ++i)
        cpu->write_word(SXY_BASE + i * 4u, s_live_sxy[i]);
}

static uint32_t lerp_euler8x3(uint32_t prev, uint32_t curr, double t) {
    uint32_t out = curr & 0xFF000000u;
    for (int b = 0; b < 3; ++b) {
        int pv = (int)((prev >> (b * 8)) & 0xFF), cv = (int)((curr >> (b * 8)) & 0xFF);
        int d = cv - pv;
        if (d > 128) d -= 256; else if (d < -128) d += 256;
        int r = pv + (int)((double)d * t + (d >= 0 ? 0.5 : -0.5));
        out |= (uint32_t)(r & 0xFF) << (b * 8);
    }
    return out;
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

    /* Bracket targets (all written then restored on the host fiber). */
    uint8_t* cam = (uint8_t*)openpete_psx_to_host_ptr(G_CAMERA);
    uint8_t  cam_saved[CAM_BYTES];
    int      cam_bracketed = 0;
    uint8_t* sp = NULL; uint8_t sp_saved[SPYRO_SAVE]; int sp_bracketed = 0;
    static moby_save_t s_moby_save[MOBY_SAVE_MAX];
    int      moby_save_n = 0;
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
            /* Body/head/tail rotation (packed 3×8-bit euler @ +0xC/+0x10/+0x14).
             * Body always lerps; head/tail are a 0-sentinel ("use body") so lerp
             * only when both endpoints are real (nonzero) — else snap. */
            uint32_t pb, cb;
            memcpy(&pb, psp + 0x0C, 4); memcpy(&cb, sp_saved + 0x0C, 4);
            { uint32_t lv = lerp_euler8x3(pb, cb, alpha); memcpy(sp + 0x0C, &lv, 4); }
            for (int r = 0x10; r <= 0x14; r += 4) {
                uint32_t pr, cr; memcpy(&pr, psp + r, 4); memcpy(&cr, sp_saved + r, 4);
                if (pr != 0 && cr != 0) { uint32_t lv = lerp_euler8x3(pr, cr, alpha); memcpy(sp + r, &lv, 4); }
            }
            sp_bracketed = 1;
        }

        /* (c) Per-moby position (indirect table, keyed by Moby vaddr). Walk the
         * live moby array, lerp each paired moby's m_Position from its prev/curr
         * snapshot; build_moby_record_fields then reads the interpolated value. */
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
                            int32_t lv = lerp32(pp[k], cp[k], alpha);
                            memcpy(mh + MOBY_POS + k * 4, &lv, 4);
                        }
                        /* Animation clock — sub-tick keyframe blend (so the
                         * animation is as smooth as the movement). m_FrameProgress
                         * (+0x40) is a 0..64 phase within the (m_Frame +0x3E,
                         * m_NextFrame +0x3F) keyframe pair; the decode consumes it
                         * as blend = prog<<6 (0..4096). Work in those 0..4096
                         * blend units (wrap = 4096 at a keyframe knot):
                         *  - same pair → lerp the blend.
                         *  - clean single advance ((A,B)→(B,C), i.e. prev's
                         *    m_NextFrame == curr's m_Frame) → continuous phase
                         *    across the knot: pick the prev pair while phase<4096,
                         *    else the curr pair (already live), so the mesh morphs
                         *    smoothly THROUGH the keyframe instead of snapping.
                         *  - anything else (multi-advance / non-consecutive /
                         *    loop) → leave curr (snap). */
                        {
                            int frA_p = o->prev[0x3E], frB_p = o->prev[0x3F];
                            int frA_c = o->curr[0x3E], frB_c = o->curr[0x3F];
                            double bp = (double)(o->prev[0x40] << 6);   /* prev blend 0..4096 */
                            double bc = (double)(o->curr[0x40] << 6);   /* curr blend 0..4096 */
                            /* Animation-switch guard. m_Frame/m_NextFrame are indices
                             * into m_Animation's keyframe table; across an m_Animation
                             * change (a loop that reseeds a NEW clip) the indices are
                             * not comparable. The clip's last frame holds m_NextFrame=0,
                             * and the new clip starts at m_Frame=0 — so frB_p==frA_c
                             * (0==0) spuriously matches the ADV "clean advance" path and
                             * holds the OLD clip's m_Frame (e.g. 59) while the LIVE
                             * m_Animation (read by build_moby_record_fields) is the NEW
                             * clip → frame 59 indexes past the new clip's table into a
                             * garbage keyframe = the warped mesh the cutscene mobys flash
                             * at the loop seam. Snap (leave curr live) across the switch. */
                            if (o->prev[0x3C] != o->curr[0x3C]) {
                                /* snap: keep curr's live m_Frame/m_NextFrame/blend */
                            } else if (frA_p == frA_c && frB_p == frB_c) {
                                int bl = (int)(bp + (bc - bp) * alpha + 0.5) >> 6;
                                if (bl < 0) bl = 0; else if (bl > 64) bl = 64;
                                mh[0x40] = (uint8_t)bl;
                            } else if (frB_p == frA_c) {
                                double phase = bp + ((4096.0 + bc) - bp) * alpha;
                                if (phase < 4096.0) {        /* still finishing the prev pair */
                                    mh[0x3E] = (uint8_t)frA_p; mh[0x3F] = (uint8_t)frB_p;
                                    int bl = (int)(phase + 0.5) >> 6; if (bl > 64) bl = 64;
                                    mh[0x40] = (uint8_t)bl;
                                } else {                     /* into the curr pair (already live) */
                                    int bl = (int)((phase - 4096.0) + 0.5) >> 6;
                                    if (bl < 0) bl = 0; else if (bl > 64) bl = 64;
                                    mh[0x40] = (uint8_t)bl;
                                }
                            }
                        }
                        /* Rotation: shortest-arc lerp of m_Rotation (+0x44,
                         * Vector3D8 = 3 × 8-bit angle, 256/circle) — the gamestate
                         * resolution is 8-bit, so build_moby_record_fields reads
                         * (and composes) the byte euler; wrap each axis to the
                         * short way round. */
                        for (int k = 0; k < 3; k++) {
                            int d = (int)o->curr[0x44 + k] - (int)o->prev[0x44 + k];
                            if (d > 128) d -= 256; else if (d < -128) d += 256;
                            int r = (int)o->prev[0x44 + k] + (int)((double)d * alpha + (d >= 0 ? 0.5 : -0.5));
                            mh[0x44 + k] = (uint8_t)(r & 0xFF);
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
            /* Rotation (m_Rotation +0x44, shortest-arc per 8-bit axis). */
            for (int k = 0; k < 3; k++) {
                int d = (int)o->curr[0x44 + k] - (int)o->prev[0x44 + k];
                if (d > 128) d -= 256; else if (d < -128) d += 256;
                int r = (int)o->prev[0x44 + k] + (int)((double)d * ha + (d >= 0 ? 0.5 : -0.5));
                mh[0x44 + k] = (uint8_t)(r & 0xFF);
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
    terrain_native_reextract(cpu);        /* interpolated camera → smooth terrain */
    moby_native_reextract(cpu);           /* re-derived: interpolated camera + lerped position */
    shadow_native_reextract(cpu);         /* drop-shadows under mobys + Spyro */
    spyro_player_native_reextract(cpu);   /* interpolated camera + lerped position */
    particle_native_reextract(cpu);       /* particles / glows / sparkles (Sparx) */
    flame_native_reextract(cpu);          /* fire breath (self-gated on m_IsFlameActive) */

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
    if (sp_bracketed)  memcpy(sp, sp_saved, SPYRO_SAVE);
    if (cam_bracketed) memcpy(cam, cam_saved, CAM_BYTES);

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
