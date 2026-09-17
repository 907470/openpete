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
#include "renderer/ui_game.h"
#include "openpete_recomp_module.h"   /* the game's own routine, through the module dispatch */
#include "texture_pack.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE — fiber-hook cost split */

extern unsigned openpete_frame;

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
/* Feed one screen-space FT4 (textured quad) prim into the native HUD overlay
 * layer. Layout (verified by the dump): word[1]=rgb|code, [2]=xy0,[3]=uv0|clut,
 * [4]=xy1,[5]=uv1|tpage,[6]=xy2,[7]=uv2,[8]=xy3,[9]=uv3. XY: x=lo16,y=hi16
 * (signed). PSX FT4 corner order TL,TR,BL,BR -> two tris (0,1,2)+(1,3,2). */
static int s_flush_at_capture;   /* set while the capture-boundary flush feeds */
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
    int tpx=(int)(tp&0xFu)*64, tpy=(int)((tp>>4)&1u)*256;
    int mode=(int)((tp>>7)&3u), bpp=(mode==0)?4:(mode==1)?8:16;
    int clx=(int)(clut&0x3Fu)*16, cly=(int)((clut>>6)&0x1FFu);
    uint32_t rgba = (w1 & 0xFFFFFFu) | 0xFF000000u;
    int abe = (int)((w1>>24)&0x2u);   /* code bit1 = semi-transparent (ABR mode 0) */

    /* The capture-boundary flush runs at the canonical present, where
     * openpete_frame may already be the next VBL: beginning THAT frame would
     * push the finished scene into the history ring a VBL early and start a
     * new scene holding only this sprite, and the lagged headless compose then
     * reads the wrong slot (the canonical dump jumps a tick). Append to the
     * live scene instead — the sprite belongs to the frame that deferred it. */
    unsigned live = openpete_native_scene_frame();
    openpete_native_scene_begin_frame((s_flush_at_capture && live != 0xFFFFFFFFu) ? live : openpete_frame);
    static const int idx[2][3] = { {0,1,2}, {1,3,2} };   /* TL,TR,BL,BR -> 2 tris */
    for (int t=0;t<2;t++) {
        openpete_scene_tri tri; memset(&tri,0,sizeof(tri));
        for (int j=0;j<3;j++){ int c=idx[t][j];
            tri.v[j].sx=(float)sx[c]; tri.v[j].sy=(float)sy[c];
            tri.v[j].z=0.0f; tri.v[j].u=(float)u[c]; tri.v[j].v=(float)v[c]; tri.v[j].rgba=rgba; }
        tri.textured=1; tri.tpage_x=tpx; tri.tpage_y=tpy; tri.clut_x=clx; tri.clut_y=cly;
        tri.bpp=bpp; tri.blend = abe ? 1 : 0;
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

    openpete_native_scene_begin_frame(openpete_frame);   /* idempotent within a frame */
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

    openpete_native_scene_begin_frame(openpete_frame);
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
void ui_sprite_feed_sprt(CPUState* cpu, uint32_t prim, unsigned tpage) {
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
    int tpx = (int)(tpage & 0xFu) * 64, tpy = (int)((tpage >> 4) & 1u) * 256;
    int mode = (int)((tpage >> 7) & 3u), bpp = (mode == 0) ? 4 : (mode == 1) ? 8 : 16;
    int clx = (int)(clut & 0x3Fu) * 16, cly = (int)((clut >> 6) & 0x1FFu);

    float sx[4] = { (float)x0, (float)(x0 + w), (float)x0, (float)(x0 + w) };
    float sy[4] = { (float)y0, (float)y0, (float)(y0 + h), (float)(y0 + h) };
    float su[4] = { (float)u0, (float)(u0 + w), (float)u0, (float)(u0 + w) };
    float sv[4] = { (float)v0, (float)v0, (float)(v0 + h), (float)(v0 + h) };

    openpete_native_scene_begin_frame(openpete_frame);
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

/* ---- dispatch ------------------------------------------------------------
 * A caller can MODIFY a prim AFTER linking it: a title-screen sprite draw
 * mirrors a sprite by swapping its U coords *after* the insert returns (the
 * right-hand memory-card border). So a prim's UVs are NOT final at its own
 * insert. They ARE final by the NEXT insert (the swap runs in the caller
 * before it draws the next sprite). So the PREVIOUS prim is fed at each call
 * ("feed-previous"): it has its post-link mirror applied, and the insert's
 * placement is kept (begin_frame at the render-pass openpete_frame, matching
 * the 3D feeders). The chokepoint is the clean 2D-vs-3D discriminator (the 3D
 * renderers inline the insert). The last prim of a frame is fed at the next
 * frame's first call — invisible for the static screens this serves
 * (title/menu re-emit identical 2D every frame). */
static uint32_t s_pending = 0;        /* prim awaiting its (post-link) mirror swap */
static unsigned s_pending_frame = 0;  /* openpete_frame the pending prim was recorded */
static uint32_t s_pending_words[10];  /* the FT4's words as they stood at link time */
static unsigned s_mutated;            /* FT4s whose words changed between link and flush:
                                       * the deferral's engagement counter (0 means the
                                       * game never rewrites a linked prim and the
                                       * deferral is costing a frame for nothing) */
static CPUState* s_cpu = NULL;        /* the game CPU (stable) — for the precapture flush */
static int      s_in_fade = 0;        /* set across the fade emitter's real-function call so
                                       * the chokepoint hook can tell the fade's 0x2A F4 from
                                       * a dark panel's (both go through the chokepoint) */
static int      s_last_abr = 0;       /* semi-transparency rate (ABR) of the most recent
                                       * DR_MODE seen — the GPU draw-mode in effect for the
                                       * next semi-transparent F4 (the game links a box's
                                       * DR_MODE immediately before its 0x2A F4). PSX ABR:
                                       * 0 (B+F)/2, 1 B+F, 2 B-F, 3 B+F/4. */

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

/* Flush the deferred FT4 (feed-previous) into the current scene/carry. Two
 * callers: the next chokepoint call (the original feed point — the caller's
 * post-link mirror swap has run by then), and the canonical capture_overlay
 * (via the registered precapture hook) — when the FT4 is the frame's LAST 2D
 * prim there IS no next call within the frame, and without this boundary flush
 * the sprite misses its own tick's carry: in gameplay the collected-egg HUD
 * sprite is the ONLY 2D prim, so every one of its animation frames arrived a
 * tick late and vanished whenever the deferral crossed a capture (egg.bk2
 * ~f3603 at --render-fps > 30). By either flush point the prim's words are
 * final, so both are exact. */
static void flush_pending(CPUState* cpu) {
    if (!s_pending) return;
    /* Stale guard: feed only if the pending prim is recent (<= one game
     * frame ≈ 2 host ticks). On a screen that STOPS emitting 2D, a
     * lingering pending must not flash into a later unrelated frame. */
    int fresh = openpete_frame - s_pending_frame <= 2u;
    int mutated = 0;
    for (int k = 0; k < 10; k++)
        if (cpu->read_word(s_pending + (uint32_t)k * 4u) != s_pending_words[k]) { mutated = 1; break; }
    if (mutated) s_mutated++;
    { static int s_tr = -1;
      if (s_tr < 0) { const char* e = getenv("OPENPETE_UISPRITE_TRACE"); s_tr = (e && *e && *e != '0'); }
      if (s_tr) fprintf(stderr, "[uifeed f=%u] pending from f=%u %s%s uv0=%u,%u (mutated=%u fed=%u)\n",
                        openpete_frame, s_pending_frame, fresh ? "FEED" : "DROP-stale",
                        mutated ? " POST-LINK-EDIT" : "",
                        cpu->read_word(s_pending + 0x0C) & 0xFFu,
                        (cpu->read_word(s_pending + 0x0C) >> 8) & 0xFFu,
                        s_mutated, s_fed_prims); }
    if (fresh)
        feed_ft4(cpu, s_pending, cpu->read_word(s_pending + 4));
    s_pending = 0;
}

static void on_overlay_precapture(void) {
    if (!s_cpu || !openpete_native_scene_enabled()) return;
    s_flush_at_capture = 1;
    flush_pending(s_cpu);
    s_flush_at_capture = 0;
}

static int on_ot_insert(CPUState* cpu, uint32_t addr) {
    int feed = openpete_native_scene_enabled();
    if (!feed) return 0;                        /* inert: normal dispatch runs it */
    if (!s_cpu) {                               /* one-time: arm the capture-boundary flush */
        s_cpu = cpu;
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
    if (feed) {
        flush_pending(cpu);                      /* previous FT4 — its mirror swap is done now */
        if (is_freeze_backdrop(cpu, prim)) {
            /* Pause / inventory / flight-results green overlay: drive the native
             * freeze backdrop from our own last world frame and DROP this FT4 (it
             * samples the emulated VRAM scratch). Tint = the FT4's setRGB0 (B,G,R
             * packed in w1) — r=lo byte, g=mid, b=hi. */
            uint32_t w1 = cpu->read_word(prim + 4);
            openpete_native_scene_begin_frame(openpete_frame);  /* idempotent within a frame */
            openpete_native_scene_set_freeze_backdrop((int)(w1 & 0xFFu),
                                                      (int)((w1 >> 8) & 0xFFu),
                                                      (int)((w1 >> 16) & 0xFFu),
                                                      0);   /* static: no fade-to-black */
            s_fed_prims++;
        }
        else if (is_ft4(cpu, prim)) {
            s_pending = prim; s_pending_frame = openpete_frame;
            for (int k = 0; k < 10; k++) s_pending_words[k] = cpu->read_word(prim + (uint32_t)k * 4u);
        }
        else if (is_dr_mode(cpu, prim))                                         /* track the GPU draw-mode's ABR */
            s_last_abr = (int)((cpu->read_word(prim + 4) >> 5) & 3u);
        else if (is_f4_opaque(cpu, prim))                                       /* letterbox bars: OVERLAY, opaque, full-panel-wide */
            feed_f4(cpu, prim, cpu->read_word(prim + 4), OPENPETE_SCENE_OVERLAY, 0xFFu, 0, 1);
        else if (is_f4_semitrans(cpu, prim) && !s_in_fade) {                    /* 2D box (HUD shaded box / menu panel): blend by the box's ABR */
            /* The box links its own DR_MODE immediately before the quad, so
             * the tracked ABR is the blend the GPU is in when the quad draws.
             * ABR 2 (B-F) DARKENS the framebuffer by the quad's colour and is
             * rendered subtractive (blend 3); ABR 0 ((B+F)/2) is a halving and
             * is rendered at alpha 0x80. Modes 1 (B+F) and 3 (B+F/4) fall back
             * to alpha — add an additive HUD pipe when a consumer appears. */
            int sub = (s_last_abr == 2);
            feed_f4(cpu, prim, cpu->read_word(prim + 4), OPENPETE_SCENE_HUD_PANEL,
                    sub ? 0xFFu : 0x80u, sub ? 3 : 1, 0);
        }
        else if (is_line_g2(cpu, prim)) feed_line_g2(cpu, prim, cpu->read_word(prim + 4));  /* UI separator lines */
    }

    { OP_PROF_ACCUM_SCOPE("ui_sprite.srend");
      openpete_recomp_module_get()->dispatch(cpu, addr); }  /* the real OT insert (ref unchanged) */

    /* OPENPETE_UISPRITE_CENSUS: the per-class engagement counters, once per
     * 100 VBLs. `mutated` is how many deferred FT4s the game rewrote between
     * their link and their flush, the measurement the feed-previous deferral
     * exists for. */
    { static int cen = -1;
      if (cen < 0) { const char* e = getenv("OPENPETE_UISPRITE_CENSUS"); cen = (e && *e && *e != '0'); }
      static unsigned last = 0xFFFFFFFFu;
      if (cen && openpete_frame / 100u != last) { last = openpete_frame / 100u;
          fprintf(stderr, "[census f=%u UI-SPRITE] fed=%u sprt=%u mutated=%u abr=%d"
                          " ft4=%u bar=%u panel=%u line=%u sprt_n=%u fade=%u\n",
                  openpete_frame, s_fed_prims, s_fed_sprts, s_mutated, s_last_abr,
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

static void ui_sprite_apply_options(const openpete_options_t* opts) {
    (void)opts;
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
