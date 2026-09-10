// games/spyro-1/features/spyro_native.c — Phase 3b.4 of
// notes/plans/openpete-native-renderer.md: the SPYRO (player-avatar)
// renderer re-derived from gamestate in float + its numerical A/B harness.
//
// Spyro (func_80023AC4 / r_pete.s) is a self-contained ~2 KLOC handwritten GTE
// routine, distinct from the moby path in three ways the format-spike doc
// calls out:
//   1. DELTA-ACCUMULATED vertices — each vertex is a signed (dx,dy,dz) added to
//      a running accumulator, dual 1-byte / 2-byte encoding with a wide escape
//      (NOT moby's random-access packed XYZ).
//   2. THREE skeletal segments (body/head/tail), each its own view×rotation
//      matrix + seam trig.
//   3. Progress is u8 0..255 (NOT moby's 0..4096).
//
// The doc carries several UNCERTAINs (exact 2-byte field widths, the IR0
// progress scale, which LUT carries colour) that it explicitly says to pin by
// live A/B, not by transcribing the atan2-style asm. So this module starts as a
// CAPTURE + ONE-SHOT DUMP harness: hook the renderer, bracket the polybuf
// cursor, capture the prims it emits as ground truth, and dump g_Spyro state +
// the model/animation headers + the vertex streams + the prim packets. The
// float decoder is built and A/B-verified against this ground truth before any
// scene feed.
//
// Determinism: never writes PSX RAM. Calls the real renderer exactly once
// (behaviour-identical to the normal dispatch fall-through) and only READS
// state afterwards. Gated by OPENPETE_SPYRO_VERIFY (dump/A/B) or a native
// renderer mode (--renderer vk; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "psx_runtime_internal.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "openpete_native_render.h"
#include "openpete_log.h"           /* OPL_ERROR_F — ABR/blend FIXME runtime alarms */
#include "openpete_psx.h"
#include "openpete_subtick_view.h"            /* openpete_psx_to_host_ptr (seg-scratch mix) */
#include "openpete_mods.h"           /* openpete_mods_active (actor-pose snapshot) */
#include "spyro_native_reextract.h"  /* canon-scratch capture (sub-tick spine) */
#include "spyro1_actor_pose.h"       /* mod actor render service pose blob (rung 16) */
#include "s3_actor_native.h"         /* kind 1: Spyro 3 player model (M12) */
#include "native_gte.h"    /* shared GTE float-mirror math (W2.3) */
#include "render_paths.h"  /* --render-paths isolation */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE — fiber-hook cost split */

extern void     func_80023AC4(CPUState* cpu);  /* the real SPYRO renderer */
extern unsigned openpete_frame;

/* ---- gamestate addresses (psx.map) --------------------------------------- */

/* ---- captured prims (ground truth) --------------------------------------- */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (prim[0x7])  */
    uint8_t  len;           /* OT tag length (prim[0]>>24)     */
    int      nv;            /* 3 or 4                          */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad;
    uint32_t raw[20];
    int      nraw;
} prim_t;

#define MAX_PRIMS 8192
static prim_t s_gte[MAX_PRIMS]; static int s_gte_n;
static int s_code_hist[256];

/* Walk the polybuf [start,end) by OT tag length (stride=((w0>>24)+1)*4); decode
 * each prim by its code byte (prim[0x7] == word[1]>>24) with the general PSX
 * polygon decode (bit4 gouraud / bit3 quad / bit2 textured). Same scheme the
 * moby harness validated — Spyro emits the same POLY_* family (F3/FT3/G3/GT3,
 * see the format notes). */
static void capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    OP_PROF_ACCUM_SCOPE("spyro.capture");
    s_gte_n = 0;
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < MAX_PRIMS) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t len = (w0 >> 24) & 0xFF;
        if (len == 0 || len > 24) break;          /* not a tag / corrupt — stop */
        uint32_t stride = (len + 1) * 4;
        uint8_t code = (uint8_t)(cpu->read_word(p + 4) >> 24);
        if (s_gte_n < MAX_PRIMS) {
            prim_t* t = &s_gte[s_gte_n];
            memset(t, 0, sizeof(*t));
            t->code = code; t->len = (uint8_t)len;
            t->nraw = (int)(stride / 4); if (t->nraw > 20) t->nraw = 20;
            for (int i = 0; i < t->nraw; i++) t->raw[i] = cpu->read_word(p + (uint32_t)i*4);
            if ((code & 0xE0) == 0x20) {
                int gouraud = (code & 0x10) != 0;
                int quad    = (code & 0x08) != 0;
                int tex     = (code & 0x04) != 0;
                t->gouraud = gouraud; t->textured = tex; t->quad = quad;
                t->nv = quad ? 4 : 3;
                uint32_t rgb0 = t->raw[1] & 0xFFFFFF;
                int c = 2;
                for (int k = 0; k < t->nv; k++) {
                    t->rgb[k] = (gouraud && k > 0) ? (t->raw[c++] & 0xFFFFFF) : rgb0;
                    uint32_t xy = t->raw[c++];
                    t->x[k] = (float)(int16_t)(xy & 0xFFFF);
                    t->y[k] = (float)(int16_t)(xy >> 16);
                    if (tex) {
                        uint32_t uv = t->raw[c++];
                        t->u[k] = (int)(uv & 0xFF); t->v[k] = (int)((uv >> 8) & 0xFF);
                        if (k == 0)      t->clut  = (int)(uv >> 16);
                        else if (k == 1) t->tpage = (int)(uv >> 16);
                    }
                }
            }
            s_code_hist[code & 0xFF]++;
            s_gte_n++;
        }
        p += stride;
    }
}

/* ---- one-shot dump (falsify-before-ship): pin the format notes vs live ---- */
static void dump_words(CPUState* cpu, const char* label, uint32_t base, int nwords) {
    fprintf(stderr, "  %s @%08X:\n   ", label, base);
    for (int i = 0; i < nwords; i++) {
        fprintf(stderr, "%08X ", cpu->read_word(base + (uint32_t)i*4));
        if ((i & 7) == 7 && i != nwords - 1) fprintf(stderr, "\n   ");
    }
    fprintf(stderr, "\n");
}

static const char* k_seg_name[3] = { "body", "head", "tail" };

static void spyro_dump(CPUState* cpu, uint32_t startp, uint32_t endp) {
    fprintf(stderr, "\n[spyro-dump f=%u] prims [%08X,%08X) %u bytes  OFX=%d OFY=%d H=%u\n",
            openpete_frame, startp, endp, endp - startp,
            (int32_t)cpu->gte_ctrl[24] >> 16, (int32_t)cpu->gte_ctrl[25] >> 16,
            cpu->gte_ctrl[26] & 0xFFFF);

    /* g_Spyro: position, anim/frame/progress bytes, colorFilter, flag, rotmat. */
    dump_words(cpu, "g_Spyro[0x00..0x50]", G_SPYRO, 0x14);
    fprintf(stderr,
        "  anim:  body=%02X/%02X head=%02X/%02X tail=%02X/%02X  (cur/next)\n"
        "  kf:    body A=%02X B=%02X  head A=%02X B=%02X  tail A=%02X B=%02X\n"
        "  prog:  body=%02X head=%02X tail=%02X  sortDepth=%02X\n"
        "  colorFilter@0x28=%08X  flag@0x30=%08X\n",
        cpu->read_byte(G_SPYRO+0x18), cpu->read_byte(G_SPYRO+0x19),
        cpu->read_byte(G_SPYRO+0x1A), cpu->read_byte(G_SPYRO+0x1B),
        cpu->read_byte(G_SPYRO+0x1C), cpu->read_byte(G_SPYRO+0x1D),
        cpu->read_byte(G_SPYRO+0x1E), cpu->read_byte(G_SPYRO+0x1F),
        cpu->read_byte(G_SPYRO+0x20), cpu->read_byte(G_SPYRO+0x21),
        cpu->read_byte(G_SPYRO+0x22), cpu->read_byte(G_SPYRO+0x23),
        cpu->read_byte(G_SPYRO+0x24), cpu->read_byte(G_SPYRO+0x25),
        cpu->read_byte(G_SPYRO+0x26), cpu->read_byte(G_SPYRO+0x27),
        cpu->read_word(G_SPYRO+0x28), cpu->read_word(G_SPYRO+0x30));

    /* SPYRO_MODEL = g_Models[0] | KUSEG. */
    uint32_t model = cpu->read_word(G_MODELS) | 0x80000000u;
    fprintf(stderr, "  SPYRO_MODEL=%08X  numAnims=%u\n", model, cpu->read_word(model) & 0xFFFF);
    dump_words(cpu, "model[0x00..0x40]", model, 0x10);

    /* Per-segment: anim header + frame A/B SpyroAnimationFrame + vertex stream. */
    static const int seg_anim_off[3] = { 0x18, 0x1A, 0x1C };  /* g_Spyro anim idx byte */
    static const int seg_frA_off[3]  = { 0x1E, 0x20, 0x22 };  /* keyframe A byte        */
    static const int seg_frB_off[3]  = { 0x1F, 0x21, 0x23 };  /* keyframe B byte        */
    static const int seg_prog_off[3] = { 0x24, 0x25, 0x26 };
    for (int s = 0; s < 3; s++) {
        uint32_t animIdx = cpu->read_byte(G_SPYRO + (uint32_t)seg_anim_off[s]);
        uint32_t frA     = cpu->read_byte(G_SPYRO + (uint32_t)seg_frA_off[s]);
        uint32_t frB     = cpu->read_byte(G_SPYRO + (uint32_t)seg_frB_off[s]);
        uint32_t prog    = cpu->read_byte(G_SPYRO + (uint32_t)seg_prog_off[s]);
        uint32_t animHdr = cpu->read_word(model + 0x38 + animIdx * 4);
        fprintf(stderr, "  --- seg %d (%s): animIdx=%u frA=%u frB=%u prog=%u animHdr=%08X\n",
                s, k_seg_name[s], animIdx, frA, frB, prog, animHdr);
        if (animHdr < 0x80000000u || animHdr >= 0x80800000u) { fprintf(stderr, "    (bad hdr ptr)\n"); continue; }
        dump_words(cpu, "animHdr[0x00..0x28]", animHdr, 0x0A);
        uint32_t numFrames = cpu->read_word(animHdr) & 0xFFFF;
        uint32_t vbase = cpu->read_word(animHdr + 0x10);  /* m_AnimationVertices */
        fprintf(stderr, "    numFrames=%u vertCntHi=%u vertCntLo=%u shift=%u depthScale=%u "
                "isSpyroAnim=%u scale=%u  m_AnimationVertices=%08X faces=%08X colors=%08X\n",
                numFrames,
                cpu->read_byte(animHdr + 0x08), cpu->read_byte(animHdr + 0x09),
                cpu->read_byte(animHdr + 0x06), cpu->read_byte(animHdr + 0x0B),
                cpu->read_byte(animHdr + 0x04), cpu->read_byte(animHdr + 0x05),
                vbase, cpu->read_word(animHdr + 0x14), cpu->read_word(animHdr + 0x18));
        /* SpyroAnimationFrame for A/B — try BOTH strides (4B and 8B) so the dump
         * resolves the doc's "Frames[0] 8B but lw 0x24(hdr+frame*4)" ambiguity. */
        fprintf(stderr, "    frame words @0x24 stride4: A=%08X B=%08X | stride8: A=%08X,%08X B=%08X,%08X\n",
                cpu->read_word(animHdr + 0x24 + frA * 4), cpu->read_word(animHdr + 0x24 + frB * 4),
                cpu->read_word(animHdr + 0x24 + frA * 8), cpu->read_word(animHdr + 0x24 + frA * 8 + 4),
                cpu->read_word(animHdr + 0x24 + frB * 8), cpu->read_word(animHdr + 0x24 + frB * 8 + 4));
        if (vbase >= 0x80000000u && vbase < 0x80800000u)
            dump_words(cpu, "m_AnimationVertices[0x00..0x20]", vbase, 0x08);
        /* Face stream (m_Faces @ animHdr+0x14): word0 = byte length, records
         * start at +4. Dump the length + first ~24 words so the walk's stride /
         * index decode can be hand-verified against the captured prims. */
        uint32_t faces = cpu->read_word(animHdr + 0x14);
        if (faces >= 0x80000000u && faces < 0x80800000u) {
            uint32_t flen = cpu->read_word(faces);
            fprintf(stderr, "    m_Faces=%08X len=%u (records @+4)\n", faces, flen);
            dump_words(cpu, "faces[0x00..0x60]", faces, 0x18);
        }
    }

    /* Live GTE rotation matrix (post-call) vs each segment's +0x10 slot — which
     * matrix does the body actually use? (R11R12/R13R21/R22R23/R31R32/R33.) */
    fprintf(stderr, "  gte_ctrl R[0..4] (post-call): %08X %08X %08X %08X %08X\n",
            cpu->gte_ctrl[0], cpu->gte_ctrl[1], cpu->gte_ctrl[2], cpu->gte_ctrl[3], cpu->gte_ctrl[4]);

    /* Segment matrix scratch block (G_SCRATCH+0x1900, stride 0x44). */
    for (int s = 0; s < 3; s++) {
        uint32_t e = G_SCRATCH + 0x1900u + (uint32_t)s * 0x44u;
        fprintf(stderr, "  seg-scratch[%d] @%08X:\n   ", s, e);
        for (int w = 0; w < 0x11; w++) fprintf(stderr, "%08X ", cpu->read_word(e + (uint32_t)w*4));
        fprintf(stderr, "\n");
    }

    /* Prim histogram + first prims. */
    char hb[300]; int hp = 0; hb[0] = 0;
    for (int c = 0; c < 256; c++) if (s_code_hist[c] && hp < (int)sizeof(hb)-16)
        hp += snprintf(hb+hp, sizeof(hb)-hp, " %02X=%d", c, s_code_hist[c]);
    fprintf(stderr, "  prim codes:%s  (total=%d)\n", hb, s_gte_n);
    int show = s_gte_n < 4 ? s_gte_n : 4;
    for (int i = 0; i < show; i++) {
        prim_t* t = &s_gte[i];
        fprintf(stderr, "   prim[%d] code=%02X len=%u nv=%d g=%d t=%d q=%d  raw:",
                i, t->code, t->len, t->nv, t->gouraud, t->textured, t->quad);
        for (int w = 0; w < t->nraw; w++) fprintf(stderr, " %08X", t->raw[w]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[spyro-dump] end\n\n");
}

/* ======================================================================== *
 *  NATIVE VERTEX DECODE + PROJECTION (the format notes)                *
 * ======================================================================== *
 * Spyro's geometry is delta-accumulated, dual-width, two-keyframe-blended.
 * Phase A (the .s) pre-builds, per segment, a scratch entry at
 * G_SCRATCH+0x1900 + seg*0x44:
 *   +0x00 frameData  (sign=matrix-present, 0x1000=A 2-byte, 0x2000=B 2-byte,
 *                     [11:0]=INTPL blend (1.12), [23:14]=vertex count)
 *   +0x04..0x0C TRX/TRY/TRZ          +0x10..0x20 R11R12..R33 (SHORTMATRIX)
 *   +0x24/0x28/0x2C  A: animVtx-word / 1-byte / 2-byte cursors
 *   +0x34/0x38/0x3C  B: same for the next keyframe
 * The matrix is sticky: only reloaded when frameData<0 (else reuse previous
 * segment's). We read these GTE inputs straight from the scratch block (the
 * apples-to-apples analog of moby 3a.1 reading the cull record) and reproduce
 * the running-accumulator decode + INTPL lerp + RTPS in float. Re-deriving the
 * segment matrices from gamestate (Phase A) is a later step. */

#define SEG_SCRATCH (G_SCRATCH + 0x1900u)
#define SEG_STRIDE  0x44u
#define SEG_BLOCK   (3u * SEG_STRIDE)   /* 3 segments (body/head/tail) */

/* --- sub-tick keyframe-blend interpolation (animation smoothness) -----------
 * Spyro's decode (spyro_project_segment) blends keyframe A→B by
 * blend = (seg.fd & 0xFFF)/4096, read from the per-segment scratch the .s
 * render builds. That scratch is captured at canonical tick rate, so without
 * sub-tick blend the animation steps at 30 Hz while movement is smooth. We
 * roll prev/curr copies of the whole seg block at the canonical hook, then in
 * the re-extract build a MIXED block: per segment, interpolate the blend in
 * 0..4096 units (wrap = 4096 at a keyframe knot) — same pair → lerp; clean
 * single advance (the stream pointers changed but the cumulative vert count is
 * fixed) → continuous phase, picking the prev pair while phase<4096 else curr.
 * The keyframe DATA pointers are model-static, so a prev entry decodes the prev
 * pair correctly; only the blend (and which entry) is chosen per phase. */
static uint8_t s_seg_prev[SEG_BLOCK], s_seg_curr[SEG_BLOCK];
static int     s_seg_valid = 0;       /* 0 none, 1 only curr, 2 prev+curr */
static double  s_anim_alpha = 0.0;
static int     s_anim_on    = 0;

/* Roll prev<-curr and capture curr from the live seg scratch (canonical hook).
 * The two pose snapshots feed the two-pass shape lerp in the re-extract. */
static void spyro_anim_capture_seg(CPUState* cpu) {
    (void)cpu;
    const void* h = openpete_subtick_host_ptr(SEG_SCRATCH);
    if (!h) return;
    if (s_seg_valid >= 1) memcpy(s_seg_prev, s_seg_curr, SEG_BLOCK);
    memcpy(s_seg_curr, h, SEG_BLOCK);
    s_seg_valid = (s_seg_valid >= 1) ? 2 : 1;
}

/* Orchestrator sets the sub-tick alpha + whether interpolation is active
 * (camera bracketed). Read by spyro_player_native_reextract. */
void spyro_native_set_anim_interp(double alpha, int on) { s_anim_alpha = alpha; s_anim_on = on ? 1 : 0; }

/* ---- mod actor render service (rung 16): pose source parameterization ----
 * The extractor normally reads the LIVE pose (g_Spyro + the seg scratch the
 * .s Phase A just built). For a ghost replay the same decode runs against a
 * RECORDED pose: spyro1_actor_emit swaps the recorded seg block into the
 * scratch (saved/restored around the call — render-transient, same trick as
 * the sub-tick two-pass above) and arms this override so the derivation
 * takes position / rotation / animation from the record instead of g_Spyro.
 * The camera side stays LIVE — the ghost is projected through the current
 * present's camera, which is the whole point. */
static struct {
    int      active;
    double   pos[3];
    uint32_t rot[3];
    uint32_t anim_idx;
} s_pose_ovr;

/* Hook-consistent live-pose snapshot for capture_actor_pose: rolled at the
 * canonical Spyro-render hook, AFTER the .s ran (scratch fresh, g_Spyro
 * fields exactly the ones that render consumed) — a mid-tick read can never
 * pair a new anim index with old stream cursors. Armed whenever mods are
 * loaded; works headless (capture is sim-side; only DRAWING needs vk). */
static spyro1_actor_pose_t s_pose_live;
static int s_pose_live_valid = 0;

static void spyro_pose_snapshot(CPUState* cpu) {
    const void* h = openpete_subtick_host_ptr(SEG_SCRATCH);
    if (!h) return;
    s_pose_live.magic    = SPYRO1_ACTOR_POSE_MAGIC;
    s_pose_live.version  = SPYRO1_ACTOR_POSE_VERSION;
    s_pose_live.pos[0]   = (float)(int32_t)cpu->read_word(G_SPYRO + 0x00);
    s_pose_live.pos[1]   = (float)(int32_t)cpu->read_word(G_SPYRO + 0x04);
    s_pose_live.pos[2]   = (float)(int32_t)cpu->read_word(G_SPYRO + 0x08);
    s_pose_live.rot[0]   = cpu->read_word(G_SPYRO + 0x0C);
    s_pose_live.rot[1]   = cpu->read_word(G_SPYRO + 0x10);
    s_pose_live.rot[2]   = cpu->read_word(G_SPYRO + 0x14);
    s_pose_live.anim_idx = cpu->read_byte(G_SPYRO + 0x18);
    s_pose_live.model    = cpu->read_word(G_MODELS);
    memcpy(s_pose_live.seg, h, SEG_BLOCK);
    s_pose_live_valid = 1;
}

/* Sub-tick ROTATION interpolation: per-segment float byte-angles (body=0, head=1,
 * tail=2; axis k=0 Z / 1 X / 2 Y) the orchestrator interpolates from g_Spyro's
 * prev/curr euler snapshots, handed in so spyro_compose_euler renders the rotation
 * below the 8-bit euler grid (256/circle) instead of stepping on it. s_rot_active[s]
 * gates each segment (head/tail off when sticky / mid-transition → byte path). */
static int    s_rot_active[3] = {0,0,0};
static double s_rot_ang[3][3];
void spyro_native_set_rot_interp(const int* active, const double* ang /*3x3, row-major*/) {
    if (!active) { s_rot_active[0]=s_rot_active[1]=s_rot_active[2]=0; return; }
    for (int s = 0; s < 3; s++) {
        s_rot_active[s] = active[s];
        for (int k = 0; k < 3; k++) s_rot_ang[s][k] = ang[s*3 + k];
    }
}

/* Float RTPS mirror WITH translation (gte.cpp gte_rtps_internal), reproducing
 * the GTE's near-plane behaviour in float. For a vertex close to the camera the
 * hardware caps the perspective divide: push_sz clamps SZ3 to [0,0xFFFF], and
 * gte_divide returns 0x1FFFF (≈2.0 in the 1.16 quotient) whenever H >= 2*SZ3
 * (or SZ3==0) instead of the true H/SZ3. The .s therefore pins a near vertex
 * near the screen centre+2*IR; using the *true* (much larger) 1/z instead made
 * such verts shoot off — the Spyro tail-tip "stretch to the screen edge" (demo
 * f19196-19200, the only residual in the 20k sweep: 18/12110 frames, all the
 * tail). Mirroring the cap (and the IR16 / ±0x400 SXY clamps) removes it while
 * leaving the 99.85% of in-range verts byte-identical (there q = H/SZ3 exactly,
 * the path that already matched to ~0.8px). The UNR reciprocal's sub-pixel
 * wobble is not reproduced (our true divide is strictly closer to ideal). */
static double project(const int M[3][3], const double TR[3], double vx, double vy, double vz,
                      double ofx_px, double ofy_px, double H,
                      double* out_sx, double* out_sy, int* out_sat) {
    ngte_proj o;   /* shared GTE-faithful pipeline, ±1024 SXY bound ON (W2.3) */
    ngte_project_gte(M, TR, vx, vy, vz, ofx_px, ofy_px, H, 1, &o);
    *out_sx = o.sx;
    *out_sy = o.sy;
    if (out_sat) *out_sat = o.sat;
    return o.m3;
}

/* MIPS `(v << lsh) >> rsh` (sll then arithmetic sra) reproduced exactly. */
static inline int32_t sra_field(int32_t v, int lsh, int rsh) {
    return (int32_t)((uint32_t)v << lsh) >> rsh;
}

typedef struct { float sx, sy, vz; int sat, seg; } spxy;
#define MAX_SPROJ 2048
static spxy s_sproj[MAX_SPROJ]; static int s_sproj_n;
static spxy s_sproj_prev[MAX_SPROJ];   /* prev-pose projection for the two-pass shape lerp */
static int s_lastseg_start, s_lastseg_n, s_lastseg_id;  /* last segment's s_sproj range */

/* 3b.4 face-stream walk — forward decls (the aggregate verdict uses these; the
 * definitions are below spyro_feed_scaffold). */
typedef struct { int walk, expected, cnt_ok, n, pos_ok, rgb_ok, uv_ok, first_bad; } walk_stats;
static int  build_spyro_walk(CPUState* cpu);
static void spyro_walk_eval(CPUState* cpu, walk_stats* st);

/* Decode ONE vertex of ONE keyframe stream into the running accumulator
 * (*az/*ax/*ay), advancing whichever packed cursor (v0 1-byte / v1 2-byte) this
 * vertex uses. `corr` is the per-vertex m_AnimationVertices correction word
 * (fetched + advanced by the caller). Returns the NEXT vertex's width as a
 * boolean (1 = 2-byte): the width is PER-VERTEX, carried in a flag bit of the
 * current vertex — bit0 of the byte (1-byte form, .L800246C8 `andi $t0,$s1,0x1`)
 * or bit14 (0x4000) of the halfword (2-byte form, .L800247F0 `andi $t0,$s1,0x4000`,
 * set before the escape branch so it applies to escapes too). The first vertex's
 * width comes from frameData bit 0x1000 (A) / 0x2000 (B). This is the headline
 * fix: the stream interleaves 1-/2-byte verts and a fixed per-segment width
 * desyncs the prefix-sum at the first switch. X/Y deltas subtract, Z adds. */
static int spyro_decode_vertex(CPUState* cpu, int is2, int32_t corr,
                               uint32_t* v0, uint32_t* v1, int* az, int* ax, int* ay) {
    if (!is2) {                                      /* 1-byte form */
        uint32_t b = cpu->read_byte(*v0); *v0 += 1;
        int32_t d = (int32_t)cpu->read_word(D_VERT_LUT + ((b << 1) & 0x1FCu)) + corr;
        *az += sra_field(d,0,21); *ax -= sra_field(d,11,21); *ay -= sra_field(d,22,21);
        return (b & 1u) ? 1 : 0;                     /* bit0 -> next vertex width */
    }
    int16_t h = (int16_t)cpu->read_half(*v1); *v1 += 2;  /* 2-byte form */
    int next2 = ((uint16_t)h & 0x4000u) ? 1 : 0;     /* bit14 -> next vertex width */
    if (h < 0) {                                     /* wide escape (absolute set, no corr) */
        uint32_t s2 = cpu->read_half(*v1); *v1 += 2;
        *az = sra_field((int32_t)h,18,21);
        *ax = -sra_field((int32_t)((s2 << 12) | (((uint32_t)(int32_t)h) << 28)),0,21);
        *ay = -sra_field((int32_t)(s2 << 22),0,21);
    } else {                                         /* h-split + corr-split */
        *az += sra_field((int32_t)h,18,25); *ax -= sra_field((int32_t)h,23,25); *ay -= sra_field((int32_t)h,28,25);
        *az += sra_field(corr,0,21); *ax -= sra_field(corr,11,21); *ay -= sra_field(corr,22,21);
    }
    return next2;
}

/* Decode one segment's model-space verts (running accumulator + INTPL lerp),
 * project each, append to s_sproj. Returns verts emitted. Mirrors the four
 * width combos of .L800244D8/.L80024580/.L800246B4/.L800247D8 + wide escapes,
 * with per-vertex width switching (see spyro_decode_vertex). */
static int spyro_project_segment(CPUState* cpu, uint32_t entry, const int M[3][3], const double TR[3],
                                 double ofx, double ofy, double H, int seg, int count) {
    uint32_t fd = cpu->read_word(entry + 0x00);
    int a2 = (fd & 0x1000u) != 0;           /* current frame-A width (per-vertex) */
    int b2 = (fd & 0x2000u) != 0;           /* current frame-B width (per-vertex) */
    int dual = (fd & 0xFFFu) != 0;          /* s7 != 0 -> two-keyframe INTPL */
    double blend = (double)(fd & 0xFFFu) / 4096.0;
    uint32_t atA = cpu->read_word(entry + 0x24), v0A = cpu->read_word(entry + 0x28), v1A = cpu->read_word(entry + 0x2C);
    uint32_t atB = cpu->read_word(entry + 0x34), v0B = cpu->read_word(entry + 0x38), v1B = cpu->read_word(entry + 0x3C);
    int aZ=0,aX=0,aY=0;   /* frame-A accumulator (Z added, X/Y subtracted) */
    int bZ=0,bX=0,bY=0;   /* frame-B accumulator */
    int emitted = 0;
    for (int i = 0; i < count && s_sproj_n < MAX_SPROJ; i++) {
        /* ---- frame A vertex (corr + cursor advance happen every vertex) ---- */
        int32_t corrA = (int32_t)cpu->read_word(atA); atA += 4;
        a2 = spyro_decode_vertex(cpu, a2, corrA, &v0A, &v1A, &aZ, &aX, &aY);
        double VX = aX, VY = aY, VZ = aZ;
        /* ---- frame B vertex (only if dual) ---- */
        if (dual) {
            int32_t corrB = (int32_t)cpu->read_word(atB); atB += 4;
            b2 = spyro_decode_vertex(cpu, b2, corrB, &v0B, &v1B, &bZ, &bX, &bY);
            VX = (double)aX + blend * (bX - aX);
            VY = (double)aY + blend * (bY - aY);
            VZ = (double)aZ + blend * (bZ - aZ);
        }
        double sx, sy; int sat;
        double vz = project(M, TR, VX, VY, VZ, ofx, ofy, H, &sx, &sy, &sat);
        s_sproj[s_sproj_n].sx = (float)sx; s_sproj[s_sproj_n].sy = (float)sy;
        s_sproj[s_sproj_n].vz = (float)vz; s_sproj[s_sproj_n].sat = sat; s_sproj[s_sproj_n].seg = seg;
        s_sproj_n++; emitted++;
    }
    return emitted;
}

/* ======================================================================== *
 *  PHASE-A NATIVE MATRIX DERIVATION (re-derive M + TR from gamestate)       *
 * ======================================================================== *
 * The last .s per-frame coupling for Spyro geometry: spyro_project_all reads
 * each segment's view×rotation matrix (M) and translation (TR) straight from
 * the Phase-A scratch block (G_SCRATCH+0x1900). Here we rebuild them from
 * gamestate (g_Camera projection matrix × g_Spyro pose + the seam joints),
 * the moby-3a.1→3a.4.3 analog. RE'd from r_pete.s 281-622 (.L80023EE0 TR +
 * .L800241E0/.L80024290/.L80024344 euler + D_80024054 seam RTPS):
 *
 *   TR_body = swizzled-centre MVMVA (.L80023EE0): IR1=cam.y-sy, IR2=cam.z-sz,
 *     IR3=sx-cam.x (int16); MAC=(PM·IR)>>12 (sf=1, no <<1/>>2 — unlike moby).
 *   M_body  = PM · Ry·Rx·Rz from g_Spyro[0xC] (same euler idx/order/signs as
 *     moby shaded_compose_euler — Y-rotation sign verified bit-exact by tracing
 *     the GTE column-rotate: .L800241E0 builds col0'=cos·col0+sin·col2,
 *     col2'=-sin·col0+cos·col2 == M·Ry, Ry={{c,0,-s},{0,1,0},{s,0,c}}).
 *   M_head/M_tail = M_body · euler(g_Spyro[0x10]/[0x14]); else STICKY = M_body
 *     (the .s leaves the body matrix in the GTE; D_800240F8 reloads only when
 *     the angle word is non-zero — that sets the segment's frameData sign bit).
 *   TR_head/TR_tail = seam joints (D_80024054 RTPS): decode the body keyframe
 *     stream word [0x10] (head) / [0x14] (tail) — same field layout as a vertex
 *     delta, X/Y negated, Z kept — INTPL'd by body progress for dual keyframes,
 *     then RTPS through M_body + TR_body, IR = sat16(view coords).
 *
 * Verified per-element vs the scratch block (byte-exact ground truth — it holds
 * the exact M/TR Phase A built), then drives spyro_project_all (gated). */

typedef struct { int M[3][3]; double TR[3]; int present; } seg_xform;
static seg_xform s_xf[3];

/* Native Phase-A matrices are the DEFAULT (verified byte-exact vs the scratch
 * block over the full 20k/4-level demo: M worstΔ=0, TR worstΔ=1 LSB, geometry
 * imperfect=0 @3.9px). OPENPETE_SPYRO_SCRATCH_MAT=1 forces the old scratch read
 * (kept for A/B regression). */
static int spyro_native_mat(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SPYRO_SCRATCH_MAT"); v = !(e && *e && *e != '0'); }
    return v;
}

/* M = start·Ry·Rx·Rz from a packed euler word (g_Spyro[0xC/0x10/0x14]). rot==0
 * -> identity (sin@0=0, cos@0=4096) so M=start (the sticky / no-rotation case).
 * `seg` (0 body / 1 head / 2 tail; -1 = none) selects the sub-tick float angles
 * the orchestrator handed in when s_rot_active[seg] is set — rendering rotation
 * below the byte grid; otherwise the canonical byte-exact table lookup is used. */
static void spyro_compose_euler(CPUState* cpu, const int start[3][3], uint32_t rot, int out[3][3], int seg) {
    ngte_euler_sc e;
    if (seg >= 0 && seg < 3 && s_rot_active[seg]) {
        ngte_sincos_interp(cpu, s_rot_ang[seg][0], &e.sz, &e.cz);   /* Z (+0 byte) */
        ngte_sincos_interp(cpu, s_rot_ang[seg][1], &e.sx, &e.cx);   /* X (+1 byte) */
        ngte_sincos_interp(cpu, s_rot_ang[seg][2], &e.sy, &e.cy);   /* Y (+2 byte) */
    } else {
        ngte_euler_sincos(cpu, rot, &e);
    }
    ngte_compose_euler(start, &e, out);
}

static inline int spyro_sat16(int v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }

/* INTPL one seam-joint field: kfA `jw`, kfB `jwB`, blend IR0 (.s $t6 = prog<<8).
 * Mirrors the GTE INTPL pipe (color_depth_cue_finish): result = (IR<<12 +
 * IR0*sat16(FC-IR)) >> 12. Single keyframe (!dual) = the raw field. Integer-
 * exact so the seam TR matches the scratch block byte-for-byte (a float lerp +
 * truncate drifted up to 2 units, +0.2px at the near-plane tail tip). */
static int spyro_seam_field(int32_t jw, int32_t jwB, int lsh, int ir0, int dual) {
    int a = sra_field(jw, lsh, 21);
    if (!dual) return a;
    int diff = sra_field(jwB, lsh, 21) - a;        /* 11-bit fields -> fits int16 */
    diff = spyro_sat16(diff);
    return (int)((((int64_t)a << 12) + (int64_t)ir0 * diff) >> 12);
}

/* Seam-joint view-space TR (head/tail): RTPS the body keyframe offset word `jw`
 * (kfB `jwB`, blend ir0=prog<<8) through M_body + TR_body. Field layout matches
 * the vertex delta (Z=>>21, X=<<11>>21, Y=<<22>>21); X/Y negated. RTPS:
 * MAC = (TR<<12 + M·V) >> 12, IR = sat16(MAC) — the .s seam RTPS at D_80024054. */
static void spyro_seam_tr(const int Mb[3][3], const double TRb[3], int32_t jw, int32_t jwB,
                          int ir0, int dual, double out[3]) {
    int VZ =  spyro_seam_field(jw, jwB, 0,  ir0, dual);
    int VX = -spyro_seam_field(jw, jwB, 11, ir0, dual);   /* X subtracted (negated) */
    int VY = -spyro_seam_field(jw, jwB, 22, ir0, dual);   /* Y subtracted (negated) */
    /* FRACTIONAL: TRb + (Mb·V)/4096 (was ((TRb<<12)+Mb·V)>>12 with an int16 sat).
     * Carry the body's fractional TR through to the seam so the head/tail origin
     * tracks the continuous body below the 1-unit grid; project()'s own IR16 sat
     * handles off-screen, so the per-component sat16 is dropped (matches the moby
     * float-TR path). */
    for (int i = 0; i < 3; i++)
        out[i] = TRb[i]
               + ((double)Mb[i][0]*VX + (double)Mb[i][1]*VY + (double)Mb[i][2]*VZ) / 4096.0;
}

/* Fill s_xf[3] = {M,TR,present} from gamestate. */
static void spyro_derive_phaseA(CPUState* cpu) {
    memset(s_xf, 0, sizeof s_xf);
    int PM[3][3]; ngte_read_matrix(cpu, G_CAMERA, PM);  /* camera m_ProjectionMatrix @ +0x00 */
    int32_t cx=(int32_t)cpu->read_word(G_CAMERA+0x28), cy=(int32_t)cpu->read_word(G_CAMERA+0x2C), cz=(int32_t)cpu->read_word(G_CAMERA+0x30);
    int32_t sx=(int32_t)cpu->read_word(G_SPYRO+0x00), sy=(int32_t)cpu->read_word(G_SPYRO+0x04), sz=(int32_t)cpu->read_word(G_SPYRO+0x08);
    /* Sub-tick floor fix (item 22a, folded into the camera tier): prefer the
     * FRACTIONAL interpolated camera + Spyro positions (un-rounded) and drop the
     * int16/>>12 truncation, so the body's view-space origin — and thus the whole
     * Spyro model — is continuous below the 1-world-unit lerp32 grid. At the
     * canonical 30 Hz lockstep the resolver returns 0 → integer deltas → the
     * derived TR is the same value, just carried as a double (the int16 path never
     * wrapped: the camera follows Spyro, so |cam−spyro| ≪ 32767). */
    double cfx=(double)cx, cfy=(double)cy, cfz=(double)cz;
    double sfx=(double)sx, sfy=(double)sy, sfz=(double)sz;
    { double cf[3], sf[3];
      if (openpete_native_interp_pos(G_CAMERA, cf)) { cfx=cf[0]; cfy=cf[1]; cfz=cf[2]; }
      if (!s_pose_ovr.active &&
          openpete_native_interp_pos(G_SPYRO,  sf)) { sfx=sf[0]; sfy=sf[1]; sfz=sf[2]; } }
    /* Recorded-pose replay (mod actor render): position/rotation come from
     * the record; the camera terms above stay live. Euler composition takes
     * seg=-1 so the live player's sub-tick float angles never bleed in. */
    if (s_pose_ovr.active) { sfx = s_pose_ovr.pos[0]; sfy = s_pose_ovr.pos[1]; sfz = s_pose_ovr.pos[2]; }
    double IR1=cfy-sfy, IR2=cfz-sfz, IR3=sfx-cfx;
    double TRb[3];
    for (int i = 0; i < 3; i++)
        TRb[i] = (PM[i][0]*IR1 + PM[i][1]*IR2 + PM[i][2]*IR3) / 4096.0;
    uint32_t rotB = s_pose_ovr.active ? s_pose_ovr.rot[0] : cpu->read_word(G_SPYRO+0xC);
    int Mb[3][3]; spyro_compose_euler(cpu, PM, rotB, Mb, s_pose_ovr.active ? -1 : 0);
    memcpy(s_xf[0].M, Mb, sizeof Mb); memcpy(s_xf[0].TR, TRb, sizeof TRb); s_xf[0].present = 1;
    uint32_t rotH = s_pose_ovr.active ? s_pose_ovr.rot[1] : cpu->read_word(G_SPYRO+0x10);
    uint32_t rotT = s_pose_ovr.active ? s_pose_ovr.rot[2] : cpu->read_word(G_SPYRO+0x14);
    if (rotH) spyro_compose_euler(cpu, Mb, rotH, s_xf[1].M, s_pose_ovr.active ? -1 : 1); else memcpy(s_xf[1].M, Mb, sizeof Mb);
    if (rotT) spyro_compose_euler(cpu, Mb, rotT, s_xf[2].M, s_pose_ovr.active ? -1 : 2); else memcpy(s_xf[2].M, Mb, sizeof Mb);
    s_xf[1].present = s_xf[2].present = 1;
    /* seam joints: body kfA(/kfB) stream words [0x10] (head) / [0x14] (tail).
     * a0 = (2-byte cursor) - 0x18 (.s: cursor = a0+0x18); t2=*(a0+0x10), t3=*(a0+0x14)
     * => *(cursor-8) / *(cursor-4). The cursor is a static frame-offset asset
     * pointer (Phase-A stream setup, not camera-coupled); reading the keyframe
     * block through it is an asset read. (Cursor re-derivation = separate part.) */
    /* INTPL blend = IR0/4096 where IR0 = the .s's $t6 = (progress << 8) (the
     * progress byte is shifted left 8 at 80023B60 before feeding C2_IR0 at
     * .L80023E3C — NOT the raw byte). progress is 0..15 so prog<<8 <= 0xF00. */
    /* INTPL blend IR0 = the seg-0 scratch fd blend (canonically == progress<<8,
     * but the sub-tick re-extract writes a FINE interpolated fd there). Reading
     * it (not the 16-level integer g_Spyro+0x24 progress) keeps the head/tail
     * SEAM on the same continuous blend as the body verts — else the seam steps
     * in coarse 1/16 increments while the verts are smooth (head/tail jitter). */
    int ir0 = (int)(cpu->read_word(SEG_SCRATCH) & 0xFFFu);
    int dual = (ir0 != 0);
    uint32_t curA = cpu->read_word(SEG_SCRATCH + 0x2C), curB = cpu->read_word(SEG_SCRATCH + 0x3C);
    int32_t jhA=(int32_t)cpu->read_word(curA-8), jtA=(int32_t)cpu->read_word(curA-4);
    int32_t jhB=0, jtB=0;
    if (dual) { jhB=(int32_t)cpu->read_word(curB-8); jtB=(int32_t)cpu->read_word(curB-4); }
    spyro_seam_tr(Mb, TRb, jhA, jhB, ir0, dual, s_xf[1].TR);
    spyro_seam_tr(Mb, TRb, jtA, jtB, ir0, dual, s_xf[2].TR);
}

/* ---- matrix A/B: derived M+TR vs the scratch block (byte-exact ground truth)
 * Per segment: max |derived-scratch| over the 9 matrix elements + 3 TR words.
 * For a sticky segment (no own matrix) the effective matrix is the body's, so
 * compare against seg0's scratch matrix. Returns worst element/TR deltas. */
typedef struct { int seg_n; int m_max[3]; int tr_max[3]; int worst_m, worst_tr; } mat_stats;
static void spyro_matrix_eval(CPUState* cpu, mat_stats* ms) {
    memset(ms, 0, sizeof *ms);
    spyro_derive_phaseA(cpu);
    int M0[3][3]; ngte_read_matrix(cpu, SEG_SCRATCH + 0x10, M0);   /* body (seg0) scratch matrix */
    for (int s = 0; s < 3; s++) {
        uint32_t e = SEG_SCRATCH + (uint32_t)s*SEG_STRIDE;
        uint32_t fd = cpu->read_word(e);
        if (fd == 0) break;
        ms->seg_n++;
        int hasMat = (s == 0) || (fd & 0x80000000u);
        int Ms_[3][3]; if (hasMat) ngte_read_matrix(cpu, e + 0x10, Ms_); else memcpy(Ms_, M0, sizeof M0);
        int mmax = 0;
        for (int i=0;i<3;i++) for (int j=0;j<3;j++) { int d = abs(s_xf[s].M[i][j] - Ms_[i][j]); if (d>mmax) mmax=d; }
        int TRs[3] = { (int32_t)cpu->read_word(e+0x04), (int32_t)cpu->read_word(e+0x08), (int32_t)cpu->read_word(e+0x0C) };
        int tmax = 0;
        for (int i=0;i<3;i++) { int d = (int)(fabs(s_xf[s].TR[i] - (double)TRs[i]) + 0.5); if (d>tmax) tmax=d; }
        ms->m_max[s] = mmax; ms->tr_max[s] = tmax;
        if (mmax > ms->worst_m) ms->worst_m = mmax;
        if (tmax > ms->worst_tr) ms->worst_tr = tmax;
    }
}
static void spyro_matrix_compare(CPUState* cpu) {
    mat_stats ms; spyro_matrix_eval(cpu, &ms);
    fprintf(stderr, "[spyro-mat f=%u] segs=%d  Mmax(b/h/t)=%d/%d/%d  TRmax(b/h/t)=%d/%d/%d  => %s\n",
            openpete_frame, ms.seg_n, ms.m_max[0], ms.m_max[1], ms.m_max[2],
            ms.tr_max[0], ms.tr_max[1], ms.tr_max[2],
            (ms.worst_m <= 2 && ms.worst_tr <= 2) ? "MATCH" : "MISMATCH");
}

/* One-shot input dump (diagnostic): the gamestate the derivation reads + the
 * scratch outputs it must reproduce, side by side. */
static void spyro_phaseA_dump(CPUState* cpu) {
    fprintf(stderr, "[spyro-phaseA f=%u]\n", openpete_frame);
    fprintf(stderr, "  cam projM:");
    for (int w=0;w<5;w++) fprintf(stderr, " %08X", cpu->read_word(G_CAMERA + (uint32_t)w*4));
    fprintf(stderr, "  campos=(%d,%d,%d)\n", (int32_t)cpu->read_word(G_CAMERA+0x28),
            (int32_t)cpu->read_word(G_CAMERA+0x2C), (int32_t)cpu->read_word(G_CAMERA+0x30));
    fprintf(stderr, "  spyropos=(%d,%d,%d)  rot[0xC/0x10/0x14]=%08X/%08X/%08X  prog[0x24]=%02X\n",
            (int32_t)cpu->read_word(G_SPYRO+0x00), (int32_t)cpu->read_word(G_SPYRO+0x04),
            (int32_t)cpu->read_word(G_SPYRO+0x08), cpu->read_word(G_SPYRO+0xC),
            cpu->read_word(G_SPYRO+0x10), cpu->read_word(G_SPYRO+0x14), cpu->read_byte(G_SPYRO+0x24));
    spyro_derive_phaseA(cpu);
    for (int s=0;s<3;s++) {
        uint32_t e = SEG_SCRATCH + (uint32_t)s*SEG_STRIDE;
        if (cpu->read_word(e)==0) break;
        fprintf(stderr, "  seg%d scratch M:", s);
        for (int w=0;w<5;w++) fprintf(stderr, " %08X", cpu->read_word(e+0x10+(uint32_t)w*4));
        fprintf(stderr, "  TR=(%d,%d,%d)\n", (int32_t)cpu->read_word(e+0x04),
                (int32_t)cpu->read_word(e+0x08), (int32_t)cpu->read_word(e+0x0C));
        fprintf(stderr, "  seg%d derivd M: [%d %d %d / %d %d %d / %d %d %d]  TR=(%.2f,%.2f,%.2f)\n", s,
                s_xf[s].M[0][0],s_xf[s].M[0][1],s_xf[s].M[0][2], s_xf[s].M[1][0],s_xf[s].M[1][1],s_xf[s].M[1][2],
                s_xf[s].M[2][0],s_xf[s].M[2][1],s_xf[s].M[2][2], s_xf[s].TR[0],s_xf[s].TR[1],s_xf[s].TR[2]);
    }
    /* a0 base check: does (2-byte cursor - 0x18) == m_AnimationVertices + frame-offset? */
    uint32_t animIdx=cpu->read_byte(G_SPYRO+0x18), frA=cpu->read_byte(G_SPYRO+0x1E);
    uint32_t model=cpu->read_word(G_MODELS)|0x80000000u;
    uint32_t animHdr=cpu->read_word(model+0x38+animIdx*4);
    uint32_t vbase=cpu->read_word(animHdr+0x10), fword=cpu->read_word(animHdr+0x24+frA*4);
    uint32_t curA=cpu->read_word(SEG_SCRATCH+0x2C);
    fprintf(stderr, "  a0=cursorA-0x18=%08X  m_AnimVerts=%08X frameWord[A]=%08X  off=((fw<<11)>>10)=%08X  base+off=%08X\n",
            curA-0x18, vbase, fword, ((fword<<11)>>10), vbase + ((fword<<11)>>10));
}

/* Walk the 3 segment scratch entries, decode + project all verts to s_sproj. */
static int spyro_project_all(CPUState* cpu) {
    s_sproj_n = 0;
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFF);
    int native = spyro_native_mat();
    /* Recorded-pose replay always derives natively: the scratch's own M/TR
     * (the OPENPETE_SPYRO_SCRATCH_MAT regression path) were built with the
     * CAPTURE-time camera and would project the ghost through a stale view. */
    if (s_pose_ovr.active) native = 1;
    if (native) spyro_derive_phaseA(cpu);            /* M+TR from gamestate (Phase A) */
    int M[3][3]; double TR[3]; int have_mat = 0;
    int nseg = 0;
    int prev_end = 0;                                /* cumulative scratchpad byte offset */
    for (int s = 0; s < 3; s++) {
        uint32_t entry = SEG_SCRATCH + (uint32_t)s * SEG_STRIDE;
        uint32_t fd = cpu->read_word(entry + 0x00);
        if (fd == 0) break;                          /* terminator */
        /* [23:14] is the CUMULATIVE scratchpad end byte-offset (fp runs
         * continuously across segments — it does NOT reset). This segment's
         * vertex count = (this_end - prev_end) / 4. */
        int cur_end = (int)((fd >> 14) & 0x3FFu);
        int count = (cur_end - prev_end) >> 2; if (count < 0) count = 0;
        prev_end = cur_end;
        {
            static int ad = -1;
            if (ad < 0) { const char* e = getenv("OPENPETE_ANIMDBG"); ad = (e && *e && *e != '0'); }
            if (ad && (openpete_frame % 60u) == 0)
                fprintf(stderr, "[spyro-anim f=%u] seg%d fd_blend=%u prog[0x%x]=%u\n",
                        openpete_frame, s, fd & 0xFFFu, 0x24 + s,
                        cpu->read_byte(G_SPYRO + 0x24u + (uint32_t)s));
        }
        if (native) {
            /* Phase-A native: M + TR re-derived from gamestate (s_xf), the last
             * .s coupling for Spyro geometry retired. (Stickiness is resolved in
             * spyro_derive_phaseA: a no-rotation head/tail gets M = M_body.) */
            memcpy(M, s_xf[s].M, sizeof M);
            TR[0]=s_xf[s].TR[0]; TR[1]=s_xf[s].TR[1]; TR[2]=s_xf[s].TR[2];
            have_mat = 1;
            s_lastseg_start = s_sproj_n;
            s_lastseg_n = spyro_project_segment(cpu, entry, M, TR, ofx, ofy, H, s, count);
            s_lastseg_id = s; nseg++;
            continue;
        }
        /* Matrix reload keys on the frameData SIGN BIT, exactly like the .s:
         * `bgez $s1, .L80024478` SKIPS the ctc2 reload when the sign bit is
         * CLEAR, so the matrix is STICKY — the segment reuses the previous one.
         * The first segment always takes the body matrix: the .s decode loop
         * inherits whatever Phase A left in the GTE, which is the body's matrix
         * == seg0's +0x10 slot (confirmed: post-call gte_ctrl R[0..4] == seg0
         * +0x10 on every captured frame). NOTE: keying on "+0x10 != 0" is WRONG
         * — Phase A can leave non-zero scratch matrices in the head/tail +0x10
         * slots with a CLEAR sign bit (the .s ignores them, staying sticky);
         * adopting them projected head/tail with a stale matrix (f4549: head
         * vanishes, tail lands at the front of the body). */
        if (s == 0 || (fd & 0x80000000u)) { ngte_read_matrix(cpu, entry + 0x10, M); have_mat = 1; }
        TR[0] = (double)(int32_t)cpu->read_word(entry + 0x04);
        TR[1] = (double)(int32_t)cpu->read_word(entry + 0x08);
        TR[2] = (double)(int32_t)cpu->read_word(entry + 0x0C);
        if (!have_mat) continue;                     /* no matrix yet (shouldn't happen for seg0) */
        s_lastseg_start = s_sproj_n;
        s_lastseg_n = spyro_project_segment(cpu, entry, M, TR, ofx, ofy, H, s, count);
        s_lastseg_id = s;
        nseg++;
    }
    return nseg;
}

/* ---- geometry A/B: native projected verts vs the captured prim verts ----
 * NN-match each prim vertex to the nearest native s_sproj (the moby-3a.1
 * instrument): a tight match proves the decode + projection reproduce the GTE.
 * prim->native direction so an over-decode (count too high) can't inflate. */
static void spyro_compare_geometry(void) {
    if (!s_sproj_n || !s_gte_n) { fprintf(stderr, "[spyro-verify f=%u] no data (proj=%d prim=%d)\n",
                                          openpete_frame, s_sproj_n, s_gte_n); return; }
    int total = 0, hit4 = 0, hit2 = 0; double errsum = 0; float emax = 0;
    for (int i = 0; i < s_gte_n; i++) {
        prim_t* p = &s_gte[i];
        for (int k = 0; k < p->nv; k++) {
            float bestd = 1e9f;
            for (int j = 0; j < s_sproj_n; j++) {
                float dx = p->x[k] - s_sproj[j].sx, dy = p->y[k] - s_sproj[j].sy;
                float d = dx*dx + dy*dy;
                if (d < bestd) bestd = d;
            }
            float dist = sqrtf(bestd);
            total++; errsum += dist; if (dist > emax) emax = dist;
            if (dist <= 4.0f) hit4++; if (dist <= 2.0f) hit2++;
        }
    }
    fprintf(stderr, "[spyro-verify f=%u] proj=%d primVerts=%d  match<=4px=%.1f%% <=2px=%.1f%%  mean=%.2fpx max=%.1fpx\n",
            openpete_frame, s_sproj_n, total,
            100.0*hit4/total, 100.0*hit2/total, errsum/total, emax);
}

/* ---- CLEAN per-vertex A/B vs the .s scratchpad SXY (no NN) ----------------
 * fp runs continuously across segments, so post-call scratchpad 0x1F800000
 * holds ALL verts in order: body[0..], head[..], tail[..] (238*4 < 1KB). Our
 * s_sproj is in the same order, so s_sproj[j] <-> scratchpad[j]. Direct compare
 * isolates a per-axis decode error with zero NN ambiguity / back-face noise. */
#define SCRATCHPAD 0x1F800000u
/* match% (<=4px) of native vert j vs scratchpad[j+off], per segment + overall */
static void spyro_scratchpad_at(CPUState* cpu, int off) {
    int n = s_sproj_n, hit4 = 0, tot = 0; double errsum = 0;
    int seg_hit[3]={0,0,0}, seg_tot[3]={0,0,0};
    int worst_j = -1; float worst_d = 0; float worst_gx=0, worst_gy=0;
    for (int j = 0; j < n; j++) {
        int k = j + off; if (k < 0 || k >= n) continue;
        uint32_t w = cpu->read_word(SCRATCHPAD + (uint32_t)k * 4);
        float gx = (float)(int16_t)(w & 0xFFFF), gy = (float)(int16_t)(w >> 16);
        float dx = s_sproj[j].sx - gx, dy = s_sproj[j].sy - gy, d = sqrtf(dx*dx + dy*dy);
        tot++; errsum += d; if (d <= 4.0f) hit4++;
        int sg = s_sproj[j].seg; if (sg>=0&&sg<3){ seg_tot[sg]++; if(d<=4.0f) seg_hit[sg]++; }
        if (d > worst_d) { worst_d = d; worst_j = j; worst_gx = gx; worst_gy = gy; }
    }
    if (worst_j >= 0 && worst_d > 8.0f)  /* outlier: native vs .s, with view-Z */
        fprintf(stderr, "  [outlier f=%u] j=%d seg=%d err=%.1fpx  native=(%.1f,%.1f) .s=(%.0f,%.0f) vz=%.1f sat=%d\n",
                openpete_frame, worst_j, s_sproj[worst_j].seg, worst_d,
                s_sproj[worst_j].sx, s_sproj[worst_j].sy, worst_gx, worst_gy,
                s_sproj[worst_j].vz, s_sproj[worst_j].sat);
    fprintf(stderr, "[spyro-scratch f=%u off=%+d] <=4px=%.1f%% mean=%.2fpx | body=%.0f%% head=%.0f%% tail=%.0f%%\n",
            openpete_frame, off, tot?100.0*hit4/tot:0, tot?errsum/tot:0,
            seg_tot[0]?100.0*seg_hit[0]/seg_tot[0]:0, seg_tot[1]?100.0*seg_hit[1]/seg_tot[1]:0,
            seg_tot[2]?100.0*seg_hit[2]/seg_tot[2]:0);
}
static void spyro_scratchpad_ab(CPUState* cpu) {
    if (s_sproj_n <= 0) return;
    /* Per-segment frameData + distinct-matrix flag (non-zero +0x10 slot). The
     * head "turning" => its own seam matrix => +0x10 non-zero / sign bit set;
     * surface it so head-merge frames are identifiable. */
    for (int s = 0; s < 3; s++) {
        uint32_t e = SEG_SCRATCH + (uint32_t)s * SEG_STRIDE;
        uint32_t fd = cpu->read_word(e);
        if (fd == 0) break;
        uint32_t m0 = cpu->read_word(e+0x10)|cpu->read_word(e+0x14)|cpu->read_word(e+0x18)
                     |cpu->read_word(e+0x1C)|cpu->read_word(e+0x20);
        fprintf(stderr, "  (%s fd=%08X blend=%u dual=%d A%s B%s mat=%s sign=%d)\n",
                k_seg_name[s], fd, fd & 0xFFF, (fd & 0xFFF) != 0,
                (fd & 0x1000) ? "2" : "1", (fd & 0x2000) ? "2" : "1",
                m0 ? "own" : "sticky", (int)(fd >> 31));
    }
    spyro_scratchpad_at(cpu, 0);
}

/* ---- full-run aggregate (20k verdict) ------------------------------------
 * Cheap per-frame (O(verts)) per-index match vs the .s scratchpad, accumulated
 * across the WHOLE run so a 20k/4-level demo gets a single verdict without 80k
 * log lines or the O(n^2) NN/idx cost. Tracks total Spyro-render frames, frames
 * with any segment <100% (≤4px), the worst single segment-match and its frame,
 * and the max per-vertex error. Prints a running line every 2000 frames + the
 * cumulative verdict, so the last line near the run end is the answer. */
static struct {
    long frames, bad_frames;
    double worst_match; unsigned worst_frame; int worst_seg;
    double max_err; unsigned max_err_frame;
    /* 3b.4 walk verdict (topology+colour+UV vs prims, full run) */
    long w_cnt_bad, w_rgb_bad, w_uv_bad, w_interp;   /* frames with a defect */
    unsigned w_first_cnt_bad_frame, w_first_interp_frame;
    /* Phase-A matrix verdict (derived M+TR vs scratch block, full run) */
    long mat_bad; int mat_worst_m, mat_worst_tr; unsigned mat_worst_frame;
} s_agg = { 0, 0, 100.0, 0, -1, 0.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static void spyro_agg_final(void) {
    fprintf(stderr, "[spyro-agg FINAL] frames=%ld imperfect=%ld (%.2f%%)  worst=%.1f%% (seg %d @f%u)  maxerr=%.1fpx @f%u  => %s\n",
            s_agg.frames, s_agg.bad_frames,
            s_agg.frames ? 100.0 * s_agg.bad_frames / s_agg.frames : 0.0,
            s_agg.worst_match, s_agg.worst_seg, s_agg.worst_frame,
            s_agg.max_err, s_agg.max_err_frame,
            s_agg.bad_frames == 0 ? "PASS (geometry 100% all segments, all frames)" : "GEOMETRY RESIDUAL");
    fprintf(stderr, "[spyro-walk FINAL] frames=%ld  cnt-mismatch=%ld (first @f%u)  rgb-bad=%ld  uv-bad=%ld  interp-path=%ld (first @f%u)  => %s\n",
            s_agg.frames, s_agg.w_cnt_bad, s_agg.w_first_cnt_bad_frame,
            s_agg.w_rgb_bad, s_agg.w_uv_bad, s_agg.w_interp, s_agg.w_first_interp_frame,
            (s_agg.w_cnt_bad==0 && s_agg.w_rgb_bad==0 && s_agg.w_uv_bad==0 && s_agg.w_interp==0)
              ? "PASS (walk topology+colour+UV byte-exact, all frames)" : "WALK RESIDUAL");
    fprintf(stderr, "[spyro-mat FINAL] frames=%ld  mat-mismatch=%ld  worstM=%d worstTR=%d @f%u  => %s\n",
            s_agg.frames, s_agg.mat_bad, s_agg.mat_worst_m, s_agg.mat_worst_tr, s_agg.mat_worst_frame,
            (s_agg.mat_bad==0) ? "PASS (M+TR derived from gamestate match scratch, all frames)" : "MATRIX RESIDUAL");
}

static void spyro_aggregate(CPUState* cpu) {
    static int s_reg = 0;
    if (!s_reg) { s_reg = 1; atexit(spyro_agg_final); }
    int seg_hit[3] = {0,0,0}, seg_tot[3] = {0,0,0};
    double fmax = 0.0; int worst_j = -1; float wgx = 0, wgy = 0;
    for (int j = 0; j < s_sproj_n; j++) {
        uint32_t w = cpu->read_word(SCRATCHPAD + (uint32_t)j * 4);
        float gx = (float)(int16_t)(w & 0xFFFF), gy = (float)(int16_t)(w >> 16);
        float dx = s_sproj[j].sx - gx, dy = s_sproj[j].sy - gy, d = sqrtf(dx*dx + dy*dy);
        int sg = s_sproj[j].seg; if (sg>=0&&sg<3){ seg_tot[sg]++; if(d<=4.0f) seg_hit[sg]++; }
        if (d > fmax) { fmax = d; worst_j = j; wgx = gx; wgy = gy; }
    }
    if (fmax > 8.0f && worst_j >= 0)  /* outlier: capture vert + view-Z wherever it occurs in the full run */
        fprintf(stderr, "  [outlier f=%u] j=%d seg=%d err=%.1fpx native=(%.1f,%.1f) .s=(%.0f,%.0f) vz=%.1f sat=%d "
                "(n=%d)\n", openpete_frame, worst_j, s_sproj[worst_j].seg, fmax,
                s_sproj[worst_j].sx, s_sproj[worst_j].sy, wgx, wgy,
                s_sproj[worst_j].vz, s_sproj[worst_j].sat, s_sproj_n);
    s_agg.frames++;
    int bad = 0;
    for (int sg = 0; sg < 3; sg++) {
        if (!seg_tot[sg]) continue;
        double m = 100.0 * seg_hit[sg] / seg_tot[sg];
        if (m < 100.0) bad = 1;
        if (m < s_agg.worst_match) { s_agg.worst_match = m; s_agg.worst_frame = openpete_frame; s_agg.worst_seg = sg; }
    }
    if (bad) s_agg.bad_frames++;
    if (fmax > s_agg.max_err) { s_agg.max_err = fmax; s_agg.max_err_frame = openpete_frame; }

    /* 3b.4 walk verdict (cheap per-frame; build already done by the caller).
     * interp-path detector: the demo uses the DIRECT emit path (g_Spyro+0x30==0);
     * if any level sets it non-zero, the direct-only walk would mis-stride, so
     * flag it loudly rather than silently emitting garbage. */
    if (cpu->read_word(G_SPYRO + 0x30) != 0) {
        if (!s_agg.w_interp) s_agg.w_first_interp_frame = openpete_frame;
        s_agg.w_interp++;
    }
    { walk_stats wst; spyro_walk_eval(cpu, &wst);
      if (!wst.cnt_ok) { if (!s_agg.w_cnt_bad) s_agg.w_first_cnt_bad_frame = openpete_frame; s_agg.w_cnt_bad++; }
      if (wst.n && wst.rgb_ok < wst.n) s_agg.w_rgb_bad++;
      if (wst.n && wst.uv_ok  < wst.n) s_agg.w_uv_bad++;
    }

    /* Phase-A matrix verdict (derived M+TR vs scratch; cheap, every frame). */
    { mat_stats ms; spyro_matrix_eval(cpu, &ms);
      if (ms.worst_m > 2 || ms.worst_tr > 2) s_agg.mat_bad++;
      if (ms.worst_m > s_agg.mat_worst_m) { s_agg.mat_worst_m = ms.worst_m; s_agg.mat_worst_frame = openpete_frame; }
      if (ms.worst_tr > s_agg.mat_worst_tr) { s_agg.mat_worst_tr = ms.worst_tr; }
    }

    if ((s_agg.frames % 2000) == 0) {
        fprintf(stderr, "[spyro-agg f=%u] frames=%ld imperfect=%ld  worst=%.1f%% (seg %d @f%u)  maxerr=%.1fpx @f%u\n",
                openpete_frame, s_agg.frames, s_agg.bad_frames, s_agg.worst_match,
                s_agg.worst_seg, s_agg.worst_frame, s_agg.max_err, s_agg.max_err_frame);
        fprintf(stderr, "[spyro-walk f=%u] frames=%ld cnt-mismatch=%ld rgb-bad=%ld uv-bad=%ld interp-path=%ld => %s\n",
                openpete_frame, s_agg.frames, s_agg.w_cnt_bad, s_agg.w_rgb_bad, s_agg.w_uv_bad, s_agg.w_interp,
                (s_agg.w_cnt_bad==0 && s_agg.w_rgb_bad==0 && s_agg.w_uv_bad==0 && s_agg.w_interp==0)
                  ? "PASS (walk byte-exact)" : "WALK RESIDUAL");
        fprintf(stderr, "[spyro-mat f=%u] frames=%ld mat-mismatch=%ld worstM=%d worstTR=%d @f%u => %s\n",
                openpete_frame, s_agg.frames, s_agg.mat_bad, s_agg.mat_worst_m, s_agg.mat_worst_tr, s_agg.mat_worst_frame,
                (s_agg.mat_bad==0) ? "PASS (matrix byte-exact)" : "MATRIX RESIDUAL");
    }
}

/* ---- scene feed (3b.4 scaffold) ------------------------------------------
 * Native float verts (s_sproj) carry the geometry; the captured prims carry
 * topology + per-vertex colour + texture (Tiledef). Each prim vertex is mapped
 * to its native vertex by EXACT INDEX RECOVERY: the .s wrote every vertex's
 * screen SXY to scratchpad 0x1F800000 in stream order (== s_sproj order), and
 * the prim packet's SXY is a byte-identical copy of that same RTPS output, so
 * `prim SXY == scratchpad[index]` recovers the .s's own vertex index exactly.
 * We then feed s_sproj[index] (native sub-pixel float). This replaces the old
 * screen-distance nearest-neighbour match, which grabbed ACROSS segments when
 * the head overlapped the body on screen (head triangles attaching to body
 * verts -> the "head merges with body" artifact): NN picks the closest vertex,
 * which need not be the one the face references. Index recovery is segment-safe
 * by construction. (The independent face-stream walk — the moby-3a.2b analog —
 * supersedes this scaffold entirely later.) */
static int spyro_nn(float px, float py) {
    int best = -1; float bd = 1e18f;
    for (int j = 0; j < s_sproj_n; j++) {
        float dx = px - s_sproj[j].sx, dy = py - s_sproj[j].sy, d = dx*dx + dy*dy;
        if (d < bd) { bd = d; best = j; }
    }
    return best;
}
/* Recover a prim vertex's stream index via exact SXY match vs the .s scratchpad.
 * Returns -1 if no exact match (caller falls back to NN). */
static int spyro_index_of(CPUState* cpu, int px, int py) {
    uint32_t want = ((uint32_t)(py & 0xFFFF) << 16) | (uint32_t)(px & 0xFFFF);
    for (int j = 0; j < s_sproj_n; j++)
        if (cpu->read_word(SCRATCHPAD + (uint32_t)j * 4) == want) return j;
    return -1;
}
/* Headless falsification: how often would screen-distance NN pick a DIFFERENT
 * vertex than exact index recovery? (Runs under OPENPETE_SPYRO_VERIFY, where the
 * scene feed is unavailable.) A high % means NN was mis-attaching faces — the
 * head-merge cause. */
static void spyro_idx_audit(CPUState* cpu) {
    long exact = 0, disagree = 0, fallback = 0, total = 0;
    for (int i = 0; i < s_gte_n; i++) {
        prim_t* p = &s_gte[i];
        if (p->nv < 3) continue;
        for (int k = 0; k < p->nv; k++) {
            int ex = spyro_index_of(cpu, (int)p->x[k], (int)p->y[k]);
            total++;
            if (ex < 0) { fallback++; continue; }
            exact++;
            if (ex != spyro_nn(p->x[k], p->y[k])) disagree++;
        }
    }
    fprintf(stderr, "[spyro-idx f=%u] exact=%ld/%ld fallback=%ld  NN-would-disagree=%ld (%.1f%%)\n",
            openpete_frame, exact, total, fallback, disagree,
            total ? 100.0 * disagree / total : 0.0);
}
/* (spyro_feed_scaffold removed 2026-06-16 — the prim-driven NN/index-recovery
 * feed is superseded by the independent face-stream walk spyro_feed_walk, the
 * moby-3a.2b analog. spyro_index_of/spyro_nn are kept: the verify A/B still uses
 * them. See spyro_feed_walk below the walk code.) */

/* ======================================================================== *
 *  3b.4: INDEPENDENT FACE-STREAM WALK (retire the prim/NN scaffold)         *
 * ======================================================================== *
 * Derive triangle TOPOLOGY + per-vertex COLOUR + UV natively from the Spyro
 * face stream (m_Faces), exactly as the .s Phase C does (r_pete.s 1170-1447,
 * the DIRECT path; g_Spyro+0x30 == 0, confirmed live for the demo). This is the
 * moby 3a.2b analog: it retires the prim-driven NN/index scaffold, leaving only
 * the static texture/VRAM read (a sanctioned asset read, not per-frame reuse).
 *
 * Face stream = m_Faces (animHdr+0x14): word0 @+0 = byte length, records @+4.
 * All 238 verts share ONE global face stream + colour table (body/head/tail
 * stitch into one mesh), so the walk is segment-agnostic: face indices slot
 * straight into s_sproj (== the .s SXY scratchpad at 0x1F800000, same order).
 *
 * Per face:
 *   word0 = $at: sign<0 = FRESH (quad-capable), >=0 = STRIP (single tri);
 *     bit0 = NCLIP-sign toggle; three vertex indices i0/i1/i2 =
 *     ((w0>>{20,11,2})&0x7FC)>>2.
 *   word1 = $s0: three colour indices ((>>17,>>8,<<1)&0x7FC) into m_Colors
 *     (packed RGB words). Colour table = m_Colors when colorFilter.interp==0
 *     (the common case), else the Fairy-Kiss pre-blend scratch G_SCRATCH+0x1D00.
 *   FRESH (stride 0x18): word2 @+8 -> i3=(w2&0x7FC)>>2, c3=(w2>>9)&0x7FC; UVs
 *     (Tiledef) @+0xC/+0x10/+0x14; dual-NCLIP -> quad(0x3C GT4) / tri(0x34 GT3) /
 *     skip (the moby emit map: t6a=NCLIP1, t6b=NCLIP2 with bit0 sign-negate;
 *     t6a>=0&t6b>0 -> tri(v3,v1,v2); t6a>=0&t6b<=0 -> skip; t6a<0&t6b>0 -> QUAD
 *     (v0,v1,v2)+(v1,v3,v2); t6a<0&t6b<=0 -> tri(v0,v1,v2)).
 *   STRIP (stride 0x14): single tri, UVs @+0x8/+0xC/+0x10, keep if
 *     (bit0 ? always : NCLIP1>0).
 *
 * NCLIP uses the .s's int16 SXY straight from the scratchpad (byte-exact cull
 * decision while the .s runs); the EMITTED vertex positions use s_sproj's native
 * float (smooth sub-pixel geometry). Validated by spyro_walk_compare (ordered,
 * index-exact vs the captured prims) before it replaces the feed. */

typedef struct { float sx,sy,vz; int u,v; uint32_t rgb; int idx; } swv;
typedef struct {
    swv v[3];
    int textured, tpage_x, tpage_y, clut_x, clut_y, bpp, blend;
} swtri;
#define MAX_SWTRI (MAX_SPROJ * 4)
static swtri s_swtri[MAX_SWTRI]; static int s_swtri_n;

/* signed 2*area on the native FLOAT screen coords == GTE NCLIP sign, WITHOUT the
 * GTE's int16 floor. The floored version (the old .s-scratchpad read) flips a
 * near-zero-area face's sign at grazing angles, culling genuinely front-facing thin
 * segments (Spyro's tail-tip / head / wing edges) so the background shows through —
 * the same bug fixed in moby_native (tri_nclip). s_sproj[] is the same vertex-index
 * space the emit reads; the Z-buffer sorts the survivors. */
static double spyro_nclip(int i0, int i1, int i2) {
    if (i0>=s_sproj_n || i1>=s_sproj_n || i2>=s_sproj_n) return -1.0;
    double ax=s_sproj[i0].sx, ay=s_sproj[i0].sy;
    double bx=s_sproj[i1].sx, by=s_sproj[i1].sy;
    double cx=s_sproj[i2].sx, cy=s_sproj[i2].sy;
    return (bx-ax)*(cy-ay) - (cx-ax)*(by-ay);
}

static void emit_swtri(int i0,int i1,int i2, uint32_t c0,uint32_t c1,uint32_t c2,
                       int u0,int v0,int u1,int v1,int u2,int v2,
                       int tex,int tpx,int tpy,int clx,int cly,int bpp,int blend) {
    if (s_swtri_n >= MAX_SWTRI) return;
    if (i0 >= s_sproj_n || i1 >= s_sproj_n || i2 >= s_sproj_n) return;
    swtri* t = &s_swtri[s_swtri_n++];
    t->v[0]=(swv){s_sproj[i0].sx,s_sproj[i0].sy,s_sproj[i0].vz,u0,v0,c0,i0};
    t->v[1]=(swv){s_sproj[i1].sx,s_sproj[i1].sy,s_sproj[i1].vz,u1,v1,c1,i1};
    t->v[2]=(swv){s_sproj[i2].sx,s_sproj[i2].sy,s_sproj[i2].vz,u2,v2,c2,i2};
    t->textured=tex; t->tpage_x=tpx; t->tpage_y=tpy; t->clut_x=clx; t->clut_y=cly; t->bpp=bpp; t->blend=blend;
}

static int build_spyro_walk(CPUState* cpu) {
    s_swtri_n = 0;
    if (s_sproj_n <= 0) return 0;
    /* face stream + colour table from the body (seg0) animHdr — shared across
     * segments (the mesh topology is fixed; only vertex positions animate). */
    uint32_t model = cpu->read_word(G_MODELS) | 0x80000000u;
    uint32_t animIdx = s_pose_ovr.active ? s_pose_ovr.anim_idx
                                         : cpu->read_byte(G_SPYRO + 0x18);
    uint32_t animHdr = cpu->read_word(model + 0x38 + animIdx * 4);
    if (animHdr < 0x80000000u) return 0;
    uint32_t faces     = cpu->read_word(animHdr + 0x14);
    uint32_t colorbase = cpu->read_word(animHdr + 0x18);          /* m_Colors */
    if (!s_pose_ovr.active &&                                     /* live player only: */
        (cpu->read_word(G_SPYRO + 0x28) >> 24) != 0)              /* colorFilter.interp */
        colorbase = G_SCRATCH + 0x1D00u;                         /* Fairy-Kiss pre-blend */
    if (faces < 0x80000000u || colorbase < 0x80000000u) return 0;
    uint32_t len = cpu->read_word(faces);
    if (len == 0 || len > 0x20000u) return 0;
    uint32_t fp = faces + 4u, end = faces + 4u + len;
    int guard = 0;
    while (fp + 8u <= end && guard++ < 8192) {
        uint32_t w0 = cpu->read_word(fp);
        uint32_t w1 = cpu->read_word(fp + 4);
        int fresh = ((int32_t)w0) < 0;
        int bit0  = (int)(w0 & 1u);
        int i0 = (int)((w0>>20)&0x7FCu)>>2, i1=(int)((w0>>11)&0x7FCu)>>2, i2=(int)((w0>>2)&0x7FCu)>>2;
        uint32_t c0 = cpu->read_word(colorbase + ((w1>>17)&0x7FCu)) & 0xFFFFFFu;
        uint32_t c1 = cpu->read_word(colorbase + ((w1>> 8)&0x7FCu)) & 0xFFFFFFu;
        uint32_t c2 = cpu->read_word(colorbase + ((w1<< 1)&0x7FCu)) & 0xFFFFFFu;
        uint32_t adv    = fresh ? 0x18u : 0x14u;
        uint32_t uvbase = fresh ? fp + 0xCu : fp + 0x8u;          /* Tiledef words */
        uint32_t td0=cpu->read_word(uvbase), td1=cpu->read_word(uvbase+4), td2=cpu->read_word(uvbase+8);
        int u0=(int)(td0&0xFF), v0=(int)((td0>>8)&0xFF); unsigned cl=(td0>>16)&0xFFFFu;
        int u1=(int)(td1&0xFF), v1=(int)((td1>>8)&0xFF); unsigned tp=(td1>>16)&0xFFFFu;
        int u2=(int)(td2&0xFF), v2=(int)((td2>>8)&0xFF);
        int tpx=(int)(tp&0xFu)*64, tpy=(int)((tp>>4)&1u)*256;
        int mode=(int)((tp>>7)&3u), bpp=(mode==0)?4:(mode==1)?8:16;
        int clx=(int)(cl&0x3Fu)*16, cly=(int)((cl>>6)&0x1FFu);
        /* func_80024404 GT4@0x80024E3C / GT3@0x80024FD8: word1 bit0 -> GPU ABE bit
         * (<<25), so the prim code 0x3C->0x3E / 0x34->0x36 (semi-transparent). Mirror
         * the regular moby walk (build_walk_tris): map the texpage ABR mode. No-op when
         * bit0 is clear (the common opaque case), so it can't regress normal Spyro. */
        int blend=0;
        if (w1 & 1u) {
            int abr=(int)((tp>>5)&3u);   /* texpage semi-transparency mode (ABR 0-3) */
            blend = (abr==0) ? 1 : (abr==1) ? 2 : (abr==2) ? 3 : 2;
            /* ABR-FIXME (audit 2026-06-30): ABR3 (B+F/4) has no native pipe -> drawn
             * full-additive (too bright). Spam if a Spyro-body face ever hits it. */
            if (abr == 3)
                OPL_ERROR_F("native", "ABR-FIXME Spyro body ABR3 (B+F/4) face -> "
                            "drawn full-additive (no quarter-add pipe) f=%u -- FIX spyro_native.c:1087",
                            openpete_frame);
        }
        if (fresh) {
            uint32_t w2 = cpu->read_word(fp + 8);
            int i3 = (int)(w2 & 0x7FCu)>>2;
            uint32_t c3 = cpu->read_word(colorbase + ((w2>>9)&0x7FCu)) & 0xFFFFFFu;
            int u3=(int)((td2>>16)&0xFF), v3=(int)((td2>>24)&0xFF);   /* uv3 = high half of uv2 word */
            if (bit0) {
                /* bit0==1 fresh: the .s skips the NCLIP test entirely
                 * (.L80024D04 bgtz $t6 -> .L80024D70 -> .L80024D98) and ALWAYS
                 * emits the GT4 quad — these faces are pre-marked quads. (Keying
                 * the quad/tri/skip on the per-frame NCLIP sign here was the
                 * dual-keyframe count desync: a bit0==1 face flipped tri<->quad
                 * as Spyro animated.) GT4 -> (v0,v1,v2) + (v1,v3,v2). */
                emit_swtri(i0,i1,i2, c0,c1,c2, u0,v0,u1,v1,u2,v2, 1,tpx,tpy,clx,cly,bpp,blend);
                emit_swtri(i1,i3,i2, c1,c3,c2, u1,v1,u3,v3,u2,v2, 1,tpx,tpy,clx,cly,bpp,blend);
            } else {
                /* bit0==0 fresh: dual-NCLIP decides (.s map, NCLIP on int16 SXY).
                 *   n1>=0 & n2>0  -> tri (v3,v1,v2)
                 *   n1>=0 & n2<=0 -> skip
                 *   n1<0  & n2>0  -> QUAD (v0,v1,v2)+(v1,v3,v2)
                 *   n1<0  & n2<=0 -> tri (v0,v1,v2) */
                double n1 = spyro_nclip(i0,i1,i2);
                double n2 = (i3 < s_sproj_n) ? spyro_nclip(i3,i1,i2) : -1.0;
                if (n1 >= 0.0) {
                    if (n2 > 0.0)   /* single tri (v3,v1,v2) */
                        emit_swtri(i3,i1,i2, c3,c1,c2, u3,v3,u1,v1,u2,v2, 1,tpx,tpy,clx,cly,bpp,blend);
                    /* else skip */
                } else if (n2 > 0.0) {   /* QUAD GT4 -> (v0,v1,v2) + (v1,v3,v2) */
                    emit_swtri(i0,i1,i2, c0,c1,c2, u0,v0,u1,v1,u2,v2, 1,tpx,tpy,clx,cly,bpp,blend);
                    emit_swtri(i1,i3,i2, c1,c3,c2, u1,v1,u3,v3,u2,v2, 1,tpx,tpy,clx,cly,bpp,blend);
                } else {   /* single tri (v0,v1,v2) */
                    emit_swtri(i0,i1,i2, c0,c1,c2, u0,v0,u1,v1,u2,v2, 1,tpx,tpy,clx,cly,bpp,blend);
                }
            }
        } else {   /* strip: single tri, keep if (bit0 ? always : NCLIP1>0) */
            double n1 = spyro_nclip(i0,i1,i2);
            if (bit0 || n1 > 0.0)
                emit_swtri(i0,i1,i2, c0,c1,c2, u0,v0,u1,v1,u2,v2, 1,tpx,tpy,clx,cly,bpp,blend);
        }
        fp += adv;
    }
    return s_swtri_n;
}

/* ---- walk A/B: ordered, index-exact vs the captured prims ----------------
 * The .s emits prims in face order; the walk visits faces in face order and
 * splits quads the same way (GT4 -> (v0,v1,v2)+(v1,v3,v2)), so the two emitted
 * tri sequences align 1:1. We rebuild the EXPECTED tri list from the prims (with
 * exact stream indices recovered via the scratchpad) and compare element-wise:
 * walk[k] vs expected[k] must agree on (RGB, UV); count-OK + rgb/uv is the
 * byte-exact verdict (a wrong vertex carries a different model colour/UV). The
 * `pos` cross-check is confounded by coincident-pixel verts (two of the 238 can
 * share one int16 SXY -> index recovery is ambiguous), so geometry truth is the
 * separate spyro-scratch A/B; here pos is informational only. */
static void spyro_walk_eval(CPUState* cpu, walk_stats* st) {
    memset(st, 0, sizeof(*st)); st->first_bad = -1; st->cnt_ok = 1;
    if (s_gte_n <= 0 || s_sproj_n <= 0) return;
    static struct { int i[3]; uint32_t rgb[3]; int u[3], v[3]; } exp[MAX_SWTRI];
    int en = 0;
    for (int p = 0; p < s_gte_n && en + 2 < MAX_SWTRI; p++) {
        prim_t* pr = &s_gte[p];
        if (pr->nv < 3) continue;
        int pi[4];
        for (int k = 0; k < pr->nv; k++) pi[k] = spyro_index_of(cpu, (int)pr->x[k], (int)pr->y[k]);
        int splits = (pr->nv == 4) ? 2 : 1;
        for (int s = 0; s < splits; s++) {
            int a,b,c;
            if (s == 0) { a=0;b=1;c=2; } else { a=1;b=3;c=2; }   /* GT4 GPU split */
            exp[en].i[0]=pi[a]; exp[en].i[1]=pi[b]; exp[en].i[2]=pi[c];
            exp[en].rgb[0]=pr->rgb[a]; exp[en].rgb[1]=pr->rgb[b]; exp[en].rgb[2]=pr->rgb[c];
            exp[en].u[0]=pr->u[a]; exp[en].v[0]=pr->v[a];
            exp[en].u[1]=pr->u[b]; exp[en].v[1]=pr->v[b];
            exp[en].u[2]=pr->u[c]; exp[en].v[2]=pr->v[c];
            en++;
        }
    }
    int n = s_swtri_n < en ? s_swtri_n : en;
    for (int k = 0; k < n; k++) {
        int pok=1, rok=1, uok=1;
        for (int j = 0; j < 3; j++) {
            int wi = s_swtri[k].v[j].idx, ei = exp[k].i[j];
            if (ei < 0 || wi < 0 ||
                floorf(s_sproj[wi].sx) != floorf(s_sproj[ei].sx) ||
                floorf(s_sproj[wi].sy) != floorf(s_sproj[ei].sy)) pok=0;
            if (s_swtri[k].v[j].rgb != exp[k].rgb[j]) rok=0;
            if (s_swtri[k].v[j].u != exp[k].u[j] || s_swtri[k].v[j].v != exp[k].v[j]) uok=0;
        }
        if (pok) st->pos_ok++; if (rok) st->rgb_ok++; if (uok) st->uv_ok++;
        if (!(rok&&uok) && st->first_bad<0) st->first_bad=k;
    }
    st->walk = s_swtri_n; st->expected = en; st->n = n; st->cnt_ok = (s_swtri_n == en);
}
static void spyro_walk_compare(CPUState* cpu) {
    walk_stats st; spyro_walk_eval(cpu, &st);
    fprintf(stderr, "[spyro-walk f=%u] walk=%d expected=%d (prims=%d) %s pos=%.1f%% rgb=%.1f%% uv=%.1f%%  first_bad=%d\n",
            openpete_frame, st.walk, st.expected, s_gte_n, st.cnt_ok ? "CNT-OK" : "CNT-MISMATCH",
            st.n?100.0*st.pos_ok/st.n:0, st.n?100.0*st.rgb_ok/st.n:0, st.n?100.0*st.uv_ok/st.n:0, st.first_bad);
}

/* ---- scene feed (3b.4 NATIVE WALK) ---------------------------------------
 * Emit the independent face-stream walk's triangles (native float geometry +
 * gamestate colour + static-Tiledef texture) into the shared depth-sorted
 * scene. This RETIRES the prim-driven scaffold: nothing here reads the .s's
 * per-frame prim output — topology/colour/UV come from m_Faces/m_Colors, the
 * geometry from s_sproj (native float projection), the texture from VRAM (a
 * static asset). The only remaining .s coupling is spyro_nclip reading the
 * int16 SXY scratchpad for a byte-exact cull decision; that becomes a native
 * float NCLIP at the Phase-6 switch (the .s geometry turning off). */
static void spyro_feed_walk_ex(CPUState* cpu, int reproject) {
    if (reproject) spyro_project_all(cpu);  /* native float verts -> s_sproj (skip if caller filled it) */
    build_spyro_walk(cpu);                  /* face-stream topology+colour+UV -> s_swtri */
    if (s_swtri_n <= 0) return;
    /* .s OT key, Spyro walk: r_pete keys every SEGMENT at
     * max(0, (segment-origin viewZ >> 7) − g_Spyro.sortDepth) (@80023F8C,
     * lbu 0x27($ra); one OT slot = 128 full-world view-Z units). Like the
     * moby walks' m_DepthOffset, sortDepth is an OT ordering KEY, not
     * geometry — it never enters stored depth (sortDepth is 5 in ordinary
     * play, and mirroring it as a 128·sortDepth world pull showed Spyro
     * through any wall within a constant 640 world). Per-pixel Z over TRUE
     * depth mirrors the per-face OT sort; the sub-slot tie pull below
     * resolves coincident surfaces to the player — ground contact, and the
     * carrier-ride band (the balloonist basket sits ~90 world in front of
     * the rider's body, inside the tie band, so the basket walls/wires
     * don't z-eat him).
     *
     * EXCEPTION: sortDepth SATURATED at 127 is the .s's own "OT slot 0 —
     * draw in front of everything" sentinel (the level-transition flight).
     * That is authored full-front LAYERING, not contact depth: keep the
     * near-plane-capped whole-model pull for exactly that window.
     * OPENPETE_MOBY_OTBIAS=0 disables both (A/B). */
    double sp_pull = 0.0;               /* flyby front-layer window only */
    double tie_pull = 0.0;
    {
        static int s_on = -1;
        if (s_on < 0) { const char* e = getenv("OPENPETE_MOBY_OTBIAS"); s_on = (e && *e) ? (*e != '0') : 1; }
        int sd = (int)(int8_t)cpu->read_byte(G_SPYRO + 0x27u);
        if (s_on && sd == 127) {
            sp_pull = 128.0 * (double)sd;
            /* Cap the pull per FRAME so the nearest vertex lands ON the near
             * floor instead of clamping each vertex at it: a per-vertex clamp
             * flattens everything past the floor to ONE depth and kills
             * self-occlusion (transition-flyby teeth-through-head). */
            double minvz = 1e30;
            for (int i = 0; i < s_swtri_n; i++)
                for (int k = 0; k < 3; k++)
                    if (s_swtri[i].v[k].vz > 0.0f && (double)s_swtri[i].v[k].vz < minvz)
                        minvz = (double)s_swtri[i].v[k].vz;
            if (minvz - sp_pull < 1.0) sp_pull = minvz > 1.0 ? minvz - 1.0 : 0.0;
        } else if (s_on) {
            /* One-slot tie pull, the moby walks' twin: 127 world < one OT
             * slot, so it can only flip pairs og calls a tie. */
            tie_pull = 127.0;
        }
    }
    /* OPENPETE_DEPTHDBG="f0,f1": the Spyro-feed OT-key inputs (sortDepth,
     * applied pull post-cap, vz range) — twin of the moby walk's [mb-pull]. */
    { static int s_dd = -1; static int df0, df1;
      if (s_dd < 0) { const char* e = getenv("OPENPETE_DEPTHDBG");
          s_dd = 0; if (e && sscanf(e, "%d,%d", &df0, &df1) == 2) s_dd = 1; }
      if (s_dd == 1 && (int)openpete_frame >= df0 && (int)openpete_frame <= df1) {
          double mn = 1e30, mx = -1e30;
          for (int i = 0; i < s_swtri_n; i++)
              for (int k = 0; k < 3; k++) {
                  double z = (double)s_swtri[i].v[k].vz;
                  if (z > 0.0) { if (z < mn) mn = z; if (z > mx) mx = z; }
              }
          fprintf(stderr, "[sp-pull f=%u] sortDepth=%d pull=%.0f tie=%.0f "
                  "reproject=%d n=%d vz=%.0f..%.0f\n",
                  openpete_frame, (int)(int8_t)cpu->read_byte(G_SPYRO + 0x27u),
                  sp_pull, tie_pull, reproject, s_swtri_n, mn, mx);
      } }
    openpete_native_scene_begin_frame(openpete_frame);   /* append after cyclorama + mobys */
    openpete_native_scene_set_tag(OPENPETE_TAG_PLAYER(0));
    for (int i = 0; i < s_swtri_n; i++) {
        swtri* w = &s_swtri[i];
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = w->v[k].sx; st.v[k].sy = w->v[k].sy;
            double fvz = (double)w->v[k].vz - sp_pull - tie_pull;
            st.v[k].z  = openpete_native_depth_from_viewz(openpete_worldvz_full((float)fvz));
            /* Perspective W from the UNSHIFTED view-Z. sortDepth saturates at
             * 127 during the level-transition flight (OT slot 0 = in front of
             * everything), which pulls the whole model onto the near plane; a
             * W taken from that depth varies 2:1 across the snout's faces and
             * smears one texel over them. */
            st.v[k].pvz = w->v[k].vz;
            st.v[k].u  = (float)w->v[k].u; st.v[k].v = (float)w->v[k].v;
            st.v[k].rgba = w->v[k].rgb | 0xFF000000u;
        }
        /* OPENPETE_SPYRO_UNTEX=1: bisect hack — draw the whole spyro feed
         * untextured (gouraud only). Separates vertex-path losses from
         * fragment-path losses (texture decode discard). */
        { static int s_ut = -1;
          if (s_ut < 0) { const char* e = getenv("OPENPETE_SPYRO_UNTEX"); s_ut = (e && *e && *e != '0'); }
          if (s_ut) w->textured = 0; }
        st.textured = w->textured; st.tpage_x = w->tpage_x; st.tpage_y = w->tpage_y;
        st.clut_x = w->clut_x; st.clut_y = w->clut_y; st.bpp = w->bpp; st.blend = w->blend;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_PLAYER, 0);
    }
    openpete_native_scene_set_tag(0);
}
static void spyro_feed_walk(CPUState* cpu) { spyro_feed_walk_ex(cpu, 1); }

/* ---- mod actor render service (rung 16): capture + emit -------------------
 * Registered with the core draw service by main.cpp
 * (openpete_mods_draw_set_actor_fns). capture returns the hook-consistent
 * live-pose snapshot; emit re-runs the whole native extraction against a
 * RECORDED pose — decode, walk, project — through the CURRENT camera, and
 * appends the result to this present's scene as a tinted (optionally
 * PSX-average translucent) second player. No game code runs for the ghost;
 * zero guest-RAM writes survive (the seg-scratch swap is restored). */

int spyro1_actor_pose_capture(int kind, void* buf, int cap) {
    if (kind != SPYRO1_ACTOR_KIND_PLAYER || !s_pose_live_valid) return -1;
    if (!buf || cap < (int)sizeof(spyro1_actor_pose_t)) return -1;
    memcpy(buf, &s_pose_live, sizeof(spyro1_actor_pose_t));
    return (int)sizeof(spyro1_actor_pose_t);
}

/* One-shot skip/success diagnostics (draw-service style): the emit fails
 * SILENTLY by design (a stale pose must never crash a present), so name
 * the first instance of each outcome once. */
static void actor_emit_diag(const char* what, unsigned a, unsigned b) {
    static int said[6] = {0};
    int slot = what[0] == 'e' ? 0 : what[0] == 's' ? 1 : what[0] == 'm' ? 2
             : what[0] == 'w' ? 3 : what[0] == 'b' ? 4 : 5;
    if (said[slot]) return;
    said[slot] = 1;
    OPL_INFO_F("native", "actor emit: first %s (%u, %u) f=%u", what, a, b,
               openpete_frame);
}

int spyro1_actor_emit(CPUState* cpu, int kind, const void* pose, int len,
                      uint32_t tint_rgba) {
    if (kind == SPYRO1_ACTOR_KIND_S3_PLAYER)
        return spyro1_s3_actor_emit(cpu, pose, len, tint_rgba);
    if (kind != SPYRO1_ACTOR_KIND_PLAYER || !pose ||
        len < (int)sizeof(spyro1_actor_pose_t)) return 0;
    const spyro1_actor_pose_t* p = (const spyro1_actor_pose_t*)pose;
    if (p->magic != SPYRO1_ACTOR_POSE_MAGIC ||
        p->version != SPYRO1_ACTOR_POSE_VERSION) {
        actor_emit_diag("bad-blob", p->magic, p->version);
        return 0;
    }
    if (!openpete_native_scene_enabled()) return 0;
    /* Scene liveness: append ONLY to a live, recently built scene. During a
     * load stall the renderer holds the last composed frame — begin_frame
     * with a newer stamp would snapshot-and-wipe it (black flash). */
    unsigned sf = openpete_native_scene_frame();
    if (sf == 0xFFFFFFFFu || (unsigned)(openpete_frame - sf) > 6u) {
        actor_emit_diag("scene-stale skip", sf, openpete_frame);
        return 0;
    }
    /* Model validity: the pose's stream cursors point into the Spyro model
     * loaded at capture; a level change reloads g_Models[0] and the pose's
     * cursors dangle. Skip (don't decode garbage). */
    if (p->model != cpu->read_word(G_MODELS)) {
        actor_emit_diag("model-mismatch skip", p->model,
                        cpu->read_word(G_MODELS));
        return 0;
    }

    void* h = openpete_subtick_host_ptr(SEG_SCRATCH);
    if (!h) return 0;
    uint8_t live_seg[SEG_BLOCK];
    memcpy(live_seg, h, SEG_BLOCK);          /* render-transient swap:      */
    memcpy(h, p->seg, SEG_BLOCK);            /* recorded stream state in... */
    s_pose_ovr.active   = 1;
    s_pose_ovr.pos[0]   = p->pos[0];
    s_pose_ovr.pos[1]   = p->pos[1];
    s_pose_ovr.pos[2]   = p->pos[2];
    s_pose_ovr.rot[0]   = p->rot[0];
    s_pose_ovr.rot[1]   = p->rot[1];
    s_pose_ovr.rot[2]   = p->rot[2];
    s_pose_ovr.anim_idx = p->anim_idx;

    spyro_project_all(cpu);                  /* recorded pose, current camera */
    build_spyro_walk(cpu);

    int emitted = 0;
    if (s_swtri_n > 0) {
        unsigned tA = (tint_rgba >> 24) & 0xFFu, tR = (tint_rgba >> 16) & 0xFFu;
        unsigned tG = (tint_rgba >> 8) & 0xFFu,  tB = tint_rgba & 0xFFu;
        openpete_native_scene_begin_frame(sf);   /* same stamp: pure append */
        for (int i = 0; i < s_swtri_n; i++) {
            swtri* w = &s_swtri[i];
            /* Behind-camera / off-screen reject: a ghost pose can sit fully
             * outside the view (the live player never does, so the walk
             * feed never needed this). Saturated projections pin to ±1024
             * corners and NCLIP on pinned coords is arbitrary — the
             * survivors are screen-crossing slivers. Skip any tri touching
             * a saturated vert (the PSX GPU's own >1023 prim reject would
             * have eaten these). */
            int sat = 0;
            for (int k = 0; k < 3; k++) {
                int idx = w->v[k].idx;
                if (idx < 0 || idx >= s_sproj_n || s_sproj[idx].sat) sat = 1;
            }
            if (sat) continue;
            openpete_scene_tri st = {0};
            for (int k = 0; k < 3; k++) {
                st.v[k].sx = w->v[k].sx; st.v[k].sy = w->v[k].sy;
                st.v[k].z  = openpete_native_depth_from_viewz(openpete_worldvz_full(w->v[k].vz));
                st.v[k].u  = (float)w->v[k].u; st.v[k].v = (float)w->v[k].v;
                uint32_t c = w->v[k].rgb;      /* PSX word: R low byte */
                uint32_t r = (((c      ) & 0xFFu) * tR) / 255u;
                uint32_t g = (((c >>  8) & 0xFFu) * tG) / 255u;
                uint32_t b = (((c >> 16) & 0xFFu) * tB) / 255u;
                st.v[k].rgba = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
            st.textured = w->textured; st.tpage_x = w->tpage_x; st.tpage_y = w->tpage_y;
            st.clut_x = w->clut_x; st.clut_y = w->clut_y; st.bpp = w->bpp;
            /* tint alpha < 0xFF: the classic ghost — PSX-average (ABR 0)
             * semi-transparency, exactly the pipe the game's own
             * semi-transparent faces use. Opaque tint keeps model blends. */
            st.blend = (tA < 0xFFu) ? 1 : w->blend;
            openpete_native_scene_add_tri(&st, OPENPETE_SCENE_PLAYER, 0);
            emitted++;
        }
    }

    s_pose_ovr.active = 0;
    memcpy(h, live_seg, SEG_BLOCK);          /* ...and restored: zero net writes */
    if (emitted) actor_emit_diag("emit", (unsigned)emitted, tint_rgba);
    else         actor_emit_diag("walk-empty", (unsigned)s_sproj_n, 0);
    /* OPENPETE_ACTORDBG=1: per-emit projection stats (tris, screen bbox,
     * GTE offsets) — "is the ghost off-screen?" answered by numbers. */
    { static int s_ad = -1;
      if (s_ad < 0) { const char* e = getenv("OPENPETE_ACTORDBG"); s_ad = (e && *e && *e != '0'); }
      if (s_ad) {
          static unsigned s_last = 0;
          if (openpete_frame - s_last >= 100u) {
              s_last = openpete_frame;
              float x0=1e9f,y0=1e9f,x1=-1e9f,y1=-1e9f;
              for (int i = 0; i < s_sproj_n; i++) {
                  if (s_sproj[i].sx<x0) x0=s_sproj[i].sx; if (s_sproj[i].sx>x1) x1=s_sproj[i].sx;
                  if (s_sproj[i].sy<y0) y0=s_sproj[i].sy; if (s_sproj[i].sy>y1) y1=s_sproj[i].sy;
              }
              fprintf(stderr, "[actor-dbg f=%u] tris=%d verts=%d bbox=(%.0f,%.0f)-(%.0f,%.0f) "
                      "ofx=%d ofy=%d H=%u pos=(%.0f,%.0f,%.0f) scene_f=%u\n",
                      openpete_frame, emitted, s_sproj_n, x0, y0, x1, y1,
                      (int32_t)cpu->gte_ctrl[24] >> 16, (int32_t)cpu->gte_ctrl[25] >> 16,
                      cpu->gte_ctrl[26] & 0xFFFF, p->pos[0], p->pos[1], p->pos[2], sf);
          } } }
    return emitted;
}

/* ---- gating --------------------------------------------------------------- */
static int spyro_verify_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SPYRO_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
static unsigned spyro_verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_SPYRO_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

static int on_80023AC4_spyro_native(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = spyro_verify_enabled();
    int feed   = openpete_native_scene_enabled();
    /* --render-paths also needs the bracket so the OT filter knows this path's
     * polybuf range, even when not feeding/verifying. Mods loaded also arms
     * the bracket: the actor-pose snapshot below needs it (headless too —
     * capture_actor_pose is sim-side; drawing is what needs vk). Modless
     * stock runs keep the inert one-branch fall-through. */
    if (!verify && !feed && !render_paths_active() && !openpete_mods_active)
        return 0;  /* inert: normal dispatch runs it */
    static int s_in = 0; if (s_in) return 0; s_in = 1;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    { OP_PROF_ACCUM_SCOPE("spyro.srend");
      func_80023AC4(cpu); }                      /* run the real renderer */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_SPYRO, startp, endp);   /* polybuf range for the OT filter */

    /* Actor-pose snapshot (mod actor render service): only when the render
     * actually emitted Spyro — a suppressed render (cutscene) leaves the
     * scratch stale, so the last drawn pose stays the capture answer. */
    if (openpete_mods_active && endp > startp)
        spyro_pose_snapshot(cpu);

    static int hookdbg = -1;
    if (hookdbg < 0) { const char* e = getenv("OPENPETE_CANON_CAPDBG"); hookdbg = (e && *e && *e != '0'); }
    if (hookdbg && (openpete_frame % 200u) == 0)
        fprintf(stderr, "[spyro-hook f=%u] feed=%d endp-startp=%u rp=%d\n",
                openpete_frame, feed, endp - startp, render_path_enabled(RP_SPYRO));

    if (feed && endp > startp && render_path_enabled(RP_SPYRO)) {  /* native feed (if selected) */
        spyro_feed_walk(cpu);                     /* independent face-stream walk: native verts+topology+colour+UV */
        spyro1_s3_actor_test_emit(cpu);           /* OPENPETE_S3_ACTOR_TEST harness (inert unless set) */
        /* Sub-tick spine: snapshot the per-segment scratch (keyframe stream
         * pointers + vert counts) and Spyro's projected SXY (the NCLIP cull
         * verdict), both live now, so intermediates can re-derive Spyro. */
        spyro_native_canon_capture_range(cpu, 0x1900u, 0x100u);
        /* Fairy-Kiss pre-blend colour table (G_SCRATCH+0x1D00): produced by
         * func_80023AC4 ABOVE when g_Spyro.colorFilter.interp != 0. It lives inside
         * the [0x1600,0x2400) record region, so the sub-tick's s_canon_recs restore
         * (captured at the earlier MOBY hook, BEFORE this Spyro pass wrote it) leaves
         * it STALE — Spyro's own colours then flicker at >30fps during a Fairy-Kiss.
         * Capture it fresh HERE and overlay it for the Spyro feeder (like the seg
         * scratch). Read range is [0x1D00,0x1EEC) (max colour index 0x1E8); 0x300
         * covers it with margin and stays clear of SHADED@0x2400. */
        spyro_native_canon_capture_range(cpu, 0x1D00u, 0x300u);
        spyro_native_canon_capture_sxy(cpu);
        spyro_native_canon_capture_camera(cpu, SPYRO_PROD_SPYRO);
        spyro_anim_capture_seg(cpu);   /* roll prev/curr seg block for sub-tick blend */
    }

    if (verify) {
        static int s_dumped = 0, s_reports = 0;
        if (openpete_frame >= spyro_verify_frame() && endp > startp) {
            capture_prims(cpu, startp, endp);
            if (!s_dumped) { spyro_dump(cpu, startp, endp); spyro_phaseA_dump(cpu); s_dumped = 1; }
            spyro_project_all(cpu);                  /* native decode + project -> s_sproj */
            build_spyro_walk(cpu);                   /* 3b.4 independent face-stream walk */
            if (s_reports < 200) {                   /* detailed spot-check (first 200 frames) */
                spyro_compare_geometry();            /* prim-NN A/B (geometry verdict) */
                spyro_scratchpad_ab(cpu);            /* clean per-vertex A/B vs scratchpad */
                spyro_idx_audit(cpu);                /* index-recovery vs NN disagreement */
                spyro_walk_compare(cpu);             /* walk topology+colour+UV vs prims (ordered) */
                spyro_matrix_compare(cpu);           /* Phase-A derived M+TR vs scratch block */
                s_reports++;
            }
            spyro_aggregate(cpu);                    /* cheap per-frame full-run (20k) verdict */
        }
    }
    s_in = 0;
    return 1;
}

/* Sub-tick re-extract (high-FPS spine): reproduce the player from the current
 * gamestate + the interpolated g_Camera the orchestrator wrote — extract-only
 * (no func_80023AC4 call, no verdict writes). spyro_feed_walk reads g_Camera
 * (→ smooth orientation) and g_Spyro (position lerp lands in step 3). */
void spyro_player_native_reextract(CPUState* cpu) {
    if (!openpete_native_scene_enabled()) return;
    /* Only re-extract the gameplay Spyro on sub-ticks if the canonical frame
     * actually drew him (func_80023AC4 emitted prims → RP_SPYRO recorded). In
     * dragon-rescue cutscenes the gameplay-Spyro render is SUPPRESSED and Spyro is
     * shown as a moby (cls 511) instead; without this gate the spine would re-extract
     * the gameplay Spyro from g_Spyro state on every intermediate present, painting a
     * SECOND Spyro over the cutscene moby (same position, different pose) — the
     * duplicate-Spyro ghost. Mirrors moby_native_reextract's RP_MOBY gate. */
    if (!render_path_canon_drawn(RP_SPYRO)) return;

    /* Sub-tick animation = two-pass keyframe-SHAPE interpolation. The earlier
     * one-decode "mixed seg block" approach interpolated by decoding a keyframe
     * pair at sub-tick blends the game NEVER rendered. For a fast animation (the
     * L1/R1 roll advances a whole keyframe per tick) that whipped seam-relative
     * verts off-model — stretched body/head/tail polys (whichever segment moved
     * fastest that tick), worst near the keyframe knot. The seg entries are also
     * cumulatively chained (fd[23:14] is a running scratchpad offset), so a
     * per-segment prev/curr mix corrupts the vertex counts.
     *
     * Instead decode the two REAL rendered poses — the prev seg block at its real
     * blend and the curr seg block at its real blend, each a pose the game
     * actually drew with its own internally-consistent seam — and LERP the
     * projected vertices. No un-rendered blend, no per-segment seam/count mismatch.
     * Rotation + position interpolate via their own brackets (applied identically
     * to both passes), so this lerps only the SHAPE; the lerp of two real poses is
     * always on-model. */
    int two_pass = 0;
    if (s_anim_on && s_seg_valid >= 2) {
        void* h = openpete_subtick_host_ptr(SEG_SCRATCH);
        if (h) {
            uint8_t live[SEG_BLOCK];
            memcpy(live, h, SEG_BLOCK);                 /* the canonical (curr) seg block */
            memcpy(h, s_seg_prev, SEG_BLOCK);           /* pass 1: PREV rendered pose */
            spyro_project_all(cpu);
            int pn = s_sproj_n;
            memcpy(s_sproj_prev, s_sproj, (size_t)pn * sizeof(s_sproj[0]));
            memcpy(h, s_seg_curr, SEG_BLOCK);           /* pass 2: CURR rendered pose */
            spyro_project_all(cpu);                     /* s_sproj = curr */
            if (s_sproj_n == pn) {                      /* index-matched (same mesh/order) → lerp shape */
                double a = s_anim_alpha;
                for (int i = 0; i < pn; i++) {
                    s_sproj[i].sx = (float)(s_sproj_prev[i].sx + (s_sproj[i].sx - s_sproj_prev[i].sx) * a);
                    s_sproj[i].sy = (float)(s_sproj_prev[i].sy + (s_sproj[i].sy - s_sproj_prev[i].sy) * a);
                    s_sproj[i].vz = (float)(s_sproj_prev[i].vz + (s_sproj[i].vz - s_sproj_prev[i].vz) * a);
                }
            }
            memcpy(h, live, SEG_BLOCK);                 /* restore the live scratch */
            spyro_feed_walk_ex(cpu, 0);                 /* build + emit from the lerped s_sproj (no reproject) */
            two_pass = 1;
        }
    }
    if (!two_pass) spyro_feed_walk(cpu);
    spyro1_s3_actor_test_emit(cpu);   /* test harness re-emits on sub-ticks too */

    static int ad = -1;
    if (ad < 0) { const char* e = getenv("OPENPETE_ANIMDBG"); ad = (e && *e && *e != '0'); }
    if (ad && s_sproj_n > 20)
        fprintf(stderr, "[anim-v f=%u] a=%.3f n=%d v10=(%.1f,%.1f) vN-8=(%.1f,%.1f)\n",
                openpete_frame, s_anim_alpha, s_sproj_n, s_sproj[10].sx, s_sproj[10].sy,
                s_sproj[s_sproj_n-8].sx, s_sproj[s_sproj_n-8].sy);
    static int feeddbg = -1;
    if (feeddbg < 0) { const char* e = getenv("OPENPETE_REEXTRACT_FEEDDBG"); feeddbg = (e && *e && *e != '0'); }
    if (feeddbg) {
        static unsigned last = 0xFFFFFFFFu;
        if (openpete_frame != last) { last = openpete_frame;
            fprintf(stderr, "[reextract-spyro f=%u] sproj=%d swtri=%d\n",
                    openpete_frame, s_sproj_n, s_swtri_n); }
    }
}

static const feature_dispatch_entry_t spyro_dispatch[] = {
    { 0x80023AC4u, on_80023AC4_spyro_native },
    { 0u, NULL },
};

const feature_module_t spyro_native_module = {
    .name           = "spyro_native",
    .apply_options  = NULL,
    .dispatch_table = spyro_dispatch,
};
