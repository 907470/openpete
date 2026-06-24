// trail_native.c — native feeder for the additive "trail/beam" renderer
// (func_800189F0 → func_800189F8), e.g. the dream-weavers fireball's glowing
// streak. The .s projects up to 64 trail points (via func_80017B48, the
// permuted-axis projector) and emits a strip of additive (ABR mode 1) gouraud
// quads — each GPU prim is an E1 drawmode packet (0xE1000420) followed by a
// G4 (code 0x38 opaque / 0x3A semi-transparent). Native hooked cyclorama,
// mobys, Spyro, flame, shadows, particles, glows, sparkles and terrain — but
// NOT this renderer, so the entire beam was absent in the native window
// (confirmed by a world-OT dump at dream-weavers-particle.bk2 f1785: the beam
// is the 0x3A strip at polybuf 0x8019AFxx–0x8019BBxx, a buffer separate from
// every other feeder; forcing mobys/particles/glows near never revealed it
// because it is none of them).
//
// CAPTURE-AND-FEED (matches particle_native's pattern): run the real renderer
// once (so it writes any verdict/gameplay state exactly as PSX would), then
// walk the polybuf range it just filled, decode the additive gouraud quads,
// and feed them to the native scene's additive layer. Display-only — reads the
// prims the .s already emitted, writes no PSX RAM, so the oracle gate is inert.
//
// v1 depth: the captured prims are screen-space (the GTE already projected
// them) and carry no view-Z, so the strip is fed at a single near-ish depth.
// That makes the additive beam draw OVER world geometry — which is what the
// PSX OT does for this effect (the beam sweeps over the archway in PsyCross).
// A full gamestate re-derivation (per-point view-Z + high-FPS sub-tick interp)
// is a follow-up; this increment makes the beam correct at canonical frames.

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "feature_registry.h"
#include "psx_runtime.h"
#include "openpete_native_render.h"
#include "render_paths.h"

extern void     func_800189F0(CPUState* cpu);   /* the real trail/beam renderer */
extern unsigned openpete_frame;

#define POLYBUF_CURSOR 0x800757B0u

/* Feed depth for the additive strip (see header note). depth_from_viewz wants
 * a GTE view-Z (1.0 == 4096); a small value sits the beam in front of world
 * terrain so the additive blend shows, matching the PSX OT overlay order. */
#define TRAIL_FEED_VIEWZ 1500.0f

/* Decode one polybuf prim's gouraud quad/tri (standard PSX layout: word[1] =
 * rgb0|code, then per vertex {rgb if gouraud&&k>0}{xy}). Returns nv (3/4) or 0
 * for non-polygon prims (E1 drawmode packets, etc.). */
static int decode_gouraud(CPUState* cpu, uint32_t p, uint32_t stride,
                          uint8_t code, float x[4], float y[4], uint32_t rgb[4]) {
    if ((code & 0xE0u) != 0x20u) return 0;     /* not a GPU polygon */
    int gouraud = (code & 0x10u) != 0;
    int quad    = (code & 0x08u) != 0;
    int tex     = (code & 0x04u) != 0;
    int nv = quad ? 4 : 3;
    int nw = (int)(stride / 4u);
    uint32_t rgb0 = cpu->read_word(p + 4u) & 0xFFFFFFu;
    int c = 2;                                 /* word cursor past rgb0|code */
    for (int k = 0; k < nv; k++) {
        rgb[k] = (gouraud && k > 0 && c < nw) ? (cpu->read_word(p + (uint32_t)c++ * 4u) & 0xFFFFFFu) : rgb0;
        if (c >= nw) return 0;                 /* malformed — bail */
        uint32_t xy = cpu->read_word(p + (uint32_t)c++ * 4u);
        x[k] = (float)(int16_t)(xy & 0xFFFFu);
        y[k] = (float)(int16_t)(xy >> 16);
        if (tex && c < nw) c++;                /* skip uv word (trail is untextured) */
    }
    return nv;
}

/* Walk [start,end) by OT tag length, feed each additive gouraud quad/tri. */
static void trail_feed_range(CPUState* cpu, uint32_t start, uint32_t end) {
    static const int split[2][3] = { {0,1,2}, {1,3,2} };  /* quad -> 2 tris */
    float depth = openpete_native_depth_from_viewz(TRAIL_FEED_VIEWZ);
    uint32_t p = start;
    int guard = 0;
    int fed = 0;
    while (p < end && guard++ < 8192) {
        uint8_t code = (uint8_t)(cpu->read_word(p + 4u) >> 24);
        /* The strip alternates E1 drawmode packets (set ABR1 additive) and G4
         * gouraud quads. The E1 packet is 3 words CONTIGUOUS (tag + 2), though
         * its tag length byte reads 3 (so (len+1)*4 would overshoot by a word
         * and desync the walk) — advance it by a fixed 12 bytes. Polygons use
         * the standard (taglen+1)*4 stride. */
        if (code == 0xE1u) { p += 12u; continue; }
        if ((code & 0xE0u) != 0x20u) break;               /* end of strip / not a polygon */
        uint32_t taglen = (cpu->read_word(p) >> 24) & 0xFFu;
        if (taglen == 0u || taglen > 12u) break;
        uint32_t stride = (taglen + 1u) * 4u;
        float x[4], y[4]; uint32_t rgb[4];
        int nv = decode_gouraud(cpu, p, stride, code, x, y, rgb);
        if (nv >= 3) {
            /* ABR mode 1 (additive) when the STP bit is set (0x3A/0x36); the
             * E1 drawmode packets in this strip select ABR 1. Opaque G4 (0x38)
             * keeps blend 0. */
            int blend = (code & 0x02u) ? 2 : 0;
            int ntri = (nv == 4) ? 2 : 1;
            for (int s = 0; s < ntri; s++) {
                openpete_scene_tri st;
                memset(&st, 0, sizeof st);
                st.textured = 0;
                st.blend    = blend;
                st.category = OPENPETE_SCENE_ACTOR;   /* ignored for blend!=0 (routes by blend) */
                for (int k = 0; k < 3; k++) {
                    int j = (nv == 4) ? split[s][k] : k;
                    st.v[k].sx   = x[j];
                    st.v[k].sy   = y[j];
                    st.v[k].z    = depth;
                    st.v[k].rgba = (rgb[j] & 0xFFFFFFu) | 0xFF000000u;
                }
                openpete_native_scene_add_tri(&st);
            }
            fed++;
        }
        p += stride;
    }
    (void)fed;
}

/* Dispatch hook: run the real trail renderer (verdict state), then capture +
 * feed the additive strip it emitted to the shared polybuf. */
static int on_800189F0_trail(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int feed = openpete_native_render_enabled();
    if (!feed && !render_paths_active()) return 0;        /* inert: normal dispatch */

    /* Re-entrancy guard: calling func_800189F0(cpu) below re-enters dispatch,
     * which would call this hook again. On the nested call return 0 so
     * psx_override_dispatch falls through and the real function body runs. */
    static int s_in = 0;
    if (s_in) return 0;
    s_in = 1;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_800189F0(cpu);                                   /* real renderer (verdict) */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_PARTICLES, startp, endp);       /* group with effects */

    if (feed && endp > startp && render_path_enabled(RP_PARTICLES)) {
        openpete_native_scene_begin_frame(openpete_frame);
        trail_feed_range(cpu, startp, endp);
    }
    s_in = 0;
    return 1;
}

static const feature_dispatch_entry_t trail_dispatch[] = {
    { 0x800189F0u, on_800189F0_trail },
    { 0u, NULL },
};

const feature_module_t trail_native_module = {
    .name           = "trail_native",
    .apply_options  = NULL,
    .dispatch_table = trail_dispatch,
};
