// games/spyro-1/features/moby_native.c — Phase 3 of
// notes/plans/openpete-native-renderer.md: the MOBY (3D actor) renderer
// re-derived from gamestate in float + its autonomous numerical A/B harness.
//
// Mobys are the keystone of Phase 3: textured, animated, three materials
// (regular/specular/shaded). This module is the game-specific *scene
// extraction* half — it reads the per-moby render records the cull pass
// (func_8001F158) built in the g_SonyImage scratch (D_8006FCF4+0x1600),
// decodes each moby's packed vertices + per-face Tiledefs, float-projects
// (mirroring runner/src/gte.cpp RTPS, now WITH a real TR translation —
// unlike the Phase-2 sky dome), and produces screen-space triangles.
//
// Verification (instruments over pixels): hook the material renderer
// (func_8001F798 = REGULAR first; the conflict-free, most-common path),
// capture the *actual* prims it emits to the polybuf as ground truth, run
// the native float extraction on the same records, compare positions +
// colours + UV + tpage/clut per vertex. Headless, numerical: the autonomous
// Phase-3 verdict. Gated by OPENPETE_MOBY_VERIFY.
//
// Determinism: never writes PSX RAM. When active it calls the real render
// function exactly once (behaviour-identical to the normal dispatch
// fall-through) and only READS the records + polybuf afterwards.
//
// Format map: the format notes (asm ground
// truth). Exact record/stream/prim byte offsets are confirmed EMPIRICALLY
// by the one-shot hex dump below (falsify-before-ship), as in Phase 2.

#include "feature_registry.h"
#include "psx_runtime.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "openpete_native_render.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void     func_8001F798(CPUState* cpu);  /* the real REGULAR moby renderer */
extern void     func_80020F34(CPUState* cpu);  /* the real SPECULAR moby renderer (same +0x1600 geometry) */
extern void     func_80022A2C(CPUState* cpu);  /* the real SHADED moby renderer (+0x2400 Moby* list) */
extern unsigned openpete_frame;

static const char* s_cur_mat = "REG";   /* material tag for the current pass (REG/SPEC) */

#define D_SONY        0x8006FCF4u   /* g_SonyImage — reused as moby render scratch */
#define REC_BASE      (D_SONY + 0x1600u) /* cull/gather records, stride 0x38 */
#define POLYBUF_CURSOR 0x800757B0u
#define G_MODELS      0x80076378u   /* Model* g_Models[512] */
#define G_CAMERA      0x80076DD0u
#define G_CYCLORAMA   0x80078A40u

#define REC_STRIDE    0x38u
#define MAX_RECORDS   512
#define SHADED_BASE   (D_SONY + 0x2400u)  /* shaded renderer: flat Moby* array (stride 4) */

/* ---- captured prim (ground truth, decoded from the polybuf) --------- */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (prim[0x7]) */
    uint8_t  len;           /* OT tag length (prim[0]>>24) */
    int      nv;            /* 3 or 4 */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad;
    uint32_t raw[20];       /* first words, for the hex dump */
    int      nraw;
} prim_t;

#define MAX_PRIMS 8192
static prim_t s_gte[MAX_PRIMS]; static int s_gte_n;
/* Cumulative prim-code coverage across the whole run (missing-poly diagnosis):
 * which GPU codes F798 emits, and which we DON'T decode (= dropped from feed). */
static int s_code_hist[256]; static int s_uncoded_hist[256];

/* ---- helpers (shared with the cyclorama harness conventions) -------- */
static void read_matrix(CPUState* cpu, uint32_t p, int M[3][3]) {
    uint32_t w0 = cpu->read_word(p), w1 = cpu->read_word(p + 4);
    uint32_t w2 = cpu->read_word(p + 8), w3 = cpu->read_word(p + 12);
    uint32_t w4 = cpu->read_word(p + 16);
    M[0][0] = (int16_t)(w0 & 0xFFFF); M[0][1] = (int16_t)(w0 >> 16); M[0][2] = (int16_t)(w1 & 0xFFFF);
    M[1][0] = (int16_t)(w1 >> 16);    M[1][1] = (int16_t)(w2 & 0xFFFF); M[1][2] = (int16_t)(w2 >> 16);
    M[2][0] = (int16_t)(w3 & 0xFFFF); M[2][1] = (int16_t)(w3 >> 16);  M[2][2] = (int16_t)(w4 & 0xFFFF);
}

/* Float RTPS mirror WITH translation: MAC_i = TR[i] + (R_i . V)>>12 ;
 * SX = OFX + IR1*H/SZ3. Returns view-Z (MAC3). (gte.cpp gte_rtps_internal.) */
static double project(const int M[3][3], const int TR[3], double vx, double vy, double vz,
                      double ofx_px, double ofy_px, double H,
                      double* out_sx, double* out_sy, int* out_sat) {
    double mac1 = (double)TR[0] + (M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0;
    double mac2 = (double)TR[1] + (M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0;
    double mac3 = (double)TR[2] + (M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0;
    double z = (mac3 != 0.0) ? mac3 : 1.0;
    *out_sx = ofx_px + mac1 * H / z;
    *out_sy = ofy_px + mac2 * H / z;
    if (out_sat) *out_sat = (mac1 > 32767.0 || mac1 < -32768.0 ||
                             mac2 > 32767.0 || mac2 < -32768.0 ||
                             mac3 <= 0.0 || H >= 2.0 * mac3);
    return mac3;
}

/* ---- ground-truth capture: decode prims F798 emitted ---------------- */
/* Walk by OT tag length (stride=((w0>>24)+1)*4) — robust regardless of the
 * exact per-code layout. Decode XY/RGB/UV by code byte (prim[0x7]); the
 * offsets are confirmed by the hex dump. */
static void capture_gte(CPUState* cpu, uint32_t start, uint32_t end) {
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
            /* GT3 (0x34, 10w) / GT4 (0x3C, 13w): per-vertex blocks of
             * {rgb|code, xy, uv|clut-or-tpage}. xy0 @ word[2], xy1 @ [5],
             * xy2 @ [8], xy3 @ [11]. (Confirmed by the hex dump.) */
            /* General PSX polygon decode by command bits (bit4=gouraud,
             * bit3=quad, bit2=textured). Handles every moby variant uniformly:
             * GT3/GT4 (0x34/0x3C), G3/G4 (0x30/0x38), FT3/FT4 (0x24/0x2C, incl.
             * the +0x02 semi-transparent 0x26/0x2E), F3/F4. Layout: word[1] =
             * rgb0|code, then per vertex {rgb if gouraud&&k>0}{xy}{uv if tex};
             * clut rides v0's uv-word hi, tpage rides v1's. Bit-identical to the
             * old hardcoded GT3/GT4 path, so the verified match is preserved. */
            if ((code & 0xE0) == 0x20) {
                int gouraud = (code & 0x10) != 0;
                int quad    = (code & 0x08) != 0;
                int tex     = (code & 0x04) != 0;
                t->gouraud = gouraud; t->textured = tex; t->quad = quad;
                t->nv = quad ? 4 : 3;
                uint32_t rgb0 = t->raw[1] & 0xFFFFFF;
                int c = 2;                          /* word cursor past rgb0|code */
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
            if (t->nv == 0) s_uncoded_hist[code & 0xFF]++;  /* not fed = missing polys */
            s_gte_n++;
        }
        p += stride;
    }
}

/* Cumulative prim-code coverage line: every code F798 has emitted so far, and
 * which we still DON'T decode (those are dropped from the native feed = missing
 * polys). "UNDECODED: none" across a whole demo run means the feed is complete
 * for the regular material. */
static void report_code_hist(void) {
    char hb[400]; int hp = 0; hb[0] = 0;
    for (int c = 0; c < 256; c++) if (s_code_hist[c] && hp < (int)sizeof(hb) - 16)
        hp += snprintf(hb + hp, sizeof(hb) - hp, " %02X=%d", c, s_code_hist[c]);
    char ub[200]; int up = 0; ub[0] = 0;
    for (int c = 0; c < 256; c++) if (s_uncoded_hist[c] && up < (int)sizeof(ub) - 16)
        up += snprintf(ub + up, sizeof(ub) - up, " %02X=%d", c, s_uncoded_hist[c]);
    fprintf(stderr, "[moby-codes f=%u] cumulative F798 codes:%s | UNDECODED(dropped):%s\n",
            openpete_frame, hb, up ? ub : " none");
}

/* ---- one-shot hex dump (falsify-before-ship) ------------------------ */
static void dump_words(CPUState* cpu, const char* label, uint32_t base, int nwords) {
    fprintf(stderr, "  %s @%08X:\n   ", label, base);
    for (int i = 0; i < nwords; i++) {
        fprintf(stderr, "%08X ", cpu->read_word(base + (uint32_t)i*4));
        if ((i & 7) == 7) fprintf(stderr, "\n   ");
    }
    fprintf(stderr, "\n");
}

static void hex_dump(CPUState* cpu, uint32_t start, uint32_t end) {
    fprintf(stderr, "[moby-dump f=%u] F798 prims [%08X,%08X) %u bytes  OFX=%d OFY=%d H=%u\n",
            openpete_frame, start, end, end - start,
            (int32_t)cpu->gte_ctrl[24] >> 16, (int32_t)cpu->gte_ctrl[25] >> 16,
            cpu->gte_ctrl[26] & 0xFFFF);

    /* Records: first 4, with interpreted fields + followed pointers. */
    for (int r = 0; r < 4; r++) {
        uint32_t rec = REC_BASE + (uint32_t)r * REC_STRIDE;
        uint32_t w0 = cpu->read_word(rec);
        uint32_t frame_p = cpu->read_word(rec + 0x04);
        uint32_t faces_p = cpu->read_word(rec + 0x08);
        uint32_t color_p = cpu->read_word(rec + 0x0C);
        int32_t trx = (int32_t)cpu->read_word(rec + 0x10);
        int32_t tr_y = (int32_t)cpu->read_word(rec + 0x14);
        int32_t trz = (int32_t)cpu->read_word(rec + 0x18);
        uint32_t mobyp = cpu->read_word(rec + 0x34);
        fprintf(stderr,
            "[rec %d @%08X] w0=%08X frame=%08X faces=%08X color=%08X TR=(%d,%d,%d) moby=%08X\n",
            r, rec, w0, frame_p, faces_p, color_p, trx, tr_y, trz, mobyp);
        if (w0 == 0 && frame_p == 0 && mobyp == 0) { fprintf(stderr, "  (terminator)\n"); break; }
        int M[3][3]; read_matrix(cpu, rec + 0x1C, M);
        fprintf(stderr, "  R=[%d %d %d | %d %d %d | %d %d %d]\n",
                M[0][0],M[0][1],M[0][2], M[1][0],M[1][1],M[1][2], M[2][0],M[2][1],M[2][2]);
        dump_words(cpu, "record", rec, 14);
        if (frame_p >= 0x80000000u && frame_p < 0x80800000u) dump_words(cpu, "->frame", frame_p, 12);
        if (faces_p >= 0x80000000u && faces_p < 0x80800000u) dump_words(cpu, "->faces", faces_p, 12);
        if (color_p >= 0x80000000u && color_p < 0x80800000u) dump_words(cpu, "->color", color_p, 8);
    }

    /* Prims: first 10 raw, with code/len. */
    int n = s_gte_n < 10 ? s_gte_n : 10;
    for (int i = 0; i < n; i++) {
        prim_t* t = &s_gte[i];
        fprintf(stderr, "[prim %d] code=%02X len=%u :", i, t->code, t->len);
        for (int k = 0; k < t->nraw; k++) fprintf(stderr, " %08X", t->raw[k]);
        fprintf(stderr, "\n");
    }
    /* Code histogram. */
    int hist[256]; memset(hist, 0, sizeof(hist));
    for (int i = 0; i < s_gte_n; i++) hist[s_gte[i].code]++;
    fprintf(stderr, "[moby-dump] prim count=%d  codes:", s_gte_n);
    for (int c = 0; c < 256; c++) if (hist[c]) fprintf(stderr, " %02X=%d", c, hist[c]);
    fprintf(stderr, "\n");
}

/* ---- per-moby native vertex decode + projection -------------------- */
/* One moby vertex stream (frame A or B): packed 11/11/10 literals interleaved
 * with 5/5/5 short-deltas, the bit-0 flag of each word selecting the next. A
 * literal RESETS the accumulator; a short ADDS. */
typedef struct { uint32_t litp, shortp; int VX, VY, VZ, flag, s5; } vstream;
static void vstream_init(CPUState* cpu, vstream* s, uint32_t frame, int s5) {
    uint32_t fw0 = cpu->read_word(frame), fw1 = cpu->read_word(frame + 4);
    s->litp   = (fw0 & 0x1FFFFFu) | 0x80000000u;
    s->shortp = s->litp + ((fw1 >> 24) & 0xFFu) * 4u;
    s->VX = s->VY = s->VZ = s->flag = 0; s->s5 = s5;
}
static void vstream_next(CPUState* cpu, vstream* s, int first) {
    if (first || s->flag == 0) {
        uint32_t w = cpu->read_word(s->litp); s->litp += 4;
        s->VZ = (int32_t)w >> 21;
        s->VX = (int32_t)(w << 10) >> 21;
        s->VY = (int32_t)(w << 20) >> 19;
        s->flag = (int)(w & 1u);
    } else {
        int16_t d = (int16_t)cpu->read_half(s->shortp); s->shortp += 2;
        s->VZ += ((int32_t)d >> 11) << s->s5;                       /* sra11 */
        s->VX -= ((int32_t)((uint32_t)(int32_t)d << 21) >> 27) << s->s5;
        s->VY -= ((int32_t)((uint32_t)(int32_t)d << 26) >> 27) << s->s5;
        s->flag = (int)((uint16_t)d & 1u);
    }
}

/* Decode + float-project one record's vertices. Mirrors F798: the no-blend path
 * (block 8001FCC8) when the keyframe blend factor is 0, else the INTPL blend
 * path (block 8001FA44) -- decode BOTH frame A and frame B streams and lerp each
 * vertex in MODEL space by blend/4096 before RTPS (the asm INTPLs IR(=A) toward
 * FC(=B) by IR0(=blend) then RTPSs the result). Writes screen XY + view-Z.
 * Returns vertex count. The caller set-matches these vs the emitted prims. */
typedef struct { float sx, sy, vz; int sat; int rec; } proj_xy;  /* vz=view-Z; rec=source record (diag) */

static int moby_project_record(CPUState* cpu, uint32_t rec, proj_xy* out, int cap) {
    uint32_t header = cpu->read_word(rec + 0x04);
    uint32_t frameA = cpu->read_word(rec + 0x08);
    if (header < 0x80000000u || frameA < 0x80000000u) return 0;
    /* C2_TR (projection translation) = (TR_stored << 2) >> m_Scale  [F798
     * @8001F858/8001F8C8]. m_Scale is the per-model shrink factor (AnimationHeader
     * +0x05): 2 = identity (the common case), 1 = ×2 size, 3 = /2, etc. Without
     * this, models whose m_Scale != 2 mis-project (the multi-moby A/B residual).
     * (This is the a3 = w0&0xFF == 0 path; the rare a3!=0 GPF-TR path is a TODO.) */
    int m_scale = cpu->read_byte(header + 5) & 0xFF;
    int TR[3];
    TR[0] = (((int32_t)cpu->read_word(rec + 0x10)) << 2) >> m_scale;
    TR[1] = (((int32_t)cpu->read_word(rec + 0x14)) << 2) >> m_scale;
    TR[2] = (((int32_t)cpu->read_word(rec + 0x18)) << 2) >> m_scale;
    int M[3][3]; read_matrix(cpu, rec + 0x1C, M);
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFFu);

    int s5    = (cpu->read_byte(header + 6) & 0xFF) + 1;
    int count = cpu->read_byte(header + 8) & 0xFF;   /* gte_ctrl[30] sign adj = 0 here */
    if (count > cap) count = cap;

    /* Keyframe blend: IR0 = (rec.w0 & 0xFF00) >> 2 (1.0=4096); frame B at +0x0C.
     * 0 (or B absent) => the no-blend path (frame A only). */
    uint32_t frameB = cpu->read_word(rec + 0x0C);
    int blend = (int)((cpu->read_word(rec) & 0xFF00u) >> 2);
    int do_blend = (blend > 0 && frameB >= 0x80000000u);
    double t = (double)blend / 4096.0;

    vstream A, B;
    vstream_init(cpu, &A, frameA, s5);
    if (do_blend) vstream_init(cpu, &B, frameB, s5);
    for (int i = 0; i < count; i++) {
        vstream_next(cpu, &A, i == 0);
        double vx = A.VX, vy = A.VY, vz = A.VZ;
        if (do_blend) {
            vstream_next(cpu, &B, i == 0);
            vx += t * (B.VX - A.VX);
            vy += t * (B.VY - A.VY);
            vz += t * (B.VZ - A.VZ);
        }
        double sx, sy; int sat;
        double vzz = project(M, TR, vx, vy, vz, ofx, ofy, H, &sx, &sy, &sat);
        out[i].sx = (float)sx; out[i].sy = (float)sy; out[i].vz = (float)vzz; out[i].sat = sat;
    }
    return count;
}

#define MAX_PROJ 8192
static proj_xy s_proj[MAX_PROJ]; static int s_proj_n;

/* Per-record metadata for the independent face-stream walk (3a.2b): where this
 * record's vertices live in s_proj[] (filled in stream order, so a face's stream
 * vertex index k maps to s_proj[proj_off + k]) + the face-stream base. */
typedef struct { uint32_t addr; int proj_off, proj_n; uint32_t header, faceblock, w0; } rec_meta;
static rec_meta s_recs[MAX_RECORDS]; static int s_nrecs;

/* Project EVERY live record's vertices into s_proj[] (native float decode +
 * RTPS+TR). Shared by the verify A/B and the scene feed. Returns record count. */
static int s_max_blend;   /* max keyframe-blend factor (rec.w0 IR0) across this frame's records */
static int project_all_records(CPUState* cpu) {
    s_proj_n = 0; s_max_blend = 0; s_nrecs = 0;
    int nrec = 0;
    for (int r = 0; r < MAX_RECORDS; r++) {
        uint32_t rec = REC_BASE + (uint32_t)r * REC_STRIDE;
        if (cpu->read_word(rec + 0x04) == 0 && cpu->read_word(rec + 0x34) == 0) break;
        uint32_t w0 = cpu->read_word(rec);
        int bl = (int)((w0 & 0xFF00u) >> 2);  /* INTPL blend 0..4096 */
        if (bl > s_max_blend) s_max_blend = bl;
        int off = s_proj_n;
        int got = moby_project_record(cpu, rec, s_proj + s_proj_n, MAX_PROJ - s_proj_n);
        for (int j = 0; j < got; j++) s_proj[s_proj_n + j].rec = nrec;  /* tag source record */
        if (nrec < MAX_RECORDS) {
            uint32_t header = cpu->read_word(rec + 0x04);
            s_recs[nrec] = (rec_meta){ rec, off, got, header,
                /* HP face stream base = AnimationHeader+0x14 (LP path TODO) */
                (header >= 0x80000000u) ? cpu->read_word(header + 0x14) : 0u, w0 };
        }
        s_proj_n += got; nrec++;
        if (s_proj_n >= MAX_PROJ) break;
    }
    s_nrecs = nrec;
    return nrec;
}

/* ---- native scene triangle (fed to panel 0) ------------------------------
 * Filled by the 3a.2b independent face-stream walk (below). The 3a.2a
 * nearest-neighbour prim scaffold it replaced was removed when the walk was
 * promoted (2026-06-15); the walk is jitter-free (exact word0 indices) where
 * the scaffold's screen-proximity matching jittered. */
typedef struct { float sx, sy, vz; int u, v; uint32_t rgb; } ntri_vert;  /* vz=depth; u,v=texel */
typedef struct {
    ntri_vert v[3];
    int textured, tpage_x, tpage_y, clut_x, clut_y, bpp;  /* PSX material (VRAM px) */
    int blend;   /* 0 = opaque, 1 = semi-transparent (prim ABE bit) */
} ntri_t;
#define MAX_NTRI (MAX_PRIMS * 2)              /* GT4 quad -> 2 tris */

/* ---- 3a.2b: INDEPENDENT gamestate face-stream walk -----------------------
 * Derive triangle TOPOLOGY from the moby face stream itself (not the captured
 * prims). Each face's word0 packs three stream-vertex indices (byte offsets
 * into the per-record SXY scratchpad; >>2 = the s_proj index); textured faces
 * bake a Tiledef at fp-0xC/-0x8/-0x4; per-vertex colour comes from the fog
 * scratchpad (g_SonyImage+0x1100), word0-indexed. This replaces the 3a.2a
 * nearest-neighbour scaffold whose screen-proximity matching was the measured
 * cause of the 55%-NN-ambiguous jitter -- here connectivity is exact.
 *
 * The face-record stride is software-pipelined in the .s (2 faces in flight),
 * so the per-shape strides are validated EMPIRICALLY: compare_walk_vs_prims
 * A/Bs the walked triangles vs the captured prims while func_8001F798 is still
 * the oracle -- the match% is the desync detector (plan 2026-06-15 method).
 * Offsets/flags: the format notes (face/emit appendix).
 * word0: sign=fresh(<0)/strip(>=0), &0x1=quad-enable, &0x2=textured,
 * &0x4=sprite-FT4, &0x8=skip. */

#define COLOR_SCRATCH (D_SONY + 0x1100u)   /* per-vertex fogged RGB (.s DPCS output) */

static ntri_t s_wtri[MAX_NTRI]; static int s_wtri_n;

/* signed 2*area of a screen-space triangle (NCLIP analog) -- backface/winding. */
static double tri_nclip(float ax,float ay,float bx,float by,float cx,float cy) {
    return (double)(bx-ax)*(cy-ay) - (double)(cx-ax)*(by-ay);
}
/* Same but on FLOORED coords, mirroring the GTE's int16 SXY NCLIP (op 0x06) so
 * the quad/tri/skip sign decisions match the .s exactly at near-zero areas. */
static double tri_nclip_i(const proj_xy* a, const proj_xy* b, const proj_xy* c) {
    return tri_nclip(floorf(a->sx),floorf(a->sy), floorf(b->sx),floorf(b->sy), floorf(c->sx),floorf(c->sy));
}

static struct {   /* walk diagnostics (printed by compare_walk_vs_prims) */
    int faces, emitted, oor, skip, sprite, strip, fresh_g, fresh_t, quad;
    int align_lo, align_hi, desync;
} s_walkdiag;

static void emit_wtri(proj_xy* a, proj_xy* b, proj_xy* c,
                      uint32_t ca, uint32_t cb, uint32_t cc,
                      int ua,int va,int ub,int vb,int uc,int vc,
                      int tex,int tpx,int tpy,int clx,int cly,int bpp,int blend) {
    if (s_wtri_n >= MAX_NTRI) return;
    ntri_t* t = &s_wtri[s_wtri_n++];
    t->v[0]=(ntri_vert){a->sx,a->sy,a->vz,ua,va,ca};
    t->v[1]=(ntri_vert){b->sx,b->sy,b->vz,ub,vb,cb};
    t->v[2]=(ntri_vert){c->sx,c->sy,c->vz,uc,vc,cc};
    t->textured=tex;t->tpage_x=tpx;t->tpage_y=tpy;t->clut_x=clx;t->clut_y=cly;t->bpp=bpp;t->blend=blend;
    s_walkdiag.emitted++;
}

/* Build s_wtri[] from the independent face-stream walk. Returns triangle count.
 * No backface cull yet (the winding sign is determined from the A/B's nclip
 * split first); quads emit both sub-tris when quad-enable is set. */
static int build_walk_tris(CPUState* cpu) {
    s_wtri_n = 0;
    memset(&s_walkdiag, 0, sizeof(s_walkdiag));
    for (int r = 0; r < s_nrecs; r++) {
        rec_meta* R = &s_recs[r];
        uint32_t fb = R->faceblock;
        if (fb < 0x80000000u || R->proj_n <= 0) continue;
        uint32_t bytecount = cpu->read_word(fb);
        if (bytecount == 0 || bytecount > 0x10000u) { s_walkdiag.desync++; continue; }
        uint32_t end_lo = fb + bytecount;          /* doc: fp + fp[0]        */
        uint32_t end_hi = fb + 4u + bytecount;     /* C:  (fb+4) + fp[0]     */
        uint32_t end = end_hi;                      /* generous; alignment check below */
        uint32_t fp = fb + 4u;
        int base = R->proj_off, vcount = R->proj_n;
        /* per-vertex colour base: near tier = model colour array (AnimationHeader
         * +0x18); fog tiers = the DPCS-lit scratch (D_SONY+0x1100). Indexed by w1f.
         * (Tier select is a ZSF4 test; near tier covers the demo mobys -- if a
         * record is fog-tier its colour will mismatch in the A/B and flag it.) */
        uint32_t cbase = (R->header >= 0x80000000u) ? cpu->read_word(R->header + 0x18) : 0u;
        if (cbase < 0x80000000u) cbase = COLOR_SCRATCH;   /* fall back to fog scratch */
        int guard = 0;
        while (fp + 8u <= end && guard++ < 8192) {
            uint32_t w0 = cpu->read_word(fp);
            s_walkdiag.faces++;
            int fresh    = ((int32_t)w0) < 0;
            int textured = (w0 & 0x2u) != 0;
            int sprite   = fresh && (w0 & 0x4u);
            int skip     = (w0 & 0x8u) != 0;
            uint32_t adv = sprite ? 0x14u
                         : fresh  ? (textured ? 0x18u : 0x0Cu)
                         :          (textured ? 0x14u : 0x08u);   /* strip */
            if (sprite)        s_walkdiag.sprite++;
            else if (!fresh)   s_walkdiag.strip++;
            else if (textured) s_walkdiag.fresh_t++;
            else               s_walkdiag.fresh_g++;
            if (skip)   s_walkdiag.skip++;
            if (skip || sprite) { fp += adv; continue; }   /* sprite TODO; advance to stay aligned */

            uint32_t bo0 = (w0 >> 20) & 0x7FCu, bo1 = (w0 >> 11) & 0x7FCu, bo2 = (w0 >> 2) & 0x7FCu;
            int i0 = (int)bo0 >> 2, i1 = (int)bo1 >> 2, i2 = (int)bo2 >> 2;
            uint32_t v4w = cpu->read_word(fp + 8);
            int i3 = (int)(v4w & 0x7FCu) >> 2;   /* 4th-vertex SXY index = (fp+8)&0x7FC (>>9 is the COLOUR idx, not SXY) */
            if (i0 >= vcount || i1 >= vcount || i2 >= vcount) { s_walkdiag.oor++; fp += adv; continue; }
            proj_xy* p0 = &s_proj[base+i0]; proj_xy* p1 = &s_proj[base+i1]; proj_xy* p2 = &s_proj[base+i2];

            /* FRESH faces run the .s's dual-NCLIP (block_8002008C): NCLIP1=(v0,v1,v2),
             * then v3 is loaded into SXY0 so NCLIP2=(v3,v1,v2). bit0(=w0f&1) flips the
             * sign test (the `neg` dance). Outcomes:
             *   t6a>=0 && t6b>0  -> QUAD: emit (v0,v1,v2)+(v1,v3,v2)
             *   t6a>=0 && t6b<=0 -> skip the whole face
             *   t6a<0  && t6b>0  -> tri-2nd: emit (v3,v1,v2)   (.L80020158)
             *   t6a<0  && t6b<=0 -> tri-1st: emit (v0,v1,v2)   (.L800203F4 s1=0)
             * STRIPS (.L8002031C) are single triangles, keep if NCLIP>0. */
            proj_xy* p3 = (i3 < vcount) ? &s_proj[base+i3] : NULL;
            double ncl1 = tri_nclip_i(p0,p1,p2);
            int emit_a = 0, emit_b = 0, b_is_v3tri = 0;
            if (fresh) {
                double ncl2 = p3 ? tri_nclip_i(p3,p1,p2) : -1.0;
                int bit0 = (int)(w0 & 1u);
                double t6a = (bit0 && ncl1 > 0.0) ? -ncl1 : ncl1;
                double t6b = (bit0 && ncl2 >= 0.0) ? -ncl2 : ncl2;
                if (t6a >= 0.0) { if (t6b > 0.0 && p3) { emit_a = emit_b = 1; s_walkdiag.quad++; } }
                else            { if (t6b > 0.0 && p3) { emit_b = b_is_v3tri = 1; } else emit_a = 1; }
            } else {
                /* strip: single tri. bit0=0 keep NCLIP>0; bit0=1 keep NCLIP!=0
                 * (.L800203B8 vs .L800203D0 -- bit0 strips carry either winding). */
                int bit0 = (int)(w0 & 1u);
                if (bit0 ? (ncl1 != 0.0) : (ncl1 > 0.0)) emit_a = 1;
            }
            if (!emit_a && !emit_b) { fp += adv; continue; }

            /* per-vertex colour: cbase indexed by w1f (NOT w0f). */
            uint32_t w1 = cpu->read_word(fp + 4);
            uint32_t c0 = cpu->read_word(cbase + ((w1 >> 17) & 0x7FCu)) & 0xFFFFFFu;
            uint32_t c1 = cpu->read_word(cbase + ((w1 >>  8) & 0x7FCu)) & 0xFFFFFFu;
            uint32_t c2 = cpu->read_word(cbase + ((w1 <<  1) & 0x7FCu)) & 0xFFFFFFu;
            uint32_t c3 = c2;   /* v3 colour approx; topology A/B measures position */

            int tpx=0,tpy=0,clx=0,cly=0,bpp=16, u[4]={0,0,0,0}, v[4]={0,0,0,0};
            if (textured) {
                uint32_t td0 = cpu->read_word(fp + adv - 0xCu);
                uint32_t td1 = cpu->read_word(fp + adv - 0x8u);
                uint32_t td2 = cpu->read_word(fp + adv - 0x4u);
                u[0]=(int)(td0&0xFFu); v[0]=(int)((td0>>8)&0xFFu);  unsigned cl=(td0>>16)&0xFFFFu;
                u[1]=(int)(td1&0xFFu); v[1]=(int)((td1>>8)&0xFFu);  unsigned tp=(td1>>16)&0xFFFFu;
                u[2]=(int)(td2&0xFFu); v[2]=(int)((td2>>8)&0xFFu);
                u[3]=(int)((td2>>16)&0xFFu); v[3]=(int)((td2>>24)&0xFFu);   /* quad u3v3 */
                tpx=(int)(tp&0xFu)*64; tpy=(int)((tp>>4)&1u)*256;
                int mode=(int)((tp>>7)&3u); bpp=(mode==0)?4:(mode==1)?8:16;
                clx=(int)(cl&0x3Fu)*16; cly=(int)((cl>>6)&0x1FFu);
            }
            int blend = 0;   /* ABE bit not in word0; 3a.3 sets it from tpage */

            if (emit_a)
                emit_wtri(p0,p1,p2, c0,c1,c2, u[0],v[0],u[1],v[1],u[2],v[2],
                          textured,tpx,tpy,clx,cly,bpp,blend);
            if (emit_b) {
                if (b_is_v3tri)          /* tri-2nd (v3,v1,v2) */
                    emit_wtri(p3,p1,p2, c3,c1,c2, u[3],v[3],u[1],v[1],u[2],v[2],
                              textured,tpx,tpy,clx,cly,bpp,blend);
                else                     /* quad 2nd half (v1,v3,v2) */
                    emit_wtri(p1,p3,p2, c1,c3,c2, u[1],v[1],u[3],v[3],u[2],v[2],
                              textured,tpx,tpy,clx,cly,bpp,blend);
            }
            fp += adv;
        }
        if      (fp == end_lo) s_walkdiag.align_lo++;
        else if (fp == end_hi) s_walkdiag.align_hi++;
        else                   s_walkdiag.desync++;
    }
    return s_wtri_n;
}

/* A/B the walked triangles vs the captured prims (topology desync detector).
 * Build prim-triangles from s_gte (quads split), match each walked tri to a
 * prim tri by order-independent vertex coincidence (<=2px), report precision
 * (walk tris that hit a prim) + recall (prims covered) + the nclip-sign split
 * of matched-vs-unmatched walk tris (to choose the backface-cull winding). */
typedef struct { float x[3], y[3]; uint32_t rgb[3]; int u[3], v[3]; int tex; } ptri;
#define MAX_PTRI (MAX_PRIMS*2)
static ptri s_ptri[MAX_PTRI]; static int s_ptri_n;
static void build_prim_tris(void) {
    s_ptri_n = 0;
    for (int g = 0; g < s_gte_n; g++) {
        prim_t* P = &s_gte[g];
        if (P->nv < 3 || s_ptri_n >= MAX_PTRI) continue;
        ptri* a = &s_ptri[s_ptri_n++];
        for (int k=0;k<3;k++){a->x[k]=P->x[k];a->y[k]=P->y[k];a->rgb[k]=P->rgb[k];a->u[k]=P->u[k];a->v[k]=P->v[k];}
        a->tex=P->textured;
        if (P->nv==4 && s_ptri_n<MAX_PTRI) {            /* quad TL,TR,BL,BR -> (1,3,2) */
            ptri* b=&s_ptri[s_ptri_n++]; int idx[3]={1,3,2};
            for (int k=0;k<3;k++){int j=idx[k];b->x[k]=P->x[j];b->y[k]=P->y[j];b->rgb[k]=P->rgb[j];b->u[k]=P->u[j];b->v[k]=P->v[j];}
            b->tex=P->textured;
        }
    }
}
/* best order-independent max-vertex distance of walk tri w vs prim tri p (greedy). */
static double tri_match_cost(const ntri_t* w, const ptri* p) {
    static const int perm[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    double bestmax=1e18;
    for (int pi=0;pi<6;pi++){
        double mx=0;
        for (int k=0;k<3;k++){
            double dx=w->v[k].sx-p->x[perm[pi][k]], dy=w->v[k].sy-p->y[perm[pi][k]];
            double d=sqrt(dx*dx+dy*dy); if(d>mx)mx=d;
        }
        if(mx<bestmax)bestmax=mx;
    }
    return bestmax;
}
static void compare_walk_vs_prims(void) {
    build_prim_tris();
    static int s_cmp_dumped = 0; int s_cmp_dump = !s_cmp_dumped; s_cmp_dumped = 1;
    int wn=s_wtri_n, pn=s_ptri_n;
    int wmatch=0, neg_m=0, pos_m=0, neg_u=0, pos_u=0;
    int mt_p=0,mt_n=0,mf_p=0,mf_n=0;  /* matched nclip split: textured/flat (winding rule) */
    int colmatch=0, coln=0, uvmatch=0, uvn=0;
    /* On-screen subset (centroid within the disp rect): the off-screen extras
     * the .s clip-rejects are scissored by panel 0 anyway, so the visually-
     * meaningful match is the on-screen one. */
    int won=0, wmatch_on=0, won_un=0;   /* on-screen walk tris / matched / unmatched-onscreen */
    #define ONSCR(cx,cy) ((cx)>=0.0&&(cx)<=512.0&&(cy)>=0.0&&(cy)<=240.0)
    char pmatched[MAX_PTRI]; if(pn>MAX_PTRI)pn=MAX_PTRI; memset(pmatched,0,pn);
    for (int wi=0;wi<wn;wi++){
        ntri_t* w=&s_wtri[wi];
        double wcx=(w->v[0].sx+w->v[1].sx+w->v[2].sx)/3.0, wcy=(w->v[0].sy+w->v[1].sy+w->v[2].sy)/3.0;
        double best=1e18; int bp=-1;
        for (int pi=0;pi<pn;pi++){
            double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
            double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
            if (fabs(pcx-wcx)>32.0||fabs(pcy-wcy)>32.0) continue;        /* centroid prune */
            double c=tri_match_cost(w,&s_ptri[pi]);
            if (c<best){best=c;bp=pi;}
        }
        double ncl=tri_nclip(w->v[0].sx,w->v[0].sy,w->v[1].sx,w->v[1].sy,w->v[2].sx,w->v[2].sy);
        int won_this = ONSCR(wcx,wcy);
        if (won_this) won++;
        if (best<=4.0 && bp>=0){
            wmatch++; pmatched[bp]=1;
            if (won_this) wmatch_on++;
            if(ncl<0)neg_m++;else pos_m++;
            if(w->textured){ if(ncl<0)mt_n++;else mt_p++; } else { if(ncl<0)mf_n++;else mf_p++; }
            /* colour + uv match vs the matched prim tri (vertex 0, coarse) */
            uint32_t wc=w->v[0].rgb&0xFFFFFFu, pc=s_ptri[bp].rgb[0]&0xFFFFFFu;
            coln++; if (wc==pc) colmatch++;
            if (w->textured){ uvn++; if (w->v[0].u==s_ptri[bp].u[0] && w->v[0].v==s_ptri[bp].v[0]) uvmatch++; }
        } else {
            if(ncl<0)neg_u++;else pos_u++;
            if(won_this) won_un++;
            if (won_this && s_cmp_dump && bp>=0) {   /* show a few on-screen misses + nearest prim */
                static int nd=0;
                if (nd++ < 8) fprintf(stderr,
                    "  [miss] w(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) best=%.1f primTri(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) tex=%d\n",
                    w->v[0].sx,w->v[0].sy,w->v[1].sx,w->v[1].sy,w->v[2].sx,w->v[2].sy, best,
                    s_ptri[bp].x[0],s_ptri[bp].y[0],s_ptri[bp].x[1],s_ptri[bp].y[1],s_ptri[bp].x[2],s_ptri[bp].y[2], w->textured);
            }
        }
    }
    int pcov=0, pon=0, pcov_on=0;
    for(int pi=0;pi<pn;pi++){
        pcov+=pmatched[pi];
        double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
        double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
        if(ONSCR(pcx,pcy)){ pon++; if(pmatched[pi]) pcov_on++; }
    }
    fprintf(stderr,
        "[moby-walk %s f=%u] recs=%d faces=%d walkTris=%d primTris=%d | "
        "ONSCREEN precision=%d/%d(%.1f%%) recall=%d/%d(%.1f%%) onscrUnmatched=%d | "
        "precision=%d/%d(%.1f%%) recall=%d/%d(%.1f%%) | ",
        s_cur_mat, openpete_frame, s_nrecs, s_walkdiag.faces, wn, pn,
        wmatch_on, won, won?100.0*wmatch_on/won:0.0, pcov_on, pon, pon?100.0*pcov_on/pon:0.0, won_un,
        wmatch, wn, wn?100.0*wmatch/wn:0.0, pcov, pn, pn?100.0*pcov/pn:0.0);
    fprintf(stderr,
        "[moby-walk2 f=%u] "
        "shapes{freshG=%d freshT=%d strip=%d sprite=%d skip=%d quad=%d oor=%d} "
        "align{lo=%d hi=%d desync=%d} | nclip matched{-=%d +=%d} unmatched{-=%d +=%d} "
        "matchedTex{-=%d +=%d} matchedFlat{-=%d +=%d} | col=%d/%d uv=%d/%d\n",
        openpete_frame,
        s_walkdiag.fresh_g, s_walkdiag.fresh_t, s_walkdiag.strip, s_walkdiag.sprite,
        s_walkdiag.skip, s_walkdiag.quad, s_walkdiag.oor,
        s_walkdiag.align_lo, s_walkdiag.align_hi, s_walkdiag.desync,
        neg_m,pos_m,neg_u,pos_u, mt_n,mt_p,mf_n,mf_p, colmatch,coln, uvmatch,uvn);
    #undef ONSCR
}

/* Append the moby triangles to the native scene (panel 0). Frame-keyed begin
 * composes with the cyclorama (drawn first, owns the bg fill) — mobys append
 * on top. No depth buffer yet (3a.3), so submission order is the painter's
 * order: sky, then mobys. Determinism-safe: host-side display only. */
static void push_scene_moby(void) {
    static int s_logged = 0;
    if (!s_logged && s_wtri_n > 0) {
        s_logged = 1;
        fprintf(stderr, "[moby-native] feeding panel 0: %d moby tris from gamestate "
                "(3a.2b independent face-stream walk) frame %u\n", s_wtri_n, openpete_frame);
    }
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_wtri_n; i++) {
        openpete_scene_tri t;
        for (int k = 0; k < 3; k++) {
            t.v[k].sx   = s_wtri[i].v[k].sx;
            t.v[k].sy   = s_wtri[i].v[k].sy;
            t.v[k].z    = openpete_native_depth_from_viewz(s_wtri[i].v[k].vz);
            t.v[k].u    = (float)s_wtri[i].v[k].u;
            t.v[k].v    = (float)s_wtri[i].v[k].v;
            t.v[k].rgba = s_wtri[i].v[k].rgb | 0xFF000000u;
        }
        t.textured = s_wtri[i].textured;
        t.tpage_x  = s_wtri[i].tpage_x; t.tpage_y = s_wtri[i].tpage_y;
        t.clut_x   = s_wtri[i].clut_x;  t.clut_y  = s_wtri[i].clut_y;
        t.bpp      = s_wtri[i].bpp;
        t.blend    = s_wtri[i].blend;
        openpete_native_scene_add_tri(&t);
    }
}

/* ---- projection A/B: native decode+project vs the emitted prim verts --
 * Nearest-neighbour match each captured GTE prim vertex against the native
 * s_proj set (the Phase-2 instrument). The prims carry the authoritative final
 * SXY; a close match proves the decode + RTPS+TR projection reproduce the GTE.
 * Requires project_all_records() already called (s_proj filled). */
static void compare_and_report(int nrec) {
    if (!s_proj_n) { fprintf(stderr, "[moby-verify f=%u] no records\n", openpete_frame); return; }

    int gv_n = 0, near_n = 0, floor_ok = 0, ambig = 0, divergent_n = 0;
    int tex_n = 0, tex_m = 0, flat_n = 0, flat_m = 0;  /* CLEAN (non-divergent) match split */
    double sum = 0, mx = 0, worst = -1, max_tiejump = 0;
    float wgx=0,wgy=0; int worst_code = 0;
    int rmatch[16] = {0}, rnat[16] = {0};   /* per-record diag: which moby mis-projects */
    for (int i = 0; i < s_proj_n; i++) if ((unsigned)s_proj[i].rec < 16) rnat[s_proj[i].rec]++;
    for (int g = 0; g < s_gte_n; g++) {
        prim_t* P = &s_gte[g];
        int it = P->textured;
        for (int k = 0; k < P->nv; k++) {
            float gx = P->x[k], gy = P->y[k];
            gv_n++;
            /* Nearest native vert INCLUDING saturated ones, so a prim vert maps
             * to its true native vert even when the GTE divide/IR clamp made the
             * two diverge; we then bin those out (sanctioned, exactly as the
             * cyclorama harness did) rather than score them as decode failures.
             * Near mobys (small view-Z, H>=2*SZ3) hit this a lot. */
            double best = 1e18, best2 = 1e18; int bn_sat = 1, bn_rec = 0; float bnx=0, bny=0, bnx2=0, bny2=0;
            for (int i = 0; i < s_proj_n; i++) {
                double dx = s_proj[i].sx - gx, dy = s_proj[i].sy - gy;
                double d = dx*dx + dy*dy;
                if (d < best) { best2 = best; bnx2 = bnx; bny2 = bny;
                                best = d; bnx = s_proj[i].sx; bny = s_proj[i].sy; bn_sat = s_proj[i].sat; bn_rec = s_proj[i].rec; }
                else if (d < best2) { best2 = d; bnx2 = s_proj[i].sx; bny2 = s_proj[i].sy; }
            }
            double dist = sqrt(best);
            if (bn_sat) { divergent_n++; continue; }   /* near-Z / IR-clamp divergence: expected */
            if (it) tex_n++; else flat_n++;            /* clean denominator */
            if (dist <= 4.0) {
                near_n++; sum += dist; if (dist > mx) mx = dist;
                if (it) tex_m++; else flat_m++;
                if ((unsigned)bn_rec < 16) rmatch[bn_rec]++;
                if (fabs(floor((double)bnx) - gx) <= 1.0 && fabs(floor((double)bny) - gy) <= 1.0)
                    floor_ok++;
                /* NN-ambiguity (jitter proxy): 2nd-nearest within 1px of nearest
                 * -> NN can flip frame-to-frame (the scaffold jitter; 3a.2b fixes). */
                if (best2 < 1e17 && sqrt(best2) - dist < 1.0) {
                    ambig++;
                    double jx = bnx2 - bnx, jy = bny2 - bny, jump = sqrt(jx*jx + jy*jy);
                    if (jump > max_tiejump) max_tiejump = jump;
                }
            }
            if (dist > worst && dist < 1e17) { worst = dist; wgx=gx; wgy=gy; worst_code = P->code; }
        }
    }
    int n_tex = 0, n_flat = 0;   /* per-frame split: confirms the non-textured (G3/G4) decode is exercised */
    for (int g = 0; g < s_gte_n; g++)
        if (s_gte[g].nv >= 3) { if (s_gte[g].textured) n_tex++; else n_flat++; }
    int clean = tex_n + flat_n;   /* prim verts whose nearest native vert is non-divergent */
    /* REVERSE check (decode-error vs slot-order discriminator): how many native
     * s_proj verts have NO prim vertex within 2px? Forward NN is order-blind and
     * hides wrong decoded verts; if this is ~0 every native vert is a real one
     * (so walk misses are pure slot/topology), else the decode emits bad verts. */
    int sp_unmatched = 0, sp_checked = 0;
    for (int i = 0; i < s_proj_n; i++) {
        if (s_proj[i].sat) continue;
        if (s_proj[i].sx < 0 || s_proj[i].sx > 512 || s_proj[i].sy < 0 || s_proj[i].sy > 240) continue;
        sp_checked++;
        double best = 1e18;
        for (int g = 0; g < s_gte_n; g++) for (int k = 0; k < s_gte[g].nv; k++) {
            double dx = s_gte[g].x[k]-s_proj[i].sx, dy = s_gte[g].y[k]-s_proj[i].sy;
            double d = dx*dx+dy*dy; if (d < best) best = d;
        }
        if (best > 4.0) sp_unmatched++;   /* >2px (squared 4) -> a native vert no prim has */
    }
    fprintf(stderr, "[moby-rev f=%u] onscreen native verts with NO prim within 2px: %d/%d "
            "(>0 => decode emits wrong verts; ~0 => misses are slot/topology)\n",
            openpete_frame, sp_unmatched, sp_checked);
    fprintf(stderr,
        "[moby-verify f=%u] recs=%d blend=%d primVerts=%d divergent(satNN)=%d | clean-match(<=4px)=%d/%d(%.1f%%) "
        "mean=%.3fpx | texMatch=%d/%d(%.1f%%) flatMatch=%d/%d(%.1f%%) | "
        "NN-amb=%d(%.1f%%) maxTieJump=%.1fpx | worst gte(%.0f,%.0f) d=%.1f code=%02X | "
        "prims=%d(tex=%d flat=%d)\n",
        openpete_frame, nrec, s_max_blend, gv_n, divergent_n, near_n, clean, clean?100.0*near_n/clean:0.0,
        near_n?sum/near_n:0.0,
        tex_m, tex_n, tex_n?100.0*tex_m/tex_n:0.0,
        flat_m, flat_n, flat_n?100.0*flat_m/flat_n:0.0,
        ambig, near_n?100.0*ambig/near_n:0.0, max_tiejump,
        wgx,wgy, worst, worst_code, s_gte_n, n_tex, n_flat);
    /* Per-record: native vert count + how many prim verts NN-matched to it.
     * A record with many native verts but ~0 matches is mis-projected (the
     * residual). w0 = the record flags word (LP/HP + per-axis sign). */
    if (nrec <= 8) {
        fprintf(stderr, "[moby-recs f=%u]", openpete_frame);
        for (int r = 0; r < nrec && r < 16; r++)
            fprintf(stderr, " rec%d{nat=%d hit=%d}", r, rnat[r], rmatch[r]);
        fprintf(stderr, "\n");
    }
}

/* ---- dispatch hook -------------------------------------------------- */
static int moby_verify_enabled(void) {
    static int s = -1;
    if (s < 0) { const char* v = getenv("OPENPETE_MOBY_VERIFY"); s = (v && v[0] && v[0] != '0') ? 1 : 0; }
    return s;
}
static unsigned moby_verify_frame(void) {
    static unsigned f = 0; static int init = 0;
    if (!init) { const char* v = getenv("OPENPETE_MOBY_VERIFY_FRAME"); f = v ? (unsigned)atoi(v) : 2000u; init = 1; }
    return f;
}

/* Shared moby pass: run the real renderer (REGULAR or SPECULAR -- both loop the
 * SAME +0x1600 record buffer with identical vertex decode + RTPS projection +
 * face walk; only the per-face highlight COLOUR differs, which is display-side),
 * capture its prims, re-derive native float geometry, feed + A/B. Determinism-
 * safe: calls the real render exactly once, only reads gamestate afterward. */
static void moby_native_pass(CPUState* cpu, void (*render_fn)(CPUState*), const char* mat) {
    int verify = moby_verify_enabled();
    int feed   = openpete_native_render_enabled();
    s_cur_mat = mat;

    uint32_t start = cpu->read_word(POLYBUF_CURSOR);
    render_fn(cpu);                            /* run the real renderer */
    uint32_t end = cpu->read_word(POLYBUF_CURSOR);

    capture_gte(cpu, start, end);
    int nrec = project_all_records(cpu);      /* native float verts -> s_proj */

    if (feed) {
        build_walk_tris(cpu);                 /* 3a.2b independent topology (promoted over the scaffold) */
        push_scene_moby();
    }

    if (verify) {
        static int s_dumped = 0, s_reports = 0;
        int target = (openpete_frame >= moby_verify_frame() && s_gte_n > 0 &&
                      (openpete_frame % 200u == 0) && s_reports < 60);  /* sample across levels */
        if (target && !s_dumped) { hex_dump(cpu, start, end); s_dumped = 1; }
        if (target) {
            compare_and_report(nrec);
            build_walk_tris(cpu);             /* 3a.2b independent walk + topology A/B */
            compare_walk_vs_prims();
            s_reports++;
        }
        static unsigned s_codes_at = 0;
        if (s_gte_n > 0 && openpete_frame >= s_codes_at + 2000) {
            s_codes_at = openpete_frame; report_code_hist();
        }
    }
}

static int on_8001F798_moby_native(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!moby_verify_enabled() && !openpete_native_render_enabled()) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    moby_native_pass(cpu, func_8001F798, "REG");   /* regular material */
    s_in = 0;
    return 1;
}
static int on_80020F34_moby_native(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!moby_verify_enabled() && !openpete_native_render_enabled()) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    moby_native_pass(cpu, func_80020F34, "SPEC");  /* specular: same geometry, +0x1600 base */
    s_in = 0;
    return 1;
}

/* ---- SHADED material (func_80022A2C) -- layout-confirming dump (3a.4.3) ----
 * Shaded is structurally different from regular/specular: it loops a flat
 * Moby* array @ D_SONY+0x2400, decodes 3-signed-byte (×2) verts, and rebuilds
 * R per-moby = ProjMatrix · Rz·Ry·Rx(m_Rotation). Before implementing that,
 * dump the data layout to confirm the RE offsets empirically (falsify-first).
 * func_80022A2C writes m_WasDrawn (verdict) -- we call it exactly once, like
 * the other hooks, so determinism is unchanged. */
static void shaded_dump(CPUState* cpu, uint32_t start, uint32_t end) {
    fprintf(stderr, "[shaded-dump f=%u] prims [%08X,%08X) %u bytes\n",
            openpete_frame, start, end, end - start);
    /* +0x2400 Moby* list */
    fprintf(stderr, "  list@%08X:", SHADED_BASE);
    int nm = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t m = cpu->read_word(SHADED_BASE + (uint32_t)i*4);
        fprintf(stderr, " %08X", m);
        if (m == 0) break;
        nm++;
    }
    fprintf(stderr, "  (%d mobys)\n", nm);
    /* first 2 live mobys + their models */
    for (int i = 0; i < 2; i++) {
        uint32_t m = cpu->read_word(SHADED_BASE + (uint32_t)i*4);
        if (m < 0x80000000u) break;
        int32_t px = (int32_t)cpu->read_word(m + 0x0C);
        int32_t py = (int32_t)cpu->read_word(m + 0x10);
        int32_t pz = (int32_t)cpu->read_word(m + 0x14);
        uint32_t cls = cpu->read_half(m + 0x36);
        uint32_t rot = cpu->read_word(m + 0x44);
        uint32_t rad = cpu->read_byte(m + 0x50);
        uint32_t wasdrawn = cpu->read_byte(m + 0x51);
        fprintf(stderr, "  moby%d @%08X pos=(%d,%d,%d) class=%u rot=%08X radius=%u wasDrawn=%u\n",
                i, m, px,py,pz, cls, rot, rad, wasdrawn);
        uint32_t model = cpu->read_word(G_MODELS + cls*4);
        fprintf(stderr, "   model[%u]=%08X:", cls, model);
        if (model >= 0x80000000u) for (int w = 0; w < 12; w++)
            fprintf(stderr, " %08X", cpu->read_word(model + (uint32_t)w*4));
        fprintf(stderr, "\n");
    }
    /* camera ProjMatrix (+0x00..0x10) + position (+0x28..0x30) */
    fprintf(stderr, "  cam projM:");
    for (int w = 0; w < 5; w++) fprintf(stderr, " %08X", cpu->read_word(G_CAMERA + (uint32_t)w*4));
    fprintf(stderr, "  pos=(%d,%d,%d)\n",
            (int32_t)cpu->read_word(G_CAMERA + 0x28), (int32_t)cpu->read_word(G_CAMERA + 0x2C),
            (int32_t)cpu->read_word(G_CAMERA + 0x30));
    /* captured shaded prims (codes 0x20/28/22/2A expected) */
    int hist[256]; memset(hist, 0, sizeof(hist));
    for (int g = 0; g < s_gte_n; g++) hist[s_gte[g].code]++;
    fprintf(stderr, "  shaded prim codes:");
    for (int c = 0; c < 256; c++) if (hist[c]) fprintf(stderr, " %02X=%d", c, hist[c]);
    fprintf(stderr, " (total=%d)\n", s_gte_n);
    for (int g = 0; g < 4 && g < s_gte_n; g++) {
        prim_t* t = &s_gte[g];
        fprintf(stderr, "  [sprim %d] code=%02X len=%u :", g, t->code, t->len);
        for (int k = 0; k < t->nraw; k++) fprintf(stderr, " %08X", t->raw[k]);
        fprintf(stderr, "\n");
    }
}

static int on_80022A2C_moby_native(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!moby_verify_enabled()) return 0;   /* dump-only for now; no feed yet */
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_80022A2C(cpu);                      /* run the real SHADED renderer (writes m_WasDrawn) */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);
    static int s_dumped = 0;
    if (!s_dumped && openpete_frame >= moby_verify_frame() && endp > startp) {
        capture_gte(cpu, startp, endp);
        shaded_dump(cpu, startp, endp);
        s_dumped = 1;
    }
    s_in = 0;
    return 1;
}

static const feature_dispatch_entry_t moby_dispatch[] = {
    { 0x8001F798u, on_8001F798_moby_native },
    { 0x80020F34u, on_80020F34_moby_native },
    { 0x80022A2Cu, on_80022A2C_moby_native },
    { 0u, NULL },
};

const feature_module_t moby_native_module = {
    .name           = "moby_native",
    .apply_options  = NULL,
    .dispatch_table = moby_dispatch,
};
