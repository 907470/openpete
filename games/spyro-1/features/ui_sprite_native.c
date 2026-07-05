// games/spyro-1/features/ui_sprite_native.c — native renderer feed for the 2D
// UI sprite/text layer (Phase: 2D-from-OT).
//
// The 3D scene (terrain, mobys, Spyro, particles, …) is re-derived from
// gamestate by the other native feeders. The 2D UI — the title-screen "SPYRO
// the dragon" logo, "PRESS START", HUD text, the level-name banner, etc. — is a
// separate layer the game builds as screen-space prims (no GTE), linking each
// into the shared ordering table through ONE helper: func_800168DC (addPrim;
// a0 = prim pointer). The 3D renderers use an INLINED addPrim, so func_800168DC
// is the clean 2D chokepoint (all 16 of its callers live in the 0x80018xxx HUD/
// text region, none in the 3D renderers at 0x8001F798+).
//
// This module hooks func_800168DC, decodes each 2D prim it inserts, and feeds it
// to the native scene's HUD overlay layer (OPENPETE_SCENE_HUD — screen-space,
// depth-test off, on top of the world). Textures sample the per-frame VRAM
// mirror the native renderer already uploads, so the font/logo art is available
// for free. Determinism-safe: the hook runs the real func_800168DC unchanged
// (the OT link still happens for the PsyCross reference), only READING the prim.
//
// Falsify-before-ship: the prim byte layout is confirmed by the one-shot dump
// (OPENPETE_UISPRITE_DUMP=<frame>) rather than trusting an inferred offset map.

#include "feature_registry.h"
#include "psx_runtime.h"
#include "openpete_native_render.h"
#include "render_paths.h"
#include "texture_pack.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern void     func_800168DC(CPUState* cpu);   /* the real OT-insert helper */
extern void     func_800190D4(CPUState* cpu);    /* the full-screen fade emitter */
extern unsigned openpete_frame;

/* ---- gating -------------------------------------------------------------- */
static int s_dump = -1; static unsigned s_dump_frame;
static int dump_enabled(void) {
    if (s_dump < 0) {
        const char* e = getenv("OPENPETE_UISPRITE_DUMP");
        s_dump = (e && *e) ? 1 : 0;
        s_dump_frame = (e && *e && *e != '1') ? (unsigned)strtoul(e, NULL, 0) : 0;
    }
    return s_dump;
}

/* ---- one-shot prim dump (verify the FT4/F4 layout empirically) ----------- */
static void dump_prim(CPUState* cpu, uint32_t p) {
    static int s_n = 0;
    if (s_dump_frame && openpete_frame != s_dump_frame) return;
    if (s_n >= 32) return;
    s_n++;
    uint32_t w0 = cpu->read_word(p);
    uint32_t len = (w0 >> 24) & 0xFF;          /* OT tag length (words after tag) */
    uint8_t  code = (uint8_t)(cpu->read_word(p + 4) >> 24);
    int nw = (int)len + 1; if (nw > 16) nw = 16; if (nw < 2) nw = 2;
    char buf[256]; int bp = 0;
    for (int i = 0; i < nw && bp < (int)sizeof(buf) - 12; i++)
        bp += snprintf(buf + bp, sizeof(buf) - bp, " %08X", cpu->read_word(p + (uint32_t)i * 4));
    fprintf(stderr, "[uisprite f=%u] @%08X code=%02X len=%u ra=%08X :%s\n",
            openpete_frame, p, code, len, cpu->ra, buf);
}

/* ---- 2D prim decode + feed ----------------------------------------------- */
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
    int tpx=(int)(tp&0xFu)*64, tpy=(int)((tp>>4)&1u)*256;
    int mode=(int)((tp>>7)&3u), bpp=(mode==0)?4:(mode==1)?8:16;
    int clx=(int)(clut&0x3Fu)*16, cly=(int)((clut>>6)&0x1FFu);
    uint32_t rgba = (w1 & 0xFFFFFFu) | 0xFF000000u;
    int abe = (int)((w1>>24)&0x2u);   /* code bit1 = semi-transparent (ABR mode 0) */

    openpete_native_scene_begin_frame(openpete_frame);   /* idempotent within a frame */
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
}

/* Feed one screen-space untextured POLY_F4 (flat quad) into a native HUD-OT layer.
 * Two consumers:
 *   - the cutscene letterbox (func_80018F30): two full-width OPAQUE black bars
 *     (code 0x28) -> OPENPETE_SCENE_OVERLAY (dead last, over everything), alpha 0xFF;
 *   - the pause/inventory menu dark panel (func_8001A40C): a SEMI-TRANSPARENT quad
 *     (code 0x2A, setRGB0(64,64,64)) -> OPENPETE_SCENE_HUD_PANEL (under the menu
 *     text, over the world/backdrop), alpha 0x80 = ABR mode 0 (B+F)/2.
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
}

/* Feed one screen-space gouraud line (LINE_G2, code 0x50) into the native HUD
 * layer as a thin gouraud quad (the scene is triangles-only, like the particle/
 * sparkle line feeds). The consumer is func_8001844C ("shaded line") — the UI
 * separator lines (the horizontal rules on the flight results screen, gem-UI
 * dividers, …), with a per-endpoint shimmer colour. Layout: word[1]=code|rgb0,
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
}

/* ---- dispatch ------------------------------------------------------------
 * A caller can MODIFY a prim AFTER linking it: TitlescreenDrawSprite mirrors a
 * sprite by swapping its U coords *after* func_800168DC returns (the "missing
 * flip" — the right-hand memory-card border). So a prim's UVs are NOT final at
 * its own func_800168DC call. But they ARE final by the NEXT func_800168DC call
 * (the swap runs in the caller before it draws the next sprite). So we feed the
 * PREVIOUS prim at each call ("feed-previous"): it has its post-link mirror
 * applied, and we keep the proven func_800168DC placement (begin_frame at the
 * render-pass openpete_frame, matching the 3D feeders — the immediate feed got
 * this right; only its UVs were stale). func_800168DC is the clean 2D-vs-3D
 * discriminator (the 3D renderers inline addPrim). The last prim of a frame is
 * fed at the next frame's first call — invisible for the static screens this
 * serves (title/menu re-emit identical 2D every frame). */
static uint32_t s_pending = 0;        /* prim awaiting its (post-link) mirror swap */
static unsigned s_pending_frame = 0;  /* openpete_frame the pending prim was recorded */
static int      s_in_fade = 0;        /* set across func_800190D4's real-func call so the
                                       * on_800168DC hook can tell the fade's 0x2A F4 from a
                                       * menu dark panel's 0x2A F4 (both go via func_800168DC) */
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

/* Semi-transparent POLY_F4 (0x2A) — the pause/inventory menu dark panel. Same code
 * as the full-screen fade, BUT the fade is emitted from func_800190D4 (the feeder
 * excludes it via s_in_fade, set across that hook's real-func call). So a 0x2A F4
 * seen with s_in_fade clear is a menu panel; with it set, it's the fade (handled). */
static int is_f4_semitrans(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return (uint8_t)(w1 >> 24) == 0x2A            /* POLY_F4 semi-transparent */
        && (uint8_t)(w0 >> 24) >= 5;              /* tag length >= 5 (full F4) */
}

/* DR_MODE / DR_TPAGE (GP0 command 0xE1) — sets the GPU draw mode, incl. the
 * semi-transparency RATE (ABR) in bits 5-6 of the tpage word (getTPage packs
 * abr as ((abr&3)<<5)). A 2D box (func_8001860C "shaded box", func_8001A40C menu
 * panel) links its DR_MODE via func_800168DC immediately before its 0x2A F4, so
 * the latest DR_MODE's ABR is the blend mode in effect for the next F4. We mirror
 * the game's own GPU state here rather than hardcode a blend. code[0] is at +4
 * (the tag occupies +0), so the GP0 byte and the ABR live in read_word(prim+4). */
static int is_dr_mode(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    return (uint8_t)(cpu->read_word(prim + 4) >> 24) == 0xE1;   /* code[0] = GP0 0xE1 */
}

/* Freeze-frame green-overlay backdrop FT4 (pause / inventory / flight results):
 * func_8001A40C / the Flight5 overlay desaturate the frozen frame to grayscale and
 * re-display it as 4 full-screen POLY_FT4 sampling a VRAM scratch copy, each with
 * the green tint setRGB0(0x4C,0x80,0x40) and the grayscale CLUT at VRAM (512,224).
 * That tint+CLUT pair is the unambiguous signature (the title logo is also 8bpp at
 * tpage x=512 but white-tinted with its own CLUT). The native renderer reproduces
 * the overlay from its OWN last world frame (set_freeze_backdrop) and skips feeding
 * this FT4, so the green overlay shows the native render rather than PsyCross VRAM. */
static int is_freeze_backdrop(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    if (((uint8_t)(w1 >> 24) & 0xFD) != 0x2C) return 0;   /* POLY_FT4 (0x2C/0x2E) */
    if ((uint8_t)(w0 >> 24) < 9) return 0;                /* full FT4 tag */
    if ((w1 & 0xFFFFFFu) != 0x40804Cu) return 0;          /* green freeze tint (B,G,R) */
    uint32_t clut = (cpu->read_word(prim + 0x0C) >> 16) & 0xFFFFu;
    int clx = (int)(clut & 0x3Fu) * 16, cly = (int)((clut >> 6) & 0x1FFu);
    return clx == 512 && cly == 224;                      /* grayscale-CLUT location */
}

/* LINE_G2 (gouraud 2-point line, code 0x50) — the func_8001844C UI separator
 * lines. (The particle sel-1 streak LINE_G goes through the particle path, not
 * func_800168DC, so there's no overlap.) */
static int is_line_g2(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return (uint8_t)(w1 >> 24) == 0x50            /* LINE_G2 */
        && (uint8_t)(w0 >> 24) >= 4;              /* tag length >= 4 (full LINE_G2) */
}

static int on_800168DC(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int feed = openpete_native_render_enabled();
    int dump = dump_enabled();
    if (!feed && !dump) return 0;               /* inert: normal dispatch runs it */
    /* First symbolic texture asset (texture-replacement plan Phase 7): name
     * the title-screen UI upload (logo + PRESS START + banner glyphs — one
     * 512x256 block at VRAM (512,256), measured via the registry dump). A
     * pack file `title_ui.png` then replaces the whole block by NAME.
     * Rect-keyed for now; decomp load-site annotation supersedes this when
     * the uploader function gets named. */
    static int s_annotated = 0;
    if (!s_annotated) {
        s_annotated = 1;
        texture_pack_annotate(512, 256, 512, 256, "title_ui");
    }
    static int s_in = 0;
    if (s_in) return 0;                         /* re-entry from our own call -> real body */
    s_in = 1;

    uint32_t prim = cpu->a0;                     /* a0 = prim pointer being linked */
    if (dump) dump_prim(cpu, prim);
    if (feed) {
        if (s_pending) {                         /* previous FT4 — its mirror swap is done now */
            /* Stale guard: feed only if the pending prim is recent (<= one game
             * frame ≈ 2 host ticks). On a screen that STOPS emitting 2D, a
             * lingering pending must not flash into a later unrelated frame. */
            if (openpete_frame - s_pending_frame <= 2u)
                feed_ft4(cpu, s_pending, cpu->read_word(s_pending + 4));
            s_pending = 0;
        }
        if (is_freeze_backdrop(cpu, prim)) {
            /* Pause / inventory / flight-results green overlay: drive the native
             * freeze backdrop from our own last world frame and DROP this FT4 (it
             * samples PsyCross's VRAM scratch). Tint = the FT4's setRGB0 (B,G,R
             * packed in w1) — r=lo byte, g=mid, b=hi. */
            uint32_t w1 = cpu->read_word(prim + 4);
            openpete_native_scene_begin_frame(openpete_frame);  /* idempotent within a frame */
            openpete_native_scene_set_freeze_backdrop((int)(w1 & 0xFFu),
                                                      (int)((w1 >> 8) & 0xFFu),
                                                      (int)((w1 >> 16) & 0xFFu),
                                                      0);   /* static: no fade-to-black */
        }
        else if (is_ft4(cpu, prim)) { s_pending = prim; s_pending_frame = openpete_frame; }
        else if (is_dr_mode(cpu, prim))                                         /* track the GPU draw-mode's ABR */
            s_last_abr = (int)((cpu->read_word(prim + 4) >> 5) & 3u);
        else if (is_f4_opaque(cpu, prim))                                       /* letterbox bars: OVERLAY, opaque, full-panel-wide */
            feed_f4(cpu, prim, cpu->read_word(prim + 4), OPENPETE_SCENE_OVERLAY, 0xFFu, 0, 1);
        else if (is_f4_semitrans(cpu, prim) && !s_in_fade) {                    /* 2D box (HUD shaded box / menu panel): blend by the box's ABR */
            /* The "shaded box" behind in-game HUD text (func_8001860C, setRGB0
             * 112,112,112) and the pause/inventory menu panel (func_8001A40C,
             * setRGB0 64,64,64) BOTH set getTPage(0, 2, ..) = ABR mode 2 (B-F):
             * the box DARKENS the framebuffer by its colour. Render that with
             * subtractive (blend 3); only ABR mode 0 ((B+F)/2) uses alpha 0x80.
             * Modes 1 (B+F) / 3 (B+F/4) have no HUD-panel consumer in this game
             * and fall back to alpha — add an additive HUD pipe if one ever does. */
            int sub = (s_last_abr == 2);
            feed_f4(cpu, prim, cpu->read_word(prim + 4), OPENPETE_SCENE_HUD_PANEL,
                    sub ? 0xFFu : 0x80u, sub ? 3 : 1, 0);
        }
        else if (is_line_g2(cpu, prim)) feed_line_g2(cpu, prim, cpu->read_word(prim + 4));  /* UI separator lines */
    }

    func_800168DC(cpu);                          /* run the real OT insert (ref unchanged) */
    s_in = 0;
    return 1;
}

/* ---- full-screen fade overlay (func_800190D4) ----------------------------
 * Every transition the game tints the WHOLE frame via func_800190D4(mode, r, g, b):
 * one semi-transparent full-screen POLY_F4 whose ABR mode is the fade direction —
 * mode 2 (B-F) fades to black, mode 1 (B+F) fades to white. PsyCross renders this
 * F4 incorrectly (a half-screen diagonal triangle), so there is no usable A/B
 * ground truth; we drive the native overlay straight from the function arguments
 * instead, which is also far cleaner than reconstructing the fade from the F4 prim
 * + its preceding DR_MODE (which carries the ABR bits). The native renderer draws a
 * full-panel quad last in the matching blend (openpete_native_scene_set_fade). The
 * fade auto-clears each frame, so we just set it whenever func_800190D4 runs.
 * Determinism-safe: the real func_800190D4 still runs (the OT/F4 are built for the
 * PsyCross reference unchanged); we only READ its arguments. */
static int on_800190D4(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!openpete_native_render_enabled()) return 0;   /* inert: normal dispatch runs it */
    static int s_in = 0;
    if (s_in) return 0;                                /* re-entry from our own call -> real body */
    s_in = 1;
    openpete_native_scene_begin_frame(openpete_frame); /* idempotent within a frame */
    openpete_native_scene_set_fade((int)cpu->a0, (int)cpu->a1, (int)cpu->a2, (int)cpu->a3);
    /* The real fade emit links its 0x2A F4 via func_800168DC; flag it so the
     * on_800168DC hook treats that F4 as the fade (already handled) and NOT a menu
     * dark panel (also a 0x2A F4). */
    s_in_fade = 1;
    func_800190D4(cpu);                                /* run the real fade emit (ref unchanged) */
    s_in_fade = 0;
    s_in = 0;
    return 1;
}

static const feature_dispatch_entry_t ui_sprite_dispatch[] = {
    { 0x800168DCu, on_800168DC },
    { 0x800190D4u, on_800190D4 },
    { 0u, NULL },
};

const feature_module_t ui_sprite_native_module = {
    .name           = "ui_sprite_native",
    .apply_options  = NULL,
    .dispatch_table = ui_sprite_dispatch,
};
