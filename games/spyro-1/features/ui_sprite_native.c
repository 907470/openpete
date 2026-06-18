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

/* ---- dispatch ------------------------------------------------------------ */
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
    if (feed && prim >= 0x80000000u) {
        uint32_t w0 = cpu->read_word(prim);
        uint32_t w1 = cpu->read_word(prim + 4);
        uint8_t  code = (uint8_t)(w1 >> 24);
        uint8_t  plen = (uint8_t)(w0 >> 24);    /* OT tag length (words after tag) */
        if ((code & 0xFD) == 0x2C && plen >= 9) /* 0x2C / 0x2E = POLY_FT4 (textured quad) */
            feed_ft4(cpu, prim, w1);
    }

    func_800168DC(cpu);                          /* run the real OT insert (ref unchanged) */
    s_in = 0;
    return 1;
}

static const feature_dispatch_entry_t ui_sprite_dispatch[] = {
    { 0x800168DCu, on_800168DC },
    { 0u, NULL },
};

const feature_module_t ui_sprite_native_module = {
    .name           = "ui_sprite_native",
    .apply_options  = NULL,
    .dispatch_table = ui_sprite_dispatch,
};
