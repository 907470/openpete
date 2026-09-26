// games/common/renderer/cyclorama.c — the cyclorama/sky renderer
// re-derived from gamestate + its autonomous numerical A/B harness. Every
// engine address comes from game_layout; the game's own sky draw runs
// through the recomp module dispatch at game_layout.hooks.sky_draw.
//
// The native renderer re-derives the sky dome from gamestate (g_Cyclorama
// + the camera matrices) in float, instead of replaying the PSX renderer's
// GPU packets. This module is the game-specific *scene extraction* half:
// it reads g_Cyclorama, unpacks the packed dome sectors, float-projects
// them (mirroring the GTE math in runner/src/gte.cpp), and produces a list
// of screen-space triangles. core/ consumes that list (display only).
//
// Verification (instruments over pixels): hook the dome renderer (Spyro 1
// func_8004EBA8), capture the *actual* POLY_F3/G3 prims it emits to
// the polybuf as ground truth, run the native float extraction on the same
// inputs, and compare screen positions (expect sub-pixel — float is more
// precise than the GTE's 16.16) + colours (expect exact — gouraud colours
// are copied, not projected). All headless, all numerical. Gated by
// OPENPETE_CYC_VERIFY (off => zero overhead).
//
// Determinism: this never writes PSX RAM. When verifying it calls the real
// func_8004EBA8 exactly once (behaviour-identical to the normal dispatch
// fall-through) and only READS the polybuf + g_Cyclorama afterwards.
//
// Format: func_8004EBA8's asm, corroborated by SpyroEdit
// SkySectorHeaderS1 / UpdateSky.
//
// The same extraction also serves the level-exit / balloonist cross-fade
// renderer func_8004F000 — the same dome with a DPCS colour fade — via
// cyclorama_native_extract_faded, which transition.c feeds.

#include "feature_registry.h"
#include "psx_runtime_internal.h"   /* full CPUState (gte_ctrl, a0/a1/a2, read_word) */
#include "openpete_psx.h"
#include "openpete_subtick_view.h"  /* openpete_psx_to_host_ptr (present-time DRAWENV read) */
#include "openpete_native_render.h"  /* native scene push + enabled gate */
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */
#include "renderer/cyclorama.h"  /* this feeder's exports */
#include "renderer/orchestrator.h"  /* cyclorama_native_env_clear_commit */
#include "openpete_recomp_module.h"  /* the game's own sky draw, run through the module dispatch */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_layout.h"          /* game_layout: the engine RAM addresses this walk reads */
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE — fiber-hook cost split */

extern void     func_8004EBA8(CPUState* cpu);  /* the real dome renderer */
extern unsigned openpete_frame;


/* ---- native scene triangle (screen-space, float) -------------------- */
typedef struct { float sx, sy; uint32_t rgb; unsigned char sat; } cyc_vert; /* rgb=low24; sat=GTE IR1/IR2 would clamp */
typedef struct { cyc_vert v[3]; int gouraud; } cyc_tri;

#define MAX_TRIS 16384
static cyc_tri s_native[MAX_TRIS]; static int s_native_n;
static cyc_tri s_gte[MAX_TRIS];    static int s_gte_n;

/* ---- helpers -------------------------------------------------------- */

/** @brief Float-project one vertex by the GTE RTPS math, with no translation.
 *
 * Wraps ngte_project_raw with a NULL translation. The sat flag bins the two
 * GTE fixed-point clamps the unclamped float path departs from (IR16
 * saturation and the H >= 2*SZ3 divide cap).
 * @param M The Q12 rotation matrix.
 * @param vx Vertex X.
 * @param vy Vertex Y.
 * @param vz Vertex Z.
 * @param ofx_px Screen X offset in pixels.
 * @param ofy_px Screen Y offset in pixels.
 * @param v The projection view (H and bounds).
 * @param out_sx Receives screen X.
 * @param out_sy Receives screen Y.
 * @param out_sat Receives the clamp flag; may be NULL.
 * @return View-space Z (MAC3); the caller treats a value <= 0 as behind the
 *         camera. */
static double project(const int M[3][3], int vx, int vy, int vz,
                      double ofx_px, double ofy_px, const ngte_view* v,
                      double* out_sx, double* out_sy, int* out_sat) {
    return ngte_project_raw(M, NULL, (double)vx, (double)vy, (double)vz,
                            ofx_px, ofy_px, v, out_sx, out_sy, out_sat);
}

/* ---- near-plane clip ----------------------------------------------------
 * The sky dome is a full 360 degree surround (all sectors live in RAM); at
 * wide aspect ratio the side/wrap geometry that the canonical 4:3 path's
 * near plane rejected (whole-tri drop when any vertex is behind the camera)
 * is on screen, so it is clipped to the near plane instead: the in-front
 * part renders, the behind part is cut. Camera-space (m1,m2,m3); m3 = view-Z.
 * The cut itself is ngte_clip_lerp, the same edge crossing the terrain uses;
 * the dome carries no UVs and its plane is fixed, so its corners leave
 * u/v/oflag zero and it passes CYC_NEARZ as the plane. */
#define CYC_NEARZ 1.0

/** @brief Project a triangle's three in-front camera-space corners and append it to s_native.
 *
 * Each corner projects as ofx + m1*H/m3, ofy + m2*H/m3 (m3 of 0 divides by
 * 1). The triangle is dropped when all three corners lie left of the view's
 * xlo, right of its xhi (the widescreen-aware horizontal bounds), above 0 or
 * below 240 (vertical stays 4:3), or when s_native is full. Colours are
 * copied; the sat flags are cleared.
 * @param a First corner.
 * @param b Second corner.
 * @param c Third corner.
 * @param gouraud Nonzero for a gouraud triangle, 0 for flat.
 * @param vw The projection view. */
static void cyc_push_tri(ngte_cvert a, ngte_cvert b, ngte_cvert c, int gouraud,
                         const ngte_view* vw) {
    const float xlo = vw->xlo, xhi = vw->xhi;
    const double ofx = vw->ofx, ofy = vw->ofy, H = vw->H_eff;
    ngte_cvert v[3] = { a, b, c }; float sx[3], sy[3];
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

/** @brief Clip a camera-space triangle to m3 >= CYC_NEARZ and push the result as a fan.
 *
 * Sutherland-Hodgman against the one plane: no corner behind yields the
 * triangle, one behind yields a quad pushed as two triangles, two behind
 * yield one triangle, all three behind yield nothing. Crossings come from
 * ngte_clip_lerp, which interpolates the colour.
 * @param a First corner.
 * @param b Second corner.
 * @param c Third corner.
 * @param gouraud Nonzero for a gouraud triangle, 0 for flat.
 * @param vw The projection view. */
static void cyc_clip_emit(ngte_cvert a, ngte_cvert b, ngte_cvert c, int gouraud,
                          const ngte_view* vw) {
    ngte_cvert in[3] = { a, b, c }, out[4]; int nout = 0;
    for (int i = 0; i < 3; i++) {
        ngte_cvert cur = in[i], nxt = in[(i + 1) % 3];
        int ci = cur.m3 >= CYC_NEARZ, ni = nxt.m3 >= CYC_NEARZ;
        if (ci) out[nout++] = cur;
        if (ci != ni && nout < 4) out[nout++] = ngte_clip_lerp(cur, nxt, CYC_NEARZ);
    }
    if (nout < 3) return;
    cyc_push_tri(out[0], out[1], out[2], gouraud, vw);
    if (nout == 4) cyc_push_tri(out[0], out[2], out[3], gouraud, vw);
}

/* ---- native extraction ---------------------------------------------- */
/* Reads g_Cyclorama + the two matrices + GTE OFX/OFY/H from the hook, and
 * fills s_native[]. Mirrors func_8004EBA8: phase-1 sector visibility cull
 * (cull matrix a1), phase-2 per-vertex unpack + float project (draw matrix
 * a2) + per-face flat/gouraud emit. */
static void cyc_extract_m(CPUState* cpu, uint32_t a0,
                          const int Mcull[3][3], const int Mdraw[3][3],
                          uint32_t fade_rgb, int fade_ir0);
static void cyc_extract(CPUState* cpu, uint32_t a0, uint32_t a1, uint32_t a2,
                        uint32_t fade_rgb, int fade_ir0) {
    int Mcull[3][3], Mdraw[3][3];
    if (!game_layout.cyc.hook_args) {
        /* The draw loads the camera's projection matrix for both the cull
         * and the projection; its registers carry no matrix arguments. */
        a1 = a2 = game_layout.cam.proj_matrix;
    }
    ngte_read_matrix(cpu, a1, Mcull);
    ngte_read_matrix(cpu, a2, Mdraw);
    cyc_extract_m(cpu, a0, Mcull, Mdraw, fade_rgb, fade_ir0);
}

/* The sky visibility bytes (game_layout.cyc.visibility), copied from the
 * scratchpad right after the canonical draw: the game reuses that scratch
 * range later in the tick, so a sub-tick re-extract reads the copy. */
static uint8_t s_cyc_vis[1024];
static uint32_t s_cyc_vis_n = 0;
static void cyc_capture_visibility(CPUState* cpu) {
    s_cyc_vis_n = 0;
    if (!game_layout.cyc.visibility) return;
    uint32_t n = cpu->read_word(game_layout.cyc.sector_count);
    if (n > sizeof s_cyc_vis) n = sizeof s_cyc_vis;
    for (uint32_t i = 0; i < n; i++) s_cyc_vis[i] = cpu->read_byte(game_layout.cyc.visibility + i);
    s_cyc_vis_n = n;
}
static void cyc_extract_m(CPUState* cpu, uint32_t a0,
                          const int Mcull[3][3], const int Mdraw[3][3],
                          uint32_t fade_rgb, int fade_ir0) {
    s_native_n = 0;
    /* One projection view for this pass: offsets, H and the Hor+ bounds. The
     * cull/draw matrices come from the hook's a1/a2, not the view. */
    ngte_view vw; ngte_view_build(cpu, &vw);

    uint32_t sectorCount = cpu->read_word(game_layout.cyc.sector_count);
    uint32_t sectorsBase = cpu->read_word(game_layout.cyc.sectors);
    uint32_t occGroups   = cpu->read_word(game_layout.cyc.occ_groups);

    /* Build the sector-pointer iteration set (mirrors func_8004EBA8 phase 0).
     * In widescreen the .s's per-view occlusion GROUP (occGroups[a0]) is a 4:3
     * selection that omits the dome sectors visible only at the widened sides,
     * so iterate ALL sectors instead (the full 360° dome is in RAM; the
     * per-sector radius cull below + the near-plane clip + the scissor confine
     * it to the visible frustum). Display-only. */
    int ws = vw.extended;   /* Hor+ or a wider FOV */
    uint32_t sectors[1024]; uint32_t nSectors = 0;
    if (sectorCount > 1024) sectorCount = 1024;
    if (game_layout.cyc.visibility) {
        /* The game's visibility pass wrote one byte per sector before the
         * draw (nonzero = drawn), keyed on the camera position against the
         * sky occlusion data: the same 4:3 selection as the group list, so
         * widescreen walks every sector here too. */
        for (uint32_t i = 0; i < sectorCount; i++)
            if (ws || (i < s_cyc_vis_n && s_cyc_vis[i]))
                sectors[nSectors++] = cpu->read_word(sectorsBase + i * 4);
    } else if ((int32_t)a0 < 0 || ws) {
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

    /* A sky draw that depth-cues its own colours: the factor word, when
     * nonzero, is IR0 against the fog far colour for every colour entry. */
    if (!fade_ir0 && game_layout.cyc.fade_in_draw && game_layout.fog.factor) {
        int ir0 = (int)cpu->read_word(game_layout.fog.factor);
        if (ir0) { fade_ir0 = ir0; fade_rgb = ngte_fog_far_colour(cpu); }
    }
    const int s3 = game_layout.cyc.face_layout == CYC_FACES_SPYRO3;

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
            double cull_mac3 = project(Mcull, cull_vx, cull_vy, cull_vz, 0, 0, &vw, &dsx, &dsy, NULL);
            if (cull_mac3 <= (double)radius) continue;
        }

        uint32_t h8 = cpu->read_word(S + 0x08), hC = cpu->read_word(S + 0x0C);
        uint32_t hF = cpu->read_word(S + 0x10);
        int centerX = (int16_t)(h8 & 0xFFFF), centerY = (int16_t)(h8 >> 16);
        int centerZ = (int16_t)(hC >> 16);
        /* Spyro 1: +0x0C low half = vertex count, +0x10 = face count (bits
         * 0-12) and colour-table byte length (>> 14). Spyro 3: +0x0C byte 0 =
         * vertex count, byte 1 = colour count; +0x10 high half = face-word
         * list byte length, low half = index-halfword list byte length. */
        uint32_t vertexCount = s3 ? (hC & 0xFF) : (hC & 0xFFFF);
        uint32_t colorCount  = s3 ? ((hC >> 8) & 0xFF) : (hF >> 14) / 4;
        uint32_t faceCount   = s3 ? 0 : (hF & 0x1FFF);

        if (vertexCount > 4096) vertexCount = 4096;
        uint32_t vertBase  = S + game_layout.cyc.vertex_base;
        uint32_t colorBase = vertBase + vertexCount * 4;       /* s4 */
        uint32_t faceBase  = colorBase + (s3 ? colorCount * 4 : (hF >> 14));

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
        if (colorCount > 4096) colorCount = 4096;
        for (uint32_t j = 0; j < colorCount; j++) {
            uint32_t c = cpu->read_word(colorBase + j * 4) & 0xFFFFFF;
            col[j] = fade_ir0 ? ngte_depth_cue(c, fade_rgb, fade_ir0) : c;
        }

        if (s3) {
            /* phase-2b, strips: one face word per strip (bits 31-24 the
             * third vertex, 23-17 / 16-10 / 9-3 the three colour indices,
             * 2-0 the number of continuation triangles) and a parallel
             * halfword list whose first entry holds the first two vertices
             * ((H >> 8) & 0xFF, H & 0xFF) and whose continuation entries each
             * add one vertex (H & 0xFF) with one colour ((H >> 8) & 0x7F);
             * bit 15 of a continuation shifts the window by one, clear keeps
             * the middle vertex and replaces the first. */
            uint32_t hw = faceBase + (hF >> 16), hwEnd = hw + (hF & 0xFFFF);
            uint32_t fw = faceBase;
            int guard = 0;
            while (hw + 2 <= hwEnd && guard++ < 8192) {
                uint32_t W = cpu->read_word(fw); fw += 4;
                uint32_t H = cpu->read_half(hw); hw += 2;
                uint32_t v0 = (H >> 8) & 0xFF, v1 = H & 0xFF, v2 = (W >> 24) & 0xFF;
                uint32_t k0 = (W >> 17) & 0x7F, k1 = (W >> 10) & 0x7F, k2 = (W >> 3) & 0x7F;
                uint32_t more = W & 7;
                for (;;) {
                    if (v0 < vertexCount && v1 < vertexCount && v2 < vertexCount) {
                        int gouraud = !(k0 == k1 && k0 == k2);
                        uint32_t cc0 = (k0 < colorCount) ? col[k0] : 0;
                        uint32_t cc1 = gouraud ? ((k1 < colorCount) ? col[k1] : 0) : cc0;
                        uint32_t cc2 = gouraud ? ((k2 < colorCount) ? col[k2] : 0) : cc0;
                        ngte_cvert A = { pm1[v0], pm2[v0], pz[v0], cc0 };
                        ngte_cvert B = { pm1[v1], pm2[v1], pz[v1], cc1 };
                        ngte_cvert C = { pm1[v2], pm2[v2], pz[v2], cc2 };
                        cyc_clip_emit(A, B, C, gouraud, &vw);
                    }
                    if (!more || hw + 2 > hwEnd) break;
                    more--;
                    uint32_t H2 = cpu->read_half(hw); hw += 2;
                    if (H2 & 0x8000) { v0 = v1; v1 = v2; k0 = k1; k1 = k2; }
                    else             { v0 = v2; k0 = k2; }
                    v2 = H2 & 0xFF; k2 = (H2 >> 8) & 0x7F;
                }
            }
            continue;
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
            ngte_cvert A = { pm1[i0], pm2[i0], pz[i0], cc0 };
            ngte_cvert B = { pm1[i1], pm2[i1], pz[i1], cc1 };
            ngte_cvert C = { pm1[i2], pm2[i2], pz[i2], cc2 };
            cyc_clip_emit(A, B, C, gouraud, &vw);
        }
    }
}

/* ---- ground-truth capture: decode the prims func_8004EBA8 emitted ---- */
/* Prims are contiguous in [start,end). POLY_F3 = 0x14 bytes
 * {next,rgb,xy0,xy1,xy2}; POLY_G3 = 0x1C bytes
 * {next,rgb0,xy0,rgb1,xy1,rgb2,xy2}. word0 high byte distinguishes them
 * (confirmed by the hex dump below). */
/** @brief Split a packed prim vertex word into signed 16-bit screen X (low half) and Y (high half).
 *
 * @param xy The packed word.
 * @param x Receives X.
 * @param y Receives Y. */
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
/** @brief A triangle's screen-space centroid.
 *
 * No caller in this file uses it; compare_and_report pairs triangles by
 * per-vertex distance.
 * @param t The triangle.
 * @param cx Receives the mean X.
 * @param cy Receives the mean Y. */
static void centroid(const cyc_tri* t, float* cx, float* cy) {
    *cx = (t->v[0].sx + t->v[1].sx + t->v[2].sx) / 3.0f;
    *cy = (t->v[0].sy + t->v[1].sy + t->v[2].sy) / 3.0f;
}
/** @brief Whether a captured GTE vertex lies inside the 512x240 display area with a 1-pixel margin.
 *
 * compare_and_report bins off-screen vertices separately: there the GTE's
 * fixed-point clamps bite and the unclamped float projection departs from
 * them by design.
 * @param v The vertex, in the GTE's integer screen coordinates.
 * @return 1 when -1 <= sx <= 513 and -1 <= sy <= 241, else 0. */
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

/* ---- one-shot format dump ------------------------------------------- */
/** @brief Print the sky draw's arguments, the GTE offsets and H, the cyclorama header, the first sector's layout and the first prim words to stderr.
 *
 * The sector fields are decoded with the Spyro 1 layout (vertex count in the
 * low half of +0x0C, colour-table byte length in +0x10 >> 14, face count in
 * its low 13 bits). At most 32 words of the prim interval are printed.
 * @param cpu The guest CPU state, read only.
 * @param a0 The draw's a0 (occlusion group index).
 * @param a1 The cull matrix address.
 * @param a2 The draw matrix address.
 * @param start Start of the prim interval the draw wrote.
 * @param end End of the prim interval. */
static void hex_dump(CPUState* cpu, uint32_t a0, uint32_t a1, uint32_t a2,
                     uint32_t start, uint32_t end) {
    fprintf(stderr, "[cyc-dump f=%u] a0=%d a1=%08X a2=%08X OFX=%d OFY=%d H=%u\n",
            openpete_frame, (int32_t)a0, a1, a2,
            (int32_t)cpu->gte_ctrl[24] >> 16, (int32_t)cpu->gte_ctrl[25] >> 16,
            cpu->gte_ctrl[26] & 0xFFFF);
    fprintf(stderr, "  g_Cyclorama: count=%u sectors=%08X occCnt=%u occGrp=%08X bg=%08X\n",
            cpu->read_word(game_layout.cyc.sector_count), cpu->read_word(game_layout.cyc.sectors),
            cpu->read_word(game_layout.cyc.occ_group_count), cpu->read_word(game_layout.cyc.occ_groups),
            cpu->read_word(game_layout.cyc.bg_colour));
    uint32_t s0 = cpu->read_word(cpu->read_word(game_layout.cyc.sectors)); /* first sector ptr */
    fprintf(stderr, "  sector0=%08X hdr: ", s0);
    for (int i = 0; i <= 0x14; i += 4) fprintf(stderr, "[%02X]=%08X ", i, cpu->read_word(s0+i));
    fprintf(stderr, "\n  sector0 verts: ");
    for (int i = 0; i < 4; i++) fprintf(stderr, "%08X ", cpu->read_word(s0+game_layout.cyc.vertex_base+i*4));
    uint32_t vc = cpu->read_word(s0+0xC) & 0xFFFF;
    uint32_t cb = s0+game_layout.cyc.vertex_base+vc*4;
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
/** @brief Whether OPENPETE_CYC_VERIFY is set, non-empty and not starting with '0'; read once and cached.
 *
 * @return 1 when the float-versus-GTE sky comparison is armed, else 0. */
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

/** @brief Begin the native scene for this VBL, set its background colour, and push the extracted dome triangles.
 *
 * The PSX cyclorama clears to g_Cyclorama.m_BackgroundColor before drawing
 * the dome, so uncovered areas show that colour. The background is a scene
 * property and is set even when --render-paths excludes the dome, so an
 * isolated path renders over the real background as PsyCross does; only the
 * triangles are gated on RP_CYCLORAMA. PSX colour low-24 is byte0=R,
 * byte1=G, byte2=B, the RHI rgba byte order; alpha is set opaque. Triangles
 * go to the SKY group at the far view Z, untextured and opaque, tagged
 * OPENPETE_TAG_SKY(0). The first non-empty push logs once.
 * @param cpu The guest CPU state, read only. */
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
    uint32_t bg = cpu->read_word(game_layout.cyc.bg_colour) & 0xFFFFFF;
    openpete_native_scene_set_bg(bg | 0xFF000000u);
    if (!render_path_enabled(RP_CYCLORAMA)) return;  /* bg only; dome not selected */
    /* Whole-dome identity: the feeder walks a flat tri list with no
     * region or band structure, so the tag names the channel and nothing
     * finer. Anything per-region has to derive a key from the vertex run
     * first. */
    openpete_native_scene_set_tag(OPENPETE_TAG_SKY(0));
    for (int i = 0; i < s_native_n; i++) {
        openpete_scene_tri t = {0};
        for (int k = 0; k < 3; k++) {
            t.v[k].sx   = s_native[i].v[k].sx;
            t.v[k].sy   = s_native[i].v[k].sy;
            /* sky = the backdrop: always behind depth-tested actors */
            t.v[k].z    = OPENPETE_DEPTH_VIEWZ_FAR;
            t.v[k].pvz  = OPENPETE_DEPTH_VIEWZ_FAR;
            t.v[k].u    = 0.0f; t.v[k].v = 0.0f;
            t.v[k].rgba = s_native[i].v[k].rgb | 0xFF000000u;
        }
        t.textured = 0;   /* cyclorama is gouraud-shaded (no texture) */
        t.tpage_x = t.tpage_y = t.clut_x = t.clut_y = t.bpp = 0;
        t.blend = 0;      /* opaque */
        /* SKY, not WORLD: drawn identically (opaque, depth-tested,
         * first) but without the WORLD group's stencil coverage MARK — the sky
         * showing through a terrain seam is exactly what the LP hole-fill
         * layer must be allowed to repaint. */
        openpete_native_scene_add_tri(&t, OPENPETE_SCENE_SKY, 0);
    }
    openpete_native_scene_set_tag(0);
}

/* Camera-matrix args from the last real render (cyc_extract reads the matrices
 * at a1/a2; they persist in RAM across the tick). Reused by the sub-tick
 * re-extract, which has no register context. */
static uint32_t s_cyc_a0 = 0xFFFFFFFFu, s_cyc_a1 = 0, s_cyc_a2 = 0;

/* The two matrices themselves, snapshotted prev+curr per canonical tick. In
 * gameplay a1/a2 are g_Camera's own view/projection fields, which the spine's
 * camera bracket interpolates in place, so the live read above is already
 * sub-tick. During the level transition (gamestate 1) the game builds both
 * matrices on its STACK each tick — camera yaw minus a decaying spin, roll,
 * pitch, a 320/512-scaled projection copy — and the stack is neither an
 * interpolated region nor stable memory, so a pointer read there returns
 * one tick's matrices for every present of the interval. The re-extract
 * lerps this pair instead (componentwise, like the camera's lerp_matrix):
 * camera motion and spin are both baked into the two endpoints, so both
 * interpolate with no extrapolation. prev is valid only when the preceding
 * canonical tick also drew the dome from the same kind of source. */
static int      s_cyc_prev[2][3][3], s_cyc_curr[2][3][3];
static int      s_cyc_prev_valid = 0, s_cyc_curr_valid = 0;
static int      s_cyc_prev_cam = 0,   s_cyc_curr_cam = 0;   /* source = g_Camera fields */
static unsigned s_cyc_curr_frame = 0xFFFFFFFFu;             /* VBL of the curr capture */

/** @brief Capture the sky draw's argument registers and roll the prev/curr matrix pair before the draw clobbers them.
 *
 * Runs as the hook bracket's pre step. On a VBL other than the last capture's,
 * curr rolls to prev, and prev is valid only when the last capture was at
 * most 3 VBLs earlier. The two matrices at a1/a2 are then read into curr,
 * which records whether they are the camera's own view and projection fields.
 * @param cpu The guest CPU state, read only.
 * @param feed Unused.
 * @param verify Unused. */
static void cyc_pre(CPUState* cpu, int feed, int verify) {
    (void)feed; (void)verify;
    uint32_t a0 = cpu->a0, a1 = cpu->a1, a2 = cpu->a2;
    /* Normalize before the capture: where the sky draw takes no matrix
     * arguments its registers hold whatever the caller left in them, and the
     * sub-tick replay's "a real frame was captured" guard would then test a
     * garbage word: a tick whose stray $a1 happens to be zero reads as "no
     * capture" and the dome is dropped from every present of that interval.
     * The captured triple is the one the extraction uses, on every game. */
    if (!game_layout.cyc.hook_args) a1 = a2 = game_layout.cam.proj_matrix;
    s_cyc_a0 = a0; s_cyc_a1 = a1; s_cyc_a2 = a2;   /* for sub-tick re-extract */
    if (openpete_frame != s_cyc_curr_frame) {
        /* New canonical tick: roll curr→prev. Consecutive = the previous
         * capture was one tick (two VBLs) ago; a longer gap (a load stall,
         * the draw loop pausing) breaks the pair and the re-extract snaps. */
        int consecutive = s_cyc_curr_valid && s_cyc_curr_frame != 0xFFFFFFFFu &&
                          openpete_frame - s_cyc_curr_frame <= 3u;
        memcpy(s_cyc_prev, s_cyc_curr, sizeof s_cyc_prev);
        s_cyc_prev_cam   = s_cyc_curr_cam;
        s_cyc_prev_valid = consecutive;
        s_cyc_curr_frame = openpete_frame;
    }
    ngte_read_matrix(cpu, a1, s_cyc_curr[0]);
    ngte_read_matrix(cpu, a2, s_cyc_curr[1]);
    s_cyc_curr_cam   = !game_layout.cyc.hook_args ||
                       (a1 == game_layout.cam.view_matrix && a2 == game_layout.cam.proj_matrix);
    s_cyc_curr_valid = 1;
}

static hook_bracket_state s_cyc_hb;
static const hook_bracket_desc k_cyc_bracket = {
    .path = RP_CYCLORAMA, .prof_tag = "cyclorama.srend",
    .verify_enabled = cyc_verify_enabled, .pre = cyc_pre,
    .st = &s_cyc_hb,
};

static int on_sky_draw_cyc(CPUState* cpu, uint32_t addr) {
    uint32_t start, end; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_cyc_bracket, &start, &end, &feed, &verify))
        return 0;
    /* cyc_pre stashed the argument triple; the registers are clobbered now. */
    const uint32_t a0 = s_cyc_a0, a1 = s_cyc_a1, a2 = s_cyc_a2;

    cyc_capture_visibility(cpu);

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
    return hook_bracket_end(&k_cyc_bracket);  /* took over (body already ran) */
}

/* Sub-tick sky re-extract; documented at the declaration in cyclorama.h. */
void cyclorama_reextract(CPUState* cpu, double alpha, int interp_on) {
    if (!openpete_native_scene_enabled()) return;
    /* Only replay the sky dome if the canonical frame drew it. In cyclorama-less
     * states (e.g. the black level-name flyby) the saved matrices are stale; the
     * draw-env clear (black) shows through instead. The sky-having transitions
     * (the "ENTERING X" card over a sky) keep RP_CYCLORAMA set, so they're
     * unaffected. */
    if (!render_path_canon_drawn(RP_CYCLORAMA)) return;
    if (s_cyc_a1 == 0 || s_cyc_a2 == 0 || !s_cyc_curr_valid) return;   /* no real frame captured yet */
    if (s_cyc_curr_cam) {
        cyc_extract(cpu, s_cyc_a0, s_cyc_a1, s_cyc_a2, 0, 0);
    } else {
        int do_lerp = interp_on && s_cyc_prev_valid && !s_cyc_prev_cam;
        if (do_lerp) {
            /* One-tick component delta sanity: a continuous transition dome
             * (camera glide + the spin's 2/4096 per tick) moves a few hundred
             * Q12 units per tick at most. */
            long worst = 0;
            for (int m = 0; m < 2; m++)
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) {
                        long d = (long)s_cyc_curr[m][i][j] - s_cyc_prev[m][i][j];
                        if (d < 0) d = -d;
                        if (d > worst) worst = d;
                    }
            if (worst > 1200) do_lerp = 0;
        }
        int M[2][3][3];
        for (int m = 0; m < 2; m++)
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    if (do_lerp) {
                        double v = (double)s_cyc_prev[m][i][j] +
                                   ((double)s_cyc_curr[m][i][j] - s_cyc_prev[m][i][j]) * alpha;
                        M[m][i][j] = (int)(v >= 0.0 ? v + 0.5 : v - 0.5);
                    } else {
                        M[m][i][j] = s_cyc_curr[m][i][j];
                    }
                }
        cyc_extract_m(cpu, s_cyc_a0, M[0], M[1], 0, 0);
    }
    push_scene(cpu);
}

/* ---- the cross-fade dome (func_8004F000); documented in cyclorama.h ---- */
int cyclorama_native_extract_faded(CPUState* cpu, cyclorama_native_tri* out, int max) {
    cyc_extract(cpu, 0xFFFFFFFFu, game_layout.cam.view_matrix, game_layout.cam.proj_matrix,
                cpu->read_word(game_layout.fog.far_colour) & 0xFFFFFFu,
                (int)cpu->read_word(game_layout.fog.factor));
    int n = s_native_n < max ? s_native_n : max;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            out[i].sx[k]  = s_native[i].v[k].sx;
            out[i].sy[k]  = s_native[i].v[k].sy;
            out[i].rgb[k] = s_native[i].v[k].rgb;
        }
    return n;
}

/* ---- present-time draw-env clear sync; documented in orchestrator.h ---- */
void cyclorama_native_env_clear_commit(void) {
    if (!game_layout.db.cur_ptr) return;   /* no draw-buffer pair located */
    const uint32_t curdb = *(const uint32_t*)openpete_subtick_host_ptr(game_layout.db.cur_ptr);
    if (curdb != game_layout.db.base && curdb != game_layout.db.base + game_layout.db.stride)
        return;   /* not a valid DB* yet */
    const uint8_t* env = (const uint8_t*)openpete_subtick_host_ptr(curdb + game_layout.db.drawenv);
    /* PSYQ DRAWENV: isbg @+0x18, r0/g0/b0 @+0x19..0x1B */
    openpete_native_set_clear_color(env[0x18], env[0x19], env[0x1A], env[0x1B]);
}

/* The hook address is the layout's sky-draw site, bound at boot: a static
 * initializer cannot read another TU's const. */
static feature_dispatch_entry_t cyc_dispatch[] = {
    { 0u, on_sky_draw_cyc },
    { 0u, NULL },
};

static void cyc_apply_options(const openpete_options_t* opts) {
    (void)opts;
    cyc_dispatch[0].addr = game_layout.hooks.sky_draw;
}

const feature_module_t cyclorama_module = {
    .name           = "cyclorama_native",
    .apply_options  = cyc_apply_options,
    .dispatch_table = cyc_dispatch,
};
