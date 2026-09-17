// games/common/renderer/ui_sprite.c — native renderer feed for the 2D UI
// sprite/text layer (2D-from-OT).
//
// The 3D scene (terrain, mobys, the player avatar, particles, …) is
// re-derived from gamestate by the other native feeders. The 2D UI — the
// title logo, "PRESS START", HUD sprites, dialog boxes, the level-name
// banner — is a separate layer the game builds as screen-space primitives
// (no GTE), linking each into the shared ordering table through ONE helper
// (ui_game.ot_insert; a0 = primitive pointer). The 3D renderers use an
// INLINED insert, so that helper is the clean 2D chokepoint.
//
// This module hooks it, decodes each 2D primitive it inserts, and feeds it to
// the native scene's HUD overlay layer (OPENPETE_SCENE_HUD — screen-space,
// depth-test off, on top of the world). Textures sample the per-frame VRAM
// mirror the native renderer already uploads, so the font/logo art is
// available for free. Determinism-safe: the hook runs the real routine
// unchanged (the OT link still happens for the PsyCross reference), only
// READING the primitive.
//
// Every address the module reads is a ui_game field
// (games/common/renderer/ui_game.h); a game that fills none registers no
// hook.

#include "feature_registry.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "renderer/render_paths.h"
#include "renderer/native_gte.h"      /* the shared texture-page decode */
#include "renderer/ui_game.h"
#include "openpete_recomp_module.h"   /* the game's own routine, through the module dispatch */
#include "texture_pack.h"
#include "openpete_fb_snapshot.h"
#include "game_layout.h"          /* the draw-buffer pair the retained freeze reads */
#include "openpete_subtick_view.h"   /* PSX RAM through the sub-tick-consistent view */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE — fiber-hook cost split */
#include "renderer/env_knob.h"

extern unsigned openpete_frame;


static unsigned s_cap_frame = 0xFFFFFFFFu;  /* build frame whose batch the last capture released */
static unsigned s_post_claim, s_post_sprt, s_post_fade;

/* Primitives fed, by class: the engagement counters a harness reads to tell
 * "the layer drew nothing" from "the layer drew identically" (a zero count is
 * a vacuous pass, not a clean one). */
static unsigned s_fed_prims, s_fed_sprts;
unsigned ui_sprite_prim_count(void) { return s_fed_prims; }
unsigned ui_sprite_sprt_count(void) { return s_fed_sprts; }

/* OPENPETE_UISPRITE_BBOX=<VBL>: at that VBL, the screen bounding box and count
 * of every 2D class fed, so a capture comparison can be confined to the box an
 * element actually occupies instead of a guessed rectangle. */
enum { UICLS_FT4 = 0, UICLS_BAR, UICLS_PANEL, UICLS_LINE, UICLS_SPRT, UICLS_FADE, UICLS_N };
static const char* const kUiClsName[UICLS_N] = { "ft4", "bar", "panel", "line", "sprt", "fade" };
static struct { float x0, y0, x1, y1; unsigned n; } s_bbox[UICLS_N];
static unsigned s_cls_n[UICLS_N];        /* fed per class since boot (always counted) */
static long bbox_vbl(void) {
    static long v = -2;
    if (v == -2) { const char* e = getenv("OPENPETE_UISPRITE_BBOX"); v = (e && *e) ? atol(e) : -1; }
    return v;
}
static void bbox_note(int cls, float x0, float y0, float x1, float y1) {
    s_cls_n[cls]++;
    if ((long)openpete_frame != bbox_vbl()) return;
    if (!s_bbox[cls].n) { s_bbox[cls].x0 = x0; s_bbox[cls].y0 = y0;
                          s_bbox[cls].x1 = x1; s_bbox[cls].y1 = y1; }
    else { if (x0 < s_bbox[cls].x0) s_bbox[cls].x0 = x0;
           if (y0 < s_bbox[cls].y0) s_bbox[cls].y0 = y0;
           if (x1 > s_bbox[cls].x1) s_bbox[cls].x1 = x1;
           if (y1 > s_bbox[cls].y1) s_bbox[cls].y1 = y1; }
    s_bbox[cls].n++;
}
static void bbox_report(void) {
    if (bbox_vbl() < 0) return;
    static long done = -1;
    if (done == bbox_vbl() || (long)openpete_frame <= bbox_vbl()) return;
    done = bbox_vbl();
    for (int c = 0; c < UICLS_N; c++)
        fprintf(stderr, "[bbox vbl=%ld %s] n=%u x=[%.0f,%.0f] y=[%.0f,%.0f]\n",
                bbox_vbl(), kUiClsName[c], s_bbox[c].n,
                s_bbox[c].x0, s_bbox[c].x1, s_bbox[c].y0, s_bbox[c].y1);
}

/* ---- 2D prim decode + feed ----------------------------------------------- */
/* Every 2D primitive is fed from the deferred flush (see the dispatch
 * section), which runs at the OT walk, where openpete_frame may already be the
 * next VBL. Beginning THAT frame would push the finished scene into the
 * history ring a VBL early and start a new scene holding only this primitive,
 * and the lagged headless compose then reads the wrong slot (the canonical
 * dump jumps a tick). Append to the live scene instead: the primitive belongs
 * to the frame that deferred it. */
static void scene_begin_for_feed(void) {
    unsigned live = openpete_native_scene_frame();
    openpete_native_scene_begin_frame(live != 0xFFFFFFFFu ? live : openpete_frame);
}

/* The scene a claimed batch belongs to: the one its build frame opened, which
 * is the scene the 3D feeders of the same ordering table filled. At the walk it
 * is still the live scene (the next frame's feeders have not run yet), so the
 * batch appends to it; a newer live scene means the build frame's has already
 * been snapshotted and the batch would land a tick late. */

/* Feed one screen-space FT4 (textured quad) prim into the native HUD overlay
 * layer. Layout (verified by the dump): word[1]=rgb|code, [2]=xy0,[3]=uv0|clut,
 * [4]=xy1,[5]=uv1|tpage,[6]=xy2,[7]=uv2,[8]=xy3,[9]=uv3. XY: x=lo16,y=hi16
 * (signed). PSX FT4 corner order TL,TR,BL,BR -> two tris (0,1,2)+(1,3,2). */
static void feed_ft4(CPUState* cpu, uint32_t p, uint32_t w1) {
    int sx[4], sy[4], u[4], v[4];
    uint32_t xyw[4], uvw[4];
    xyw[0]=cpu->read_word(p+0x08); uvw[0]=cpu->read_word(p+0x0C);
    xyw[1]=cpu->read_word(p+0x10); uvw[1]=cpu->read_word(p+0x14);
    xyw[2]=cpu->read_word(p+0x18); uvw[2]=cpu->read_word(p+0x1C);
    xyw[3]=cpu->read_word(p+0x20); uvw[3]=cpu->read_word(p+0x24);
    for (int k=0;k<4;k++){ sx[k]=(int16_t)(xyw[k]&0xFFFF); sy[k]=(int16_t)(xyw[k]>>16);
                           u[k]=(int)(uvw[k]&0xFF); v[k]=(int)((uvw[k]>>8)&0xFF); }
    unsigned clut = (uvw[0]>>16)&0xFFFF, tp = (uvw[1]>>16)&0xFFFF;
    ngte_tpage tpd;
    ngte_decode_tpage(tp, clut, 0u, &tpd);
    int tpx=tpd.tpx, tpy=tpd.tpy, bpp=tpd.bpp;
    int clx=tpd.clx, cly=tpd.cly;
    uint32_t rgba = (w1 & 0xFFFFFFu) | 0xFF000000u;
    int abe = (int)((w1>>24)&0x2u);   /* code bit1 = semi-transparent (ABR mode 0) */

    /* A quad sampling a framebuffer window (a MoveImage'd copy of a draw
     * buffer) reads texels the native render never writes. Redirect it to the
     * snapshot of the native frame taken when the copy was made; the texel box
     * becomes a pixel box in that image. No snapshot -> the stock VRAM path. */
    float snap_su = 1.0f, snap_sv = 1.0f, snap_ox = 0.0f, snap_oy = 0.0f;
    int u_lo = u[0], u_hi = u[0], v_lo = v[0], v_hi = v[0];
    for (int k = 1; k < 4; k++) {
        if (u[k] < u_lo) u_lo = u[k]; if (u[k] > u_hi) u_hi = u[k];
        if (v[k] < v_lo) v_lo = v[k]; if (v[k] > v_hi) v_hi = v[k];
    }
    const int snap = openpete_fb_snapshot_find(tpx, tpy, bpp, u_lo, v_lo, u_hi, v_hi,
                                               &snap_su, &snap_sv, &snap_ox, &snap_oy);

    scene_begin_for_feed();
    static const int idx[2][3] = { {0,1,2}, {1,3,2} };   /* TL,TR,BL,BR -> 2 tris */
    for (int t=0;t<2;t++) {
        openpete_scene_tri tri; memset(&tri,0,sizeof(tri));
        for (int j=0;j<3;j++){ int c=idx[t][j];
            tri.v[j].sx=(float)sx[c]; tri.v[j].sy=(float)sy[c];
            tri.v[j].z=0.0f; tri.v[j].u=(float)u[c]; tri.v[j].v=(float)v[c]; tri.v[j].rgba=rgba; }
        tri.textured=1; tri.tpage_x=tpx; tri.tpage_y=tpy; tri.clut_x=clx; tri.clut_y=cly;
        tri.bpp=bpp; tri.blend = abe ? 1 : 0;
        if (snap >= 0) {
            tri.tpage_x = snap; tri.tpage_y = 0; tri.clut_x = 0; tri.clut_y = 0;
            tri.bpp = OPENPETE_FB_SNAPSHOT_BPP;
            for (int j=0;j<3;j++) {
                tri.v[j].u = snap_ox + tri.v[j].u * snap_su;
                tri.v[j].v = snap_oy + tri.v[j].v * snap_sv;
            }
        }
        openpete_native_scene_add_tri(&tri, OPENPETE_SCENE_HUD, 1);
    }
    bbox_note(UICLS_FT4, (float)sx[0], (float)sy[0], (float)sx[3], (float)sy[3]);
    s_fed_prims++;
}

/* Feed one screen-space untextured POLY_F4 (flat quad) into a native HUD-OT layer.
 * Two consumers:
 *   - the cutscene letterbox: two full-width OPAQUE bars (code 0x28) ->
 *     OPENPETE_SCENE_OVERLAY (dead last, over everything), alpha 0xFF;
 *   - a dark panel (pause/inventory panel, HUD shaded box, dialog-box slab):
 *     a SEMI-TRANSPARENT quad (code 0x2A) -> OPENPETE_SCENE_HUD_PANEL (under
 *     the text, over the world/backdrop), blended by the tracked ABR.
 * Layout: word[1]=code|rgb, [2]=xy0,[3]=xy1,[4]=xy2,[5]=xy3 (NO interleaved UV — F4
 * packs the 4 XY words consecutively, unlike FT4). XY: x=lo16,y=hi16 signed. Corners
 * TL,TR,BL,BR. `category`/`alpha`/`blend` select the layer + transparency. */
static void feed_f4(CPUState* cpu, uint32_t p, uint32_t w1, int category, uint32_t alpha, int blend,
                    int full_width) {
    int sx[4], sy[4];
    uint32_t xyw[4];
    xyw[0]=cpu->read_word(p+0x08); xyw[1]=cpu->read_word(p+0x0C);
    xyw[2]=cpu->read_word(p+0x10); xyw[3]=cpu->read_word(p+0x14);
    for (int k=0;k<4;k++){ sx[k]=(int16_t)(xyw[k]&0xFFFF); sy[k]=(int16_t)(xyw[k]>>16); }
    /* Widescreen: a FULL-SCREEN bar (the cutscene letterbox) is authored at the PSX
     * 4:3 width (x 0..512), which scene_to_clip maps to the centred 4:3 inset — so
     * in a wide window the bars stop at the inset and the revealed side strips show
     * the world at top/bottom. Widen such a bar about screen-centre 256 by the Hor+
     * factor F so x 0->xlo and 512->xhi map to the full panel [-1,+1] (same vertical
     * extent — the widened bar stays continuous with the central one). No-op at 4:3
     * (F=1). Vertical bars are unchanged.
     *
     * Over-extend each side edge a few PSX px PAST the content rect: the exact
     * widened edge lands sub-pixel-precisely on clip ±1, where the rasteriser drops
     * a 1px sliver along the window border (the "letterbox doesn't reach the edge"
     * report). The scene scissor clips the overshoot, so the bleed is never visible
     * beyond the edge — it only guarantees the border column is covered. */
    if (full_width) {
        float F = openpete_native_widescreen_factor();
        if (F > 1.0f) for (int k=0;k<4;k++) {
            const float kBleed = 4.0f;   /* PSX px of edge bleed (scissor-clipped) */
            float w = ((float)sx[k] - 256.0f) * F;
            if (w < 0.0f)      w -= kBleed;
            else if (w > 0.0f) w += kBleed;
            sx[k] = 256 + (int)w;
        }
    }
    uint32_t rgba = (w1 & 0xFFFFFFu) | (alpha << 24);

    scene_begin_for_feed();
    static const int idx[2][3] = { {0,1,2}, {1,3,2} };   /* TL,TR,BL,BR -> 2 tris */
    for (int t=0;t<2;t++) {
        openpete_scene_tri tri; memset(&tri,0,sizeof(tri));
        for (int j=0;j<3;j++){ int c=idx[t][j];
            tri.v[j].sx=(float)sx[c]; tri.v[j].sy=(float)sy[c];
            tri.v[j].z=0.0f; tri.v[j].rgba=rgba; }
        tri.textured=0; tri.blend=blend;
        openpete_native_scene_add_tri(&tri, category, 0);
    }
    bbox_note(category == OPENPETE_SCENE_OVERLAY ? UICLS_BAR : UICLS_PANEL,
              (float)sx[0], (float)sy[0], (float)sx[3], (float)sy[3]);
    s_fed_prims++;
}

/* Feed one screen-space gouraud line (LINE_G2, code 0x50) into the native HUD
 * layer as a thin gouraud quad (the scene is triangles-only, like the particle/
 * sparkle line feeds). The consumers are the UI separator lines (the
 * horizontal rules on a results screen, gem-UI dividers) and the dialog name
 * underline, each with a per-endpoint shimmer colour. Layout: word[1]=code|rgb0,
 * [2]=xy0, [3]=pad|rgb1, [4]=xy1. XY: x=lo16,y=hi16 signed. */
static void feed_line_g2(CPUState* cpu, uint32_t p, uint32_t w1) {
    uint32_t xy0 = cpu->read_word(p + 0x08);
    uint32_t c1  = cpu->read_word(p + 0x0C);
    uint32_t xy1 = cpu->read_word(p + 0x10);
    double x0 = (double)(int16_t)(xy0 & 0xFFFF), y0 = (double)(int16_t)(xy0 >> 16);
    double x1 = (double)(int16_t)(xy1 & 0xFFFF), y1 = (double)(int16_t)(xy1 >> 16);
    uint32_t rgba0 = (w1 & 0xFFFFFFu) | 0xFF000000u;   /* endpoint-0 colour */
    uint32_t rgba1 = (c1 & 0xFFFFFFu) | 0xFF000000u;   /* endpoint-1 colour */
    /* perpendicular half-width (~0.6 PSX px each side -> a ~1px-thin line). */
    double dx = x1 - x0, dy = y1 - y0;
    double len = sqrt(dx*dx + dy*dy); if (len < 1e-3) len = 1.0;
    double px = -dy / len * 0.6, py = dx / len * 0.6;

    scene_begin_for_feed();
    /* corners: a=+perp, b=-perp; colour follows the endpoint (gouraud). */
    float cx[4] = {(float)(x0+px),(float)(x1+px),(float)(x0-px),(float)(x1-px)};
    float cy[4] = {(float)(y0+py),(float)(y1+py),(float)(y0-py),(float)(y1-py)};
    uint32_t cc[4] = { rgba0, rgba1, rgba0, rgba1 };
    static const int idx[2][3] = { {0,1,2}, {1,3,2} };
    for (int t=0;t<2;t++) {
        openpete_scene_tri tri; memset(&tri,0,sizeof(tri));
        for (int j=0;j<3;j++){ int c=idx[t][j];
            tri.v[j].sx=cx[c]; tri.v[j].sy=cy[c]; tri.v[j].z=0.0f; tri.v[j].rgba=cc[c]; }
        tri.textured=0; tri.blend=0;
        openpete_native_scene_add_tri(&tri, OPENPETE_SCENE_HUD, 1);
    }
    bbox_note(UICLS_LINE, (float)x0, (float)y0, (float)x1, (float)y1);
    s_fed_prims++;
}

/* Feed one screen-space SPRT (a textured sprite rectangle) into the native
 * HUD layer as two triangles. An SPRT carries no texture page of its own: the
 * page comes from the DR_MODE that heads the chain it was emitted in, which
 * the caller passes. Layout: word[1]=code|rgb, [2]=xy0 (x=lo16, y=hi16,
 * signed), [3]=u0|v0|clut (u at byte 0, v at byte 1, CLUT in the high half),
 * [4]=(h<<16)|w. Code bit 1 = semi-transparent; bit 0 = raw texture (the
 * modulation colour is ignored and the texel passes through), which the
 * renderer reproduces by feeding white. */
/* Decode one SPRT into the scene. Called from the batch release, never at
 * emission: a glyph is ranked by the same table walk as everything else. */
static void feed_sprt_now(CPUState* cpu, uint32_t prim, unsigned tpage) {
    uint32_t w1 = cpu->read_word(prim + 0x04);
    uint32_t xy = cpu->read_word(prim + 0x08);
    uint32_t uv = cpu->read_word(prim + 0x0C);
    uint32_t wh = cpu->read_word(prim + 0x10);
    int x0 = (int16_t)(xy & 0xFFFF), y0 = (int16_t)(xy >> 16);
    int u0 = (int)(uv & 0xFFu),      v0 = (int)((uv >> 8) & 0xFFu);
    unsigned clut = (uv >> 16) & 0xFFFFu;
    int w = (int)(wh & 0xFFFFu),     h = (int)((wh >> 16) & 0xFFFFu);
    if (w <= 0 || h <= 0) return;

    int code = (int)((w1 >> 24) & 0xFFu);
    uint32_t rgba = (code & 1) ? 0xFF808080u                 /* raw texture: no modulation */
                               : ((w1 & 0xFFFFFFu) | 0xFF000000u);
    int abe = code & 2;
    ngte_tpage tpd;
    ngte_decode_tpage((unsigned)tpage, (unsigned)clut, 0u, &tpd);
    int tpx = tpd.tpx, tpy = tpd.tpy, bpp = tpd.bpp;
    int clx = tpd.clx, cly = tpd.cly;

    float sx[4] = { (float)x0, (float)(x0 + w), (float)x0, (float)(x0 + w) };
    float sy[4] = { (float)y0, (float)y0, (float)(y0 + h), (float)(y0 + h) };
    float su[4] = { (float)u0, (float)(u0 + w), (float)u0, (float)(u0 + w) };
    float sv[4] = { (float)v0, (float)v0, (float)(v0 + h), (float)(v0 + h) };

    openpete_native_scene_begin_frame(openpete_frame);   /* fed at build time, not from the batch */
    static const int idx[2][3] = { {0,1,2}, {1,3,2} };
    for (int t = 0; t < 2; t++) {
        openpete_scene_tri tri; memset(&tri, 0, sizeof(tri));
        for (int j = 0; j < 3; j++) { int c = idx[t][j];
            tri.v[j].sx = sx[c]; tri.v[j].sy = sy[c]; tri.v[j].z = 0.0f;
            tri.v[j].u = su[c]; tri.v[j].v = sv[c]; tri.v[j].rgba = rgba; }
        tri.textured = 1; tri.tpage_x = tpx; tri.tpage_y = tpy;
        tri.clut_x = clx; tri.clut_y = cly; tri.bpp = bpp; tri.blend = abe ? 1 : 0;
        openpete_native_scene_add_tri(&tri, OPENPETE_SCENE_HUD, 1);
    }
    bbox_note(UICLS_SPRT, sx[0], sy[0], sx[3], sy[3]);
    s_fed_prims++; s_fed_sprts++;
}

/* Unclaimed 2D primitives, by GP0 command byte. The chain below claims a
 * closed list of encodings; anything else is linked for the reference and
 * never fed, so it is missing from the native frame. This histogram names the
 * codes the game actually links that the chain does not decode — the
 * engagement counter for adding a decoder (nonzero before, zero after).
 * Counted always (one increment); printed per tick under
 * OPENPETE_UISPRITE_CENSUS, the tick's line emitted at the first insert of
 * the NEXT tick, so a line holds a whole tick's unclaimed prims. */
static unsigned s_unclaimed[256];        /* since boot */
static unsigned s_unclaimed_tick[256];   /* current tick */
static unsigned s_unclaimed_frame = 0xFFFFFFFFu;
unsigned ui_sprite_unclaimed_count(void) {
    unsigned n = 0; for (int c = 0; c < 256; c++) n += s_unclaimed[c]; return n;
}
static void unclaimed_flush_tick(void) {
    static int cen = -1;
    ENV_FLAG_LATCH(cen, "OPENPETE_UISPRITE_CENSUS");
    if (!cen || s_unclaimed_frame == 0xFFFFFFFFu) return;
    char line[512]; int o = 0; int any = 0;
    for (int c = 0; c < 256 && o < (int)sizeof(line) - 24; c++) {
        if (!s_unclaimed_tick[c]) continue;
        any = 1;
        o += snprintf(line + o, sizeof(line) - (size_t)o, " %02X=%u", c, s_unclaimed_tick[c]);
    }
    if (any)
        fprintf(stderr, "[census f=%u UI-UNCLAIMED]%s\n", s_unclaimed_frame, line);
    memset(s_unclaimed_tick, 0, sizeof(s_unclaimed_tick));
}
static void unclaimed_note(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return;
    unsigned code = (unsigned)((cpu->read_word(prim + 4) >> 24) & 0xFFu);
    s_unclaimed[code]++; s_unclaimed_tick[code]++;
}

/* ---- dispatch ------------------------------------------------------------
 * A 2D primitive's words are NOT final when it is linked. Two callers rewrite
 * a linked primitive: a title-screen sprite draw mirrors a sprite by swapping
 * its U coords right after the insert returns (the right-hand memory-card
 * border), and the atlas page turn skews a whole page of already-linked quads
 * a tick after linking them (the quads carry the turning sheet; read at link
 * time every one of them is still the flat page). The words ARE final when the
 * ordering table holding them is handed to the GPU.
 *
 * So the chokepoint CLAIMS each primitive (records its address and kind) and
 * the whole batch is fed at the OT walk, in link order. The primitives linked
 * between two DrawOTag calls are exactly one ordering table's worth, and the
 * second call is the one that draws them, so `ui_sprite_ot_draw_begin` is both
 * the point where every word is final and the point where the batch is
 * complete. Feeding in link order keeps the native layering equal to the OT
 * walk order. The chokepoint is the clean 2D-vs-3D discriminator (the 3D
 * renderers inline the insert). */
enum { QK_FT4 = 0, QK_FREEZE, QK_DR_MODE, QK_F4_BAR, QK_F4_PANEL, QK_LINE_G2, QK_SPRT };
typedef struct { uint32_t addr; uint32_t sum; uint32_t seq; uint32_t aux; uint32_t xy_link; uint32_t xy_rel; uint8_t kind; } ui_claim_t;

/* One drawn tick's 2D. Spyro 3's atlas links ~370 quads per tick, the widest
 * screen in either game; the cap is well clear of that and an overflow is
 * counted rather than silently dropped. */
#define UI_CLAIM_MAX 2048
static ui_claim_t s_claim[UI_CLAIM_MAX];
static unsigned s_claim_n;            /* claimed, awaiting the OT walk */
static unsigned s_claim_frame;        /* openpete_frame the batch's first claim was made in */
static unsigned s_claim_lost;         /* claims dropped on overflow (engagement: 0) */
static int      s_ot_ranked;          /* 2D tris the table walk ranked at the last release: the
                                       * engagement counter for the traversal rank (0 means the
                                       * walk reached none of this frame's 2D) */
static unsigned s_claim_late;         /* batches released at the frame close rather than at the
                                       * ordering-table walk (engagement counter for that hook) */
/* 2D emissions of a frame that arrive AFTER that frame's canonical capture has
 * released its batch. The capture is the release point on the assumption that
 * it is the frame's last 2D emission; these counters test that, per emitter.
 * Only emissions stamped with the SAME frame as the batch the capture released
 * count — an emission carrying a later frame belongs to the next frame and is
 * not evidence of anything. Printed once per capture under
 * OPENPETE_UISPRITE_TRACE, zeros included: a zero line is the evidence that
 * nothing of that frame follows its capture. */
static void post_capture_report(void) {
    static int tr = -1;
    ENV_FLAG_LATCH(tr, "OPENPETE_UISPRITE_TRACE");
    if (tr && s_cap_frame != 0xFFFFFFFFu)
        fprintf(stderr, "[uipost built=%u] claim=%u sprt=%u fade=%u\n",
                s_cap_frame, s_post_claim, s_post_sprt, s_post_fade);
    s_post_claim = s_post_sprt = s_post_fade = 0;
}

static unsigned s_claim_at_capture;   /* batches released at their own frame's canonical capture:
                                       * the in-phase release, and the one that should carry
                                       * every batch on a frame that composes an image */
/* Verification read. A batch is released inside its own frame, before the game
 * has handed the ordering table holding it to the GPU. The walk is where the
 * words are beyond doubt final, so the released batch is re-read there and
 * compared with what was fed: `s_verify_stale` counts primitives whose words
 * changed after the release (must be 0 — a nonzero count means a store lands
 * after the capture and the release point is too early), against
 * `s_verify_n` primitives checked. */
static ui_claim_t s_verify[UI_CLAIM_MAX];
static unsigned s_verify_pending;     /* entries awaiting their walk */
static unsigned s_verify_n, s_verify_stale;
static unsigned s_mutated;            /* primitives whose words changed between the claim
                                       * and the flush: the deferral's engagement counter
                                       * (0 means no caller ever rewrites a linked prim
                                       * and the deferral is buying nothing) */
static CPUState* s_cpu = NULL;        /* the game CPU (stable) — for the OT-walk flush */
static int      s_in_fade = 0;        /* set across the fade emitter's real-function call so
                                       * the chokepoint hook can tell the fade's 0x2A F4 from
                                       * a dark panel's (both go through the chokepoint) */
static int      s_last_abr = 0;       /* semi-transparency rate (ABR) of the most recent
                                       * DR_MODE seen — the GPU draw-mode in effect for the
                                       * next semi-transparent F4 (the game links a box's
                                       * DR_MODE immediately before its 0x2A F4). PSX ABR:
                                       * 0 (B+F)/2, 1 B+F, 2 B-F, 3 B+F/4. */

/* Fold the words a feeder reads into one value, so the release can tell a
 * rewritten primitive from an untouched one without keeping every word. Words
 * 1..9 cover the longest encoding claimed here (POLY_FT4: the code+colour word
 * and the four xy/uv pairs). Word 0 is EXCLUDED: its low 24 bits are the
 * ordering table's link pointer, rewritten whenever another primitive is
 * linked in front of this one, which is not a change to what is drawn. Its top
 * byte, the tag length, is folded in — that is the byte the decode chain
 * reads. */
static uint32_t claim_sum(CPUState* cpu, uint32_t prim) {
    uint32_t h = 2166136261u;
    h = (h ^ (cpu->read_word(prim) >> 24)) * 16777619u;
    for (int k = 1; k < 10; k++) h = (h ^ cpu->read_word(prim + (uint32_t)k * 4u)) * 16777619u;
    return h;
}

static int is_ft4(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return ((uint8_t)(w1 >> 24) & 0xFD) == 0x2C   /* 0x2C / 0x2E = POLY_FT4 */
        && (uint8_t)(w0 >> 24) >= 9;              /* tag length >= 9 (full FT4) */
}

/* Opaque POLY_F4 (0x28) — the cutscene letterbox bars. */
static int is_f4_opaque(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return (uint8_t)(w1 >> 24) == 0x28            /* POLY_F4 opaque */
        && (uint8_t)(w0 >> 24) >= 5;              /* tag length >= 5 (full F4) */
}

/* Semi-transparent POLY_F4 (0x2A) — a dark panel: the pause/inventory panel,
 * the HUD shaded box, a dialog-box slab. Same code as the full-screen fade,
 * BUT the fade comes from ui_game.fade_emitter (excluded via s_in_fade, set
 * across that hook's real-function call). So a 0x2A F4 seen with s_in_fade
 * clear is a panel; with it set, it is the fade (already handled). */
static int is_f4_semitrans(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return (uint8_t)(w1 >> 24) == 0x2A            /* POLY_F4 semi-transparent */
        && (uint8_t)(w0 >> 24) >= 5;              /* tag length >= 5 (full F4) */
}

/* DR_MODE / DR_TPAGE (GP0 command 0xE1) — sets the GPU draw mode, incl. the
 * semi-transparency RATE (ABR) in bits 5-6 of the tpage word (getTPage packs
 * abr as ((abr&3)<<5)). A 2D box links its DR_MODE through the chokepoint
 * immediately before its 0x2A F4, so the latest DR_MODE's ABR is the blend
 * mode in effect for the next F4. The game's own GPU state is mirrored here
 * rather than a blend hardcoded. code[0] is at +4 (the tag occupies +0), so
 * the GP0 byte and the ABR live in read_word(prim+4). */
static int is_dr_mode(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    return (uint8_t)(cpu->read_word(prim + 4) >> 24) == 0xE1;   /* code[0] = GP0 0xE1 */
}

/* Freeze-frame overlay backdrop FT4 (pause / inventory / flight results): the
 * pause renderer desaturates the frozen frame to grayscale and re-displays it
 * as 4 full-screen POLY_FT4 sampling a VRAM scratch copy, each with a flat
 * tint and the grayscale CLUT at a fixed VRAM position. That tint+CLUT pair is
 * the unambiguous signature (the title logo is also 8bpp at tpage x=512 but
 * white-tinted with its own CLUT). The native renderer reproduces the overlay
 * from its OWN last world frame (set_freeze_backdrop) and skips feeding this
 * FT4, so the overlay shows the native render rather than the emulated VRAM. */
static int is_freeze_backdrop(CPUState* cpu, uint32_t prim) {
    if (!ui_game.freeze_tint) return 0;                   /* no such quad on this game */
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    if (((uint8_t)(w1 >> 24) & 0xFD) != 0x2C) return 0;   /* POLY_FT4 (0x2C/0x2E) */
    if ((uint8_t)(w0 >> 24) < 9) return 0;                /* full FT4 tag */
    if ((w1 & 0xFFFFFFu) != ui_game.freeze_tint) return 0;/* freeze tint (B,G,R) */
    uint32_t clut = (cpu->read_word(prim + 0x0C) >> 16) & 0xFFFFu;
    int clx = (int)(clut & 0x3Fu) * 16, cly = (int)((clut >> 6) & 0x1FFu);
    return clx == ui_game.freeze_clut_x && cly == ui_game.freeze_clut_y;
}

/* LINE_G2 (gouraud 2-point line, code 0x50) — UI separator lines and the
 * dialog name underline. (A particle streak LINE_G goes through the particle
 * path, not the chokepoint, so there is no overlap.) */
static int is_line_g2(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return (uint8_t)(w1 >> 24) == 0x50            /* LINE_G2 */
        && (uint8_t)(w0 >> 24) >= 4;              /* tag length >= 4 (full LINE_G2) */
}


/* The ordering table's own traversal, as an address-to-rank map.
 *
 * The GPU draws a 2D primitive when the table walk reaches it, and the walk is
 * a flat singly-linked list: each tag's low 24 bits point at the next
 * primitive, 0x00FFFFFF ends it. AddPrim links a primitive at the HEAD of its
 * entry, so within an entry the last-linked primitive is drawn FIRST — the
 * order a primitive is emitted in is not the order it is drawn in, and no
 * counter kept at emission can stand in for the walk. Reproduce the walk itself
 * (the game's own list, not a proxy for it).
 *
 * The table being fed is walked by the game a tick after this runs, so both of
 * the recent bases are tried and the one whose chain holds more of this frame's
 * 2D wins. Every primitive in the chain is recorded, not just the batch's, so a
 * glyph fed at emission and a quad fed from the deferred batch rank against
 * each other. */
#define PSYQ_OT_BASE_RING 8
#define UI_OT_WALK_MAX 16384
/* PSX main RAM. A tag's next pointer is 24 bits of a word the game is still
 * writing when this runs, so it can name anything; only an address that is a
 * word inside RAM may be followed. */
#define UI_RAM_LO 0x80000000u
#define UI_RAM_HI 0x801FFFFCu
static int ui_ram_word(uint32_t a) { return a >= UI_RAM_LO && a <= UI_RAM_HI && !(a & 3u); }

static uint32_t s_walk_addr[UI_OT_WALK_MAX];   /* sorted by address */
static uint32_t s_walk_rank[UI_OT_WALK_MAX];   /* traversal position of that address */
static int      s_walk_n;
static int      s_walk_hits;                   /* tris the map ranked at the last release:
                                                * the engagement counter (0 = the walk found
                                                * none of this frame's 2D) */

/* `known` is a sorted list of primitive addresses this frame's 2D was decoded
 * from; a candidate chain is scored by how many of them it contains, so the 2D
 * table is picked out of the frame's several walks rather than assumed. */
static int known_has(const uint32_t* known, int kn, uint32_t a) {
    int lo = 0, hi = kn - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2;
        if (known[mid] == a) return 1;
        if (known[mid] < a) lo = mid + 1; else hi = mid - 1; }
    return 0;
}

/* Rank over the UNION of every recent chain that holds any of this frame's 2D,
 * taken in DrawOTag ISSUE order (oldest ring entry first), because the game may
 * split its 2D across more than one table and a single "best" chain then leaves
 * the others' primitives unranked. An address seen in an earlier chain keeps
 * that rank. */
static void build_ot_rank_map(CPUState* cpu, const uint32_t* known, int kn) {
    s_walk_n = 0;
    static int pz = -1;
    ENV_FLAG_LATCH(pz, "OPENPETE_OT_RING_PROBE");
    for (int b = PSYQ_OT_BASE_RING - 1; b >= 0; b--) {      /* issue order */
        uint32_t base = psyq_ot_recent_base(b);
        if (!ui_ram_word(base)) continue;
        int len = 0, score = 0, added = 0;
        uint32_t addr = base;
        for (int step = 0; step < UI_OT_WALK_MAX && ui_ram_word(addr); step++) {
            len++;
            if (known_has(known, kn, addr)) {
                score++;
                if (s_walk_n < UI_OT_WALK_MAX) {            /* first chain to hold it wins */
                    int dup = 0;
                    for (int i = 0; i < s_walk_n && !dup; i++) if (s_walk_addr[i] == addr) dup = 1;
                    if (!dup) { s_walk_addr[s_walk_n] = addr;
                                s_walk_rank[s_walk_n] = (uint32_t)(s_walk_n + 1); s_walk_n++; added++; }
                }
            }
            uint32_t next = cpu->read_word(addr) & 0x00FFFFFFu;
            if (next == 0x00FFFFFFu) break;
            addr = next | 0x80000000u;
            if (!ui_ram_word(addr)) break;
        }
        if (pz) fprintf(stderr, "[otring f=%u] slot=%d base=%08X len=%d hits=%d added=%d\n",
                        openpete_frame, b, base, len, score, added);
    }
    for (int i = 1; i < s_walk_n; i++) {          /* sort by address for the lookup */
        uint32_t a = s_walk_addr[i], r = s_walk_rank[i];
        int j = i - 1;
        while (j >= 0 && s_walk_addr[j] > a) { s_walk_addr[j+1] = s_walk_addr[j];
                                               s_walk_rank[j+1] = s_walk_rank[j]; j--; }
        s_walk_addr[j+1] = a; s_walk_rank[j+1] = r;
    }
    if (pz) fprintf(stderr, "[otring f=%u] union=%d of known=%d\n", openpete_frame, s_walk_n, kn);
}

static uint32_t ot_rank_of_quiet(uint32_t prim) {
    int lo = 0, hi = s_walk_n - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2;
        if (s_walk_addr[mid] == prim) return s_walk_rank[mid];
        if (s_walk_addr[mid] < prim) lo = mid + 1; else hi = mid - 1; }
    return 0u;
}

static uint32_t ot_rank_of(uint32_t prim) {
    int lo = 0, hi = s_walk_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_walk_addr[mid] == prim) { s_walk_hits++; return s_walk_rank[mid]; }
        if (s_walk_addr[mid] < prim) lo = mid + 1; else hi = mid - 1;
    }
    return 0u;   /* not in the chain: keeps its feed position under the stable sort */
}

/* Feed the claimed batch: the ordering table about to be walked. Every word is
 * final here, and the entries are in link order, which is the OT walk order. */
/* Decode one claimed primitive into the scene. */
static void feed_one_claim(CPUState* cpu, uint32_t prim, int kind, uint32_t aux) {
    (void)aux;
    const uint32_t w1 = cpu->read_word(prim + 4);
    switch (kind) {
    case QK_FT4:    feed_ft4(cpu, prim, w1); break;
    case QK_FREEZE:
        /* Pause / inventory / flight-results green overlay: drive the native
         * freeze backdrop from our own last world frame and DROP this FT4 (it
         * samples the emulated VRAM scratch). Tint = the FT4's setRGB0 (B,G,R
         * packed in w1) — r=lo byte, g=mid, b=hi. */
        scene_begin_for_feed();
        openpete_native_scene_set_freeze_backdrop((int)(w1 & 0xFFu),
                              (int)((w1 >> 8) & 0xFFu),
                              (int)((w1 >> 16) & 0xFFu),
                              0);   /* static: no fade-to-black */
        s_fed_prims++;
        break;
    case QK_DR_MODE: s_last_abr = (int)((w1 >> 5) & 3u); break;   /* the GPU draw-mode's ABR */
    case QK_F4_BAR:  feed_f4(cpu, prim, w1, OPENPETE_SCENE_OVERLAY, 0xFFu, 0, 1); break;
    case QK_F4_PANEL: {
        /* The box links its own DR_MODE immediately before the quad, so the
         * ABR tracked at this point of the batch is the blend the GPU is in
         * when the quad draws. ABR 2 (B-F) DARKENS the framebuffer by the
         * quad's colour and is rendered subtractive (blend 3); ABR 0
         * ((B+F)/2) is a halving and is rendered at alpha 0x80. Modes 1
         * (B+F) and 3 (B+F/4) fall back to alpha — add an additive HUD pipe
         * when a consumer appears. */
        int sub = (s_last_abr == 2);
        feed_f4(cpu, prim, w1, OPENPETE_SCENE_HUD_PANEL,
            sub ? 0xFFu : 0x80u, sub ? 3 : 1, 0);
        break; }
    case QK_LINE_G2: feed_line_g2(cpu, prim, w1); break;
    default: break;
    }
}

/* Release `n` of the claimed batch, keeping the rest for a later release. The
 * canonical capture keeps the batch's LAST primitive back: the game links a
 * primitive and fills it afterwards, so at the capture every primitive but the
 * one linked last is complete, and the last one still holds the previous
 * content of its polybuf slot. It is released at the frame close, which is
 * still inside the scene that built it. */
static void release_batch(unsigned n) {
    CPUState* cpu = s_cpu;
    if (n > s_claim_n) n = s_claim_n;
    if (!cpu || !n || !openpete_native_scene_enabled()) { s_claim_n -= n; return; }
    /* Which primitives are released is decided in CLAIM order: the capture keeps
     * the last-LINKED one back, because that is the one the game has not filled
     * yet. What order they are COMPOSED in is decided after the feed, by the
     * table walk below, over every 2D tri of the frame at once. */
    static int s_tr = -1;
    ENV_FLAG_LATCH(s_tr, "OPENPETE_UISPRITE_TRACE");
    for (unsigned i = 0; i < n; i++) {
        const uint32_t prim = s_claim[i].addr;
        openpete_native_scene_set_prim(prim);   /* ranked by the table walk below */
        const uint32_t sum = claim_sum(cpu, prim);
        if (sum != s_claim[i].sum) s_mutated++;
        { unsigned vi = s_verify_pending + i;
          if (vi < UI_CLAIM_MAX) { s_verify[vi].addr = prim; s_verify[vi].sum = sum;
                                   s_verify[vi].kind = s_claim[i].kind;
                                   s_verify[vi].xy_link = s_claim[i].xy_link;
                                   s_verify[vi].xy_rel  = cpu->read_word(prim + 0x08u); } }
        const uint32_t w1 = cpu->read_word(prim + 4);
        feed_one_claim(cpu, prim, s_claim[i].kind, s_claim[i].aux);
    }
    openpete_native_scene_set_prim(0u);
    /* One walk of the table, then rank every 2D tri of the frame by the address
     * it came from — the batch just fed and the glyphs fed at emission alike. */
    s_walk_hits = 0;
    { static uint32_t known[UI_CLAIM_MAX * 2];
      int kn = openpete_native_scene_collect_2d_prims(known, (int)(sizeof known / sizeof known[0]));
      for (int i = 1; i < kn; i++) {            /* sorted for the chain scoring */
          uint32_t a = known[i]; int j = i - 1;
          while (j >= 0 && known[j] > a) { known[j+1] = known[j]; j--; }
          known[j+1] = a; }
      build_ot_rank_map(cpu, known, kn); }
    openpete_native_scene_rank_2d(ot_rank_of);
    s_ot_ranked = s_walk_hits;
    { static int pz2 = -1; ENV_FLAG_LATCH(pz2, "OPENPETE_OT_RING_PROBE");
      if (pz2) { static uint32_t kn2[UI_CLAIM_MAX * 2];
        int k2 = openpete_native_scene_collect_2d_prims(kn2, (int)(sizeof kn2 / sizeof kn2[0]));
        for (int i = 0; i < k2; i++) if (!ot_rank_of_quiet(kn2[i]))
            fprintf(stderr, "[otmiss f=%u] prim=%08X words=%08X %08X %08X\n", openpete_frame, kn2[i],
                    cpu->read_word(kn2[i]), cpu->read_word(kn2[i]+4), cpu->read_word(kn2[i]+8)); } }
    { static int tr2 = -1; ENV_FLAG_LATCH(tr2, "OPENPETE_UISPRITE_TRACE");
      if (tr2) fprintf(stderr, "[uirank f=%u] chain=%d ranked=%d\n",
                       openpete_frame, s_walk_n, s_walk_hits); }
    /* Carry the unreleased tail to the front for its own release. */
    s_claim_n -= n;
    for (unsigned i = 0; i < s_claim_n; i++) s_claim[i] = s_claim[i + n];
    s_verify_pending += n;
    if (s_tr)
        fprintf(stderr, "[uifeed f=%u built=%u live=%u] rel=%u kept=%u mutated=%u lost=%u late=%u cap=%u fed=%u\n",
                openpete_frame, s_claim_frame, openpete_native_scene_frame(), n, s_claim_n,
                s_mutated, s_claim_lost, s_claim_late, s_claim_at_capture, s_fed_prims);
}

/* Release everything still claimed: the ordering-table walk and the close of
 * the frame that built the batch, where nothing more can be stored into it. */
void ui_sprite_ot_draw_begin(void) { release_batch(s_claim_n); }

/* The build frame's scene is about to be snapshotted and the next started: this
 * is the last moment the batch can reach the scene it belongs to. On a screen
 * whose other feeders all run after the walk this never fires with a batch
 * pending (the walk released it first); on one where a feeder of the next frame
 * runs before the walk — a menu's HUD mobys — it is the release point, and the
 * primitives' words are final by then. */
static void on_scene_frame_close(unsigned frame) {
    if (s_claim_n && frame == s_claim_frame) { s_claim_late++; ui_sprite_ot_draw_begin(); }
}

/* The canonical capture of the frame that built the batch: every one of that
 * frame's stores into the primitives has run (the game fills a primitive right
 * after linking it, and a write census over the batch's polybuf range shows no
 * store landing in a later frame), and the image has not been composed yet. So
 * this is where the batch is both final and still in its own frame — the
 * release point. The frame close and the ordering-table walk stay as later
 * releases for a frame that composes no image. */
static void on_overlay_precapture(void) {
    post_capture_report();
    const unsigned built = s_claim_frame;
    /* The WHOLE batch goes at the capture. Holding the last-linked primitive
     * back to the frame close reads its words a frame later, which for 2D that
     * ANIMATES (the cutscene letterbox sliding in) is the next tick's position:
     * measured, that put the bar 2 PSX rows out on every frame of the slide. */
    if (s_claim_n) { s_claim_at_capture++; release_batch(s_claim_n); }
    s_cap_frame = s_claim_n ? built : 0xFFFFFFFFu;
}

/* Re-read the batch released for the ordering table now being walked: by here
 * the game has finished with every primitive in it, so any word that differs
 * from what was fed was stored after the release point. */
void ui_sprite_ot_verify(void) {
    CPUState* cpu = s_cpu;
    unsigned n = s_verify_pending;
    s_verify_pending = 0;
    if (!cpu || !n) return;
    static int s_tr = -1;
    ENV_FLAG_LATCH(s_tr, "OPENPETE_UISPRITE_TRACE");
    { static int pf = -2; ENV_INT_LATCH(pf, "OPENPETE_XY3_PROBE", -1);
      if (pf >= 0 && (long)openpete_frame >= (long)pf - 3 && (long)openpete_frame <= (long)pf + 3)
        for (unsigned i = 0; i < n; i++) {
            uint32_t w = cpu->read_word(s_verify[i].addr + 0x08u);
            fprintf(stderr, "[xy3 f=%u] prim=%08X kind=%u link_y=%d cap_y=%d walk_y=%d\n",
                    openpete_frame, s_verify[i].addr, s_verify[i].kind,
                    (int)(int16_t)(s_verify[i].xy_link >> 16),
                    (int)(int16_t)(s_verify[i].xy_rel  >> 16),
                    (int)(int16_t)(w >> 16)); } }
    for (unsigned i = 0; i < n; i++) {
        s_verify_n++;
        if (claim_sum(cpu, s_verify[i].addr) != s_verify[i].sum) { s_verify_stale++;
            if (s_tr) fprintf(stderr, "[uistale f=%u] i=%u/%u kind=%u p=%08X\n",
                              openpete_frame, i, n, s_verify[i].kind, s_verify[i].addr); }
    }
}

static void feed_one_claim(CPUState* cpu, uint32_t prim, int kind, uint32_t aux);

static void claim_aux(CPUState* cpu, uint32_t prim, int kind, uint32_t aux) {
    if (openpete_frame == s_cap_frame) s_post_claim++;
    /* A batch never spans frames: the tail the capture kept back belongs to the
     * frame that built it, and by the first claim of a later frame it is final,
     * so release it before starting the new batch. Without this the tail holds
     * the batch open and the next frame's primitives inherit its build frame. */
    if (s_claim_n && openpete_frame != s_claim_frame) release_batch(s_claim_n);
    if (!s_claim_n) {
        s_claim_frame = openpete_frame;
        /* Open this frame's scene now, where a 3D feeder of the same ordering
         * table opens it, so the batch has a scene of its own build frame to be
         * appended to at the walk. On a screen with no 3D feeder (a menu, the
         * atlas) nothing else would open it, and the batch would land in the
         * walk's newer scene — one drawn tick after the world it belongs to. */
        openpete_native_scene_begin_frame(openpete_frame);
    }
    if (s_claim_n >= UI_CLAIM_MAX) { s_claim_lost++; return; }
    s_claim[s_claim_n].addr = prim;
    s_claim[s_claim_n].sum  = claim_sum(cpu, prim);
    s_claim[s_claim_n].seq  = 0;                /* the traversal walk ranks it at the release */
    s_claim[s_claim_n].aux  = aux;
    s_claim[s_claim_n].xy_link = cpu->read_word(prim + 0x08u);   /* xy0 as the game linked it */
    s_claim[s_claim_n].kind = (uint8_t)kind;
    s_claim_n++;
}
static void claim(CPUState* cpu, uint32_t prim, int kind) { claim_aux(cpu, prim, kind, 0u); }

/* The glyph emitters link their SPRT chains with an inlined insert, so the
 * chokepoint never sees them; text_sprt hands each glyph here instead. Claimed
 * rather than fed, so the release's table walk ranks a glyph against the
 * chokepoint's primitives by the order the GPU will draw them. */
void ui_sprite_feed_sprt(CPUState* cpu, uint32_t prim, unsigned tpage) {
    if (openpete_frame == s_cap_frame) s_post_sprt++;
    if (!openpete_native_scene_enabled()) return;
    if (!s_cpu) s_cpu = cpu;
    openpete_native_scene_set_prim(prim);   /* ranked by the release's table walk */
    feed_sprt_now(cpu, prim, tpage);
    openpete_native_scene_set_prim(0u);
}

static int on_ot_insert(CPUState* cpu, uint32_t addr) {
    int feed = openpete_native_scene_enabled();
    if (!feed) return 0;                        /* inert: normal dispatch runs it */
    if (!s_cpu) {                               /* one-time: the CPU the flush reads through */
        s_cpu = cpu;
        openpete_native_scene_register_frame_close(on_scene_frame_close);
        openpete_native_scene_register_overlay_precapture(on_overlay_precapture);
    }
    /* Name the UI texture upload (logo + PRESS START + banner glyphs, one
     * VRAM block) so a pack file of that name replaces the whole block. The
     * first 2D primitive is linked after the block is uploaded, so the
     * annotation binds to the atlas content; the level texture pages that
     * later reuse the same VRAM block hash differently and stay unnamed. */
    static int s_annotated = 0;
    if (!s_annotated && ui_game.atlas_w) {
        s_annotated = 1;
        texture_pack_annotate(ui_game.atlas_x, ui_game.atlas_y,
                              ui_game.atlas_w, ui_game.atlas_h, ui_game.atlas_name);
    }
    static int s_in = 0;
    if (s_in) return 0;                         /* re-entry from our own call -> real body */
    s_in = 1;

    uint32_t prim = cpu->a0;                     /* a0 = prim pointer being linked */
    if (openpete_frame != s_unclaimed_frame) { unclaimed_flush_tick(); s_unclaimed_frame = openpete_frame; }

    if (feed) {
        /* Claimed here, fed at the OT walk — the primitive's words are not
         * final yet (see the dispatch header). */
        if (is_freeze_backdrop(cpu, prim)) claim(cpu, prim, QK_FREEZE);
        else if (is_ft4(cpu, prim))        claim(cpu, prim, QK_FT4);
        else if (is_dr_mode(cpu, prim))    claim(cpu, prim, QK_DR_MODE);   /* the GPU draw-mode's ABR */
        else if (is_f4_opaque(cpu, prim))  claim(cpu, prim, QK_F4_BAR);    /* letterbox bars */
        else if (is_f4_semitrans(cpu, prim) && !s_in_fade)
                                           claim(cpu, prim, QK_F4_PANEL);  /* HUD shaded box / menu panel */
        else if (is_line_g2(cpu, prim))    claim(cpu, prim, QK_LINE_G2);   /* UI separator lines */
        else if (!s_in_fade) unclaimed_note(cpu, prim);   /* no decoder for this encoding: linked, not fed
                                                          * (a prim linked inside the fade emitter is the
                                                          * fade's own F4, drawn from its arguments) */
    }

    { OP_PROF_ACCUM_SCOPE("ui_sprite.srend");
      openpete_recomp_module_get()->dispatch(cpu, addr); }  /* the real OT insert (ref unchanged) */

    /* OPENPETE_UISPRITE_CENSUS: the per-class engagement counters, once per
     * 100 VBLs. `mutated` is how many claimed primitives the game rewrote
     * between their claim and the OT walk, the measurement the deferral exists
     * for; `lost` is claims dropped on batch overflow. */
    { static int cen = -1;
      ENV_FLAG_LATCH(cen, "OPENPETE_UISPRITE_CENSUS");
      static unsigned last = 0xFFFFFFFFu;
      if (cen && openpete_frame / 100u != last) { last = openpete_frame / 100u;
          fprintf(stderr, "[census f=%u UI-SPRITE] fed=%u sprt=%u mutated=%u lost=%u"
                          " verified=%u stale=%u abr=%d"
                          " ft4=%u bar=%u panel=%u line=%u sprt_n=%u fade=%u\n",
                  openpete_frame, s_fed_prims, s_fed_sprts, s_mutated, s_claim_lost,
                  s_verify_n, s_verify_stale, s_last_abr,
                  s_cls_n[UICLS_FT4], s_cls_n[UICLS_BAR], s_cls_n[UICLS_PANEL],
                  s_cls_n[UICLS_LINE], s_cls_n[UICLS_SPRT], s_cls_n[UICLS_FADE]); } }
    bbox_report();
    s_in = 0;
    return 1;
}

/* ---- full-screen fade overlay (ui_game.fade_emitter) ---------------------
 * Every transition the game tints the WHOLE frame through one emitter taking
 * (mode, r, g, b): one semi-transparent full-screen POLY_F4 whose ABR mode is
 * the fade direction — mode 2 (B-F) fades to black, mode 1 (B+F) to white.
 * PsyCross renders that F4 incorrectly (a half-screen diagonal triangle), so
 * there is no usable A/B ground truth; the native overlay is driven straight
 * from the arguments instead, which is also cleaner than reconstructing the
 * fade from the F4 plus its preceding DR_MODE. The native renderer draws a
 * full-panel quad last in the matching blend
 * (openpete_native_scene_set_fade). The fade auto-clears each frame, so it is
 * set whenever the emitter runs. Determinism-safe: the real emitter still
 * runs (the OT/F4 are built for the PsyCross reference unchanged); only its
 * arguments are READ. */
static int on_fade_emitter(CPUState* cpu, uint32_t addr) {
    if (!openpete_native_scene_enabled()) return 0;   /* inert: normal dispatch runs it */
    if (openpete_frame == s_cap_frame) s_post_fade++;
    static int s_in = 0;
    if (s_in) return 0;                                /* re-entry from our own call -> real body */
    s_in = 1;
    openpete_native_scene_begin_frame(openpete_frame); /* idempotent within a frame */
    openpete_native_scene_set_fade((int)cpu->a0, (int)cpu->a1, (int)cpu->a2, (int)cpu->a3);
    if ((int)cpu->a1 | (int)cpu->a2 | (int)cpu->a3) bbox_note(UICLS_FADE, 0.0f, 0.0f, 512.0f, 240.0f);
    /* The real fade emit links its 0x2A F4 through the chokepoint; flag it so
     * the chokepoint hook treats that F4 as the fade (already handled) and NOT
     * a dark panel (also a 0x2A F4). */
    s_in_fade = 1;
    { OP_PROF_ACCUM_SCOPE("ui_sprite.srend");
      openpete_recomp_module_get()->dispatch(cpu, addr); }  /* the real fade emit (ref unchanged) */
    s_in_fade = 0;
    s_in = 0;
    return 1;
}

/* The hook addresses are the descriptor's, bound at boot: a static
 * initializer cannot read another TU's const. A game whose descriptor names
 * neither address registers no hook and the module is inert. */
static feature_dispatch_entry_t ui_sprite_dispatch[] = {
    { 0u, on_ot_insert },
    { 0u, on_fade_emitter },
    { 0u, NULL },
};

/* Retained-framebuffer freeze backdrop (see ui_game_t::freeze_retained).
 *
 * The menu screens that keep the last rendered frame write DRAWENV.isbg = 0 on
 * both draw buffers and draw no world; every other screen leaves isbg at 1 and
 * clears to its own background colour. Reading the byte reproduces the game's
 * own discriminator rather than guessing from what the scene holds: the same
 * DRAWENV the clear-colour commit reads decides whether the frame is cleared at
 * all, so the two are read at the same point of the present.
 *
 * Nothing here writes PSX RAM: the backdrop is display state only. */
static unsigned s_freeze_retain_n = 0;

unsigned ui_freeze_retain_count(void) { return s_freeze_retain_n; }

void ui_freeze_retain_commit(void) {
    if (!ui_game.freeze_retained) return;
    if (!game_layout.db.cur_ptr) return;
    const uint32_t curdb = *(const uint32_t*)openpete_subtick_host_ptr(game_layout.db.cur_ptr);
    if (curdb != game_layout.db.base && curdb != game_layout.db.base + game_layout.db.stride)
        return;   /* not a valid DB* yet */
    const uint8_t* env = (const uint8_t*)openpete_subtick_host_ptr(curdb + game_layout.db.drawenv);
    if (env[0x18]) return;            /* PSYQ DRAWENV isbg @+0x18; 1 = the frame is cleared */
    /* The tint is unused in retained mode (blit.frag passes the snapshot
     * through) but the packed word's active bit lives in the same field, so a
     * full-scale colour is what the encoder expects. */
    openpete_native_scene_set_freeze_backdrop(0xFF, 0xFF, 0xFF, 0);
    s_freeze_retain_n++;
}

static void ui_freeze_retain_census(void) {
    fprintf(stderr, "ui-freeze: retained-backdrop presents=%u\n", s_freeze_retain_n);
}

static void ui_sprite_apply_options(const openpete_options_t* opts) {
    (void)opts;
    if (ui_game.freeze_retained) {
        openpete_native_scene_set_freeze_retained(1);
        atexit(ui_freeze_retain_census);
    }
    /* A zero addr terminates the table, so the named addresses are packed to
     * the front: a game that names only one still reaches it. */
    int n = 0;
    if (ui_game.ot_insert) {
        ui_sprite_dispatch[n].addr = ui_game.ot_insert;
        ui_sprite_dispatch[n].fn   = on_ot_insert; n++;
    }
    if (ui_game.fade_emitter) {
        ui_sprite_dispatch[n].addr = ui_game.fade_emitter;
        ui_sprite_dispatch[n].fn   = on_fade_emitter; n++;
    }
    ui_sprite_dispatch[n].addr = 0u;
    ui_sprite_dispatch[n].fn   = NULL;
}

const feature_module_t ui_sprite_module = {
    .name           = "ui_sprite_native",
    .apply_options  = ui_sprite_apply_options,
    .dispatch_table = ui_sprite_dispatch,
};
