// games/spyro-1/features/cyclorama_native.c — Phase 2 of
// notes/plans/openpete-native-renderer.md: the cyclorama/sky "native
// 3D from gamestate" spike + its autonomous numerical A/B harness.
//
// The native renderer re-derives the sky dome from gamestate (g_Cyclorama
// + the camera matrices) in float, instead of replaying the PSX renderer's
// GPU packets. This module is the game-specific *scene extraction* half:
// it reads g_Cyclorama, unpacks the packed dome sectors, float-projects
// them (mirroring the GTE math in runner/src/gte.cpp), and produces a list
// of screen-space triangles. core/ consumes that list (display only).
//
// Verification (instruments over pixels): hook func_8004EBA8 (the dome
// renderer, 0x8004EBA8), capture the *actual* POLY_F3/G3 prims it emits to
// the polybuf as ground truth, run the native float extraction on the same
// inputs, and compare screen positions (expect sub-pixel — float is more
// precise than the GTE's 16.16) + colours (expect exact — gouraud colours
// are copied, not projected). All headless, all numerical: the autonomous
// Phase-2 verdict. Gated by OPENPETE_CYC_VERIFY (off => zero overhead).
//
// Determinism: this never writes PSX RAM. When verifying it calls the real
// func_8004EBA8 exactly once (behaviour-identical to the normal dispatch
// fall-through) and only READS the polybuf + g_Cyclorama afterwards.
//
// Format map: the format notes (asm ground
// truth, corroborated by SpyroEdit SkySectorHeaderS1 / UpdateSky).
//
// The same extraction also serves the level-exit / balloonist cross-fade
// renderer func_8004F000 — the same dome with a DPCS colour fade — via
// cyclorama_native_extract_faded, which transition_native.c feeds.

#include "feature_registry.h"
#include "psx_runtime.h"   /* full CPUState (gte_ctrl, a0/a1/a2, read_word) */
#include "openpete_psx.h"  /* openpete_psx_to_host_ptr (present-time DRAWENV read) */
#include "openpete_native_render.h"  /* native scene push + enabled gate */
#include "render_paths.h"  /* --render-paths isolation */
#include "spyro_native_reextract.h"  /* sub-tick re-extract entry */
#include "native_gte.h"    /* shared GTE float-mirror math (W2.3) */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"

extern void     func_8004EBA8(CPUState* cpu);  /* the real dome renderer */
extern unsigned openpete_frame;


/* ---- native scene triangle (screen-space, float) -------------------- */
typedef struct { float sx, sy; uint32_t rgb; unsigned char sat; } cyc_vert; /* rgb=low24; sat=GTE IR1/IR2 would clamp */
typedef struct { cyc_vert v[3]; int gouraud; } cyc_tri;

#define MAX_TRIS 16384
static cyc_tri s_native[MAX_TRIS]; static int s_native_n;
static cyc_tri s_gte[MAX_TRIS];    static int s_gte_n;

/* ---- helpers -------------------------------------------------------- */

/* Continuous float projection mirroring the GTE RTPS: ngte_project_raw
 * (shared, W2.3). The sat flag bins the two GTE fixed-point clamps the
 * unclamped float path diverges from on purpose (IR16 saturation + the
 * H >= 2*SZ3 divide cap). Returns view-space Z (MAC3); caller treats
 * <=0 as behind-camera. */
static double project(const int M[3][3], int vx, int vy, int vz,
                      double ofx_px, double ofy_px, double H,
                      double* out_sx, double* out_sy, int* out_sat) {
    return ngte_project_raw(M, NULL, (double)vx, (double)vy, (double)vz,
                            ofx_px, ofy_px, H, out_sx, out_sy, out_sat);
}

/* ---- near-plane clip (mirrors terrain_native.c::clip_push_tri) -------
 * The sky dome is a FULL 360° surround (all sectors live in RAM); at wide
 * aspect ratio the side/wrap geometry that the canonical 4:3 path near-plane
 * REJECTED (whole-tri drop when any vertex is behind the camera) is now on
 * screen, so it must be CLIPPED to the near plane instead — the in-front part
 * renders, the behind part is cut. Camera-space (m1,m2,m3); m3 = view-Z. */
#define CYC_NEARZ 1.0
typedef struct { double m1, m2, m3; uint32_t rgb; } cyc_cv;

static cyc_cv cyc_lerp(cyc_cv a, cyc_cv b) {       /* crossing at m3 == CYC_NEARZ */
    double t = (CYC_NEARZ - a.m3) / (b.m3 - a.m3);
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    cyc_cv r;
    r.m1 = a.m1 + t * (b.m1 - a.m1);
    r.m2 = a.m2 + t * (b.m2 - a.m2);
    r.m3 = CYC_NEARZ;
    int ar=a.rgb&0xFF, ag=(a.rgb>>8)&0xFF, ab=(a.rgb>>16)&0xFF;
    int br=b.rgb&0xFF, bg=(b.rgb>>8)&0xFF, bb=(b.rgb>>16)&0xFF;
    int rr=(int)(ar+t*(br-ar)+0.5), rg=(int)(ag+t*(bg-ag)+0.5), rb=(int)(ab+t*(bb-ab)+0.5);
    r.rgb = (uint32_t)rr | ((uint32_t)rg<<8) | ((uint32_t)rb<<16);
    return r;
}

/* Project an in-front cyc_cv to screen and push to s_native, applying the
 * widescreen-aware horizontal trivial reject (vertical stays 4:3). */
static void cyc_push_tri(cyc_cv a, cyc_cv b, cyc_cv c, int gouraud,
                         double ofx, double ofy, double H) {
    float xlo, xhi; openpete_native_widescreen_x_bounds(&xlo, &xhi);
    cyc_cv v[3] = { a, b, c }; float sx[3], sy[3];
    for (int k = 0; k < 3; k++) {
        double z = v[k].m3 != 0.0 ? v[k].m3 : 1.0;
        sx[k] = (float)(ofx + v[k].m1 * H / z);
        sy[k] = (float)(ofy + v[k].m2 * H / z);
    }
    if ((sx[0]<xlo&&sx[1]<xlo&&sx[2]<xlo) || (sx[0]>xhi&&sx[1]>xhi&&sx[2]>xhi) ||
        (sy[0]<0&&sy[1]<0&&sy[2]<0)       || (sy[0]>240&&sy[1]>240&&sy[2]>240)) return;
    if (s_native_n >= MAX_TRIS) return;
    cyc_tri* t = &s_native[s_native_n++];
    t->gouraud = gouraud;
    for (int k = 0; k < 3; k++) {
        t->v[k].sx = sx[k]; t->v[k].sy = sy[k]; t->v[k].rgb = v[k].rgb; t->v[k].sat = 0;
    }
}

/* GTE DPCS in float: lerp a dome colour toward the far colour FC by IR0/4096.
 * The cross-fade dome renderer (func_8004F000) runs the real op (cmd 0x10,
 * sf=1) over the sector colour table with FC = D_BLEND_TARGET's components
 * SHL 4 and IR0 = D_FOG_FACTOR; since FC SHL 12 then lines up with the
 * colour's SHL 16, the whole op reduces to this per-channel lerp. */
static uint32_t cyc_fade(uint32_t rgb, uint32_t fc, int ir0) {
    uint32_t out = 0;
    for (int k = 0; k < 3; k++) {
        int c = (int)((rgb >> (k * 8)) & 0xFF), f = (int)((fc >> (k * 8)) & 0xFF);
        int v = (int)(c + (f - c) * (double)ir0 / 4096.0 + 0.5);   /* float, like the rest of the path */
        if (v < 0) v = 0; else if (v > 255) v = 255;
        out |= (uint32_t)v << (k * 8);
    }
    return out;
}

/* Sutherland-Hodgman clip a gouraud tri to m3 >= CYC_NEARZ, then push the
 * resulting polygon (fan). 0 clipped -> 1 tri; 1 -> quad (2 tris); 2 -> 1 tri;
 * fully behind -> dropped. */
static void cyc_clip_emit(cyc_cv a, cyc_cv b, cyc_cv c, int gouraud,
                          double ofx, double ofy, double H) {
    cyc_cv in[3] = { a, b, c }, out[4]; int nout = 0;
    for (int i = 0; i < 3; i++) {
        cyc_cv cur = in[i], nxt = in[(i + 1) % 3];
        int ci = cur.m3 >= CYC_NEARZ, ni = nxt.m3 >= CYC_NEARZ;
        if (ci) out[nout++] = cur;
        if (ci != ni && nout < 4) out[nout++] = cyc_lerp(cur, nxt);
    }
    if (nout < 3) return;
    cyc_push_tri(out[0], out[1], out[2], gouraud, ofx, ofy, H);
    if (nout == 4) cyc_push_tri(out[0], out[2], out[3], gouraud, ofx, ofy, H);
}

/* ---- native extraction (the Phase-2 deliverable) -------------------- */
/* Reads g_Cyclorama + the two matrices + GTE OFX/OFY/H from the hook, and
 * fills s_native[]. Mirrors func_8004EBA8: phase-1 sector visibility cull
 * (cull matrix a1), phase-2 per-vertex unpack + float project (draw matrix
 * a2) + per-face flat/gouraud emit. */
static void cyc_extract(CPUState* cpu, uint32_t a0, uint32_t a1, uint32_t a2,
                        uint32_t fade_rgb, int fade_ir0) {
    s_native_n = 0;

    int Mcull[3][3], Mdraw[3][3];
    ngte_read_matrix(cpu, a1, Mcull);
    ngte_read_matrix(cpu, a2, Mdraw);
    double ofx = (double)(int32_t)cpu->gte_ctrl[24] / 65536.0;
    double ofy = (double)(int32_t)cpu->gte_ctrl[25] / 65536.0;
    double H   = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);

    uint32_t sectorCount = cpu->read_word(G_CYCLORAMA + 0x00);
    uint32_t sectorsBase = cpu->read_word(G_CYCLORAMA + 0x04);
    uint32_t occGroups   = cpu->read_word(G_CYCLORAMA + 0x0C);

    /* Build the sector-pointer iteration set (mirrors func_8004EBA8 phase 0).
     * In widescreen the .s's per-view occlusion GROUP (occGroups[a0]) is a 4:3
     * selection that omits the dome sectors visible only at the widened sides,
     * so iterate ALL sectors instead (the full 360° dome is in RAM; the
     * per-sector radius cull below + the near-plane clip + the scissor confine
     * it to the visible frustum). Display-only. */
    int ws = openpete_native_widescreen_factor() > 1.0001f;
    uint32_t sectors[1024]; uint32_t nSectors = 0;
    if ((int32_t)a0 < 0 || ws) {
        if (sectorCount > 1024) sectorCount = 1024;
        for (uint32_t i = 0; i < sectorCount; i++)
            sectors[nSectors++] = cpu->read_word(sectorsBase + i * 4);
    } else if (occGroups) {
        uint32_t list = cpu->read_word(occGroups + a0 * 4);
        if (list) {
            for (uint32_t k = 0; k < 1024; k++) {
                uint8_t idx = cpu->read_byte(list + k);
                if (idx == 0xFF) break;
                sectors[nSectors++] = cpu->read_word(sectorsBase + (uint32_t)idx * 4);
            }
        }
    }

    static double   pm1[4096], pm2[4096], pz[4096];   /* camera-space MAC per vertex */
    static uint32_t col[4096];

    for (uint32_t si = 0; si < nSectors; si++) {
        uint32_t S = sectors[si];
        if (!S) continue;

        /* phase-1 cull: project the representative point with the cull
         * matrix; visible iff MAC3 > radius. SKIPPED in widescreen — this is a
         * 4:3-frustum range/behind cull that drops side sectors which are
         * partially visible at wide FOV; the near-plane clip + trivial reject
         * confine the kept geometry correctly instead. */
        if (!ws) {
            uint32_t c0 = cpu->read_word(S + 0x00), c1 = cpu->read_word(S + 0x04);
            int cull_vx = (int16_t)(c0 & 0xFFFF), cull_vy = (int16_t)(c0 >> 16);
            int cull_vz = (int16_t)(c1 >> 16);
            int radius  = (int16_t)(c1 & 0xFFFF);
            double dsx, dsy;
            double cull_mac3 = project(Mcull, cull_vx, cull_vy, cull_vz, 0, 0, H, &dsx, &dsy, NULL);
            if (cull_mac3 <= (double)radius) continue;
        }

        uint32_t h8 = cpu->read_word(S + 0x08), hC = cpu->read_word(S + 0x0C);
        uint32_t hF = cpu->read_word(S + 0x10);
        int centerX = (int16_t)(h8 & 0xFFFF), centerY = (int16_t)(h8 >> 16);
        int centerZ = (int16_t)(hC >> 16);
        uint32_t vertexCount = hC & 0xFFFF;
        uint32_t faceCount   = hF & 0x1FFF;
        uint32_t faceOffByte = hF >> 14;

        if (vertexCount > 4096) vertexCount = 4096;
        uint32_t vertBase  = S + 0x18;
        uint32_t colorBase = vertBase + vertexCount * 4;       /* s4 */
        uint32_t faceBase  = colorBase + faceOffByte;

        /* phase-2a: unpack + compute the camera-space MAC for every vertex (the
         * near-plane clip interpolates in camera space, then projects). */
        for (uint32_t i = 0; i < vertexCount; i++) {
            uint32_t w = cpu->read_word(vertBase + i * 4);
            int vx = centerX - (int)((w >> 10) & 0x7FF);
            int vy = centerY - (int)(w & 0x3FF);
            int vz = centerZ + (int)(w >> 21);
            pm1[i] = (Mdraw[0][0]*(double)vx + Mdraw[0][1]*(double)vy + Mdraw[0][2]*(double)vz) / 4096.0;
            pm2[i] = (Mdraw[1][0]*(double)vx + Mdraw[1][1]*(double)vy + Mdraw[1][2]*(double)vz) / 4096.0;
            pz[i]  = (Mdraw[2][0]*(double)vx + Mdraw[2][1]*(double)vy + Mdraw[2][2]*(double)vz) / 4096.0;
        }
        uint32_t colorCount = faceOffByte / 4;
        if (colorCount > 4096) colorCount = 4096;
        for (uint32_t j = 0; j < colorCount; j++) {
            uint32_t c = cpu->read_word(colorBase + j * 4) & 0xFFFFFF;
            col[j] = fade_ir0 ? cyc_fade(c, fade_rgb, fade_ir0) : c;
        }

        /* phase-2b: faces. word0 = vertex indices, word1 = colour indices.
         * Flat (F3) when the 3 colour indices match, else gouraud (G3). */
        for (uint32_t f = 0; f < faceCount; f++) {
            uint32_t w0 = cpu->read_word(faceBase + f * 8);
            uint32_t w1 = cpu->read_word(faceBase + f * 8 + 4);
            uint32_t i0 = (w0 >> 22) & 0x3FF, i1 = (w0 >> 12) & 0xFF, i2 = (w0 >> 2) & 0xFF;
            uint32_t k0 = (w1 >> 22) & 0x3FF, k1 = (w1 >> 12) & 0xFF, k2 = (w1 >> 2) & 0xFF;
            if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
            int gouraud = !(k0 == k1 && k0 == k2);
            uint32_t cc0 = (k0 < colorCount) ? col[k0] : 0;
            uint32_t cc1 = gouraud ? ((k1 < colorCount) ? col[k1] : 0) : cc0;
            uint32_t cc2 = gouraud ? ((k2 < colorCount) ? col[k2] : 0) : cc0;

            /* Near-plane CLIP (not reject) so the dome's side/wrap geometry that
             * straddles the camera renders its in-front part at wide aspect — the
             * old whole-tri reject dropped it (tuned for 4:3 ±37°). cyc_push_tri
             * applies the widescreen-aware horizontal trivial reject + scissor. */
            cyc_cv A = { pm1[i0], pm2[i0], pz[i0], cc0 };
            cyc_cv B = { pm1[i1], pm2[i1], pz[i1], cc1 };
            cyc_cv C = { pm1[i2], pm2[i2], pz[i2], cc2 };
            cyc_clip_emit(A, B, C, gouraud, ofx, ofy, H);
        }
    }
}

/* ---- ground-truth capture: decode the prims func_8004EBA8 emitted ---- */
/* Prims are contiguous in [start,end). POLY_F3 = 0x14 bytes
 * {next,rgb,xy0,xy1,xy2}; POLY_G3 = 0x1C bytes
 * {next,rgb0,xy0,rgb1,xy1,rgb2,xy2}. word0 high byte distinguishes them
 * (confirmed by the hex dump below). */
static void unpack_xy(uint32_t xy, float* x, float* y) {
    *x = (float)(int16_t)(xy & 0xFFFF);
    *y = (float)(int16_t)(xy >> 16);
}
static void capture_gte(CPUState* cpu, uint32_t start, uint32_t end) {
    s_gte_n = 0;
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < MAX_TRIS) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t code = w0 >> 24;            /* prim type byte (see dump) */
        int gouraud;
        if (code == 0x06) gouraud = 1;
        else if (code == 0x04) gouraud = 0;
        else { p += 4; continue; }           /* resync if layout differs */
        cyc_tri* t = &s_gte[s_gte_n];
        if (s_gte_n < MAX_TRIS) {
            if (gouraud) {
                t->gouraud = 1;
                t->v[0].rgb = cpu->read_word(p + 0x04) & 0xFFFFFF;
                unpack_xy(cpu->read_word(p + 0x08), &t->v[0].sx, &t->v[0].sy);
                t->v[1].rgb = cpu->read_word(p + 0x0C) & 0xFFFFFF;
                unpack_xy(cpu->read_word(p + 0x10), &t->v[1].sx, &t->v[1].sy);
                t->v[2].rgb = cpu->read_word(p + 0x14) & 0xFFFFFF;
                unpack_xy(cpu->read_word(p + 0x18), &t->v[2].sx, &t->v[2].sy);
            } else {
                uint32_t rgb = cpu->read_word(p + 0x04) & 0xFFFFFF;
                t->gouraud = 0;
                t->v[0].rgb = t->v[1].rgb = t->v[2].rgb = rgb;
                unpack_xy(cpu->read_word(p + 0x08), &t->v[0].sx, &t->v[0].sy);
                unpack_xy(cpu->read_word(p + 0x0C), &t->v[1].sx, &t->v[1].sy);
                unpack_xy(cpu->read_word(p + 0x10), &t->v[2].sx, &t->v[2].sy);
            }
            s_gte_n++;
        }
        p += gouraud ? 0x1C : 0x14;
    }
}

/* ---- comparison ----------------------------------------------------- */
static void centroid(const cyc_tri* t, float* cx, float* cy) {
    *cx = (t->v[0].sx + t->v[1].sx + t->v[2].sx) / 3.0f;
    *cy = (t->v[0].sy + t->v[1].sy + t->v[2].sy) / 3.0f;
}
/* On-screen iff the GTE (int16, post-clip) position is inside the frame.
 * Off-screen verts are where the GTE saturates IR1/IR2 to +-32767 while the
 * float projection does not — the sanctioned, expected float/GTE divergence
 * (the float is the more-correct projection). Binned separately. */
static int onscreen(const cyc_vert* v) {
    return v->sx >= -1.f && v->sx <= 513.f && v->sy >= -1.f && v->sy <= 241.f;
}
static void compare_and_report(void) {
    /* Match gte->native by centroid, breaking ties toward an exact colour
     * match (avoids mispairing among the few extra boundary tris). */
    static int used[MAX_TRIS];
    memset(used, 0, sizeof(int) * (s_native_n > 0 ? s_native_n : 1));
    int matched = 0, color_exact = 0, fg_disagree = 0;
    /* on-screen, non-saturated verts: the visible result, expected ~exact */
    int on_n = 0;       double on_sum = 0.0, on_max = 0.0;
    /* floor-truncation check: GTE = floor(continuous); so floor(native)
     * should equal GTE within +-1 (reciprocal rounding) for on-screen */
    int floor_ok = 0;   double signed_sum = 0.0;
    int off_n = 0;
    double worst = -1.0; float wnx=0,wny=0,wgx=0,wgy=0;
    for (int g = 0; g < s_gte_n; g++) {
        /* Score candidates by per-vertex distance sum (my tri vertex order
         * matches the asm's emit order o0->xy0,o1->xy1,o2->xy2, so v[k]<->v[k]).
         * This disambiguates colour-identical neighbour tris that a centroid
         * match would mispair. */
        int best = -1; float bestd = 1e18f;
        for (int n = 0; n < s_native_n; n++) {
            if (used[n]) continue;
            float d = 0.f;
            for (int k = 0; k < 3; k++) {
                float dx = s_gte[g].v[k].sx - s_native[n].v[k].sx;
                float dy = s_gte[g].v[k].sy - s_native[n].v[k].sy;
                d += dx*dx + dy*dy;
            }
            if (d < bestd) { bestd = d; best = n; }
        }
        if (best < 0 || bestd > 3.f*(96.0f*96.0f)) continue;
        used[best] = 1; matched++;
        const cyc_tri *G = &s_gte[g], *N = &s_native[best];
        if (G->v[0].rgb==N->v[0].rgb && G->v[1].rgb==N->v[1].rgb && G->v[2].rgb==N->v[2].rgb)
            color_exact++;
        if (G->gouraud != N->gouraud) fg_disagree++;
        for (int k = 0; k < 3; k++) {
            double dx = N->v[k].sx - G->v[k].sx, dy = N->v[k].sy - G->v[k].sy;
            double dist = sqrt(dx*dx + dy*dy);
            /* exact-expected = on-screen AND the GTE IR clamp didn't bite;
             * everything else is the sanctioned float/GTE saturation divergence. */
            if (onscreen(&G->v[k]) && !N->v[k].sat) {
                on_n++; on_sum += dist; if (dist > on_max) on_max = dist;
                signed_sum += dx;
                if (fabs(floor(N->v[k].sx) - G->v[k].sx) <= 1.0 &&
                    fabs(floor(N->v[k].sy) - G->v[k].sy) <= 1.0) floor_ok++;
                if (dist > worst) { worst = dist; wnx=N->v[k].sx; wny=N->v[k].sy; wgx=G->v[k].sx; wgy=G->v[k].sy; }
            } else off_n++;
        }
    }
    fprintf(stderr,
        "[cyc-verify f=%u] gte=%d native=%d matched=%d | ONSCREEN verts=%d "
        "mean=%.3fpx max=%.3fpx floor-match(+-1)=%d/%d(%.1f%%) bias=%.2fpx | "
        "offscreen(saturated)=%d | colour-exact=%d/%d(%.1f%%) F3/G3-disagree=%d | "
        "worst-onscreen native(%.1f,%.1f)vs gte(%.0f,%.0f)\n",
        openpete_frame, s_gte_n, s_native_n, matched,
        on_n, on_n?on_sum/on_n:0.0, on_max, floor_ok, on_n,
        on_n?100.0*floor_ok/on_n:0.0, on_n?signed_sum/on_n:0.0,
        off_n, color_exact, matched, matched?100.0*color_exact/matched:0.0,
        fg_disagree, wnx,wny,wgx,wgy);
}

/* ---- one-shot format dump (falsify-before-ship) --------------------- */
static void hex_dump(CPUState* cpu, uint32_t a0, uint32_t a1, uint32_t a2,
                     uint32_t start, uint32_t end) {
    fprintf(stderr, "[cyc-dump f=%u] a0=%d a1=%08X a2=%08X OFX=%d OFY=%d H=%u\n",
            openpete_frame, (int32_t)a0, a1, a2,
            (int32_t)cpu->gte_ctrl[24] >> 16, (int32_t)cpu->gte_ctrl[25] >> 16,
            cpu->gte_ctrl[26] & 0xFFFF);
    fprintf(stderr, "  g_Cyclorama: count=%u sectors=%08X occCnt=%u occGrp=%08X bg=%08X\n",
            cpu->read_word(G_CYCLORAMA), cpu->read_word(G_CYCLORAMA+4),
            cpu->read_word(G_CYCLORAMA+8), cpu->read_word(G_CYCLORAMA+0xC),
            cpu->read_word(G_CYCLORAMA+0x10));
    uint32_t s0 = cpu->read_word(cpu->read_word(G_CYCLORAMA+4)); /* first sector ptr */
    fprintf(stderr, "  sector0=%08X hdr: ", s0);
    for (int i = 0; i <= 0x14; i += 4) fprintf(stderr, "[%02X]=%08X ", i, cpu->read_word(s0+i));
    fprintf(stderr, "\n  sector0 verts: ");
    for (int i = 0; i < 4; i++) fprintf(stderr, "%08X ", cpu->read_word(s0+0x18+i*4));
    uint32_t vc = cpu->read_word(s0+0xC) & 0xFFFF;
    uint32_t cb = s0+0x18+vc*4;
    fprintf(stderr, "\n  sector0 colours@%08X: ", cb);
    for (int i = 0; i < 4; i++) fprintf(stderr, "%08X ", cpu->read_word(cb+i*4));
    uint32_t foff = cpu->read_word(s0+0x10) >> 14;
    uint32_t fb = cb + foff;
    fprintf(stderr, "\n  sector0 faces@%08X (foff=%u fcnt=%u): ", fb, foff, cpu->read_word(s0+0x10)&0x1FFF);
    for (int i = 0; i < 4; i++) fprintf(stderr, "%08X ", cpu->read_word(fb+i*4));
    fprintf(stderr, "\n  prim region [%08X,%08X) %u bytes:\n   ", start, end, end-start);
    int n = (int)((end - start) / 4); if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "%08X ", cpu->read_word(start + i*4));
        if ((i & 7) == 7) fprintf(stderr, "\n   ");
    }
    fprintf(stderr, "\n");
}

/* ---- dispatch hook -------------------------------------------------- */
static int cyc_verify_enabled(void) {
    static int s = -1;
    if (s < 0) { const char* v = getenv("OPENPETE_CYC_VERIFY"); s = (v && v[0] && v[0] != '0') ? 1 : 0; }
    return s;
}
static unsigned cyc_verify_frame(void) {
    static unsigned f = 0; static int init = 0;
    if (!init) { const char* v = getenv("OPENPETE_CYC_VERIFY_FRAME"); f = v ? (unsigned)atoi(v) : 2000u; init = 1; }
    return f;
}

/* Push the extracted native triangles into the core scene buffer (panel 0).
 * PSX prim colour low-24 is byte0=R,byte1=G,byte2=B — already the RHI rgba
 * byte order; add opaque alpha. */
static void push_scene(CPUState* cpu) {
    static int s_logged = 0;
    if (!s_logged && s_native_n > 0) {
        s_logged = 1;
        fprintf(stderr, "[cyc-native] feeding panel 0: %d cyclorama tris from gamestate (frame %u)\n",
                s_native_n, openpete_frame);
    }
    openpete_native_scene_begin_frame(openpete_frame);
    /* The PSX cyclorama clears the frame to g_Cyclorama.m_BackgroundColor
     * (+0x10) before drawing the dome; mirror that so uncovered areas (below
     * the dome / gaps) show it, not black. Same colour conversion as the dome
     * (low 24 = R,G,B; opaque alpha). **The background COLOUR is a scene
     * property, not part of the cyclorama dome geometry** — set it always (even
     * when `--render-paths` excludes the dome), so an isolated path (e.g.
     * `--render-paths spyro`) renders over the real scene background, matching
     * PsyCross (which always clears to it via the draw-env). Only the dome
     * TRIANGLES are gated on RP_CYCLORAMA. */
    uint32_t bg = cpu->read_word(G_CYCLORAMA + 0x10) & 0xFFFFFF;
    openpete_native_scene_set_bg(bg | 0xFF000000u);
    if (!render_path_enabled(RP_CYCLORAMA)) return;  /* bg only; dome not selected */
    for (int i = 0; i < s_native_n; i++) {
        openpete_scene_tri t;
        for (int k = 0; k < 3; k++) {
            t.v[k].sx   = s_native[i].v[k].sx;
            t.v[k].sy   = s_native[i].v[k].sy;
            t.v[k].z    = 1.0f;   /* sky = the backdrop: always behind depth-tested actors */
            t.v[k].u    = 0.0f; t.v[k].v = 0.0f;
            t.v[k].rgba = s_native[i].v[k].rgb | 0xFF000000u;
        }
        t.textured = 0;   /* cyclorama is gouraud-shaded (no texture) */
        t.tpage_x = t.tpage_y = t.clut_x = t.clut_y = t.bpp = 0;
        t.blend = 0;      /* opaque */
        /* SKY, not WORLD (W2.6-c): drawn identically (opaque, depth-tested,
         * first) but without the WORLD group's stencil coverage MARK — the sky
         * showing through a terrain seam is exactly what the LP hole-fill
         * layer must be allowed to repaint. */
        openpete_native_scene_add_tri(&t, OPENPETE_SCENE_SKY, 0);
    }
}

/* Camera-matrix args from the last real render (cyc_extract reads the matrices
 * at a1/a2; they persist in RAM across the tick). Reused by the sub-tick
 * re-extract, which has no register context. */
static uint32_t s_cyc_a0 = 0xFFFFFFFFu, s_cyc_a1 = 0, s_cyc_a2 = 0;

static int on_8004EBA8_cyc_native(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = cyc_verify_enabled();
    int feed   = openpete_native_render_enabled();   /* --renderer vk|ab|diff live */
    /* --render-paths also needs the bracket so the OT filter knows this path's
     * polybuf range, even when not feeding/verifying. */
    if (!verify && !feed && !render_paths_active()) return 0;  /* zero overhead */
    static int s_in = 0;
    if (s_in) return 0;
    s_in = 1;

    uint32_t a0 = cpu->a0, a1 = cpu->a1, a2 = cpu->a2;
    s_cyc_a0 = a0; s_cyc_a1 = a1; s_cyc_a2 = a2;   /* for sub-tick re-extract */
    uint32_t start = cpu->read_word(POLYBUF_CURSOR);
    func_8004EBA8(cpu);                     /* run the real renderer */
    uint32_t end = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_CYCLORAMA, start, end);  /* polybuf range for the OT filter */

    cyc_extract(cpu, a0, a1, a2, 0, 0);     /* native scene from gamestate */
    if (feed)
        push_scene(cpu);                    /* always sets the scene bg colour;
                                             * dome tris gated inside on RP_CYCLORAMA */

    if (verify) {
        static int s_dumped = 0;
        static int s_reports = 0;
        int target = (openpete_frame >= cyc_verify_frame() && s_reports < 16);
        if (target && !s_dumped) { hex_dump(cpu, a0, a1, a2, start, end); s_dumped = 1; }
        if (target) {
            capture_gte(cpu, start, end);
            compare_and_report();
            s_reports++;
        }
    }
    s_in = 0;
    return 1;                               /* took over (body already ran) */
}

/* Sub-tick re-extract (high-FPS spine): rebuild the sky dome + background from
 * the saved matrix POINTERS — extract-only (no func_8004EBA8 call, no verdict
 * writes). The saved a1/a2 are g_Camera+0x14 (m_ViewMatrix) and +0x00
 * (m_ProjectionMatrix) — the SAME regions the spine's camera bracket interpolates
 * each present — and cyc_extract reads them LIVE, so the sky DOES rotate smoothly
 * sub-tick (verified 2026-06-22: a1=cam+0x14, a2=cam+0x00; the earlier
 * "stale tick-N / later refinement" note was wrong). push_scene also re-sets the
 * scene background. */
void cyclorama_native_reextract(CPUState* cpu) {
    if (!openpete_native_render_enabled()) return;
    /* Only replay the sky dome if the canonical frame drew it. In cyclorama-less
     * states (e.g. the black level-name flyby) the saved matrices are stale; the
     * draw-env clear (black) shows through instead. The sky-having transitions
     * (the "ENTERING X" card over a sky) keep RP_CYCLORAMA set, so they're
     * unaffected. */
    if (!render_path_canon_drawn(RP_CYCLORAMA)) return;
    if (s_cyc_a1 == 0 || s_cyc_a2 == 0) return;   /* no real frame captured yet */
    cyc_extract(cpu, s_cyc_a0, s_cyc_a1, s_cyc_a2, 0, 0);
    push_scene(cpu);
}

/* ---- the cross-fade dome (func_8004F000) ----------------------------------
 * The level-exit / balloonist cross-fade renderer draws the SAME g_Cyclorama
 * dome as func_8004EBA8, with two differences: every sector colour is first
 * run through a GTE DPCS fade toward D_BLEND_TARGET by D_FOG_FACTOR, and the
 * cull/draw matrices are g_Camera's own (view @+0x14, projection @+0x00)
 * rather than caller-supplied. Its visibility test is a per-vertex outcode
 * against the 4:3 box [0,512]x[0,256], AND-ed over each face, so the prims it
 * emits stop dead at the 4:3 edges — replaying them leaves the widened sides
 * of a Hor+ frame on the background clear. Re-derive the dome here instead,
 * through the same widescreen-aware sector iteration, near-plane clip and
 * trivial reject the main sky uses. Fills up to `max` triangles and returns
 * the count. */
int cyclorama_native_extract_faded(CPUState* cpu, cyclorama_native_tri* out, int max) {
    cyc_extract(cpu, 0xFFFFFFFFu, G_CAMERA + 0x14, G_CAMERA,
                cpu->read_word(D_BLEND_TARGET) & 0xFFFFFFu,
                (int)cpu->read_word(D_FOG_FACTOR));
    int n = s_native_n < max ? s_native_n : max;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            out[i].sx[k]  = s_native[i].v[k].sx;
            out[i].sy[k]  = s_native[i].v[k].sy;
            out[i].rgb[k] = s_native[i].v[k].rgb;
        }
    return n;
}

/* ---- present-time draw-env clear sync (portal-entry flash fix) ---------
 * The native background fill (s_drawenv_clear) used to come only from the
 * PutDrawEnv THUNK latch — but the game calls PutDrawEnv one VSync AFTER it
 * builds (and we feed) each OT: GamestateDraw flips g_CurDB at the top of the
 * pass, the gamestate writes g_CurDB->m_DrawEnv.r0g0b0 (e.g. GS_LevelTransition
 * = g_Cyclorama.m_BackgroundColor, fresh destination sky), builds the OT
 * (feeders fire), then VSync-WAITS — yielding to the host, which PRESENTS the
 * just-fed scene — and only then PutDrawEnv+DrawOTag. So at the transition
 * boundary the live present paired the NEW scene with the OLD latched clear:
 * a 1-present flash of the homeworld background under the destination sky.
 * PsyCross never flashes because it composes the OT only at DrawOTag, after
 * the env applies.
 *
 * Fix: at every canonical host present (render_paths_commit), re-read the
 * clear from the DRAWENV STRUCT the game keeps in RAM — g_CurDB->m_DrawEnv —
 * which by the flip-at-pass-top invariant is exactly the env DrawOTag will
 * use for the scene being presented. State-universal: every gamestate writes
 * its clear into the struct before building the OT (GS_ExitLevel even writes
 * its ColorLerp fade step there). The thunk latch stays as the fallback for
 * the brief boot window where g_CurDB doesn't point at g_DB[] yet. */
void cyclorama_native_env_clear_commit(void) {
    const uint32_t curdb = *(const uint32_t*)openpete_psx_to_host_ptr(G_CUR_DB);
    if (curdb != G_DB && curdb != G_DB + 0x84u) return;   /* not a valid DB* yet */
    const uint8_t* env = (const uint8_t*)openpete_psx_to_host_ptr(curdb);
    /* PSYQ DRAWENV: isbg @+0x18, r0/g0/b0 @+0x19..0x1B (m_DrawEnv is DB +0x00) */
    openpete_native_set_clear_color(env[0x18], env[0x19], env[0x1A], env[0x1B]);
}

static const feature_dispatch_entry_t cyc_dispatch[] = {
    { 0x8004EBA8u, on_8004EBA8_cyc_native },
    { 0u, NULL },
};

const feature_module_t cyclorama_native_module = {
    .name           = "cyclorama_native",
    .apply_options  = NULL,
    .dispatch_table = cyc_dispatch,
};
