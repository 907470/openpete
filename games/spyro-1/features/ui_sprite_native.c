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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    openpete_native_scene_set_category(OPENPETE_SCENE_HUD);
    static const int idx[2][3] = { {0,1,2}, {1,3,2} };   /* TL,TR,BL,BR -> 2 tris */
    for (int t=0;t<2;t++) {
        openpete_scene_tri tri; memset(&tri,0,sizeof(tri));
        for (int j=0;j<3;j++){ int c=idx[t][j];
            tri.v[j].sx=(float)sx[c]; tri.v[j].sy=(float)sy[c];
            tri.v[j].z=0.0f; tri.v[j].u=(float)u[c]; tri.v[j].v=(float)v[c]; tri.v[j].rgba=rgba; }
        tri.textured=1; tri.tpage_x=tpx; tri.tpage_y=tpy; tri.clut_x=clx; tri.clut_y=cly;
        tri.bpp=bpp; tri.blend = abe ? 1 : 0;
        openpete_native_scene_add_tri(&tri);
    }
    openpete_native_scene_set_category(OPENPETE_SCENE_WORLD);
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

static int is_ft4(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return ((uint8_t)(w1 >> 24) & 0xFD) == 0x2C   /* 0x2C / 0x2E = POLY_FT4 */
        && (uint8_t)(w0 >> 24) >= 9;              /* tag length >= 9 (full FT4) */
}

static int on_800168DC(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int feed = openpete_native_render_enabled();
    int dump = dump_enabled();
    if (!feed && !dump) return 0;               /* inert: normal dispatch runs it */
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
        if (is_ft4(cpu, prim)) { s_pending = prim; s_pending_frame = openpete_frame; }
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
    func_800190D4(cpu);                                /* run the real fade emit (ref unchanged) */
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
