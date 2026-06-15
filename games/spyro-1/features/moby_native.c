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
extern unsigned openpete_frame;

#define D_SONY        0x8006FCF4u   /* g_SonyImage — reused as moby render scratch */
#define REC_BASE      (D_SONY + 0x1600u) /* cull/gather records, stride 0x38 */
#define POLYBUF_CURSOR 0x800757B0u
#define G_MODELS      0x80076378u   /* Model* g_Models[512] */
#define G_CAMERA      0x80076DD0u
#define G_CYCLORAMA   0x80078A40u

#define REC_STRIDE    0x38u
#define MAX_RECORDS   512

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

/* Project EVERY live record's vertices into s_proj[] (native float decode +
 * RTPS+TR). Shared by the verify A/B and the scene feed. Returns record count. */
static int s_max_blend;   /* max keyframe-blend factor (rec.w0 IR0) across this frame's records */
static int project_all_records(CPUState* cpu) {
    s_proj_n = 0; s_max_blend = 0;
    int nrec = 0;
    for (int r = 0; r < MAX_RECORDS; r++) {
        uint32_t rec = REC_BASE + (uint32_t)r * REC_STRIDE;
        if (cpu->read_word(rec + 0x04) == 0 && cpu->read_word(rec + 0x34) == 0) break;
        int bl = (int)((cpu->read_word(rec) & 0xFF00u) >> 2);  /* INTPL blend 0..4096 */
        if (bl > s_max_blend) s_max_blend = bl;
        int got = moby_project_record(cpu, rec, s_proj + s_proj_n, MAX_PROJ - s_proj_n);
        for (int j = 0; j < got; j++) s_proj[s_proj_n + j].rec = nrec;  /* tag source record */
        s_proj_n += got; nrec++;
        if (s_proj_n >= MAX_PROJ) break;
    }
    return nrec;
}

/* ---- native triangles for the scene feed (3a.2) --------------------------
 * SCAFFOLD topology: the independent gamestate face-stream walk is the next
 * 3a.2 increment (built + A/B-verified against the prims while the .s is still
 * the oracle, then promoted here — NOT deferred to Phase 6). Until then the
 * triangle TOPOLOGY comes from the captured prims (the .s already walked the
 * pipelined, variable-stride stream) while the VERTEX POSITIONS are native
 * float: each prim vertex is nearest-neighbour matched to the verified s_proj
 * set, so the fed geometry is native (sub-pixel; widescreen/interp-ready),
 * just not yet topology-independent. Per-vertex colour is the prim's fogged
 * RGB (the .s's DPCS output; float DPCS is 3a.4). */
typedef struct { float sx, sy, vz; int u, v; uint32_t rgb; } ntri_vert;  /* vz=depth; u,v=texel */
typedef struct {
    ntri_vert v[3];
    int textured, tpage_x, tpage_y, clut_x, clut_y, bpp;  /* PSX material (VRAM px) */
    int blend;   /* 0 = opaque, 1 = semi-transparent (prim ABE bit) */
} ntri_t;
#define MAX_NTRI (MAX_PRIMS * 2)              /* GT4 quad -> 2 tris */
static ntri_t s_ntri[MAX_NTRI]; static int s_ntri_n;

/* Nearest non-saturated native vertex to a prim screen point; -1 if none. */
static int nearest_native(float gx, float gy) {
    double best = 1e18; int bi = -1;
    for (int i = 0; i < s_proj_n; i++) {
        if (s_proj[i].sat) continue;
        double dx = s_proj[i].sx - gx, dy = s_proj[i].sy - gy;
        double d = dx*dx + dy*dy;
        if (d < best) { best = d; bi = i; }
    }
    return bi;
}

/* Build s_ntri[] from the captured prims (requires s_proj already filled).
 * Returns the count of prim vertices that found NO native match (fallback to
 * the prim's own GTE XY) — a headless health metric for the feed: 0 means every
 * fed vertex is genuinely native. */
static int build_native_tris(void) {
    s_ntri_n = 0;
    int fallback = 0;
    for (int g = 0; g < s_gte_n; g++) {
        prim_t* P = &s_gte[g];
        if (P->nv < 3) continue;
        float fx[4], fy[4], fz[4];
        for (int k = 0; k < P->nv; k++) {
            int idx = nearest_native(P->x[k], P->y[k]);
            if (idx >= 0) { fx[k] = s_proj[idx].sx; fy[k] = s_proj[idx].sy; fz[k] = s_proj[idx].vz; }
            else          { fx[k] = P->x[k];        fy[k] = P->y[k]; fz[k] = 4096.0f; fallback++; }
        }
        /* PSX material from the prim's Tiledef halfwords -> VRAM-pixel coords.
         * tpage: X=bits0-3 *64, Y=bit4 *256, colour-mode=bits7-8 (0=4/1=8/2=16bpp).
         * clut:  X=bits0-5 *16, Y=bits6-14. */
        int tex = P->textured, tpx=0, tpy=0, clx=0, cly=0, bpp=16;
        if (tex) {
            unsigned tp = (unsigned)P->tpage, cl = (unsigned)P->clut;
            tpx = (int)(tp & 0xFu) * 64;
            tpy = (int)((tp >> 4) & 1u) * 256;
            int mode = (int)((tp >> 7) & 3u);
            bpp = (mode == 0) ? 4 : (mode == 1) ? 8 : 16;
            clx = (int)(cl & 0x3Fu) * 16;
            cly = (int)((cl >> 6) & 0x1FFu);
            static int s_mat_logged = 0;
            if (!s_mat_logged) { s_mat_logged = 1;
                fprintf(stderr, "[moby-mat] first textured prim: tpage=%04X clut=%04X -> "
                        "tpage(%d,%d) clut(%d,%d) bpp=%d uv0=(%d,%d)\n",
                        (unsigned)P->tpage, (unsigned)P->clut, tpx, tpy, clx, cly, bpp,
                        P->u[0], P->v[0]); }
        }
        int blend = (P->code & 0x02) ? 1 : 0;   /* PSX ABE (semi-transparent) bit */
        if (s_ntri_n >= MAX_NTRI) break;
        ntri_t* t = &s_ntri[s_ntri_n++];
        t->v[0] = (ntri_vert){ fx[0], fy[0], fz[0], P->u[0], P->v[0], P->rgb[0] };
        t->v[1] = (ntri_vert){ fx[1], fy[1], fz[1], P->u[1], P->v[1], P->rgb[1] };
        t->v[2] = (ntri_vert){ fx[2], fy[2], fz[2], P->u[2], P->v[2], P->rgb[2] };
        t->textured=tex; t->tpage_x=tpx; t->tpage_y=tpy; t->clut_x=clx; t->clut_y=cly; t->bpp=bpp; t->blend=blend;
        if (P->nv == 4 && s_ntri_n < MAX_NTRI) {  /* PSX quad TL,TR,BL,BR -> tris (0,1,2)+(1,3,2) */
            ntri_t* q = &s_ntri[s_ntri_n++];
            q->v[0] = (ntri_vert){ fx[1], fy[1], fz[1], P->u[1], P->v[1], P->rgb[1] };
            q->v[1] = (ntri_vert){ fx[3], fy[3], fz[3], P->u[3], P->v[3], P->rgb[3] };
            q->v[2] = (ntri_vert){ fx[2], fy[2], fz[2], P->u[2], P->v[2], P->rgb[2] };
            q->textured=tex; q->tpage_x=tpx; q->tpage_y=tpy; q->clut_x=clx; q->clut_y=cly; q->bpp=bpp; q->blend=blend;
        }
    }
    return fallback;
}

/* Append the moby triangles to the native scene (panel 0). Frame-keyed begin
 * composes with the cyclorama (drawn first, owns the bg fill) — mobys append
 * on top. No depth buffer yet (3a.3), so submission order is the painter's
 * order: sky, then mobys. Determinism-safe: host-side display only. */
static void push_scene_moby(void) {
    static int s_logged = 0;
    if (!s_logged && s_ntri_n > 0) {
        s_logged = 1;
        fprintf(stderr, "[moby-native] feeding panel 0: %d moby tris from gamestate "
                "(native verts; prim topology scaffold) frame %u\n", s_ntri_n, openpete_frame);
    }
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_ntri_n; i++) {
        openpete_scene_tri t;
        for (int k = 0; k < 3; k++) {
            t.v[k].sx   = s_ntri[i].v[k].sx;
            t.v[k].sy   = s_ntri[i].v[k].sy;
            t.v[k].z    = openpete_native_depth_from_viewz(s_ntri[i].v[k].vz);
            t.v[k].u    = (float)s_ntri[i].v[k].u;
            t.v[k].v    = (float)s_ntri[i].v[k].v;
            t.v[k].rgba = s_ntri[i].v[k].rgb | 0xFF000000u;
        }
        t.textured = s_ntri[i].textured;
        t.tpage_x  = s_ntri[i].tpage_x; t.tpage_y = s_ntri[i].tpage_y;
        t.clut_x   = s_ntri[i].clut_x;  t.clut_y  = s_ntri[i].clut_y;
        t.bpp      = s_ntri[i].bpp;
        t.blend    = s_ntri[i].blend;
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

static int on_8001F798_moby_native(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = moby_verify_enabled();
    int feed   = openpete_native_render_enabled();
    if (!verify && !feed) return 0;
    static int s_in = 0;
    if (s_in) return 0;
    s_in = 1;

    uint32_t start = cpu->read_word(POLYBUF_CURSOR);
    func_8001F798(cpu);                       /* run the real REGULAR renderer */
    uint32_t end = cpu->read_word(POLYBUF_CURSOR);

    capture_gte(cpu, start, end);
    int nrec = project_all_records(cpu);      /* native float verts -> s_proj */

    if (feed) {
        build_native_tris();
        push_scene_moby();
    }

    if (verify) {
        static int s_dumped = 0, s_reports = 0;
        int target = (openpete_frame >= moby_verify_frame() && s_gte_n > 0 &&
                      (openpete_frame % 200u == 0) && s_reports < 40);  /* sample across levels */
        if (target && !s_dumped) { hex_dump(cpu, start, end); s_dumped = 1; }
        if (target) {
            compare_and_report(nrec);
            int fb = build_native_tris();   /* exercise the feed build headless */
            fprintf(stderr, "[moby-feed f=%u] tris=%d from prims=%d (nn-fallbacks=%d) "
                    "-- 0 fallbacks => every fed vertex is native\n",
                    openpete_frame, s_ntri_n, s_gte_n, fb);
            s_reports++;
        }
        /* Cumulative prim-code coverage, sampled across the whole demo (catches
         * enemy/fodder frames beyond the 16-report A/B window): which codes the
         * regular renderer emits and which we still drop. */
        static unsigned s_codes_at = 0;
        if (s_gte_n > 0 && openpete_frame >= s_codes_at + 2000) {
            s_codes_at = openpete_frame; report_code_hist();
        }
    }
    s_in = 0;
    return 1;
}

static const feature_dispatch_entry_t moby_dispatch[] = {
    { 0x8001F798u, on_8001F798_moby_native },
    { 0u, NULL },
};

const feature_module_t moby_native_module = {
    .name           = "moby_native",
    .apply_options  = NULL,
    .dispatch_table = moby_dispatch,
};
