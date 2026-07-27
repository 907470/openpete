// games/spyro-1/features/flame_native.c — Phase 3b.1 of
// notes/plans/openpete-native-renderer.md: the SPYRO FLAME renderer
// (func_80058D64 / r_flame.s) re-derived from gamestate in float + its
// numerical A/B harness, and the first ADDITIVE-blend consumer (ABR mode 1).
//
// The flame is a self-contained ~490-line handwritten GTE routine: a
// procedurally-generated billboard ribbon strip with sine-table flicker,
// two-stage transform (flame origin -> camera space, then the flame-attach
// matrix the Spyro renderer cached this frame), additively blended. See the
// format-spike doc.
//
// STATIC ASM FINDING (corrects the doc's UNCERTAIN "untextured G3"): the strip
// cell is 0x34 bytes = 13 words = POLY_GT4 and the colour/code word is 0x3E
// (poly+gouraud+QUAD+TEXTURED+semi-transparent+modulate). So the flame is a
// TEXTURED semi-transparent gouraud quad ribbon, additively blended. This
// harness captures live prims to confirm the exact field layout before the
// native math commits.
//
// Determinism: never writes PSX RAM. Calls the real renderer exactly once
// (behaviour-identical to normal dispatch) and only READS state afterwards.
// Gated by OPENPETE_FLAME_VERIFY (dump/A/B) or a native renderer mode
// (--renderer vk|ab|diff; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "psx_runtime.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "openpete_native_render.h"
#include "openpete_log.h"  /* OPL_ERROR_F — ABR/blend FIXME runtime alarms */
#include "render_paths.h"  /* --render-paths isolation */
#include "spyro_native_reextract.h"  /* sub-tick re-extract entry */
#include "native_gte.h"    /* shared GTE float-mirror math (W2.3) */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"

extern void     func_80058D64(CPUState* cpu);  /* the real FLAME renderer */
extern unsigned openpete_frame;

/* Sub-keyframe slide (0..1): the fractional part of the per-strand counter the
 * spine is advancing this present. The counter selects the ribbon's window into
 * the STATIC cross-section spine (one 8-byte record per counter step); this frac
 * slides each cross-section between record[p] and record[p+8] so the ribbon
 * animates BELOW the keyframe grid (matching Spyro's keyframe-lerp smoothness)
 * instead of stepping one record at a time. Set by the spine orchestrator before
 * each sub-tick re-extract; 0 on the canonical render (exact keyframes). */
static double s_flame_subframe = 0.0;
void flame_native_set_subframe(double frac) {
    s_flame_subframe = (frac < 0.0) ? 0.0 : (frac > 1.0 ? 1.0 : frac);
}

/* ---- gamestate addresses (psx.map) --------------------------------------- */

/* flame struct offsets (from the format notes) */
#define FL_POS        0x00u         /* Vector3D m_Position (origin)            */
#define FL_ACTIVE     0x98u         /* m_IsFlameActive                         */
#define FL_SUPER      0x9Cu         /* super-flame flag (s2)                   */
#define FL_ATTACH     0xB8u         /* flame-attach SHORTMATRIX (5 words)      */
#define FL_CNT        0x20u         /* per-strand vertex count [i] (lb)        */
#define FL_LEN        0x28u         /* per-strand length [i] (lb)              */

/* ---- captured prims (ground truth) --------------------------------------- */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (word[1]>>24) */
    uint8_t  len;           /* OT tag length (word[0]>>24)      */
    int      nv;            /* 3 or 4                           */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad;
    uint32_t raw[20];
    int      nraw;
} prim_t;

#define MAX_PRIMS 4096
static prim_t s_gte[MAX_PRIMS]; static int s_gte_n;
static int s_code_hist[256];

/* Walk the polybuf [start,end) by OT tag length (stride=((w0>>24)+1)*4); decode
 * each prim by its code byte (word[1]>>24) with the general PSX polygon decode
 * (bit4 gouraud / bit3 quad / bit2 textured). Same scheme moby/Spyro use. */
static void capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
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

/* ---- one-shot input + prim dump ------------------------------------------ */
static void flame_dump(CPUState* cpu, uint32_t start, uint32_t end) {
    uint32_t fb = G_SPYROFLAME;
    fprintf(stderr, "\n=== [flame-dump f=%u] polybuf [%08X,%08X) %u bytes ===\n",
            openpete_frame, start, end, end - start);
    fprintf(stderr, "  m_Position = (%d, %d, %d)\n",
            (int)cpu->read_word(fb+FL_POS), (int)cpu->read_word(fb+FL_POS+4),
            (int)cpu->read_word(fb+FL_POS+8));
    fprintf(stderr, "  m_IsFlameActive=%d  super(+0x9C)=%d\n",
            (int)cpu->read_word(fb+FL_ACTIVE), (int)cpu->read_word(fb+FL_SUPER));
    fprintf(stderr, "  per-strand cnt[0..7] =");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %d", (int8_t)cpu->read_byte(fb+FL_CNT+(uint32_t)i));
    fprintf(stderr, "\n  per-strand len[0..7] =");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %d", (int8_t)cpu->read_byte(fb+FL_LEN+(uint32_t)i));
    fprintf(stderr, "\n  attach matrix (+0xB8, 5 words -> 9 int16):");
    for (int i = 0; i < 5; i++) fprintf(stderr, " %08X", cpu->read_word(fb+FL_ATTACH+(uint32_t)i*4));
    fprintf(stderr, "\n  camera projMat (+0x00, 5 words):");
    for (int i = 0; i < 5; i++) fprintf(stderr, " %08X", cpu->read_word(G_CAMERA+(uint32_t)i*4));
    fprintf(stderr, "\n  camera pos (+0x28) = (%d, %d, %d)\n",
            (int)cpu->read_word(G_CAMERA+0x28), (int)cpu->read_word(G_CAMERA+0x2C),
            (int)cpu->read_word(G_CAMERA+0x30));

    fprintf(stderr, "  rodata D_8006DAA8 geom[0..3] (16B records):\n");
    for (int r = 0; r < 4; r++)
        fprintf(stderr, "    +%02X: %08X %08X %08X %08X\n", r*16,
                cpu->read_word(D_FLAME_GEOM+(uint32_t)r*16+0), cpu->read_word(D_FLAME_GEOM+(uint32_t)r*16+4),
                cpu->read_word(D_FLAME_GEOM+(uint32_t)r*16+8), cpu->read_word(D_FLAME_GEOM+(uint32_t)r*16+12));
    fprintf(stderr, "  rodata D_8006E1A8 uv-table[0..7]:");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", cpu->read_word(D_FLAME_UV+(uint32_t)i*4));
    fprintf(stderr, "\n  rodata D_8006D94C conn[0..7]:");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", cpu->read_word(D_FLAME_CONN+(uint32_t)i*4));
    fprintf(stderr, "\n  flame struct +0x08..0x1C (strip colour/UV base):");
    for (int o = 0x08; o <= 0x1C; o += 4) fprintf(stderr, " +%02X=%08X", o, cpu->read_word(fb+(uint32_t)o));
    fprintf(stderr, "\n");

    /* per active-strand geometry block (s6 = D_8006DAA8 + 0xC0*strand for
     * strand<4, else +0x100*(strand-4)+0x300). Dump the first 16 8-byte verts. */
    for (int strand = 0; strand < 8; strand++) {
        int cnt = (int8_t)cpu->read_byte(fb+FL_CNT+(uint32_t)strand);
        if (cnt <= 0) continue;
        int len = (int8_t)cpu->read_byte(fb+FL_LEN+(uint32_t)strand);
        uint32_t conn = cpu->read_word(D_FLAME_CONN+(uint32_t)(cnt-1)*4);
        uint32_t off = (strand < 4) ? (uint32_t)0xC0*(uint32_t)strand
                                    : (uint32_t)0x100*(uint32_t)(strand-4)+0x300;
        uint32_t s6 = D_FLAME_GEOM + off;
        uint32_t s7off = ((conn >> 24) << 3) & 0x7F8u;
        fprintf(stderr, "  strand %d: cnt=%d len=%d conn=%08X off=%X s6=%08X s7=s6+%X verts(8B Z/X/Y):\n",
                strand, cnt, len, conn, off, s6, s7off);
        for (int r = 0; r < 16; r++) {
            uint32_t w0 = cpu->read_word(s6+(uint32_t)r*8), w1 = cpu->read_word(s6+(uint32_t)r*8+4);
            int Z = (int16_t)(w0 & 0xFFFF), X = -(int)(int16_t)(w0 >> 16), Y = -(int)(int16_t)(w1 & 0xFFFF);
            fprintf(stderr, "    [%2d] %08X %08X  Z=%d X=%d Y=%d scale=%04X\n", r, w0, w1, Z, X, Y, w1 >> 16);
        }
    }

    /* code histogram */
    fprintf(stderr, "  prim code histogram:");
    for (int c = 0; c < 256; c++) if (s_code_hist[c]) fprintf(stderr, " %02X:%d", c, s_code_hist[c]);
    fprintf(stderr, "  (total prims=%d)\n", s_gte_n);

    /* first few prims, raw + decoded */
    int n = s_gte_n < 6 ? s_gte_n : 6;
    for (int p = 0; p < n; p++) {
        prim_t* t = &s_gte[p];
        fprintf(stderr, "  prim[%d] code=%02X len=%d nv=%d tex=%d gour=%d quad=%d  raw:",
                p, t->code, t->len, t->nv, t->textured, t->gouraud, t->quad);
        for (int i = 0; i < t->nraw; i++) fprintf(stderr, " %08X", t->raw[i]);
        fprintf(stderr, "\n      decoded:");
        for (int k = 0; k < t->nv; k++)
            fprintf(stderr, " v%d(xy=%.0f,%.0f rgb=%06X uv=%d,%d)",
                    k, t->x[k], t->y[k], t->rgb[k], t->u[k], t->v[k]);
        if (t->textured) fprintf(stderr, "  clut=%04X tpage=%04X", t->clut, t->tpage);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "=== end flame-dump ===\n\n");
}

/* compact per-frame line for the first N active-flame frames */
static void flame_report(CPUState* cpu) {
    uint32_t fb = G_SPYROFLAME;
    int textured = 0, quad = 0, tri = 0;
    for (int p = 0; p < s_gte_n; p++) {
        if (s_gte[p].textured) textured++;
        if (s_gte[p].nv == 4) quad++; else if (s_gte[p].nv == 3) tri++;
    }
    fprintf(stderr, "[flame f=%u] prims=%d (quad=%d tri=%d tex=%d) super=%d pos=(%d,%d,%d)\n",
            openpete_frame, s_gte_n, quad, tri, textured, (int)cpu->read_word(fb+FL_SUPER),
            (int)cpu->read_word(fb+FL_POS), (int)cpu->read_word(fb+FL_POS+4), (int)cpu->read_word(fb+FL_POS+8));
}

/* ---- flame origin -> camera space (the two-stage transform, step 1) -------
 * r_flame.s lines 37–70: the flame origin's camera-space position is a swizzled
 * MVMVA (sf=1, mx=0=RT, vv=3=IR, tv=3=none) of (flame - cam) through the camera
 * rotation matrix g_Camera+0x00..0x10. The IR vector is swizzled exactly as
 * Spyro's TR_body: IR1=cam.y-flame.y, IR2=cam.z-flame.z, IR3=flame.x-cam.x (each
 * truncated to int16 by mtc2). TR[i] = (R · IR) >> 12. The result becomes the
 * GTE TR for every flame vertex (R is then replaced by the attach matrix). For
 * the scaffold we only need TR[2] = the camera-space depth. */
static int s16(int v) { return (int)(int16_t)(uint32_t)v; }

static void flame_origin_tr(CPUState* cpu, double tr[3]) {
    int fx = (int)cpu->read_word(G_SPYROFLAME + FL_POS + 0);
    int fy = (int)cpu->read_word(G_SPYROFLAME + FL_POS + 4);
    int fz = (int)cpu->read_word(G_SPYROFLAME + FL_POS + 8);
    int cx = (int)cpu->read_word(G_CAMERA + 0x28);
    int cy = (int)cpu->read_word(G_CAMERA + 0x2C);
    int cz = (int)cpu->read_word(G_CAMERA + 0x30);
    int ir1 = s16(cy - fy);   /* C2_IR1 */
    int ir2 = s16(cz - fz);   /* C2_IR2 */
    int ir3 = s16(fx - cx);   /* C2_IR3 */
    int R[3][3];
    ngte_read_matrix(cpu, G_CAMERA + 0x00, R);
    /* Fractional matrix-scale (÷4096.0, not >>12) so the ribbon's camera-space
     * origin tracks the smooth sub-tick camera instead of stepping ~1 view-unit
     * per present (same fix as the moby/terrain deltas; the s16 IR delta above is
     * the faithful GTE int16 load and the dominant loss was this final truncation). */
    for (int i = 0; i < 3; i++)
        tr[i] = (R[i][0]*ir1 + R[i][1]*ir2 + R[i][2]*ir3) / 4096.0;
}

/* ---- native gamestate walk (3b.1: float geometry for HD + frame-gen) ------
 * Re-derive the flame ribbon from gamestate in float — the scaffold's int16
 * PSX-resolution prims can give neither sub-pixel/HD geometry nor sub-tick
 * interpolation, so this reproduces the procedural generation (spine curve +
 * sine flicker + ribbon assembly) and projects in float. Validated SXY-shape
 * against the captured prims (the .s int16 output as the oracle), then it
 * replaces the scaffold. Stage 1 = the STRIP ribbon (GT4 textured additive);
 * the first-cell caps (G3) follow. */
static int rd_s16(CPUState* cpu, uint32_t addr) {
    uint32_t w = cpu->read_word(addr & ~3u);
    return (int)(int16_t)((addr & 2u) ? (w >> 16) : (w & 0xFFFF));
}

/* load a PSYQ SHORTMATRIX (5 words -> 9 int16, 1.0=4096) at `base` into R[3][3] */
static void flame_load_matrix(CPUState* cpu, uint32_t base, int R[3][3]) {
    ngte_read_matrix(cpu, base, R);
}

/* RTPS one local vertex through R + TR (shift 12) -> screen (float) + view-Z */
static void flame_project(const int R[3][3], const double TR[3], double ofx, double ofy, double H,
                          double vx, double vy, double vz, float* sx, float* sy, float* sz) {
    ngte_proj o;   /* shared GTE-faithful pipeline, ±1024 SXY bound ON (W2.3) */
    ngte_project_gte(R, TR, vx, vy, vz, ofx, ofy, H, 1, &o);
    *sx = (float)o.sx; *sy = (float)o.sy; *sz = (float)o.m3;
}

/* Read one spine cross-section record (Xraw/Z/idx/sIdx), sub-keyframe-lerping
 * toward the NEXT record (p+8, the tip direction) by s_flame_subframe so the
 * ribbon slides smoothly between the static keyframes. The counter advance moves
 * the window by one 8-byte record per step, so record[p+8] is exactly where the
 * next integer counter would read this cross-section → frac=1 converges on it.
 * Guarded: skip the lerp at a Z discontinuity (Z went backward / jumped — the
 * read crossed into the next strand's spine at max extension). */
static void flame_read_cross(CPUState* cpu, uint32_t p,
                             int* Xraw, int* Z, int* idx, int* sIdx) {
    uint32_t w0 = cpu->read_word(p), w1 = cpu->read_word(p + 4);
    int X0 = s16(w0 >> 16), Z0 = s16(w0 & 0xFFFF), i0 = s16(w1 & 0xFFFF);
    int s0 = (int)((w1 >> 15) & 0x1FEu);
    double f = s_flame_subframe;
    if (f > 0.0) {
        uint32_t w0b = cpu->read_word(p + 8), w1b = cpu->read_word(p + 12);
        int X1 = s16(w0b >> 16), Z1 = s16(w0b & 0xFFFF), i1 = s16(w1b & 0xFFFF);
        int s1 = (int)((w1b >> 15) & 0x1FEu);
        int dZ = Z1 - Z0;
        if (dZ >= 0 && dZ < 200) {          /* continuous spine — lerp */
            X0 += (int)((X1 - X0) * f + (X1 >= X0 ? 0.5 : -0.5));
            Z0 += (int)(dZ * f + 0.5);
            i0 += (int)((i1 - i0) * f + (i1 >= i0 ? 0.5 : -0.5));
            int ds = s1 - s0; if (ds > 256) ds -= 512; else if (ds < -256) ds += 512;
            s0 = (s0 + (int)(ds * f + (ds >= 0 ? 0.5 : -0.5))) & 0x1FE;
        }   /* else: discontinuity (crossed into next strand) — keep record[p] */
    }
    *Xraw = X0; *Z = Z0; *idx = i0; *sIdx = s0;
}

/* one ribbon cross-section: spine vert at `p` -> two edge verts (float screen) */
typedef struct { float sx, sy, sz; } fpt;
static void flame_cross_section(CPUState* cpu, uint32_t p, int amp,
                                const int R[3][3], const double TR[3],
                                double ofx, double ofy, double H, fpt* e0, fpt* e1) {
    int Xraw, Z, idx, sIdx;
    flame_read_cross(cpu, p, &Xraw, &Z, &idx, &sIdx);
    int cosv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)sIdx + 0x80);
    int sinv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)sIdx);
    int co = (amp * cosv) >> 11;
    int si = (amp * sinv) >> 11;
    /* VXY0 = (co - Xraw, si - idx); VXY1 = (-co - Xraw, -si - idx); both at Z */
    flame_project(R, TR, ofx, ofy, H, (double)(co - Xraw), (double)(si - idx), (double)Z,
                  &e0->sx, &e0->sy, &e0->sz);
    flame_project(R, TR, ofx, ofy, H, (double)(-co - Xraw), (double)(-si - idx), (double)Z,
                  &e1->sx, &e1->sy, &e1->sz);
}

/* native ribbon quads (for A/B vs the captured GT4 prims + the scene feed) */
typedef struct {
    float sx[4], sy[4], sz[4];
    int   u[4], v[4];
    uint32_t rgb[4];
    int   tpx, tpy, clx, cly, bpp;
} fquad;
#define MAX_FQUAD 1024
static fquad s_fquad[MAX_FQUAD]; static int s_fquad_n;

/* native cap tris (G3 untextured opaque) — the first-cell flame base */
typedef struct { float sx[3], sy[3], sz[3]; uint32_t rgb[3]; } fcap;
#define MAX_FCAP 1024
static fcap s_fcap[MAX_FCAP]; static int s_fcap_n;

/* signed 2*area (== GTE NCLIP MAC0 sign); the .s emits a cap iff MAC0 > 0.
 * The GTE NCLIPs the *int16* SXY, so round to int before the sign test — else
 * near-degenerate caps cull differently from the .s (off-by-one cap count). */
static double cap_nclip(const fpt* a, const fpt* b, const fpt* c) {
    double ax = lrintf(a->sx), ay = lrintf(a->sy);
    double bx = lrintf(b->sx), by = lrintf(b->sy);
    double cx = lrintf(c->sx), cy = lrintf(c->sy);
    return ax*(by - cy) + bx*(cy - ay) + cx*(ay - by);
}
static void emit_cap(const fpt* a, const fpt* b, const fpt* c,
                     uint32_t ca, uint32_t cb, uint32_t cc) {
    if (cap_nclip(a, b, c) <= 0.0) return;          /* blez MAC0 -> drop */
    if (s_fcap_n >= MAX_FCAP) return;
    fcap* t = &s_fcap[s_fcap_n++];
    t->sx[0]=a->sx; t->sy[0]=a->sy; t->sz[0]=a->sz; t->rgb[0]=ca & 0xFFFFFFu;
    t->sx[1]=b->sx; t->sy[1]=b->sy; t->sz[1]=b->sz; t->rgb[1]=cb & 0xFFFFFFu;
    t->sx[2]=c->sx; t->sy[2]=c->sy; t->sz[2]=c->sz; t->rgb[2]=cc & 0xFFFFFFu;
}

/* first cell (.s block_80058F2C..8005920C): the flame-base caps. RTPS the spine
 * centre at s7 (no flicker) -> t5; RTPT 3 flicker verts from the vert at s7-16
 * -> e0/e1/e2; 4 NCLIP-gated G3 emits combine them with the D_8006E1A8 colours.
 * Returns 1 if the strand is near-culled (SZ3>>7 - 2 < 0 -> .s skips the whole
 * strand: no caps AND no strip). */
static int flame_first_cell(CPUState* cpu, uint32_t s7, int super,
                            const int R[3][3], const double TR[3],
                            double ofx, double ofy, double H) {
    uint32_t w0c = cpu->read_word(s7), w1c = cpu->read_word(s7 + 4);
    int Xc = s16(w0c >> 16), Zc = s16(w0c & 0xFFFF), idxc = s16(w1c & 0xFFFF);
    fpt t5;  /* spine centre (no flicker) */
    flame_project(R, TR, ofx, ofy, H, (double)(-Xc), (double)(-idxc), (double)Zc,
                  &t5.sx, &t5.sy, &t5.sz);
    int sz = (int)t5.sz; if (sz < 0) sz = 0; else if (sz > 0xFFFF) sz = 0xFFFF;
    if (((sz >> 7) - 2) < 0) return 1;              /* near-cull -> skip strand */

    uint32_t w0 = cpu->read_word(s7 - 16), w1 = cpu->read_word(s7 - 12);
    int Xraw2 = s16(w0 >> 16), Z2 = s16(w0 & 0xFFFF), idx2 = s16(w1 & 0xFFFF);
    int sIdx2 = (int)((w1 >> 15) & 0x1FEu);
    int amp = super ? 0x40 : 0x2C;
    int co = (amp * rd_s16(cpu, D_SIN_TABLE + (uint32_t)sIdx2 + 0x80)) >> 11;
    int si = (amp * rd_s16(cpu, D_SIN_TABLE + (uint32_t)sIdx2)) >> 11;
    int t0 = -co, t1 = si;
    if (super) { t0 >>= 1; t1 >>= 1; }
    fpt e0, e1, e2;
    flame_project(R, TR, ofx, ofy, H, (double)(t1 - Xraw2), (double)(t0 - idx2), (double)Z2,
                  &e0.sx, &e0.sy, &e0.sz);                       /* VXY0 */
    flame_project(R, TR, ofx, ofy, H, (double)(co - Xraw2), (double)(si - idx2), (double)Z2,
                  &e1.sx, &e1.sy, &e1.sz);                       /* VXY1 */
    flame_project(R, TR, ofx, ofy, H, (double)(-co - Xraw2), (double)(-si - idx2), (double)Z2,
                  &e2.sx, &e2.sy, &e2.sz);                       /* VXY2 */

    uint32_t cb = D_FLAME_UV + (super ? 16u : 0u);
    uint32_t c0 = cpu->read_word(cb), c1 = cpu->read_word(cb + 4);
    uint32_t c2 = cpu->read_word(cb + 8), c3 = cpu->read_word(cb + 12);
    emit_cap(&t5, &e1, &e0, c0, c2, c1);             /* A: (t5,t7,t6) */
    emit_cap(&t5, &e0, &e2, c0, c1, c3);             /* B: (t5,t6,s0) */
    emit_cap(&t5, &e2, &e1, c0, c3, c2);             /* C: (t5,s0,t7) */
    emit_cap(&e0, &e1, &e2, c1, c2, c3);             /* D: (t6,t7,s0) */
    return 0;
}

static void flame_build_strip(CPUState* cpu) {
    s_fquad_n = 0;
    s_fcap_n = 0;
    uint32_t fb = G_SPYROFLAME;
    int super = (int)cpu->read_word(fb + FL_SUPER) != 0;
    int R[3][3]; double TR[3];
    flame_load_matrix(cpu, fb + FL_ATTACH, R);
    flame_origin_tr(cpu, TR);
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFF);

    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("OPENPETE_FLAME_DEBUG"); dbg = (e && *e && *e != '0'); }
    static int dbg_n = 0;
    int do_dbg = dbg && dbg_n < 3;

    for (int strand = 7; strand >= 0; strand--) {   /* .s processes t9 = 7..0 */
        int cnt = (int8_t)cpu->read_byte(fb + FL_CNT + (uint32_t)strand);
        if (cnt <= 0) continue;
        int len = (int8_t)cpu->read_byte(fb + FL_LEN + (uint32_t)strand);
        uint32_t connPtr = D_FLAME_CONN + (uint32_t)(cnt - 1) * 4;
        uint32_t stride;
        if (strand >= 4) { connPtr += 0x80; stride = 0x100; } else { stride = 0xC0; }
        uint32_t conn = cpu->read_word(connPtr);
        uint32_t geomOff = (strand < 4) ? stride * (uint32_t)strand
                                        : 0x100u * (uint32_t)(strand - 4) + 0x300u;
        uint32_t s6 = D_FLAME_GEOM + geomOff;
        uint32_t s7 = s6 + (((conn >> 24) << 3) & 0x7F8u);
        uint32_t s5b = s6 + (uint32_t)len * 8u;
        uint32_t v1b = s6 + stride;
        /* First-cell bounds (line 125-128): these gate the CAPS only — on
         * failure the .s sets gp=0 and jumps to .L8005920C, which still runs
         * the strip (just with the amplitude forced to 8). So never skip the
         * whole strand here; only the caps. (Skipping the strand dropped the
         * flame's retract phase, where len shrinks and s7 > s5b.) */
        int gp = (!((int32_t)(s7 - s5b) > 0) && !((int32_t)(v1b - s7) <= 0)) ? 1 : 0;

        if (gp) {                                    /* first cell: the base caps */
            if (flame_first_cell(cpu, s7, super, R, TR, ofx, ofy, H))
                continue;                            /* near-cull -> skip strand */
        }
        s7 -= 0x10;                                  /* .L8005920C */
        int byte1 = (int)((conn >> 8) & 0xFF);
        int byte2 = (int)((conn >> 16) & 0xFF);
        if ((int32_t)(s7 - s5b) > 0) {               /* clamp to s5b */
            int d = (int)((s7 - s5b) >> 3);
            byte1 += d; byte2 -= d; s7 = s5b;
        }
        int strand_q0 = s_fquad_n, byte2_init = byte2;
        int grey = 0x80 - 5 * byte1; if (grey < 0) grey = 0;
        uint32_t color = ((uint32_t)grey) | ((uint32_t)grey << 8) | ((uint32_t)grey << 16);
        uint32_t base = super ? fb + 8 : fb;
        uint32_t uv0w = cpu->read_word(base + 0x10) + ((uint32_t)byte1 << 8);
        uint32_t uv1w = cpu->read_word(base + 0x14) + ((uint32_t)byte1 << 8);
        int tpx = ((int)((uv1w >> 16) & 0xF)) * 64, tpy = ((int)((uv1w >> 20) & 1)) * 256;
        int mode = (int)((uv1w >> 23) & 3), bpp = (mode == 0) ? 4 : (mode == 1) ? 8 : 16;
        /* ABR-FIXME (audit 2026-06-30): the flame-strip draw hardcodes blend=2 (ABR1
         * additive); the strip tpage's ABR is data-driven (assumed additive, not
         * statically proven). If a flame ever ships a non-additive tpage this fires —
         * derive blend from the captured ABR instead of hardcoding. FIX flame_native.c:654 */
        if (((uv1w >> 21) & 3) != 1)
            OPL_ERROR_F("native", "ABR-FIXME flame strip tpage ABR=%d (feeder hardcodes "
                        "blend=2 additive) f=%u -- FIX flame_native.c:654",
                        (int)((uv1w >> 21) & 3), openpete_frame);
        int clx = ((int)((uv0w >> 16) & 0x3F)) * 16, cly = (int)((uv0w >> 22) & 0x1FF);
        uint32_t s4 = 0xA00u, s5step = 0x0A0A0Au;

        fpt prev0, prev1; int have_prev = 0;
        uint32_t puv0 = uv0w, puv1 = uv1w, pcol = color;
        /* `gp` is NOT a constant gate: line 386 `add gp,gp,at` accumulates the
         * previous cross-section's packed SXY0 each step (at=0 only on the first
         * step, line 361). So gp==0 (amp forced to 8) holds only for the FIRST
         * cross-section when the caps-bounds failed; after one step gp is the
         * SXY sum (non-zero) and amp reverts to 0x2C. Reproduce the accumulator
         * exactly or the retract phase (gp=0 strands) renders too narrow. */
        int32_t gp_acc = gp;
        int32_t prev_sxy0 = 0;          /* `at` on loop entry (.s line 361) */
        for (;;) {
            gp_acc += prev_sxy0;        /* add gp, gp, at */
            int amp = (byte2 <= 0 || gp_acc == 0) ? 8 : (super ? 0x40 : 0x2C);
            fpt c0, c1;
            flame_cross_section(cpu, s7, amp, R, TR, ofx, ofy, H, &c0, &c1);
            s7 -= 0x10;
            /* pack edge0's SXY (the GTE SXY0 the .s reads into `at`) for next +gp */
            prev_sxy0 = (int32_t)(((uint32_t)(uint16_t)(int16_t)lrintf(c0.sx))
                                  | ((uint32_t)(int16_t)lrintf(c0.sy) << 16));
            if (have_prev && s_fquad_n < MAX_FQUAD) {
                fquad* q = &s_fquad[s_fquad_n++];
                /* GT4 order: v0=prevEdge0 v1=prevEdge1 v2=curEdge0 v3=curEdge1 */
                fpt vv[4] = { prev0, prev1, c0, c1 };
                uint32_t cuv0 = puv0 + s4, cuv1 = puv1 + s4, ncol = pcol - s5step;
                uint32_t uvw[4] = { puv0, puv1, cuv0, cuv1 };
                uint32_t cols[4] = { pcol, pcol, ncol, ncol };
                for (int k = 0; k < 4; k++) {
                    q->sx[k] = vv[k].sx; q->sy[k] = vv[k].sy; q->sz[k] = vv[k].sz;
                    q->u[k] = (int)(uvw[k] & 0xFF); q->v[k] = (int)((uvw[k] >> 8) & 0xFF);
                    q->rgb[k] = cols[k] & 0xFFFFFFu;
                }
                q->tpx = tpx; q->tpy = tpy; q->clx = clx; q->cly = cly; q->bpp = bpp;
                /* advance the ramp (carry into next quad) */
                puv0 += s4; puv1 += s4; pcol -= s5step;
            }
            prev0 = c0; prev1 = c1; have_prev = 1;
            /* counter (.L80059298): the bgtz AND the blez both carry an
             * `addi t8,-1` in their branch-delay slot, and a delay slot runs
             * whether or not its branch is taken — so EACH counter pass drops
             * byte2 by 2, not 1. (Getting this wrong doubled the quad count.)
             * The s7+=8 / s4,s5 final-step swap happens only on the fall-through
             * (the second test failed). */
            if (byte2 <= 0) break;        /* blez t8 (old value) */
            byte2 -= 1;                   /* delay slot of blez */
            int last = byte2;             /* value the bgtz tests */
            byte2 -= 1;                   /* delay slot of bgtz (always runs) */
            if (last <= 0) { s7 += 8; s4 = 0x500u; s5step = 0x050505u; }
        }
        if (do_dbg)
            fprintf(stderr, "    [flame-dbg] strand %d cnt=%d conn=%08X byte1=%d byte2_init=%d quads=%d\n",
                    strand, cnt, conn, byte1, byte2_init, s_fquad_n - strand_q0);
    }
    if (do_dbg) { fprintf(stderr, "    [flame-dbg] total native quads=%d (frame %u)\n", s_fquad_n, openpete_frame); dbg_n++; }
}

/* ordered A/B: native ribbon quads vs captured GT4 prims (the .s int16 SXY as
 * the shape oracle). The walk visits strands+steps in .s order and the GT4
 * vertex order matches, so compare element-wise; report max |dSXY| per vertex
 * (sub-pixel = HD-faithful; the .s is integer-quantized so ~1px is expected). */
static void flame_strip_compare(CPUState* cpu) {
    flame_build_strip(cpu);
    int gt4 = 0; for (int p = 0; p < s_gte_n; p++) if (s_gte[p].textured && s_gte[p].nv == 4) gt4++;
    int n = s_fquad_n < gt4 ? s_fquad_n : gt4;
    double worst = 0, sum = 0; int worst_q = -1, over4 = 0, nv = 0;
    int qi = 0;
    for (int p = 0; p < s_gte_n && qi < n; p++) {
        prim_t* pr = &s_gte[p];
        if (!(pr->textured && pr->nv == 4)) continue;
        fquad* q = &s_fquad[qi];
        double qworst = 0;
        for (int k = 0; k < 4; k++) {
            double dx = q->sx[k] - pr->x[k], dy = q->sy[k] - pr->y[k];
            double d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            sum += d; nv++;
            if (d > qworst) qworst = d;
        }
        if (qworst > worst) { worst = qworst; worst_q = qi; }
        if (qworst > 4.0) over4++;
        qi++;
    }
    fprintf(stderr, "[flame-strip f=%u] quads native=%d prims(GT4)=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx @q%d over4px=%d/%d\n",
            openpete_frame, s_fquad_n, gt4, n, nv ? sum/nv : 0, worst, worst_q, over4, n);

    /* caps A/B: native G3 caps vs the captured untextured G3 prims (code 0x30) */
    int g3 = 0; for (int p = 0; p < s_gte_n; p++) if (!s_gte[p].textured && s_gte[p].nv == 3) g3++;
    int cn = s_fcap_n < g3 ? s_fcap_n : g3;
    double cworst = 0, csum = 0; int cover4 = 0, cnv = 0, ci = 0;
    for (int p = 0; p < s_gte_n && ci < cn; p++) {
        prim_t* pr = &s_gte[p];
        if (!(!pr->textured && pr->nv == 3)) continue;
        fcap* t = &s_fcap[ci];
        double tw = 0;
        for (int k = 0; k < 3; k++) {
            double dx = t->sx[k] - pr->x[k], dy = t->sy[k] - pr->y[k];
            double d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            csum += d; cnv++; if (d > tw) tw = d;
        }
        if (tw > cworst) cworst = tw;
        if (tw > 4.0) cover4++;
        ci++;
    }
    fprintf(stderr, "[flame-cap   f=%u] caps native=%d prims(G3)=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx over4px=%d/%d\n",
            openpete_frame, s_fcap_n, g3, cn, cnv ? csum/cnv : 0, cworst, cover4, cn);
}

/* ---- scaffold feed (3b.1 stage: feed the .s prims with additive blend) -----
 * Decode the captured .s flame prims and push them into the native scene with
 * the correct PSX semantics: the GT4 strip (code 0x3E) is textured + ADDITIVE
 * (blend=2, ABR mode 1); the G3 caps (code 0x30) are untextured opaque gouraud.
 * Geometry (SXY) comes straight from the .s prims — byte-exact — so this proves
 * the additive pipeline end-to-end with a real consumer before the native
 * geometry walk replaces the prim coupling. Depth is the flame origin's
 * camera-space Z (a single representative depth for the small billboard; the
 * native walk gives per-vertex Z). */
static void flame_feed_scaffold(CPUState* cpu) {
    if (s_gte_n <= 0) return;
    double tr[3]; flame_origin_tr(cpu, tr);
    float vz = (float)tr[2];
    float depth = openpete_native_depth_from_viewz(openpete_worldvz_full(vz));
    openpete_native_scene_begin_frame(openpete_frame);   /* append after world + actors */
    for (int p = 0; p < s_gte_n; p++) {
        prim_t* t = &s_gte[p];
        if (t->nv < 3) continue;
        int tpx = (t->tpage & 0xF) * 64, tpy = ((t->tpage >> 4) & 1) * 256;
        int mode = (t->tpage >> 7) & 3, bpp = (mode == 0) ? 4 : (mode == 1) ? 8 : 16;
        int clx = (t->clut & 0x3F) * 16, cly = (t->clut >> 6) & 0x1FF;
        int blend = t->textured ? 2 : 0;   /* GT4 strip = additive; G3 cap = opaque */
        int splits = (t->nv == 4) ? 2 : 1;
        for (int s = 0; s < splits; s++) {
            int a, b, c;
            if (s == 0) { a = 0; b = 1; c = 2; } else { a = 1; b = 3; c = 2; }  /* GT4 split */
            int idx[3] = { a, b, c };
            openpete_scene_tri st = {0};
            for (int k = 0; k < 3; k++) {
                int j = idx[k];
                st.v[k].sx = t->x[j]; st.v[k].sy = t->y[j]; st.v[k].z = depth;
                st.v[k].u = (float)t->u[j]; st.v[k].v = (float)t->v[j];
                st.v[k].rgba = (t->rgb[j] & 0xFFFFFFu) | 0xFF000000u;
            }
            st.textured = t->textured; st.tpage_x = tpx; st.tpage_y = tpy;
            st.clut_x = clx; st.clut_y = cly; st.bpp = bpp; st.blend = blend;
            openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
        }
    }
}

/* ---- native walk feed (3b.1 endpoint: float geometry from gamestate) -------
 * Feed the native ribbon (strip quads, textured additive) + base caps (G3
 * untextured opaque) re-derived from gamestate — no .s prim dependency, so it
 * runs at the sub-tick gamestate the host re-renders. Per-vertex view-Z gives
 * correct depth (vs the scaffold's single billboard depth). */
static void flame_feed_native(CPUState* cpu) {
    flame_build_strip(cpu);                  /* fills s_fquad (strip) + s_fcap (caps) */
    if (s_fquad_n <= 0 && s_fcap_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);

    for (int i = 0; i < s_fquad_n; i++) {
        fquad* q = &s_fquad[i];
        /* GT4 -> two tris: (v0,v1,v2) + (v1,v3,v2) */
        int split[2][3] = { {0,1,2}, {1,3,2} };
        for (int s = 0; s < 2; s++) {
            openpete_scene_tri st = {0};
            for (int k = 0; k < 3; k++) {
                int j = split[s][k];
                st.v[k].sx = q->sx[j]; st.v[k].sy = q->sy[j];
                st.v[k].z  = openpete_native_depth_from_viewz(openpete_worldvz_full(q->sz[j]));
                st.v[k].u  = (float)q->u[j]; st.v[k].v = (float)q->v[j];
                st.v[k].rgba = (q->rgb[j] & 0xFFFFFFu) | 0xFF000000u;
            }
            st.textured = 1; st.tpage_x = q->tpx; st.tpage_y = q->tpy;
            st.clut_x = q->clx; st.clut_y = q->cly; st.bpp = q->bpp; st.blend = 2; /* additive */
            openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
        }
    }
    for (int i = 0; i < s_fcap_n; i++) {
        fcap* c = &s_fcap[i];
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = c->sx[k]; st.v[k].sy = c->sy[k];
            st.v[k].z  = openpete_native_depth_from_viewz(openpete_worldvz_full(c->sz[k]));
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (c->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0; st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = 0;                       /* G3 caps are opaque (code 0x30, ABE clear) */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    }
}

/* ---- gating --------------------------------------------------------------- */
static int flame_verify_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_FLAME_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
static unsigned flame_verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_FLAME_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

static int on_80058D64_flame_native(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = flame_verify_enabled();
    int feed   = openpete_native_render_enabled();
    /* --render-paths also needs the bracket so the OT filter knows this path's
     * polybuf range, even when not feeding/verifying. */
    if (!verify && !feed && !render_paths_active()) return 0;  /* inert: normal dispatch runs it */
    static int s_in = 0; if (s_in) return 0; s_in = 1;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_80058D64(cpu);                          /* run the real renderer */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_FLAME, startp, endp);   /* polybuf range for the OT filter */

    if ((feed || verify) && endp > startp) capture_prims(cpu, startp, endp);

    if (feed && endp > startp && render_path_enabled(RP_FLAME)) {
        if (getenv("OPENPETE_FLAME_SCAFFOLD"))
            flame_feed_scaffold(cpu);             /* fallback: .s-prim scaffold */
        else
            flame_feed_native(cpu);               /* native gamestate walk (default) */
    }

    if (verify && endp > startp && openpete_frame >= flame_verify_frame()) {
        static int s_dumped = 0, s_reports = 0;
        if (!s_dumped) { flame_dump(cpu, startp, endp); s_dumped = 1; }
        if (s_reports < 200) { flame_report(cpu); flame_strip_compare(cpu); s_reports++; }
    }

    s_in = 0;
    return 1;
}

/* Sub-tick re-extract (high-FPS spine): rebuild Spyro's flame ribbon from
 * gamestate — extract-only. Self-gated on m_IsFlameActive (the .s renderer is
 * only called while breathing fire, so feeding unconditionally would draw a
 * stale ribbon). The ribbon attaches to Spyro's (bracketed) pose and projects
 * through the interpolated g_Camera, so it stays coherent with the scene. */
void flame_native_reextract(CPUState* cpu) {
    if (!openpete_native_render_enabled()) return;
    if (!render_path_enabled(RP_FLAME)) return;
    /* Belt-and-braces with the m_IsFlameActive self-gate below: only replay the
     * flame if the canonical frame drew it (skips stale ribbons during a level
     * transition where the active flag could lag the suppressed render). */
    if (!render_path_canon_drawn(RP_FLAME)) return;
    if (cpu->read_word(G_SPYROFLAME + FL_ACTIVE) == 0) return;   /* not breathing fire */
    flame_feed_native(cpu);
}

static const feature_dispatch_entry_t flame_dispatch[] = {
    { 0x80058D64u, on_80058D64_flame_native },
    { 0u, NULL },
};

const feature_module_t flame_native_module = {
    .name           = "flame_native",
    .apply_options  = NULL,
    .dispatch_table = flame_dispatch,
};
