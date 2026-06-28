// games/spyro-1/features/transition_native.c — native feeder for the level
// transition / balloonist cross-fade backdrop (func_8004F000), + a thin probe
// for the still-unfed GS_ExitLevel grayscale screen (func_8001C694).
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
// func_8001C694 (GS_ExitLevel grayscale screen, 4x POLY_FT4 tiles + a black
// line via the unhooked world-OT add func_800168A0) is still unfed — kept here
// as an opt-in red probe (OPENPETE_TRANSITION_PROBE=1) until its own bk2 fixture
// is recorded. See notes/plans/openpete-native-renderer.md.

#include "feature_registry.h"
#include "psx_runtime.h"            /* CPUState */
#include "openpete_native_render.h" /* native scene API */
#include "render_paths.h"           /* render_paths_active */
#include "spyro_native_reextract.h" /* transition_native_reextract decl */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void     func_8004F000(CPUState* cpu);   /* the real cross-fade renderer */
extern unsigned openpete_frame;

#define POLYBUF_CURSOR 0x800757B0u

#define CRED   "\033[1;31m"
#define CRESET "\033[0m"

/* ---- captured cross-fade mesh (screen-space gouraud tris) ----------------- */
typedef struct { float sx[3], sy[3]; uint32_t rgb[3]; int blend; } xtri;
#define MAX_XTRI 8192
static xtri s_cross[MAX_XTRI];
static int  s_cross_n;

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
}

/* Feed the captured backdrop mesh as the backmost layer: WORLD-category, far
 * depth (z just inside the far plane) so the depth test keeps it behind the
 * balloon moby and under the HUD text, exactly the PSX draw order (backdrop
 * first, balloon + text over it). Colours are the .s's already-faded vertex
 * colours, so the gradient matches frame-for-frame. */
static void crossfade_feed_native(void) {
    if (s_cross_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_category(OPENPETE_SCENE_WORLD);
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
        st.category = OPENPETE_SCENE_WORLD;
        openpete_native_scene_add_tri(&st);
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

/* GS_ExitLevel grayscale screen — still unfed; probe only (returns 0 inert). */
static int on_8001C694(CPUState* cpu, uint32_t addr) {
    (void)addr;
    static unsigned last = 0xFFFFFFFFu;
    probe_log("func_8001C694", "GS_ExitLevel grayscale screen (UNFED)", cpu, &last);
    return 0;
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
