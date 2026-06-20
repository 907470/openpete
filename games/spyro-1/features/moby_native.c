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
#include "render_paths.h"  /* --render-paths isolation */
#include "openpete_ui_moby.h"  /* HUD/UI moby classifier (overlay-on-top tagging) */

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
static long s_shmode[3]; /* shaded face-mode census: [0]=gouraud [1]=flat-basic [2]=flat-metal(bit1) */
static int  s_sh_nohl = -1; /* OPENPETE_SHADED_NOHL=1 disables the gouraud spec boost (A/B) */

#define D_SONY        0x8006FCF4u   /* g_SonyImage — reused as moby render scratch */
/* Cull-record buffer base (stride 0x38). The .s REGULAR cull computes it as
 * g_SonyImage + psx_runtime_moby_reg_queue_offset (offset_add channel at
 * 0x8001F19C/0x8001F7E4); the offset is 0x1600 normally but --no-lod RELOCATES +
 * GROWS the regular visible-queue to a separate host region (0x80600000), so a
 * hardcoded +0x1600 would read the now-empty canonical slot → ZERO records → all
 * regular mobys (enemies, fodder, Sparx) vanish in the native renderer under
 * --no-lod. REC_BASE mirrors the live REGULAR offset.
 *
 * The SPECULAR cull (func_800208FC/func_80020F34, 0x80020xxx) is NOT in the
 * channel substitution, so under --no-lod its queue stays at the CANONICAL
 * 0x1600 (the regular relocation does not apply to it). The native feed must use
 * a PER-PASS base (s_rec_base, set in moby_native_pass) — REGULAR=REC_BASE,
 * SPECULAR=D_SONY+0x1600 — else native SPEC reads the relocated slot (stale
 * regular records) and specular mobys (e.g. the Artisans rescue dragon, cls 0xFA)
 * vanish under --no-lod while regular mobys are fine. (Normal mode: both are
 * 0x1600.) g_sony_image_base is left canonical by --no-lod, so D_SONY is right. */
#define REC_BASE      (D_SONY + psx_runtime_moby_reg_queue_offset) /* REGULAR base */
static uint32_t s_rec_base = D_SONY + 0x1600u;   /* per-pass; set by moby_native_pass */
#define POLYBUF_CURSOR 0x800757B0u
#define G_MODELS      0x80076378u   /* Model* g_Models[512] */
#define G_CAMERA      0x80076DD0u
#define G_CYCLORAMA   0x80078A40u
#define G_LEVELMOBYS  0x80075828u   /* Moby* g_LevelMobys (head of the stride-0x58 array) */
#define G_SPYRO       0x80078A58u   /* g_Spyro; m_Position @ +0x00 (update-gate anchor) */

#define REC_STRIDE    0x38u
#define MAX_RECORDS   512
#define SHADED_BASE   (D_SONY + 0x2400u)  /* shaded renderer: flat Moby* array (stride 4) */
#define D_SPEC_LIGHT  0x800770C8u   /* SpecularAndMetal: +0/+4/+8 = directional light dir */
#define D_SHADE_COLTAB 0x8006E3D8u  /* shaded base-colour table, indexed by m_SpecularMetalColor>>22 */

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

/* Forward decl: the host-side moby record builder (widescreen reveal, below)
 * reuses the shaded path's euler matrix composer, defined later in this file. */
static void shaded_compose_euler(CPUState* cpu, const int start[3][3], uint32_t rot, int out[3][3]);

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
typedef struct { float sx, sy, vz; int sat; int rec;
                 float ex, ey, ez; } proj_xy;  /* vz=view-Z; rec=source record (diag);
                 ex/ey/ez = the model->view rotated vertex offset (M.v)/4096 = viewPos-TR,
                 the input to the specular env-reflection UV map (#1). */

/* Field-driven decode + float-project core. Both the .s-record path
 * (moby_project_record) and the host-built widescreen-reveal path
 * (append_revealed_regular) feed it the SAME six fields, so a revealed moby
 * decodes/projects bit-identically to a culled one:
 *  header  = AnimationHeader* (m_Scale@+5, ShortShift@+6, count@+8, faces@+0x14, colors@+0x18)
 *  frameA/frameB = AnimationFrame* keyframes (vertex stream packed in word0)
 *  w0      = record flags word (keyframe blend factor in bits 8-15)
 *  TR_raw  = stored translation (pre (<<2)>>m_Scale), the GTE view-space MAC
 *  M       = model->view rotation (int16, 1.0=4096) */
static int moby_decode_project(CPUState* cpu, uint32_t header, uint32_t frameA,
                               uint32_t frameB, uint32_t w0, const int TR_raw[3],
                               const int M[3][3], proj_xy* out, int cap) {
    if (header < 0x80000000u || frameA < 0x80000000u) return 0;
    /* C2_TR (projection translation) = (TR_stored << 2) >> m_Scale  [F798
     * @8001F858/8001F8C8]. m_Scale is the per-model shrink factor (AnimationHeader
     * +0x05): 2 = identity (the common case), 1 = ×2 size, 3 = /2, etc. Without
     * this, models whose m_Scale != 2 mis-project (the multi-moby A/B residual).
     * (This is the a3 = w0&0xFF == 0 path; the rare a3!=0 GPF-TR path is a TODO.) */
    int m_scale = cpu->read_byte(header + 5) & 0xFF;
    int TR[3];
    TR[0] = (TR_raw[0] << 2) >> m_scale;
    TR[1] = (TR_raw[1] << 2) >> m_scale;
    TR[2] = (TR_raw[2] << 2) >> m_scale;
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFFu);

    int s5    = (cpu->read_byte(header + 6) & 0xFF) + 1;
    int count = cpu->read_byte(header + 8) & 0xFF;   /* gte_ctrl[30] sign adj = 0 here */
    if (count > cap) count = cap;

    /* Keyframe blend: IR0 = (w0 & 0xFF00) >> 2 (1.0=4096); frame B at frameB.
     * 0 (or B absent) => the no-blend path (frame A only). */
    int blend = (int)((w0 & 0xFF00u) >> 2);
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
        /* The Z-buffer needs FULL-WORLD view-Z so mobys sort vs terrain / Spyro /
         * each other. project's mac3 is in the m_scale-adjusted world/4 scale
         * (C2_TR = (stored<<2)>>m_scale, stored is world/4), so multiply by
         * 2^m_scale to reach full-world (probe: m_scale=2 mac3 x4 == true cam-Z).
         * Sign-preserving, so emit_wtri's vz<=0 near cull still holds; sx/sy and
         * the saturation flag keep the unscaled GTE-faithful mac3 (vz is depth-only). */
        int ds = m_scale; if (ds < 0) ds = 0; else if (ds > 8) ds = 8;
        out[i].sx = (float)sx; out[i].sy = (float)sy;
        out[i].vz = (float)(vzz * (double)(1 << ds)); out[i].sat = sat;
        /* rotated vertex offset (M.v)/4096 = viewPos-TR -- the specular env-UV input. */
        out[i].ex = (float)((M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0);
        out[i].ey = (float)((M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0);
        out[i].ez = (float)((M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0);
    }
    return count;
}

/* Thin wrapper: read the six fields from a live .s cull record (+0x1600). */
static int moby_project_record(CPUState* cpu, uint32_t rec, proj_xy* out, int cap) {
    uint32_t header = cpu->read_word(rec + 0x04);
    uint32_t frameA = cpu->read_word(rec + 0x08);
    uint32_t frameB = cpu->read_word(rec + 0x0C);
    uint32_t w0     = cpu->read_word(rec);
    int TR_raw[3] = { (int32_t)cpu->read_word(rec + 0x10),
                      (int32_t)cpu->read_word(rec + 0x14),
                      (int32_t)cpu->read_word(rec + 0x18) };
    int M[3][3]; read_matrix(cpu, rec + 0x1C, M);
    return moby_decode_project(cpu, header, frameA, frameB, w0, TR_raw, M, out, cap);
}

#define MAX_PROJ 8192
static proj_xy s_proj[MAX_PROJ]; static int s_proj_n;

/* Per-record metadata for the independent face-stream walk (3a.2b): where this
 * record's vertices live in s_proj[] (filled in stream order, so a face's stream
 * vertex index k maps to s_proj[proj_off + k]) + the face-stream base. */
typedef struct { uint32_t addr; int proj_off, proj_n; uint32_t header, faceblock, w0;
                 int metal; uint32_t mscol; uint32_t moby; } rec_meta;  /* metal = m_SpecularMetalColor
                 (rec+0x30) high byte (env flag); mscol = full word (highlight BK + spec byte);
                 moby = Moby* (rec+0x34 for .s records; set directly for host-built reveal records,
                 whose addr is then 0 — there is no live +0x1600 record behind them) */
static rec_meta s_recs[MAX_RECORDS]; static int s_nrecs;

/* Moby pointers fed by the REGULAR pass this frame — the SPECULAR pass dedups
 * against this so a moby drawn by BOTH passes (the common case; .s overlaps them
 * at one pose) is fed ONCE (by REGULAR, at its pre-specular-cull pose), while a
 * moby UNIQUE to the specular queue (e.g. the Artisans rescue dragon cls 0xFA,
 * which the specular cull writes into a slot a regular moby held at REGULAR-read
 * time) is still fed by SPECULAR. Snapshotted in the REGULAR pass (runs first). */
static uint32_t s_reg_moby[MAX_RECORDS]; static int s_reg_moby_n;

/* Project EVERY live record's vertices into s_proj[] (native float decode +
 * RTPS+TR). Shared by the verify A/B and the scene feed. Returns record count. */
static int s_max_blend;   /* max keyframe-blend factor (rec.w0 IR0) across this frame's records */
static int project_all_records(CPUState* cpu) {
    s_proj_n = 0; s_max_blend = 0; s_nrecs = 0;
    int nrec = 0;
    for (int r = 0; r < MAX_RECORDS; r++) {
        uint32_t rec = s_rec_base + (uint32_t)r * REC_STRIDE;
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
                (header >= 0x80000000u) ? cpu->read_word(header + 0x14) : 0u, w0,
                /* env metal flag = high byte of m_SpecularMetalColor @rec+0x30 (specular
                 * cull pass copies it; @80020CA0 / read @8002176C srl 24, bnez .L800226F8) */
                (int)((cpu->read_word(rec + 0x30) >> 24) & 0xFFu),
                /* full m_SpecularMetalColor: the highlight back-colour (.L80021DB4) +
                 * byte2 spec-intensity. Carries the per-moby tint (flamed chest -> red). */
                cpu->read_word(rec + 0x30),
                cpu->read_word(rec + 0x34) };
        }
        s_proj_n += got; nrec++;
        if (s_proj_n >= MAX_PROJ) break;
    }
    s_nrecs = nrec;
    return nrec;
}

/* ---- widescreen live-moby reveal (host-built records) ---------------------
 * In widescreen the .s cull (run headless at canonical 4:3) only writes +0x1600
 * records for 4:3-visible mobys, so enemies/fodder/Sparx/gems pop in at the
 * screen edges. We must NOT re-run the cull (it sets m_WasDrawn — a render->
 * gameplay leak). Instead rebuild the per-moby record FIELDS host-side from
 * gamestate, exactly as func_8001F158 does, for the mobys the 4:3 cull skipped,
 * and decode/project them through moby_decode_project (the same core).
 *
 * Validated against the .s records for in-frustum mobys (OPENPETE_MOBY_BUILDDBG):
 * a host-built record for a moby the .s already culled in must match the .s record
 * field-for-field (M/TR within GTE truncation, header/frameA/frameB/blend exact).
 * Only after that A/B passes is the builder trusted on the revealed (off-4:3) set. */
typedef struct { uint32_t header, frameA, frameB, w0, mscol; int metal; int TR_raw[3]; int M[3][3]; } moby_fields;

/* Build the REGULAR (animated) render-record fields for one moby from gamestate.
 * Returns 1 on success, 0 if the moby has no animated model (SimpleModel =>
 * shaded material, handled by the shaded path) or an invalid model/header. */
static int build_moby_record_fields(CPUState* cpu, uint32_t moby, moby_fields* f) {
    uint32_t cls   = cpu->read_half(moby + 0x36);            /* m_Class */
    uint32_t model = cpu->read_word(G_MODELS + cls * 4u);    /* g_Models[cls] */
    if (model < 0x80000000u) return 0;
    if ((int32_t)cpu->read_word(model + 0x00) < 0) return 0; /* m_NumAnimations<0 => SimpleModel (shaded) */
    uint32_t anim   = cpu->read_byte(moby + 0x3C) & 0xFFu;   /* m_Animation */
    uint32_t header = cpu->read_word(model + 0x38u + anim * 4u);  /* AnimationHeader* (Model.m_Animations[]) */
    if (header < 0x80000000u) return 0;
    uint32_t frm  = cpu->read_byte(moby + 0x3E) & 0xFFu;     /* m_Frame */
    uint32_t nfrm = cpu->read_byte(moby + 0x3F) & 0xFFu;     /* m_NextFrame */
    f->header = header;
    f->frameA = header + 0x24u + frm  * 8u;                  /* &m_Frames[m_Frame]     (AnimationFrame, stride 8) */
    f->frameB = header + 0x24u + nfrm * 8u;                  /* &m_Frames[m_NextFrame] */
    uint32_t prog = cpu->read_byte(moby + 0x40) & 0xFFu;     /* m_FrameProgress -> blend bits 8-15 */
    f->w0    = (prog << 8) & 0xFF00u;
    f->mscol = cpu->read_word(moby + 0x4Cu);                 /* m_SpecularMetalColor */
    f->metal = (int)((f->mscol >> 24) & 0xFFu);
    /* M = ProjMatrix . Ry . Rx . Rz(m_Rotation) — the SAME euler compose the shaded
     * path uses (verified bit-exact there); m_Rotation @ moby+0x44. */
    int PM[3][3]; read_matrix(cpu, G_CAMERA + 0x00, PM);
    shaded_compose_euler(cpu, PM, cpu->read_word(moby + 0x44u), f->M);
    /* TR = ProjMatrix . swizzled (cam-moby)>>2  (the cull's centre MVMVA: IR1=(cy-my)>>2,
     * IR2=(cz-mz)>>2, IR3=(mx-cx)>>2; MAC=(PM·IR)>>12). NOT scaled by m_Scale or <<1
     * here — moby_decode_project applies (<<2)>>m_Scale, matching the .s record store. */
    int32_t cx=(int32_t)cpu->read_word(G_CAMERA+0x28), cy=(int32_t)cpu->read_word(G_CAMERA+0x2C), cz=(int32_t)cpu->read_word(G_CAMERA+0x30);
    int32_t mx=(int32_t)cpu->read_word(moby+0x0C), my=(int32_t)cpu->read_word(moby+0x10), mz=(int32_t)cpu->read_word(moby+0x14);
    int32_t Vy=(cy-my)>>2, Vz=(cz-mz)>>2, Vx=(mx-cx)>>2;
    for (int i=0;i<3;i++)
        f->TR_raw[i] = (PM[i][0]*Vy + PM[i][1]*Vz + PM[i][2]*Vx) >> 12;
    return 1;
}

/* Builder A/B (OPENPETE_MOBY_BUILDDBG): for each .s record, rebuild its fields
 * host-side from the moby and compare. A clean match => the builder reproduces the
 * cull, so it is trusted for the off-4:3 reveal set. Prints once at the target. */
static void moby_build_validate(CPUState* cpu) {
    static int s_on = -1;
    if (s_on < 0) { const char* e=getenv("OPENPETE_MOBY_BUILDDBG"); s_on = (e&&*e&&*e!='0'); }
    if (!s_on) return;
    /* Sample across the demo's level loads (varied class / m_scale / blend) rather
     * than once: print on each crossing of a ~1500-frame grid, up to 12 reports. */
    static unsigned s_next = 1850u; static int s_reports = 0;
    if (openpete_frame < s_next || s_reports >= 12) return;
    s_next = openpete_frame + 1500u; s_reports++;
    int n=0, okHdr=0, okA=0, okB=0, okBl=0, okM=0, okTR=0, noModel=0, worstM=0, worstTR=0;
    for (int r=0; r<s_nrecs; r++) {
        uint32_t rec = s_recs[r].addr; if (rec < 0x80000000u) continue;
        uint32_t moby = cpu->read_word(rec + 0x34);
        moby_fields f;
        if (!build_moby_record_fields(cpu, moby, &f)) { noModel++; continue; }
        n++;
        if (f.header == cpu->read_word(rec+0x04)) okHdr++;
        if (f.frameA == cpu->read_word(rec+0x08)) okA++;
        if (f.frameB == cpu->read_word(rec+0x0C)) okB++;
        if ((f.w0 & 0xFF00u) == (cpu->read_word(rec) & 0xFF00u)) okBl++;
        int sM[3][3]; read_matrix(cpu, rec+0x1C, sM);
        int dM=0; for(int i=0;i<3;i++)for(int j=0;j<3;j++){int d=f.M[i][j]-sM[i][j]; if(d<0)d=-d; if(d>dM)dM=d;}
        if (dM<=2) okM++; if (dM>worstM) worstM=dM;
        int sTR[3]={(int32_t)cpu->read_word(rec+0x10),(int32_t)cpu->read_word(rec+0x14),(int32_t)cpu->read_word(rec+0x18)};
        int dT=0; for(int i=0;i<3;i++){int d=f.TR_raw[i]-sTR[i]; if(d<0)d=-d; if(d>dT)dT=d;}
        if (dT<=2) okTR++; if (dT>worstTR) worstTR=dT;
        if (r < 6) fprintf(stderr,
            "  [build r%d moby=%08X] hdr %08X/%08X A %08X/%08X B %08X/%08X bl %04X/%04X dM=%d dTR=%d\n",
            r, moby, f.header, cpu->read_word(rec+0x04), f.frameA, cpu->read_word(rec+0x08),
            f.frameB, cpu->read_word(rec+0x0C), (f.w0&0xFF00u), (cpu->read_word(rec)&0xFF00u), dM, dT);
    }
    fprintf(stderr,
        "[moby-build f=%u] recs=%d built=%d noModel(shaded)=%d | hdr=%d frameA=%d frameB=%d blend=%d "
        "M(<=2)=%d TR(<=2)=%d worst{M=%d TR=%d}  (== built means builder reproduces the .s cull)\n",
        openpete_frame, s_nrecs, n, noModel, okHdr, okA, okB, okBl, okM, okTR, worstM, worstTR);
}

/* Append host-built records for mobys the 4:3 cull skipped but that fall in the
 * widescreen reveal margins (projected centroid X outside the 4:3 [0,512] inset,
 * inside the widened bounds). Dedups against the .s 4:3 set and only draws LIVE
 * (animating) mobys — frozen mobys hold a stale pose (display-anim is deferred).
 * Extends s_recs[]/s_proj[]; build_walk_tris then walks the union. Inert at 4:3. */
static void append_revealed_regular(CPUState* cpu) {
    float F = openpete_native_widescreen_factor();
    if (F <= 1.0f) return;                              /* 4:3 — nothing to reveal */
    float xlo, xhi; openpete_native_widescreen_x_bounds(&xlo, &xhi);
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFFu);
    int32_t spx=(int32_t)cpu->read_word(G_SPYRO+0x00), spy=(int32_t)cpu->read_word(G_SPYRO+0x04);
    int nbase = s_nrecs;                               /* the .s 4:3 records (dedup set) */
    int revealed = 0;
    uint32_t m = cpu->read_word(G_LEVELMOBYS);
    int guard = 0;
    while (m >= 0x80000000u && guard++ < 4096) {
        uint8_t stt = cpu->read_byte(m + 0x48);
        if (stt == 0xFF) break;                        /* array sentinel */
        if (stt >= 0x80) { m += 0x58; continue; }      /* dead/inactive */
        /* dedup vs the .s 4:3 set (exact: same Moby* the cull recorded) */
        int dup = 0;
        for (int k = 0; k < nbase; k++) if (s_recs[k].moby == m) { dup = 1; break; }
        if (dup) { m += 0x58; continue; }
        /* live gate (Spyro-anchored, camera-angle-independent): m_WasDrawn grace,
         * always-update sentinel, or within the m_UpdateDistance xy-disk of Spyro */
        uint8_t wasDrawn = cpu->read_byte(m + 0x51), updDist = cpu->read_byte(m + 0x52);
        int live = wasDrawn != 0;
        if (!live && updDist != 0) {
            if (updDist >= 0x80) live = 1;
            else { double sdx=(double)((int32_t)cpu->read_word(m+0x0C)-spx),
                          sdy=(double)((int32_t)cpu->read_word(m+0x10)-spy), thr=(double)updDist*1024.0;
                   if (sdx*sdx + sdy*sdy <= thr*thr) live = 1; }
        }
        if (!live) { m += 0x58; continue; }
        moby_fields f;
        if (!build_moby_record_fields(cpu, m, &f)) { m += 0x58; continue; }  /* shaded/invalid */
        double rx = (double)f.TR_raw[0], rz = (double)f.TR_raw[2];           /* origin in view space (world/4) */
        if (rz + 128.0 <= 0.0) { m += 0x58; continue; }                     /* behind camera */
        double rRad = (double)(cpu->read_byte(m + 0x50) & 0xFF);
        if (rz - rRad * 256.0 >= 0.0) { m += 0x58; continue; }              /* beyond render distance */
        double sx = ofx + rx * H / rz;                                       /* projected centroid X (m_Scale cancels) */
        if (sx >= 0.0 && sx <= 512.0) { m += 0x58; continue; }              /* central 4:3 — leave untouched */
        if (sx < (double)xlo || sx > (double)xhi) { m += 0x58; continue; }  /* outside widened view */
        if (s_nrecs >= MAX_RECORDS || s_proj_n >= MAX_PROJ) break;
        int off = s_proj_n;
        int got = moby_decode_project(cpu, f.header, f.frameA, f.frameB, f.w0, f.TR_raw, f.M,
                                      s_proj + s_proj_n, MAX_PROJ - s_proj_n);
        if (got <= 0) { m += 0x58; continue; }
        for (int j = 0; j < got; j++) s_proj[off + j].rec = s_nrecs;
        s_recs[s_nrecs] = (rec_meta){ 0u, off, got, f.header,
            (f.header >= 0x80000000u) ? cpu->read_word(f.header + 0x14) : 0u, f.w0,
            f.metal, f.mscol, m };
        s_proj_n += got; s_nrecs++; revealed++;
        m += 0x58;
    }
    static int s_logged = 0;
    if (!s_logged && revealed > 0) {
        s_logged = 1;
        fprintf(stderr, "[moby-reveal-feed] frame=%u F=%.3f host-built %d revealed regular mobys "
                "(off-4:3, live) appended to the native scene\n", openpete_frame, F, revealed);
    }
}

/* ---- native scene triangle (fed to panel 0) ------------------------------
 * Filled by the 3a.2b independent face-stream walk (below). The 3a.2a
 * nearest-neighbour prim scaffold it replaced was removed when the walk was
 * promoted (2026-06-15); the walk is jitter-free (exact word0 indices) where
 * the scaffold's screen-proximity matching jittered. */
typedef struct { float sx, sy, vz; int u, v; uint32_t rgb;
                 float ex, ey, ez; } ntri_vert;  /* vz=depth; u,v=texel; ex/ey/ez=env-UV input (#1) */
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
static uint8_t s_wtri_mode[MAX_NTRI]; /* shaded only: 0=gouraud 1=flat-basic 2=flat-metal */
static uint8_t s_wtri_ui[MAX_NTRI];   /* 1 = this tri's moby is a HUD/UI overlay (draw on top) */
static int     s_cur_ui;              /* set per-record by the walks before emitting its tris */

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
    /* NEAR-PLANE CULL: drop any triangle with a vertex at/behind the camera
     * (view-Z <= 0). The .s relies on the GTE perspective divide + NCLIP to
     * reject behind-camera prims; our float project() does a raw H/Z, so a
     * negative Z flips the vertex (sign inverts both screen axes) and hurls it
     * to the top of the view. Without this, a moby that passes behind the camera
     * — e.g. the gem value-popup the player charges THROUGH — renders inverted
     * ("upside down") at screen top. Matches the .s (it draws nothing for a
     * fully-behind moby). Cheap whole-triangle cull (no near-plane clipping):
     * mobys rarely straddle the near plane, and a 1-frame early disappear beats
     * a flipped explosion. */
    if (a->vz <= 0.0f || b->vz <= 0.0f || c->vz <= 0.0f) return;
    s_wtri_ui[s_wtri_n] = (uint8_t)s_cur_ui;   /* HUD/UI tag for this tri (current record) */
    ntri_t* t = &s_wtri[s_wtri_n++];
    t->v[0]=(ntri_vert){a->sx,a->sy,a->vz,ua,va,ca, a->ex,a->ey,a->ez};
    t->v[1]=(ntri_vert){b->sx,b->sy,b->vz,ub,vb,cb, b->ex,b->ey,b->ez};
    t->v[2]=(ntri_vert){c->sx,c->sy,c->vz,uc,vc,cc, c->ex,c->ey,c->ez};
    t->textured=tex;t->tpage_x=tpx;t->tpage_y=tpy;t->clut_x=clx;t->clut_y=cly;t->bpp=bpp;t->blend=blend;
    s_walkdiag.emitted++;
}

/* specular env-reflection UV (#1) -- definitions below (near recolor); forward-
 * declared so the walk can derive metal-face UVs natively. s_env_* = the static
 * metal Tiledef (D_800756F0, level-load asset), decoded to VRAM px by _load. */
static void specular_env_load(CPUState* cpu);
static void specular_env_uv(float ex, float ey, float ez, int* pu, int* pv);
static int  s_env_tpx, s_env_tpy, s_env_clx, s_env_cly, s_env_bpp;
static float s_specL[3], s_specLlen;   /* D_800770C8+0xC specular light dir (highlight) */

/* Build s_wtri[] from the independent face-stream walk. Returns triangle count.
 * No backface cull yet (the winding sign is determined from the A/B's nclip
 * split first); quads emit both sub-tris when quad-enable is set. */
static int build_walk_tris(CPUState* cpu) {
    specular_env_load(cpu);   /* refresh metalMatrix + metal Tiledef (cheap; SPEC env-UV) */
    s_wtri_n = 0;
    memset(&s_walkdiag, 0, sizeof(s_walkdiag));
    int is_spec = (s_cur_mat && s_cur_mat[0]=='S' && s_cur_mat[1]=='P');
    for (int r = 0; r < s_nrecs; r++) {
        rec_meta* R = &s_recs[r];
        uint32_t mob = cpu->read_word(R->addr + 0x34);
        /* SPECULAR dedup: skip a moby the REGULAR pass already fed (overlap → would
         * double at a different pose). Specular-only mobys (not in s_reg_moby) feed. */
        if (is_spec) {
            int dup = 0;
            for (int k = 0; k < s_reg_moby_n; k++) if (s_reg_moby[k] == mob) { dup = 1; break; }
            if (dup) continue;
        }
        { /* HUD tag: 2=HUD3D icon, 1=HUD flat, 0=actor */
          s_cur_ui = openpete_ui_moby_is_addr_static(mob) ? 2
                   : openpete_ui_moby_is_addr_ui(mob) ? 1 : 0; }
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
             * sign test (the `neg` dance). The emit TARGET (read from the .s by its GPU
             * command byte -- 0x30=tri, 0x38=quad) determines tri-vs-quad, and it is the
             * OPPOSITE of what the t6a>=0 "s1=0x10" path suggests:
             *   t6a>=0 && t6b>0  -> .L800203F4 (+8=v3): single 0x30 TRI (v3,v1,v2)
             *   t6a>=0 && t6b<=0 -> skip the whole face
             *   t6a<0  && t6b>0  -> .L80020158:        0x38 QUAD (v0,v1,v2)+(v1,v3,v2)
             *   t6a<0  && t6b<=0 -> .L800203F4 (+8=v0): single 0x30 TRI (v0,v1,v2)
             * (Measured: walkQ matches the .s's primQ=49/frame on the Insomniac sign,
             * recall 64%->99%. The earlier inverted map drew one half of every quad.)
             * STRIPS (.L8002031C) are single triangles, keep if NCLIP>0. */
            proj_xy* p3 = (i3 < vcount) ? &s_proj[base+i3] : NULL;
            double ncl1 = tri_nclip_i(p0,p1,p2);
            double ncl2 = p3 ? tri_nclip_i(p3,p1,p2) : -1.0;
            int emit_a = 0, emit_b = 0, b_is_v3tri = 0;
            if (fresh) {
                int bit0 = (int)(w0 & 1u);
                double t6a = (bit0 && ncl1 > 0.0) ? -ncl1 : ncl1;   /* asm .L800200D0 blez: negate NCLIP1 when >0 */
                double t6b = (bit0 && ncl2 < 0.0) ? -ncl2 : ncl2;   /* asm .L80020108/.L80020138 bgez: negate NCLIP2 when <0 */
                /* Emit-target codes read from the .s:
                 *   t6a>=0 && t6b>0  -> .L800203F4 (+8=v3): single 0x30 TRI (v3,v1,v2)
                 *   t6a>=0 && t6b<=0 -> SKIP
                 *   t6a<0  && t6b>0  -> .L80020158:        0x38 QUAD (v0,v1,v2)+(v1,v3,v2)
                 *   t6a<0  && t6b<=0 -> .L800203F4 (+8=v0): single 0x30 TRI (v0,v1,v2) */
                if (t6a >= 0.0) { if (t6b > 0.0 && p3) { emit_b = b_is_v3tri = 1; } }   /* tri (v3,v1,v2) */
                else if (t6b > 0.0 && p3) { emit_a = emit_b = 1; s_walkdiag.quad++; }   /* QUAD */
                else emit_a = 1;                                                        /* tri (v0,v1,v2) */
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
            /* v3 (4th-vertex) gouraud colour: its index is the HIGH field of the
             * fp+8 word ((v4w>>9)&0x7FC), the sibling of the SXY index in the low
             * field. Previously approximated as c2, which forced every quad's 2nd
             * tri (v1,v3,v2) and every (v3,v1,v2) tri to colours (c1,c2,c2)/(c2,..)
             * — a broken gouraud gradient that rendered as hard facets on ~all
             * quad faces (51/face-stream here). Reading the real colour restores
             * the smooth per-vertex shading. */
            uint32_t c3 = cpu->read_word(cbase + ((v4w >> 9) & 0x7FCu)) & 0xFFFFFFu;

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
            /* #1 SPECULAR per-face shading. The .s gives ONLY strip faces (!fresh)
             * with word0&0x4 set a special per-face treatment (the bit is the
             * highlight-enable @80021B64; on FRESH faces the same bit is the quad
             * flag, so they're excluded). METAL record -> env reflection
             * (.L800226F8/.L800222C0); non-metal -> diffuse highlight (.L80021DB4).
             * Every OTHER face keeps its baked Tiledef texture + per-vertex colour.
             * This per-face gate is load-bearing: a metal moby's sombrero/eyes are
             * regular textured faces and must NOT be env-mapped -- only its at&4
             * strip faces reflect. (The earlier gate env-mapped EVERY textured face
             * of a metal moby -> sombrero went shiny-green with the wrong texture.) */
            if (s_cur_mat[0]=='S' && !fresh && (w0 & 0x4u)) {
              if (R->metal) {
                /* ENV REFLECTION -- view-dependent spherical UV (byte-exact <=1
                 * texel vs prims); tpage/clut from the static metal Tiledef. */
                specular_env_uv(p0->ex,p0->ey,p0->ez, &u[0],&v[0]);
                specular_env_uv(p1->ex,p1->ey,p1->ez, &u[1],&v[1]);
                specular_env_uv(p2->ex,p2->ey,p2->ez, &u[2],&v[2]);
                if (p3) specular_env_uv(p3->ex,p3->ey,p3->ez, &u[3],&v[3]);
                /* SEAM UNWRAP: u is azimuth mod 128; pull u[1..3] within +-64 of
                 * u[0] (then keep >=0) so a seam face isn't interpolated backwards
                 * (mirrors the .s shifting a whole face by +-128). */
                int nvtx = p3 ? 4 : 3, umin = u[0];
                for (int q = 1; q < nvtx; q++) {
                    while (u[q] - u[0] >  64) u[q] -= 128;
                    while (u[q] - u[0] < -64) u[q] += 128;
                    if (u[q] < umin) umin = u[q];
                }
                if (umin < 0) for (int q = 0; q < nvtx; q++) u[q] += 128;
                textured = 1;   /* env faces sample the reflection texture */
                tpx=s_env_tpx; tpy=s_env_tpy; clx=s_env_clx; cly=s_env_cly; bpp=s_env_bpp;
                /* FLAMED-METAL heat red-shift (.L80021FE0, gated heat!=0): the .s
                 * red-shifts the env face's modulating colour by a2=(ms>>16)&0xFF
                 * (R+=a2, G-=a2, B-=a2, clamp) before the env UV -- so a heated
                 * METAL object reddens, the same as the non-metal chest. cbase (the
                 * base) is already correct; only this shift was missing. No-op for
                 * cold metal (heat==0). [audit 2026-06-16] */
                int heat=(int)((R->mscol>>16)&0xFFu);
                if (heat) {
                    uint32_t* cc[4]={&c0,&c1,&c2,&c3};
                    for (int q=0;q<4;q++){ uint32_t c=*cc[q];
                        int r=(int)(c&0xFF)+heat, g=(int)((c>>8)&0xFF)-heat, b=(int)((c>>16)&0xFF)-heat;
                        if(r>255)r=255; if(g<0)g=0; if(b<0)b=0;
                        *cc[q]=(uint32_t)(r|(g<<8)|(b<<16)); }
                }
              } else {
                /* DIFFUSE HIGHLIGHT (.L80021DB4) -- per-face flat colour that
                 * REPLACES the base. The .s builds it from the moby's
                 * m_SpecularMetalColor as the GTE back-colour (BK) plus a gray
                 * light contribution scaled by the spec coefficient (byte2) and the
                 * facing. BK carries the per-moby TINT, so a flamed metal chest
                 * (mscol -> red) reddens ("gets hot but doesn't break"); the cold
                 * chest's BK is gray -> silver. Using the gray *base* colour missed
                 * this. BK channels (the .s packing @80021E38): RBK=(ms>>10)&0xFF,
                 * GBK=(ms>>4)&0xFF, BBK=(ms<<2)&0xFF; spec = byte2 = (ms>>16)&0xFF.
                 * facing add fitted to the prims: ~ -1.19 * spec * NL (gray), NL =
                 * view face-normal . spec-light (D_800770C8+0xC). */
                uint32_t ms=R->mscol;
                int RBK=(int)((ms>>10)&0xFFu), GBK=(int)((ms>>4)&0xFFu), BBK=(int)((ms<<2)&0xFFu);
                int spec=(int)((ms>>16)&0xFFu);
                float e1x=p1->ex-p0->ex, e1y=p1->ey-p0->ey, e1z=p1->ez-p0->ez;
                float e2x=p2->ex-p0->ex, e2y=p2->ey-p0->ey, e2z=p2->ez-p0->ez;
                float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
                float nl=sqrtf(nx*nx+ny*ny+nz*nz);
                if (nl > 0.001f) {
                    float NL=(nx*s_specL[0]+ny*s_specL[1]+nz*s_specL[2])/(nl*s_specLlen);
                    int add=(int)(-1.19f * (float)spec * NL);
                    int rr=RBK+add, gg=GBK+add, bb2=BBK+add;
                    if(rr<0)rr=0; if(rr>255)rr=255; if(gg<0)gg=0; if(gg>255)gg=255;
                    if(bb2<0)bb2=0; if(bb2>255)bb2=255;
                    c0=c1=c2=c3=(uint32_t)(rr|(gg<<8)|(bb2<<16));   /* flat per face */
                }
              }
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
typedef struct { float x[3], y[3]; uint32_t rgb[3]; int u[3], v[3]; int tex, clut, tpage; } ptri;
#define MAX_PTRI (MAX_PRIMS*2)
static ptri s_ptri[MAX_PTRI]; static int s_ptri_n;
static void build_prim_tris(void) {
    s_ptri_n = 0;
    for (int g = 0; g < s_gte_n; g++) {
        prim_t* P = &s_gte[g];
        if (P->nv < 3 || s_ptri_n >= MAX_PTRI) continue;
        ptri* a = &s_ptri[s_ptri_n++];
        for (int k=0;k<3;k++){a->x[k]=P->x[k];a->y[k]=P->y[k];a->rgb[k]=P->rgb[k];a->u[k]=P->u[k];a->v[k]=P->v[k];}
        a->tex=P->textured; a->clut=P->clut; a->tpage=P->tpage;
        if (P->nv==4 && s_ptri_n<MAX_PTRI) {            /* quad TL,TR,BL,BR -> (1,3,2) */
            ptri* b=&s_ptri[s_ptri_n++]; int idx[3]={1,3,2};
            for (int k=0;k<3;k++){int j=idx[k];b->x[k]=P->x[j];b->y[k]=P->y[j];b->rgb[k]=P->rgb[j];b->u[k]=P->u[j];b->v[k]=P->v[j];}
            b->tex=P->textured; b->clut=P->clut; b->tpage=P->tpage;
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
    int prim_quad=0, prim_tri=0;   /* how many GPU quads vs tris the .s actually emitted */
    for(int g=0;g<s_gte_n;g++){ if(s_gte[g].nv==4)prim_quad++; else if(s_gte[g].nv==3)prim_tri++; }
    int wmatch=0, neg_m=0, pos_m=0, neg_u=0, pos_u=0;
    int mt_p=0,mt_n=0,mf_p=0,mf_n=0;  /* matched nclip split: textured/flat (winding rule) */
    int colmatch=0, coln=0, uvmatch=0, uvn=0; double wluma_sum=0, pluma_sum=0;
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
            wluma_sum += (wc&0xFF)+((wc>>8)&0xFF)+((wc>>16)&0xFF);   /* brightness: is my colour darker than the .s's? */
            pluma_sum += (pc&0xFF)+((pc>>8)&0xFF)+((pc>>16)&0xFF);
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
    int pcov=0, pon=0, pcov_on=0, puncov_dump=0;
    for(int pi=0;pi<pn;pi++){
        pcov+=pmatched[pi];
        double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
        double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
        if(ONSCR(pcx,pcy)){
            pon++;
            if(pmatched[pi]) pcov_on++;
            else if(s_cmp_dump && puncov_dump++<12)   /* prims the .s drew that the walk MISSED */
                fprintf(stderr,"  [uncovered-prim] (%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) tex=%d rgb=%06X\n",
                    s_ptri[pi].x[0],s_ptri[pi].y[0],s_ptri[pi].x[1],s_ptri[pi].y[1],
                    s_ptri[pi].x[2],s_ptri[pi].y[2], s_ptri[pi].tex, s_ptri[pi].rgb[0]&0xFFFFFFu);
        }
    }
    /* self-duplicate walk tris (order-independent vertex coincidence <=1px): the
     * "repeating tri" detector. A count-preserving topology substitution (emit a
     * wrong tri that still matches some prim's POINTS, miss the real one) shows up
     * here even while precision stays 100% -- the matcher is winding/dup-blind. */
    int wdup=0;
    for(int i=0;i<wn;i++) for(int j=0;j<i;j++){
        static const int pm[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
        double cost=1e18;
        for(int q=0;q<6;q++){ double mx=0; for(int k=0;k<3;k++){
            double dx=s_wtri[i].v[k].sx-s_wtri[j].v[pm[q][k]].sx;
            double dy=s_wtri[i].v[k].sy-s_wtri[j].v[pm[q][k]].sy;
            double d=sqrt(dx*dx+dy*dy); if(d>mx)mx=d; } if(mx<cost)cost=mx; }
        if(cost<=1.0){ wdup++; break; }
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
        "matchedTex{-=%d +=%d} matchedFlat{-=%d +=%d} | col=%d/%d uv=%d/%d wdup=%d/%d primQ=%d primT=%d walkQ=%d luma=w%.0f/p%.0f\n",
        openpete_frame,
        s_walkdiag.fresh_g, s_walkdiag.fresh_t, s_walkdiag.strip, s_walkdiag.sprite,
        s_walkdiag.skip, s_walkdiag.quad, s_walkdiag.oor,
        s_walkdiag.align_lo, s_walkdiag.align_hi, s_walkdiag.desync,
        neg_m,pos_m,neg_u,pos_u, mt_n,mt_p,mf_n,mf_p, colmatch,coln, uvmatch,uvn, wdup,wn,
        prim_quad, prim_tri, s_walkdiag.quad, coln?wluma_sum/coln:0.0, coln?pluma_sum/coln:0.0);
    #undef ONSCR
}

/* Within-matched-tri colour A/B for the native shaded lighting (#2b). Global
 * position-NN is unreliable here — cliff_town packs ~58 shaded mobys into a
 * small region, so a vertex's nearest prim point often belongs to a DIFFERENT
 * moby. So: match each native walk tri to a prim tri by geometry (centroid
 * prune + tri_match_cost, the same as compare_walk_vs_prims), then min-assign
 * the 3 colour pairs (best of 6 permutations) and tally per-vertex exact match
 * + mean per-channel error. Staying inside a geometry-matched face pair keeps
 * the comparison within the same moby. On-screen tris only. */
static void shaded_color_ab(void) {
    build_prim_tris();
    if (s_ptri_n <= 0 || s_wtri_n <= 0) return;
    static const int pm[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    int n[3]={0,0,0}, exact[3]={0,0,0}; double cherr[3]={0,0,0};
    for (int wi=0; wi<s_wtri_n; wi++) {
        ntri_t* w=&s_wtri[wi]; int md=s_wtri_mode[wi]; if(md>2)md=0;
        double wcx=(w->v[0].sx+w->v[1].sx+w->v[2].sx)/3.0, wcy=(w->v[0].sy+w->v[1].sy+w->v[2].sy)/3.0;
        if (!(wcx>=0&&wcx<=512&&wcy>=0&&wcy<=240)) continue;
        double best=1e18; int bp=-1;
        for (int pi=0; pi<s_ptri_n; pi++) {
            double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
            double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
            if (fabs(pcx-wcx)>16.0||fabs(pcy-wcy)>16.0) continue;
            double c=tri_match_cost(w,&s_ptri[pi]); if(c<best){best=c;bp=pi;}
        }
        if (best>1.0||bp<0) continue;   /* tight geom gate: kill cross-moby matches */
        /* best assignment of my 3 colours to the prim's 3 (min total channel err) */
        double bce=1e18; int bq=0;
        for (int q=0;q<6;q++){ double e=0; for(int k=0;k<3;k++){
            uint32_t wc=w->v[k].rgb&0xFFFFFFu, pc=s_ptri[bp].rgb[pm[q][k]]&0xFFFFFFu;
            e += abs((int)(wc&0xFF)-(int)(pc&0xFF))+abs((int)((wc>>8)&0xFF)-(int)((pc>>8)&0xFF))
               + abs((int)((wc>>16)&0xFF)-(int)((pc>>16)&0xFF)); }
            if(e<bce){bce=e;bq=q;} }
        for(int k=0;k<3;k++){
            uint32_t wc=w->v[k].rgb&0xFFFFFFu, pc=s_ptri[bp].rgb[pm[bq][k]]&0xFFFFFFu;
            n[md]++; if(wc==pc) exact[md]++;
            cherr[md] += abs((int)(wc&0xFF)-(int)(pc&0xFF))+abs((int)((wc>>8)&0xFF)-(int)((pc>>8)&0xFF))
                   + abs((int)((wc>>16)&0xFF)-(int)((pc>>16)&0xFF));
        }
    }
    int N=n[0]+n[1]+n[2], E=exact[0]+exact[1]+exact[2]; double CE=cherr[0]+cherr[1]+cherr[2];
    #define PMODE(i) n[i], n[i]?100.0*exact[i]/n[i]:0.0, n[i]?cherr[i]/(3.0*n[i]):0.0
    fprintf(stderr,"[shaded-color f=%u] verts=%d exact=%d(%.1f%%) meanChErr=%.2f | "
        "gouraud[v=%d %.0f%% e%.1f] flatBasic[v=%d %.0f%% e%.1f] flatMetal[v=%d %.0f%% e%.1f] nohl=%d\n",
        openpete_frame, N, E, N?100.0*E/N:0.0, N?CE/(3.0*N):0.0,
        PMODE(0), PMODE(1), PMODE(2), s_sh_nohl);
    #undef PMODE
}

/* Specular (shiny-metal) per-face colour is a GTE env-mapped reflection
 * (.L800226F8: light matrix D_800770C8+0x18 · face normal, atan2 reflection,
 * env lookup) gated by the metal flag in m_SpecularMetalColor's high byte. The
 * regular per-vertex colour path produces only the near-black DPCS *base*
 * (+0x1100), so specular metal rendered dark. Until the reflection is re-derived
 * natively, reuse the .s's already-correct per-face colour from the captured
 * prims (NN-matched to the walk geometry, per-vertex by nearest point) -- the
 * same "reuse the game-computed colour" the regular path does with the +0x1100
 * scratch, just per-face. Walk geometry (float, no wobble) is kept; only RGB is
 * overwritten. Walk tris with no nearby prim (the game's distance-culled far
 * mobys) keep their base colour. */
static void recolor_walk_from_prims(void) {
    build_prim_tris();
    for (int wi = 0; wi < s_wtri_n; wi++) {
        ntri_t* w = &s_wtri[wi];
        double wcx = (w->v[0].sx + w->v[1].sx + w->v[2].sx) / 3.0;
        double wcy = (w->v[0].sy + w->v[1].sy + w->v[2].sy) / 3.0;
        double best = 1e18; int bp = -1;
        for (int pi = 0; pi < s_ptri_n; pi++) {
            double pcx = (s_ptri[pi].x[0] + s_ptri[pi].x[1] + s_ptri[pi].x[2]) / 3.0;
            double pcy = (s_ptri[pi].y[0] + s_ptri[pi].y[1] + s_ptri[pi].y[2]) / 3.0;
            if (fabs(pcx - wcx) > 32.0 || fabs(pcy - wcy) > 32.0) continue;
            double c = tri_match_cost(w, &s_ptri[pi]);
            if (c < best) { best = c; bp = pi; }
        }
        if (bp < 0 || best > 8.0) continue;            /* no nearby prim -> keep base */
        ptri* P = &s_ptri[bp];
        for (int k = 0; k < 3; k++) {                  /* per-vertex: nearest prim vertex */
            double bd = 1e18; int bj = 0;
            for (int j = 0; j < 3; j++) {
                double dx = w->v[k].sx - P->x[j], dy = w->v[k].sy - P->y[j];
                double d = dx * dx + dy * dy; if (d < bd) { bd = d; bj = j; }
            }
            w->v[k].rgb = P->rgb[bj];                   /* env-metal colour */
            w->v[k].u = P->u[bj]; w->v[k].v = P->v[bj]; /* env-reflection UVs (the shine) */
        }
        /* texture page/CLUT: decode the prim's raw tpage/clut like the walk's Tiledef.
         * The shiny metal is an env-mapped texture; the per-face reflection picks the
         * tpage/UVs, which the static-Tiledef walk can't reproduce. */
        if (P->tex) {
            int tp = P->tpage, cl = P->clut;
            w->textured = 1;
            w->tpage_x = (tp & 0xF) * 64; w->tpage_y = ((tp >> 4) & 1) * 256;
            int mode = (tp >> 7) & 3; w->bpp = (mode == 0) ? 4 : (mode == 1) ? 8 : 16;
            w->clut_x = (cl & 0x3F) * 16; w->clut_y = (cl >> 6) & 0x1FF;
        }
    }
}

/* ---- specular env-reflection UV (#1), native float ---------------------
 * The shiny-metal faces reflect the environment: the .s computes per-vertex UV
 * from refl = metalMatrix . (viewPos-TR) mapped to spherical coords (atan2 + the
 * D_8006D908/D_8006CBF8 tables), gated by m_SpecularMetalColor's high byte and
 * offset per-moby by the camera heading (.L800226F8/.L800222C0). metalMatrix
 * (D_800770C8+0x18) is the camera rotation with z negated (MetalBuildMatrix,
 * update-side), so refl is already CAMERA-relative -> the reflection tracks the
 * camera for free. We re-derive in FLOAT (design choice): a clean spherical map,
 * HD-ready, the prim UV is a closeness/camera-tracking sanity check (not 100%).*/
static int   s_metalM[3][3];     /* D_800770C8+0x18, int16 1.0=4096 */
static int   s_camHead;          /* g_Camera+0x50 (camera heading word) */
/* s_specL[3]/s_specLlen forward-declared above build_walk_tris (highlight uses them) */
#define D_METAL_TILEDEF 0x800756F0u   /* static metal env-texture Tiledef (level-load) */
static void specular_env_load(CPUState* cpu) {
    read_matrix(cpu, D_SPEC_LIGHT + 0x18u, s_metalM);
    s_camHead = (int)cpu->read_word(G_CAMERA + 0x50u);
    s_specL[0] = (float)(int32_t)cpu->read_word(D_SPEC_LIGHT + 0xCu);
    s_specL[1] = (float)(int32_t)cpu->read_word(D_SPEC_LIGHT + 0x10u);
    s_specL[2] = (float)(int32_t)cpu->read_word(D_SPEC_LIGHT + 0x14u);
    s_specLlen = sqrtf(s_specL[0]*s_specL[0] + s_specL[1]*s_specL[1] + s_specL[2]*s_specL[2]);
    if (s_specLlen < 1.0f) s_specLlen = 1.0f;
    /* metal env texture page/CLUT: D_800756F0 word0 hi=clut, word1 hi=tpage
     * (empirically clut=0x1C20 tpage=0x80AC; == the captured env prims). */
    unsigned cl = cpu->read_word(D_METAL_TILEDEF) >> 16;
    unsigned tp = cpu->read_word(D_METAL_TILEDEF + 4u) >> 16;
    s_env_tpx = (int)(tp & 0xFu) * 64; s_env_tpy = (int)((tp >> 4) & 1u) * 256;
    int mode = (int)((tp >> 7) & 3u); s_env_bpp = (mode == 0) ? 4 : (mode == 1) ? 8 : 16;
    s_env_clx = (int)(cl & 0x3Fu) * 16; s_env_cly = (int)((cl >> 6) & 0x1FFu);
}
/* refl = metalMatrix . vec  (vec = the rotated vertex offset ex/ey/ez). */
static void specular_env_refl(float ex, float ey, float ez, float r[3]) {
    r[0] = (s_metalM[0][0]*ex + s_metalM[0][1]*ey + s_metalM[0][2]*ez) / 4096.0f;
    r[1] = (s_metalM[1][0]*ex + s_metalM[1][1]*ey + s_metalM[1][2]*ez) / 4096.0f;
    r[2] = (s_metalM[2][0]*ex + s_metalM[2][1]*ey + s_metalM[2][2]*ez) / 4096.0f;
}

/* CALIBRATION (falsify-before-ship): for the metal/env textured walk faces,
 * dump refl + several candidate spherical mappings alongside the captured prim
 * U/V/tpage so the exact axis/scale/origin can be read off real data before the
 * final mapping is wired in. Mirrors recolor's NN walk->prim match. */
#define PI_F 3.14159265358979f
static float wrap128(float x) { x = fmodf(x, 128.0f); return x < 0 ? x + 128.0f : x; }
/* Native env-UV, fitted byte-exact (<=1 texel) to the prim-UV dump 2026-06-15:
 *   refl = metalMatrix . (viewPos-TR)              [camera-relative pitch/roll]
 *   u = (52.5 - (128/2pi)*atan2(rx,ry) - camHead/16) mod 128
 *   v = 64 + (128/pi)*asin(rz/|r|)
 * The metalMatrix (MetalBuildMatrix, z-negated camera rot) is NOT rebuilt every
 * frame, so the .s tracks fine camera YAW with a per-frame camHead term
 * (g_Camera+0x50, an 11-bit 2048/circle heading; 2048 -> the 128-texel azimuth
 * wrap == camHead/16). This keeps the reflection WORLD-anchored: turning the
 * camera slides the reflected environment across the metal. */
static void specular_env_uv(float ex, float ey, float ez, int* pu, int* pv) {
    float r[3]; specular_env_refl(ex, ey, ez, r);
    float len = sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    float az = atan2f(r[0], r[1]);
    float el = len > 0.001f ? asinf(r[2] / len) : 0.0f;
    int u = (int)lrintf(wrap128(52.5f - (128.0f / (2.0f*PI_F)) * az - s_camHead / 16.0f));
    int v = (int)lrintf(64.0f + (128.0f / PI_F) * el);
    if (u < 0) u = 0; if (u > 255) u = 255;
    if (v < 0) v = 0; if (v > 255) v = 255;
    *pu = u; *pv = v;
}
static void specular_env_calibrate(CPUState* cpu) {
    specular_env_load(cpu);
    build_prim_tris();
    fprintf(stderr, "[spec-env f=%u] metalM=[%d %d %d|%d %d %d|%d %d %d] camHead=%d nrecs=%d "
        "D756F0=%08X %08X clutmix\n",
        openpete_frame, s_metalM[0][0],s_metalM[0][1],s_metalM[0][2],
        s_metalM[1][0],s_metalM[1][1],s_metalM[1][2], s_metalM[2][0],s_metalM[2][1],s_metalM[2][2],
        s_camHead, s_nrecs, cpu->read_word(0x800756F0u), cpu->read_word(0x800756F4u));
    for (int r = 0; r < s_nrecs; r++)
        fprintf(stderr, "  rec%d metal=%d mscol=%08X moby=%08X\n", r,
            s_recs[r].metal, s_recs[r].mscol, cpu->read_word(s_recs[r].addr + 0x34));
    int dumped = 0, nv = 0; double sumu = 0, sumv = 0, sgnu = 0; int maxu = 0, maxv = 0;
    for (int wi = 0; wi < s_wtri_n && dumped < 16; wi++) {
        ntri_t* w = &s_wtri[wi];
        if (!w->textured) continue;
        double wcx=(w->v[0].sx+w->v[1].sx+w->v[2].sx)/3.0, wcy=(w->v[0].sy+w->v[1].sy+w->v[2].sy)/3.0;
        double best=1e18; int bp=-1;
        for (int pi=0; pi<s_ptri_n; pi++) {
            double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
            double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
            if (fabs(pcx-wcx)>32.0 || fabs(pcy-wcy)>32.0) continue;
            double c=tri_match_cost(w,&s_ptri[pi]); if (c<best){best=c;bp=pi;}
        }
        if (bp<0 || best>8.0) continue;
        ptri* P=&s_ptri[bp];
        for (int k=0;k<3;k++) {
            double bd=1e18; int bj=0;
            for (int j=0;j<3;j++){ double dx=w->v[k].sx-P->x[j], dy=w->v[k].sy-P->y[j];
                double d=dx*dx+dy*dy; if(d<bd){bd=d;bj=j;} }
            float r[3]; specular_env_refl(w->v[k].ex,w->v[k].ey,w->v[k].ez,r);
            int nu, nv2; specular_env_uv(w->v[k].ex,w->v[k].ey,w->v[k].ez, &nu, &nv2);
            int pu = P->u[bj], pv = P->v[bj];
            int du = nu - (pu & 127); if (du > 64) du -= 128; if (du < -64) du += 128;
            int dv = nv2 - pv;
            if (pu || pv) {  /* skip the (0,0) untextured-prim matches */
                nv++; sumu += fabs((double)du); sumv += fabs((double)dv); sgnu += du;
                if (abs(du) > maxu) maxu = abs(du); if (abs(dv) > maxv) maxv = abs(dv);
            }
            fprintf(stderr, "  refl(%.0f %.0f %.0f) -> uv(%d,%d) prim(%d,%d) d(%d,%d) clut=%04X\n",
                r[0],r[1],r[2], nu,nv2, pu,pv, du,dv, P->clut);
        }
        dumped++;
    }
    fprintf(stderr, "[spec-env f=%u] camHead=%d dumped %d faces, %d verts: meanErr u=%.1f v=%.1f "
        "signedU=%.2f maxErr u=%d v=%d\n",
        openpete_frame, s_camHead, dumped, nv, nv?sumu/nv:0, nv?sumv/nv:0, nv?sgnu/nv:0, maxu, maxv);
}

/* SPECULAR HIGHLIGHT correlation (#1 follow-up: the flat-lit chest). The .s gives
 * non-metal shiny faces a per-face highlight (.L80021DB4: OP screen-face-normal ->
 * normalize -> CC against m_SpecularMetalColor + the specular light) that REPLACES
 * the per-vertex base colour. The native walk omits it -> flat/dim. Dump the
 * view-space face-normal . specular-light vs the prim colour to find the float
 * model. Runs BEFORE recolor so w->v[].rgb is still the native base. */
static void specular_hl_calibrate(CPUState* cpu) {
    int lx=(int32_t)cpu->read_word(D_SPEC_LIGHT+0xCu);   /* m_specularLightDirectionMoby */
    int ly=(int32_t)cpu->read_word(D_SPEC_LIGHT+0x10u);
    int lz=(int32_t)cpu->read_word(D_SPEC_LIGHT+0x14u);
    float Llen=sqrtf((float)(lx*lx+ly*ly+lz*lz)); if(Llen<1) Llen=1;
    build_prim_tris();
    fprintf(stderr, "[spec-hl f=%u] light=(%d,%d,%d)\n", openpete_frame, lx,ly,lz);
    int dumped=0;
    for (int wi=0; wi<s_wtri_n && dumped<24; wi++) {
        ntri_t* w=&s_wtri[wi];
        double wcx=(w->v[0].sx+w->v[1].sx+w->v[2].sx)/3.0, wcy=(w->v[0].sy+w->v[1].sy+w->v[2].sy)/3.0;
        double best=1e18; int bp=-1;
        for (int pi=0;pi<s_ptri_n;pi++){
            double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
            double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
            if(fabs(pcx-wcx)>32.0||fabs(pcy-wcy)>32.0) continue;
            double c=tri_match_cost(w,&s_ptri[pi]); if(c<best){best=c;bp=pi;}
        }
        if(bp<0||best>8.0) continue;
        ptri* P=&s_ptri[bp];
        /* view-space face normal from the rotated vertex offsets */
        float e1x=w->v[1].ex-w->v[0].ex, e1y=w->v[1].ey-w->v[0].ey, e1z=w->v[1].ez-w->v[0].ez;
        float e2x=w->v[2].ex-w->v[0].ex, e2y=w->v[2].ey-w->v[0].ey, e2z=w->v[2].ez-w->v[0].ez;
        float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
        float nl=sqrtf(nx*nx+ny*ny+nz*nz); if(nl<0.001f) nl=1;
        float dotp=(nx*lx+ny*ly+nz*lz)/(nl*Llen);   /* [-1,1] both signs */
        uint32_t b=w->v[0].rgb; int br=b&0xFF, bg=(b>>8)&0xFF, bb=(b>>16)&0xFF;
        uint32_t pr0=P->rgb[0]; int pr=pr0&0xFF, pg=(pr0>>8)&0xFF, pb=(pr0>>16)&0xFF;
        fprintf(stderr, "  NL=%+.3f tex=%d | base(%d,%d,%d) L%d -> prim(%d,%d,%d) L%d  ratio=%.2f\n",
            dotp, w->textured, br,bg,bb,(br+bg+bb)/3, pr,pg,pb,(pr+pg+pb)/3,
            (br+bg+bb)?(double)(pr+pg+pb)/(br+bg+bb):0.0);
        dumped++;
    }
}

/* Append the moby triangles to the native scene (panel 0). Frame-keyed begin
 * composes with the cyclorama (drawn first, owns the bg fill) — mobys append
 * on top. Depth ordering is the real per-pixel Z-buffer (3a.3): each vertex
 * carries its own view-space Z, so opaque faces sort correctly regardless of
 * submission order. Determinism-safe: host-side display only. */
static void push_scene_moby(void) {
    if (!render_path_enabled(RP_MOBY)) return;  /* --render-paths: mobys not selected */
    static int s_logged = 0;
    if (!s_logged && s_wtri_n > 0) {
        s_logged = 1;
        fprintf(stderr, "[moby-native] feeding panel 0: %d moby tris from gamestate "
                "(3a.2b independent face-stream walk) frame %u\n", s_wtri_n, openpete_frame);
    }
    openpete_native_scene_begin_frame(openpete_frame);
    /* mobys cover shadows (ACTOR); HUD/UI mobys overlay on top. Stamped per-tri
     * from the walk's UI tag (0=ACTOR, 1=HUD flat overlay, 2=HUD3D depth-sorted
     * icon): the in-game 3D HUD icons (treasure/dragon/lives, the static g_Hud
     * range) go to HUD3D so their own faces Z-sort (depth-off paints them
     * inside-out); the flat near-OT text/2D billboards stay HUD. */
    for (int i = 0; i < s_wtri_n; i++) {
        openpete_native_scene_set_category(s_wtri_ui[i] == 2 ? OPENPETE_SCENE_HUD3D
                                          : s_wtri_ui[i] == 1 ? OPENPETE_SCENE_HUD
                                                              : OPENPETE_SCENE_ACTOR);
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
    openpete_native_scene_set_category(OPENPETE_SCENE_WORLD);
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
    /* SPECULAR queue is NOT relocated by --no-lod (see REC_BASE note); REGULAR is. */
    s_rec_base = (mat && mat[0]=='S' && mat[1]=='P') ? (D_SONY + 0x1600u) : REC_BASE;

    uint32_t start = cpu->read_word(POLYBUF_CURSOR);
    render_fn(cpu);                            /* run the real renderer */
    uint32_t end = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_MOBY, start, end);  /* polybuf range for the OT filter */

    capture_gte(cpu, start, end);
    int nrec = project_all_records(cpu);      /* native float verts -> s_proj */

    /* The .s runs REGULAR (func_8001F798) and SPECULAR (func_80020F34) over the
     * same +0x1600 record queue; SPECULAR re-draws the identical geometry to overlay
     * an additive per-face specular highlight (PSX can't bake it into one pass), so
     * the two .s passes OVERLAP at one pose. The native bakes the specular shading
     * per-face in build_walk_tris (#1) already, so re-feeding the same moby from the
     * SPECULAR pass is redundant — AND wrong-pose: the native re-projects AFTER
     * func_80020F34 (and the intervening specular cull) has advanced the per-record
     * keyframe/blend state, so the SPEC feed paints a SECOND, different animation
     * pose over the REGULAR one (the "death animation shows 2 frames at once"
     * doubling — e.g. a dying moby read blend=3072 in REG, blend=0 in SPEC). So
     * feed each moby ONCE: the REGULAR pass feeds its whole set and snapshots it
     * (s_reg_moby); the SPECULAR pass feeds ONLY mobys REGULAR did not (build_walk_tris
     * skips records whose moby is in s_reg_moby) — which keeps specular-ONLY mobys
     * (e.g. the rescue dragon the specular cull adds to a slot a regular moby held at
     * REGULAR-read time; also the disjoint specular queue under --no-lod). */
    int is_spec = (mat && mat[0]=='S' && mat[1]=='P');
    if (!is_spec) moby_build_validate(cpu);   /* OPENPETE_MOBY_BUILDDBG: host-builder A/B vs the .s records */
    if (!is_spec) {                           /* REGULAR runs first — snapshot its moby set */
        s_reg_moby_n = 0;
        for (int r = 0; r < nrec && s_reg_moby_n < MAX_RECORDS; r++)
            s_reg_moby[s_reg_moby_n++] = s_recs[r].moby;
    }
    if (feed) {
        if (!is_spec) append_revealed_regular(cpu);  /* widescreen: host-build records for mobys the 4:3 cull skipped */
        build_walk_tris(cpu);                 /* 3a.2b walk; SPEC dedups vs s_reg_moby; shading baked (#1) */
        push_scene_moby();                    /* recolor_walk_from_prims dropped: colour already native
                                               * (walk +0x1100/base), env-UV now native (build_walk_tris) */
    }

    if (verify) {
        static int s_dumped = 0, s_reports = 0;
        unsigned vf = moby_verify_frame();
        int target = (s_gte_n > 0 && s_reports < 120 &&
                      ((openpete_frame >= vf && openpete_frame <= vf + 12u) ||   /* dense window at target frame */
                       (openpete_frame >= vf && openpete_frame % 200u == 0)));   /* + sparse across levels */
        if (target && !s_dumped) { hex_dump(cpu, start, end); s_dumped = 1; }
        if (target) {
            compare_and_report(nrec);
            build_walk_tris(cpu);             /* 3a.2b independent walk + topology A/B */
            compare_walk_vs_prims();
            if (mat && mat[0] == 'S' && mat[1] == 'P') {   /* confirm specular recolor attaches the prim colour */
                specular_env_calibrate(cpu);  /* #1 env-UV calibration dump (native refl vs prim UV) */
                specular_hl_calibrate(cpu);   /* #1 follow-up: highlight (face-normal.light vs prim) */
                recolor_walk_from_prims();
                compare_walk_vs_prims();      /* the post-recolor col= should jump high */
            }
            s_reports++;
        }
        static unsigned s_codes_at = 0;
        if (s_gte_n > 0 && openpete_frame >= s_codes_at + 2000) {
            s_codes_at = openpete_frame; report_code_hist();
        }
    }
}

/* OPENPETE_MOBY_REVEALDBG: size the widescreen moby-reveal problem (the live-moby
 * task's mandated first step). Walks g_LevelMobys, replicates the .s moby cone
 * cull (spyro-moby-update-vs-render.md §3.3) at 4:3 AND at the active widescreen
 * factor, and classifies each moby the wide cone reveals (in the wide cone, NOT
 * the 4:3 cone) as LIVE (on the Spyro-anchored update list → animating, current
 * pose) or FROZEN (not updating → stale pose). The update gate is camera-angle-
 * INDEPENDENT (Spyro-anchored m_UpdateDistance xy-disk + always/never sentinels +
 * the m_WasDrawn grace), so a revealed moby being LIVE means we can draw it at its
 * current gameplay pose with no synthesis (the easy path); FROZEN means it needs
 * display-only animation (the deferred path). The live/frozen ratio sizes the
 * split. Read-only; prints once at the target frame. */
static void moby_reveal_dbg(CPUState* cpu) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_MOBY_REVEALDBG"); s_on = (e && *e && *e != '0'); }
    if (!s_on) return;
    static int s_done = 0;
    if (s_done || openpete_frame < 1850u) return;
    s_done = 1;

    float F = openpete_native_widescreen_factor();   /* 1.0 at native 4:3 */
    int M[3][3]; read_matrix(cpu, G_CAMERA + 0x00, M);   /* moby transform uses ProjMatrix */
    int camx = (int32_t)cpu->read_word(G_CAMERA + 0x28);
    int camy = (int32_t)cpu->read_word(G_CAMERA + 0x2C);
    int camz = (int32_t)cpu->read_word(G_CAMERA + 0x30);
    int spx = (int32_t)cpu->read_word(G_SPYRO + 0x00);
    int spy = (int32_t)cpu->read_word(G_SPYRO + 0x04);

    uint32_t m = cpu->read_word(G_LEVELMOBYS);
    int alive=0, drawn_s=0, in43=0, inwide=0, revealed=0, rev_live=0, rev_frozen=0;
    int guard = 0;
    while (m && guard++ < 4096) {
        uint8_t st = cpu->read_byte(m + 0x48);
        if (st == 0xFF) break;                       /* sentinel ends the array */
        if (st >= 0x80) { m += 0x58; continue; }     /* dead/inactive */
        alive++;
        uint8_t wasDrawn = cpu->read_byte(m + 0x51);
        uint8_t updDist  = cpu->read_byte(m + 0x52);
        uint8_t rRad     = cpu->read_byte(m + 0x50);
        if (wasDrawn) drawn_s++;

        int mx = (int32_t)cpu->read_word(m + 0x0C);
        int my = (int32_t)cpu->read_word(m + 0x10);
        int mz = (int32_t)cpu->read_word(m + 0x14);
        /* camera-relative, .s moby swizzle V=(dy,dz,dx)>>2 through ProjMatrix */
        double dx = (double)((mx - camx) >> 2);
        double dy = (double)((camy - my) >> 2);
        double dz = (double)((camz - mz) >> 2);
        double rx = (M[0][0]*dy + M[0][1]*dz + M[0][2]*dx) / 4096.0;
        double rz = (M[2][0]*dy + M[2][1]*dz + M[2][2]*dx) / 4096.0;
        double radius = (double)rRad * 256.0;        /* m_RenderRadius<<8 */
        if (rz + 128.0 <= 0.0) { m += 0x58; continue; }   /* behind camera */
        if (rz - radius >= 0.0) { m += 0x58; continue; }  /* beyond render distance */
        double arx = rx < 0 ? -rx : rx;
        /* X-cone (r_moby.s 0x80022C04): cull if 4*(|rx|-102) - 3*S*(rz+77) >= 0;
         * S=1 → 4:3 (slope 0.75), S=F → widened horizontal half-FOV. */
        int pass43   = (4.0*(arx-102.0) - 3.0*       (rz+77.0)) < 0.0;
        int passwide = (4.0*(arx-102.0) - 3.0*(double)F*(rz+77.0)) < 0.0;
        if (pass43)   in43++;
        if (passwide) inwide++;
        if (passwide && !pass43) {
            revealed++;
            int live = wasDrawn != 0;                /* render grace = on update list */
            if (!live && updDist != 0) {             /* else the Spyro-anchored distance gate */
                if (updDist >= 0x80) live = 1;       /* always-update sentinel */
                else {
                    double sdx = (double)(mx - spx), sdy = (double)(my - spy);
                    double thr = (double)updDist * 1024.0;
                    if (sdx*sdx + sdy*sdy <= thr*thr) live = 1;
                }
            }
            if (live) rev_live++; else rev_frozen++;
        }
        m += 0x58;
    }
    fprintf(stderr,
        "[moby-reveal] frame=%u F=%.3f alive=%d drawn(.s m_WasDrawn)=%d  "
        "myCull{4:3=%d wide=%d}  REVEALED=%d (live=%d frozen=%d)\n",
        openpete_frame, F, alive, drawn_s, in43, inwide, revealed, rev_live, rev_frozen);
}

static int on_8001F798_moby_native(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!moby_verify_enabled() && !openpete_native_render_enabled() && !render_paths_active()) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    moby_native_pass(cpu, func_8001F798, "REG");   /* regular material */
    moby_reveal_dbg(cpu);                          /* widescreen-reveal sizing (env-gated) */
    s_in = 0;
    return 1;
}
static int on_80020F34_moby_native(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!moby_verify_enabled() && !openpete_native_render_enabled() && !render_paths_active()) return 0;
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

/* SHADED geometry A/B. Decode the moby's verts (3 signed bytes x2: byte0=Z,
 * byte1=X, byte2=Y per .L80023010) from model[+0x4] (count=model[+0x0]) and
 * project with the per-moby R+TR. The attract demo has exactly ONE shaded moby,
 * so the GTE R+TR left after func_80022A2C ARE that moby's (RTPS doesn't modify
 * them and the face loop doesn't touch them) -- read them post-hoc to validate
 * the signed-byte decode + projection independent of the (harder) R-rebuild.
 * Set-matches the decoded verts vs the captured shaded prims, like the regular
 * vertex A/B. */
#define SINCOS_TABLE 0x8006CBF8u   /* sin[idx], cos at [idx+0x80]; s16 1.12 */

static void mat_mul3(const int A[3][3], const int B[3][3], int O[3][3]) {
    for (int i=0;i<3;i++) for (int j=0;j<3;j++)
        O[i][j] = (A[i][0]*B[0][j] + A[i][1]*B[1][j] + A[i][2]*B[2][j]) >> 12;
}

/* Compose the moby's full euler rotation onto `start`, mirroring func_80022A2C's
 * matrix build (block_80022DBC..80022F34): out = start·Ry·Rx·Rz, applied in that
 * order. The per-axis rotation columns come straight from the .s's MVMVA inputs
 * (Y@80022DE4, X@80022E90, Z@80022F40); the sin/cos table values go in verbatim
 * (sin may be negative). VERIFIED bit-exact vs the live RT registers: base·Ry =
 * gte_ctrl[0..4] for a billboard moby (cliff_town cls 0x3A). Euler indices:
 * Y=(rot>>15)&0x1FE, X=(rot&0xFF00)>>7, Z=(rot&0xFF)<<1 (rot=m_Rotation@+0x44). */
static void shaded_compose_euler(CPUState* cpu, const int start[3][3], uint32_t rot, int out[3][3]) {
    uint32_t iy=(rot>>15)&0x1FEu, ix=(rot&0xFF00u)>>7, iz=(rot&0xFFu)<<1;
    int sy=(int16_t)cpu->read_half(SINCOS_TABLE+iy), cy=(int16_t)cpu->read_half(SINCOS_TABLE+0x80u+iy);
    int sx=(int16_t)cpu->read_half(SINCOS_TABLE+ix), cx=(int16_t)cpu->read_half(SINCOS_TABLE+0x80u+ix);
    int sz=(int16_t)cpu->read_half(SINCOS_TABLE+iz), cz=(int16_t)cpu->read_half(SINCOS_TABLE+0x80u+iz);
    int Ry[3][3]={{cy,0,-sy},{0,4096,0},{sy,0,cy}};
    int Rx[3][3]={{4096,0,0},{0,cx,-sx},{0,sx,cx}};
    int Rz[3][3]={{cz,-sz,0},{sz,cz,0},{0,0,4096}};
    int t1[3][3], t2[3][3];
    mat_mul3(start, Ry, t1);
    mat_mul3(t1, Rx, t2);
    mat_mul3(t2, Rz, out);
}

static void shaded_verify(CPUState* cpu) {
    /* The +0x2400 list holds MULTIPLE live shaded mobys -- project EVERY one
     * (each with its own R = ProjMatrix . Ry(m_Rotation axis0) and TR =
     * ProjMatrix . (pos - cam)) and accumulate, then A/B vs all shaded prims.
     * (axis0 = Y confirmed; axis1/2 euler TODO for fuller rotations.) */
    int PM[3][3]; read_matrix(cpu, G_CAMERA, PM);
    int32_t cx=(int32_t)cpu->read_word(G_CAMERA+0x28), cy=(int32_t)cpu->read_word(G_CAMERA+0x2C), cz=(int32_t)cpu->read_word(G_CAMERA+0x30);
    double ofx=(double)((int32_t)cpu->gte_ctrl[24]>>16), ofy=(double)((int32_t)cpu->gte_ctrl[25]>>16);
    double H=(double)(cpu->gte_ctrl[26]&0xFFFFu);
    static proj_xy sv[8192]; int n=0; int nmoby=0;
    for (int r=0; r<256; r++) {
        uint32_t m = cpu->read_word(SHADED_BASE + (uint32_t)r*4u);
        if (m < 0x80000000u) break;
        nmoby++;
        uint32_t cls = cpu->read_half(m + 0x36);
        uint32_t model = cpu->read_word(G_MODELS + cls*4u); if (model<0x80000000u) model|=0x80000000u;
        uint32_t vbase = cpu->read_word(model + 0x4u);      if (vbase<0x80000000u) vbase|=0x80000000u;
        int count = cpu->read_byte(model + 0x0u) & 0xFF;
        int32_t px=(int32_t)cpu->read_word(m+0xC), py=(int32_t)cpu->read_word(m+0x10), pz=(int32_t)cpu->read_word(m+0x14);
        /* billboard/2D vs 3D path: flag = bit7 of the low byte at m+0x50 (asm
         * `lhu a0,0x50; sll a0,24; bltz -> .L80022D1C`). */
        int billboard = (cpu->read_byte(m + 0x50) & 0x80) != 0;
        int M[3][3], TR[3]; double mofx, mofy;
        if (billboard) {                              /* .L80022D1C */
            M[0][0]=4096;M[0][1]=0;M[0][2]=0; M[1][0]=0;M[1][1]=2560;M[1][2]=0; M[2][0]=0;M[2][1]=0;M[2][2]=4096;
            TR[0]=0; TR[1]=0; TR[2]=pz>>1;            /* OFX/OFY = pos.x/y; TRZ = pos.z>>1 */
            mofx=(double)px; mofy=(double)py;
        } else {                                      /* 3D: R = ProjMatrix . Ry(axis0), TR = PM.(pos-cam) */
            uint32_t rot = cpu->read_word(m + 0x44u);
            uint32_t idx0 = (rot >> 15) & 0x1FEu;
            int s0 = (int16_t)cpu->read_half(SINCOS_TABLE + idx0);
            int c0 = (int16_t)cpu->read_half(SINCOS_TABLE + 0x80u + idx0);
            int Ry[3][3] = {{c0,0,s0},{0,4096,0},{-s0,0,c0}};
            for (int i=0;i<3;i++) for (int j=0;j<3;j++)
                M[i][j] = (PM[i][0]*Ry[0][j] + PM[i][1]*Ry[1][j] + PM[i][2]*Ry[2][j]) >> 12;
            int32_t dx=px-cx, dy=py-cy, dz=pz-cz;
            for (int i=0;i<3;i++) TR[i] = (PM[i][0]*dx + PM[i][1]*dy + PM[i][2]*dz) >> 12;
            mofx=ofx; mofy=ofy;
        }
        for (int i=0;i<count && n<8192;i++) {
            uint32_t p = vbase + (uint32_t)i*3u;
            uint32_t w = cpu->read_byte(p) | (cpu->read_byte(p+1)<<8) | (cpu->read_byte(p+2)<<16);
            int VZ=((int32_t)(w<<24))>>23, VX=((int32_t)(w<<16))>>23, VY=((int32_t)(w<<8))>>23;
            double sx,sy; int sat;
            project(M,TR,(double)VX,(double)VY,(double)VZ, mofx,mofy,H, &sx,&sy,&sat);
            sv[n].sx=(float)sx; sv[n].sy=(float)sy; sv[n].sat=sat; n++;
        }
    }
    /* set-match shaded prim verts vs sv + centroid offset (to characterize any
     * constant projection-center error, e.g. a 3D-vs-billboard OFX mismatch). */
    int gv=0,near=0,div=0; double sum=0,mx=0;
    double mycx=0,mycy=0, pcx=0,pcy=0; int pcn=0;
    for (int i=0;i<n;i++){ mycx+=sv[i].sx; mycy+=sv[i].sy; } if(n){mycx/=n;mycy/=n;}
    for (int g=0; g<s_gte_n; g++) for (int k=0;k<s_gte[g].nv;k++) {
        float gx=s_gte[g].x[k], gy=s_gte[g].y[k]; gv++; pcx+=gx; pcy+=gy; pcn++;
        double best=1e18; int bs=1;
        for (int i=0;i<n;i++){ double dx=sv[i].sx-gx,dy=sv[i].sy-gy,d=dx*dx+dy*dy; if(d<best){best=d;bs=sv[i].sat;} }
        double dist=sqrt(best);
        if (bs){div++;continue;}
        if (dist<=4.0){near++;sum+=dist;if(dist>mx)mx=dist;}
    }
    if(pcn){pcx/=pcn;pcy/=pcn;}
    int clean=gv-div;
    fprintf(stderr,"[shaded-verify f=%u] mobys=%d nativeVerts=%d | "
            "myCentroid=(%.0f,%.0f) primCentroid=(%.0f,%.0f) offset=(%.0f,%.0f) | "
            "primVerts=%d match<=4px=%d/%d(%.1f%%) mean=%.2fpx\n",
            openpete_frame, nmoby, n, mycx,mycy, pcx,pcy, mycx-pcx, mycy-pcy,
            gv, near,clean, clean?100.0*near/clean:0.0, near?sum/near:0.0);
}

/* ---- shaded directional lighting (#2b) — faithful integer GTE mirror -------
 * Reproduces func_80022A2C's per-normal lighting (gouraud block_80023148 /
 * flat block_80023574) bit-for-bit so the native colour equals the .s
 * renderer's, instead of NN-sampling the .s's emitted prims. Op chain:
 *   IR = normal (3 signed bytes)
 *   MVMVA(mx=RT, v=IR, sf=1, lm=0)   -> view-space normal  (gte_mvmva)
 *   IR0 = (base>>23)&0x1E ; GPF(sf=0,lm=0): IR = IR0*IR     (gte_gpf)
 *   BK  = base colour channels<<? ; LC = light dir broadcast
 *   CC: light_color (BK + LC·IR) then color_output (white·IR) -> RGB
 * Light dir is read fresh per frame (s_shL); base colour per moby. */
static int s_shL[3];   /* int16-truncated directional-light dir (D_800770C8+0/4/8) */

static int32_t sat_ir_lm(int64_t v, int lm) {   /* mirrors GTEState::saturate_ir */
    int32_t mn = lm ? 0 : -0x8000;
    if (v < mn) return mn;
    if (v > 0x7FFF) return 0x7FFF;
    return (int32_t)v;
}
static int32_t sat_col8(int64_t v) {            /* mirrors GTEState::saturate_color */
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (int32_t)v;
}

/* RT = model->view rotation (same matrix used to project this moby's verts).
 * base = per-moby base-colour word. highlight=1 applies the gouraud spec boost
 * (block_800231C4..80023250); the flat path omits it. Returns 0x00BBGGRR (the
 * low 24 bits the .s stores, r in the low byte — matches s_gte/s_ptri rgb). */
static uint32_t shaded_lit_color(const int RT[3][3], int nx, int ny, int nz,
                                 uint32_t base, int highlight) {
    int64_t m1 = ((int64_t)RT[0][0]*nx + (int64_t)RT[0][1]*ny + (int64_t)RT[0][2]*nz) >> 12;
    int64_t m2 = ((int64_t)RT[1][0]*nx + (int64_t)RT[1][1]*ny + (int64_t)RT[1][2]*nz) >> 12;
    int64_t m3 = ((int64_t)RT[2][0]*nx + (int64_t)RT[2][1]*ny + (int64_t)RT[2][2]*nz) >> 12;
    int32_t ir1 = sat_ir_lm(m1,0), ir2 = sat_ir_lm(m2,0), ir3 = sat_ir_lm(m3,0);
    int32_t ir0 = (int32_t)(((uint32_t)base >> 23) & 0x1Eu);
    ir1 = sat_ir_lm((int64_t)ir0*ir1, 0);
    ir2 = sat_ir_lm((int64_t)ir0*ir2, 0);
    ir3 = sat_ir_lm((int64_t)ir0*ir3, 0);
    int32_t rbk = (int32_t)(((uint32_t)base << 4) & 0xFF0u);
    int32_t gbk = (int32_t)(((uint32_t)base >> 4) & 0xFF0u);
    int32_t bbk = (int32_t)(((uint32_t)base >> 12) & 0xFF0u);
    /* CC step 1 — light_color: LC matrix = light dir broadcast to all 3 rows */
    int64_t lc = (int64_t)s_shL[0]*ir1 + (int64_t)s_shL[1]*ir2 + (int64_t)s_shL[2]*ir3;
    ir1 = sat_ir_lm((((int64_t)rbk << 12) + lc) >> 12, 1);
    ir2 = sat_ir_lm((((int64_t)gbk << 12) + lc) >> 12, 1);
    ir3 = sat_ir_lm((((int64_t)bbk << 12) + lc) >> 12, 1);
    /* CC step 2 — color_output: RGBC = white (0xFFFFFF). MAC = (255*IR<<4)>>12 */
    int32_t M1 = (int32_t)(((int64_t)255*ir1) >> 8);
    int32_t M2 = (int32_t)(((int64_t)255*ir2) >> 8);
    int32_t M3 = (int32_t)(((int64_t)255*ir3) >> 8);
    int32_t oir1 = sat_ir_lm(M1,1), oir2 = sat_ir_lm(M2,1), oir3 = sat_ir_lm(M3,1);
    uint32_t a0 = (uint32_t)sat_col8(M1>>4) | ((uint32_t)sat_col8(M2>>4)<<8) | ((uint32_t)sat_col8(M3>>4)<<16);
    if (!highlight) return a0;
    /* gouraud spec boost */
    int32_t s1 = (int32_t)(base & 0xFFFFFFu);
    if ((int32_t)a0 - s1 < 0) return (uint32_t)s1;
    int32_t hn = (int32_t)((uint32_t)base >> 28);
    if (hn == 0) return a0;
    int32_t d = oir1 - (hn << 7) - rbk;
    if (d <= 0) return a0;
    d <<= 1;
    int32_t ch[3] = { oir1 + d, oir2 + d, oir3 + d };
    for (int k = 0; k < 3; k++) ch[k] = (ch[k] < 4096) ? (ch[k] >> 4) : 0xFF;
    return (uint32_t)ch[0] | ((uint32_t)ch[1]<<8) | ((uint32_t)ch[2]<<16);
}

#define D_METAL_COLTAB 0x8006E44Cu  /* shaded METAL base-colour table, 8B/entry, idx = m_SpecularMetalColor>>21 */

/* Shaded METAL sub-mode colour (word0 bit1, block_80023720) — env-mapped
 * reflection. The face normal is rotated to view space (RT·N), then the
 * reflection intensity = light·N' (the LCM is the directional light broadcast
 * across all rows, set from D_800770C8). The metal table entry (8B at
 * D_8006E44C[scolor>>21]) gives word0 = base tint, word1 = reflected tint; the
 * final colour = word0_base + (intensity·word1)>>12 + a brightness boost,
 * clamped 12->8. (This is the gem shine — without it gems read as the gray flat
 * table. The exact far-distance branch >>10/word0>>1 is approximated by the near
 * >>8 path; env-texture/depth select is display-irrelevant.) Returns 0x00BBGGRR. */
static uint32_t shaded_metal_color(const int RT[3][3], int nx, int ny, int nz,
                                   uint32_t w0, uint32_t w1) {
    int64_t m1=((int64_t)RT[0][0]*nx+(int64_t)RT[0][1]*ny+(int64_t)RT[0][2]*nz)>>12;
    int64_t m2=((int64_t)RT[1][0]*nx+(int64_t)RT[1][1]*ny+(int64_t)RT[1][2]*nz)>>12;
    int64_t m3=((int64_t)RT[2][0]*nx+(int64_t)RT[2][1]*ny+(int64_t)RT[2][2]*nz)>>12;
    int32_t ir1=sat_ir_lm(m1,0), ir2=sat_ir_lm(m2,0), ir3=sat_ir_lm(m3,0);
    int64_t refl=(int64_t)s_shL[0]*ir1+(int64_t)s_shL[1]*ir2+(int64_t)s_shL[2]*ir3;
    int32_t intensity=(int32_t)(refl>>8);
    int16_t ir0=(int16_t)intensity;
    int r1=(int)((w1<<4)&0xFF0u), g1=(int)((w1>>4)&0xFF0u), b1=(int)((w1>>12)&0xFF0u);
    int r0=(int)((w0<<4)&0xFF0u), g0=(int)((w0>>4)&0xFF0u), b0=(int)((w0>>12)&0xFF0u);
    int R=r0+(((int)ir0*r1)>>12), G=g0+(((int)ir0*g1)>>12), B=b0+(((int)ir0*b1)>>12);
    int bo=intensity-0x5C0; if(bo>0){ bo<<=3; R+=bo; G+=bo; B+=bo; }
    R = R<0?0:(R>=0xFFF?0xFF:(R>>4));
    G = G<0?0:(G>=0xFFF?0xFF:(G>>4));
    B = B<0?0:(B>=0xFFF?0xFF:(B>>4));
    return (uint32_t)R | ((uint32_t)G<<8) | ((uint32_t)B<<16);
}

/* Per-shaded-moby metadata recorded alongside the projected verts, so the
 * native face-stream walk (build_shaded_walk_tris) can index each moby's verts
 * in out[] and read its face stream. RT/base/model carry the lighting inputs. */
typedef struct { int sv_off, vcount; uint32_t faces; int fcount; int cls;
                 uint32_t model, base, moby; int RT[3][3]; int near_ot;
                 int vzspread; } shaded_rec;   /* vzspread = model-space Z extent (max-min);
                 * a large extent means a real 3D model that self-occludes → needs the HUD3D
                 * depth pass, vs a thin text/decal overlay that renders fine depth-off. */
static shaded_rec s_shrecs[256]; static int s_nshrecs;

/* Project EVERY shaded moby's verts (billboard or 3D, both verified 100%) into
 * out[]. Returns vertex count. Shared by shaded_verify + the feed. Also records
 * per-moby metadata in s_shrecs[] for the native topology walk. */
static int project_all_shaded(CPUState* cpu, proj_xy* out, int cap) {
    int PM[3][3]; read_matrix(cpu, G_CAMERA, PM);
    int32_t cx=(int32_t)cpu->read_word(G_CAMERA+0x28), cy=(int32_t)cpu->read_word(G_CAMERA+0x2C), cz=(int32_t)cpu->read_word(G_CAMERA+0x30);
    double ofx=(double)((int32_t)cpu->gte_ctrl[24]>>16), ofy=(double)((int32_t)cpu->gte_ctrl[25]>>16);
    double H=(double)(cpu->gte_ctrl[26]&0xFFFFu);
    /* directional light dir (int16-truncated on ctc2 into the LCM), refreshed
     * each frame from D_800770C8+0/+4/+8 (block_80022AD0). */
    s_shL[0]=(int16_t)cpu->read_word(D_SPEC_LIGHT+0); s_shL[1]=(int16_t)cpu->read_word(D_SPEC_LIGHT+4); s_shL[2]=(int16_t)cpu->read_word(D_SPEC_LIGHT+8);
    int n=0; s_nshrecs = 0;
    for (int r=0; r<256; r++) {
        uint32_t m = cpu->read_word(SHADED_BASE + (uint32_t)r*4u);
        if (m < 0x80000000u) break;
        uint32_t cls = cpu->read_half(m + 0x36);
        uint32_t model = cpu->read_word(G_MODELS + cls*4u); if (model<0x80000000u) model|=0x80000000u;
        uint32_t vbase = cpu->read_word(model + 0x4u);      if (vbase<0x80000000u) vbase|=0x80000000u;
        int count = cpu->read_byte(model + 0x0u) & 0xFF;
        uint32_t faces = cpu->read_word(model + 0xCu); if (faces<0x80000000u) faces|=0x80000000u;  /* shaded face stream */
        int fcount = cpu->read_byte(model + 0x1u) & 0xFF;   /* face count: base model+0xC, count model+0x1, stride 8 */
        int sv_off = n;
        int32_t px=(int32_t)cpu->read_word(m+0xC), py=(int32_t)cpu->read_word(m+0x10), pz=(int32_t)cpu->read_word(m+0x14);
        int billboard = (cpu->read_byte(m + 0x50) & 0x80) != 0;
        uint32_t rot = cpu->read_word(m + 0x44u);   /* m_Rotation (3 euler bytes) */
        /* OT-bias overlay flag: the .s billboard path (block_80022D90) computes the
         * OT index as (pz>>7) - ((int32_t)m_0x44 >> 24) clamped >= 0. A large bias
         * byte forces the index to 0 — the NEAR OT slot — so the moby draws OVER the
         * world/terrain (but still UNDER Spyro, which renders in a later pass). This
         * is how the game keeps HUD icons, the "DEMO MODE" banner, and the level-
         * intro text on top of the scene. Mirror it: such mobys go to the HUD/overlay
         * layer instead of being world-depth-sorted (else nearer terrain occludes
         * them). Only billboards carry this; 3D shaded mobys keep their own depth. */
        int near_ot = billboard && (((pz >> 7) - ((int32_t)rot >> 24)) <= 0);
        int M[3][3], TR[3]; double mofx, mofy;
        double vz_scale = 1.0;   /* depth -> full-world (set for the 3D path below) */
        if (billboard) {
            /* base = [4096 0 0/0 2560 0/0 0 4096] composed with the moby euler
             * (block_80022D64 base, then the Ry/Rx/Rz MVMVA chain). */
            int base[3][3]={{4096,0,0},{0,2560,0},{0,0,4096}};
            shaded_compose_euler(cpu, base, rot, M);
            TR[0]=0; TR[1]=0; TR[2]=pz>>1; mofx=(double)px; mofy=(double)py;
            /* Like the 3D path, TR[2] is HALF the true view-Z (the .s sets the GTE
             * depth to pz>>1, but orders the prim in the OT by pz>>7 — i.e. the FULL
             * pz). Scale the fed depth back to full-world so billboards (e.g. the
             * level-intro "THE ADVENTURE BEGINS" text, and any sprite moby) sort vs
             * terrain / Spyro / 3D mobys on the same scale. Without this they sit at
             * half-depth — artificially near — and a billboard wrongly occludes
             * actors that are actually in front of it (Spyro drew BEHIND the text). */
            vz_scale = 2.0;
        } else {
            /* model->view = PM (camera ProjMatrix) composed with the full moby
             * euler rotation (block_80022CE4 PM start, then the same chain). */
            shaded_compose_euler(cpu, PM, rot, M);
            /* TR (moby centre in view space) = the .s's swizzled centre MVMVA
             * (block_80022B40..80022D18): IR=((cam.y-my)>>2,(cam.z-mz)>>2,(mx-cam.x)>>2)
             * loaded as (IR1,IR2,IR3); MAC=(PM·IR)>>12; TR=MAC<<1; then an optional
             * GPF perspective scale by m_DrawScale (moby+0x57). NOT the naive
             * PM·(pos-cam) — that swizzle/scale mismatch sank 3D mobys (gems)
             * below ground (verified: gem v0 nnPrim 35-130px -> 0.1px). */
            int32_t Dx=(px-cx)>>2, Dy=(cy-py)>>2, Dz=(cz-pz)>>2;
            int64_t t1=((int64_t)PM[0][0]*Dy+(int64_t)PM[0][1]*Dz+(int64_t)PM[0][2]*Dx)>>12;
            int64_t t2=((int64_t)PM[1][0]*Dy+(int64_t)PM[1][1]*Dz+(int64_t)PM[1][2]*Dx)>>12;
            int64_t t3=((int64_t)PM[2][0]*Dy+(int64_t)PM[2][1]*Dz+(int64_t)PM[2][2]*Dx)>>12;
            TR[0]=(int)(t1<<1); TR[1]=(int)(t2<<1); TR[2]=(int)(t3<<1);
            uint8_t p57=cpu->read_byte(m+0x57);
            if (p57){ for(int i=0;i<3;i++){ int16_t ir=(int16_t)TR[i]; TR[i]=((int32_t)p57*ir)>>5; } }
            mofx=ofx; mofy=ofy;
            /* TR here = trueViewZ/2 (D=(pos-cam)>>2 then MAC<<1), or x p57/32 with
             * draw-scale: scale the fed depth back to full-world so shaded mobys
             * (gems) sort vs terrain/Spyro. p57=0 -> x2 (probe: ratio exactly 2.0);
             * p57!=0 -> x64/p57. Sign-preserving (near cull intact); depth-only. */
            vz_scale = p57 ? 64.0 / (double)p57 : 2.0;
        }
        int vzmin=1<<30, vzmax=-(1<<30);   /* model-space Z extent (HUD3D 3D-vs-flat test) */
        for (int i=0;i<count && n<cap;i++) {
            uint32_t p=vbase+(uint32_t)i*3u;
            uint32_t w=cpu->read_byte(p)|(cpu->read_byte(p+1)<<8)|(cpu->read_byte(p+2)<<16);
            int VZ=((int32_t)(w<<24))>>23, VX=((int32_t)(w<<16))>>23, VY=((int32_t)(w<<8))>>23;
            if (VZ<vzmin) vzmin=VZ; if (VZ>vzmax) vzmax=VZ;
            double sx,sy; int sat;
            /* TRUE per-vertex view-Z (project returns mac3). The old code threw this
             * away and stamped the moby-constant TR[2] on every vertex, collapsing
             * within-moby depth so the Z-buffer couldn't separate a moby's faces
             * (interior/back faces would punch through the front). Per-vertex Z is
             * what the real Z-buffer needs. */
            double vzz = project(M,TR,(double)VX,(double)VY,(double)VZ, mofx,mofy,H, &sx,&sy,&sat);
            out[n].sx=(float)sx; out[n].sy=(float)sy; out[n].vz=(float)(vzz*vz_scale); out[n].sat=sat;
            out[n].ex=out[n].ey=out[n].ez=0.0f;   /* env-UV unused on shaded faces */
            n++;
        }
        if (s_nshrecs < 256) {
            uint32_t scolor = cpu->read_word(m + 0x4Cu);   /* m_SpecularMetalColor */
            uint32_t base = cpu->read_word(D_SHADE_COLTAB + (uint32_t)((int32_t)scolor >> 22));
            shaded_rec* R = &s_shrecs[s_nshrecs++];
            R->sv_off=sv_off; R->vcount=n-sv_off; R->faces=faces; R->fcount=fcount; R->cls=(int)cls;
            R->model=model; R->base=base; R->moby=m; R->near_ot=near_ot;
            R->vzspread = (vzmax>vzmin) ? (vzmax-vzmin) : 0;
            for (int i=0;i<3;i++) for (int j=0;j<3;j++) R->RT[i][j]=M[i][j];
        }
    }
    return n;
}

/* Native-topology shaded walk (backlog #2a) — VERIFIED, FED. Derives the shaded
 * triangle set from the face stream (func_80022A2C .L800232A8) instead of riding
 * the .s's prims: per moby, word0 packs 4 vertex indices (idx_k =
 * (w0>>{21,14,7,0})&0x1FC, >>2 = slot into this moby's projected verts).
 * tri-vs-quad = (idx2==idx3):
 *   TRI (v0,v1,v2)             kept if NCLIP1>0
 *   QUAD (v0,v1,v2)+(v1,v3,v2) kept if NCLIP1>0 || NCLIP2(v3,v1,v2)<0
 * (bit0/bit1 of word0 only select the shading MODE — gouraud vs flat — not the
 * topology, so they're ignored here.) Positions come from project_all_shaded
 * (100% A/B-verified); s_shrecs[] gives each moby's vert base + face stream.
 *
 * SLOT MAP = IDENTITY (sv[slot]). Confirmed by reading the .s's projected SXY
 * straight from PSX scratchpad 0x1F800000 on a clean shaded moby (cliff_town
 * class 0x3A, 115 verts): direct sv[slot]-vs-scratch[slot] = ~1-2px (GTE
 * truncation) for every slot, and the topology A/B hit ~90% ONSCREEN precision.
 * The earlier "permutation / 33%" reading was a phantom of the attract demo's
 * degenerate-tiny shaded mobys (6-14px, verts ~2px apart), where NN matching and
 * the direct check both break down.
 *
 * COLOUR (#2b) is now native too — shaded_lit_color() reproduces the .s's
 * directional-lighting GTE chain bit-for-bit (verified vs the +0x2A00 scratchpad:
 * flat 100%, gouraud Y-only 61-64/64, err ~0.02). word0 bit0 selects the shading
 * MODE: gouraud (per-vertex normals at model+0x10, colour-indexed by word1) vs
 * flat (one colour from word1's 3-byte face normal). word0 bit1 on a flat face
 * marks the METAL env-reflection sub-mode (block_80023720) — NOT yet derived
 * natively (same family as #1 specular reflection); rendered as basic-flat for
 * now (approximate but leak-free). The feed reads ZERO .s prim output. */
static int build_shaded_walk_tris(CPUState* cpu) {
    static proj_xy sv[8192]; project_all_shaded(cpu, sv, 8192);   /* fills sv[] + s_shrecs[] */
    if (s_sh_nohl < 0) { const char* e=getenv("OPENPETE_SHADED_NOHL"); s_sh_nohl = (e&&*e!='0'); }
    s_shmode[0]=s_shmode[1]=s_shmode[2]=0;
    s_wtri_n = 0;
    for (int r = 0; r < s_nshrecs; r++) {
        shaded_rec* R = &s_shrecs[r];
        /* HUD/overlay tag (0=ACTOR, 1=HUD flat, 2=HUD3D depth-sorted). Two kinds of
         * moby reach the HUD layer: the static g_Hud icons (gem/dragon/Spyro-head/
         * key/digits) and the .s OT-bias near-OT billboards (level-intro/RESCUED
         * text, "DEMO MODE", and 3D transition props like the "TREASURE FOUND"
         * chest). The ones that are real 3D MODELS self-occlude and need their own
         * Z-buffer (HUD3D, depth-isolated, on top) or they render inside-out; thin
         * text/decal overlays (small model-space Z extent) are convex enough for
         * painter's order and stay flat HUD (drawn UNDER the player — keeps the
         * level-intro/RESCUED text behind Spyro). Static icons are always 3D → HUD3D;
         * a near-OT/UI billboard is HUD3D only if its model Z-extent is large. */
        int model_3d = (R->vzspread >= 96);   /* >~3× a text glyph's thickness */
        s_cur_ui = openpete_ui_moby_is_addr_static(R->moby) ? 2
                 : (openpete_ui_moby_is_addr_ui(R->moby) || R->near_ot) ? (model_3d ? 2 : 1)
                 : 0;
        proj_xy* base = sv + R->sv_off;
        int vc = R->vcount;
        /* Per-vertex gouraud lit colours from the inline normal stream
         * (model+0x10, 3 signed bytes/vertex: byte0=Nz, byte1=Nx, byte2=Ny —
         * same byte layout as the vertex positions). Computed for every moby;
         * only read for gouraud faces (flat faces light per-face below). */
        static uint32_t pcol[256];
        int pcn = vc < 256 ? vc : 256;
        for (int i = 0; i < pcn; i++) {
            uint32_t p = R->model + 0x10u + (uint32_t)i*3u;
            int nz=(int8_t)cpu->read_byte(p), nx=(int8_t)cpu->read_byte(p+1), ny=(int8_t)cpu->read_byte(p+2);
            pcol[i] = shaded_lit_color(R->RT, nx, ny, nz, R->base, s_sh_nohl?0:1);
        }
        /* METAL sub-mode base (word0 bit1): the gem/shiny tint comes from the
         * metal table D_8006E44C[scolor>>21] (8B: word0=base, word1=reflected),
         * NOT the flat gray table — see shaded_metal_color. Read once per moby. */
        uint32_t mscolor = cpu->read_word(R->moby + 0x4Cu);
        uint32_t mtab = D_METAL_COLTAB + (uint32_t)((int32_t)mscolor >> 21);
        uint32_t metal_w0 = cpu->read_word(mtab) & 0xFFFFFFu;
        uint32_t metal_w1 = cpu->read_word(mtab + 4u) & 0xFFFFFFu;
        uint32_t fp = R->faces;
        for (int f = 0; f < R->fcount && s_wtri_n < MAX_NTRI; f++, fp += 8u) {
            uint32_t w0 = cpu->read_word(fp), w1 = cpu->read_word(fp + 4u);
            if (w0 & 1u) { if (w0 & 2u) s_shmode[2]++; else s_shmode[1]++; } else s_shmode[0]++;
            int i0 = (int)((w0 >> 21) & 0x1FCu) >> 2;
            int i1 = (int)((w0 >> 14) & 0x1FCu) >> 2;
            int i2 = (int)((w0 >>  7) & 0x1FCu) >> 2;
            int i3 = (int)( w0        & 0x1FCu) >> 2;
            if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
            /* Colour: w0 bit0 selects shading MODE — flat (1) lights once from
             * w1's 3-byte face normal; gouraud (0) reads per-vertex colours
             * indexed by w1's colour fields (>>{21,14,7,0}&0x1FC, >>2 = slot). */
            uint32_t c0,c1,c2,c3; uint8_t mode;
            if (w0 & 1u) {
                int fnz=(int8_t)(w1>>24), fnx=(int8_t)(w1>>16), fny=(int8_t)(w1>>8);
                if (w0 & 2u)   /* METAL: env-reflection tint (block_80023720) */
                    c0=c1=c2=c3 = shaded_metal_color(R->RT, fnx, fny, fnz, metal_w0, metal_w1);
                else           /* flat-basic: directional light, flat table base */
                    c0=c1=c2=c3 = shaded_lit_color(R->RT, fnx, fny, fnz, R->base, 0);
                mode = (w0 & 2u) ? 2 : 1;
            } else {
                int j0=(int)((w1>>21)&0x1FCu)>>2, j1=(int)((w1>>14)&0x1FCu)>>2;
                int j2=(int)((w1>> 7)&0x1FCu)>>2, j3=(int)( w1     &0x1FCu)>>2;
                c0 = j0<pcn?pcol[j0]:0x808080u; c1 = j1<pcn?pcol[j1]:0x808080u;
                c2 = j2<pcn?pcol[j2]:0x808080u; c3 = j3<pcn?pcol[j3]:0x808080u;
                mode = 0;
            }
            int mt0 = s_wtri_n;
            proj_xy* p0 = &base[i0]; proj_xy* p1 = &base[i1]; proj_xy* p2 = &base[i2];
            double ncl1 = tri_nclip_i(p0, p1, p2);
            if (i2 == i3) {                                       /* TRI */
                if (ncl1 > 0.0)
                    emit_wtri(p0,p1,p2, c0,c1,c2, 0,0,0,0,0,0, 0,0,0,0,0,16,0);
            } else if (i3 < vc) {                                 /* QUAD */
                proj_xy* p3 = &base[i3];
                double ncl2 = tri_nclip_i(p3, p1, p2);
                if (ncl1 > 0.0 || ncl2 < 0.0) {
                    emit_wtri(p0,p1,p2, c0,c1,c2, 0,0,0,0,0,0, 0,0,0,0,0,16,0);
                    if (s_wtri_n < MAX_NTRI)
                        emit_wtri(p1,p3,p2, c1,c3,c2, 0,0,0,0,0,0, 0,0,0,0,0,16,0);
                }
            }
            for (int t = mt0; t < s_wtri_n; t++) s_wtri_mode[t] = mode;
        }
    }
    return s_wtri_n;
}

/* Ground-truth check for the gouraud per-vertex lighting: the .s writes its
 * per-vertex lit colours to scratchpad D_SONY+0x2A00 (block_80023258), one
 * word per vertex, for the LAST gouraud moby it processed (overwritten each
 * moby). Recompute pcol[] for each rec, find the one whose pcol matches the
 * scratchpad (that's the last gouraud moby), and dump element-wise — zero
 * tri-match / cross-moby confound. Call from the verify path. */
static void shaded_pcol_scratch_check(CPUState* cpu) {
    uint32_t SCR = D_SONY + 0x2A00u;
    int brec=-1; double bscore=1e18;
    for (int r=0; r<s_nshrecs; r++) {            /* find the rec the scratchpad holds */
        shaded_rec* R=&s_shrecs[r]; if (R->vcount<3) continue;
        double e=0; int nn = R->vcount<8?R->vcount:8;
        for (int i=0;i<nn;i++){
            uint32_t p=R->model+0x10u+(uint32_t)i*3u;
            int nz=(int8_t)cpu->read_byte(p), nx=(int8_t)cpu->read_byte(p+1), ny=(int8_t)cpu->read_byte(p+2);
            uint32_t mine=shaded_lit_color(R->RT,nx,ny,nz,R->base,s_sh_nohl?0:1);
            uint32_t sc=cpu->read_word(SCR+(uint32_t)i*4u)&0xFFFFFFu;
            e += abs((int)(mine&0xFF)-(int)(sc&0xFF))+abs((int)((mine>>8)&0xFF)-(int)((sc>>8)&0xFF))
               + abs((int)((mine>>16)&0xFF)-(int)((sc>>16)&0xFF));
        }
        e/=nn; if(e<bscore){bscore=e;brec=r;}
    }
    if (brec<0) return;
    shaded_rec* R=&s_shrecs[brec];
    uint32_t rot=cpu->read_word(R->moby+0x44u);
    int nn=R->vcount<64?R->vcount:64, exact=0; double err=0;
    for (int i=0;i<nn;i++){
        uint32_t p=R->model+0x10u+(uint32_t)i*3u;
        int nz=(int8_t)cpu->read_byte(p),nx=(int8_t)cpu->read_byte(p+1),ny=(int8_t)cpu->read_byte(p+2);
        uint32_t mine=shaded_lit_color(R->RT,nx,ny,nz,R->base,s_sh_nohl?0:1)&0xFFFFFFu;
        uint32_t s=cpu->read_word(SCR+(uint32_t)i*4u)&0xFFFFFFu;
        if(mine==s)exact++;
        err += abs((int)(mine&0xFF)-(int)(s&0xFF))+abs((int)((mine>>8)&0xFF)-(int)((s>>8)&0xFF))
             + abs((int)((mine>>16)&0xFF)-(int)((s>>16)&0xFF));
    }
    int nyonly=0, nx=0, nz=0;   /* euler census across all shaded mobys this frame */
    for (int r=0;r<s_nshrecs;r++){ uint32_t rr=cpu->read_word(s_shrecs[r].moby+0x44u);
        int X=(rr&0xFF00u)>>7, Z=(rr&0xFFu)<<1; if(X)nx++; if(Z)nz++; if(!X&&!Z)nyonly++; }
    fprintf(stderr,"[shaded-pcol f=%u] rec=%d cls=0x%X vcount=%d euler{Y=%u X=%u Z=%u} R->RT exact=%d/%d meanChErr=%.2f nohl=%d | census{Yonly=%d Xrot=%d Zrot=%d of %d}\n",
        openpete_frame, brec, R->cls, R->vcount,
        (rot>>15)&0x1FEu, (rot&0xFF00u)>>7, (rot&0xFFu)<<1, exact, nn, err/(3.0*nn), s_sh_nohl,
        nyonly, nx, nz, s_nshrecs);
}

static int on_80022A2C_moby_native(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = moby_verify_enabled();
    int feed   = openpete_native_render_enabled();
    if (!verify && !feed && !render_paths_active()) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_80022A2C(cpu);                      /* run the real SHADED renderer (writes m_WasDrawn) */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_MOBY, startp, endp);  /* polybuf range for the OT filter */

    if (feed) {                                 /* feed shaded mobys to the scene — fully native */
        s_cur_mat = "SHD";
        build_shaded_walk_tris(cpu);            /* native topology (#2a) + native lighting (#2b) */
        push_scene_moby();                      /* reads zero .s prim output */
        /* Flat HUD/text draws OVER the player by default; UNDER it only during the
         * "THE ADVENTURE BEGINS" reveal where Spyro flies in front of the text being
         * revealed — that runs under GS_TitleScreen (13) (the title state machine
         * drives the homeworld-entrance flyby). NOT GS_LevelTransition (1) — the
         * "ENTERING <level>" portal transition has no Spyro-reveal, so text stays on
         * top there like every other state (gameplay 0, RESCUED/Dragon 8, …).
         * (g_Gamestate@0x800757D8.) Set after push (begin_frame, which resets the
         * flag, has run); the moby renderer runs every 3D frame. */
        openpete_native_scene_set_hud_under_player(cpu->read_word(0x800757D8u) == 13u);
    }

    if (verify) {
        static int s_dumped = 0, s_sreports = 0;
        if (openpete_frame >= moby_verify_frame() && endp > startp && s_sreports < 12) {
            s_cur_mat = "SHD";
            capture_gte(cpu, startp, endp);
            if (!s_dumped) { shaded_dump(cpu, startp, endp); s_dumped = 1; }
            shaded_verify(cpu);                 /* vertex A/B (100% geometry match) */
            build_shaded_walk_tris(cpu);        /* native topology (#2a) + lighting (#2b) -> s_wtri */
            compare_walk_vs_prims();            /* triangle A/B vs the shaded prims (~90% onscreen) */
            shaded_color_ab();                  /* position-keyed colour A/B (#2b lighting) */
            shaded_pcol_scratch_check(cpu);     /* direct pcol vs .s +0x2A00 scratchpad */
            s_sreports++;
        }
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
