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
static double project(const int M[3][3], const int TR[3], int vx, int vy, int vz,
                      double ofx_px, double ofy_px, double H,
                      double* out_sx, double* out_sy, int* out_sat) {
    double mac1 = (double)TR[0] + (M[0][0]*(double)vx + M[0][1]*(double)vy + M[0][2]*(double)vz) / 4096.0;
    double mac2 = (double)TR[1] + (M[1][0]*(double)vx + M[1][1]*(double)vy + M[1][2]*(double)vz) / 4096.0;
    double mac3 = (double)TR[2] + (M[2][0]*(double)vx + M[2][1]*(double)vy + M[2][2]*(double)vz) / 4096.0;
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
            if (code == 0x34 || code == 0x3C) {
                t->gouraud = 1; t->textured = 1;
                t->quad = (code == 0x3C);
                t->nv  = t->quad ? 4 : 3;
                static const int xyw[4] = { 2, 5, 8, 11 };
                static const int uvw[4] = { 3, 6, 9, 12 };
                static const int rgbw[4]= { 1, 4, 7, 10 };
                for (int k = 0; k < t->nv; k++) {
                    uint32_t xy = t->raw[xyw[k]];
                    t->x[k] = (float)(int16_t)(xy & 0xFFFF);
                    t->y[k] = (float)(int16_t)(xy >> 16);
                    t->rgb[k] = t->raw[rgbw[k]] & 0xFFFFFF;
                    uint32_t uv = t->raw[uvw[k]];
                    t->u[k] = (int)(uv & 0xFF); t->v[k] = (int)((uv >> 8) & 0xFF);
                }
                t->clut  = (int)(t->raw[3] >> 16);
                t->tpage = (int)(t->raw[6] >> 16);
            }
            s_gte_n++;
        }
        p += stride;
    }
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
/* Decodes one record's frame-A vertex stream (literal words + interleaved
 * 5/5/5 short-delta, the bit-0 flag of each word selecting the next), float-
 * projects with the record's R/TR (mirrors F798's no-blend path block
 * 8001FCC8), and writes screen XY into out[]. Returns vertex count.
 * Static-pose (blend factor 0) only; the keyframe-INTPL blend path is a TODO.
 * The caller set-matches these against the emitted prim vertices. */
typedef struct { float sx, sy; int sat; } proj_xy;

static int moby_project_record(CPUState* cpu, uint32_t rec, proj_xy* out, int cap) {
    uint32_t header = cpu->read_word(rec + 0x04);
    uint32_t frameA = cpu->read_word(rec + 0x08);
    if (header < 0x80000000u || frameA < 0x80000000u) return 0;
    int TR[3];
    TR[0] = (int32_t)cpu->read_word(rec + 0x10);
    TR[1] = (int32_t)cpu->read_word(rec + 0x14);
    TR[2] = (int32_t)cpu->read_word(rec + 0x18);
    int M[3][3]; read_matrix(cpu, rec + 0x1C, M);
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFFu);

    uint32_t fw0 = cpu->read_word(frameA), fw1 = cpu->read_word(frameA + 4);
    uint32_t litp   = (fw0 & 0x1FFFFFu) | 0x80000000u;
    uint32_t shortp = litp + ((fw1 >> 24) & 0xFFu) * 4u;
    int s5    = (cpu->read_byte(header + 6) & 0xFF) + 1;
    int count = cpu->read_byte(header + 8) & 0xFF;   /* gte_ctrl[30] sign adj = 0 here */
    if (count > cap) count = cap;

    int VX = 0, VY = 0, VZ = 0, flag = 0;
    for (int i = 0; i < count; i++) {
        if (i == 0 || flag == 0) {
            uint32_t w = cpu->read_word(litp); litp += 4;
            VZ = (int32_t)w >> 21;
            VX = (int32_t)(w << 10) >> 21;
            VY = (int32_t)(w << 20) >> 19;
            flag = (int)(w & 1u);
        } else {
            int16_t d = (int16_t)cpu->read_half(shortp); shortp += 2;
            VZ += ((int32_t)d >> 11) << s5;                       /* sra11 */
            VX -= ((int32_t)((uint32_t)(int32_t)d << 21) >> 27) << s5;
            VY -= ((int32_t)((uint32_t)(int32_t)d << 26) >> 27) << s5;
            flag = (int)((uint16_t)d & 1u);
        }
        double sx, sy; int sat;
        project(M, TR, VX, VY, VZ, ofx, ofy, H, &sx, &sy, &sat);
        out[i].sx = (float)sx; out[i].sy = (float)sy; out[i].sat = sat;
    }
    return count;
}

#define MAX_PROJ 8192
static proj_xy s_proj[MAX_PROJ]; static int s_proj_n;

/* ---- projection A/B: native decode+project vs the emitted prim verts --
 * Project EVERY record's vertices into s_proj[], then nearest-neighbour
 * match each captured GTE prim vertex against the native set (the Phase-2
 * instrument). The prims carry the authoritative final SXY; a close match
 * proves the decode + RTPS+TR projection reproduce the GTE. */
static void compare_and_report(CPUState* cpu) {
    s_proj_n = 0;
    int nrec = 0;
    for (int r = 0; r < MAX_RECORDS; r++) {
        uint32_t rec = REC_BASE + (uint32_t)r * REC_STRIDE;
        if (cpu->read_word(rec + 0x04) == 0 && cpu->read_word(rec + 0x34) == 0) break;
        int got = moby_project_record(cpu, rec, s_proj + s_proj_n, MAX_PROJ - s_proj_n);
        s_proj_n += got; nrec++;
        if (s_proj_n >= MAX_PROJ) break;
    }
    if (!s_proj_n) { fprintf(stderr, "[moby-verify f=%u] no records\n", openpete_frame); return; }

    int gv_n = 0, near_n = 0, floor_ok = 0;
    double sum = 0, mx = 0, worst = -1;
    float wgx=0,wgy=0,wnx=0,wny=0;
    for (int g = 0; g < s_gte_n; g++) {
        prim_t* P = &s_gte[g];
        for (int k = 0; k < P->nv; k++) {
            float gx = P->x[k], gy = P->y[k];
            gv_n++;
            double best = 1e18; float bnx=0, bny=0;
            for (int i = 0; i < s_proj_n; i++) {
                if (s_proj[i].sat) continue;
                double dx = s_proj[i].sx - gx, dy = s_proj[i].sy - gy;
                double d = dx*dx + dy*dy;
                if (d < best) { best = d; bnx = s_proj[i].sx; bny = s_proj[i].sy; }
            }
            double dist = sqrt(best);
            if (dist <= 4.0) {
                near_n++; sum += dist; if (dist > mx) mx = dist;
                if (fabs(floor((double)bnx) - gx) <= 1.0 && fabs(floor((double)bny) - gy) <= 1.0)
                    floor_ok++;
            }
            if (dist > worst && dist < 1e17) { worst = dist; wgx=gx; wgy=gy; wnx=bnx; wny=bny; }
        }
    }
    fprintf(stderr,
        "[moby-verify f=%u] recs=%d nativeVerts=%d primVerts=%d | matched(<=4px)=%d/%d(%.1f%%) "
        "mean=%.3fpx max=%.3fpx floor(+-1)=%d | worst gte(%.0f,%.0f) near(%.1f,%.1f) d=%.1f | prims=%d\n",
        openpete_frame, nrec, s_proj_n, gv_n, near_n, gv_n,
        gv_n?100.0*near_n/gv_n:0.0, near_n?sum/near_n:0.0, mx, floor_ok,
        wgx,wgy,wnx,wny, worst, s_gte_n);
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

    if (verify) {
        static int s_dumped = 0, s_reports = 0;
        int target = (openpete_frame >= moby_verify_frame() && s_gte_n > 0 && s_reports < 16);
        if (target && !s_dumped) { hex_dump(cpu, start, end); s_dumped = 1; }
        if (target) { compare_and_report(cpu); s_reports++; }
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
