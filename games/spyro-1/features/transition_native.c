// games/spyro-1/features/transition_native.c — native feeders for the level
// transition presentation layer: the balloonist / level-exit cross-fade backdrop
// (func_8004F000) and the GS_ExitLevel grayscale freeze (func_8001C694).
//
// func_8004F000 ("cross-fade / edge-fill"): during a level transition or a
// balloonist take-off (GS_Balloonist func_8001E24C, and GS_ExitLevel
// func_8001C694) the game captures the scene into g_SonyImage as a coarse
// vertex-colour mesh, then every tick re-projects + colour-fades it into a
// full-screen GOURAUD mesh (POLY_G3/F3, no texture) drawn behind the balloon
// and the "ENTERING <world>" text — the soft dreamy gradient backdrop. The
// native renderer never fed it, so during the balloonist screen the panel
// showed a flat background-clear fill instead of the gradient.
//
// It writes its prims into the shared polybuf cursor (0x800757B0) exactly like
// the other renderers, so this is a CAPTURE-AND-FEED feeder (cf. tracer /
// dragonstar): run the real .s once (verdict / state untouched), bracket the
// polybuf, decode the screen-space gouraud tris it emitted, and feed them into
// the native scene as the backmost opaque layer. The mesh is already 2D screen
// space (GTE-projected), so there is nothing to re-derive in float — we replay
// the projected tris verbatim, held static across sub-tick presents (the warp
// animates slowly; a 33 ms hold is imperceptible and avoids strobing the
// backdrop between canonical and intermediate frames).
//
// Display-only / oracle-inert: the real renderer still runs (it owns the
// gameplay verdict + g_SonyImage state); we only read the prims it left in the
// polybuf and draw our own copy.
//
// func_8001C694 (GS_ExitLevel): case 0 is the green-tinted grayscale freeze of
// the level being exited — the SAME effect as the pause/inventory freeze
// backdrop, reproduced from the native renderer's own last world frame via
// openpete_native_scene_set_freeze_backdrop (its FT4s go through the unhooked
// world-OT add func_800168A0, so native never showed them); case 1 is the
// cross-fade, fed by the func_8004F000 hook. The OPENPETE_TRANSITION_PROBE=1 red
// probe is retained as a diagnostic. See notes/plans/openpete-native-renderer.md.

#include "feature_registry.h"
#include "psx_runtime.h"            /* CPUState */
#include "openpete_native_render.h" /* native scene API */
#include "render_paths.h"           /* render_paths_active */
#include "spyro_native_reextract.h" /* transition_native_reextract decl */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"

extern void     func_8004F000(CPUState* cpu);   /* the real cross-fade renderer */
extern void     func_8001C694(CPUState* cpu);   /* the real GS_ExitLevel screen */
extern unsigned openpete_frame;

/* D_800758B8: func_8001C694's case selector — 0 = the green-tinted grayscale
 * freeze of the level being exited; 1 = the colour cross-fade (-> func_8004F000,
 * already fed). (The decomp names it "pause text rotation ticks"; reused here.) */
/* D_8007568C: the per-case fade tick, 0..31 (func_8002E084 increments it each tick,
 * resets at 32 and advances the case). In case 0 it shrinks the grayscale CLUT load
 * (setRECT width 32-tick), fading the frozen level to black. */
/* g_Cyclorama.m_BackgroundColor (g_Cyclorama @0x80078A40 + 0x10): the destination
 * homeworld's flat background colour. In case 1 the game clears the framebuffer to
 * ColorLerp(bg, 0, D_8007575C) = bg*(tick/32) — a linear fade-in from black, drawn
 * BEHIND the cyclorama mesh. */
/* func_8001C694's address span [start, next fn) — to tell the exit-level's nested
 * func_8004F000 call (ra inside this range) from the balloonist's standalone one. */

#define CRED   "\033[1;31m"
#define CRESET "\033[0m"

/* ---- captured cross-fade mesh (screen-space gouraud tris) ----------------- */
typedef struct { float sx[3], sy[3]; uint32_t rgb[3]; int blend; } xtri;
#define MAX_XTRI 8192
static xtri s_cross[MAX_XTRI];
static int  s_cross_n;
/* Backdrop for the warp-pinch gaps. The warp mesh is a 4:3 shape that pinches
 * inward at the bottom sides, so in widescreen (Hor+) its bottom corners don't
 * reach the widened frame and the background clear leaks through. Behind the
 * mesh we draw (1) a flat full-panel quad in the mesh's average colour (catches
 * any area beyond the mesh's bbox at extreme aspects), then (2) a bilinear
 * gradient quad spanning the mesh's bbox with its four CORNER colours — so the
 * fill matches the mesh seamlessly where the warp boundary cuts the corner. */
static uint32_t s_cross_corner[4];   /* TL, TR, BL, BR (PSX 0xBBGGRR) */
static uint32_t s_cross_avg;
static float    s_cross_bbox[4];     /* minx, miny, maxx, maxy */
static int      s_cross_have_bg;
/* Flat backdrop override for the GS_ExitLevel cyclorama fade (case 1). The mesh
 * corner colours used for the widescreen edge-fill are the DPCS-faded cyclorama,
 * near-black early in the fade — so the backdrop grid covers the lerped clear and
 * the homeworld bg appears too late. For the exit-level the true backdrop IS flat
 * (the game clears to ColorLerp(cyclorama_bg, 0)=bg*tick/32), so override the grid
 * with that colour. Bit 31 = set; low 24 = PSX 0xBBGGRR. 0 (balloonist) = use the
 * mesh corners. */
static uint32_t s_cross_bg_flat;

/* Staleness gate (mirrors portal_native): set while the canonical render runs,
 * latched per canonical present by a chained commit, reset there — so the
 * sub-tick reextract stops replaying the moment func_8004F000 stops firing. */
static int  s_cross_ran = 0;
static int  s_cross_canon_drawn = 0;

static int16_t s16(uint32_t v) { return (int16_t)(uint16_t)v; }

/* Walk the polybuf as a linked OT list and decode the gouraud/flat polygon
 * prims. The cursor slot at `start` holds a forward link to the first prim;
 * each prim word0 = tag (high byte = length, low 24 = next-prim link), word1 =
 * code|rgb0, then per-vertex rgb (gouraud) + xy (+uv if textured). The
 * cross-fade emits POLY_G3 (code 0x30, len 6, screen-space, often extending
 * past the 4:3 edges — the warp over-covers). A 0xE1 draw-mode prim updates the
 * current ABR (semi-transparency) mode; quads split into two tris. */
static void crossfade_capture(CPUState* cpu, uint32_t start, uint32_t end) {
    s_cross_n = 0;
    int cur_abr = 0;                         /* ABR mode from the last 0xE1 prim */
    uint32_t p = cpu->read_word(start);      /* head link -> first prim tag */
    int guard = 0;
    while (p >= start && p < end && guard++ < MAX_XTRI * 2) {
        uint32_t tag = cpu->read_word(p);
        uint32_t taglen = (tag >> 24) & 0xFF;
        uint32_t link24 = tag & 0x00FFFFFFu;
        uint32_t next = (p & 0xFF000000u) | link24;     /* restore the KSEG high byte */
        if (link24 == 0x00FFFFFFu) taglen = 0;          /* OT terminator: decode nothing, stop */
        if (taglen == 0 || taglen > 12) break;          /* not a prim / end */
        uint32_t w1 = cpu->read_word(p + 4);
        uint8_t code = (uint8_t)(w1 >> 24);

        if ((code & 0xE0) == 0x20) {                     /* GPU polygon */
            int gouraud = (code & 0x10) != 0;
            int quad    = (code & 0x08) != 0;
            int tex     = (code & 0x04) != 0;
            int stp     = (code & 0x02) != 0;
            int nv = quad ? 4 : 3;
            uint32_t rgb0 = w1 & 0xFFFFFF;
            float    x[4], y[4];
            uint32_t rgb[4];
            int wi = 2;                                   /* word index (word1 done) */
            for (int k = 0; k < nv; k++) {
                rgb[k] = (gouraud && k > 0) ? (cpu->read_word(p + (uint32_t)wi++ * 4) & 0xFFFFFF) : rgb0;
                uint32_t xy = cpu->read_word(p + (uint32_t)wi++ * 4);
                x[k] = (float)s16(xy & 0xFFFF);
                y[k] = (float)s16(xy >> 16);
                if (tex) wi++;                            /* skip uv/clut/tpage word */
            }
            /* blend: opaque unless the poly carries STP, then the current ABR
             * (0 -> (B+F)/2 alpha, 1 -> additive, 2 -> subtractive). The
             * cross-fade backdrop is opaque in practice; STP is honoured for
             * faithfulness if a prim ever sets it. */
            int blend = 0;
            if (stp) blend = (cur_abr == 1) ? 2 : (cur_abr == 2) ? 3 : 1;
            int tri_idx[2][3] = { {0,1,2}, {1,3,2} };    /* quad -> 2 tris */
            int ntri = quad ? 2 : 1;
            for (int ti = 0; ti < ntri && s_cross_n < MAX_XTRI; ti++) {
                xtri* t = &s_cross[s_cross_n++];
                for (int k = 0; k < 3; k++) {
                    int vi = tri_idx[ti][k];
                    t->sx[k] = x[vi]; t->sy[k] = y[vi]; t->rgb[k] = rgb[vi];
                }
                t->blend = blend;
            }
        } else if (code == 0xE1) {                        /* DR_MODE: ABR in bits 5-6 */
            cur_abr = (int)((w1 >> 5) & 3);
        }
        if (next == p) break;                             /* self-loop guard */
        p = next;
    }

    /* Backdrop colours: the mesh bbox, its four corner-quadrant average colours
     * (for the bilinear gradient quad), and the overall average (for the flat
     * backstop). Corner quadrants are the 40% nearest each bbox corner in both
     * axes; an empty quadrant falls back to the overall average. */
    s_cross_have_bg = 0;
    if (s_cross_n > 0) {
        float minx=1e9f,maxx=-1e9f,miny=1e9f,maxy=-1e9f;
        for (int i = 0; i < s_cross_n; i++)
            for (int k = 0; k < 3; k++) {
                float X=s_cross[i].sx[k], Y=s_cross[i].sy[k];
                if(X<minx)minx=X; if(X>maxx)maxx=X; if(Y<miny)miny=Y; if(Y>maxy)maxy=Y;
            }
        float spanx = maxx-minx; if (spanx<1.0f) spanx=1.0f;
        float spany = maxy-miny; if (spany<1.0f) spany=1.0f;
        float xlo = minx + spanx*0.4f, xhi = maxx - spanx*0.4f;   /* quadrant thresholds */
        float ylo = miny + spany*0.4f, yhi = maxy - spany*0.4f;
        double sr[4]={0,0,0,0}, sg[4]={0,0,0,0}, sb[4]={0,0,0,0}; int sn[4]={0,0,0,0};
        double ar=0, ag=0, ab=0; int an=0;
        for (int i = 0; i < s_cross_n; i++)
            for (int k = 0; k < 3; k++) {
                uint32_t c = s_cross[i].rgb[k];
                double r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                ar+=r; ag+=g; ab+=b; an++;
                float X=s_cross[i].sx[k], Y=s_cross[i].sy[k];
                int left = X <= xlo, right = X >= xhi, top = Y <= ylo, bot = Y >= yhi;
                int q = -1;
                if (top && left) q=0; else if (top && right) q=1;
                else if (bot && left) q=2; else if (bot && right) q=3;
                if (q>=0) { sr[q]+=r; sg[q]+=g; sb[q]+=b; sn[q]++; }
            }
        if (an==0) an=1;
        s_cross_avg = (uint32_t)(ar/an) | ((uint32_t)(ag/an)<<8) | ((uint32_t)(ab/an)<<16);
        for (int q = 0; q < 4; q++)
            s_cross_corner[q] = sn[q] ? ((uint32_t)(sr[q]/sn[q]) | ((uint32_t)(sg[q]/sn[q])<<8) | ((uint32_t)(sb[q]/sn[q])<<16))
                                      : s_cross_avg;
        s_cross_bbox[0]=minx; s_cross_bbox[1]=miny; s_cross_bbox[2]=maxx; s_cross_bbox[3]=maxy;
        s_cross_have_bg = 1;
    }
}

/* Feed the captured backdrop mesh as the backmost layer: WORLD-category, far
 * depth (z just inside the far plane) so the depth test keeps it behind the
 * balloon moby and under the HUD text, exactly the PSX draw order (backdrop
 * first, balloon + text over it). Colours are the .s's already-faded vertex
 * colours, so the gradient matches frame-for-frame. */
static void crossfade_feed_native(void) {
    if (s_cross_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);

    /* Backdrop behind the mesh: a 3x3 quad grid that EDGE-CLAMP-extends the
     * mesh's bilinear corner gradient outward to a huge scissor-clipped rect.
     * The centre cell spans the mesh bbox with the real 4-corner gradient (so it
     * continues the mesh seamlessly into the warp-pinch corner gaps); the 8 outer
     * cells repeat the nearest edge/corner colour, so the gradient fills a frame
     * of ANY width — including super-ultrawide (96:9) — with no flat block and no
     * background-clear leak. Hor+ keeps the vertical FOV, so only the horizontal
     * extent ever needs the extension; the verticals are safety margin. */
    if (s_cross_have_bg) {
        const float BIGX = 16384.0f, BIGY = 4096.0f;
        float gx[4] = { -BIGX, s_cross_bbox[0], s_cross_bbox[2], BIGX };
        float gy[4] = { -BIGY, s_cross_bbox[1], s_cross_bbox[3], BIGY };
        /* grid-vertex colour = nearest bbox corner (col 0/1 -> left, 2/3 -> right;
         * row 0/1 -> top, 2/3 -> bottom). s_cross_corner: TL=0,TR=1,BL=2,BR=3.
         * Exit-level case 1 overrides all four with the flat lerped cyclorama bg. */
        uint32_t corner[4] = { s_cross_corner[0], s_cross_corner[1],
                               s_cross_corner[2], s_cross_corner[3] };
        if (s_cross_bg_flat & (1u << 31)) {
            uint32_t f = s_cross_bg_flat & 0xFFFFFFu;
            corner[0] = corner[1] = corner[2] = corner[3] = f;
        }
        const float bz = 0.9995f;
        int qi[2][3] = { {0,1,2}, {1,3,2} };
        for (int ci = 0; ci < 3; ci++)
            for (int ri = 0; ri < 3; ri++) {
                float cx[4] = { gx[ci], gx[ci+1], gx[ci], gx[ci+1] };
                float cy[4] = { gy[ri], gy[ri], gy[ri+1], gy[ri+1] };
                int col[4] = { ci, ci+1, ci, ci+1 }, row[4] = { ri, ri, ri+1, ri+1 };
                for (int ti = 0; ti < 2; ti++) {
                    openpete_scene_tri st;
                    for (int k = 0; k < 3; k++) {
                        int vi = qi[ti][k];
                        int cidx = (row[vi] <= 1 ? 0 : 2) + (col[vi] <= 1 ? 0 : 1);
                        st.v[k].sx = cx[vi]; st.v[k].sy = cy[vi]; st.v[k].z = bz;
                        st.v[k].u = 0.0f; st.v[k].v = 0.0f;
                        st.v[k].rgba = (corner[cidx] & 0xFFFFFFu) | 0xFF000000u;
                    }
                    st.textured = 0;
                    st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
                    st.blend = 0;
                    openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
                }
            }
    }

    const float depth = 0.999f;                           /* far backdrop, still < 1.0 clear */
    for (int i = 0; i < s_cross_n; i++) {
        xtri* t = &s_cross[i];
        openpete_scene_tri st;
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = t->sx[k]; st.v[k].sy = t->sy[k]; st.v[k].z = depth;
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (t->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = t->blend;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    }
}

/* ---- gating + diagnostics ------------------------------------------------- */
static int probe_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TRANSITION_PROBE"); v = (e && *e && *e != '0'); }
    return v;
}
static int dump_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TRANSITION_DUMP"); v = (e && *e && *e != '0'); }
    return v;
}

/* One red line per (func, frame) — a multi-call frame doesn't flood, but a
 * transition that spans many frames logs a burst that brackets it. ra names the
 * caller (func_8001C694 ~0x8001C6.. = GS_ExitLevel; func_8001E24C ~0x8001E2.. =
 * GS_Balloonist). */
static void probe_log(const char* name, const char* what, CPUState* cpu, unsigned* last) {
    if (!probe_on()) return;
    if (*last == openpete_frame) return;
    *last = openpete_frame;
    fprintf(stderr, CRED "[TRANSITION-PROBE f=%u] %s  %s  (ra=0x%08x)" CRESET "\n",
            openpete_frame, name, what, cpu->ra);
}

/* ---- cross-fade hook (func_8004F000): run the real renderer, capture + feed ---- */
static int on_8004F000_crossfade(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int feed = openpete_native_render_enabled();
    static unsigned last_probe = 0xFFFFFFFFu;
    probe_log("func_8004F000", "edge-fill/cross-fade (GS_ExitLevel + GS_Balloonist)", cpu, &last_probe);
    if (!feed && !render_paths_active()) return 0;        /* inert: normal dispatch */

    static int s_in = 0;
    if (s_in) return 0;                                   /* re-entrancy guard */
    s_in = 1;

    /* Balloonist take-off (standalone func_8004F000, ra outside func_8001C694) uses
     * the mesh-corner edge-fill backdrop, not the exit-level's flat cyclorama bg —
     * clear any stale override left from a prior GS_ExitLevel. The exit-level's
     * nested call (ra inside func_8001C694) keeps the override on_8001C694 just set. */
    if (cpu->ra < GS_EXIT_FN_LO || cpu->ra >= GS_EXIT_FN_HI) s_cross_bg_flat = 0;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_8004F000(cpu);                                   /* real renderer (verdict + g_SonyImage) */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    if (feed && endp > startp) {
        crossfade_capture(cpu, startp, endp);
        crossfade_feed_native();
        s_cross_ran = 1;                                  /* mark for the staleness latch */
        if (dump_on()) {
            static int s_rawdumped = 0;
            if (!s_rawdumped) {
                fprintf(stderr, CRED "[transition-RAW f=%u] [%08X,%08X) %u bytes:" CRESET "\n",
                        openpete_frame, startp, endp, endp - startp);
                uint32_t q = startp; int row = 0;
                while (q < endp && row < 24) {
                    fprintf(stderr, "  %08X:", q);
                    for (int c = 0; c < 4 && q < endp; c++, q += 4) fprintf(stderr, " %08X", cpu->read_word(q));
                    fprintf(stderr, "\n"); row++;
                }
                s_rawdumped = 1;
            }
            static int s_dumped = 0;
            if (!s_dumped && openpete_frame >= 1) {
                int blendhist[4] = {0,0,0,0};
                float minx=1e9f,maxx=-1e9f,miny=1e9f,maxy=-1e9f;
                for (int i = 0; i < s_cross_n; i++) {
                    if (s_cross[i].blend>=0 && s_cross[i].blend<4) blendhist[s_cross[i].blend]++;
                    for (int k=0;k<3;k++){ float X=s_cross[i].sx[k],Y=s_cross[i].sy[k];
                        if(X<minx)minx=X; if(X>maxx)maxx=X; if(Y<miny)miny=Y; if(Y>maxy)maxy=Y; }
                }
                fprintf(stderr, CRED "[transition-dump f=%u] polybuf %u bytes -> %d tris  "
                        "extent x[%.0f,%.0f] y[%.0f,%.0f]  blend(op=%d,alpha=%d,add=%d,sub=%d)" CRESET "\n",
                        openpete_frame, endp - startp, s_cross_n, minx, maxx, miny, maxy,
                        blendhist[0], blendhist[1], blendhist[2], blendhist[3]);
                for (int i = 0; i < s_cross_n && i < 4; i++)
                    fprintf(stderr, "    tri[%d] (%.0f,%.0f)=%06X (%.0f,%.0f)=%06X (%.0f,%.0f)=%06X\n", i,
                            s_cross[i].sx[0],s_cross[i].sy[0],s_cross[i].rgb[0],
                            s_cross[i].sx[1],s_cross[i].sy[1],s_cross[i].rgb[1],
                            s_cross[i].sx[2],s_cross[i].sy[2],s_cross[i].rgb[2]);
                s_dumped = 1;
            }
        }
    }
    s_in = 0;
    return 1;
}

/* GS_ExitLevel screen (func_8001C694): case 0 is the green-tinted grayscale
 * FREEZE of the level being exited (4x POLY_FT4 sampling the frozen framebuffer
 * through a grayscale CLUT, tinted setRGB0(0x4C,0x80,0x40), emitted via the
 * unhooked world-OT add func_800168A0 — so native never showed it, just the
 * clear). It is the SAME effect as the pause / inventory / flight-results freeze
 * backdrop, which the native renderer already reproduces from its OWN last world
 * frame (openpete_native_scene_set_freeze_backdrop) — that path just isn't
 * triggered here. So: run the real renderer (verdict + the FT4s for PsyCross),
 * then drive the native freeze backdrop with case 0's hardcoded green tint. The
 * existing overlay carry re-applies it across sub-tick presents. Case 1 (the
 * cross-fade) calls func_8004F000, fed by its own hook above. */
static int on_8001C694(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int feed = openpete_native_render_enabled();
    static unsigned last = 0xFFFFFFFFu;
    probe_log("func_8001C694", "GS_ExitLevel grayscale freeze + cross-fade", cpu, &last);
    if (!feed && !render_paths_active()) return 0;        /* inert: normal dispatch */

    static int s_in = 0;
    if (s_in) return 0;                                   /* re-entrancy guard */
    s_in = 1;

    /* Case 1 backdrop: the game clears to ColorLerp(g_Cyclorama.m_BackgroundColor,
     * 0, D_8007575C). D_8007575C = (0x20-tick)*0x80, and ColorLerp (GTE DPCS toward
     * far-colour 0) gives bg*(1 - D_8007575C/0x1000) = bg*(tick/32) — a linear fade-
     * in from black. Set the flat backdrop BEFORE the nested func_8004F000 feed so
     * the captured cyclorama mesh sits over the lerped homeworld bg (not the near-
     * black faded-mesh corners), making the bg appear as early as the reference. */
    if (feed && cpu->read_word(D_GS_EXIT_MODE) == 1) {    /* case 1: cyclorama fade-in */
        uint32_t tick = cpu->read_word(D_GS_EXIT_TICK);
        if (tick > 31) tick = 31;
        uint32_t bg = cpu->read_word(G_CYCLORAMA_BG_COLOR);
        uint32_t r = (bg & 0xFFu) * tick / 32u;
        uint32_t g = ((bg >> 8) & 0xFFu) * tick / 32u;
        uint32_t b = ((bg >> 16) & 0xFFu) * tick / 32u;
        s_cross_bg_flat = (1u << 31) | r | (g << 8) | (b << 16);
    } else {
        s_cross_bg_flat = 0;                              /* case 0: freeze owns the screen */
    }

    func_8001C694(cpu);                                   /* real renderer (case 1 nests func_8004F000) */

    if (feed && cpu->read_word(D_GS_EXIT_MODE) == 0) {    /* case 0: grayscale freeze */
        /* Fade the frozen level to black over the 32-tick case, matching PSX: the
         * game shrinks the grayscale CLUT load each tick, so a frozen pixel of luma-
         * index i samples palette[i-tick] (subtractive shift). tick = D_8007568C in
         * 0..31 -> shift = tick/31, packed as 0..127 for set_freeze_backdrop. */
        uint32_t tick = cpu->read_word(D_GS_EXIT_TICK);
        if (tick > 31) tick = 31;
        int fade = (int)((tick * 127u + 15u) / 31u);      /* 0..31 -> 0..127 (rounded) */
        openpete_native_scene_begin_frame(openpete_frame);
        openpete_native_scene_set_freeze_backdrop(0x4C, 0x80, 0x40, fade);
    }
    s_in = 0;
    return 1;
}

/* ---- sub-tick re-extract (high-FPS spine): replay the captured backdrop ----
 * The scene is rebuilt against interpolated gamestate on every intermediate
 * present; without re-adding the captured backdrop it would strobe (gradient on
 * canonical frames, background-clear on intermediates). The mesh is screen-space
 * 2D, so we replay it verbatim (no re-projection). Gated on s_cross_canon_drawn
 * so a stale capture isn't painted over the next level once the transition ends. */
void transition_native_reextract(CPUState* cpu) {
    (void)cpu;
    if (!openpete_native_render_enabled()) return;
    if (!s_cross_canon_drawn) return;
    crossfade_feed_native();
}

/* Chained host-present commit: latch "func_8004F000 ran this canonical present"
 * (s_cross_ran) into s_cross_canon_drawn and reset it, then call the previously
 * registered commit. Runs once per canonical present, after the game render and
 * before the sub-tick reextract — so the reextract sees this present's verdict. */
static openpete_native_commit_fn s_prev_commit = NULL;
static void transition_native_commit(void) {
    s_cross_canon_drawn = s_cross_ran;
    s_cross_ran = 0;
    if (s_prev_commit) s_prev_commit();
}

static void transition_native_apply_options(const openpete_options_t* opts) {
    (void)opts;
    s_prev_commit = openpete_native_get_commit();
    openpete_native_register_commit(transition_native_commit);
}

static const feature_dispatch_entry_t transition_dispatch[] = {
    { 0x8004F000u, on_8004F000_crossfade },
    { 0x8001C694u, on_8001C694 },
    { 0u, NULL },
};

const feature_module_t transition_native_module = {
    .name           = "transition_native",
    .apply_options  = transition_native_apply_options,
    .dispatch_table = transition_dispatch,
};
