/* core/runtime/native_render.c — native Vulkan renderer A/B harness.
 *
 * Phase 1c of notes/plans/openpete-native-renderer.md. See
 * openpete_native_render.h for the contract + the two-window rationale.
 *
 * 1c.1: RHI in the runtime, Vulkan window alongside PsyCross.
 * 1c.2: PsyCross hidden; its frame read back + shown in the single window.
 * 1c.3a: mirror PsyCross's VRAM into a native R16_UINT texture each present
 *   (proving real game VRAM reaches the native GPU path).
 * 1c.3b (this step): panel 0 renders the PSX DISPLAY REGION cropped to the
 *   visible viewport. The DISPENV disp rect (512x240, from PutDispEnv) and the
 *   DRAWENV clip rect (512x224, from PutDrawEnv) are captured; the panel
 *   samples the full disp width but only clip.h rows, dropping the symmetric
 *   8px overscan bands, and is fitted at the
 *   reference aspect so it lines up with the reference panel. The front-buffer
 *   origin follows the per-frame double-buffer flip via the material tpage.
 *   Layout: [native-display | reference]. 1c.3b-ii adds the diff panel +
 *   validates against the logo screen.
 *
 * Determinism: nothing here touches PSX RAM or the `.s` verdict producers.
 */

#include "openpete_native_render.h"

#include "openpete_log.h"
#include "rhi.h"
#include "screenshot.h"  /* openpete_write_png_rgba */

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>   /* snprintf */
#include <stdlib.h>
#include <string.h>

/* PsyCross VRAM + present-FBO readback (extern "C" in PsyX_render.cpp);
 * forward-declared so this C TU need not include the C++-flavored header.
 * GR_ReadVRAM copies a rect of the 1024x512 CPU VRAM mirror tightly into dst;
 * GR_ReadPresentSceneRGBA returns the present FBO bottom-up (GL origin). */
extern int  GR_GetPresentSceneSize(int *out_w, int *out_h);
extern int  GR_ReadPresentSceneRGBA(unsigned char *pixels, int w, int h,
                                    unsigned int gl_format);
extern void GR_ReadVRAM(unsigned short *dst, int x, int y, int dst_w, int dst_h);

/* GL_RGBA enum — byte order R,G,B,A, matching VK_FORMAT_R8G8B8A8_UNORM. */
#define NR_GL_RGBA 0x1908u

#define NR_VRAM_W 1024
#define NR_VRAM_H 512

/* Default window size when the caller passes none. */
#define NR_DEFAULT_W 1280
#define NR_DEFAULT_H 720

/* Per-draw PSX material — must match the push_constant block in
 * shaders/psx_sample.frag (five tightly-packed 32-bit ints). */
typedef struct {
    int32_t tpage_x, tpage_y;
    int32_t clut_x, clut_y;
    int32_t bpp;
} psx_material;

/* How many panels the window shows (1c.3a: VRAM + reference). */
#define NR_PANELS 2

/* Native 3D scene (Phase 2): screen-space triangles the game pushes in
 * (cyclorama/sky from gamestate). Double-buffered isn't needed — the game
 * fills it during its render pass (before openpete_native_render_frame reads
 * it). begin() clears; add_tri() appends. */
#define MAX_SCENE_TRIS 16384
#define SCENE_VB_VERTS (MAX_SCENE_TRIS * 3 + 12)  /* +6 bg-fill quad, +6 fade overlay quad */
static openpete_scene_tri s_scene[MAX_SCENE_TRIS];      /* accumulating (frame N) */
static int                s_scene_n;
static uint32_t           s_scene_bg;  /* panel-0 background fill (rgba); 0 = none */

/* Reference frame-sync: the native scene is built from the FRESHLY-computed
 * geometry (frame N), but PsyCross's reference shows its display FBO, which lags
 * the native geometry (the OT PsyCross draws is the one Spyro built a frame or
 * two earlier — PSX double-buffer + the build-vs-draw split). Comparing the
 * native scene against the reference as-is puts them N frames apart — in A/B the
 * two panels are skewed; in diff EVERY moving object ghosts (it appears at both
 * frames in |native - ref|). To keep native and the reference on the SAME frame
 * we delay the native scene by an integer DEPTH: keep a ring of recent completed
 * frames and present the one `depth` frames back. Default depth in any
 * reference-compare mode = AB_DIFF_SCENE_DELAY (measured to align with the lagged
 * reference); vk-only has no reference, so depth 0 (live frame, runs ahead).
 * OPENPETE_NATIVE_SCENE_DELAY=<n> overrides with an explicit integer depth. */
#define SCENE_HIST 4   /* ring of completed-frame snapshots; supports delay depth 0..3 */
#define AB_DIFF_SCENE_DELAY 2   /* ab/diff default: native lagged 2 frames to match PsyCross */
static openpete_scene_tri s_scene_hist[SCENE_HIST][MAX_SCENE_TRIS]; /* completed-frame ring */
static int                s_scene_hist_n[SCENE_HIST];
static uint32_t           s_scene_hist_bg[SCENE_HIST];
static uint32_t           s_scene_hist_fade[SCENE_HIST];  /* full-screen fade overlay, per snapshot */
static int                s_scene_hist_head = -1;  /* ring index of the most-recent completed frame */
static int                s_scene_hist_count = 0;  /* completed frames recorded (warmup guard) */
static int                s_scene_delay = -2;   /* -2 uninit; >=0 = depth */
static int                s_scene_ref_compare = 1; /* set by init: 1 = ab/diff (lagged reference), 0 = vk-only */

static int scene_delay_depth(void) {
    if (s_scene_delay < -1) {
        const char* e = getenv("OPENPETE_NATIVE_SCENE_DELAY");
        /* Explicit env wins (integer depth); otherwise default to the measured
         * ab/diff lag so native and the lagged reference are the same frame.
         * vk-only has no reference → depth 0 (live frame). */
        if (e && *e) { int d = atoi(e); s_scene_delay = (d < 0) ? 0 : (d >= SCENE_HIST ? SCENE_HIST - 1 : d); }
        else         s_scene_delay = s_scene_ref_compare ? AB_DIFF_SCENE_DELAY : 0;
    }
    return s_scene_delay;
}

/* Current draw layer stamped onto every add_tri (OPENPETE_SCENE_WORLD/_ACTOR).
 * Reset to WORLD per frame so a non-actor feeder never inherits a stale ACTOR. */
static int s_scene_category = OPENPETE_SCENE_WORLD;

/* Full-screen fade overlay for the current frame (func_800190D4). 0 = none; else
 * bit31 = active, bits[25:24] = PSX ABR mode (0..3), bits[23:0] = rgb (R in 16..23,
 * G in 8..15, B in 0..7). Reset every frame; the present draws it dead last. */
static uint32_t s_scene_fade = 0;

void openpete_native_scene_begin(void) { s_scene_n = 0; s_scene_bg = 0; s_scene_fade = 0; s_scene_category = OPENPETE_SCENE_WORLD; }

/* Frame-keyed reset: the first feeder of each game frame clears; later feeders
 * (in any order) append, so the cyclorama + world + mobys compose into one
 * scene instead of clobbering each other. See the header for the rationale.
 * On a frame change, the just-completed scene (still in s_scene) is snapshotted
 * to s_scene_disp before the clear, so the present can draw the 1-frame-delayed
 * copy that matches the reference panel. */
static unsigned s_scene_frame = 0xFFFFFFFFu;
void openpete_native_scene_begin_frame(unsigned frame) {
    if (frame != s_scene_frame) {
        if (s_scene_frame != 0xFFFFFFFFu) {  /* a completed frame is sitting in s_scene */
            int slot = (s_scene_hist_head + 1) % SCENE_HIST;
            if (s_scene_n > 0) memcpy(s_scene_hist[slot], s_scene, (size_t)s_scene_n * sizeof(s_scene[0]));
            s_scene_hist_n[slot] = s_scene_n; s_scene_hist_bg[slot] = s_scene_bg;
            s_scene_hist_fade[slot] = s_scene_fade;
            s_scene_hist_head = slot;
            if (s_scene_hist_count < SCENE_HIST) s_scene_hist_count++;
        }
        s_scene_n = 0; s_scene_bg = 0; s_scene_fade = 0; s_scene_frame = frame;
        s_scene_category = OPENPETE_SCENE_WORLD;
    }
}
void openpete_native_scene_add_tri(const openpete_scene_tri *tri) {
    if (tri && s_scene_n < MAX_SCENE_TRIS) {
        s_scene[s_scene_n] = *tri;
        s_scene[s_scene_n].category = s_scene_category;  /* stamp the active layer */
        s_scene_n++;
    }
}
void openpete_native_scene_set_category(int category) { s_scene_category = category; }
void openpete_native_scene_set_bg(uint32_t rgba) { s_scene_bg = rgba; }
void openpete_native_scene_set_fade(int mode, int r, int g, int b) {
    if (mode < 0) { s_scene_fade = 0; return; }
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    s_scene_fade = (1u << 31) | ((uint32_t)(mode & 3) << 24)
                 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* The scene buffer the present should draw: the depth-frames-back snapshot when
 * a reference-compare delay is set (A/B-synced with the lagged reference), else
 * the live accumulator (vk-only, runs ahead). Falls back to the live frame
 * during warmup before enough history has accumulated. */
static const openpete_scene_tri* scene_present_buf(int* out_n, uint32_t* out_bg, uint32_t* out_fade) {
    int depth = scene_delay_depth();
    if (depth > 0 && depth <= s_scene_hist_count && s_scene_hist_head >= 0) {
        int slot = (s_scene_hist_head - (depth - 1) + SCENE_HIST * 4) % SCENE_HIST;
        *out_n = s_scene_hist_n[slot]; *out_bg = s_scene_hist_bg[slot];
        *out_fade = s_scene_hist_fade[slot]; return s_scene_hist[slot];
    }
    *out_n = s_scene_n; *out_bg = s_scene_bg; *out_fade = s_scene_fade; return s_scene;
}

/* Single native renderer instance (one window per process). */
static struct {
    bool          enabled;
    bool          ab_mode;      /* true = A/B compare (2 panels + reference); false = vk-only */
    int           n_panels;     /* 2 in A/B, 1 in vk-only */
    SDL_Window   *window;
    rhi_device   *dev;
    int           win_w, win_h; /* last-known pixel size, for resize detection */

    /* Reference panel: PsyCross's read-back frame (RGBA8). */
    rhi_pipeline *blit_pipe;
    rhi_texture  *ref_tex;
    int           ref_w, ref_h;
    unsigned char *readback;    /* CPU readback scratch, ref_w*ref_h*4 */
    size_t        readback_cap;

    /* The native-display panel samples the PSX VRAM mirror (R16_UINT) through
     * the ported PSX sampler. */
    rhi_pipeline *vram_pipe;
    rhi_texture  *vram_tex;
    uint16_t     *vram_mirror;  /* NR_VRAM_W*NR_VRAM_H */

    /* Native 3D render (Phase 2): gouraud triangles (quad.vert/quad.frag) for
     * the cyclorama/sky scene the game pushes in. Replaces the VRAM re-display
     * in panel 0 when a scene is present. */
    rhi_pipeline *gouraud_pipe;
    rhi_pipeline *gouraud_blend_pipe; /* 3b.2: semi-transparent UNTEXTURED gouraud (alpha, ABR mode 0) */
    rhi_pipeline *gouraud_sub_pipe;   /* 3b.2: subtractive UNTEXTURED gouraud (ABR mode 2, B-F) — Spyro shadow */
    rhi_pipeline *gouraud_add_pipe;   /* 3b.3: additive UNTEXTURED gouraud (ABR mode 1, B+F) — glow rings */
    rhi_pipeline *scene_tex_pipe;     /* 3a.3: opaque textured scene tris (VRAM sampler) */
    rhi_pipeline *scene_tex_blend_pipe; /* 3a.3: semi-transparent (ABE) textured tris (alpha, ABR mode 0) */
    rhi_pipeline *scene_tex_add_pipe; /* 3b.1: additive (ABR mode 1, B+F) textured tris — flame */
    rhi_pipeline *scene_tex_sub_pipe; /* 3b.2: subtractive (ABR mode 2, B-F) textured tris — moby shadows */
    rhi_pipeline *gouraud_hud_pipe;   /* HUD overlay: UNTEXTURED gouraud, depth-test OFF (on top) */
    rhi_pipeline *scene_tex_hud_pipe; /* HUD overlay: textured (VRAM sampler), depth-test OFF (on top) */
    rhi_buffer   *scene_vb;     /* MAX_SCENE_TRIS*3 verts, rebuilt per frame */
    float         p0_cx, p0_hx, p0_hy; /* panel-0 fitted rect (clip space, cy=0) */

    /* Active PSX display region (DISPENV `disp` rect, VRAM px), from the
     * PutDispEnv thunk. The full disp is 512x240 double-buffered (origin flips
     * per frame). */
    int           disp_x, disp_y, disp_w, disp_h;
    /* DRAWENV `clip` rect (VRAM px), from the PutDrawEnv thunk: the actually-
     * drawn region. The visible viewport is clip.h tall (224) within the
     * 240-tall disp; the 8px bands top+bottom are garbage overscan. Used to
     * crop the native-display vertically to match the bars-free reference. */
    int           clip_x, clip_y, clip_w, clip_h;

    /* Panel geometry: one VB of NR_PANELS*6 verts, rebuilt only when a panel's
     * content SIZE changes. The disp/clip origins flip every frame (double
     * buffer) but don't change geometry — they ride the per-frame material
     * (tpage), so layout keys on sizes only. */
    rhi_buffer   *panels_vb;
    int           layout_win_w, layout_win_h, layout_ref_w, layout_ref_h;
    int           layout_disp_w, layout_clip_h;

    /* Diff mode (--renderer diff): render the native scene into native_rtt (a
     * sampleable render target at the reference resolution), then a fullscreen
     * diff pass samples native_rtt (set 0) + ref_tex (set 1) and writes
     * |native - ref| into the offscreen. diff_quad_vb is the panel-0 rect with
     * 0..1 UVs that the diff pass draws. */
    bool          diff_mode;
    bool          stats_enabled;   /* --render-stats: per-frame diff readback + stats */
    rhi_pipeline *diff_pipe;
    rhi_texture  *native_rtt;
    int           rtt_w, rtt_h;
    rhi_buffer   *diff_quad_vb;
} s_nr;

/* When set, draw_native_scene maps the PSX screen to the FULL render target
 * (clip -1..1) instead of panel-0's window rect — used when rendering into
 * native_rtt for the diff pass (native_rtt IS the frame, no panel inset). */
static int s_render_full_extent = 0;

/* Path-isolation mode (--render-paths, set by the game). When on, an empty
 * native scene presents the background instead of re-displaying the PSX VRAM
 * framebuffer — the VRAM is unfiltered, so falling back to it would defeat the
 * isolation. Generic core toggle; no-op until set. */
static bool s_isolation = false;
void openpete_native_set_isolation(bool on) { s_isolation = on; }

/* Diff colour-stats (--render-stats). */
#define DIFF_STATS_TOL 8           /* per-channel "match" tolerance (of 255) */
static struct {
    double   sum[3];               /* per-channel abs-diff sums over all counted px */
    uint64_t n;                    /* total counted (panel) px across frames */
    int      max[3];               /* per-channel max abs diff seen */
    uint64_t within;               /* px with max-channel diff <= TOL */
    uint64_t off;                  /* px with max-channel diff >  TOL (the differing ones) */
    unsigned frames;               /* frames sampled */
} s_diff_stats;
static unsigned char *s_stats_buf = NULL;   /* offscreen readback scratch */
static size_t         s_stats_cap = 0;

/* Read the just-presented diff offscreen back and fold its panel-rect pixels into
 * the running stats; log a throttled per-frame line. */
static void diff_stats_accumulate(void)
{
    if (s_nr.win_w <= 0 || s_nr.win_h <= 0) return;
    size_t cap = (size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u;
    if (cap > s_stats_cap) {
        free(s_stats_buf);
        s_stats_buf = malloc(cap);
        s_stats_cap = s_stats_buf ? cap : 0;
    }
    if (!s_stats_buf) return;
    int w = 0, h = 0;
    if (!rhi_read_offscreen(s_nr.dev, s_stats_buf, cap, &w, &h) || w <= 0 || h <= 0) return;

    /* Panel-0 pixel rect (exclude the letterbox bars, which are a 0 diff). */
    int sx = (int)(((s_nr.p0_cx - s_nr.p0_hx) * 0.5f + 0.5f) * (float)w + 0.5f);
    int sw = (int)((s_nr.p0_hx) * (float)w + 0.5f);
    int sy = (int)((0.5f - 0.5f * s_nr.p0_hy) * (float)h + 0.5f);
    int sh = (int)((s_nr.p0_hy) * (float)h + 0.5f);
    if (sx < 0) sx = 0; if (sy < 0) sy = 0;
    if (sx + sw > w) sw = w - sx; if (sy + sh > h) sh = h - sy;
    if (sw <= 0 || sh <= 0) return;

    double fsum[3] = {0, 0, 0};
    uint64_t fn = 0, fwithin = 0, foff = 0;
    int fmax[3] = {0, 0, 0};
    for (int y = sy; y < sy + sh; y++) {
        const unsigned char* row = &s_stats_buf[(size_t)y * w * 4u];
        for (int x = sx; x < sx + sw; x++) {
            const unsigned char* px = &row[(size_t)x * 4u];
            int r = px[0], g = px[1], b = px[2];
            fsum[0] += r; fsum[1] += g; fsum[2] += b;
            if (r > fmax[0]) fmax[0] = r;
            if (g > fmax[1]) fmax[1] = g;
            if (b > fmax[2]) fmax[2] = b;
            int mc = r; if (g > mc) mc = g; if (b > mc) mc = b;
            if (mc > DIFF_STATS_TOL) foff++; else fwithin++;
            fn++;
        }
    }
    if (fn == 0) return;

    for (int c = 0; c < 3; c++) {
        s_diff_stats.sum[c] += fsum[c];
        if (fmax[c] > s_diff_stats.max[c]) s_diff_stats.max[c] = fmax[c];
    }
    s_diff_stats.n += fn; s_diff_stats.within += fwithin; s_diff_stats.off += foff;
    s_diff_stats.frames++;

    if ((s_diff_stats.frames % 60u) == 1u) {  /* throttled per-frame line */
        OPL_INFO_F("native", "diff-stats: mean=(%.1f,%.1f,%.1f) max=(%d,%d,%d) "
                   "within%d=%.1f%% diff-px=%llu/%llu",
                   fsum[0]/fn, fsum[1]/fn, fsum[2]/fn, fmax[0], fmax[1], fmax[2],
                   DIFF_STATS_TOL, 100.0 * (double)fwithin / (double)fn,
                   (unsigned long long)foff, (unsigned long long)fn);
    }
}

/* Fill a panel quad: clip-space rect centered at (cx,cy) with half-extents
 * (hx,hy); v0 is the UV-v at the TOP edge, v1 at the BOTTOM. (VRAM is uploaded
 * top-down so it uses v0<v1; the GL readback is bottom-up so the reference
 * uses v0>v1.) rgba is unused by the textured shaders. */
static void build_panel(rhi_vertex out[6], float cx, float cy, float hx, float hy,
                        float u0, float v0, float u1, float v1)
{
    const rhi_vertex tl = { cx - hx, cy + hy, 0.0f, u0, v0, 0xFFFFFFFFu, 0 };
    const rhi_vertex tr = { cx + hx, cy + hy, 0.0f, u1, v0, 0xFFFFFFFFu, 0 };
    const rhi_vertex br = { cx + hx, cy - hy, 0.0f, u1, v1, 0xFFFFFFFFu, 0 };
    const rhi_vertex bl = { cx - hx, cy - hy, 0.0f, u0, v1, 0xFFFFFFFFu, 0 };
    out[0] = tl; out[1] = tr; out[2] = br;
    out[3] = tl; out[4] = br; out[5] = bl;
}

/* Aspect-fit content (content_w x content_h px) inside a clip-space cell
 * centered at (cx,cy) with clip half-extents (chx,chy), returning the fitted
 * half-extents (letterbox/pillarbox; the clear fills the bars). */
static void fit_cell(float cx, float cy, float chx, float chy,
                     int content_w, int content_h, int win_w, int win_h,
                     float *out_hx, float *out_hy)
{
    (void)cx; (void)cy;
    float cell_w = chx * (float)win_w;  /* cell size in px (full clip = win px) */
    float cell_h = chy * (float)win_h;
    float s = cell_w / (float)content_w;
    float sy = cell_h / (float)content_h;
    if (sy < s) s = sy;
    *out_hx = ((float)content_w * s) / (float)win_w;
    *out_hy = ((float)content_h * s) / (float)win_h;
}

int openpete_native_render_init(const openpete_options_t *opts, int win_w, int win_h)
{
    if (!opts || opts->renderer == OPENPETE_RENDERER_PSYCROSS)
        return 0; /* PsyCross owns the display — native window inert */

    if (opts->headless) {
        OPL_WARN_F("native", "--renderer vk|ab|diff ignored under --headless (no display)");
        return 0;
    }

    /* A/B compare mode shows the PsyCross reference in a second panel and delays
     * the native scene by one frame to align with that lagged reference. vk and
     * diff are single full-window panels with no reference panel and no scene
     * delay. (diff's live colour-diff render + diff screenshot land in a later
     * step; for now diff mirrors vk so the flag is usable.) */
    bool ab_mode = (opts->renderer == OPENPETE_RENDERER_AB);
    bool diff_mode = (opts->renderer == OPENPETE_RENDERER_DIFF);
    s_nr.ab_mode  = ab_mode;
    s_nr.diff_mode = diff_mode;
    s_nr.stats_enabled = diff_mode && opts->render_stats;  /* per-frame diff readback */
    s_nr.n_panels = ab_mode ? 2 : 1;   /* vk/diff are single full-window panels */
    /* Delay the native scene one frame whenever we compare against the lagged
     * PsyCross reference — A/B (panel sync) AND diff (else moving objects ghost
     * across native frame N vs reference frame N-1). vk-only has no reference. */
    s_scene_ref_compare = (ab_mode || diff_mode) ? 1 : 0;

    /* SDL_INIT_VIDEO is already up (PsyX_Initialise). Create a SECOND window
     * for the Vulkan surface: a single SDL window can't carry both PsyCross's
     * GL context and our Vulkan surface, so the native renderer owns its own.
     * PsyCross's GL window is started hidden (see run.c), so this Vulkan
     * window is the single visible one. Match the game's resolved window size
     * so it isn't surprisingly larger than the PsyCross-only window. */
    int w = (win_w > 0) ? win_w : NR_DEFAULT_W;
    int h = (win_h > 0) ? win_h : NR_DEFAULT_H;

    /* A/B shows two panels side-by-side, so widen the window 2x by default —
     * each panel then gets the game's full width instead of being squeezed into
     * half. vk/diff are single full-window panels and keep the resolved size.
     * An explicit --window is treated as the intended TOTAL window size and is
     * left alone (the user picked it deliberately). */
    if (ab_mode && opts->window_w <= 0)
        w *= 2;

    const char *title = ab_mode   ? "openpete — native renderer (A/B)"
                      : diff_mode ? "openpete — native renderer (diff)"
                                  : "openpete — native renderer";
    SDL_Window *win = SDL_CreateWindow(title, w, h,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        OPL_ERROR_F("native", "SDL_CreateWindow(VULKAN) failed: %s", SDL_GetError());
        return 1;
    }

    rhi_device *dev = rhi_create(win, "openpete-native");
    if (!dev) {
        OPL_ERROR_F("native", "rhi_create failed; continuing without native A/B");
        SDL_DestroyWindow(win);
        return 1;
    }

    rhi_pipeline_desc blit_desc = {
        .vert_spv = "psx_sample.vert.spv", /* passthrough pos + UV */
        .frag_spv = "blit.frag.spv",       /* sampler2D at set 0 */
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = 0,
    };
    rhi_pipeline_desc vram_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "psx_sample.frag.spv", /* usampler2D + material push const */
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* Gouraud pipeline for the native 3D scene: vertex-coloured, untextured
     * (quad.vert/quad.frag from 1b.1) — exactly the cyclorama dome's shading. */
    rhi_pipeline_desc gouraud_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,  /* 3a.3: sort actors near→far */
        .uses_vram = false, .push_const_size = 0,
    };
    /* Semi-transparent UNTEXTURED gouraud pipeline (3b.2): the Spyro silhouette
     * shadow (func_80059A48) is a POLY_G3 strip with ABE set (ABR mode 0,
     * (B+F)/2) — untextured, so it can't go through the textured-blend pipe.
     * RHI_BLEND_ALPHA = src*a + dst*(1-a); the feeder sets vertex alpha = 128
     * (~0.5) so the result is (B+F)/2, reproducing mode 0. depth_write_off: the
     * shadow tests against opaque geometry (a wall/ground occludes it) but
     * never occludes another transparent layer. Same quad.vert/quad.frag as the
     * opaque gouraud pipe — quad.frag emits the full vertex rgba incl. alpha. */
    rhi_pipeline_desc gouraud_blend_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = true, .depth_write_off = true,
        .uses_vram = false, .push_const_size = 0,
    };
    /* Subtractive UNTEXTURED gouraud pipeline (3b.2, the Spyro silhouette
     * shadow): POLY_G3 with ABR mode 2 (dst = B - F). RHI_BLEND_SUBTRACT
     * reverse-subtracts the vertex colour from the framebuffer, darkening the
     * ground (the shadow grey is subtracted; black vertices leave it
     * unchanged). depth_write_off like the other transparent passes. */
    rhi_pipeline_desc gouraud_sub_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = true, .depth_write_off = true,
        .uses_vram = false, .push_const_size = 0,
    };
    /* Additive UNTEXTURED gouraud pipeline (3b.3, the glow rings): POLY_G3 with
     * ABR mode 1 (dst = B + F). RHI_BLEND_ADD = src ONE + dst ONE. The glow is a
     * gouraud fan, bright centre colour fading to black ring vertices, so the
     * additive accumulation peaks at the centre and vanishes at the edge — the
     * classic halo. depth_write_off like the other transparent passes. */
    rhi_pipeline_desc gouraud_add_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_ADD, .depth_test = true, .depth_write_off = true,
        .uses_vram = false, .push_const_size = 0,
    };
    /* Textured scene pipeline (3a.3): samples the PSX VRAM mirror via the ported
     * decoder + per-draw material, modulates by the vertex colour (tex*col/128).
     * Reuses psx_sample.vert (clip-space passthrough + uv + colour). */
    rhi_pipeline_desc scene_tex_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* Semi-transparent (ABE) textured pipeline: alpha blend, reproducing PSX ABR
     * mode 0 ((B+F)/2) with per-texel STP gating via psx_scene.frag's STP alpha.
     * depth_write_off: transparent tris Z-TEST against opaque but do NOT write
     * depth, so they never occlude each other (correct ordering still needs the
     * back-to-front sort, 3c; this is the prerequisite half). */
    rhi_pipeline_desc scene_tex_blend_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* Additive (ABR mode 1, dst = B + F) textured pipeline (3b.1): the first
     * additive consumer is Spyro's flame (func_80058D64 / r_flame.s), a textured
     * gouraud quad ribbon whose tpage selects ABR=1. RHI_BLEND_ADD = src ONE +
     * dst ONE; depth_write_off so the glow tests against opaque geometry (a wall
     * occludes the flame) but additive layers never occlude each other. The
     * grey-ramp vertex colour modulates the flame sprite (tex*col/128) and the
     * result accumulates into the framebuffer — bright core, never darkens. */
    rhi_pipeline_desc scene_tex_add_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_ADD, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* Subtractive (ABR mode 2, dst = B - F) textured pipeline (3b.2): moby
     * shadows (func_80059F8C), a textured FT3 fan whose tpage selects ABR=2.
     * RHI_BLEND_SUBTRACT reverse-subtracts the modulated shadow sprite from the
     * framebuffer (tex*col/128 darkens the ground), depth_write_off so the
     * shadow tests against opaque geometry but never occludes another layer. */
    rhi_pipeline_desc scene_tex_sub_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* HUD overlay pipelines (depth_test = false): HUD/UI mobys are a 2D overlay
     * drawn LAST, ignoring the depth buffer so they sit on top of all world/actor
     * geometry regardless of their world-space view-Z (the PSX draws HUD in a
     * separate near-OT pass). Painter's order = submission order. One untextured
     * (gouraud) + one textured (VRAM sampler), mirroring the opaque pair. */
    rhi_pipeline_desc gouraud_hud_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = false, .push_const_size = 0,
    };
    rhi_pipeline_desc scene_tex_hud_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* Diff pipeline (--renderer diff): samples two textures — native render at
     * set 0, reference at set 1 — and outputs |native - ref| (diff.frag). No
     * depth, no blend. Built unconditionally so --screenshot-on-frame can dump
     * a -diff.png regardless of the live --renderer mode. */
    rhi_pipeline_desc diff_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "diff.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .uses_vram2 = true, .push_const_size = 0,
    };
    rhi_pipeline *blit_pipe    = rhi_create_pipeline(dev, &blit_desc);
    rhi_pipeline *vram_pipe    = rhi_create_pipeline(dev, &vram_desc);
    rhi_pipeline *gouraud_pipe = rhi_create_pipeline(dev, &gouraud_desc);
    rhi_pipeline *gouraud_blend_pipe = rhi_create_pipeline(dev, &gouraud_blend_desc);
    rhi_pipeline *gouraud_sub_pipe = rhi_create_pipeline(dev, &gouraud_sub_desc);
    rhi_pipeline *gouraud_add_pipe = rhi_create_pipeline(dev, &gouraud_add_desc);
    rhi_pipeline *scene_tex_pipe = rhi_create_pipeline(dev, &scene_tex_desc);
    rhi_pipeline *scene_tex_blend_pipe = rhi_create_pipeline(dev, &scene_tex_blend_desc);
    rhi_pipeline *scene_tex_add_pipe = rhi_create_pipeline(dev, &scene_tex_add_desc);
    rhi_pipeline *scene_tex_sub_pipe = rhi_create_pipeline(dev, &scene_tex_sub_desc);
    rhi_pipeline *gouraud_hud_pipe = rhi_create_pipeline(dev, &gouraud_hud_desc);
    rhi_pipeline *scene_tex_hud_pipe = rhi_create_pipeline(dev, &scene_tex_hud_desc);
    rhi_pipeline *diff_pipe = rhi_create_pipeline(dev, &diff_desc);
    rhi_buffer   *panels_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * NR_PANELS * 6);
    rhi_buffer   *scene_vb  = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * SCENE_VB_VERTS);
    rhi_buffer   *diff_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    rhi_texture  *vram_tex  = rhi_create_vram(dev, NR_VRAM_W, NR_VRAM_H);
    uint16_t     *mirror    = calloc((size_t)NR_VRAM_W * NR_VRAM_H, sizeof(uint16_t));
    if (!blit_pipe || !vram_pipe || !gouraud_pipe || !gouraud_blend_pipe || !gouraud_sub_pipe || !gouraud_add_pipe || !scene_tex_pipe || !scene_tex_blend_pipe || !scene_tex_add_pipe || !scene_tex_sub_pipe || !gouraud_hud_pipe || !scene_tex_hud_pipe || !panels_vb || !scene_vb || !vram_tex || !mirror ||
        !diff_pipe || !diff_quad_vb) {
        OPL_ERROR_F("native", "RHI setup failed; continuing without native renderer");
        free(mirror);
        if (vram_tex)      rhi_destroy_texture(dev, vram_tex);
        if (diff_quad_vb)  rhi_destroy_buffer(dev, diff_quad_vb);
        if (scene_vb)      rhi_destroy_buffer(dev, scene_vb);
        if (panels_vb)     rhi_destroy_buffer(dev, panels_vb);
        if (diff_pipe)     rhi_destroy_pipeline(dev, diff_pipe);
        if (scene_tex_hud_pipe) rhi_destroy_pipeline(dev, scene_tex_hud_pipe);
        if (gouraud_hud_pipe)   rhi_destroy_pipeline(dev, gouraud_hud_pipe);
        if (scene_tex_sub_pipe) rhi_destroy_pipeline(dev, scene_tex_sub_pipe);
        if (scene_tex_add_pipe) rhi_destroy_pipeline(dev, scene_tex_add_pipe);
        if (scene_tex_blend_pipe) rhi_destroy_pipeline(dev, scene_tex_blend_pipe);
        if (scene_tex_pipe) rhi_destroy_pipeline(dev, scene_tex_pipe);
        if (gouraud_add_pipe) rhi_destroy_pipeline(dev, gouraud_add_pipe);
        if (gouraud_sub_pipe) rhi_destroy_pipeline(dev, gouraud_sub_pipe);
        if (gouraud_blend_pipe) rhi_destroy_pipeline(dev, gouraud_blend_pipe);
        if (gouraud_pipe)  rhi_destroy_pipeline(dev, gouraud_pipe);
        if (vram_pipe)     rhi_destroy_pipeline(dev, vram_pipe);
        if (blit_pipe)     rhi_destroy_pipeline(dev, blit_pipe);
        rhi_destroy(dev);
        SDL_DestroyWindow(win);
        return 1;
    }

    s_nr.enabled     = true;
    s_nr.window      = win;
    s_nr.dev         = dev;
    s_nr.win_w       = w;
    s_nr.win_h       = h;
    s_nr.blit_pipe    = blit_pipe;
    s_nr.vram_pipe    = vram_pipe;
    s_nr.gouraud_pipe = gouraud_pipe;
    s_nr.gouraud_blend_pipe = gouraud_blend_pipe;
    s_nr.gouraud_sub_pipe = gouraud_sub_pipe;
    s_nr.gouraud_add_pipe = gouraud_add_pipe;
    s_nr.scene_tex_pipe = scene_tex_pipe;
    s_nr.scene_tex_blend_pipe = scene_tex_blend_pipe;
    s_nr.scene_tex_add_pipe = scene_tex_add_pipe;
    s_nr.scene_tex_sub_pipe = scene_tex_sub_pipe;
    s_nr.gouraud_hud_pipe = gouraud_hud_pipe;
    s_nr.scene_tex_hud_pipe = scene_tex_hud_pipe;
    s_nr.panels_vb    = panels_vb;
    s_nr.scene_vb     = scene_vb;
    s_nr.diff_pipe    = diff_pipe;
    s_nr.diff_quad_vb = diff_quad_vb;
    s_nr.vram_tex     = vram_tex;
    s_nr.vram_mirror  = mirror;
    OPL_INFO_F("native", "Vulkan %s window up (%dx%d, device='%s'); PsyCross hidden",
               ab_mode ? "A/B" : diff_mode ? "diff" : "vk",
               w, h, rhi_device_name(dev));
    return 0;
}

bool openpete_native_render_enabled(void)
{
    return s_nr.enabled;
}

void openpete_native_set_display_rect(int x, int y, int w, int h)
{
    /* Log size changes only; the disp origin flips every frame (double buffer). */
    if (s_nr.enabled && (w != s_nr.disp_w || h != s_nr.disp_h))
        OPL_INFO_F("native", "disp %dx%d @ (%d,%d) [VRAM px]", w, h, x, y);
    s_nr.disp_x = x; s_nr.disp_y = y; s_nr.disp_w = w; s_nr.disp_h = h;
}

void openpete_native_set_draw_clip(int x, int y, int w, int h)
{
    /* Log on size change; the clip origin flips per frame like disp. The
     * overscan band (per side) is derived from the sizes, not clip.y. */
    if (s_nr.enabled && (w != s_nr.clip_w || h != s_nr.clip_h))
        OPL_INFO_F("native", "draw clip %dx%d @ (%d,%d) [VRAM px] (overscan band=%d/side)",
                   w, h, x, y, s_nr.disp_h > 0 ? (s_nr.disp_h - h) / 2 : 0);
    s_nr.clip_x = x; s_nr.clip_y = y; s_nr.clip_w = w; s_nr.clip_h = h;
}

/* Mirror PsyCross's whole VRAM (1024x512) into the native R16_UINT texture.
 * Full snapshot for now; 1c's dirty-rect sub-rect path (driven by hooking the
 * image ops) is a later efficiency step. Always available (reads PsyCross's
 * CPU mirror), even before the first scene. */
static void sync_vram_texture(void)
{
    GR_ReadVRAM(s_nr.vram_mirror, 0, 0, NR_VRAM_W, NR_VRAM_H);
    rhi_update_vram(s_nr.dev, s_nr.vram_tex, s_nr.vram_mirror);
}

/* Pull PsyCross's finished frame from its present FBO into s_nr.ref_tex.
 * Returns true if a reference frame is ready to draw this present. */
static bool sync_reference_texture(void)
{
    int pw = 0, ph = 0;
    if (!GR_GetPresentSceneSize(&pw, &ph) || pw <= 0 || ph <= 0)
        return false; /* PsyCross hasn't produced a scene yet */

    if (pw != s_nr.ref_w || ph != s_nr.ref_h) {
        if (s_nr.ref_tex) {
            rhi_wait_idle(s_nr.dev);
            rhi_destroy_texture(s_nr.dev, s_nr.ref_tex);
            s_nr.ref_tex = NULL;
        }
        s_nr.ref_tex = rhi_create_rgba8(s_nr.dev, (uint32_t)pw, (uint32_t)ph);
        s_nr.ref_w = pw;
        s_nr.ref_h = ph;

        size_t need = (size_t)pw * (size_t)ph * 4u;
        if (need > s_nr.readback_cap) {
            free(s_nr.readback);
            s_nr.readback = malloc(need);
            s_nr.readback_cap = s_nr.readback ? need : 0;
        }
        OPL_INFO_F("native", "reference FBO size %dx%d", pw, ph);
    }

    if (!s_nr.ref_tex || !s_nr.readback)
        return false;
    if (!GR_ReadPresentSceneRGBA(s_nr.readback, pw, ph, NR_GL_RGBA))
        return false;

    rhi_update_rgba8(s_nr.dev, s_nr.ref_tex, s_nr.readback);
    return true;
}

/* (Re)create native_rtt at the reference resolution — the diff pass renders the
 * native scene into it at the same size as ref_tex so the per-pixel compare is
 * fair. No-op until the reference size is known; recreates on change. Must run
 * OUTSIDE a frame (creates GPU resources / waits idle). `want` gates allocation:
 * the live path passes diff_mode; the screenshot-all path passes true so the
 * -diff.png variant has an RTT even when launched in vk/ab mode. */
static void ensure_native_rtt(bool want)
{
    if (!want || s_nr.ref_w <= 0 || s_nr.ref_h <= 0) return;
    if (s_nr.native_rtt && s_nr.rtt_w == s_nr.ref_w && s_nr.rtt_h == s_nr.ref_h) return;
    if (s_nr.native_rtt) {
        rhi_wait_idle(s_nr.dev);
        rhi_destroy_texture(s_nr.dev, s_nr.native_rtt);
        s_nr.native_rtt = NULL;
    }
    s_nr.native_rtt = rhi_create_render_target(s_nr.dev, (uint32_t)s_nr.ref_w, (uint32_t)s_nr.ref_h);
    s_nr.rtt_w = s_nr.ref_w; s_nr.rtt_h = s_nr.ref_h;
    if (s_nr.native_rtt)
        OPL_INFO_F("native", "diff render target %dx%d", s_nr.ref_w, s_nr.ref_h);
}

/* Rebuild the panel row (VRAM | reference), each aspect-fit into its cell, so
 * resizing never stretches. Only runs when the window or reference size
 * changes (the VRAM panel is a fixed 1024x512). */
static void update_layout(void)
{
    if (s_nr.win_w <= 0 || s_nr.win_h <= 0 || s_nr.ref_w <= 0 || s_nr.ref_h <= 0)
        return;
    if (s_nr.win_w == s_nr.layout_win_w && s_nr.win_h == s_nr.layout_win_h &&
        s_nr.ref_w == s_nr.layout_ref_w && s_nr.ref_h == s_nr.layout_ref_h &&
        s_nr.disp_w == s_nr.layout_disp_w && s_nr.clip_h == s_nr.layout_clip_h)
        return;

    /* n_panels cells across the window; cell i centered at cx_i, half-width
     * 1/n_panels in clip space, full height. vk-only mode has 1 (full-window)
     * panel; A/B has 2 (native | reference). */
    const int   np  = s_nr.n_panels > 0 ? s_nr.n_panels : 2;
    const float chx = 1.0f / (float)np;
    rhi_vertex verts[NR_PANELS * 6] = {0};

    /* Panel 0 — native render of the PSX display region, sampled from the VRAM
     * texture and cropped to the visible viewport: full disp width, but only
     * clip.h rows (the 8px overscan bands top+bottom are dropped). Fitted at
     * the REFERENCE aspect so it lines up 1:1 with the reference panel (the
     * 512-wide framebuffer is non-square; both panels show it at the same
     * display shape). Falls back to the whole VRAM until DISPENV/DRAWENV are
     * set. Top-down (v0=0 top .. v1=H bottom); UVs in texel units relative to
     * the per-frame source tpage. */
    {
        int have_clip = (s_nr.disp_w > 0 && s_nr.clip_h > 0);
        int uv_w  = have_clip ? s_nr.disp_w : NR_VRAM_W;  /* sampled region size */
        int uv_h  = have_clip ? s_nr.clip_h : NR_VRAM_H;
        int fit_w = have_clip ? s_nr.ref_w  : NR_VRAM_W;  /* display shape */
        int fit_h = have_clip ? s_nr.ref_h  : NR_VRAM_H;
        float cx = -1.0f + chx, hx, hy;
        fit_cell(cx, 0.0f, chx, 1.0f, fit_w, fit_h, s_nr.win_w, s_nr.win_h, &hx, &hy);
        build_panel(&verts[0], cx, 0.0f, hx, hy, 0.0f, 0.0f, (float)uv_w, (float)uv_h);
        s_nr.p0_cx = cx; s_nr.p0_hx = hx; s_nr.p0_hy = hy; /* for the native scene map */
    }
    /* Panel 1 — reference (RGBA8, bottom-up: v0=1 top .. v1=0 bottom), UVs
     * normalized (blit uses texture()). Only in A/B mode (np >= 2). */
    if (np >= 2) {
        float cx = -1.0f + 3.0f * chx, hx, hy;
        fit_cell(cx, 0.0f, chx, 1.0f, s_nr.ref_w, s_nr.ref_h, s_nr.win_w, s_nr.win_h, &hx, &hy);
        build_panel(&verts[6], cx, 0.0f, hx, hy, 0.0f, 1.0f, 1.0f, 0.0f);
    }

    /* The VB may still be read by an in-flight frame; layout changes are rare
     * (startup / resize), so fully sync before rewriting it. */
    rhi_wait_idle(s_nr.dev);
    rhi_update_buffer(s_nr.panels_vb, verts, sizeof(verts));
    /* Diff mode: the diff pass draws one quad over panel 0's rect, sampling
     * native_rtt (set 0) + ref_tex (set 1) with 0..1 UVs (top-down; diff.frag
     * V-flips the GL-bottom-up reference). */
    if (s_nr.diff_quad_vb) {
        rhi_vertex dq[6] = {0};
        build_panel(dq, s_nr.p0_cx, 0.0f, s_nr.p0_hx, s_nr.p0_hy, 0.0f, 0.0f, 1.0f, 1.0f);
        rhi_update_buffer(s_nr.diff_quad_vb, dq, sizeof(dq));
    }
    s_nr.layout_win_w = s_nr.win_w; s_nr.layout_win_h = s_nr.win_h;
    s_nr.layout_ref_w = s_nr.ref_w; s_nr.layout_ref_h = s_nr.ref_h;
    s_nr.layout_disp_w = s_nr.disp_w; s_nr.layout_clip_h = s_nr.clip_h;
}

/* Map a PSX screen-space point (x in [0,disp_w], y in [0,disp_h]) into panel
 * 0's fitted clip-space rect, cropping the symmetric overscan band so the
 * native scene lines up with the overscan-cropped reference. */
static void scene_to_clip(float sx, float sy, float *cx, float *cy)
{
    float disp_w = s_nr.disp_w > 0 ? (float)s_nr.disp_w : 512.0f;
    float disp_h = s_nr.disp_h > 0 ? (float)s_nr.disp_h : 240.0f;
    float clip_h = s_nr.clip_h > 0 ? (float)s_nr.clip_h : 224.0f;
    float band   = (disp_h - clip_h) * 0.5f;
    float nx = sx / disp_w;             /* 0..1 across the frame width */
    float ny = (sy - band) / clip_h;    /* 0..1 down the visible band  */
    if (s_render_full_extent) {
        /* native_rtt IS the frame: map to the full target, clip -1..1. */
        *cx = -1.0f + nx * 2.0f;
        *cy =  1.0f - ny * 2.0f;        /* screen y down -> clip y up */
    } else {
        *cx = (s_nr.p0_cx - s_nr.p0_hx) + nx * (2.0f * s_nr.p0_hx);
        *cy = (0.0f + s_nr.p0_hy)       - ny * (2.0f * s_nr.p0_hy);
    }
}

/* The scene background colour = what PsyCross actually clears the framebuffer to,
 * sampled from a top-left corner of the reference readback. This matches PsyCross
 * in EVERY state (levels, titlescreen, menus) and mode — unlike a gamestate
 * source like g_Cyclorama.m_BackgroundColor, which is the level sky colour but is
 * stale/wrong outside a level (e.g. the titlescreen, where the frame clears to a
 * different colour). Returns 0 (no bg) until a reference frame exists; the caller
 * then falls back to the scene-fed colour. rgba byte order R,G,B,A. */
static uint32_t bg_from_ref_corner(void)
{
    if (!s_nr.readback || s_nr.ref_w < 6 || s_nr.ref_h < 6) return 0;
    /* GL readback is bottom-up, so the TOP rows are at the end; inset a few px. */
    const unsigned char* c =
        &s_nr.readback[(((size_t)(s_nr.ref_h - 4) * (size_t)s_nr.ref_w) + 4) * 4u];
    return 0xFF000000u | (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16);
}

/* Build + draw the native 3D scene (cyclorama + actors). full_extent=false
 * draws into panel 0's window rect (the normal vk/ab path); full_extent=true
 * draws into the whole render target (used when rendering into native_rtt for
 * the diff pass, where the target IS the frame). Returns true if anything was
 * drawn. */
static bool draw_native_scene(bool full_extent)
{
    int scene_n; uint32_t scene_bg, scene_fade;
    const openpete_scene_tri* scene = scene_present_buf(&scene_n, &scene_bg, &scene_fade);

    /* Background colour = PsyCross's actual framebuffer clear (ref corner), so it
     * matches in every state/mode; fall back to the scene-fed colour only until a
     * reference frame exists. (This is why an isolated path — e.g.
     * `--render-paths spyro` — gets the real scene background on both sides,
     * including the titlescreen, instead of a stale gamestate colour.) */
    uint32_t bg = bg_from_ref_corner();
    if (bg == 0) bg = scene_bg;

    /* Empty scene: normally return false so the caller re-displays the PSX VRAM
     * framebuffer (load screens, transitions, and other direct-present frames).
     * But in ISOLATION (--render-paths) the VRAM is the UNFILTERED frame — falling
     * back to it would show every path and defeat the isolation. So in isolation
     * present just the background (= PsyCross's filtered clear) instead. */
    if (scene_n <= 0 && (!s_isolation || bg == 0))
        return false;

    s_render_full_extent = full_extent ? 1 : 0;

    static rhi_vertex buf[SCENE_VB_VERTS];
    int n = 0;
    /* Background fill (untextured, far plane), drawn first; depth-tested actors
     * sit in front of it. Covers the panel rect (window) or the full target. */
    if (bg != 0) {
        float l = full_extent ? -1.0f : s_nr.p0_cx - s_nr.p0_hx;
        float r = full_extent ?  1.0f : s_nr.p0_cx + s_nr.p0_hx;
        float t = full_extent ?  1.0f : s_nr.p0_hy;
        float b = full_extent ? -1.0f : -s_nr.p0_hy;
        const rhi_vertex tl = { l, t, 1.0f, 0, 0, bg, 0 };
        const rhi_vertex tr = { r, t, 1.0f, 0, 0, bg, 0 };
        const rhi_vertex br = { r, b, 1.0f, 0, 0, bg, 0 };
        const rhi_vertex bl = { l, b, 1.0f, 0, 0, bg, 0 };
        buf[0] = tl; buf[1] = tr; buf[2] = br;
        buf[3] = tl; buf[4] = br; buf[5] = bl;
        n = 6;
    }
    /* The scene draws in 6 LAYERED groups, in this order, so a ground-decal
     * shadow lands OVER the terrain it falls on but UNDER the actors standing on
     * it (the PSX terrain -> shadow -> Spyro draw order). A single near-bias on
     * the shadow can't do both (it would also paint over Spyro's feet); the layer
     * order does. Each group's UNTEXTURED tris are packed into a contiguous VB
     * range (one batched draw); its TEXTURED tris are drawn per-tri (material
     * push) by filtering the appended textured block. gcat <0 = any (only OPAQUE
     * splits WORLD vs ACTOR vs PLAYER; transparent classes route by blend alone).
     *   0 WORLD-opaque  (blend 0, cat WORLD)   terrain / sky / props
     *   1 SUBTRACT      (blend 3)              shadows (depth_write_off)
     *   2 ACTOR-opaque  (blend 0, cat ACTOR)   mobys — cover the shadows
     *   --- HUD/overlay (depth-test off) drawn here: OVER mobys, UNDER the player ---
     *   3 PLAYER-opaque (blend 0, cat PLAYER)  Spyro — covers the near-OT HUD text
     *   4 ALPHA         (blend 1)              ABR0 semi-transparent
     *   5 ADDITIVE      (blend 2)              ABR1 flame / glows (last)
     * The HUD/overlay sits BETWEEN regular mobys and the player so the PSX's
     * near-OT-slot UI (HUD icons, "DEMO MODE", level-intro / "RESCUED <dragon>"
     * text) draws OVER mobys (e.g. the rescued dragon Nestor) yet UNDER the player
     * Spyro (who flies in front of the intro text) — exactly the PSX OT order
     * (text at the near slot, over other mobys; the player rendered on top). */
    struct scene_group { int blend, gcat; rhi_pipeline *utex, *tex; };
    const struct scene_group groups[6] = {
        { 0, OPENPETE_SCENE_WORLD,  s_nr.gouraud_pipe,       s_nr.scene_tex_pipe       },
        { 3, -1,                    s_nr.gouraud_sub_pipe,   s_nr.scene_tex_sub_pipe   },
        { 0, OPENPETE_SCENE_ACTOR,  s_nr.gouraud_pipe,       s_nr.scene_tex_pipe       },
        { 0, OPENPETE_SCENE_PLAYER, s_nr.gouraud_pipe,       s_nr.scene_tex_pipe       },
        { 1, -1,                    s_nr.gouraud_blend_pipe, s_nr.scene_tex_blend_pipe },
        { 2, -1,                    s_nr.gouraud_add_pipe,   s_nr.scene_tex_add_pipe   },
    };
    /* ut group 6 = HUD untextured overlay (after ACTOR); 7 = OVERLAY (dead last,
     * letterbox / HUD-OT 2D flat quads, over everything). Both depth-test off. */
    int ut_first[8], ut_count[8];
    for (int gi = 0; gi < 6; gi++) {
        ut_first[gi] = n;
        int gb = groups[gi].blend, gc = groups[gi].gcat;
        for (int t = 0; t < scene_n; t++) {
            if (scene[t].textured || n + 3 > SCENE_VB_VERTS) continue;
            if (scene[t].category == OPENPETE_SCENE_HUD) continue;  /* HUD -> overlay pass */
            int b = scene[t].blend; if (b < 0 || b > 3) b = 0;   /* untextured class */
            if (b != gb) continue;
            if (gb == 0 && gc >= 0 && scene[t].category != gc) continue;
            for (int k = 0; k < 3; k++) {
                float cx, cy;
                scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
                rhi_vertex *o = &buf[n + k];
                o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
                o->rgba = scene[t].v[k].rgba; o->tex = 0;
            }
            n += 3;
        }
        ut_count[gi] = n - ut_first[gi];
    }
    /* HUD untextured: every HUD-category untextured tri (any blend), painter's
     * order = submission order; the overlay pipe ignores depth so they sit on top. */
    ut_first[6] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[6] = n - ut_first[6];
    /* OVERLAY untextured: HUD-OT 2D flat quads (letterbox bars). Drawn dead last,
     * over everything (incl. the player). Untextured opaque; painter's order. */
    ut_first[7] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_OVERLAY || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[7] = n - ut_first[7];
    int tx_start = n;
    for (int t = 0; t < scene_n; t++) {
        if (!scene[t].textured || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z;
            o->u = scene[t].v[k].u; o->v = scene[t].v[k].v;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }

    /* Full-screen fade overlay (func_800190D4): a single full-panel quad at the
     * near plane (z=0 passes LESS_OR_EQUAL against everything), drawn dead last so
     * it tints the WHOLE composited frame — world, actors, HUD and 2D UI — exactly
     * as the PSX's semi-transparent full-screen POLY_F4 does. The vertex colour is
     * the fade colour; the blend mode (set below) reproduces the PSX ABR. */
    int fade_first = -1, fade_mode = -1;
    if ((scene_fade & (1u << 31)) && n + 6 <= SCENE_VB_VERTS) {
        fade_mode  = (int)((scene_fade >> 24) & 3u);
        int fr = (int)((scene_fade >> 16) & 0xFFu);
        int fg = (int)((scene_fade >> 8) & 0xFFu);
        int fb = (int)(scene_fade & 0xFFu);
        /* mode 0 = (B+F)/2 -> alpha 128; add/subtract ignore alpha. */
        uint32_t fa = (fade_mode == 0) ? 0x80u : 0xFFu;
        uint32_t frgba = (uint32_t)fr | ((uint32_t)fg << 8) | ((uint32_t)fb << 16) | (fa << 24);
        float l = full_extent ? -1.0f : s_nr.p0_cx - s_nr.p0_hx;
        float r = full_extent ?  1.0f : s_nr.p0_cx + s_nr.p0_hx;
        float t = full_extent ?  1.0f : s_nr.p0_hy;
        float b = full_extent ? -1.0f : -s_nr.p0_hy;
        const rhi_vertex tl = { l, t, 0.0f, 0, 0, frgba, 0 };
        const rhi_vertex tr = { r, t, 0.0f, 0, 0, frgba, 0 };
        const rhi_vertex br = { r, b, 0.0f, 0, 0, frgba, 0 };
        const rhi_vertex bl = { l, b, 0.0f, 0, 0, frgba, 0 };
        fade_first = n;
        buf[n+0] = tl; buf[n+1] = tr; buf[n+2] = br;
        buf[n+3] = tl; buf[n+4] = br; buf[n+5] = bl;
        n += 6;
    }

    rhi_update_buffer(s_nr.scene_vb, buf, sizeof(rhi_vertex) * (size_t)(n > 0 ? n : 1));

    /* Confine the scene to panel 0's pixel rect so triangles that extend off
     * the PSX screen (legitimately, after near-plane culling) are clipped to
     * the panel instead of streaking across the window. In full_extent mode the
     * RT pass already scissors to the whole target (the frame fills it). */
    if (!full_extent) {
        int sx = (int)(((s_nr.p0_cx - s_nr.p0_hx) * 0.5f + 0.5f) * (float)s_nr.win_w + 0.5f);
        int sw = (int)((s_nr.p0_hx) * (float)s_nr.win_w + 0.5f);
        int sy = (int)((0.5f - 0.5f * s_nr.p0_hy) * (float)s_nr.win_h + 0.5f);
        int sh = (int)((s_nr.p0_hy) * (float)s_nr.win_h + 0.5f);
        rhi_set_scissor(s_nr.dev, sx, sy, sw, sh);
    }

    /* Draw the 6 layered groups in order (WORLD-opaque, SUBTRACT shadows,
     * ACTOR-opaque, PLAYER-opaque, ALPHA, ADDITIVE). For each: the batched
     * untextured range, then its textured tris per-tri (material push). Opaque
     * groups write depth; the transparent groups (1/4/5) are depth_write_off (test
     * vs opaque, never occlude each other). Per-tri material draws; material-batched
     * draws + back-to-front transparent sort (3c) are later.
     *
     * The HUD/overlay layer (depth-test OFF) is drawn AFTER the ACTOR group (mobys,
     * gi==2) and BEFORE the PLAYER group (Spyro, gi==3): it ignores world depth so
     * it sits on top of the terrain AND regular mobys (the "DEMO MODE" banner /
     * level-intro & "RESCUED <dragon>" text / HUD icons the .s forces to the near OT
     * slot, over other mobys like the rescued dragon), but writes no depth, so the
     * player Spyro drawn next still covers it (Spyro flies in front of the intro
     * text). Matches the PSX moby OT: near-slot UI over other mobys, player on top. */
    for (int gi = 0; gi < 6; gi++) {
        const struct scene_group *g = &groups[gi];
        if (g->utex && ut_count[gi] > 0) {
            rhi_bind_pipeline(s_nr.dev, g->utex);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, (uint32_t)ut_count[gi], (uint32_t)ut_first[gi]);
        }
        if (n > tx_start && s_nr.vram_tex && g->tex) {
            rhi_bind_pipeline(s_nr.dev, g->tex);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            int off = tx_start;
            for (int t = 0; t < scene_n && off + 3 <= n; t++) {
                if (!scene[t].textured) continue;
                int b = scene[t].blend; if (b < 0 || b > 3) b = 0;
                int match = scene[t].category != OPENPETE_SCENE_HUD &&  /* HUD -> overlay pass */
                            (b == g->blend) &&
                            !(g->blend == 0 && g->gcat >= 0 && scene[t].category != g->gcat);
                if (match) {
                    const psx_material m = { scene[t].tpage_x, scene[t].tpage_y,
                                             scene[t].clut_x, scene[t].clut_y, scene[t].bpp };
                    rhi_push_constants(s_nr.dev, &m, sizeof m);
                    rhi_draw(s_nr.dev, 3, (uint32_t)off);
                }
                off += 3;  /* advance for every textured tri (verts are contiguous) */
            }
        }
        /* HUD/overlay layer, inserted between the ACTOR group (mobys, gi==2) and the
         * PLAYER group (Spyro, gi==3): untextured batch, then HUD textured tris
         * per-tri. depth-test off (over world + mobys, under the player). */
        if (gi == 2) {
            if (s_nr.gouraud_hud_pipe && ut_count[6] > 0) {
                rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_hud_pipe);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                rhi_draw(s_nr.dev, (uint32_t)ut_count[6], (uint32_t)ut_first[6]);
            }
            if (n > tx_start && s_nr.vram_tex && s_nr.scene_tex_hud_pipe) {
                rhi_bind_pipeline(s_nr.dev, s_nr.scene_tex_hud_pipe);
                rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                int off = tx_start;
                for (int t = 0; t < scene_n && off + 3 <= n; t++) {
                    if (!scene[t].textured) continue;
                    if (scene[t].category == OPENPETE_SCENE_HUD) {
                        const psx_material m = { scene[t].tpage_x, scene[t].tpage_y,
                                                 scene[t].clut_x, scene[t].clut_y, scene[t].bpp };
                        rhi_push_constants(s_nr.dev, &m, sizeof m);
                        rhi_draw(s_nr.dev, 3, (uint32_t)off);
                    }
                    off += 3;  /* advance for every textured tri (verts are contiguous) */
                }
            }
        }
    }

    /* OVERLAY layer (HUD-OT 2D flat quads — the cutscene letterbox bars): drawn
     * after every scene group, over everything incl. the player, depth-test off
     * (gouraud_hud_pipe). Before the fade, matching the PSX HUD-OT order (the fade
     * F4 is linked after the border, so it draws on top of the bars). */
    if (s_nr.gouraud_hud_pipe && ut_count[7] > 0) {
        rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_hud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count[7], (uint32_t)ut_first[7]);
    }

    /* Draw the full-screen fade overlay last (over every layer above), in the PSX
     * ABR blend: mode 2 (B-F) subtract -> fade to black; mode 1 (B+F) add -> fade to
     * white; mode 0 ((B+F)/2) -> alpha. These pipes are depth_write_off + the quad
     * is at z=0, so it tests-passes against the whole frame and writes no depth. */
    if (fade_first >= 0) {
        rhi_pipeline* fp = (fade_mode == 2) ? s_nr.gouraud_sub_pipe
                         : (fade_mode == 0) ? s_nr.gouraud_blend_pipe
                                            : s_nr.gouraud_add_pipe;  /* 1 (and 3) -> additive */
        if (fp) {
            rhi_bind_pipeline(s_nr.dev, fp);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, 6, (uint32_t)fade_first);
        }
    }

    if (!full_extent) rhi_reset_scissor(s_nr.dev);
    s_render_full_extent = 0;
    return true;
}

/* Renderer variants — the live mode picks one; --screenshot-on-frame dumps all
 * three (see openpete_native_render_screenshot_all). */
enum { NRV_VK = 0, NRV_AB = 1, NRV_DIFF = 2 };

/* Compose one variant into the open frame's offscreen, ready for the caller to
 * rhi_blit_to_swapchain + rhi_end_frame. Assumes rhi_begin_frame succeeded, the
 * layout matches the variant's panel count (1 for vk/diff, 2 for ab), and — for
 * NRV_DIFF — native_rtt is allocated. Returns true iff the diff pass actually
 * ran (scene non-empty + reference present), so the caller can fold per-frame
 * --render-stats. Shared by the live render path and the screenshot-all dump. */
static bool compose_variant(int variant, bool have_ref)
{
    /* Diff: render the native scene into native_rtt (reference resolution, full
     * extent), then a fullscreen diff pass writes |native - ref| into the
     * offscreen. Falls through to the plain native path until the reference /
     * RTT are ready or the scene is empty. */
    if (variant == NRV_DIFF && s_nr.native_rtt && have_ref) {
        int dn; uint32_t dbg, dfade;
        (void)scene_present_buf(&dn, &dbg, &dfade);
        if (dn > 0) {
            /* Clear native_rtt to the scene's background colour (the same colour
             * the cyclorama module sets, which PsyCross also clears to) so the
             * background cancels in |native - ref| — the diff/stats then measure
             * the path, not the framebuffer-clear. draw_native_scene also paints
             * a bg quad in this colour; the clear just covers any gap. */
            float br  = (float)((dbg >>  0) & 0xFF) / 255.0f;
            float bgc = (float)((dbg >>  8) & 0xFF) / 255.0f;
            float bb  = (float)((dbg >> 16) & 0xFF) / 255.0f;
            rhi_begin_render_target(s_nr.dev, s_nr.native_rtt, br, bgc, bb, 1.0f);
            draw_native_scene(true);
            rhi_end_render_target(s_nr.dev, s_nr.native_rtt);

            rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);
            rhi_bind_pipeline(s_nr.dev, s_nr.diff_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.native_rtt);  /* set 0 = native */
            rhi_bind_vram2(s_nr.dev, s_nr.ref_tex);    /* set 1 = reference */
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.diff_quad_vb);
            rhi_draw(s_nr.dev, 6, 0);
            rhi_end_offscreen(s_nr.dev);
            return true;
        }
        /* scene empty — fall through to plain native (diff has nothing to show) */
    }

    rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);

    /* Panel 0 — native render of the PSX display region, decoded 16bpp from
     * the VRAM mirror at the DISPENV disp origin (whole VRAM until the game
     * sets DISPENV). The tpage matches the UVs baked into the layout. */
    if (s_nr.layout_ref_w > 0) { /* layout built (needs a known ref size) */
        /* tpage = the CURRENT front-buffer origin (flips per frame via the
         * double buffer), plus the top overscan band dy so the sampled rows are
         * the visible clip. The panel UVs are baked by size; only the origin
         * moves per frame. */
        int tx = 0, ty = 0;
        if (s_nr.disp_w > 0 && s_nr.clip_h > 0 && s_nr.clip_h <= s_nr.disp_h) {
            /* Overscan band, centered: clip.y − disp.y is unreliable here (at
             * present time disp is the FRONT buffer while clip was last set for
             * the BACK buffer — different double-buffer pages). The bands are
             * symmetric (8px top+bottom on Spyro's 240→224), so derive the top
             * band from the sizes instead. */
            int dy = (s_nr.disp_h - s_nr.clip_h) / 2;
            tx = s_nr.disp_x;
            ty = s_nr.disp_y + dy;
        }
        /* Panel 0 — the native 3D render (Phase 2: cyclorama from gamestate)
         * when the game pushed a scene; otherwise fall back to re-displaying
         * the PSX framebuffer region from the VRAM mirror (pre-Phase-2). */
        if (!draw_native_scene(false)) {
            const psx_material nd_mat = { tx, ty, 0, 0, 16 };
            rhi_bind_pipeline(s_nr.dev, s_nr.vram_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            rhi_push_constants(s_nr.dev, &nd_mat, sizeof(nd_mat));
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
            rhi_draw(s_nr.dev, 6, 0);
        }

        /* Panel 1 — reference (A/B compare only; vk/diff are single panels). */
        if (variant == NRV_AB && have_ref) {
            rhi_bind_pipeline(s_nr.dev, s_nr.blit_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.ref_tex);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
            rhi_draw(s_nr.dev, 6, 6);
        }
    }

    rhi_end_offscreen(s_nr.dev);
    return false;
}

void openpete_native_render_frame(void)
{
    if (!s_nr.enabled)
        return;

    /* Detect window-size changes WITHOUT polling SDL events (the host loop
     * owns the event queue, and stealing events would starve PsyCross input). */
    int pw = 0, ph = 0;
    if (SDL_GetWindowSizeInPixels(s_nr.window, &pw, &ph) && pw > 0 && ph > 0 &&
        (pw != s_nr.win_w || ph != s_nr.win_h)) {
        s_nr.win_w = pw;
        s_nr.win_h = ph;
        rhi_notify_resize(s_nr.dev);
    }

    /* Mirror VRAM + read the reference back BEFORE opening our frame: these do
     * their own one-shot GPU submits, kept clear of our render pass. */
    sync_vram_texture();
    bool have_ref = sync_reference_texture();
    update_layout();
    ensure_native_rtt(s_nr.diff_mode);

    if (!rhi_begin_frame(s_nr.dev))
        return; /* swapchain recreating or window minimized — skip this present */

    int variant = s_nr.diff_mode ? NRV_DIFF : s_nr.ab_mode ? NRV_AB : NRV_VK;
    bool did_diff = compose_variant(variant, have_ref);

    rhi_blit_to_swapchain(s_nr.dev);
    rhi_end_frame(s_nr.dev);

    if (did_diff && s_nr.stats_enabled) diff_stats_accumulate();  /* reads back the diff */
}

/* Insert a suffix before the extension: ("shot-42.png","-vk") -> "shot-42-vk.png". */
static void path_with_suffix(const char *base, const char *suffix,
                             char *out, size_t cap)
{
    const char *dot = strrchr(base, '.');
    if (dot) snprintf(out, cap, "%.*s%s%s", (int)(dot - base), base, suffix, dot);
    else     snprintf(out, cap, "%s%s", base, suffix);
}

int openpete_native_render_screenshot_all(const char *base)
{
    if (!s_nr.enabled || !base) return 1;

    /* Re-sync VRAM + reference so the dump matches the just-presented frame. */
    sync_vram_texture();
    bool have_ref = sync_reference_texture();
    ensure_native_rtt(true);
    if (!have_ref || !s_nr.native_rtt || !s_nr.readback ||
        s_nr.ref_w <= 0 || s_nr.ref_h <= 0)
        return 1;

    /* Compose every variant at the REFERENCE resolution rather than the window
     * size, so the images are the true content size with no window letterbox:
     *   -vk   = native scene           (W x H)
     *   -diff = |native - reference|   (W x H)
     *   -ab   = native | reference     (2W x H)
     * The native scene is rendered full-extent into the ref-sized RTT and read
     * straight back; the reference comes from the PsyCross readback. */
    const int W = s_nr.ref_w, H = s_nr.ref_h;
    const size_t frame_px = (size_t)W * (size_t)H;

    /* Background = PsyCross's framebuffer clear (ref corner), matching the live
     * native render; clears the RTT so empty regions cancel against the ref. */
    int scene_n; uint32_t scene_bg, scene_fade;
    (void)scene_present_buf(&scene_n, &scene_bg, &scene_fade);
    uint32_t bg = bg_from_ref_corner();
    if (bg == 0) bg = scene_bg;
    const float br  = (float)((bg >>  0) & 0xFF) / 255.0f;
    const float bgc = (float)((bg >>  8) & 0xFF) / 255.0f;
    const float bb  = (float)((bg >> 16) & 0xFF) / 255.0f;

    /* Render the native scene into the ref-sized RTT (full extent, no panels).
     * The offscreen + present are kept valid with a plain bg clear so the window
     * isn't disturbed mid-dump; the RTT (not the window) is the data source. */
    if (!rhi_begin_frame(s_nr.dev)) return 1;
    rhi_begin_render_target(s_nr.dev, s_nr.native_rtt, br, bgc, bb, 1.0f);
    draw_native_scene(true);
    rhi_end_render_target(s_nr.dev, s_nr.native_rtt);
    rhi_begin_offscreen(s_nr.dev, br, bgc, bb, 1.0f);
    rhi_end_offscreen(s_nr.dev);
    rhi_blit_to_swapchain(s_nr.dev);
    rhi_end_frame(s_nr.dev);

    unsigned char *nat = malloc(frame_px * 4);          /* native, top-down    */
    unsigned char *ref = malloc(frame_px * 4);          /* reference, top-down  */
    unsigned char *ab  = malloc(frame_px * 4 * 2);      /* native | reference   */
    unsigned char *dif = malloc(frame_px * 4);          /* |native - reference| */
    if (!nat || !ref || !ab || !dif) {
        free(nat); free(ref); free(ab); free(dif);
        return 1;
    }

    int nw = 0, nh = 0;
    if (!rhi_read_render_target(s_nr.dev, s_nr.native_rtt, nat, frame_px * 4, &nw, &nh)) {
        free(nat); free(ref); free(ab); free(dif);
        return 1;
    }
    /* The PsyCross readback is GL bottom-up; flip to top-down to match the RTT. */
    for (int y = 0; y < H; y++)
        memcpy(&ref[(size_t)y * W * 4],
               &s_nr.readback[(size_t)(H - 1 - y) * W * 4], (size_t)W * 4);
    /* Empty native scene (load/transition frames): mirror the reference so -vk
     * shows the framebuffer and -diff reads ~zero instead of a flat clear. */
    if (scene_n <= 0) memcpy(nat, ref, frame_px * 4);

    /* -ab: native on the left, reference on the right (2W wide). */
    for (int y = 0; y < H; y++) {
        memcpy(&ab[(size_t)y * (2 * W) * 4],         &nat[(size_t)y * W * 4], (size_t)W * 4);
        memcpy(&ab[((size_t)y * (2 * W) + W) * 4],   &ref[(size_t)y * W * 4], (size_t)W * 4);
    }
    /* -diff: per-channel absolute difference, opaque. */
    for (size_t i = 0; i < frame_px; i++) {
        for (int c = 0; c < 3; c++) {
            int d = (int)nat[i * 4 + c] - (int)ref[i * 4 + c];
            dif[i * 4 + c] = (unsigned char)(d < 0 ? -d : d);
        }
        dif[i * 4 + 3] = 0xFF;
    }

    int rc = 0;
    char path[1100];
    path_with_suffix(base, "-vk", path, sizeof(path));
    if (openpete_write_png_rgba(path, nat, W, H) == 0)
        OPL_INFO_F("native", "vk screenshot %dx%d -> %s", W, H, path);
    else rc = 1;
    path_with_suffix(base, "-ab", path, sizeof(path));
    if (openpete_write_png_rgba(path, ab, 2 * W, H) == 0)
        OPL_INFO_F("native", "ab screenshot %dx%d -> %s", 2 * W, H, path);
    else rc = 1;
    path_with_suffix(base, "-diff", path, sizeof(path));
    if (openpete_write_png_rgba(path, dif, W, H) == 0)
        OPL_INFO_F("native", "diff screenshot %dx%d -> %s", W, H, path);
    else rc = 1;

    free(nat); free(ref); free(ab); free(dif);
    return rc;
}

int openpete_native_render_screenshot(const char *path)
{
    if (!s_nr.enabled || !path) return 1;
    size_t cap = (size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u;
    if (cap == 0) return 1;
    unsigned char *buf = malloc(cap);
    if (!buf) return 1;
    int w = 0, h = 0, rc = 1;
    if (rhi_read_offscreen(s_nr.dev, buf, cap, &w, &h))
        rc = openpete_write_png_rgba(path, buf, w, h);
    free(buf);
    if (rc == 0)
        OPL_INFO_F("native", "A/B screenshot %dx%d -> %s", w, h, path);
    return rc;
}

void openpete_native_render_shutdown(void)
{
    if (!s_nr.enabled)
        return;
    rhi_wait_idle(s_nr.dev);
    if (s_nr.ref_tex)      rhi_destroy_texture(s_nr.dev, s_nr.ref_tex);
    if (s_nr.native_rtt)   rhi_destroy_texture(s_nr.dev, s_nr.native_rtt);
    if (s_nr.vram_tex)     rhi_destroy_texture(s_nr.dev, s_nr.vram_tex);
    if (s_nr.diff_quad_vb) rhi_destroy_buffer(s_nr.dev, s_nr.diff_quad_vb);
    if (s_nr.scene_vb)     rhi_destroy_buffer(s_nr.dev, s_nr.scene_vb);
    if (s_nr.panels_vb)    rhi_destroy_buffer(s_nr.dev, s_nr.panels_vb);
    if (s_nr.diff_pipe)    rhi_destroy_pipeline(s_nr.dev, s_nr.diff_pipe);
    if (s_nr.scene_tex_hud_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_hud_pipe);
    if (s_nr.gouraud_hud_pipe)   rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_hud_pipe);
    if (s_nr.scene_tex_sub_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_sub_pipe);
    if (s_nr.scene_tex_add_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_add_pipe);
    if (s_nr.scene_tex_blend_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_blend_pipe);
    if (s_nr.scene_tex_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_pipe);
    if (s_nr.gouraud_add_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_add_pipe);
    if (s_nr.gouraud_sub_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_sub_pipe);
    if (s_nr.gouraud_blend_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_blend_pipe);
    if (s_nr.gouraud_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_pipe);
    if (s_nr.vram_pipe)    rhi_destroy_pipeline(s_nr.dev, s_nr.vram_pipe);
    if (s_nr.blit_pipe)    rhi_destroy_pipeline(s_nr.dev, s_nr.blit_pipe);
    rhi_destroy(s_nr.dev);
    SDL_DestroyWindow(s_nr.window);
    free(s_nr.readback);
    free(s_nr.vram_mirror);

    /* --render-stats aggregate over the whole run. */
    if (s_diff_stats.frames > 0 && s_diff_stats.n > 0) {
        double inv = 1.0 / (double)s_diff_stats.n;
        OPL_INFO_F("native", "diff-stats AGGREGATE over %u frames: mean=(%.2f,%.2f,%.2f) "
                   "max=(%d,%d,%d) within%d=%.2f%% diff-px=%.1f%% (avg %llu/frame)",
                   s_diff_stats.frames,
                   s_diff_stats.sum[0]*inv, s_diff_stats.sum[1]*inv, s_diff_stats.sum[2]*inv,
                   s_diff_stats.max[0], s_diff_stats.max[1], s_diff_stats.max[2],
                   DIFF_STATS_TOL, 100.0 * (double)s_diff_stats.within * inv,
                   100.0 * (double)s_diff_stats.off * inv,
                   (unsigned long long)(s_diff_stats.off / s_diff_stats.frames));
    }
    free(s_stats_buf);
    s_stats_buf = NULL; s_stats_cap = 0;
    memset(&s_diff_stats, 0, sizeof(s_diff_stats));

    memset(&s_nr, 0, sizeof(s_nr));
    OPL_INFO_F("native", "Vulkan native renderer window shut down");
}
