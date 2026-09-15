// games/spyro-1/renderer/transition.c — native feeders for the level
// transition presentation layer: the balloonist / level-exit cross-fade backdrop
// (func_8004F000) and the GS_ExitLevel grayscale freeze (func_8001C694).
//
// func_8004F000 ("cross-fade"): during a level transition or a balloonist
// take-off (GS_Balloonist func_8001E24C, and GS_ExitLevel func_8001C694) the
// game draws the g_Cyclorama dome with every sector colour run through a GTE
// DPCS fade toward D_BLEND_TARGET by D_FOG_FACTOR — the soft dreamy gradient
// backdrop behind the balloon and the "ENTERING <world>" text. It is the same
// dome geometry the main sky renderer (func_8004EBA8) draws, so this feeder
// RE-DERIVES it from gamestate via cyclorama_native_extract_faded rather than
// replaying prims: func_8004F000 culls each face by AND-ing its three vertices'
// outcodes against the 4:3 box [0,512]x[0,256], so the prims it emits stop dead
// at the 4:3 edges and a Hor+ frame was left on background clear beyond them.
//
// The real renderer still runs first (it owns the gameplay verdict and the
// g_SonyImage scratch), and its polybuf interval is still bracketed — that
// range drives the --render-paths OT filter and the RP_TRANSITION liveness
// latch. Only the pixels come from the native extraction.
//
// Display-only / oracle-inert: the extraction only READS g_Cyclorama and
// g_Camera, so no gameplay-readable byte moves.
//
// func_8001C694 (GS_ExitLevel): case 0 is the green-tinted grayscale freeze of
// the level being exited — the SAME effect as the pause/inventory freeze
// backdrop, reproduced from the native renderer's own last world frame via
// openpete_native_scene_set_freeze_backdrop (its FT4s go through the unhooked
// world-OT add func_800168A0, so native never showed them); case 1 is the
// cross-fade, fed by the func_8004F000 hook. The OPENPETE_TRANSITION_PROBE=1 red
// probe is retained as a diagnostic.

#include "feature_registry.h"
#include "psx_runtime_internal.h"            /* CPUState */
#include "openpete_native_render.h" /* native scene API */
#include "renderer/render_paths.h"           /* render_paths_active */
#include "renderer/reextract.h" /* transition_reextract decl */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"
#include "game_layout.h"
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE — fiber-hook cost split */

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
 * BEHIND the cyclorama dome. */
/* func_8001C694's address span [start, next fn) — to tell the exit-level's nested
 * func_8004F000 call (ra inside this range) from the balloonist's standalone one. */

#define CRED   "\033[1;31m"
#define CRESET "\033[0m"

/* ---- the cross-fade dome (screen-space tris) ------------------------------ */
#define MAX_DOME_TRI 8192
static cyclorama_native_tri s_cross[MAX_DOME_TRI];
static int  s_cross_n;
/* Backdrop behind the dome. The dome only covers the sky; below its horizon
 * (and above its cap, when the camera pitches) the PSX shows the draw-env
 * clear. Behind the dome we draw (1) a flat full-panel quad in the dome's
 * average colour (catches any area beyond its bbox at extreme aspects), then
 * (2) a bilinear gradient quad spanning the dome's bbox with its four CORNER
 * colours — so the fill matches the dome seamlessly where its silhouette cuts
 * a corner. */
static uint32_t s_cross_corner[4];   /* TL, TR, BL, BR (PSX 0xBBGGRR) */
static uint32_t s_cross_avg;
static float    s_cross_bbox[4];     /* minx, miny, maxx, maxy */
static int      s_cross_have_bg;
/* Flat backdrop override for the GS_ExitLevel cyclorama fade (case 1). The dome
 * corner colours the backdrop grid is built from are the DPCS-faded cyclorama,
 * near-black early in the fade — so the grid covers the lerped clear and the
 * homeworld bg appears too late. For the exit-level the true backdrop IS flat
 * (the game clears to ColorLerp(cyclorama_bg, 0)=bg*tick/32), so override the grid
 * with that colour. Bit 31 = set; low 24 = PSX 0xBBGGRR. 0 (balloonist) = use the
 * dome corners. */
static uint32_t s_cross_bg_flat;

/* Staleness gate: the hook records its emitted polybuf interval as
 * RP_TRANSITION (liveness unification); the sub-tick reextract gates on
 * render_path_canon_drawn(RP_TRANSITION), which drops the moment
 * func_8004F000 stops emitting. Replaces the old private s_cross_ran /
 * s_cross_canon_drawn latch + chained commit. */

/* Rebuild the dome func_8004F000 just drew, from gamestate, against the ACTIVE
 * frustum. The .s culls each face by AND-ing its three vertices' outcodes
 * against the 4:3 box, so the prims it leaves in the polybuf stop at the 4:3
 * edges and a Hor+ frame showed background clear beyond them; the native
 * extraction iterates the whole 360-degree dome and rejects against the widened
 * bounds instead. Also derives the backdrop stats used behind the dome. */
static void crossfade_extract(CPUState* cpu) {
    /* Runahead lookahead: a speculative tick crossing a level-exit trigger
     * would leave its tris in these host buffers; the rollback does not revert
     * them. The committed re-run re-extracts for real. */
    extern int openpete_speculative;
    if (openpete_speculative) return;
    s_cross_n = cyclorama_native_extract_faded(cpu, s_cross, MAX_DOME_TRI);

    /* Backdrop colours: the dome's bbox, its four corner-quadrant average
     * colours (for the bilinear gradient quad), and the overall average (for the
     * flat backstop). Corner quadrants are the 40% nearest each bbox corner in
     * both axes; an empty quadrant falls back to the overall average. */
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

/* Feed the dome as the backmost layer: WORLD-category, far depth (z just
 * inside the far plane) so the depth test keeps it behind the balloon moby and
 * under the HUD text, exactly the PSX draw order (backdrop first, balloon +
 * text over it). Vertex colours carry the same DPCS fade the .s applies. */
static void crossfade_feed_native(void) {
    if (s_cross_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);

    /* Backdrop behind the dome: a 3x3 quad grid that EDGE-CLAMP-extends the
     * dome's bilinear corner gradient outward to a huge scissor-clipped rect.
     * The centre cell spans the dome's bbox with the real 4-corner gradient (so
     * it continues seamlessly where the dome silhouette cuts a corner); the 8
     * outer cells repeat the nearest edge/corner colour, so the gradient fills a
     * frame of ANY width — including super-ultrawide (96:9) — with no flat block
     * and no background-clear leak beyond the dome's reach. */
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
                    openpete_scene_tri st = {0};
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
        cyclorama_native_tri* t = &s_cross[i];
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = t->sx[k]; st.v[k].sy = t->sy[k]; st.v[k].z = depth;
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (t->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = 0;                                      /* the dome is opaque F3/G3 */
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
    int feed = openpete_native_scene_enabled();
    static unsigned last_probe = 0xFFFFFFFFu;
    probe_log("func_8004F000", "edge-fill/cross-fade (GS_ExitLevel + GS_Balloonist)", cpu, &last_probe);
    if (!feed && !render_paths_active()) return 0;        /* inert: normal dispatch */

    static int s_in = 0;
    if (s_in) return 0;                                   /* re-entrancy guard */
    s_in = 1;

    /* Balloonist take-off (standalone func_8004F000, ra outside func_8001C694) uses
     * the dome-corner backdrop grid, not the exit-level's flat cyclorama bg —
     * clear any stale override left from a prior GS_ExitLevel. The exit-level's
     * nested call (ra inside func_8001C694) keeps the override on_8001C694 just set. */
    if (cpu->ra < GS_EXIT_FN_LO || cpu->ra >= GS_EXIT_FN_HI) s_cross_bg_flat = 0;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    { OP_PROF_ACCUM_SCOPE("transition.srend");
      func_8004F000(cpu); }                               /* real renderer (verdict + g_SonyImage) */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    /* Record the emitted polybuf range: feeds the OT-walk isolation filter
     * AND the canon-drawn mask — the staleness gate the sub-tick replay reads
     * (render_path_canon_drawn(RP_TRANSITION), latched once per canonical
     * present by the shared render_paths commit; empty ranges are dropped, so
     * a frame where the cross-fade emitted nothing reads "not drawn"). */
    render_path_record(RP_TRANSITION, startp, endp);

    if (feed && endp > startp) {
        crossfade_extract(cpu);
        crossfade_feed_native();
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
                float minx=1e9f,maxx=-1e9f,miny=1e9f,maxy=-1e9f;
                for (int i = 0; i < s_cross_n; i++)
                    for (int k=0;k<3;k++){ float X=s_cross[i].sx[k],Y=s_cross[i].sy[k];
                        if(X<minx)minx=X; if(X>maxx)maxx=X; if(Y<miny)miny=Y; if(Y>maxy)maxy=Y; }
                /* The .s's own prim bytes are the 4:3-culled subset; the extent
                 * below is the widened re-derivation actually fed. */
                fprintf(stderr, CRED "[transition-dump f=%u] polybuf %u bytes -> %d dome tris  "
                        "extent x[%.0f,%.0f] y[%.0f,%.0f]" CRESET "\n",
                        openpete_frame, endp - startp, s_cross_n, minx, maxx, miny, maxy);
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
    int feed = openpete_native_scene_enabled();
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
     * the cyclorama dome sits over the lerped homeworld bg (not the near-black
     * faded dome corners), making the bg appear as early as the reference. */
    if (feed && cpu->read_word(D_GS_EXIT_MODE) == 1) {    /* case 1: cyclorama fade-in */
        uint32_t tick = cpu->read_word(D_GS_EXIT_TICK);
        if (tick > 31) tick = 31;
        uint32_t bg = cpu->read_word(game_layout.cyc.bg_colour);
        uint32_t r = (bg & 0xFFu) * tick / 32u;
        uint32_t g = ((bg >> 8) & 0xFFu) * tick / 32u;
        uint32_t b = ((bg >> 16) & 0xFFu) * tick / 32u;
        s_cross_bg_flat = (1u << 31) | r | (g << 8) | (b << 16);
    } else {
        s_cross_bg_flat = 0;                              /* case 0: freeze owns the screen */
    }

    { OP_PROF_ACCUM_SCOPE("transition.srend");
      func_8001C694(cpu); }                               /* real renderer (case 1 nests func_8004F000) */

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

/* ---- sub-tick re-extract (high-FPS spine): re-derive the backdrop ---------
 * The scene is rebuilt against interpolated gamestate on every intermediate
 * present; without re-adding the backdrop it would strobe (dome on canonical
 * frames, background-clear on intermediates). Re-running the extraction (rather
 * than replaying the canonical tris) reads g_Camera's matrices live, so the dome
 * turns smoothly with the bracketed sub-tick camera — the same property the main
 * sky's re-extract has. Gated on render_path_canon_drawn(RP_TRANSITION) —
 * latched once per canonical present by the shared render_paths commit from the
 * hook's recorded polybuf interval — so the dome isn't painted over the next
 * level once the transition ends (the hook stops emitting → the mask bit drops). */
void transition_reextract(CPUState* cpu) {
    if (!openpete_native_scene_enabled()) return;
    if (!render_path_canon_drawn(RP_TRANSITION)) return;
    crossfade_extract(cpu);
    crossfade_feed_native();
    {   /* OPENPETE_LOAD_CENSUS: replay-fire census (liveness-gate verification). */
        static int cen = -1;
        if (cen < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); cen = (e && *e && *e != '0'); }
        static unsigned s_fires = 0; s_fires++;
        static unsigned last = 0xFFFFFFFFu;
        if (cen && openpete_frame / 100u != last) { last = openpete_frame / 100u;
            fprintf(stderr, "[census f=%u TRANSITION-REPLAY] fires=%u\n", openpete_frame, s_fires); }
    }
}

static void transition_native_apply_options(const openpete_options_t* opts) {
    (void)opts;
}

static const feature_dispatch_entry_t transition_dispatch[] = {
    { 0x8004F000u, on_8004F000_crossfade },
    { 0x8001C694u, on_8001C694 },
    { 0u, NULL },
};

const feature_module_t transition_module = {
    .name           = "transition_native",
    .apply_options  = transition_native_apply_options,
    .dispatch_table = transition_dispatch,
};
