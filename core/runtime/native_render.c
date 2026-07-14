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
#include "openpete_menu.h"
#include "openpete_mods.h"   /* openpete_mods_texture_get (mod-texture runs) */

#include "openpete_log.h"
#include "openpete_time.h"   /* op_now — overlay's own sub-tick alpha (camera-independent) */
#include "openpete_record.h" /* native --record capture seam */
#include "rhi.h"
#include "screenshot.h"  /* openpete_write_png_rgba */

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <math.h>    /* fminf/fmaxf/floorf (prepass recon + occlusion grid) */
#include <stdbool.h>
#include <stddef.h>  /* offsetof (scene_tri_t prefix assert) */
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
#include "openpete_native_vram.h"   /* W2.4: the owned VRAM model + native ops */
#include "texture_pack.h"           /* texture-replacement registry (Phase 1+) */
extern void PsyX_SetVRAMDirtyCallback(void (*cb)(int x, int y, int w, int h));
static void nr_vram_dirty_cb(int x, int y, int w, int h);   /* W2.4, defined below */
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

/* shadow_decal.frag push constant (must match the GLSL block field-for-field) —
 * the depth-aware shadow decal's per-pixel world-Y wall rejection. The PSX
 * screen coord rides the vertex u,v as a noperspective varying, and the
 * pre-pass depth lookup goes through the uax..vby affine (the SAME full-extent
 * mapping the shared pre-pass rasterized with) — no gl_FragCoord->screen math
 * anywhere, so full-extent, panel, and SSAA paths are identical by
 * construction. */
typedef struct {
    float col0x, col0y, col0z;     /* camera matrix columns (Q12 int16 as float) */
    float col1x, col1y, col1z;
    float col2x, col2y, col2z;
    float cam_pos_up;              /* camera world height (game z field) */
    float ofx, ofy, h;
    float ground_up;               /* shadow ground-plane height (SpyroShadow z) */
    float wall_height;
    float steep_nup;               /* |up normal| below which a surface is a wall */
    float mode;                    /* 0 = draw all, 1 = height+steep discard, 2 = viz */
    float band;                    /* fallback view-Z band when params invalid */
    float plat_eps;                /* raised-occluder-in-front: min rise */
    float plat_slack;              /* raised-occluder-in-front: view-Z slack */
    float uax, ubx, vay, vby;      /* PSX screen -> pre-pass uv affine */
    float pvw, pvh;                /* pre-pass raster extent (texels) */
} shadow_decal_pc;


/* shadow_decal_tex.frag push constant (must match the GLSL block field-for-field)
 * — the TEXTURED moby drop-shadow decal: the camera reconstruction params of the
 * Spyro decal (shadow_decal_pc, minus its single ground_up — the moby plane is
 * per-pixel from gl_FragCoord.z) plus the per-draw PSX material (all moby shadows
 * share one shadow-sprite tpage, so one material serves the whole batch). The PSX
 * screen coord rides vertex location 3 (sx,sy). 25 floats + 5 ints = 120 B, under
 * the 128-byte guaranteed maxPushConstantsSize. */
typedef struct {
    float col0x, col0y, col0z;     /* camera rows 4096^2 * C^-1 (world reconstruct) */
    float col1x, col1y, col1z;
    float col2x, col2y, col2z;
    float cam_pos_up;              /* camera world height (game z field) */
    float ofx, ofy, h;
    float wall_height;
    float steep_nup;               /* |up normal| below which a surface is a wall */
    float mode;                    /* 0 = draw all (band), 1 = height+steep discard */
    float band;                    /* fallback view-Z band when mode 0 */
    float plat_eps;                /* raised-occluder-in-front: min rise */
    float plat_slack;              /* raised-occluder-in-front: view-Z slack */
    float uax, ubx, vay, vby;      /* PSX screen -> pre-pass uv affine */
    float pvw, pvh;                /* pre-pass raster extent (texels) */
    int32_t tpage_x, tpage_y;      /* PSX material (shadow sprite): texture page */
    int32_t clut_x, clut_y;        /* CLUT origin (paletted) */
    int32_t bpp;                   /* 4 / 8 / 16 */
} shadow_decal_tex_pc;

/* How many panels the window shows (1c.3a: VRAM + reference). */
#define NR_PANELS 2

/* Native 3D scene (Phase 2): screen-space triangles the game pushes in
 * (cyclorama/sky from gamestate). Double-buffered isn't needed — the game
 * fills it during its render pass (before openpete_native_render_frame reads
 * it). begin() clears; add_tri() appends. */
/* Native scene-triangle cap. Sized for the worst-case --no-lod load: forced-HQ
 * routes every foreground terrain cell through the 2x2 subdivide (~4x the tris)
 * and renders every moby with no distance cull, so the densest content
 * (gnastys_loot, the all-gems loot vault) peaks at ~43k tris vs ~10k for a
 * typical level at stock LOD. The old 16384 cap silently dropped everything past
 * it, and which tris dropped depended on the view → terrain that appeared/
 * disappeared with camera angle. Worst measured across all 35 levels is
 * beast_makers at ~63k (a homeworld: every enemy + gem + dragon rendered with no
 * cull, plus forced-HQ terrain); 131072 is ~2x that, covering worst-camera-angle
 * beyond the charge-path sample. The drop counter below is the permanent safety
 * net for any outlier. (Static arrays cost ~68 MB; dynamic sizing keyed on
 * --no-lod is a future optimisation — see the disable-lods plan.) */
#define MAX_SCENE_TRIS 131072
#define SCENE_VB_VERTS (MAX_SCENE_TRIS * 3 + 18)  /* +6 bg-fill, +6 fade overlay, +6 freeze backdrop */

/* Moby occlusion-culling grid extent (cells over the PSX screen: ~8×5 PSX px
 * per cell at 512×240). Coarse on purpose — the CPU reads it back per present
 * (OCCL_GW*OCCL_GH*4 = 12 KB) and tests whole-moby screen rects against it. */
#define OCCL_GW 64
#define OCCL_GH 48
#define OVL_MAX_VERTS 4096   /* overlay (3D text-moby glyph) vertex cap (see draw_fps_overlay) */
#define MOD_OVL_MAX_VERTS 16384 /* mod draw-service overlay verts (see draw_mod_overlay) */
#define MENU_MAX_VERTS 256   /* in-game menu panel/border/selection quads (see draw_menu_overlay) */
#define MENU_GLYPH_MAX 16384 /* in-game menu 3D glyph text verts (labels/values/title) */
/* FPS-overlay supersampling: render the glyphs at OVL_SS_FACTOR× into a small
 * RGBA target covering the top-left OVL_REGION_W × OVL_REGION_H of the frame (in
 * win_h-fraction units, matching the glyph layout's win_h-relative coords), then
 * box-downsample + alpha-blend onto the frame so the corner counter is crisp at
 * any resolution. The region is generous enough for "FPS: 9999". */
#define OVL_SS_FACTOR  4
#define OVL_REGION_W   0.30f
#define OVL_REGION_H   0.05f
/* Settings-menu glyph supersample factor. The menu region is the whole (tall)
 * panel, so this is smaller than OVL_SS_FACTOR to keep the target memory sane at
 * high resolutions (2× = 4 samples/px, a clear anti-alias). */
#define MENU_SS_FACTOR 2
/* The INTERNAL stored tri (W0.7b): the feeder-facing openpete_scene_tri plus
 * the routing state that travels through the add_tri arguments. Private to
 * this TU — feeders can't write routing fields (no such field exists on their
 * type), and no API ever hands a stored tri back out. The feeder tri is a
 * strict LAYOUT PREFIX (asserted below), so the store is a prefix memcpy. */
typedef struct {
    openpete_scene_vert v[3];
    int textured;
    int tpage_x, tpage_y, clut_x, clut_y, bpp;
    int blend;
    int category;   /* draw layer (OPENPETE_SCENE_*) — add_tri's explicit arg */
    int carry;      /* 2D-UI sub-tick carry: 0/1 from add_tri; 2 = re-applied marker
                     * stamped by apply_overlay so the next canonical capture skips it */
    int group;      /* per-portal grouping id (add_tri_grouped; plain add_tri: 0) */
    uint32_t tag;   /* provenance (mod-shaders S6): sticky set_tag value; joins
                     * the material key ONLY when tag_split_on() (else inert) */
} scene_tri_t;
/* C99 compile-time prefix check: if a field is added to openpete_scene_tri
 * without mirroring it here (or vice versa), this array gets a negative size. */
typedef char nr_assert_scene_tri_prefix[
    (offsetof(scene_tri_t, category) == sizeof(openpete_scene_tri)) ? 1 : -1];
static scene_tri_t s_scene[MAX_SCENE_TRIS];      /* accumulating (frame N) */
static int                s_scene_n;

/* Provenance-tag state (S6). s_tag_split_ext is armed by the material-
 * selector registry (S8); the env flags are the dev instruments. */
static uint32_t s_cur_tag;        /* sticky, stamped by add_tri */
static int      s_tag_tint = -1;  /* OPENPETE_TAG_TINT: tint colours by tag */
static int      s_tag_split_env = -1;   /* OPENPETE_TAG_SPLIT: force key join */
static int      s_tag_split_ext = 0;    /* armed by registered selectors (S8) */

void openpete_native_scene_set_tag(uint32_t tag) { s_cur_tag = tag; }
/* S8 arms this when material selectors register (0 selectors ⇒ inert key). */
void openpete_native_set_tag_split(int on) { s_tag_split_ext = on ? 1 : 0; }

static int tag_split_on(void) {
    if (s_tag_split_env < 0) {
        const char* e = getenv("OPENPETE_TAG_SPLIT");
        s_tag_split_env = (e && *e && *e != '0');
        const char* t = getenv("OPENPETE_TAG_TINT");
        s_tag_tint = (t && *t && *t != '0');
    }
    return s_tag_split_env || s_tag_split_ext;
}

static int tag_tint_on(void) {
    (void)tag_split_on();          /* lazy env init lives there */
    return s_tag_tint > 0;
}

/* Mod custom-pipeline drain (S7) — defined with the postfx executor
 * below; called from compose_variant (mod 3D under mod HUD). */
static void draw_mod_meshes(void);
/* Shared with the postfx executor below (S3): the build-failed pipeline
 * sentinel and the engine/mod UBO — draw_tex_runs' material overrides
 * (S8a) bind them from earlier in the file. */
#define PFX_PIPE_FAILED OPENPETE_POSTFX_PIPE_FAILED
static rhi_ubo* s_pfx_ubo;
/* Material-selector UBO (S8b): block 0 = engine block, blocks 1+slot =
 * per-instance uniform blocks. Filled once per present at the top of
 * draw_native_scene (defined after the postfx typedef); draw_tex_runs'
 * override binds it. */
static rhi_ubo* s_mat_ubo;
static void fill_material_ubo(void);
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
/* Reference-aligned depth: PsyCross's present FBO lags the freshly-fed native
 * scene by exactly ONE frame (PSX build-vs-draw split). Measured 2026-07-10 by
 * a delay sweep against a fast-changing scene (delvin-rescue f2500 shatter
 * fragments + smoke, big-diff px vs -og): depth 0 → 87.7k, 1 → 49.5k (min),
 * 2 → 78.7k, 3 → 96.7k. The old value 2 overshot by one — and the vk-only
 * screenshot path (depth 0) undershot by one, which is how the skew was
 * found in testing (og showed the smoke one frame on from the native fragments). */
#define AB_DIFF_SCENE_DELAY 1   /* ab/diff default: native lagged 1 frame to match PsyCross */
static scene_tri_t s_scene_hist[SCENE_HIST][MAX_SCENE_TRIS]; /* completed-frame ring */
static int                s_scene_hist_n[SCENE_HIST];
static uint32_t           s_scene_hist_bg[SCENE_HIST];
static uint32_t           s_scene_hist_fade[SCENE_HIST];  /* full-screen fade overlay, per snapshot */
static uint32_t           s_scene_hist_freeze[SCENE_HIST]; /* freeze-backdrop overlay, per snapshot */
static int                s_scene_hist_hud_under[SCENE_HIST]; /* flat-HUD-under-player flag, per snapshot */
static int                s_scene_hist_actor_front[SCENE_HIST]; /* actors-in-front (dragon rescue) flag, per snapshot */
static int                s_scene_hist_head = -1;  /* ring index of the most-recent completed frame */
static int                s_scene_hist_count = 0;  /* completed frames recorded (warmup guard) */
static int                s_scene_delay = -2;   /* -2 uninit; >=0 = depth */
static int                s_scene_ref_compare = 1; /* set by init: 1 = ab/diff (lagged reference), 0 = vk-only */

/* 1 while openpete_native_render_screenshot_all composes: the -vk/-ab/-diff
 * screenshot variants sit beside the -og PsyCross capture, so they use the
 * reference-aligned depth even when the LIVE mode is vk-only (whose on-screen
 * present keeps running ahead at depth 0). */
static int s_scene_shot_compose = 0;

static int scene_delay_depth(void) {
    if (s_scene_delay < -1) {
        const char* e = getenv("OPENPETE_NATIVE_SCENE_DELAY");
        /* Explicit env = a forced depth (wins in every mode, incl. the
         * screenshot compose — that's how the alignment sweep is run);
         * unset = -1 (resolve per mode below). */
        if (e && *e) { int d = atoi(e); s_scene_delay = (d < 0) ? 0 : (d >= SCENE_HIST ? SCENE_HIST - 1 : d); }
        else         s_scene_delay = -1;
    }
    if (s_scene_delay >= 0) return s_scene_delay;
    /* Mode default: reference-compare modes and the screenshot compose align
     * with the lagged PsyCross reference; the live vk-only present has no
     * reference → depth 0 (live frame, runs ahead). */
    return (s_scene_ref_compare || s_scene_shot_compose) ? AB_DIFF_SCENE_DELAY : 0;
}

/* No ambient scene category/carry: add_tri takes both EXPLICITLY per call, so a
 * feeder can't leak a stale layer into the next (the old s_scene_category global)
 * and every emit path — including the sub-tick re-extract — is category-correct
 * by construction. */

/* Full-screen fade overlay for the current frame (func_800190D4). 0 = none; else
 * bit31 = active, bits[25:24] = PSX ABR mode (0..3), bits[23:0] = rgb (R in 16..23,
 * G in 8..15, B in 0..7). Reset every frame; the present draws it dead last. */
static uint32_t s_scene_fade = 0;

/* Freeze-frame green overlay for the current frame (pause / inventory / flight
 * results). 0 = inactive; else bit31 = active, bits[23:0] = green tint rgb (the
 * FT4's setRGB0). Reset every frame; when active the present draws frozen_tex
 * desaturated + tinted as the backdrop instead of the world. */
static uint32_t s_scene_freeze = 0;

/* Flat-HUD-vs-player draw order for the current frame. 0 (default) = flat HUD/text
 * drawn OVER the player (on top of everything); 1 = drawn UNDER the player, set only
 * during GS_LevelTransition (the "THE ADVENTURE BEGINS"/"ENTERING <level>" flyby,
 * where Spyro flies in front of the revealed text). Reset per frame; the feeder sets
 * it from g_Gamestate. The 3D HUD icons (HUD3D) and OVERLAY/fade are always on top. */
static int s_scene_hud_under = 0;

/* Actors-in-front draw order for the current frame. 0 (default) = the ACTOR
 * group depth-tests against the world normally. 1 = the ACTOR group draws
 * depth-ISOLATED after the world+player (mid-pass depth clear, same trick as
 * HUD3D): actors still Z-sort among their own faces but are never occluded by
 * terrain. Set by the moby feeder ONLY during the dragon-rescue cutscene
 * (GS_Dragon with the cutscene state machine active), where the .s draws just
 * the rescue actors and the PSX's coarse OT bucketing painted them over the
 * terrain they interpenetrate (the spread wing inside a wall) — true per-pixel
 * Z would amputate them. Reset per frame. */
static int s_scene_actor_front = 0;

void openpete_native_scene_begin(void) { s_scene_n = 0; s_scene_bg = 0; s_scene_fade = 0; s_scene_freeze = 0; s_scene_hud_under = 0; s_scene_actor_front = 0; openpete_mods_material_frame_reset(); /* S8b: per-present interned slots */ }

/* Frame-keyed reset: the first feeder of each game frame clears; later feeders
 * (in any order) append, so the cyclorama + world + mobys compose into one
 * scene instead of clobbering each other. See the header for the rationale.
 * On a frame change, the just-completed scene (still in s_scene) is snapshotted
 * to s_scene_disp before the clear, so the present can draw the 1-frame-delayed
 * copy that matches the reference panel. */
static unsigned s_scene_frame = 0xFFFFFFFFu;
static void alpha_census(const scene_tri_t* scene, int scene_n);  /* 3c instrument, defined below */
static bool scene_has_world(const scene_tri_t* scene, int n);     /* defined below */
static int s_scene_dropped;       /* tris refused this frame (cap hit) — capdbg */
void openpete_native_scene_begin_frame(unsigned frame) {
    if (frame != s_scene_frame) {
        if (s_scene_frame != 0xFFFFFFFFu) {  /* a completed frame is sitting in s_scene */
            /* Permanent safety net: if the scene cap is ever exceeded, terrain/
             * mobys silently vanish — warn once so the cap can be raised rather
             * than leaving a view-dependent dropout in the wild. */
            if (s_scene_dropped > 0) {
                static int s_warned = 0;
                if (!s_warned) { s_warned = 1;
                    fprintf(stderr, "native: WARNING scene cap %d exceeded (frame=%u wanted=%d, "
                            "%d tris dropped) — geometry will vanish view-dependently; raise MAX_SCENE_TRIS\n",
                            MAX_SCENE_TRIS, s_scene_frame, s_scene_n + s_scene_dropped, s_scene_dropped); }
            }
            static int s_capdbg = -1; static int s_scene_peak = 0;
            if (s_capdbg < 0) { const char* e = getenv("OPENPETE_SCENE_CAPDBG"); s_capdbg = (e && *e && *e != '0'); }
            if (s_capdbg) {
                int want = s_scene_n + s_scene_dropped;   /* true demand (raised caps → dropped=0) */
                if (want > s_scene_peak) { s_scene_peak = want;
                    fprintf(stderr, "[scenecap] frame=%u NEW PEAK want=%d (accepted=%d/%d dropped=%d)\n",
                            s_scene_frame, want, s_scene_n, MAX_SCENE_TRIS, s_scene_dropped); }
            }
            s_scene_dropped = 0;
            alpha_census(s_scene, s_scene_n);  /* census the completed scene (runs even headless) */
            int slot = (s_scene_hist_head + 1) % SCENE_HIST;
            if (s_scene_n > 0) memcpy(s_scene_hist[slot], s_scene, (size_t)s_scene_n * sizeof(s_scene[0]));
            s_scene_hist_n[slot] = s_scene_n; s_scene_hist_bg[slot] = s_scene_bg;
            s_scene_hist_fade[slot] = s_scene_fade;
            s_scene_hist_freeze[slot] = s_scene_freeze;
            s_scene_hist_hud_under[slot] = s_scene_hud_under;
            s_scene_hist_actor_front[slot] = s_scene_actor_front;
            s_scene_hist_head = slot;
            if (s_scene_hist_count < SCENE_HIST) s_scene_hist_count++;
        }
        s_scene_n = 0; s_scene_bg = 0; s_scene_fade = 0; s_scene_freeze = 0; s_scene_hud_under = 0; s_scene_actor_front = 0; s_scene_frame = frame;
    }
}

/* The frame stamp of the scene currently accumulating in s_scene
 * (0xFFFFFFFF = none yet). Late feeders (the mod actor-render service)
 * use it to append ONLY to a live scene: calling begin_frame with a
 * newer frame would snapshot-and-wipe a held scene during load stalls. */
unsigned openpete_native_scene_frame(void) { return s_scene_frame; }

/* Mod actor render service: suppress the PLAYER-category scene tris for
 * the NEXT presented frame (the flutter's hover window draws a stand-in
 * actor instead). Set per present by the mod draw service's flush;
 * cleared after each native render. Draw-only — occlusion/census
 * consumers still see the player's coverage. */
static int s_suppress_player;
void openpete_native_set_player_suppress(int on) { s_suppress_player = on ? 1 : 0; }
/* --vertex-precision (`--vertex-precision integer`, live via the O-menu). When
 * set, openpete_native_scene_add_tri_grouped rounds each 3D-world vertex's PSX
 * screen coordinate (sx,sy — still in 512x240 display space at that point) to
 * the integer pixel grid, reproducing the original GTE's int16 SXY snap: the
 * classic PS1 polygon wobble. 0 = smooth sub-pixel (default). Depth (z) and UVs
 * are left alone, so the true Z-buffer + perspective-correct texturing survive.
 * A file-static (not an s_nr field) so the feed chokepoint above s_nr can read
 * it; seeded from opts->vertex_precision, mutated live by the setter below. */
static int s_vertex_snap = 0;

/* --vertex-precision integer: which draw layers get the PSX integer-grid vertex
 * snap. The 3D-world-projected layers wobbled on hardware (the GTE stored screen
 * X/Y as int16); the flat 2D UI layers — HUD text/icons, the dim HUD panel, and
 * the over-everything OVERLAY — were placed at integer screen coords by the game
 * and must stay put (snapping them would only add sub-pixel churn, never wobble).
 * HUD3D (the spinning 3D icon mobys) IS a real GTE projection, so it wobbles too. */
static int category_snaps_vertices(int category) {
    switch (category) {
    case OPENPETE_SCENE_HUD:
    case OPENPETE_SCENE_OVERLAY:
    case OPENPETE_SCENE_HUD_PANEL:
        return 0;
    default:
        return 1;
    }
}

void openpete_native_scene_add_tri_grouped(const openpete_scene_tri *tri, int category,
                                           int carry, int group) {
    if (!tri) return;
    /* OPENPETE_SCENE_BOXDBG="x0,y0,x1,y1,fA,fB": temporary forensics — log every
     * scene tri (any feeder) whose centroid falls in the PSX-screen box for
     * frames [fA,fB], with layer/texture identity. Display-only. */
    { static int s_bd = -1; static int bx0,by0,bx1,by1,bfa,bfb;
      if (s_bd < 0) { const char* e = getenv("OPENPETE_SCENE_BOXDBG");
          s_bd = 0;
          if (e && sscanf(e, "%d,%d,%d,%d,%d,%d", &bx0,&by0,&bx1,&by1,&bfa,&bfb) == 6) s_bd = 1; }
      if (s_bd == 1 && (int)s_scene_frame >= bfa && (int)s_scene_frame <= bfb) {
          float cx = (tri->v[0].sx + tri->v[1].sx + tri->v[2].sx) / 3.0f;
          float cy = (tri->v[0].sy + tri->v[1].sy + tri->v[2].sy) / 3.0f;
          if (cx >= bx0 && cx <= bx1 && cy >= by0 && cy <= by1)
              fprintf(stderr, "[sbox] f=%u cat=%d tex=%d tp=%d,%d cl=%d,%d bpp=%d bl=%d "
                      "rgba=%08X,%08X,%08X v0=%.1f,%.1f,%.3f v1=%.1f,%.1f v2=%.1f,%.1f uv0=%.0f,%.0f\n",
                      s_scene_frame, category, tri->textured, tri->tpage_x, tri->tpage_y,
                      tri->clut_x, tri->clut_y, tri->bpp, tri->blend,
                      tri->v[0].rgba, tri->v[1].rgba, tri->v[2].rgba,
                      tri->v[0].sx, tri->v[0].sy, tri->v[0].z, tri->v[1].sx, tri->v[1].sy,
                      tri->v[2].sx, tri->v[2].sy, tri->v[0].u, tri->v[0].v);
      } }
    if (s_scene_n < MAX_SCENE_TRIS) {
        /* geometry+material prefix copy (the feeder tri IS the stored tri's
         * layout prefix — asserted at scene_tri_t), then stamp the routing
         * state from the explicit args. */
        memcpy(&s_scene[s_scene_n], tri, sizeof *tri);
        s_scene[s_scene_n].category = category;  /* explicit layer (ignored for blend!=0) */
        s_scene[s_scene_n].carry = carry;        /* explicit 2D-UI sub-tick carry flag */
        s_scene[s_scene_n].group = group;        /* portal grouping id (PORTAL_* only) */
        s_scene[s_scene_n].tag = s_cur_tag;      /* sticky provenance (set_tag) */
        if (s_cur_tag && tag_tint_on()) {
            /* Coverage instrument: hash the tag to a stable tint and modulate
             * the vertex colours — "which source produced this pixel". */
            uint32_t hsh = s_cur_tag * 2654435761u;
            uint32_t tr = 128u + (hsh & 0x7Fu), tg = 128u + ((hsh >> 8) & 0x7Fu),
                     tb = 128u + ((hsh >> 16) & 0x7Fu);
            for (int k = 0; k < 3; k++) {
                uint32_t c = s_scene[s_scene_n].v[k].rgba;
                uint32_t r = (((c) & 0xFFu) * tr) >> 8,
                         g = (((c >> 8) & 0xFFu) * tg) >> 8,
                         b = (((c >> 16) & 0xFFu) * tb) >> 8;
                s_scene[s_scene_n].v[k].rgba = (c & 0xFF000000u) | (b << 16) | (g << 8) | r;
            }
        }
        /* --vertex-precision integer: re-snap the (still 512x240) screen coords to
         * the PSX integer pixel grid — the polygon wobble. Round X/Y only; depth
         * and UVs are untouched (Z-buffer + perspective texturing preserved). The
         * grid is display-resolution-independent because sx/sy are pre-normalize
         * (scene_to_clip upscales afterward), so SSAA / widescreen keep the exact
         * 512x240 jitter the original GTE produced. */
        if (s_vertex_snap && category_snaps_vertices(category)) {
            openpete_scene_vert *sv = s_scene[s_scene_n].v;
            for (int k = 0; k < 3; k++) {
                sv[k].sx = floorf(sv[k].sx + 0.5f);
                sv[k].sy = floorf(sv[k].sy + 0.5f);
            }
        }
        s_scene_n++;
    } else {
        s_scene_dropped++;   /* cap hit — this tri (and the rest of the feed) is lost */
    }
}
void openpete_native_scene_add_tri(const openpete_scene_tri *tri, int category, int carry) {
    openpete_native_scene_add_tri_grouped(tri, category, carry, 0);
}
void openpete_native_scene_set_bg(uint32_t rgba) { s_scene_bg = rgba; }

/* Gamestate draw-env clear colour (the framebuffer background the canonical
 * render auto-clears to). Two producers: the PutDrawEnv thunk (fallback,
 * trails the OT build by one VSync — the game PutDrawEnvs an OT's env only
 * when it DRAWS it) and the game module's per-canonical-present sync from the
 * DRAWENV struct in RAM (spyro-1: cyclorama_native_env_clear_commit reads
 * g_CurDB->m_DrawEnv, the env the presented scene's DrawOTag will use —
 * without it the transition boundary flashed the stale clear for a present).
 * The primary bg source in draw_native_scene (preferred over g_Cyclorama,
 * which is stale in cyclorama-less states). rgba R,G,B,A; 0 = unset / no
 * auto-clear → fall back to the scene-fed colour. Persists across sub-tick
 * presents (both producers fire on canonical frames only). */
static uint32_t s_drawenv_clear = 0;
void openpete_native_set_clear_color(int isbg, int r, int g, int b) {
    uint32_t v = isbg
        ? (0xFF000000u | (uint32_t)(r & 0xFF) | ((uint32_t)(g & 0xFF) << 8)
                       | ((uint32_t)(b & 0xFF) << 16))
        : 0u;   /* no auto-clear → defer to the scene-fed bg / ref corner */
    {
        static int s_census = -1;
        if (s_census < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); s_census = (e && *e && *e != '0'); }
        if (s_census && v != s_drawenv_clear)
            fprintf(stderr, "[census f=%u CLEAR] isbg=%d rgb=%08x (was %08x)\n",
                    s_scene_frame, isbg, v, s_drawenv_clear);
    }
    s_drawenv_clear = v;
}
/* The game's fade for THIS tick + a "set this tick" flag. The flag (not the
 * begin_frame reset of s_scene_fade) is what tells the re-extract whether the
 * fade is actually active: the game calls func_800190D4 every tick the fade is
 * on and simply STOPS when it ends (no explicit clear). The re-extract's own
 * apply_overlay writes s_scene_fade, which would otherwise feed back into
 * commit_fade and freeze the last transition's fade on screen forever. */
static uint32_t s_game_fade = 0;
static int      s_game_fade_seen = 0;
/* Same "active only if the game set it this tick" tracking for the freeze backdrop
 * (pause/inventory/flight-results). Without it the carry fed back into itself
 * (apply_overlay re-set s_scene_freeze from the carry, capture read it back) and the
 * freeze never cleared when the menu closed → the world stayed hidden on unpause. */
static uint32_t s_game_freeze = 0;
static int      s_game_freeze_seen = 0;

void openpete_native_scene_set_fade(int mode, int r, int g, int b) {
    uint32_t v;
    if (mode < 0) { v = 0; }
    else {
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        v = (1u << 31) | ((uint32_t)(mode & 3) << 24)
          | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    s_scene_fade = v;                       /* raw (no-interp) present path */
    s_game_fade = v; s_game_fade_seen = 1;  /* the game set the fade this tick */
}
void openpete_native_scene_set_freeze_backdrop(int r, int g, int b, int fade) {
    if (r < 0) { s_scene_freeze = 0; return; }
    if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    if (fade < 0) fade = 0; if (fade > 127) fade = 127;   /* 7-bit fade shift in bits 24-30 */
    s_scene_freeze = (1u << 31) | ((uint32_t)fade << 24)
                   | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    s_game_freeze = s_scene_freeze; s_game_freeze_seen = 1;  /* the game set it this tick */
}
void openpete_native_scene_set_hud_under_player(int under) { s_scene_hud_under = under ? 1 : 0; }

void openpete_native_scene_set_actor_front(int front) { s_scene_actor_front = front ? 1 : 0; }

/* --- 2D-overlay carry across sub-tick re-extracts -------------------------
 * The fade overlay (func_800190D4) and the cutscene letterbox bars
 * (func_80018F30, OVERLAY-category F4s) + menu panel (HUD_PANEL) are produced
 * by the 2D-UI feeder while the game renders. They are NOT re-derivable from
 * gamestate by the sub-tick re-extract (it has no 2D-prim chokepoint to hook),
 * and the re-extract's scene_begin() wipes them — so without carrying them they
 * vanish on every re-extracted present (which, since the canonical present now
 * also routes through the re-extract for the 1-tick lag, means they vanish
 * outright). They're screen-space and tick-constant, so capture them from the
 * live scene before the rebuild and re-apply them after. Categories that the
 * 3D feeders re-produce (HUD/HUD3D billboards, PLAYER near-OT text) are NOT
 * carried by category — they'd double. The 2D-UI HUD SPRITES/LINES (title logo,
 * "PRESS START", HUD glyph text, UI separators) live in OPENPETE_SCENE_HUD too,
 * but the 3D feeders never re-derive THEM, so they carry by an explicit per-tri
 * flag (set_carry, stamped by add_tri) that the moby near-OT billboards leave 0. */
#define OVERLAY_CARRY_MAX 1024
static scene_tri_t s_overlay_carry[OVERLAY_CARRY_MAX];      /* curr (latest canonical capture) */
static int                s_overlay_carry_n;
static uint32_t           s_overlay_carry_freeze;
/* The previous canonical carry set, so apply_overlay can interpolate the SCREEN
 * POSITION of an animated 2D-UI element across sub-ticks — the title-logo drop-in
 * slide moves a few px/tick; held (un-interpolated) it staircases at 30 Hz just
 * like the fade did. Rolled curr->prev by capture_overlay (canonical only); paired
 * by index when the two captures have equal count + matching sprite identity
 * (category/textured/uv), else that tri snaps to curr (a structural change — a
 * blink toggling the count, a glyph swap — is discrete anyway). */
static scene_tri_t s_overlay_prev[OVERLAY_CARRY_MAX];
static int                s_overlay_prev_n;
/* Wall-clock timestamps of the two most-recent CANONICAL overlay captures, so
 * apply_overlay computes the position/fade interpolation alpha from the OVERLAY's
 * OWN cadence — NOT the camera-bracket alpha, which the re-extract leaves at 0
 * whenever the camera+Spyro ring dedupes (a STATIC camera: the title screen, a
 * pause menu). The logo drop-in slides while the title camera is still, so without
 * a camera-independent alpha it staircased at the canonical tick rate. (Same trap
 * the 3D HUD-icon spin/slide hit — fixed there with per-object timestamps.) */
static op_time_t          s_overlay_curr_ns, s_overlay_prev_ns;

/* The fade ramps per sim tick, so unlike the (static) letterbox/panel tris it is
 * interpolated rather than held: s_fade_prev/curr are the two most-recent
 * CANONICAL fades (rolled by commit_fade on each canonical present), and
 * apply_overlay lerps them by the spine alpha — a 1-tick-lagged smooth ramp,
 * consistent with the rest of the scene. Without this the fade staircases at
 * 30 Hz (intermediates hold the previous tick's brightness). */
static uint32_t s_fade_prev, s_fade_curr;

void openpete_native_scene_commit_fade(void) {
    s_fade_prev = s_fade_curr;
    /* The fade is active this tick only if the game actually called set_fade
     * since the last commit; else it ended (the game stopped calling) → off. */
    s_fade_curr = s_game_fade_seen ? s_game_fade : 0;
    s_game_fade_seen = 0;
    /* Same one-tick liveness for the freeze backdrop carry: held while the game keeps
     * emitting the freeze FT4, cleared the tick it stops (menu closed → unpause), so
     * the spine's apply_overlay turns the backdrop off and the world reappears. (This
     * replaces capturing it from s_scene_freeze, which fed back into itself.) */
    uint32_t prev_freeze = s_overlay_carry_freeze;
    uint32_t new_freeze  = s_game_freeze_seen ? s_game_freeze : 0;
    /* Bridge a 1-frame freeze gap at the GS_Paused -> GS_ExitLevel handoff: the pause
     * menu stops emitting its freeze FT4 the frame it closes, but GS_ExitLevel's
     * func_8001C694 hasn't drawn its own freeze yet — so for exactly one canonical
     * tick neither sets the freeze and the carry would drop, letting the spine's
     * re-extracted (reset) Spyro + bg flash through. The game doesn't flicker because
     * its frozen framebuffer persists in VRAM. A handoff frame carries only the reset
     * Spyro (PLAYER) + bg with no real WORLD/ACTOR geometry, so when the freeze is
     * dropping but the scene has no world, hold the previous freeze. A genuine unpause
     * rebuilds terrain (WORLD) that same tick, so scene_has_world is true and the
     * freeze drops correctly — the world reappears with no lag. */
    if (new_freeze == 0 && prev_freeze != 0 && !scene_has_world(s_scene, s_scene_n)) {
        new_freeze = prev_freeze;
        /* The canonical present of this degenerate tick may show the RAW game-fiber
         * scene (the sub-tick re-extract that re-applies the carry is skipped on a
         * stuck/transition tick), which left the live s_scene_freeze at 0 — so force
         * it too, and drop the degenerate geometry (reset Spyro + bg) so only the
         * held freeze shows, exactly as on a settled freeze frame. */
        s_scene_freeze = new_freeze;
        s_scene_n = 0;
    }
    s_overlay_carry_freeze = new_freeze;
    s_game_freeze_seen = 0;
}

/* Lerp two packed fade words (bit31 active, [25:24] ABR mode, [23:0] rgb). */
static uint32_t fade_lerp(uint32_t a, uint32_t b, double t) {
    if (!(b & (1u << 31))) return 0;                         /* curr inactive → no fade */
    if (!(a & (1u << 31))) return b;                         /* prev inactive → snap on */
    if ((a & (3u << 24)) != (b & (3u << 24))) return b;      /* mode changed → snap */
    int ar=(a>>16)&0xFF, ag=(a>>8)&0xFF, ab=a&0xFF;
    int br=(b>>16)&0xFF, bg=(b>>8)&0xFF, bb=b&0xFF;
    int r =(int)(ar+(br-ar)*t+0.5), g=(int)(ag+(bg-ag)*t+0.5), bl=(int)(ab+(bb-ab)*t+0.5);
    return (b & 0xFF000000u) | ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)bl;
}

void openpete_native_scene_capture_overlay(void) {
    /* Roll curr -> prev so apply_overlay can interpolate animated 2D-UI positions
     * (the logo slide-in) across sub-ticks. Runs on the CANONICAL present only, so
     * prev/curr are consecutive canonical captures (one sim tick apart). */
    if (s_overlay_carry_n > 0)
        memcpy(s_overlay_prev, s_overlay_carry, (size_t)s_overlay_carry_n * sizeof(s_overlay_carry[0]));
    s_overlay_prev_n = s_overlay_carry_n;
    s_overlay_prev_ns = s_overlay_curr_ns;   /* roll the capture timestamp too */
    s_overlay_curr_ns = op_now();
    s_overlay_carry_n = 0;
    int s_overlay_dropped = 0;
    for (int t = 0; t < s_scene_n; t++) {
        int c = s_scene[t].category, cy = s_scene[t].carry;
        /* carry==2 = a tri RE-APPLIED by apply_overlay last present. The canonical
         * game render appends to the prior sub-tick's (un-wiped) scene, so those
         * re-applied tris are still here — skip them or the carry set accumulates
         * unbounded (the 2D-UI logo cloning). Only the GAME RENDER's fresh emission
         * is captured: OVERLAY/HUD_PANEL by category, 2D-UI sprites/lines by carry==1
         * (see the carry arg to add_tri). Capture runs on the CANONICAL present
         * only (run.c), so a clear here is correct (sub-ticks hold the set). */
        if (cy == 2) continue;
        if (c == OPENPETE_SCENE_OVERLAY || c == OPENPETE_SCENE_HUD_PANEL || cy == 1) {
            if (s_overlay_carry_n < OVERLAY_CARRY_MAX) s_overlay_carry[s_overlay_carry_n++] = s_scene[t];
            else s_overlay_dropped++;
        }
    }
    /* Mirror the scene-cap warn-once: a busy title/menu that overruns the carry
     * buffer silently loses 2D-UI sprites sub-tick with no signal — warn so the
     * cap can be raised rather than leaving a view-dependent dropout in the wild. */
    if (s_overlay_dropped > 0) {
        static int s_ov_warned = 0;
        if (!s_ov_warned) { s_ov_warned = 1;
            fprintf(stderr, "native: WARNING OVERLAY_CARRY_MAX %d exceeded (frame=%u, %d 2D-UI carry "
                    "tris dropped) — sprites will vanish sub-tick; raise OVERLAY_CARRY_MAX\n",
                    OVERLAY_CARRY_MAX, s_scene_frame, s_overlay_dropped);
        }
    }
    /* NOTE: the freeze backdrop carry is committed in commit_fade (the per-tick
     * overlay-signal commit), NOT here — capturing it from s_scene_freeze created a
     * feedback loop (apply_overlay re-set s_scene_freeze from the carry, so capture
     * read it straight back and the freeze never died after the menu closed → the
     * world stayed hidden on unpause). See s_game_freeze_seen. */
}

void openpete_native_scene_apply_overlay(double alpha) {
    /* The passed `alpha` is the camera-bracket alpha — 0 when the camera ring
     * dedupes (static camera). Animated 2D-UI (the logo drop-in) moves while the
     * camera is still, so derive the overlay's own alpha from its capture cadence:
     * 1-tick-lagged (now≈curr_ns ⇒ show prev, advance to curr over the next
     * capture period), exactly the camera bracket's convention but from the
     * overlay's timestamps. Falls back to the camera alpha if the timestamps
     * aren't primed yet (first frames). */
    double oalpha = alpha;
    if (s_overlay_curr_ns > s_overlay_prev_ns && s_overlay_prev_ns > 0) {
        double period = (double)(s_overlay_curr_ns - s_overlay_prev_ns);
        oalpha = (double)(op_now() - s_overlay_curr_ns) / period;
        if (oalpha < 0.0) oalpha = 0.0; else if (oalpha > 1.0) oalpha = 1.0;
    }
    /* Pair curr with prev by index only when the two captures are structurally the
     * same (equal count) — a count change (a blink toggling, a glyph added) makes
     * index pairing meaningless, so snap to curr that present. */
    int pairable = (s_overlay_prev_n == s_overlay_carry_n);
    for (int t = 0; t < s_overlay_carry_n && s_scene_n < MAX_SCENE_TRIS; t++) {
        scene_tri_t tri = s_overlay_carry[t];   /* curr; category already stamped */
        if (pairable) {
            const scene_tri_t *p = &s_overlay_prev[t];
            /* Same sprite? A rigid screen translation (the slide) keeps the texture
             * region, so category + textured + the first vertex's UV identify it; a
             * glyph swap at the same slot differs in UV and snaps to curr. */
            if (p->category == tri.category && p->textured == tri.textured &&
                p->v[0].u == tri.v[0].u && p->v[0].v == tri.v[0].v) {
                for (int k = 0; k < 3; k++) {
                    tri.v[k].sx = (float)(p->v[k].sx + (tri.v[k].sx - p->v[k].sx) * oalpha);
                    tri.v[k].sy = (float)(p->v[k].sy + (tri.v[k].sy - p->v[k].sy) * oalpha);
                }
            }
        }
        tri.carry = 2;                             /* mark re-applied so the next capture skips it */
        s_scene[s_scene_n++] = tri;
    }
    s_scene_freeze = s_overlay_carry_freeze;
    s_scene_fade   = fade_lerp(s_fade_prev, s_fade_curr, oalpha);
}

/* The scene buffer the present should draw: the depth-frames-back snapshot when
 * a reference-compare delay is set (A/B-synced with the lagged reference), else
 * the live accumulator (vk-only, runs ahead). Falls back to the live frame
 * during warmup before enough history has accumulated. */
static const scene_tri_t* scene_present_buf(int* out_n, uint32_t* out_bg,
                                                   uint32_t* out_fade, uint32_t* out_freeze,
                                                   int* out_hud_under, int* out_actor_front) {
    int depth = scene_delay_depth();
    if (depth > 0 && depth <= s_scene_hist_count && s_scene_hist_head >= 0) {
        int slot = (s_scene_hist_head - (depth - 1) + SCENE_HIST * 4) % SCENE_HIST;
        *out_n = s_scene_hist_n[slot]; *out_bg = s_scene_hist_bg[slot];
        *out_fade = s_scene_hist_fade[slot];
        if (out_freeze) *out_freeze = s_scene_hist_freeze[slot];
        if (out_hud_under) *out_hud_under = s_scene_hist_hud_under[slot];
        if (out_actor_front) *out_actor_front = s_scene_hist_actor_front[slot];
        return s_scene_hist[slot];
    }
    *out_n = s_scene_n; *out_bg = s_scene_bg; *out_fade = s_scene_fade;
    if (out_freeze) *out_freeze = s_scene_freeze;
    if (out_hud_under) *out_hud_under = s_scene_hud_under;
    if (out_actor_front) *out_actor_front = s_scene_actor_front;
    return s_scene;
}


/* True if the presented scene holds real WORLD or ACTOR geometry — i.e. a full
 * gameplay frame worth snapshotting into frozen_tex (the pause/inventory freeze
 * backdrop). Menu/transition frames (only HUD/OVERLAY tris, or empty) return false,
 * so the snapshot keeps the last gameplay frame to freeze.
 *
 * PLAYER (Spyro) alone does NOT qualify: at the pause-start transition the world
 * feeders cut out one present before the freeze FT4 is emitted, leaving a degenerate
 * frame with only Spyro's body (W=0, Ac=0, Pl>0, freeze not yet set). Counting that
 * as a world frame snapshots a near-empty image into frozen_tex — the "residual
 * framebuffer instead of the cyclorama" under the high-FPS spine's pause menu. A real
 * gameplay frame always carries terrain (WORLD) and/or mobys (ACTOR), so gate on
 * those and the transition frame is rejected, preserving the last full world. */
static bool scene_has_world(const scene_tri_t* scene, int n) {
    for (int t = 0; t < n; t++) {
        int c = scene[t].category;
        /* SKY counts as world: the cyclorama was WORLD before the W2.6-c split
         * and a dome-only frame qualified as a gameplay frame — preserve that. */
        if (c == OPENPETE_SCENE_WORLD || c == OPENPETE_SCENE_ACTOR ||
            c == OPENPETE_SCENE_SKY)
            return true;
    }
    return false;
}

/* Single native renderer instance (one window per process). */
static struct {
    bool          enabled;
    bool          headless;     /* W1.1: surfaceless capture (no window/present); screenshots only */
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
    rhi_pipeline *gouraud_sub_pipe;   /* 3b.2: subtractive UNTEXTURED gouraud (ABR mode 2, B-F) — the Spyro shadow as a DEPTH-AWARE decal (shadow_decal.frag samples opaque_depth at set 0; depth-test OFF, the shader owns occlusion) */
    rhi_pipeline *gouraud_sub_ztest_pipe; /* fallback subtractive pipe (plain depth test + W2.5 hw bias) when the depth pre-pass is unavailable */
    rhi_pipeline *gouraud_sub_fade_pipe;  /* subtractive, depth-test OFF, quad.frag — the full-screen mode-2 fade (NOT the decal pipe, which would sample/discard) */
    rhi_pipeline *gouraud_add_pipe;   /* 3b.3: additive UNTEXTURED gouraud (ABR mode 1, B+F) — glow rings */
    rhi_pipeline *gouraud_noz_pipe;   /* opaque UNTEXTURED gouraud, depth-test OFF + stencil TEST — portal-sky colours inside the aperture mask (W2.6) */
    rhi_pipeline *portal_mask_pipe;   /* W2.6: invisible (colour-write-off) depth-tested stencil-MARK of the portal aperture */
    rhi_pipeline *gouraud_mark_pipe;  /* W2.6-c: WORLD-opaque untextured + stencil-MARK coverage (may be NULL → plain pipe, no hole-fill) */
    rhi_pipeline *scene_tex_mark_pipe;/* W2.6-c: WORLD-opaque textured + stencil-MARK coverage (may be NULL → plain pipe, no hole-fill) */
    rhi_pipeline *gouraud_fill_pipe;  /* W2.6-c: WORLD_FILL LP backing, stencil TEST_ZERO (paints only unmarked pixels; may be NULL → no hole-fill) */
    rhi_pipeline *scene_tex_pipe;     /* 3a.3: opaque textured scene tris (VRAM sampler) */
    rhi_pipeline *scene_tex_blend_pipe; /* 3a.3: semi-transparent (ABE) textured tris (alpha, ABR mode 0) */
    rhi_pipeline *scene_tex_add_pipe; /* 3b.1: additive (ABR mode 1, B+F) textured tris — flame */
    rhi_pipeline *scene_tex_sub_pipe; /* 3b.2: subtractive (ABR mode 2, B-F) textured tris — moby shadows (plain depth test + bias; the fallback when the depth pre-pass is unavailable) */
    rhi_pipeline *scene_tex_sub_decal_pipe; /* moby shadows as a DEPTH-AWARE decal: shadow_decal_tex.frag samples opaque_depth (set 0) + VRAM (set 1); depth-test OFF, the shader owns wall/steep/platform occlusion. NULL → fall back to scene_tex_sub_pipe */
    rhi_pipeline *gouraud_hud_pipe;   /* HUD overlay: UNTEXTURED gouraud, depth-test OFF (on top) */
    rhi_pipeline *gouraud_hud_blend_pipe; /* HUD panel: UNTEXTURED alpha gouraud, depth-test OFF (menu dark panel under text) */
    rhi_pipeline *gouraud_hud_sub_pipe; /* HUD panel: UNTEXTURED subtractive gouraud, depth-test OFF (the ABR mode-2 shaded text box, B-F) */
    rhi_pipeline *scene_tex_hud_pipe; /* HUD overlay: textured (VRAM sampler), depth-test OFF (on top) */
    rhi_pipeline *hd_tex_pipe;        /* HD replacement, HUD overlay: RGBA sampler, alpha blend, depth OFF */
    rhi_pipeline *hd_cutout_pipe;     /* HD replacement, opaque scene: depth-tested, alpha-cutout */
    rhi_pipeline *hd_cutout_mark_pipe;/* cutout twin with stencil MARK (WORLD group when hole-fill is on) */
    rhi_pipeline *hd_scene_blend_pipe;/* HD replacement, alpha scene group (ABR 0): ztest, no zwrite */
    rhi_pipeline *hd_scene_add_pipe;  /* HD replacement, additive scene group (ABR 1): ztest, no zwrite */
    rhi_buffer   *scene_vb;     /* MAX_SCENE_TRIS*3 verts, rebuilt per frame */
    float         p0_cx, p0_hx, p0_hy; /* panel-0 fitted rect (clip space, cy=0) — the 4:3 CONTENT sub-rect */
    /* Target display aspect (W/H), from --aspect (0 → default 4:3). Drives the
     * Hor+ render aspect independently of the window: the scene renders at this
     * aspect and is letterboxed/pillarboxed into the window (like PsyCross), NOT
     * stretched/auto-widened to fill it. Set at init from opts->aspect_w_over_h. */
    float         aspect_wh;
    /* Widescreen (Hor+) widen factor = render_aspect / 4:3, i.e. how much MORE
     * horizontal world the --aspect view reveals vs 4:3 (1.0 = plain 4:3). Now
     * driven by --aspect (FIXED per run), not the window. The panel-0 content
     * mapping (scene_to_clip via p0_hx) is the centred 4:3 sub-rect; the reveal
     * extends to ±p0_hx*ws_factor (the --aspect rect), which fit_cell fits into
     * the window with bars. vk-only (ab/diff stay 4:3). See update_layout. */
    float         ws_factor;

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

    /* SSAA (--ssaa N): the live vk scene renders into a supersampled render
     * target (ss_factor× the window per axis), then a fragment resolve pass
     * box-averages it in linear light into the 1× offscreen. ss_factor is the
     * EFFECTIVE factor — capped to the GPU's max image dimension (and reduced on
     * an allocation failure), so a large --ssaa degrades gracefully. ssaa==1 (or
     * ss_factor==1) disables it: the scene renders straight into the offscreen. */
    int           ssaa;             /* requested factor (opts->ssaa, >=1) */
    rhi_pipeline *resolve_pipe;     /* psx_sample.vert + resolve.frag (box+gamma) */
    rhi_texture  *ss_rtt;           /* supersampled scene target (ss_w × ss_h) */
    int           ss_w, ss_h, ss_factor;

    /* Freeze-frame green overlay (pause / inventory / flight-results backdrop).
     * native_rtt is the canonical native frame each present; frozen_tex snapshots
     * the last WORLD frame's native render (copied from native_rtt on non-menu
     * frames). When the game's freeze backdrop is active, the scene draws
     * frozen_tex desaturated + green-tinted (freeze_overlay_pipe) instead of the
     * FT4 that samples PsyCross's VRAM scratch — so the green overlay shows OUR
     * render and survives the end-state where PsyCross's VRAM is gone. */
    rhi_texture  *frozen_tex;
    int           frozen_w, frozen_h;
    bool          frozen_valid;       /* a world frame has been captured */
    rhi_pipeline *freeze_overlay_pipe;/* samples frozen_tex, luma + tint (set 0) */
    rhi_buffer   *fs_quad_vb;         /* full-target (-1..1) quad, UV 0..1 (RT copies) */
    rhi_buffer   *overlay_vb;         /* FPS overlay glyph quads (NDC), rebuilt per present */
    rhi_buffer   *mod_ovl_vb;         /* mod draw-service overlay (rects + text glyphs, NDC), rebuilt per present */
    rhi_buffer   *menu_vb;            /* in-game settings menu panel/border/selection quads (NDC) */
    rhi_buffer   *menu_glyph_vb;      /* in-game settings menu 3D glyph text (NDC, depth-tested) */

    /* FPS-overlay 4× supersample: the glyphs render into ovl_ss (RGBA,
     * OVL_SS_FACTOR× the overlay region), then ovl_resolve_pipe box-downsamples +
     * straight-alpha-blends them onto the offscreen via the ovl_quad rect — so the
     * corner counter is anti-aliased regardless of window/render res. See
     * draw_fps_overlay. */
    rhi_texture  *ovl_ss;
    int           ovl_ss_w, ovl_ss_h;
    rhi_pipeline *ovl_resolve_pipe;   /* overlay_resolve.frag, box + straight alpha, RHI_BLEND_ALPHA */
    rhi_buffer   *ovl_quad_vb;        /* composite quad at the overlay's screen rect */
    int           ovl_quad_win_w, ovl_quad_win_h;
    float         ovl_quad_ox;        /* overlay-x offset (win_h units) the quad was built at; shifts the counter right of the open settings menu */

    /* Settings-menu glyph/border supersample: same idea as ovl_ss but sized to the
     * menu PANEL rect (much larger). The glyph+border layer renders into menu_ss at
     * MENU_SS_FACTOR×, then reuses ovl_resolve_pipe to box-downsample + alpha-blend
     * it onto the offscreen via menu_quad — so the 3D menu text is anti-aliased. */
    rhi_texture  *menu_ss;
    int           menu_ss_w, menu_ss_h;
    rhi_buffer   *menu_quad_vb;       /* composite quad at the panel rect */
    int           menu_quad_win_w, menu_quad_win_h;

    /* Depth-aware decal infrastructure: the WORLD-opaque depth PRE-PASS.
     * Rendered ONCE per present in the FULL-EXTENT PSX mapping at
     * max(ref, window) resolution; every consumer pass (native_rtt / SSAA /
     * offscreen panel / screenshots) samples it by PSX screen coord through the
     * same affine — no per-target re-render and no per-target alignment
     * contract. opaque_depth allocates lazily on the first present whose scene
     * carries a Spyro shadow (blend-3 untextured tris); NULL means the decal
     * falls back to the plain depth-tested subtractive pipe. Reusable by future
     * depth-aware passes (glow auras, soft particles). */
    rhi_pipeline *prepass_pipe;       /* depth-only raster (colour-write-off) */
    rhi_texture  *opaque_depth;       /* rhi_create_render_target_depth_sampled */
    int           od_w, od_h;         /* current opaque_depth allocation */
    rhi_buffer   *prepass_vb;         /* WORLD-opaque clip geometry, one batched draw */
    rhi_pipeline *occl_pipe;          /* occl_downsample.frag: prepass depth → grid (may be NULL) */
    rhi_texture  *occl_grid;          /* OCCL_GW×OCCL_GH grid RT (may be NULL → no culling) */
} s_nr;

/* When set, draw_native_scene maps the PSX screen to the FULL render target
 * (clip -1..1) instead of panel-0's window rect — used when rendering into
 * native_rtt for the diff pass (native_rtt IS the frame, no panel inset). */
static int s_render_full_extent = 0;
/* Horizontal divisor for the full-extent mapping. 1.0 = the 4:3 view fills the
 * target (native_rtt / diff). >1.0 = the Hor+ WIDESCREEN view fills the target:
 * the 4:3 content compresses to the centre (1/wf) and the revealed side world
 * fills out to ±1 — used to capture the freeze backdrop at the widescreen extent
 * so the pause/exit backdrop fills a widescreen window (not pillarboxed). */
static float s_render_freeze_wf = 1.0f;

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

    /* W1.1: under --headless, bring the native renderer up SURFACELESS purely to
     * service screenshots (CI visual-regression capture under lavapipe) — but
     * only when a native renderer is selected (checked above) AND screenshots
     * are requested. Plain --headless (the oracle / determinism gates) keeps
     * native OFF, so those gates are unaffected. The live present loop never runs
     * headless; only openpete_native_render_screenshot_all is driven (run.c). */
    const bool headless = opts->headless;
    if (headless && opts->screenshot_frame_count <= 0) {
        OPL_WARN_F("native", "--renderer vk|ab|diff ignored under --headless (no screenshots requested)");
        return 0;
    }
    s_nr.headless = headless;

    /* --texpack-dir: point the HD texture-replacement scan at a chosen dir. Set
     * before openpete_vram_install_ops() below runs texture_pack_init(). NULL is
     * a no-op (falls back to OPENPETE_TEXPACK_DIR env, then the built-in dir). */
    texture_pack_set_dir_override(opts->texpack_dir);

    /* A/B compare mode shows the PsyCross reference in a second panel and delays
     * the native scene by one frame to align with that lagged reference. vk and
     * diff are single full-window panels with no reference panel and no scene
     * delay. (diff's live colour-diff render + diff screenshot land in a later
     * step; for now diff mirrors vk so the flag is usable.) */
    bool ab_mode = (opts->renderer == OPENPETE_RENDERER_AB);
    bool diff_mode = (opts->renderer == OPENPETE_RENDERER_DIFF);
    s_nr.ab_mode  = ab_mode;
    s_nr.diff_mode = diff_mode;
    /* Render aspect from --aspect (W/H); 0 → default 4:3. Drives the Hor+ amount
     * + letterbox/pillarbox (vk-only; ab/diff stay 4:3 vs the reference). */
    s_nr.aspect_wh = (opts->aspect_w_over_h > 0.0f) ? opts->aspect_w_over_h : 0.0f;
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

    SDL_Window *win = NULL;
    rhi_device *dev = NULL;
    if (headless) {
        /* No window: render into the offscreen/RTT and read back for screenshots.
         * The offscreen extent is the resolved size; the screenshot path composes
         * at the reference resolution regardless, so w/h only size unused buffers. */
        dev = rhi_create_headless("openpete-native", (uint32_t)w, (uint32_t)h);
        if (!dev) {
            OPL_ERROR_F("native", "rhi_create_headless failed; native capture disabled");
            return 1;
        }
    } else {
        /* vk (the default display) wears the game's own name; the dev compare modes
         * keep a descriptive label so the A/B / diff windows are self-identifying. */
        const char *title = ab_mode   ? "openpete — native renderer (A/B)"
                          : diff_mode ? "openpete — native renderer (diff)"
                          : (opts->display_name && opts->display_name[0])
                                      ? opts->display_name
                                      : "Spyro the Dragon";
        SDL_Window *adopt = (SDL_Window*)opts->adopted_window;
        if (adopt && !ab_mode && !diff_mode) {
            /* In-process continuation: reuse the first-run splash's window
             * (main.cpp hands it over in windowed vk mode only; its
             * SDL_Renderer is already destroyed). SDL converts a plain
             * window to Vulkan inside SDL_Vulkan_CreateSurface, so the
             * missing SDL_WINDOW_VULKAN creation flag is fine — only the
             * game's title/flags/size need applying. The splash opens at
             * the game's size, so this is normally just a retitle. */
            win = adopt;
            SDL_SetWindowTitle(win, title);
            SDL_SetWindowResizable(win, true);
            int cur_w = 0, cur_h = 0;
            SDL_GetWindowSize(win, &cur_w, &cur_h);
            if (cur_w != w || cur_h != h)
                SDL_SetWindowSize(win, w, h);
            OPL_INFO_F("native", "adopted splash window (%dx%d)", w, h);
        } else {
            win = SDL_CreateWindow(title, w, h,
                                   SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
            if (!win) {
                OPL_ERROR_F("native", "SDL_CreateWindow(VULKAN) failed: %s", SDL_GetError());
                return 1;
            }
        }

        dev = rhi_create(win, "openpete-native", opts->vsync);
        if (!dev) {
            OPL_ERROR_F("native", "rhi_create failed; continuing without native A/B");
            SDL_DestroyWindow(win);
            return 1;
        }
    }
    /* On an unrecoverable device-loss (acquire/present), flag a clean shutdown
     * rather than spin failed frames forever (W0.6 RHI robustness). */
    extern void openpete_request_shutdown(void);
    rhi_set_device_lost_handler(openpete_request_shutdown);
    /* Live-window supersampling (--ssaa N, default 1=no-op): the ss_rtt + resolve
     * pass are set up lazily (ensure_ss_rtt) once the window size is known. */
    s_nr.ssaa = (opts->ssaa > 0) ? opts->ssaa : 1;
    /* Vertex-precision snap (--vertex-precision integer): re-introduce the PSX
     * integer-grid vertex snap in add_tri_grouped. Default smooth (subpixel). */
    s_vertex_snap = (opts->vertex_precision == OPENPETE_VERTEX_PRECISION_INTEGER) ? 1 : 0;

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
    /* WORLD-group variants that also stencil-MARK their coverage (W2.6-c): the
     * WORLD-opaque group rasterizes normally AND writes stencil=1 wherever a
     * fragment passes the depth test, defining "pixels real world geometry
     * covered". The terrain LP hole-fill layer (WORLD_FILL, TEST_ZERO below)
     * then paints only the uncovered pixels — LP↔HP T-junction seams — and can
     * never poke through the HP. The SKY category (cyclorama dome) deliberately
     * does NOT use these: the sky showing through a seam is exactly what the
     * fill must repaint. */
    rhi_pipeline_desc gouraud_mark_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .stencil = RHI_STENCIL_MARK,
        .uses_vram = false, .push_const_size = 0,
    };
    /* Hole-fill pipe (WORLD_FILL): opaque untextured gouraud, depth-tested and
     * depth-written at TRUE depth, gated on stencil==0 (only pixels the WORLD
     * mark left uncovered). LP is untextured, so no textured variant needed. */
    rhi_pipeline_desc gouraud_fill_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .stencil = RHI_STENCIL_TEST_ZERO,
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
     * unchanged).
     * DEPTH-AWARE DECAL: shadow_decal.frag samples the WORLD-opaque pre-pass
     * depth (opaque_depth, set 0) and rejects only pixels whose terrain stands
     * ABOVE the shadow's ground plane by more than wall_height (a wall/ledge)
     * — replacing the retired ×0.70 view-Z pull, which punched through walls.
     * depth_test = false: the shader OWNS occlusion, so the flat fan never
     * self-culls against its own bumpy/slanted ground (the artifact the pull
     * used to mask). */
    rhi_pipeline_desc gouraud_sub_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "shadow_decal.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(shadow_decal_pc), /* set 0 = opaque_depth */
    };
    /* Fallback for a device whose depth format is not sampleable (the pre-pass
     * RT can't be created): the pre-decal behaviour — plain depth test + the
     * W2.5 hardware bias (correct against walls, may slice on steep ground). */
    rhi_pipeline_desc gouraud_sub_ztest_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = true, .depth_write_off = true,
        .depth_bias = true,
        .uses_vram = false, .push_const_size = 0,
    };
    /* The full-screen mode-2 fade (func_800190D4) is also ABR-2 subtractive but
     * is NOT a ground decal — it tints the whole frame. It used to share the
     * shadow pipe; now that the shadow pipe is the depth-aware decal (which
     * would sample opaque_depth and discard), the fade gets its own plain
     * subtractive pipe (quad.frag, depth-test off — the z=0 quad passed the
     * old LEQUAL test everywhere anyway). */
    rhi_pipeline_desc gouraud_sub_fade_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = false,
        .uses_vram = false, .push_const_size = 0,
    };
    /* WORLD-opaque depth PRE-PASS pipe: rasterize the opaque world geometry
     * into the depth-sampled RT that shadow_decal.frag reads. prepass.vert
     * stores TRUE PERSPECTIVE depth (z = 1 - 1/vz, screen-affine for planes)
     * so the decal's neighbour-texel NORMAL reconstruction is exact — the
     * canonical vz/(vz+4096) curve interpolates screen-linearly with a
     * distorted local gradient across large terrain tris (read flat floors as
     * fronto-parallel). Colour writes remain ON but are currently unused: the
     * seam-inpaint pass that sampled the pre-pass COLOUR attachment was removed
     * (the LP↔HP handoff fix made it redundant), and no other consumer reads it
     * — a depth-only pre-pass is a possible follow-up optimisation. */
    rhi_pipeline_desc prepass_desc = {
        .vert_spv = "prepass.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
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
    /* Opaque UNTEXTURED gouraud, depth-test ON but depth-WRITE OFF: the portal-sky
     * previews (portal_native.c, OPENPETE_SCENE_PORTAL_CYCLORAMA). They are a far
     * background seen through the arch window — the arch stone's depth (written by
     * the WORLD group, drawn first) must occlude them, but they must never write
     * depth, so the actors/player drawn afterward are never per-pixel cut by the
     * portal sky as Spyro flies into/through the portal. Same quad.vert/quad.frag
     * as the opaque pipe; only the depth-write differs. */
    rhi_pipeline_desc gouraud_noz_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        /* W2.6 (revised): portal-sky colours — depth-TEST ON (LEQUAL at the
         * portal-plane z, write OFF) + stencil TEST, drawn AFTER the ACTOR
         * group. The depth test is what lets the sky repaint over mobys
         * BEHIND the plane while losing to geometry/mobys in front of it;
         * the stencil mask confines it to the aperture footprint. (A fully
         * depth-free variant was tried and replaced — with a depth STAMP it
         * clipped Spyro mid-entry, and without one it needed the stamp.) */
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_write_off = true,
        .stencil = RHI_STENCIL_TEST,
        .uses_vram = false, .push_const_size = 0,
    };
    /* W2.6 — the portal aperture mask: rasterize the portal's sky footprint
     * invisibly (colour-write-off), depth-TESTED against the world written by
     * the WORLD group (depth-write off, so the mask pass can't perturb it),
     * marking stencil=1 where the sky is actually visible (arch stone / a
     * nearer wall punches the mask out via depth-fail). Carries the decal
     * depth-bias so the coincident inner-frame tie-break lives HERE, in the
     * invisible pass, not in the colours. */
    rhi_pipeline_desc portal_mask_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_write_off = true,
        .depth_bias = true, .stencil = RHI_STENCIL_MARK, .color_write_off = true,
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
    /* Textured WORLD-group stencil-MARK variant (W2.6-c) — most terrain is
     * textured, so the coverage mark must come from this pipe too. */
    rhi_pipeline_desc scene_tex_mark_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .stencil = RHI_STENCIL_MARK,
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
        .depth_bias = true, /* shadow decal: hardware coplanar tie-break (W2.5) */
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* DEPTH-AWARE moby-shadow decal (the same fix Spyro's silhouette shadow got,
     * textured): shadow_decal_tex.frag samples the shared WORLD-opaque pre-pass
     * depth (opaque_depth, set 0) AND the shadow-sprite VRAM (u_vram, set 1),
     * rejecting per pixel the wall / steep-face / raised-in-front terrain a flat
     * fan can't separate with a plain depth test. depth_test OFF: the shader OWNS
     * occlusion, so the flat fan never Z-fights the slanted/bumpy ground it drapes.
     * Preferred over scene_tex_sub_desc whenever the depth pre-pass is available;
     * NULL (unsupported device) falls back to it. */
    rhi_pipeline_desc scene_tex_sub_decal_desc = {
        .vert_spv = "shadow_decal_tex.vert.spv",
        .frag_spv = "shadow_decal_tex.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = false,
        .uses_vram = true, .uses_vram2 = true, /* set 0 = opaque_depth, set 1 = VRAM */
        .push_const_size = sizeof(shadow_decal_tex_pc),
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
    /* HUD panel pipeline: untextured alpha gouraud, depth-test OFF — the dim
     * semi-transparent dark panel behind the pause/inventory menu text (a 0x2A F4,
     * ABR mode 0 = (B+F)/2). Like gouraud_hud_pipe but alpha-blended; the vertex
     * alpha (128) gives (B+F)/2. Drawn at the start of the HUD layer so the text
     * reads on top. quad.frag emits the full vertex rgba incl. alpha. */
    rhi_pipeline_desc gouraud_hud_blend_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = false,
        .uses_vram = false, .push_const_size = 0,
    };
    /* HUD panel SUBTRACTIVE pipeline: untextured subtractive gouraud, depth-test OFF.
     * The in-game "shaded box" behind HUD text (func_8001860C) and the pause/inventory
     * menu panel (func_8001A40C) both emit their 0x2A POLY_F4 under
     * SetDrawMode(getTPage(0, 2, ..)) = ABR mode 2 (B-F), which DARKENS the framebuffer
     * by the box colour. (B+F)/2 alpha would instead wash it toward grey (a too-bright
     * box) — the bug this fixes. RHI_BLEND_SUBTRACT reverse-subtracts the vertex colour
     * (alpha ignored), exactly the PSX. The box's real ABR is read per-prim from its
     * DR_MODE in ui_sprite_native, so the HUD-panel draw picks alpha vs subtractive. */
    rhi_pipeline_desc gouraud_hud_sub_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = false,
        .uses_vram = false, .push_const_size = 0,
    };
    rhi_pipeline_desc scene_tex_hud_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* HD texture-replacement PoC (openpete-texture-replacement Phase 4 spike):
     * samples a host RGBA8 replacement (hd_tex.frag) with real per-texel alpha
     * instead of the PSX VRAM decode. Depth OFF (the PoC target is a HUD-layer
     * 2D quad); push constants carry the replaced texel box. */
    rhi_pipeline_desc hd_tex_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "hd_tex.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = false,
        .uses_vram = true, .push_const_size = 4 * sizeof(float),
    };
    /* Opaque-scene HD replacement: depth-tested cutout (true blending on
     * opaque groups would break Z ordering); the MARK twin keeps the W2.6-c
     * hole-fill stencil coverage intact when replacing WORLD-group runs. */
    rhi_pipeline_desc hd_cutout_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "hd_tex_cutout.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .uses_vram = true, .push_const_size = 4 * sizeof(float),
    };
    rhi_pipeline_desc hd_cutout_mark_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "hd_tex_cutout.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .stencil = RHI_STENCIL_MARK,
        .uses_vram = true, .push_const_size = 4 * sizeof(float),
    };
    /* HD twins of the transparent scene pipes (Phase 5): alpha (ABR 0) and
     * additive (ABR 1) — depth-tested, no depth write, mirroring their SD
     * counterparts. STP encoding: dumps bake STP texels at alpha 128, so
     * src-over on the alpha pipe reproduces (B+F)/2 while non-STP texels
     * stay opaque. The SUBTRACTIVE group keeps SD (its draws depend on the
     * caller's dynamic depth-bias state, which an HD rebind would
     * invalidate — and shadow-fan replacement is out of scope). */
    rhi_pipeline_desc hd_scene_blend_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "hd_tex.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .push_const_size = 4 * sizeof(float),
    };
    rhi_pipeline_desc hd_scene_add_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "hd_tex.frag.spv",
        .blend = RHI_BLEND_ADD, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .push_const_size = 4 * sizeof(float),
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
    /* Freeze-frame overlay pipeline (pause / inventory / flight-results backdrop):
     * samples frozen_tex (set 0) — the last world frame's native render — and
     * outputs it desaturated + green-tinted (freeze_overlay.frag; tint = vertex
     * colour). No depth (it's the menu backdrop), no blend (opaque, like the PSX
     * grayscale-freeze FT4 it replaces). */
    rhi_pipeline_desc freeze_overlay_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "freeze_overlay.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = 0,
    };
    /* SSAA resolve pipeline (--ssaa N): samples the supersampled scene RT (set 0)
     * and box-averages the N×N block in linear light into the 1× offscreen
     * (resolve.frag). No depth, no blend; the ssaa factor rides a push constant. */
    rhi_pipeline_desc resolve_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "resolve.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(int),
    };
    /* Occlusion-grid downsample (moby occlusion culling): reduces the WORLD-
     * opaque depth pre-pass to a coarse conservative farthest-depth grid the
     * CPU reads back to cull fully-hidden mobys before re-extraction. Optional
     * (graceful fallback: no grid → no culling). */
    rhi_pipeline_desc occl_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "occl_downsample.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = 4 * sizeof(int),
    };
    /* FPS-overlay supersample resolve: like resolve_desc but alpha-aware
     * (overlay_resolve.frag) and alpha-blended, to composite the downsampled
     * glyphs over the scene. Optional — draw_fps_overlay falls back to a direct
     * 1× draw if this (or ovl_quad_vb) didn't come up. */
    rhi_pipeline_desc ovl_resolve_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "overlay_resolve.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(int),
    };
    rhi_pipeline *blit_pipe    = rhi_create_pipeline(dev, &blit_desc);
    rhi_pipeline *vram_pipe    = rhi_create_pipeline(dev, &vram_desc);
    rhi_pipeline *gouraud_pipe = rhi_create_pipeline(dev, &gouraud_desc);
    rhi_pipeline *gouraud_blend_pipe = rhi_create_pipeline(dev, &gouraud_blend_desc);
    rhi_pipeline *gouraud_sub_pipe = rhi_create_pipeline(dev, &gouraud_sub_desc);
    rhi_pipeline *gouraud_sub_ztest_pipe = rhi_create_pipeline(dev, &gouraud_sub_ztest_desc);
    rhi_pipeline *gouraud_sub_fade_pipe = rhi_create_pipeline(dev, &gouraud_sub_fade_desc);
    rhi_pipeline *prepass_pipe = rhi_create_pipeline(dev, &prepass_desc);
    rhi_pipeline *gouraud_add_pipe = rhi_create_pipeline(dev, &gouraud_add_desc);
    rhi_pipeline *gouraud_noz_pipe = rhi_create_pipeline(dev, &gouraud_noz_desc);
    rhi_pipeline *portal_mask_pipe = rhi_create_pipeline(dev, &portal_mask_desc);
    rhi_pipeline *scene_tex_pipe = rhi_create_pipeline(dev, &scene_tex_desc);
    rhi_pipeline *scene_tex_blend_pipe = rhi_create_pipeline(dev, &scene_tex_blend_desc);
    rhi_pipeline *scene_tex_add_pipe = rhi_create_pipeline(dev, &scene_tex_add_desc);
    rhi_pipeline *scene_tex_sub_pipe = rhi_create_pipeline(dev, &scene_tex_sub_desc);
    rhi_pipeline *scene_tex_sub_decal_pipe = rhi_create_pipeline(dev, &scene_tex_sub_decal_desc); /* optional: NULL → fall back to scene_tex_sub_pipe */
    rhi_pipeline *gouraud_hud_pipe = rhi_create_pipeline(dev, &gouraud_hud_desc);
    rhi_pipeline *gouraud_hud_blend_pipe = rhi_create_pipeline(dev, &gouraud_hud_blend_desc);
    rhi_pipeline *gouraud_hud_sub_pipe = rhi_create_pipeline(dev, &gouraud_hud_sub_desc);
    rhi_pipeline *scene_tex_hud_pipe = rhi_create_pipeline(dev, &scene_tex_hud_desc);
    rhi_pipeline *hd_tex_pipe = rhi_create_pipeline(dev, &hd_tex_desc);   /* HD pipes: allowed to fail (NULL = feature off) */
    rhi_pipeline *hd_cutout_pipe = rhi_create_pipeline(dev, &hd_cutout_desc);
    rhi_pipeline *hd_cutout_mark_pipe = rhi_create_pipeline(dev, &hd_cutout_mark_desc);
    rhi_pipeline *hd_scene_blend_pipe = rhi_create_pipeline(dev, &hd_scene_blend_desc);
    rhi_pipeline *hd_scene_add_pipe   = rhi_create_pipeline(dev, &hd_scene_add_desc);
    rhi_pipeline *diff_pipe = rhi_create_pipeline(dev, &diff_desc);
    rhi_pipeline *freeze_overlay_pipe = rhi_create_pipeline(dev, &freeze_overlay_desc);
    rhi_pipeline *resolve_pipe = rhi_create_pipeline(dev, &resolve_desc);
    /* Per-present-rewritten buffers (scene/prepass/overlay) are frame-in-flight
     * RINGS (W2.2): their every-present rhi_update_buffer used to race the
     * frame two presents ago, previously masked only by the per-present VRAM
     * upload's vkQueueWaitIdle drain. panels/diff_quad/ovl_quad/fs_quad update
     * rarely (init / re-layout) under rhi_wait_idle and stay single. */
    rhi_buffer   *panels_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * NR_PANELS * 6);
    rhi_buffer   *scene_vb  = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * SCENE_VB_VERTS);
    rhi_buffer   *diff_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    rhi_buffer   *fs_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    rhi_buffer   *overlay_vb = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * OVL_MAX_VERTS);
    /* Mod draw-service overlay (optional — NULL just disables mod draws). */
    rhi_buffer   *mod_ovl_vb = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * MOD_OVL_MAX_VERTS);
    /* Optional overlay-supersample resources (graceful fallback if NULL). */
    rhi_pipeline *ovl_resolve_pipe = rhi_create_pipeline(dev, &ovl_resolve_desc);
    rhi_buffer   *ovl_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    rhi_buffer   *menu_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    /* Optional occlusion-grid resources (graceful fallback: NULL → no culling). */
    rhi_pipeline *occl_pipe = rhi_create_pipeline(dev, &occl_desc);
    /* Optional W2.6-c hole-fill pipes (graceful fallback: any NULL → WORLD uses
     * the plain pipes and the FILL layer is skipped — the pre-fill look). */
    rhi_pipeline *gouraud_mark_pipe = rhi_create_pipeline(dev, &gouraud_mark_desc);
    rhi_pipeline *scene_tex_mark_pipe = rhi_create_pipeline(dev, &scene_tex_mark_desc);
    rhi_pipeline *gouraud_fill_pipe = rhi_create_pipeline(dev, &gouraud_fill_desc);
    if (!gouraud_mark_pipe || !scene_tex_mark_pipe || !gouraud_fill_pipe) {
        if (gouraud_mark_pipe)   { rhi_destroy_pipeline(dev, gouraud_mark_pipe);   gouraud_mark_pipe = NULL; }
        if (scene_tex_mark_pipe) { rhi_destroy_pipeline(dev, scene_tex_mark_pipe); scene_tex_mark_pipe = NULL; }
        if (gouraud_fill_pipe)   { rhi_destroy_pipeline(dev, gouraud_fill_pipe);   gouraud_fill_pipe = NULL; }
        OPL_ERROR_F("native", "hole-fill pipelines unavailable; terrain LP hole-fill disabled");
    }
    rhi_texture  *occl_grid = rhi_create_render_target(dev, OCCL_GW, OCCL_GH);
    rhi_buffer   *prepass_vb = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * SCENE_VB_VERTS);
    rhi_texture  *vram_tex  = rhi_create_vram(dev, NR_VRAM_W, NR_VRAM_H);
    /* W2.4: the CPU mirror IS the owned native VRAM model (static storage in
     * native_vram.c) — written by the native ops at op time and shadow-synced
     * from PsyCross's mirror per dirty rect at present time. */
    uint16_t     *mirror    = openpete_vram_data();
    if (!blit_pipe || !vram_pipe || !gouraud_pipe || !gouraud_blend_pipe || !gouraud_sub_pipe || !gouraud_sub_ztest_pipe || !gouraud_sub_fade_pipe || !prepass_pipe || !gouraud_add_pipe || !scene_tex_pipe || !scene_tex_blend_pipe || !scene_tex_add_pipe || !scene_tex_sub_pipe || !gouraud_hud_pipe || !gouraud_hud_blend_pipe || !gouraud_hud_sub_pipe || !scene_tex_hud_pipe || !panels_vb || !scene_vb || !vram_tex || !mirror ||
        !diff_pipe || !diff_quad_vb || !freeze_overlay_pipe || !resolve_pipe || !fs_quad_vb || !overlay_vb || !prepass_vb) {
        OPL_ERROR_F("native", "RHI setup failed; continuing without native renderer");
        if (prepass_vb)    rhi_destroy_buffer(dev, prepass_vb);
        if (prepass_pipe)  rhi_destroy_pipeline(dev, prepass_pipe);
        if (gouraud_sub_fade_pipe) rhi_destroy_pipeline(dev, gouraud_sub_fade_pipe);
        if (gouraud_sub_ztest_pipe) rhi_destroy_pipeline(dev, gouraud_sub_ztest_pipe);
        if (resolve_pipe) rhi_destroy_pipeline(dev, resolve_pipe);
        if (gouraud_hud_sub_pipe) rhi_destroy_pipeline(dev, gouraud_hud_sub_pipe);
        if (gouraud_hud_blend_pipe) rhi_destroy_pipeline(dev, gouraud_hud_blend_pipe);
        if (freeze_overlay_pipe) rhi_destroy_pipeline(dev, freeze_overlay_pipe);
        if (overlay_vb)    rhi_destroy_buffer(dev, overlay_vb);
        if (fs_quad_vb)    rhi_destroy_buffer(dev, fs_quad_vb);
        if (vram_tex)      rhi_destroy_texture(dev, vram_tex);
        if (diff_quad_vb)  rhi_destroy_buffer(dev, diff_quad_vb);
        if (scene_vb)      rhi_destroy_buffer(dev, scene_vb);
        if (panels_vb)     rhi_destroy_buffer(dev, panels_vb);
        if (diff_pipe)     rhi_destroy_pipeline(dev, diff_pipe);
        if (scene_tex_hud_pipe) rhi_destroy_pipeline(dev, scene_tex_hud_pipe);
        if (hd_tex_pipe) rhi_destroy_pipeline(dev, hd_tex_pipe);
        if (hd_cutout_pipe) rhi_destroy_pipeline(dev, hd_cutout_pipe);
        if (hd_cutout_mark_pipe) rhi_destroy_pipeline(dev, hd_cutout_mark_pipe);
        if (hd_scene_blend_pipe) rhi_destroy_pipeline(dev, hd_scene_blend_pipe);
        if (hd_scene_add_pipe) rhi_destroy_pipeline(dev, hd_scene_add_pipe);
        if (gouraud_hud_pipe)   rhi_destroy_pipeline(dev, gouraud_hud_pipe);
        if (scene_tex_sub_decal_pipe) rhi_destroy_pipeline(dev, scene_tex_sub_decal_pipe);
        if (scene_tex_sub_pipe) rhi_destroy_pipeline(dev, scene_tex_sub_pipe);
        if (scene_tex_add_pipe) rhi_destroy_pipeline(dev, scene_tex_add_pipe);
        if (scene_tex_blend_pipe) rhi_destroy_pipeline(dev, scene_tex_blend_pipe);
        if (scene_tex_pipe) rhi_destroy_pipeline(dev, scene_tex_pipe);
        if (gouraud_noz_pipe) rhi_destroy_pipeline(dev, gouraud_noz_pipe);
        if (portal_mask_pipe) rhi_destroy_pipeline(dev, portal_mask_pipe);
        if (gouraud_mark_pipe)   rhi_destroy_pipeline(dev, gouraud_mark_pipe);
        if (scene_tex_mark_pipe) rhi_destroy_pipeline(dev, scene_tex_mark_pipe);
        if (gouraud_fill_pipe)   rhi_destroy_pipeline(dev, gouraud_fill_pipe);
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
    s_nr.gouraud_sub_ztest_pipe = gouraud_sub_ztest_pipe;
    s_nr.gouraud_sub_fade_pipe = gouraud_sub_fade_pipe;
    s_nr.prepass_pipe = prepass_pipe;
    s_nr.prepass_vb   = prepass_vb;
    s_nr.gouraud_add_pipe = gouraud_add_pipe;
    s_nr.gouraud_noz_pipe = gouraud_noz_pipe;
    s_nr.portal_mask_pipe = portal_mask_pipe;
    s_nr.gouraud_mark_pipe = gouraud_mark_pipe;     /* may be NULL → no hole-fill */
    s_nr.scene_tex_mark_pipe = scene_tex_mark_pipe; /* may be NULL → no hole-fill */
    s_nr.gouraud_fill_pipe = gouraud_fill_pipe;     /* may be NULL → no hole-fill */
    s_nr.scene_tex_pipe = scene_tex_pipe;
    s_nr.scene_tex_blend_pipe = scene_tex_blend_pipe;
    s_nr.scene_tex_add_pipe = scene_tex_add_pipe;
    s_nr.scene_tex_sub_pipe = scene_tex_sub_pipe;
    s_nr.scene_tex_sub_decal_pipe = scene_tex_sub_decal_pipe; /* may be NULL → plain sub pipe */
    s_nr.gouraud_hud_pipe = gouraud_hud_pipe;
    s_nr.gouraud_hud_blend_pipe = gouraud_hud_blend_pipe;
    s_nr.gouraud_hud_sub_pipe = gouraud_hud_sub_pipe;
    s_nr.scene_tex_hud_pipe = scene_tex_hud_pipe;
    s_nr.hd_tex_pipe  = hd_tex_pipe;
    s_nr.hd_cutout_pipe = hd_cutout_pipe;
    s_nr.hd_cutout_mark_pipe = hd_cutout_mark_pipe;
    s_nr.hd_scene_blend_pipe = hd_scene_blend_pipe;
    s_nr.hd_scene_add_pipe = hd_scene_add_pipe;
    s_nr.panels_vb    = panels_vb;
    s_nr.scene_vb     = scene_vb;
    s_nr.diff_pipe    = diff_pipe;
    s_nr.diff_quad_vb = diff_quad_vb;
    s_nr.freeze_overlay_pipe = freeze_overlay_pipe;
    s_nr.resolve_pipe = resolve_pipe;
    s_nr.fs_quad_vb   = fs_quad_vb;
    s_nr.overlay_vb   = overlay_vb;
    s_nr.mod_ovl_vb   = mod_ovl_vb;
    /* Optional menu overlay buffer (init has already passed the fatal guard —
     * a NULL here just disables the menu draw, it doesn't fail the renderer). */
    s_nr.menu_vb      = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * MENU_MAX_VERTS);
    s_nr.menu_glyph_vb= rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * MENU_GLYPH_MAX);
    s_nr.ovl_resolve_pipe = ovl_resolve_pipe;   /* may be NULL → 1× fallback */
    s_nr.ovl_quad_vb  = ovl_quad_vb;            /* may be NULL → 1× fallback */
    s_nr.menu_quad_vb = menu_quad_vb;           /* may be NULL → 1× fallback */
    s_nr.occl_pipe    = occl_pipe;              /* may be NULL → no moby occlusion culling */
    s_nr.occl_grid    = occl_grid;              /* may be NULL → no moby occlusion culling */
    s_nr.vram_tex     = vram_tex;
    s_nr.vram_mirror  = mirror;
    /* W2.4: subscribe to PsyCross VRAM writes (dirty-rect tracked mirror) and
     * install the native op implementations (1b — verified via
     * OPENPETE_VRAM_VERIFY against PsyCross's mirror). */
    PsyX_SetVRAMDirtyCallback(nr_vram_dirty_cb);
    openpete_vram_install_ops();

    /* Static full-target quad (clip -1..1, UV 0..1 top-down) for render-target
     * copies — currently the native_rtt -> frozen_tex freeze-snapshot blit. */
    {
        rhi_vertex fsq[6];
        build_panel(fsq, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
        rhi_update_buffer(fs_quad_vb, fsq, sizeof(fsq));
    }
    OPL_INFO_F("native", "Vulkan %s window up (%dx%d, device='%s'); PsyCross hidden",
               ab_mode ? "A/B" : diff_mode ? "diff" : "vk",
               w, h, rhi_device_name(dev));
    return 0;
}

bool openpete_native_render_enabled(void)
{
    return s_nr.enabled;
}

/* Toggle the native window between windowed and borderless-desktop fullscreen.
 * Bound to Alt+Enter and double-click in run.c (moved out of PsyCross, which
 * only ever drove its now-hidden GL window). SDL3 decides the fullscreen *kind*
 * by the window's fullscreen mode: NULL = borderless desktop (no video-mode
 * switch), which is what we want — so force it NULL before entering. The present
 * path re-reads the window size every frame (SDL_GetWindowSizeInPixels) and the
 * RHI recreates the swapchain on resize, so layout + swapchain follow with no
 * extra work here. No-op until the native window is up. */
void openpete_native_toggle_fullscreen(void)
{
    if (!s_nr.enabled || !s_nr.window) return;
    bool fs = (SDL_GetWindowFlags(s_nr.window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!fs)
        SDL_SetWindowFullscreenMode(s_nr.window, NULL);  /* borderless desktop */
    SDL_SetWindowFullscreen(s_nr.window, !fs);
    OPL_INFO_F("native", "fullscreen %s", !fs ? "on (borderless desktop)" : "off");
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

/* ---- W2.4 native VRAM model: dirty-rect tracked mirror -------------------
 * PsyCross notifies every write to its CPU VRAM mirror (LoadImage/MoveImage/
 * ClearImage, DR_LOAD/DR_MOVE, framebuffer copyback, savestate restore) via
 * PsyX_SetVRAMDirtyCallback with the effective post-clamp rect. We accumulate
 * the rects, refresh ONLY those regions of our own mirror at present time,
 * and upload only those regions (rhi_update_vram_rects) — replacing the old
 * full 1 MiB GR_ReadVRAM copy + full-texture upload every present. The list
 * overflows to a full-frame refresh (correct, just slower). The callback runs
 * on the game thread synchronously with the write; the loop is
 * single-threaded (game fiber + present interleave), so no locking. */
#define NR_VRAM_MAX_RECTS 64
static rhi_rect s_vram_rects[NR_VRAM_MAX_RECTS];
static int      s_vram_nrects  = 0;
static bool     s_vram_full    = true;    /* first sync = full snapshot */

static void nr_vram_dirty_cb(int x, int y, int w, int h)
{
    /* Defensive clamp (GR_ClearVRAM can pass unmasked coords). */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > NR_VRAM_W) w = NR_VRAM_W - x;
    if (y + h > NR_VRAM_H) h = NR_VRAM_H - y;
    if (w <= 0 || h <= 0) return;
    if (s_vram_full) return;
    if (s_vram_nrects >= NR_VRAM_MAX_RECTS) { s_vram_full = true; return; }
    s_vram_rects[s_vram_nrects++] = (rhi_rect){ x, y, w, h };
}

/* Pending upload set (drained by upload_synced_textures inside the frame). */
static rhi_rect s_vram_up_rects[NR_VRAM_MAX_RECTS];
static int      s_vram_up_n    = 0;
static bool     s_vram_up_full = false;
static bool     s_ref_dirty    = false;  /* readback refreshed; upload pending */

static void sync_vram_texture(void)
{
    /* OPENPETE_VRAM_CENSUS: per-300-present dirty-traffic summary. */
    static int s_census = -1;
    if (s_census < 0) { const char* e = getenv("OPENPETE_VRAM_CENSUS"); s_census = (e && *e && *e != '0'); }
    if (s_census) {
        static long presents, rects, full, bytes; static int logged;
        presents++;
        if (s_vram_full) full++;
        rects += s_vram_nrects;
        for (int i = 0; i < s_vram_nrects; i++)
            bytes += (long)s_vram_rects[i].w * s_vram_rects[i].h * 2;
        if (presents % 300 == 0 && logged < 40) {
            logged++;
            fprintf(stderr, "[vram-census] presents=%ld rects=%.2f/present bytes=%.1fKB/present fullSyncs=%ld\n",
                    presents, (double)rects / presents, (double)bytes / presents / 1024.0, full);
        }
    }
    /* OPENPETE_VRAM_VERIFY (W2.4-1b): before the shadow copy overwrites the
     * model with PsyCross's bytes, memcmp every op-written rect (exact-rect
     * match against the ops' post-clamp log) — proves the native op
     * implementations produce PsyCross's bytes. Shadow-only rects (framebuffer
     * copyback, savestate restore) are counted but not compared. */
    static int s_verify = -1;
    if (s_verify < 0) { const char* e = getenv("OPENPETE_VRAM_VERIFY"); s_verify = (e && *e && *e != '0'); }
    if (s_verify) {
        static openpete_vram_op_rect ops[NR_VRAM_MAX_RECTS];
        int ovf = 0;
        int nops = openpete_vram_drain_op_rects(ops, NR_VRAM_MAX_RECTS, &ovf);
        static long v_presents, v_dirty, v_ops, v_match, v_mismatch, v_shadow, v_oponly, v_unverified;
        static int  v_logged, v_verbose;
        v_presents++;
        v_ops += nops;
        if (s_vram_full || ovf) {
            v_unverified++;               /* full-sync window / op-log overflow */
        } else {
            for (int i = 0; i < s_vram_nrects; i++) {
                const rhi_rect r = s_vram_rects[i];
                int oi = -1;
                for (int k = 0; k < nops; k++)
                    if (ops[k].w > 0 && ops[k].x == r.x && ops[k].y == r.y &&
                        ops[k].w == r.w && ops[k].h == r.h) { oi = k; break; }
                v_dirty++;
                if (oi < 0) { v_shadow++; continue; }
                ops[oi].w = -ops[oi].w;   /* consume */
                v_match++;
                static uint16_t vrow[NR_VRAM_W];
                for (int row = 0; row < r.h; row++) {
                    GR_ReadVRAM(vrow, r.x, r.y + row, r.w, 1);
                    if (memcmp(vrow, s_nr.vram_mirror + (size_t)(r.y + row) * NR_VRAM_W + r.x,
                               (size_t)r.w * 2) != 0) {
                        v_mismatch++;
                        if (v_verbose < 8) {
                            v_verbose++;
                            fprintf(stderr, "[vram-verify] MISMATCH rect(%d,%d %dx%d) row %d\n",
                                    r.x, r.y, r.w, r.h, row);
                        }
                        break;
                    }
                }
            }
            for (int k = 0; k < nops; k++) if (ops[k].w > 0) v_oponly++;
        }
        if (v_presents % 300 == 0 && v_logged < 40) {
            v_logged++;
            fprintf(stderr, "[vram-verify] presents=%ld dirty=%ld ops=%ld matched=%ld MISMATCH=%ld shadowOnly=%ld opOnly=%ld unverifiedWindows=%ld\n",
                    v_presents, v_dirty, v_ops, v_match, v_mismatch, v_shadow, v_oponly, v_unverified);
        }
    }
    /* OPENPETE_VRAM_PEEK="x,y,w,h,frame": one-shot dump of PsyCross VRAM vs
     * the native mirror for a rect (fog-arc palette forensics). */
    { static int s_peek = -1; static int px_, py_, pw_, ph_, pf_;
      if (s_peek < 0) { const char* e = getenv("OPENPETE_VRAM_PEEK");
          s_peek = 0; pf_ = 0;
          if (e && sscanf(e, "%d,%d,%d,%d,%d", &px_, &py_, &pw_, &ph_, &pf_) >= 4) s_peek = 1; }
      if (s_peek == 1 && s_scene_frame != 0xFFFFFFFFu && (int)s_scene_frame >= pf_) {
          s_peek = 2;
          static uint16_t vrow[NR_VRAM_W];
          if (pw_ > 32) pw_ = 32;
          for (int row = 0; row < ph_; row++) {
              GR_ReadVRAM(vrow, px_, py_ + row, pw_, 1);
              fprintf(stderr, "[vram-peek] psx  y=%3d:", py_ + row);
              for (int i2 = 0; i2 < pw_; i2++) fprintf(stderr, " %04X", vrow[i2]);
              fprintf(stderr, "\n[vram-peek] mirr y=%3d:", py_ + row);
              for (int i2 = 0; i2 < pw_; i2++)
                  fprintf(stderr, " %04X",
                          s_nr.vram_mirror[(size_t)(py_ + row) * NR_VRAM_W + px_ + i2]);
              fprintf(stderr, "\n");
          }
      } }
    /* Refresh the owned mirror from PsyCross's, dirty regions only, and queue
     * the same regions for in-frame upload (W2.2 staging-ring path). */
    if (s_vram_full) {
        GR_ReadVRAM(s_nr.vram_mirror, 0, 0, NR_VRAM_W, NR_VRAM_H);
        s_vram_up_full = true;
        s_vram_up_n = 0;
        s_vram_full = false;
        s_vram_nrects = 0;
        return;
    }
    for (int i = 0; i < s_vram_nrects; i++) {
        const rhi_rect r = s_vram_rects[i];
        for (int row = 0; row < r.h; row++)
            GR_ReadVRAM(s_nr.vram_mirror + (size_t)(r.y + row) * NR_VRAM_W + r.x,
                        r.x, r.y + row, r.w, 1);
        if (!s_vram_up_full) {
            if (s_vram_up_n >= NR_VRAM_MAX_RECTS) s_vram_up_full = true;
            else s_vram_up_rects[s_vram_up_n++] = r;
        }
    }
    s_vram_nrects = 0;
}

/* Record the pending VRAM/reference uploads into the CURRENT frame's command
 * buffer. Call right after a successful rhi_begin_frame, BEFORE the frame's
 * first render pass (the rhi appends transfer commands + barriers to the
 * frame cb — see rhi_update_vram's contract in rhi.h). */
static void upload_synced_textures(void)
{
    if (s_vram_up_full) {
        rhi_update_vram(s_nr.dev, s_nr.vram_tex, s_nr.vram_mirror);
        s_vram_up_full = false;
        s_vram_up_n = 0;
    } else if (s_vram_up_n > 0) {
        rhi_update_vram_rects(s_nr.dev, s_nr.vram_tex, s_nr.vram_mirror,
                              s_vram_up_rects, s_vram_up_n);
        s_vram_up_n = 0;
    }
    if (s_ref_dirty) {
        rhi_update_rgba8(s_nr.dev, s_nr.ref_tex, s_nr.readback);
        s_ref_dirty = false;
    }
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

    s_ref_dirty = true;   /* upload deferred to upload_synced_textures() */
    return true;
}

/* (Re)create native_rtt (the canonical native frame each present) AND frozen_tex
 * (the freeze-backdrop snapshot) at the reference resolution. native_rtt is the
 * frame the diff pass compares and the live vk/ab panel blits from; frozen_tex
 * holds the last world frame for the pause/inventory/flight green overlay. No-op
 * until the reference size is known; recreates both on size change. Must run
 * OUTSIDE a frame (creates GPU resources / waits idle). `want` gates allocation:
 * always true once the native path is live (the canonical-frame model needs the
 * RTTs in every mode); the screenshot-all path also passes true. */
static void ensure_native_rtt(bool want)
{
    if (!want || s_nr.ref_w <= 0 || s_nr.ref_h <= 0) return;
    if (s_nr.native_rtt && s_nr.rtt_w == s_nr.ref_w && s_nr.rtt_h == s_nr.ref_h) return;
    rhi_wait_idle(s_nr.dev);
    if (s_nr.native_rtt) { rhi_destroy_texture(s_nr.dev, s_nr.native_rtt); s_nr.native_rtt = NULL; }
    if (s_nr.frozen_tex) { rhi_destroy_texture(s_nr.dev, s_nr.frozen_tex); s_nr.frozen_tex = NULL; }
    s_nr.frozen_valid = false;
    s_nr.native_rtt = rhi_create_render_target(s_nr.dev, (uint32_t)s_nr.ref_w, (uint32_t)s_nr.ref_h);
    s_nr.frozen_tex = rhi_create_render_target(s_nr.dev, (uint32_t)s_nr.ref_w, (uint32_t)s_nr.ref_h);
    s_nr.rtt_w = s_nr.ref_w; s_nr.rtt_h = s_nr.ref_h;
    s_nr.frozen_w = s_nr.ref_w; s_nr.frozen_h = s_nr.ref_h;
    if (s_nr.native_rtt)
        OPL_INFO_F("native", "native render target %dx%d (+ freeze snapshot)", s_nr.ref_w, s_nr.ref_h);
}

/* (Re)create ss_rtt — the SSAA supersampled scene target — at the EFFECTIVE
 * factor × the window. The requested --ssaa factor is capped to the GPU's max
 * 2D image dimension and stepped down on an allocation failure (to 1 = SSAA
 * off). No-op when --ssaa is 1, the window size is unknown, or it already
 * matches. Runs OUTSIDE a frame (creates GPU resources / waits idle). */
static void ensure_ss_rtt(void)
{
    int req = (s_nr.ssaa > 1) ? s_nr.ssaa : 1;
    int win_w = s_nr.win_w, win_h = s_nr.win_h;
    if (req <= 1 || win_w <= 0 || win_h <= 0) {              /* SSAA off → drop any RT */
        if (s_nr.ss_rtt) {
            rhi_wait_idle(s_nr.dev);
            rhi_destroy_texture(s_nr.dev, s_nr.ss_rtt); s_nr.ss_rtt = NULL;
            s_nr.ss_w = s_nr.ss_h = 0; s_nr.ss_factor = 1;
        }
        return;
    }
    uint32_t maxdim = rhi_max_image_dimension(s_nr.dev);
    int eff = req;
    while (eff > 1 && ((uint32_t)(win_w * eff) > maxdim || (uint32_t)(win_h * eff) > maxdim)) eff--;
    int want_w = win_w * eff, want_h = win_h * eff;
    if (s_nr.ss_rtt && s_nr.ss_w == want_w && s_nr.ss_h == want_h) return;  /* unchanged */
    rhi_wait_idle(s_nr.dev);
    if (s_nr.ss_rtt) { rhi_destroy_texture(s_nr.dev, s_nr.ss_rtt); s_nr.ss_rtt = NULL; }
    while (eff > 1) {  /* allocation fallback: step down until it fits VRAM */
        s_nr.ss_rtt = rhi_create_render_target(s_nr.dev, (uint32_t)(win_w * eff), (uint32_t)(win_h * eff));
        if (s_nr.ss_rtt) break;
        eff--;
    }
    if (s_nr.ss_rtt) {
        rhi_texture_extent(s_nr.ss_rtt, &s_nr.ss_w, &s_nr.ss_h);
        s_nr.ss_factor = eff;
        OPL_INFO_F("native", "SSAA %dx%s -> scene RT %dx%d (window %dx%d, GPU max dim %u)",
                   req, (eff != req) ? " (capped)" : "", s_nr.ss_w, s_nr.ss_h, win_w, win_h, maxdim);
    } else {
        s_nr.ss_w = s_nr.ss_h = 0; s_nr.ss_factor = 1;   /* couldn't allocate → SSAA off */
    }
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
        int fit_w = have_clip ? s_nr.ref_w  : NR_VRAM_W;  /* 4:3 display shape */
        int fit_h = have_clip ? s_nr.ref_h  : NR_VRAM_H;

        /* Hor+ widen factor from --aspect (FIXED per run): how much wider the
         * --aspect view is than 4:3. vk-only; ab/diff stay 4:3 vs the reference.
         * Clamp ≥1 — Hor+ only ADDS world; a narrower --aspect just renders 4:3
         * and the window letterboxes. */
        float wf = 1.0f;
        if (!s_scene_ref_compare && s_nr.aspect_wh > 0.0f) {
            wf = s_nr.aspect_wh / (4.0f / 3.0f);
            if (wf < 1.0f) wf = 1.0f;
        }
        s_nr.ws_factor = wf;

        /* Fit the FULL --aspect display rect (4:3 content widened by wf) into the
         * window → letterbox/pillarbox bars where the window ≠ --aspect. The 4:3
         * CONTENT sub-rect (what scene_to_clip maps the canonical 512-wide screen
         * onto, unstretched) is 1/wf of that width and centred; the Hor+ reveal
         * fills the rest of the --aspect rect. (Replaces the old window-driven
         * auto-Hor+ that filled the whole window — now --aspect controls the render
         * shape and the window only adds bars, matching PsyCross.) */
        float cx = -1.0f + chx, hx, hy;
        fit_cell(cx, 0.0f, chx, 1.0f, (int)((float)fit_w * wf + 0.5f), fit_h,
                 s_nr.win_w, s_nr.win_h, &hx, &hy);
        s_nr.p0_cx = cx; s_nr.p0_hx = hx / wf; s_nr.p0_hy = hy; /* 4:3 sub-rect for the scene map */
        /* VRAM fallback panel (loading screens / OT-empty): the canonical 4:3
         * framebuffer, shown at the 4:3 sub-rect (unstretched, pillarboxed within
         * the --aspect area). */
        build_panel(&verts[0], cx, 0.0f, s_nr.p0_hx, hy, 0.0f, 0.0f, (float)uv_w, (float)uv_h);
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
        /* The render target IS the frame: map to the full target, clip -1..1.
         * Divide x by s_render_freeze_wf so a widescreen freeze capture (wf>1)
         * compresses the 4:3 content to the centre and the Hor+ sides fill ±1
         * (wf==1 → the plain 4:3 full-extent for native_rtt / diff). */
        *cx = (-1.0f + nx * 2.0f) / s_render_freeze_wf;
        *cy =  1.0f - ny * 2.0f;        /* screen y down -> clip y up */
    } else {
        *cx = (s_nr.p0_cx - s_nr.p0_hx) + nx * (2.0f * s_nr.p0_hx);
        *cy = (0.0f + s_nr.p0_hy)       - ny * (2.0f * s_nr.p0_hy);
    }
}

/* ---- depth-aware shadow decal: the shared WORLD-opaque depth pre-pass ------ */

/* Camera reconstruction params from the game (openpete_native_render.h); the
 * shadow feeder refreshes them every extract (incl. the sub-tick re-extract, so
 * they track the interpolated camera). Not snapshotted into scene history: a
 * 1-present staleness moves the reconstruction by the camera's per-present
 * delta (a few world units), far under the wall_height threshold. */
static openpete_shadow_decal_params s_shadow_decal;
void openpete_native_set_shadow_decal_params(const openpete_shadow_decal_params *p)
{
    if (p) s_shadow_decal = *p; else s_shadow_decal.valid = 0;
}

/* Per-present pre-pass state consumed by draw_native_scene's SUBTRACT group:
 * whether the pre-pass rendered this present, the raster extent inside the
 * (grow-only) opaque_depth allocation, and the PSX-screen→uv affine of the
 * full-extent mapping the pre-pass rasterized with. */
static int   s_od_ready = 0;
static float s_od_vw = 0.0f, s_od_vh = 0.0f;
static float s_od_uax = 0.0f, s_od_ubx = 0.0f, s_od_vay = 0.0f, s_od_vby = 0.0f;

/* Fallback view-Z band for shadow_decal.frag's mode-0 path (params invalid);
 * the height test does not use it. Env-tunable for calibration. */
static float shadow_decal_band(void)
{
    static float b = -1.0f;
    if (b < 0.0f) { const char* e = getenv("OPENPETE_SHADOW_BAND"); b = (e && *e) ? (float)atof(e) : 256.0f; }
    return b;
}

/* Draw moby drop-shadows through the DEPTH-AWARE textured decal (default ON)?
 * OPENPETE_MOBY_SHADOW_DECAL=0 reverts to the plain depth-tested subtract pipe +
 * hardware bias — the pre-fix behaviour — for A/B of the decal against the old
 * flat-fan-clips-slopes path. Display-only. */
static int moby_shadow_decal_enabled(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_SHADOW_DECAL"); v = !(e && *e && *e == '0'); }
    return v;
}

/* Moby occlusion culling on? (OPENPETE_MOBY_OCCLUSION=0 disables; default ON.)
 * Display-only: culling only skips NATIVE re-extraction/drawing of mobys whose
 * screen rect is provably behind nearer WORLD-opaque depth — the canonical
 * cull pass (and every gameplay-readable byte it writes) is untouched. */
static int occl_cull_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_MOBY_OCCLUSION"); on = !(e && *e == '0'); }
    return on;
}
/* Culling usable this run (enabled + the optional grid resources came up). */
static int occl_active(void)
{
    return s_nr.occl_pipe && s_nr.occl_grid && occl_cull_enabled();
}

/* Does the present scene carry a Spyro silhouette shadow? (The untextured
 * SUBTRACT group's membership test, mirrored: blend 3, not HUD-routed.) Gates
 * both the opaque_depth allocation and the per-present pre-pass, so menus /
 * cutscenes / shadowless scenes pay nothing. */
static int scene_has_spyro_shadow(const scene_tri_t* scene, int scene_n)
{
    for (int t = 0; t < scene_n; t++) {
        int c = scene[t].category;
        if (!scene[t].textured && scene[t].blend == 3 &&
            c != OPENPETE_SCENE_HUD && c != OPENPETE_SCENE_HUD3D &&
            c != OPENPETE_SCENE_HUD_PANEL)
            return 1;
    }
    return 0;
}

/* (Re)create opaque_depth sized to max(reference, window) — the presented pixel
 * grid bounds the useful discard-edge precision, so the SSAA extent is
 * intentionally NOT covered (consumers sample by uv; a coarser pre-pass texel
 * only quantizes the wall edge, bounded by one WINDOW pixel). Grows only.
 * Creates GPU resources / waits idle → runs OUTSIDE a frame, beside
 * ensure_ss_rtt. No-op until a scene actually carries a Spyro shadow. */
static void ensure_opaque_depth(void)
{
    static int s_failed = 0;
    if (s_failed) return;               /* device can't sample depth — fallback pipe */
    int scene_n; uint32_t bg_, fade_, freeze_; int hud_;
    const scene_tri_t* scene = scene_present_buf(&scene_n, &bg_, &fade_, &freeze_, &hud_, NULL);
    if (scene_n <= 0) return;
    /* The pre-pass serves two consumers: the shadow decal and the moby
     * occlusion grid. Allocate when either wants it. */
    if (!scene_has_spyro_shadow(scene, scene_n) && !occl_active()) return;
    int want_w = s_nr.ref_w, want_h = s_nr.ref_h;
    if (s_nr.win_w > want_w) want_w = s_nr.win_w;
    if (s_nr.win_h > want_h) want_h = s_nr.win_h;
    if (want_w <= 0 || want_h <= 0) return;
    if (s_nr.opaque_depth && s_nr.od_w >= want_w && s_nr.od_h >= want_h) return;
    rhi_wait_idle(s_nr.dev);
    if (s_nr.opaque_depth) { rhi_destroy_texture(s_nr.dev, s_nr.opaque_depth); s_nr.opaque_depth = NULL; }
    s_nr.opaque_depth = rhi_create_render_target_depth_sampled(s_nr.dev, (uint32_t)want_w, (uint32_t)want_h);
    if (s_nr.opaque_depth) {
        rhi_texture_extent(s_nr.opaque_depth, &s_nr.od_w, &s_nr.od_h);
        OPL_INFO_F("native", "opaque-depth pre-pass target %dx%d (shadow decal occlusion)",
                   s_nr.od_w, s_nr.od_h);
    } else {
        s_nr.od_w = s_nr.od_h = 0;
        s_failed = 1;
        OPL_WARN_F("native", "depth-sampled RT unavailable; Spyro shadow uses the depth-tested fallback");
    }
}

/* The shared WORLD-opaque depth PRE-PASS. Rasterizes the present scene's opaque
 * WORLD geometry (terrain / walls / props — the shadow's occluder set; actors
 * are excluded, layering handles them) depth-only into opaque_depth, ONCE per
 * present, in the FULL-EXTENT PSX mapping. Every consumer pass then samples it
 * by PSX screen coord through the recorded affine, so one raster serves
 * native_rtt, SSAA, the offscreen panel, and screenshots alike. Must record
 * before the first consumer pass (own render pass; call right after
 * rhi_begin_frame). Display-only. */
static void native_depth_prepass(void)
{
    s_od_ready = 0;
    if (!s_nr.opaque_depth || !s_nr.prepass_vb || !s_nr.prepass_pipe) return;
    int scene_n; uint32_t bg_, fade_, freeze_; int hud_;
    const scene_tri_t* scene = scene_present_buf(&scene_n, &bg_, &fade_, &freeze_, &hud_, NULL);
    if (scene_n <= 0) return;
    if (freeze_ & (1u << 31)) return;   /* freeze backdrop: gameplay groups skipped, no shadow drawn */
    if (!scene_has_spyro_shadow(scene, scene_n) && !occl_active()) return;

    /* Raster extent = the ensure_opaque_depth sizing, clamped to the allocation. */
    int vw = s_nr.ref_w, vh = s_nr.ref_h;
    if (s_nr.win_w > vw) vw = s_nr.win_w;
    if (s_nr.win_h > vh) vh = s_nr.win_h;
    if (vw > s_nr.od_w) vw = s_nr.od_w;
    if (vh > s_nr.od_h) vh = s_nr.od_h;
    if (vw <= 0 || vh <= 0) return;

    /* Extract the opaque WORLD occluders' clip geometry with the FULL-EXTENT
     * mapping (positions only — the pipe writes depth, never colour).
     * WIDENED full extent under Hor+ (--aspect): the freeze_wf divide maps the
     * whole widened PSX x-band [256-256F, 256+256F] across the target, so the
     * pre-pass covers every column the widescreen view actually shows. Without
     * this the recorded affine spanned only the 4:3 [0,512] rect and the
     * occl-query offscreen reject culled edge mobys that ARE visible — on
     * sub-tick presents only (canonical never occl-culls) = the widescreen
     * edge flicker (user report 2026-07-03, probe-confirmed sx∈[-316,0]
     * rejects at F=2.67). Also gives the shadow decal terrain depth at the
     * revealed edges (it samples through the same recorded affine). */
    int prev_fe = s_render_full_extent;
    float prev_wf = s_render_freeze_wf;
    s_render_full_extent = 1;
    s_render_freeze_wf = openpete_native_widescreen_factor();
    static rhi_vertex pbuf[SCENE_VB_VERTS];
    int m = 0;
    /* Far BACKSTOP: a fullscreen quad at z=0.999 (view-Z ≈ 4.09M — decodes as
     * the far sentinel) drawn under everything, so every pre-pass texel is
     * DEFINED. Without it, texels no WORLD tri covers (sky, cyclorama gaps)
     * hold only the pass CLEAR — and sampling never-drawn cleared depth
     * returned 0.0 (= nearest possible!) on lavapipe, making the SKY an
     * infinitely-near occluder: every moby seen against sky was culled (the
     * stone-hill tree flicker). Same driver-dependence class as the June
     * storeOp=DONT_CARE trap — make it explicit, not implicit.
     * z must stay comfortably below the 0.9999 clamp: prepass.vert's
     * W = 4096z/(1-z) at z=0.9999 is ~4.1e7 where fp32's ulp is 4 — W-1
     * rounds back to W, NDC depth lands on EXACTLY 1.0 and every fragment
     * fails the LESS test against the 1.0 clear (a silent no-op backstop).
     * At z=0.999, W ≈ 4.09e6, depth ≈ 1-2.4e-7 < 1.0 — draws everywhere. */
    {
        static const float bq[6][2] =
            { {-1,-1}, {1,-1}, {1,1}, {-1,-1}, {1,1}, {-1,1} };
        for (int k = 0; k < 6; k++) {
            rhi_vertex* o = &pbuf[m + k];
            o->x = bq[k][0]; o->y = bq[k][1]; o->z = 0.999f;
            o->u = 0.0f; o->v = 0.0f; o->rgba = 0xFF000000u; o->tex = 0;
        }
        m += 6;
    }
    for (int t = 0; t < scene_n && m + 3 <= SCENE_VB_VERTS; t++) {
        int b = scene[t].blend; if (b < 0 || b > 3) b = 0;
        if (b != 0) continue;                                    /* opaque only */
        /* World occluders only — plus the WORLD_FILL LP backing: at the fogged
         * waterline much of the painted terrain IS the hole-fill layer, and the
         * decal / occlusion consumers gain accuracy from counting it as coverage
         * (it is canonical LP geometry at true depth). */
        if (scene[t].category != OPENPETE_SCENE_WORLD &&
            scene[t].category != OPENPETE_SCENE_WORLD_FILL) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex* o = &pbuf[m + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z;
            /* Vertex colour retained for vertex-struct completeness; the shadow
             * and occlusion consumers sample the pre-pass depth only. */
            o->u = 0.0f; o->v = 0.0f; o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        m += 3;
    }
    /* Record the PSX-screen→uv affine of THIS mapping for the consumers
     * (probe the mapping itself rather than duplicating its math):
     * u = (cx+1)/2, v = (1-cy)/2 with cx,cy affine in sx,sy. */
    {
        float cx0, cy0, cx1, cy1;
        scene_to_clip(0.0f, 0.0f, &cx0, &cy0);
        scene_to_clip(1.0f, 1.0f, &cx1, &cy1);
        s_od_uax = (cx1 - cx0) * 0.5f; s_od_ubx = (cx0 + 1.0f) * 0.5f;
        s_od_vay = (cy0 - cy1) * 0.5f; s_od_vby = (1.0f - cy0) * 0.5f;
    }
    s_render_full_extent = prev_fe;
    s_render_freeze_wf = prev_wf;
    rhi_update_buffer(s_nr.prepass_vb, pbuf, sizeof(rhi_vertex) * (size_t)(m > 0 ? m : 1));

    if (getenv("OPENPETE_DEPTH_PREPASS_DEBUG")) {
        static int once = 0;
        if (!once) { once = 1;
            fprintf(stderr, "[depth-prepass] WORLD-opaque tris=%d (of %d scene tris) raster %dx%d in %dx%d\n",
                    m/3, scene_n, vw, vh, s_nr.od_w, s_nr.od_h); }
    }
    /* OPENPETE_PREPASS_RECON_DEBUG=SX,SY: reconstruct the WORLD-space vertices
     * of terrain tris covering the given PSX screen point through the shadow
     * decal's camera model (the pushed inverse) and print them + the implied
     * surface normal — a CPU-side consistency check of the FED terrain
     * geometry against the model, with no GPU in the loop. */
    {
        const char* pr = getenv("OPENPETE_PREPASS_RECON_DEBUG");
        static int printed = 0;
        extern unsigned openpete_frame;
        float psx = 300.0f, psy = 145.0f; unsigned fmin = 0;
        if (pr && *pr) sscanf(pr, "%f,%f,%u", &psx, &psy, &fmin);
        if (pr && *pr && s_shadow_decal.valid && printed < 40 &&
            openpete_frame >= fmin) {
            for (int t = 0; t < scene_n && printed < 40; t++) {
                int b = scene[t].blend; if (b < 0 || b > 3) b = 0;
                if (b != 0 || scene[t].category != OPENPETE_SCENE_WORLD) continue;
                float x0 = scene[t].v[0].sx, x1 = scene[t].v[1].sx, x2 = scene[t].v[2].sx;
                float y0 = scene[t].v[0].sy, y1 = scene[t].v[1].sy, y2 = scene[t].v[2].sy;
                float xmin = fminf(x0, fminf(x1, x2)), xmax = fmaxf(x0, fmaxf(x1, x2));
                float ymin = fminf(y0, fminf(y1, y2)), ymax = fmaxf(y0, fmaxf(y1, y2));
                if (psx < xmin || psx > xmax || psy < ymin || psy > ymax) continue;
                double W[3][3];
                for (int i = 0; i < 3; i++) {
                    W[0][i] = s_shadow_decal.cam_col0[i];
                    W[1][i] = s_shadow_decal.cam_col1[i];
                    W[2][i] = s_shadow_decal.cam_col2[i];
                }
                double P[3][3];
                for (int k = 0; k < 3; k++) {
                    double z = scene[t].v[k].z; if (z > 0.9999) z = 0.9999;
                    double vz = 4096.0 * z / (1.0 - z); if (vz < 1.0) vz = 1.0;
                    double M1 = (scene[t].v[k].sx - s_shadow_decal.ofx) * vz / s_shadow_decal.h;
                    double M2 = (scene[t].v[k].sy - s_shadow_decal.ofy) * vz / s_shadow_decal.h;
                    double V0 = (W[0][0]*M1 + W[0][1]*M2 + W[0][2]*vz) / 4096.0;
                    double V1 = (W[1][0]*M1 + W[1][1]*M2 + W[1][2]*vz) / 4096.0;
                    double V2 = (W[2][0]*M1 + W[2][1]*M2 + W[2][2]*vz) / 4096.0;
                    P[k][0] = V2; P[k][1] = -V0; P[k][2] = -V1;   /* camera-relative world */
                }
                double e1[3] = { P[1][0]-P[0][0], P[1][1]-P[0][1], P[1][2]-P[0][2] };
                double e2[3] = { P[2][0]-P[0][0], P[2][1]-P[0][1], P[2][2]-P[0][2] };
                double N[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
                double nl = sqrt(N[0]*N[0]+N[1]*N[1]+N[2]*N[2]);
                fprintf(stderr, "[prepass-recon] tri sxy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) vz=(%.0f,%.0f,%.0f) relY=(%.0f,%.0f,%.0f) ny=%.2f relZ=(%.0f,%.0f,%.0f) nz=%.2f\n",
                        x0, y0, x1, y1, x2, y2,
                        4096.0*scene[t].v[0].z/(1.0-scene[t].v[0].z),
                        4096.0*scene[t].v[1].z/(1.0-scene[t].v[1].z),
                        4096.0*scene[t].v[2].z/(1.0-scene[t].v[2].z),
                        P[0][1], P[1][1], P[2][1],
                        nl > 1e-9 ? fabs(N[1])/nl : -1.0,
                        P[0][2], P[1][2], P[2][2],
                        nl > 1e-9 ? fabs(N[2])/nl : -1.0);
                printed++;
            }
        }
    }

    /* OPENPETE_OCCL_TRIDUMP=SX,SY[,MINFRAME]: print EVERY scene tri covering
     * the given PSX point — category/blend/textured + per-vert view-Z — the
     * direct "what is actually fed at this pixel" comparison for occlusion
     * triage. MINFRAME filters on openpete_frame (default 0). */
    {
        extern unsigned openpete_frame;
        const char* td = getenv("OPENPETE_OCCL_TRIDUMP");
        static int td_prints = 0;
        if (td && *td && td_prints < 40) {
            float psx = 0, psy = 0; unsigned fmin = 0;
            if (sscanf(td, "%f,%f,%u", &psx, &psy, &fmin) >= 2 && openpete_frame >= fmin) {
                for (int t = 0; t < scene_n && td_prints < 40; t++) {
                    float x0 = scene[t].v[0].sx, x1 = scene[t].v[1].sx, x2 = scene[t].v[2].sx;
                    float y0 = scene[t].v[0].sy, y1 = scene[t].v[1].sy, y2 = scene[t].v[2].sy;
                    if (psx < fminf(x0, fminf(x1, x2)) || psx > fmaxf(x0, fmaxf(x1, x2))) continue;
                    if (psy < fminf(y0, fminf(y1, y2)) || psy > fmaxf(y0, fmaxf(y1, y2))) continue;
                    fprintf(stderr, "[occl-tri f=%u] cat=%d blend=%d tex=%d sxy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) vz=(%.0f,%.0f,%.0f)\n",
                            s_scene_frame, scene[t].category, scene[t].blend, scene[t].textured,
                            x0, y0, x1, y1, x2, y2,
                            4096.0f * scene[t].v[0].z / (1.0f - scene[t].v[0].z),
                            4096.0f * scene[t].v[1].z / (1.0f - scene[t].v[1].z),
                            4096.0f * scene[t].v[2].z / (1.0f - scene[t].v[2].z));
                    td_prints++;
                }
            }
        }
    }

    /* Render the opaque depth (begin clears depth to far=1.0). Even with m==0
     * the cleared far-depth reads as "no occluder" — never a stale frame. */
    rhi_begin_render_target(s_nr.dev, s_nr.opaque_depth, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_set_viewport(s_nr.dev, vw, vh);
    if (m > 0) {
        rhi_bind_pipeline(s_nr.dev, s_nr.prepass_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.prepass_vb);
        rhi_draw(s_nr.dev, (uint32_t)m, 0);
    }
    rhi_end_render_target(s_nr.dev, s_nr.opaque_depth);
    s_od_vw = (float)vw; s_od_vh = (float)vh;
    s_od_ready = 1;
}

/* ---- moby occlusion culling: grid pass + readback + query -------------------
 *
 * Per LIVE present: one tiny render pass reduces the WORLD-opaque depth
 * pre-pass to an OCCL_GW×OCCL_GH grid of the FARTHEST depth per cell
 * (occl_downsample.frag — conservative: any sky/gap texel makes its cell
 * uncullable), which rides the frame's ring readback (rhi_cycle_rt_readback,
 * no GPU stall) back to the CPU RHI_FRAMES_IN_FLIGHT presents later. The moby
 * feeder then asks openpete_native_occl_query BEFORE re-extracting a record:
 * "is this screen rect at this nearest view-Z provably behind nearer terrain
 * everywhere?" — yes ⇒ it skips the model walk, scene tris and draws for a
 * moby the depth test would erase anyway.
 *
 * Staleness discipline: the grid is a FEW PRESENTS OLD, so every consumer is
 * conservative — the query inflates the rect by one cell, demands the
 * occluder be nearer by a slack (OPENPETE_MOBY_OCCL_SLACK, default 1500 world
 * units — also absorbing the terrain feeder's measured off-anchor depth skew,
 * see W2.3), refuses rects touching the grid edge, and refuses grids older
 * than 8 presents (freeze/menu/minimized). The feeder additionally suspends
 * culling around camera cuts (openpete_native_occl_suspend) and widens its
 * rects with camera pan speed. The affine mapping PSX-screen→grid-uv is
 * FIFO'd alongside each queued copy so a retrieved grid decodes with the
 * mapping it was rendered under (it only changes on window/layout changes).
 *
 * Display-only by construction: consumers skip native drawing of provably
 * hidden geometry; nothing here reads back into gameplay state. */
static float occl_slack(void)
{
    static float s = -1.0f;
    if (s < 0.0f) { const char* e = getenv("OPENPETE_MOBY_OCCL_SLACK"); s = (e && *e) ? (float)atof(e) : 1500.0f; }
    return s;
}

typedef struct { float uax, ubx, vay, vby; unsigned stamp; } occl_meta_t;
#define OCCL_FIFO_N 16
static float       s_occl_vz[OCCL_GW * OCCL_GH]; /* decoded farthest view-Z per cell */
static int         s_occl_have = 0;
static unsigned    s_occl_presents = 0;          /* live present counter */
static unsigned    s_occl_stamp = 0;             /* present the current grid was captured at */
static int         s_occl_suspend = 0;           /* presents left of cut suspension */
static occl_meta_t s_occl_fifo[OCCL_FIFO_N];     /* affine+stamp queued with each copy */
static unsigned    s_occl_fifo_head = 0, s_occl_fifo_tail = 0;
static float       s_occl_uax, s_occl_ubx, s_occl_vay, s_occl_vby;

static void native_occl_downsample(void)
{
    s_occl_presents++;
    if (s_occl_suspend > 0) s_occl_suspend--;
    if (!occl_active() || !s_od_ready || !s_nr.fs_quad_vb) return;

    /* Reduce this present's pre-pass into the grid RT (viewport = full RT). */
    struct { int gw, gh, pvw, pvh; } pc =
        { OCCL_GW, OCCL_GH, (int)s_od_vw, (int)s_od_vh };
    rhi_begin_render_target(s_nr.dev, s_nr.occl_grid, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_bind_pipeline(s_nr.dev, s_nr.occl_pipe);
    rhi_bind_depth_sampler(s_nr.dev, s_nr.opaque_depth, 0);
    rhi_push_constants(s_nr.dev, &pc, sizeof pc);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.fs_quad_vb);
    rhi_draw(s_nr.dev, 6, 0);
    rhi_end_render_target(s_nr.dev, s_nr.occl_grid);

    /* Queue this present's copy; retrieve + decode the one from a few presents
     * ago. The FIFO pairs each retrieved grid with the affine it was rendered
     * under (pop order matches the ring's round-robin retrieval order). */
    static uint8_t raw[OCCL_GW * OCCL_GH * 4];
    int gw = 0, gh = 0;
    int got = rhi_cycle_rt_readback(s_nr.dev, s_nr.occl_grid, raw, sizeof raw, &gw, &gh);
    if (got && gw == OCCL_GW && gh == OCCL_GH && s_occl_fifo_tail != s_occl_fifo_head) {
        const occl_meta_t* m = &s_occl_fifo[s_occl_fifo_tail % OCCL_FIFO_N];
        s_occl_fifo_tail++;
        for (int i = 0; i < OCCL_GW * OCCL_GH; i++) {
            uint32_t q = ((uint32_t)raw[i*4+0] << 16) | ((uint32_t)raw[i*4+1] << 8) | raw[i*4+2];
            /* Decode the pre-pass encode d = 1 - 1/viewZ; q at full scale is
             * the far-clear (sky / no occluder) sentinel. */
            s_occl_vz[i] = (q >= 16777200u) ? 3.4e38f
                         : 1.0f / (1.0f - (float)q / 16777215.0f);
        }
        s_occl_uax = m->uax; s_occl_ubx = m->ubx;
        s_occl_vay = m->vay; s_occl_vby = m->vby;
        s_occl_stamp = m->stamp;
        s_occl_have = 1;
        /* OPENPETE_OCCL_GRID_DUMP=MINFRAME: write the decoded grid as a PGM
         * (bright = near, black = far/sky) once, plus the affine — lining it
         * up against a same-frame screenshot exposes orientation/alignment
         * bugs instantly. */
        {
            extern unsigned openpete_frame;
            static int dumped = 0;
            const char* gd = getenv("OPENPETE_OCCL_GRID_DUMP");
            if (gd && *gd && !dumped && openpete_frame >= (unsigned)atoi(gd)) {
                dumped = 1;
                FILE* f = fopen("/tmp/occl-grid.pgm", "wb");
                if (f) {
                    fprintf(f, "P5\n%d %d\n255\n", OCCL_GW, OCCL_GH);
                    for (int i = 0; i < OCCL_GW * OCCL_GH; i++) {
                        float vz = s_occl_vz[i];
                        int px = (vz > 1e30f) ? 0 : (int)(255.0f * 8192.0f / (vz + 8192.0f));
                        fputc(px < 0 ? 0 : px > 255 ? 255 : px, f);
                    }
                    fclose(f);
                    fprintf(stderr, "[occl-grid-dump f=%u] /tmp/occl-grid.pgm  affine uax=%g ubx=%g vay=%g vby=%g  psx(440,51)->uv(%.3f,%.3f)->cell(%d,%d)\n",
                            openpete_frame, s_occl_uax, s_occl_ubx, s_occl_vay, s_occl_vby,
                            s_occl_uax * 440.0f + s_occl_ubx, s_occl_vay * 51.0f + s_occl_vby,
                            (int)floorf((s_occl_uax * 440.0f + s_occl_ubx) * OCCL_GW),
                            (int)floorf((s_occl_vay * 51.0f + s_occl_vby) * OCCL_GH));
                    static const int probes[][2] = { {55,9}, {60,2}, {32,40}, {5,5}, {32,20} };
                    for (int p = 0; p < 5; p++) {
                        int i = probes[p][1] * OCCL_GW + probes[p][0];
                        fprintf(stderr, "[occl-grid-cell] (%d,%d) raw RGBA=%02x %02x %02x %02x -> vz=%g\n",
                                probes[p][0], probes[p][1],
                                raw[i*4+0], raw[i*4+1], raw[i*4+2], raw[i*4+3], s_occl_vz[i]);
                    }
                }
            }
        }
        /* OPENPETE_OCCL_SELFTEST: end-to-end depth-path probe — a screen-centre
         * rect at effectively infinite view-Z must read CULLED whenever the
         * centre cells hold opaque terrain (they do in normal gameplay; a
         * sky-centred shot legitimately reads 0). Proves grid render →
         * readback → decode → scan without needing a wall scene. */
        static int st = -1;
        if (st < 0) { const char* e = getenv("OPENPETE_OCCL_SELFTEST"); st = (e && *e && *e != '0'); }
        if (st) {
            static uint64_t t0 = 0;
            uint64_t now = openpete_phase_now_us();
            if (now - t0 >= 1000000u) {
                t0 = now;
                int sus = s_occl_suspend; s_occl_suspend = 0;   /* probe past the gate */
                int far_probe = openpete_native_occl_query(248.0f, 114.0f, 264.0f, 126.0f, 1.0e8f);
                s_occl_suspend = sus;
                fprintf(stderr, "[occl-selftest] centre far-probe=%d (1 ⇔ opaque terrain covers screen centre)\n",
                        far_probe);
            }
        }
    } else if (got) {
        s_occl_fifo_tail = s_occl_fifo_head;   /* desync (shouldn't happen) — resync empty */
    }
    if (s_occl_fifo_head - s_occl_fifo_tail >= OCCL_FIFO_N) s_occl_fifo_tail++; /* overflow: drop oldest */
    occl_meta_t* mm = &s_occl_fifo[s_occl_fifo_head % OCCL_FIFO_N];
    mm->uax = s_od_uax; mm->ubx = s_od_ubx; mm->vay = s_od_vay; mm->vby = s_od_vby;
    mm->stamp = s_occl_presents;
    s_occl_fifo_head++;
}

static long s_occl_qstat[OPENPETE_OCCL_Q_COUNT];   /* query-outcome census */
static float s_occl_last_scan_max;                 /* diagnostics (last CULLED scan) */

int openpete_native_occl_query(float sx0, float sy0, float sx1, float sy1, float vz_near)
{
    if (!s_occl_have) { s_occl_qstat[OPENPETE_OCCL_Q_NOGRID]++; return 0; }
    if (s_occl_suspend > 0) { s_occl_qstat[OPENPETE_OCCL_Q_SUSPENDED]++; return 0; }
    if (s_occl_presents - s_occl_stamp > 8u) { s_occl_qstat[OPENPETE_OCCL_Q_STALE]++; return 0; }
    float u0 = s_occl_uax * sx0 + s_occl_ubx, u1 = s_occl_uax * sx1 + s_occl_ubx;
    float v0 = s_occl_vay * sy0 + s_occl_vby, v1 = s_occl_vay * sy1 + s_occl_vby;
    if (u1 < u0) { float t = u0; u0 = u1; u1 = t; }
    if (v1 < v0) { float t = v0; v0 = v1; v1 = t; }
    /* Native frustum cull: fully outside the visible extent (the same extent
     * the affine maps — Hor+ widened when active) contributes zero pixels
     * regardless of depth. The game's view cone is wider than the screen, so
     * enclosed areas queue plenty of these. vz guard: behind-camera / near-
     * plane-straddling projections are garbage coordinates — never trust
     * their rect. (vz_near already carries the caller's staleness slack.) */
    if (vz_near > 256.0f && (u1 < 0.0f || u0 > 1.0f || v1 < 0.0f || v0 > 1.0f)) {
        s_occl_qstat[OPENPETE_OCCL_Q_OFFSCREEN]++;
        return 1;
    }
    float need = vz_near - occl_slack();
    if (need <= 1.0f) { s_occl_qstat[OPENPETE_OCCL_Q_TOONEAR]++; return 0; }
    /* One-cell inflation, then CLAMP to the grid: the off-grid part of a rect
     * is outside the visible extent (zero pixels by definition), so only the
     * on-grid slice needs the depth test. All-near cells there ⇒ every pixel
     * the moby could produce is behind nearer opaque WORLD ⇒ cullable. */
    int cx0 = (int)floorf(u0 * (float)OCCL_GW) - 1, cx1 = (int)floorf(u1 * (float)OCCL_GW) + 1;
    int cy0 = (int)floorf(v0 * (float)OCCL_GH) - 1, cy1 = (int)floorf(v1 * (float)OCCL_GH) + 1;
    if (cx0 < 0) cx0 = 0; if (cy0 < 0) cy0 = 0;
    if (cx1 >= OCCL_GW) cx1 = OCCL_GW - 1;
    if (cy1 >= OCCL_GH) cy1 = OCCL_GH - 1;
    float scan_max = 0.0f;
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++) {
            float c = s_occl_vz[cy * OCCL_GW + cx];
            if (c > scan_max) scan_max = c;
            if (c >= need) {
                s_occl_qstat[OPENPETE_OCCL_Q_VISIBLE]++; return 0;
            }
        }
    s_occl_last_scan_max = scan_max;   /* diagnostics: farthest cell in the culled rect */
    s_occl_qstat[OPENPETE_OCCL_Q_CULLED]++;
    return 1;
}

float openpete_native_occl_last_scan_max(void) { return s_occl_last_scan_max; }

void openpete_native_occl_qstats(long out[OPENPETE_OCCL_Q_COUNT])
{
    for (int i = 0; i < OPENPETE_OCCL_Q_COUNT; i++) out[i] = s_occl_qstat[i];
}

void openpete_native_occl_suspend(int presents)
{
    if (presents > s_occl_suspend) s_occl_suspend = presents;
}

/* Widescreen (Hor+) factor for the feeders' horizontal trivial rejects. 1.0
 * unless vk-only with a wider-than-4:3 window (set in update_layout). */
float openpete_native_widescreen_factor(void)
{
    return s_nr.ws_factor > 1.0f ? s_nr.ws_factor : 1.0f;
}
/* Visible PSX screen-X range under the active widen factor: [0,512] at 4:3,
 * widened symmetrically about the screen centre (256). */
void openpete_native_widescreen_x_bounds(float *xlo, float *xhi)
{
    float F = openpete_native_widescreen_factor();
    if (xlo) *xlo = 256.0f - 256.0f * F;
    if (xhi) *xhi = 256.0f + 256.0f * F;
}

/* The clip-space rect the full-frame cover quads (background fill / fade /
 * freeze backdrop) span. The --aspect RENDER rect (4:3 sub-rect widened by the
 * Hor+ factor) so the revealed side strips get a background behind the sky dome /
 * world — but NOT the letterbox/pillarbox bars beyond it (they stay black). The
 * whole target in full_extent (native_rtt) mode. */
static void cover_rect(int full_extent, float *l, float *r, float *t, float *b)
{
    float fhx = s_nr.p0_hx * s_nr.ws_factor;   /* --aspect rect half-width (= p0_hx at 4:3) */
    *l = full_extent ? -1.0f : s_nr.p0_cx - fhx;
    *r = full_extent ?  1.0f : s_nr.p0_cx + fhx;
    *t = full_extent ?  1.0f : s_nr.p0_hy;
    *b = full_extent ? -1.0f : -s_nr.p0_hy;
}

/* The scene background colour = what PsyCross actually clears the framebuffer to,
 * sampled from a top-left corner of the reference readback. This matches PsyCross
 * in EVERY state (levels, titlescreen, menus) and mode — unlike a gamestate
 * source like g_Cyclorama.m_BackgroundColor, which is the level sky colour but is
 * stale/wrong outside a level (e.g. the titlescreen, where the frame clears to a
 * different colour). Returns 0 (no bg) until a reference frame exists; the caller
 * then falls back to the scene-fed colour. rgba byte order R,G,B,A.
 *
 * ONLY a valid source in the A/B / diff HARNESS, where pixel-matching PsyCross is
 * the point. In vk-only (the shipping path) the reference readback is still
 * populated (sync_reference_texture runs ungated), but its top-left corner is
 * frequently CONTENT, not the clear — the cyclorama dome, the HUD (gem/life
 * counters), or overscan — and reusing PsyCross's per-frame output violates the
 * native renderer's Core Principle (derive from gamestate). So return 0 outside
 * compare mode, forcing the gamestate `scene_bg` (g_Cyclorama.m_BackgroundColor)
 * to be used instead. (Non-level vk states still want a gamestate draw-env clear
 * source — tracked as a follow-up, see the native-renderer plan.) */
static uint32_t bg_from_ref_corner(void)
{
    if (!s_scene_ref_compare) return 0;   /* vk-only: use gamestate scene_bg, not PsyCross */
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
/* The flat 2D HUD layer (depth-test off): the menu dark panel (HUD_PANEL, alpha)
 * first, then the textured HUD (2D backdrops), then the untextured HUD text/glyphs.
 * Textured-before-untextured matches the PSX HUD-OT submission order (a 2D backdrop
 * is linked before the text/icon glyphs — e.g. the flight results green backdrop
 * UNDER the "CRASHED" text). Drawn either UNDER the player (gi==2, GS_LevelTransition
 * flyby so Spyro flies in front of the revealed text) or, by default, OVER the player
 * (after the whole scene) — see hud_under in draw_native_scene. The 3D HUD icons
 * (HUD3D) are a separate depth-isolated pass drawn after this either way. */
/* Textured-tri material batching (see the bucket build in render_scene). The
 * scene index of each textured block-tri, in VB order, so the draw side can read
 * a tri's material and coalesce adjacent same-material runs into one draw. */
static int s_txscene[MAX_SCENE_TRIS];
/* Flat-HUD within-layer painter order (see the bucket-6 build in render_scene):
 * back-to-front by centroid z, ties broken by submission index so equal-z runs
 * (all the 2D-UI content at z=0) keep submission order — a STABLE descending
 * sort built on qsort. */
typedef struct { float z; int t; } hud_paint_t;
static int hud_paint_cmp(const void* pa, const void* pb) {
    const hud_paint_t* a = (const hud_paint_t*)pa;
    const hud_paint_t* b = (const hud_paint_t*)pb;
    if (a->z > b->z) return -1;   /* farther first */
    if (a->z < b->z) return 1;
    return (a->t < b->t) ? -1 : (a->t > b->t) ? 1 : 0;
}

static const scene_tri_t* s_txsort_scene;   /* qsort context (set per call) */
static int tex_mat_cmp(const void* pa, const void* pb) {
    const scene_tri_t* a = &s_txsort_scene[*(const int*)pa];
    const scene_tri_t* b = &s_txsort_scene[*(const int*)pb];
    if (a->tpage_y != b->tpage_y) return a->tpage_y < b->tpage_y ? -1 : 1;
    if (a->tpage_x != b->tpage_x) return a->tpage_x < b->tpage_x ? -1 : 1;
    if (a->clut_y  != b->clut_y ) return a->clut_y  < b->clut_y  ? -1 : 1;
    if (a->clut_x  != b->clut_x ) return a->clut_x  < b->clut_x  ? -1 : 1;
    if (a->bpp     != b->bpp    ) return a->bpp     < b->bpp     ? -1 : 1;
    /* Provenance tag joins the key ONLY when a split consumer is armed
     * (registered selector / OPENPETE_TAG_SPLIT) — MUST stay in lockstep
     * with the coalesce run-break in draw_tex_runs, or batches that this
     * sort separates get re-merged (or worse, interleave and split into
     * many runs). Inert ⇒ byte-identical batching without mods. */
    if (tag_split_on() && a->tag != b->tag)
        return a->tag < b->tag ? -1 : 1;
    return 0;
}
/* ---- HD texture replacement (openpete-texture-replacement Phases 3+4) -----
 * Identity comes from the upload-time registry (texture_pack_resolve, per
 * TRI — run-union boxes vary with per-frame clipping); the pack match is
 * containment-based (texture_pack_match), so one tile-sized file covers LP
 * full-tile draws, HP sub-quad draws, and partially-clipped boxes alike.
 * (The original hardcoded title-logo PoC path is retired — the generic path
 * replaced it 1:1.) */

/* ---- HD replacement GPU-texture cache (Phase 4) ---------------------------
 * identity hashkey -> uploaded rhi texture (linear-filtered). Slot collision
 * keeps the incumbent and falls back to SD for the newcomer (destroying an
 * in-flight texture is not frame-safe; with 512 slots and pack sizes in the
 * dozens this is theoretical). */
#define HD_CACHE_SLOTS 8192   /* pack sizes reach thousands game-wide; probe-16 needs headroom */
static struct { uint64_t key; rhi_texture *tex; } s_hd_cache[HD_CACHE_SLOTS];

/* GPU uploads cannot record inside the offscreen render pass, so a first
 * sighting at draw time only QUEUES the image; hd_flush_pending() uploads at
 * the next frame start (one SD frame on first appearance). */
static struct { uint64_t key; const unsigned char *px; int w, h; } s_hd_pending[64];
static int s_hd_pending_n = 0;

static rhi_texture *hd_cache_lookup(uint64_t key)
{
    uint64_t base = key * 0x9E3779B97F4A7C15ull >> 32;
    for (int p = 0; p < 16; p++) {             /* linear probe */
        int idx = (int)((base + (uint64_t)p) & (HD_CACHE_SLOTS - 1));
        if (s_hd_cache[idx].key == key) return s_hd_cache[idx].tex;
        if (s_hd_cache[idx].key == 0) return NULL;
    }
    return NULL;
}

static void hd_pending_add(uint64_t key, const unsigned char *px, int w, int h)
{
    if (s_hd_pending_n >= 64) return;
    for (int i = 0; i < s_hd_pending_n; i++)
        if (s_hd_pending[i].key == key) return;
    s_hd_pending[s_hd_pending_n++] = (__typeof__(s_hd_pending[0])){ key, px, w, h };
}

static void hd_flush_pending(void)   /* call OUTSIDE any render pass */
{
    for (int i = 0; i < s_hd_pending_n; i++) {
        uint64_t key = s_hd_pending[i].key;
        uint64_t base = key * 0x9E3779B97F4A7C15ull >> 32;
        int idx = -1;
        for (int p = 0; p < 16; p++) {         /* linear probe: existing or empty */
            int c = (int)((base + (uint64_t)p) & (HD_CACHE_SLOTS - 1));
            if (s_hd_cache[c].key == key) { idx = -1; break; }   /* already up */
            if (s_hd_cache[c].key == 0) { idx = c; break; }
        }
        if (idx < 0) {                          /* already uploaded, or neighbourhood full */
            static int drops = 0;
            if (drops < 8 && hd_cache_lookup(key) == NULL)
                fprintf(stderr, "[texpack] GPU cache insert dropped (key=%016llx, %d)\n",
                        (unsigned long long)key, ++drops);
            continue;
        }
        rhi_texture *t = rhi_create_rgba8(s_nr.dev, (uint32_t)s_hd_pending[i].w,
                                          (uint32_t)s_hd_pending[i].h);
        if (!t) continue;
        rhi_update_rgba8(s_nr.dev, t, s_hd_pending[i].px);
        rhi_texture_set_filter_linear(s_nr.dev, t);
        s_hd_cache[idx].key = key;
        s_hd_cache[idx].tex = t;
    }
    s_hd_pending_n = 0;
}

static void hd_cache_shutdown(void)
{
    for (int i = 0; i < HD_CACHE_SLOTS; i++)
        if (s_hd_cache[i].tex) {
            rhi_destroy_texture(s_nr.dev, s_hd_cache[i].tex);
            s_hd_cache[i].tex = NULL; s_hd_cache[i].key = 0;
        }
}

/* Draw a bucket's textured vert range [first_vert, first_vert+vert_count),
 * coalescing adjacent same-material tris into one rhi_draw (one push-constant per
 * run, not per tri). Caller binds the pipeline + vram + vertex buffer; `self` is
 * the pipeline the caller bound, restored after an HD-replaced run (an HD run
 * swaps pipeline + texture, so batching survives: a replaced material is its own
 * run by construction). The same restore covers S8a/S8b material
 * overrides (a resolved run swaps to the mod pipeline, then restores).
 * NB: `self` is never a depth_bias pipeline at any current call site
 * (those are the shadow-decal/stencil pipes, not the scene_tex* pipes
 * passed here), so the dynamic-bias-invalidation-on-restore gap is
 * unreachable today — a depth_bias caller would have to re-apply its
 * bias after this function returns. */
static void draw_tex_runs(int first_vert, int vert_count, int tx_start,
                          const scene_tri_t* scene, rhi_pipeline* self,
                          rhi_pipeline* hd_pipe, int allow_override) {
    int base = (first_vert - tx_start) / 3, ntri = vert_count / 3, i = 0;
    while (i < ntri) {
        const scene_tri_t* s0 = &scene[s_txscene[base + i]];
        const psx_material m = { s0->tpage_x, s0->tpage_y, s0->clut_x, s0->clut_y, s0->bpp };
        int run = 1;
        while (i + run < ntri) {
            const scene_tri_t* s1 = &scene[s_txscene[base + i + run]];
            if (s1->tpage_x != s0->tpage_x || s1->tpage_y != s0->tpage_y ||
                s1->clut_x != s0->clut_x || s1->clut_y != s0->clut_y || s1->bpp != s0->bpp) break;
            /* Lockstep with tex_mat_cmp's tag term (S6): a tag the sort
             * separated must also break the run here or coalescing undoes
             * the split. */
            if (tag_split_on() && s1->tag != s0->tag) break;
            run++;
        }
        /* Mod material override (S8a): the run's provenance tag resolves
         * to a registered selector — draw the WHOLE run with the mod
         * pipeline (same bucket/pass/depth: scheduling is engine-owned;
         * runs are tag-pure because the split key is armed whenever a
         * selector lives). Only in plain opaque groups (allow_override):
         * stencil-marking WORLD (holefill) and blend groups keep stock.
         * Stock-stage fallbacks: psx_scene.vert/.frag — fork them from
         * sdk/shaders/. */
        if (allow_override && tag_split_on() && s0->tag) {
            openpete_mod_material_gpu_t mg;
            if (openpete_mods_material_resolve(s0->tag, &mg) >= 0) {
                if (*mg.pipeline_slot == NULL) {
                    rhi_pipeline_desc md = {
                        .vert_spv = "psx_scene.vert.spv",
                        .frag_spv = "psx_scene.frag.spv",
                        .blend = RHI_BLEND_NONE,
                        .depth_test = true,
                        .mod_pipeline = true,
                        .push_const_size = sizeof(psx_material),
                    };
                    rhi_pipeline* p = rhi_create_pipeline_spv(
                        s_nr.dev, &md, mg.vert_spv, mg.vert_words,
                        mg.frag_spv, mg.frag_words);
                    *mg.pipeline_slot = p ? (void*)p : PFX_PIPE_FAILED;
                    if (!p)
                        fprintf(stderr, "[native] material: pipeline build "
                                        "FAILED for %s\n", mg.owner);
                }
                if (*mg.pipeline_slot != PFX_PIPE_FAILED) {
                    rhi_bind_pipeline(s_nr.dev, (rhi_pipeline*)*mg.pipeline_slot);
                    rhi_bind_texture_set(s_nr.dev, s_nr.vram_tex, 0);
                    /* engine block 0 + the per-instance slot's mod block
                     * (S8b, block 1+slot; slot 0 = declarative binds the
                     * engine block as the mod block too — harmless). */
                    if (s_mat_ubo)
                        rhi_ubo_bind(s_nr.dev, s_mat_ubo, 0,
                                     mg.uniform_slot > 0 ? 1 + mg.uniform_slot : 0);
                    rhi_push_constants(s_nr.dev, &m, sizeof m);
                    rhi_draw(s_nr.dev, (uint32_t)(run * 3),
                             (uint32_t)(first_vert + i * 3));
                    rhi_bind_pipeline(s_nr.dev, self);   /* HD-path restore discipline */
                    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                    i += run;
                    continue;
                }
            }
        }
        if (s0->bpp == 32) {
            /* mod-texture run (openpete_mods_texture_register): tpage_x
             * carries the registered id; bind the RGBA texture through
             * the HD pipe, UVs raw texel coords -> rect (0,0,w,h). A
             * pending upload (first sighting) or a missing registration
             * draws NOTHING this frame — SD would sample VRAM noise. */
            int mw = 0, mh = 0;
            const unsigned char* px =
                openpete_mods_texture_get((uint16_t)s0->tpage_x, &mw, &mh);
            if (px && hd_pipe) {
                uint64_t key = (uint64_t)(uintptr_t)px;   /* stable per registration */
                rhi_texture* tex = hd_cache_lookup(key);
                if (!tex) {
                    hd_pending_add(key, px, mw, mh);
                } else {
                    const float rect[4] = { 0.f, 0.f, (float)mw, (float)mh };
                    rhi_bind_pipeline(s_nr.dev, hd_pipe);
                    rhi_bind_vram(s_nr.dev, tex);
                    rhi_push_constants(s_nr.dev, rect, sizeof rect);
                    rhi_draw(s_nr.dev, (uint32_t)(run * 3), (uint32_t)(first_vert + i * 3));
                    rhi_bind_pipeline(s_nr.dev, self);
                    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                }
            }
            i += run;
            continue;
        }
        /* Registry query — Phase-1 validation (RESOLVE_LOG), Phase-2 dump
         * (DUMP_DIR), Phase-4 replacement. Identity is PER TRI (a run-union
         * box varies with per-frame clipping and tile composition); adjacent
         * tris matching the same pack file coalesce into HD sub-runs, the
         * rest draw SD in material-coalesced groups as before. */
        static int s_resolve_log = -1, s_dump_on = -1;
        if (s_resolve_log < 0) {
            s_resolve_log = getenv("OPENPETE_TEXPACK_RESOLVE_LOG") ? 1 : 0;
            s_dump_on     = getenv("OPENPETE_TEXPACK_DUMP_DIR") ? 1 : 0;
        }
        const int want_replace = hd_pipe && texture_pack_replacements_active();
        if (!(s_resolve_log || s_dump_on || want_replace)) {
            rhi_push_constants(s_nr.dev, &m, sizeof m);
            rhi_draw(s_nr.dev, (uint32_t)(run * 3), (uint32_t)(first_vert + i * 3));
            i += run;
            continue;
        }
        int t = 0;
        int m_pushed = 0;                       /* material push valid for SD draws */
        while (t < run) {
            /* This tri's texel box -> identity -> pack match. */
            const scene_tri_t* st = &scene[s_txscene[base + i + t]];
            float u0 = st->v[0].u, v0 = st->v[0].v, u1 = u0, v1 = v0;
            for (int j = 1; j < 3; j++) {
                if (st->v[j].u < u0) u0 = st->v[j].u;
                if (st->v[j].u > u1) u1 = st->v[j].u;
                if (st->v[j].v < v0) v0 = st->v[j].v;
                if (st->v[j].v > v1) v1 = st->v[j].v;
            }
            texture_pack_id id;
            texture_pack_repl repl;
            int hit = texture_pack_resolve(m.tpage_x, m.tpage_y, m.clut_x, m.clut_y,
                                           m.bpp, (int)u0, (int)v0, (int)u1, (int)v1, &id);
            int matched = hit && want_replace && texture_pack_match(&id, &repl);
            if (hit && s_dump_on)
                texture_pack_dump_draw(&id, m.tpage_x, m.tpage_y,
                                       m.clut_x, m.clut_y, (int)u0, (int)v0);
            static int s_logged = 0;
            if (s_resolve_log && s_logged < 256) {
                s_logged++;
                fprintf(stderr, "[texpack] resolve tp=%d,%d clut=%d,%d bpp=%d uv=%d,%d-%d,%d -> %s\n",
                        m.tpage_x, m.tpage_y, m.clut_x, m.clut_y, m.bpp,
                        (int)u0, (int)v0, (int)u1, (int)v1, hit ? "hit" : "MISS");
            }
            /* Grow the sub-run: consecutive tris with the same disposition
             * (same pack file, or both unreplaced). */
            int g = 1;
            while (t + g < run) {
                const scene_tri_t* sn = &scene[s_txscene[base + i + t + g]];
                float nu0 = sn->v[0].u, nv0 = sn->v[0].v, nu1 = nu0, nv1 = nv0;
                for (int j = 1; j < 3; j++) {
                    if (sn->v[j].u < nu0) nu0 = sn->v[j].u;
                    if (sn->v[j].u > nu1) nu1 = sn->v[j].u;
                    if (sn->v[j].v < nv0) nv0 = sn->v[j].v;
                    if (sn->v[j].v > nv1) nv1 = sn->v[j].v;
                }
                texture_pack_id nid;
                texture_pack_repl nrepl;
                int nhit = texture_pack_resolve(m.tpage_x, m.tpage_y, m.clut_x, m.clut_y,
                                                m.bpp, (int)nu0, (int)nv0, (int)nu1, (int)nv1, &nid);
                int nmatched = nhit && want_replace && texture_pack_match(&nid, &nrepl);
                if (nhit && s_dump_on)
                    texture_pack_dump_draw(&nid, m.tpage_x, m.tpage_y,
                                           m.clut_x, m.clut_y, (int)nu0, (int)nv0);
                if (nmatched != matched) break;
                if (matched && nrepl.pixels != repl.pixels) break;
                g++;
            }
            /* OPENPETE_TEXPACK_STATS=1: sub-run disposition counters. */
            static int s_stats = -1;
            static long sc_hd = 0, sc_pend = 0, sc_sd = 0, sc_n = 0;
            if (s_stats < 0) s_stats = getenv("OPENPETE_TEXPACK_STATS") ? 1 : 0;
            if (matched) {
                uint64_t key = (uint64_t)(uintptr_t)repl.pixels;   /* stable per file */
                rhi_texture *tex = hd_cache_lookup(key);
                if (s_stats) {
                    if (tex) sc_hd++; else sc_pend++;
                    if (++sc_n % 5000 == 0)
                        fprintf(stderr, "[texpack] stats hd=%ld pend=%ld sd=%ld last_pipe=%s\n",
                                sc_hd, sc_pend, sc_sd,
                                hd_pipe == s_nr.hd_cutout_mark_pipe ? "mark" :
                                hd_pipe == s_nr.hd_cutout_pipe ? "cutout" : "hud");
                }
                if (!tex) {
                    hd_pending_add(key, repl.pixels, repl.w, repl.h);   /* next frame */
                } else {
                    /* Shader rect = the FILE's box in page-texel coords: the
                     * query box (u0 page-relative <-> id.box upload-relative)
                     * anchors the conversion. */
                    const float rect[4] = {
                        u0 - (float)(id.box_x - repl.box_x),
                        v0 - (float)(id.box_y - repl.box_y),
                        (float)repl.box_w, (float)repl.box_h,
                    };
                    rhi_bind_pipeline(s_nr.dev, hd_pipe);
                    rhi_bind_vram(s_nr.dev, tex);
                    rhi_push_constants(s_nr.dev, rect, sizeof rect);
                    rhi_draw(s_nr.dev, (uint32_t)(g * 3), (uint32_t)(first_vert + (i + t) * 3));
                    rhi_bind_pipeline(s_nr.dev, self);
                    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                    m_pushed = 0;               /* push constants were overwritten */
                    t += g;
                    continue;
                }
            }
            if (s_stats && !matched && want_replace) sc_sd++;
            if (!m_pushed) {
                rhi_push_constants(s_nr.dev, &m, sizeof m);
                m_pushed = 1;
            }
            rhi_draw(s_nr.dev, (uint32_t)(g * 3), (uint32_t)(first_vert + (i + t) * 3));
            t += g;
        }
        i += run;
    }
}

static void draw_hud_flat_layer(const scene_tri_t* scene, int scene_n,
                                const int* ut_first, const int* ut_count, int tx_start,
                                int hud_tex_first, int hud_tex_count)
{
    if (ut_count[8] > 0) {                                          /* HUD_PANEL (semi-transparent 2D box) */
        /* Each HUD-panel box carries its real PSX ABR mode in its blend field (read
         * from the box's DR_MODE in ui_sprite_native): mode 2 (B-F) -> subtractive
         * (blend 3), mode 0 ((B+F)/2) -> alpha (blend 1). Draw each box with the pipe
         * matching its blend. The slot-8 VB range is in scene[] submission order, so
         * walking scene[] for HUD_PANEL tris tracks the vertex offset in lockstep. */
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        int voff = ut_first[8], vend = ut_first[8] + ut_count[8];
        for (int t = 0; t < scene_n && voff + 3 <= vend; t++) {
            if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD_PANEL) continue;
            rhi_pipeline *pipe = (scene[t].blend == 3) ? s_nr.gouraud_hud_sub_pipe
                                                       : s_nr.gouraud_hud_blend_pipe;
            if (pipe) { rhi_bind_pipeline(s_nr.dev, pipe); rhi_draw(s_nr.dev, 3, (uint32_t)voff); }
            voff += 3;
        }
    }
    if (hud_tex_count > 0 && s_nr.vram_tex && s_nr.scene_tex_hud_pipe) {  /* textured HUD (2D backdrops) */
        rhi_bind_pipeline(s_nr.dev, s_nr.scene_tex_hud_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        draw_tex_runs(hud_tex_first, hud_tex_count, tx_start, scene, s_nr.scene_tex_hud_pipe,
                      s_nr.hd_tex_pipe, 0);  /* bucket 6, submission order + run-coalesce; HD = alpha, depth-off */
    }
    if (s_nr.gouraud_hud_pipe && ut_count[6] > 0) {                 /* untextured HUD text/glyphs */
        /* The slot-6 VB range is depth-sorted back-to-front at build time (see
         * the bucket-6 build in render_scene) so rotating near-OT mobys — the
         * inventory letters/icons — self-occlude correctly despite the
         * depth-off pipe. (The textured HUD range above stays submission-order:
         * its content is constant-z 2D sprites; extend the sort there if a
         * rotating TEXTURED near-OT moby ever appears.) */
        rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_hud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count[6], (uint32_t)ut_first[6]);
    }
}

/* 3c falsify-first instrument: per-present census of ALPHA-class (blend==1, ABR
 * mode 0 (B+F)/2) tris — the ONLY order-dependent class (additive/subtractive are
 * commutative, so a back-to-front sort is a provable no-op for them). Reports the
 * alpha count, its NDC-depth spread, and the number of overlapping pairs emitted
 * OUT of back-to-front order (i emitted before j, screen bboxes overlap, i nearer
 * than j -> i should have been drawn last). inversions>0 on any frame means the
 * sort would visibly change that frame; inversions==0 across a corpus means the
 * sort is unobservable there. Env-gated (OPENPETE_ALPHA_CENSUS), inert otherwise.
 * Display-only diagnostic. */
static void alpha_census(const scene_tri_t* scene, int scene_n)
{
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_ALPHA_CENSUS"); s_on = (e && *e && *e != '0') ? 1 : 0; }
    if (!s_on) return;

    /* Blend histogram + running max per class. Sampling-proof: print whenever ANY
     * class sets a new running high (so the FIRST add>0 / sub>0 / alpha>0 always
     * shows), plus the first few frames (fire-confirm) and every alpha frame. This
     * settles "does mode-0 alpha ever occur" regardless of sample spacing. */
    static int s_shown = 0;
    static int mx[4] = {0,0,0,0};
    int hb[4] = {0,0,0,0};
    for (int t = 0; t < scene_n; t++) { int b = scene[t].blend; if (b >= 0 && b <= 3) hb[b]++; }
    int newmax = 0;
    for (int b = 0; b < 4; b++) { if (hb[b] > mx[b]) { mx[b] = hb[b]; newmax = 1; } }
    if (scene_n > 0 && (s_shown < 4 || newmax || hb[1] > 0)) {
        if (s_shown < 4) s_shown++;
        fprintf(stderr, "BLEND f=%u n=%d  o=%d alpha=%d add=%d sub=%d | max[o/a/ad/s]=%d/%d/%d/%d\n",
                s_scene_frame, scene_n, hb[0], hb[1], hb[2], hb[3], mx[0], mx[1], mx[2], mx[3]);
    }

    /* Collect alpha tris: centroid depth + screen bbox, in EMIT order. */
    enum { CAP = 4096 };
    static float cz[CAP]; static float bx0[CAP], by0[CAP], bx1[CAP], by1[CAP]; static unsigned char tex[CAP];
    int m = 0;
    for (int t = 0; t < scene_n && m < CAP; t++) {
        if (scene[t].blend != 1) continue;
        float x0 = scene[t].v[0].sx, x1 = x0, y0 = scene[t].v[0].sy, y1 = y0, zs = scene[t].v[0].z;
        for (int k = 1; k < 3; k++) {
            float x = scene[t].v[k].sx, y = scene[t].v[k].sy, z = scene[t].v[k].z;
            if (x < x0) x0 = x; if (x > x1) x1 = x;
            if (y < y0) y0 = y; if (y > y1) y1 = y;
            zs += z;
        }
        cz[m] = zs / 3.0f; bx0[m] = x0; by0[m] = y0; bx1[m] = x1; by1[m] = y1;
        tex[m] = (unsigned char)(scene[t].textured ? 1 : 0);
        m++;
    }
    if (m == 0) return;

    float zmin = cz[0], zmax = cz[0]; int ntex = 0;
    for (int i = 0; i < m; i++) { if (cz[i] < zmin) zmin = cz[i]; if (cz[i] > zmax) zmax = cz[i]; ntex += tex[i]; }

    /* Out-of-order overlapping pairs. i<j = emit order; inversion if bboxes overlap
     * and i is NEARER (smaller z) than j (i drawn first but should be drawn last). */
    int overlaps = 0, inversions = 0;
    for (int i = 0; i < m; i++) {
        for (int j = i + 1; j < m; j++) {
            if (bx0[i] > bx1[j] || bx0[j] > bx1[i] || by0[i] > by1[j] || by0[j] > by1[i]) continue;
            overlaps++;
            if (cz[i] < cz[j]) inversions++;   /* nearer tri emitted first -> wrong for back-to-front */
        }
    }
    fprintf(stderr, "ALPHA-CENSUS f=%u alpha=%d (tex=%d) zspan=[%.3f,%.3f] overlaps=%d inversions=%d\n",
            s_scene_frame, m, ntex, (double)zmin, (double)zmax, overlaps, inversions);
}

/* W2.5 — hardware depth-bias factors for the shadow decal group (the
 * SUBTRACT pipes). vkCmdSetDepthBias order semantics: bias = constant×r +
 * slope×m, floored at clamp (all negative here → pull toward the camera under
 * LESS_OR_EQUAL). r ≈ 2^-24 relative for D32_SFLOAT, m = max per-pixel depth
 * slope. Defaults: constant -64 (~4e-6 NDC — kills float-rounding ties vs the
 * coplanar ground), slope -2 (grows the pull exactly at grazing angles), clamp
 * -0.002 (total pull bounded well under the retired ×0.96 view-Z pull's
 * ~5-9e-3, so a genuine occluder is never reordered).
 * Tunable: OPENPETE_SHADOW_HW_BIAS="constant,slope,clamp". */
static void shadow_hw_bias(float* c, float* s, float* cl)
{
    static int init = 0;
    static float vc = -64.0f, vs = -2.0f, vcl = -0.002f;
    if (!init) {
        const char* e = getenv("OPENPETE_SHADOW_HW_BIAS");
        if (e && *e) sscanf(e, "%f,%f,%f", &vc, &vs, &vcl);
        init = 1;
    }
    *c = vc; *s = vs; *cl = vcl;
}

/* Portal-mask camera-ward pull — the close-range "ground disocclusion" knob.
 * The mask is CONFINED to the portal footprint, so a pull here cannot
 * side-poke past the arch (the failure that killed the historical forward
 * view-Z biases); it only decides whether near lips INSIDE the aperture (the
 * arch threshold step at very close range) beat the preview, reproducing the
 * PSX painter look where the sky bucket covered the step. Default NEUTRAL
 * (0,0,0 = current behaviour: real near geometry occludes the preview); tune
 * live via OPENPETE_PORTAL_MASK_BIAS="c,s,clamp" (try "-64,-4,-0.02"). */
static void portal_mask_bias(float* c, float* s, float* cl)
{
    static int init = 0;
    static float vc = 0.0f, vs = 0.0f, vcl = 0.0f;
    if (!init) {
        const char* e = getenv("OPENPETE_PORTAL_MASK_BIAS");
        if (e && *e) sscanf(e, "%f,%f,%f", &vc, &vs, &vcl);
        init = 1;
    }
    *c = vc; *s = vs; *cl = vcl;
}

/* (The earlier portal depth-STAMP mechanism — writing plane depth into the
 * aperture so later actor draws depth-fail — was replaced by drawing the
 * portal sky AFTER the ACTOR group with the depth test on: the stamp clipped
 * Spyro mid-entry (PLAYER draws after ACTOR and tested the fake depth too),
 * and its far-sentinel fallback let level mobys reappear once the camera
 * crossed the plane. See the portal draw site in draw_native_scene.) */

static bool draw_native_scene(bool full_extent)
{
    int scene_n; uint32_t scene_bg, scene_fade, scene_freeze; int hud_under = 0, actor_front = 0;
    const scene_tri_t* scene = scene_present_buf(&scene_n, &scene_bg, &scene_fade, &scene_freeze, &hud_under, &actor_front);

    /* S8b: publish this present's interned per-instance material uniforms
     * into the material UBO before any draw_tex_runs override binds them.
     * No-op (and cheap) when no per-instance selector is active. */
    fill_material_ubo();

    /* Load-window census (OPENPETE_LOAD_CENSUS=1): what THIS present actually
     * composes — tri counts per category/blend + the fade/freeze covers. One
     * line per present; correlate with the game-side RE / MOBY-RE lines to
     * attribute transition-window artifacts (who fed what, what covers it). */
    {
        static int s_census = -1;
        if (s_census < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); s_census = (e && *e && *e != '0'); }
        if (s_census) {
            int cat[13] = {0}, blend = 0;
            for (int t = 0; t < scene_n; t++) {
                if (scene[t].blend != 0) blend++;
                else if (scene[t].category >= 0 && scene[t].category <= 12) cat[scene[t].category]++;
            }
            fprintf(stderr, "[census f=%u SCENE] n=%d wor=%d act=%d hud=%d ply=%d ovl=%d pan=%d h3d=%d psky=%d pmsk=%d sky=%d fill=%d blend=%d fade=%08x freeze=%u denv=%08x sbg=%08x\n",
                    s_scene_frame, scene_n, cat[0], cat[1], cat[2], cat[3], cat[4], cat[5], cat[6], cat[7], cat[8],
                    cat[10], cat[11], blend, scene_fade, scene_freeze, s_drawenv_clear, scene_bg);
        }
    }

    /* Background colour = the GAMESTATE draw-env clear (s_drawenv_clear, captured
     * from PutDrawEnv) — the colour the canonical render actually auto-clears the
     * framebuffer to, correct in EVERY state. Preferred over scene_bg
     * (g_Cyclorama.m_BackgroundColor), which equals the clear only on frames that
     * draw the cyclorama and goes stale in cyclorama-less states (e.g. the black
     * level-name flyby: clear is black, g_Cyclorama still holds the old sky). Both
     * are gamestate (derive-from-gamestate principle holds); the draw-env clear is
     * just the more robust source. scene_bg is the fallback (no auto-clear), then
     * the PsyCross readback corner (ab/diff harness / isolated --render-paths). */
    uint32_t bg = s_drawenv_clear ? s_drawenv_clear : scene_bg;
    if (bg == 0) bg = bg_from_ref_corner();

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
        float l, r, t, b;
        cover_rect(full_extent, &l, &r, &t, &b);
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
    /* W2.6-c: the WORLD-opaque group draws through the stencil-MARK variants
     * (identical raster + stencil=1 coverage write) so the FILL layer below can
     * paint only uncovered pixels. Falls back to the plain pipes (and no fill)
     * if the mark/fill pipes didn't come up. */
    const int holefill_on = s_nr.gouraud_mark_pipe && s_nr.scene_tex_mark_pipe
                            && s_nr.gouraud_fill_pipe;
    const struct scene_group groups[6] = {
        { 0, OPENPETE_SCENE_WORLD,
              holefill_on ? s_nr.gouraud_mark_pipe   : s_nr.gouraud_pipe,
              holefill_on ? s_nr.scene_tex_mark_pipe : s_nr.scene_tex_pipe    },
        { 3, -1,                    s_nr.gouraud_sub_pipe,   s_nr.scene_tex_sub_pipe   },
        { 0, OPENPETE_SCENE_ACTOR,  s_nr.gouraud_pipe,       s_nr.scene_tex_pipe       },
        { 0, OPENPETE_SCENE_PLAYER, s_nr.gouraud_pipe,       s_nr.scene_tex_pipe       },
        { 1, -1,                    s_nr.gouraud_blend_pipe, s_nr.scene_tex_blend_pipe },
        { 2, -1,                    s_nr.gouraud_add_pipe,   s_nr.scene_tex_add_pipe   },
    };
    /* ut group 6 = HUD untextured overlay (after ACTOR); 7 = OVERLAY (dead last,
     * letterbox / HUD-OT 2D flat quads, over everything); 8 = HUD_PANEL (alpha,
     * under the HUD text); 9 = HUD3D (in-game 3D icon mobys, DEPTH-TESTED on top
     * after a depth clear). Groups 6/7/8 are depth-test off; 9 is depth-on.
     * 11 = SKY (cyclorama dome — WORLD-like, drawn FIRST with the PLAIN pipe so
     * it never stencil-marks; see OPENPETE_SCENE_SKY); 12 = WORLD_FILL (terrain
     * LP hole-fill, stencil TEST_ZERO, right after the WORLD group). */
    int ut_first[14], ut_count[14];
    for (int gi = 0; gi < 6; gi++) {
        ut_first[gi] = n;
        int gb = groups[gi].blend, gc = groups[gi].gcat;
        for (int t = 0; t < scene_n; t++) {
            if (scene[t].textured || n + 3 > SCENE_VB_VERTS) continue;
            if (scene[t].category == OPENPETE_SCENE_HUD ||
                scene[t].category == OPENPETE_SCENE_HUD3D ||
                scene[t].category == OPENPETE_SCENE_HUD_PANEL) continue;  /* -> HUD passes */
            if (s_suppress_player &&
                scene[t].category == OPENPETE_SCENE_PLAYER) continue;
            int b = scene[t].blend; if (b < 0 || b > 3) b = 0;   /* untextured class */
            if (b != gb) continue;
            if (gb == 0 && gc >= 0 && scene[t].category != gc) continue;
            /* SUBTRACT group = the drop-shadow DECALS only (SHADOW category).
             * Plain subtractive world content (blend 3, any other category —
             * e.g. hypothetical untextured smoke) goes to slot 13 below and
             * draws through the plain subtract pipe, not the decal. */
            if (gb == 3 && scene[t].category != OPENPETE_SCENE_SHADOW) continue;
            /* The SUBTRACT group (gb==3 = the depth-aware Spyro shadow decal)
             * carries its PSX screen coord in u,v so shadow_decal.frag gets the
             * exact per-pixel screen position via a NOPERSPECTIVE varying (no
             * mode-specific affine — identical full-extent and panel). */
            int is_shadow = (gb == 3);
            /* OPENPETE_PLY_VBDBG=1: print PLAYER-group verts in the upper band
             * (stamp forensics — sy<205 excludes Spyro's body). */
            { static int s_pd = -1;
              if (s_pd < 0) { const char* e = getenv("OPENPETE_PLY_VBDBG"); s_pd = (e && *e && *e != '0'); }
              if (s_pd && gi == 3 && scene[t].v[0].sy < 205.0f) {
                  static int s_pc = 0;
                  if (s_pc++ < 40)
                      fprintf(stderr, "[ply-vb] t=%d sxy=(%.0f,%.0f) z=(%.4f,%.4f,%.4f)\n",
                              t, scene[t].v[0].sx, scene[t].v[0].sy,
                              scene[t].v[0].z, scene[t].v[1].z, scene[t].v[2].z);
              } }
            for (int k = 0; k < 3; k++) {
                float cx, cy;
                scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
                rhi_vertex *o = &buf[n + k];
                o->x = cx; o->y = cy; o->z = scene[t].v[k].z;
                o->u = is_shadow ? scene[t].v[k].sx : 0.0f;
                o->v = is_shadow ? scene[t].v[k].sy : 0.0f;
                o->rgba = scene[t].v[k].rgba; o->tex = 0;
            }
            n += 3;
        }
        ut_count[gi] = n - ut_first[gi];
    }
    /* HUD untextured: every HUD-category untextured tri (any blend). The pipe is
     * depth-test OFF by design (the layer must paint over the world uncondition-
     * ally yet stay under the player), so ordering INSIDE the layer is painter's.
     * Submission order is wrong for the near-OT mobys the game ROTATES — the
     * inventory screen's spinning letter/dragon-icon mobys painted their back
     * faces over their front faces. Sort the bucket stably BACK-TO-FRONT by
     * centroid z (the tris carry real view-Z-derived depth), the depth-off
     * analogue of the PSX OT bucket order — finer, per-tri — so a rotated glyph
     * self-occludes correctly at every phase. Stability (z ties break on
     * submission index) keeps every constant-z run — the 2D-UI carries at z=0,
     * flat per-glyph quads — in submission order, i.e. exactly the old draw. */
    ut_first[6] = n;
    {
        static hud_paint_t ho[MAX_SCENE_TRIS];
        int hn = 0;
        for (int t = 0; t < scene_n; t++) {
            if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD) continue;
            ho[hn].z = (scene[t].v[0].z + scene[t].v[1].z + scene[t].v[2].z) * (1.0f/3.0f);
            ho[hn].t = t; hn++;
        }
        qsort(ho, (size_t)hn, sizeof ho[0], hud_paint_cmp);
        for (int i = 0; i < hn && n + 3 <= SCENE_VB_VERTS; i++) {
            int t = ho[i].t;
            for (int k = 0; k < 3; k++) {
                float cx, cy;
                scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
                rhi_vertex *o = &buf[n + k];
                o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
                o->rgba = scene[t].v[k].rgba; o->tex = 0;
            }
            n += 3;
        }
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
    /* HUD_PANEL untextured: the semi-transparent menu dark panel, drawn alpha-blended
     * at the START of the HUD layer (before the HUD text/icons) so the text reads on
     * top. Submission order; depth-test off. */
    ut_first[8] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD_PANEL || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[8] = n - ut_first[8];
    /* HUD3D untextured: the in-game 3D HUD icon mobys (gem/dragon/Spyro-head/key/
     * digits). DEPTH-TESTED (drawn with the opaque gouraud_pipe after a mid-pass
     * depth clear) so their own faces Z-sort — depth-off paints them inside-out.
     * Per-vertex z is the real view-Z depth (openpete_native_depth_from_viewz),
     * same as actors. */
    ut_first[9] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD3D || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[9] = n - ut_first[9];
    /* PORTAL_GUARD untextured: near-band terrain protection redraw (see the
     * category doc). Depth-tested opaque, drawn right after the portal-sky
     * sequence — re-wins its own pixels from the depth-write-off sky. */
    ut_first[10] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_PORTAL_GUARD || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[10] = n - ut_first[10];
    /* SKY (cyclorama dome) + WORLD_FILL (terrain LP hole-fill) untextured
     * buckets (W2.6-c). SKY draws before the group loop with the PLAIN opaque
     * pipe (depth-tested backdrop, no stencil mark); FILL draws right after
     * the WORLD group with the TEST_ZERO pipe. Both untextured by
     * construction (gouraud dome / gouraud LP). */
    ut_first[11] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_SKY || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[11] = n - ut_first[11];
    ut_first[12] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_WORLD_FILL || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[12] = n - ut_first[12];
    /* SUBTRACT-WORLD untextured (slot 13): blend-3 content that is NOT a
     * shadow decal (category != SHADOW; HUD layers excluded — the menu panel
     * has its own blend-3 path). Drawn in the SUBTRACT slot with the plain
     * z-test subtract pipe (neutral bias), never the shadow decal. */
    ut_first[13] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].blend != 3 || n + 3 > SCENE_VB_VERTS) continue;
        int c13 = scene[t].category;
        if (c13 == OPENPETE_SCENE_SHADOW || c13 == OPENPETE_SCENE_HUD ||
            c13 == OPENPETE_SCENE_HUD3D || c13 == OPENPETE_SCENE_HUD_PANEL) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[13] = n - ut_first[13];
    /* PORTAL buckets (W2.6): sky colours (PORTAL_CYCLORAMA) + their stencil
     * aperture masks (PORTAL_MASK), collected per-portal via the feeder's group
     * id. Draw site (after the WORLD group): per group — clear stencil →
     * rasterize the mask invisibly, depth-TESTED against the world (the arch
     * stone / a nearer wall punches it out) → draw the colours depth-FREE,
     * stencil-gated. Groups draw far→near by plane depth so a nearer portal
     * paints over a farther one seen through it; within a group, submission
     * order = the feeder's painter order (flat base → dome). */
    struct portal_run { int group; int first, count; float depth; };
    enum { MAX_PORTAL_RUNS = 32 };
    struct portal_run pmask[MAX_PORTAL_RUNS], psky[MAX_PORTAL_RUNS];
    int pmask_n = 0, psky_n = 0;
    for (int cat = 0; cat < 2; cat++) {   /* pass 0 = masks, pass 1 = sky colours */
        const int want = cat ? OPENPETE_SCENE_PORTAL_CYCLORAMA : OPENPETE_SCENE_PORTAL_MASK;
        struct portal_run *runs = cat ? psky : pmask;
        int *rn = cat ? &psky_n : &pmask_n;
        int cur = -1;   /* current run: a maximal streak of one group id */
        for (int t = 0; t < scene_n; t++) {
            if (scene[t].textured || scene[t].category != want || n + 3 > SCENE_VB_VERTS) continue;
            if (cur < 0 || runs[cur].group != scene[t].group) {
                if (*rn >= MAX_PORTAL_RUNS) continue;   /* drop past cap */
                cur = (*rn)++;
                runs[cur].group = scene[t].group;
                runs[cur].first = n; runs[cur].count = 0;
                runs[cur].depth = scene[t].v[0].z;
            }
            for (int k = 0; k < 3; k++) {
                float cx, cy;
                scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
                rhi_vertex *o = &buf[n + k];
                o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
                o->rgba = scene[t].v[k].rgba; o->tex = 0;
            }
            runs[cur].count += 3;
            n += 3;
        }
    }
    /* Textured tris, bucketed by draw group then (for opaque groups) material-
     * sorted, so the draw loop coalesces same-material runs into ONE vkCmdDraw.
     * Previously each textured triangle was its own vkCmdDraw + vkCmdPushConstants
     * (~3800 draws/present in a busy scene, ×~480/s = the high-FPS bottleneck);
     * material-sorting collapses that ~36× (terrain/mobys share few tpages).
     *   buckets 0..5 = the 6 layered groups (SAME partition as the draw loop)
     *   bucket  6    = HUD 2D textured backdrops (draw_hud_flat_layer)
     * HUD3D-textured and unmatched textured tris are not drawn by anyone (prior
     * behaviour), so they're dropped from the block. Opaque buckets (0,2,3) are
     * material-sorted — depth resolves visibility regardless of draw order, so it
     * is exact. Transparent buckets (1,4,5) and HUD (6) keep submission order
     * (additive/subtractive commute; ABR0 alpha is rare/single-layer; the flat-HUD
     * backdrops want painter's order) and only coalesce already-adjacent runs. */
    int tx_start = n;
    int txg_first[9], txg_count[9], txtri = 0;
    for (int b = 0; b < 9; b++) {
        txg_first[b] = tx_start + txtri * 3;
        int run_start = txtri;
        for (int t = 0; t < scene_n; t++) {
            if (!scene[t].textured) continue;
            if (tx_start + (txtri + 1) * 3 > SCENE_VB_VERTS) break;
            int cat = scene[t].category;
            if (s_suppress_player && cat == OPENPETE_SCENE_PLAYER) continue;
            int belongs;
            if (b == 6) belongs = (cat == OPENPETE_SCENE_HUD);
            else if (b == 7) belongs = (cat == OPENPETE_SCENE_PORTAL_GUARD);
            else if (b == 8) {
                /* SUBTRACT-WORLD (textured): blend-3 that is NOT a shadow
                 * decal — the dragon-fragment smoke puffs (particle sel-2,
                 * ABR 2) and any ABR-2 terrain/moby faces. Drawn in the
                 * SUBTRACT slot with the plain z-test subtract pipe; the
                 * decal path (bucket 1) is shadows only. */
                int bl = scene[t].blend; if (bl < 0 || bl > 3) bl = 0;
                belongs = bl == 3 && cat != OPENPETE_SCENE_SHADOW &&
                          cat != OPENPETE_SCENE_HUD && cat != OPENPETE_SCENE_HUD3D &&
                          cat != OPENPETE_SCENE_PORTAL_GUARD;
            }
            else {
                int bl = scene[t].blend; if (bl < 0 || bl > 3) bl = 0;
                belongs = cat != OPENPETE_SCENE_HUD && cat != OPENPETE_SCENE_HUD3D &&
                          cat != OPENPETE_SCENE_PORTAL_GUARD &&
                          bl == groups[b].blend &&
                          !(groups[b].blend == 0 && groups[b].gcat >= 0 && cat != groups[b].gcat) &&
                          !(groups[b].blend == 3 && cat != OPENPETE_SCENE_SHADOW);
            }
            if (belongs) s_txscene[txtri++] = t;
        }
        if (b == 0 || b == 2 || b == 3) {   /* opaque: material-sort (depth-correct) */
            s_txsort_scene = scene;
            qsort(&s_txscene[run_start], (size_t)(txtri - run_start), sizeof(int), tex_mat_cmp);
        }
        txg_count[b] = (tx_start + txtri * 3) - txg_first[b];
    }
    for (int i = 0; i < txtri; i++) {
        const scene_tri_t *st = &scene[s_txscene[i]];
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(st->v[k].sx, st->v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[tx_start + i * 3 + k];
            o->x = cx; o->y = cy; o->z = st->v[k].z;
            o->u = st->v[k].u; o->v = st->v[k].v;
            o->rgba = st->v[k].rgba; o->tex = 0;
            /* PSX screen coord for the textured shadow decal (location 3); the
             * scene_to_clip INPUT is the PSX screen coord. Harmless for the
             * other textured pipes (their shaders omit location 3). */
            o->sx = st->v[k].sx; o->sy = st->v[k].sy;
        }
    }
    n = tx_start + txtri * 3;

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
        float l, r, t, b;
        cover_rect(full_extent, &l, &r, &t, &b);
        const rhi_vertex tl = { l, t, 0.0f, 0, 0, frgba, 0 };
        const rhi_vertex tr = { r, t, 0.0f, 0, 0, frgba, 0 };
        const rhi_vertex br = { r, b, 0.0f, 0, 0, frgba, 0 };
        const rhi_vertex bl = { l, b, 0.0f, 0, 0, frgba, 0 };
        fade_first = n;
        buf[n+0] = tl; buf[n+1] = tr; buf[n+2] = br;
        buf[n+3] = tl; buf[n+4] = br; buf[n+5] = bl;
        n += 6;
    }

    /* Freeze-frame green overlay backdrop (pause / inventory / flight results):
     * a full-panel quad sampling frozen_tex (the last world frame's native render),
     * desaturated + green-tinted by freeze_overlay_pipe. The tint rides the vertex
     * colour (the game's setRGB0 0x4C,0x80,0x40). UV 0..1 (top-down, like the RT).
     * Drawn FIRST (the backdrop) so the menu UI composites on top. Only when a
     * world frame has been captured; else fall through (clear/bg shows). */
    int freeze_first = -1;
    if ((scene_freeze & (1u << 31)) && s_nr.frozen_valid && s_nr.freeze_overlay_pipe
        && n + 6 <= SCENE_VB_VERTS) {
        uint32_t trgb = scene_freeze & 0xFFFFFFu;     /* rgb in R<<16|G<<8|B */
        uint32_t tr8 = (trgb >> 16) & 0xFFu, tg8 = (trgb >> 8) & 0xFFu, tb8 = trgb & 0xFFu;
        /* Fade-to-black shift (bits 24-30, 0..127) rides the vertex alpha as 0..1; the
         * freeze_overlay shader subtracts it from the desaturated luma (GS_ExitLevel
         * case 0). 0 for the static pause/inventory backdrop -> alpha 0 -> no shift. */
        uint32_t fade7 = (scene_freeze >> 24) & 0x7Fu;
        uint32_t ta8 = (fade7 * 255u + 63u) / 127u;    /* 0..127 -> 0..255 (rounded) */
        uint32_t trgba = tr8 | (tg8 << 8) | (tb8 << 16) | (ta8 << 24);  /* byte order R,G,B,A */
        /* Fill the render rect (cover_rect = the --aspect display rect, fit into the
         * window). frozen_tex is captured at the MATCHING aspect — 4:3 in the default
         * /A-B path, or the Hor+ widescreen view in vk-widescreen (see compose_variant)
         * — so this fills with no stretch and no pillarbox. cover_rect is the --aspect
         * rect fit into the window, so a non-matching window letter/pillarboxes (bars,
         * not stretch); it recomputes each present, tracking live resizes. */
        float l, r, t, b;
        cover_rect(full_extent, &l, &r, &t, &b);
        const rhi_vertex tl = { l, t, 1.0f, 0.0f, 0.0f, trgba, 0 };
        const rhi_vertex tr = { r, t, 1.0f, 1.0f, 0.0f, trgba, 0 };
        const rhi_vertex br = { r, b, 1.0f, 1.0f, 1.0f, trgba, 0 };
        const rhi_vertex bl = { l, b, 1.0f, 0.0f, 1.0f, trgba, 0 };
        freeze_first = n;
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
        /* Scissor to the --aspect render rect: the 4:3 content sub-rect (p0_hx)
         * widened by the Hor+ factor (ws_factor). The revealed side world fills
         * out to this rect; everything beyond is the letterbox/pillarbox bars
         * (left clear → black/bg), so the render respects --aspect instead of
         * stretching to the window. ws_factor==1 → exactly the 4:3 rect. */
        float fhx = s_nr.p0_hx * s_nr.ws_factor;
        int sx = (int)(((s_nr.p0_cx - fhx) * 0.5f + 0.5f) * (float)s_nr.win_w + 0.5f);
        int sw = (int)((fhx) * (float)s_nr.win_w + 0.5f);
        int sy = (int)((0.5f - 0.5f * s_nr.p0_hy) * (float)s_nr.win_h + 0.5f);
        int sh = (int)((s_nr.p0_hy) * (float)s_nr.win_h + 0.5f);
        rhi_set_scissor(s_nr.dev, sx, sy, sw, sh);
    }

    /* Background fill: the panel-rect far-plane quad built at buf[0..6) above.
     * Drawn FIRST so all scene geometry composites over it; fills the cyclorama
     * background colour where the sky dome / terrain don't cover the panel (the
     * PSX draw-env clear). The off-display native_rtt receives this colour from
     * its render-pass CLEAR, but the LIVE window offscreen is cleared to BLACK
     * (so letterbox borders stay black) and then composes the scene over it — so
     * without this draw the live window shows BLACK sky wherever the dome has a
     * gap, even though screenshots (which read native_rtt) look correct. z=1.0
     * (far) + depth-write: the dome (also z=1) overdraws it via LESS_OR_EQUAL,
     * nearer geometry over that. */
    if (bg != 0 && s_nr.gouraud_pipe) {
        rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, 6, 0);
    }

    /* Freeze backdrop FIRST (behind world/UI): the frozen native frame sampled
     * by freeze_overlay_pipe (set 0 = frozen_tex), desaturated + green-tinted. On
     * a freeze frame the world groups are empty (the game stops rendering the
     * world), so this fills the screen and the menu UI composites over it. */
    if (freeze_first >= 0 && s_nr.frozen_tex) {
        rhi_bind_pipeline(s_nr.dev, s_nr.freeze_overlay_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.frozen_tex);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, 6, (uint32_t)freeze_first);
    }

    /* SKY (cyclorama dome) — drawn before the layered groups with the PLAIN
     * opaque pipe: same depth behaviour as its old WORLD membership (z=1.0
     * backdrop, everything nearer wins), but it never stencil-MARKs, so the
     * hole-fill layer can repaint the sky showing through terrain seams
     * (W2.6-c). Skipped on freeze frames like the other gameplay groups. */
    if (freeze_first < 0 && s_nr.gouraud_pipe && ut_count[11] > 0) {
        rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count[11], (uint32_t)ut_first[11]);
    }

    /* Draw the 6 layered groups in order (WORLD-opaque, SUBTRACT shadows,
     * ACTOR-opaque, PLAYER-opaque, ALPHA, ADDITIVE). For each: the batched
     * untextured range, then its textured tris as material-coalesced runs
     * (draw_tex_runs over the pre-bucketed/sorted block — one draw per material
     * run, not per triangle). Opaque groups write depth; the transparent groups
     * (1/4/5) are depth_write_off (test vs opaque, never occlude each other).
     *
     * The HUD/overlay layer (depth-test OFF) is drawn AFTER the ACTOR group (mobys,
     * gi==2) and BEFORE the PLAYER group (Spyro, gi==3): it ignores world depth so
     * it sits on top of the terrain AND regular mobys (the "DEMO MODE" banner /
     * level-intro & "RESCUED <dragon>" text / HUD icons the .s forces to the near OT
     * slot, over other mobys like the rescued dragon), but writes no depth, so the
     * player Spyro drawn next still covers it (Spyro flies in front of the intro
     * text). Matches the PSX moby OT: near-slot UI over other mobys, player on top. */
    for (int gi = 0; gi < 6; gi++) {
        /* Freeze backdrop active (pause / inventory / flight-results): the world is
         * frozen INTO the backdrop (drawn above), and the canonical game render emits
         * no world/actor/player/shadow/transparent geometry while a menu is up — only
         * the HUD menu (text mobys, panel, 3D icons) on top. The high-FPS spine, by
         * contrast, blindly re-extracts the still-in-RAM gameplay actors every present;
         * drawing them here would paint full-colour Spyro/mobys over the green-tinted
         * backdrop (a ghost double-image). Skip the gameplay groups; the HUD layers
         * below still draw, so the animated menu stays smooth at the present rate. */
        if (freeze_first >= 0) continue;
        const struct scene_group *g = &groups[gi];
        /* Dragon-rescue cutscene (actor_front): the ACTOR buckets are skipped
         * at their normal slot and re-drawn depth-isolated after the PLAYER
         * group (the gi==3 block below). */
        const int actor_front_skip = (gi == 2 && actor_front);
        /* SUBTRACT (shadow decal) group: engage the hardware slope-scaled
         * depth bias for these draws (both sub pipes are .depth_bias). This is
         * the W2.5 replacement for the moby-shadow ×0.96 multiplicative view-Z
         * pull, which was distance-PROPORTIONAL (the depth audit's diagnosed
         * dimensional error for a coplanar tie-break); the hardware offset is
         * distance-invariant, slope-aware at grazing angles, and its clamp
         * bounds the total pull far below anything that could reorder a
         * genuine occluder.
         *   Vulkan rule: dynamic state is INVALIDATED by binding any pipeline
         * that has the same state static (every non-decal pipeline here), so
         * the bias must be set AFTER each depth_bias pipeline bind, before its
         * draws — never rely on a value set earlier in the pass. */
        if (g->utex && ut_count[gi] > 0 && !actor_front_skip) {
            if (gi == 1) {
                /* Untextured SUBTRACT = the Spyro silhouette shadow. Preferred
                 * path: the DEPTH-AWARE decal (shadow_decal.frag samples the
                 * shared WORLD-opaque pre-pass depth; per-pixel world-height
                 * wall rejection, no bias). Fallback (pre-pass unavailable this
                 * present / unsupported device): plain depth test + W2.5 bias. */
                if (s_od_ready && s_nr.opaque_depth) {
                    rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_sub_pipe);
                    rhi_bind_depth_sampler(s_nr.dev, s_nr.opaque_depth, 0);
                    shadow_decal_pc pc;
                    pc.col0x = s_shadow_decal.cam_col0[0];
                    pc.col0y = s_shadow_decal.cam_col0[1];
                    pc.col0z = s_shadow_decal.cam_col0[2];
                    pc.col1x = s_shadow_decal.cam_col1[0];
                    pc.col1y = s_shadow_decal.cam_col1[1];
                    pc.col1z = s_shadow_decal.cam_col1[2];
                    pc.col2x = s_shadow_decal.cam_col2[0];
                    pc.col2y = s_shadow_decal.cam_col2[1];
                    pc.col2z = s_shadow_decal.cam_col2[2];
                    pc.cam_pos_up = s_shadow_decal.cam_pos_up;
                    pc.ofx = s_shadow_decal.ofx; pc.ofy = s_shadow_decal.ofy;
                    pc.h = s_shadow_decal.h;
                    pc.ground_up = s_shadow_decal.ground_up;
                    pc.wall_height = s_shadow_decal.wall_height;
                    pc.steep_nup = s_shadow_decal.steep_nup;
                    /* mode: env override for calibration; else height-test when
                     * the game supplied valid params, else draw-all (no mask). */
                    float mode = s_shadow_decal.valid ? 1.0f : 0.0f;
                    const char* mv = getenv("OPENPETE_SHADOW_DECAL_MODE");
                    if (mv && *mv) mode = (float)atof(mv);
                    pc.mode = mode;
                    pc.band = shadow_decal_band();
                    pc.plat_eps = s_shadow_decal.plat_eps;
                    pc.plat_slack = s_shadow_decal.plat_slack;
                    pc.uax = s_od_uax; pc.ubx = s_od_ubx;
                    pc.vay = s_od_vay; pc.vby = s_od_vby;
                    pc.pvw = s_od_vw; pc.pvh = s_od_vh;
                    rhi_push_constants(s_nr.dev, &pc, sizeof pc);
                } else {
                    rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_sub_ztest_pipe);
                    float c, s, cl;
                    shadow_hw_bias(&c, &s, &cl);
                    rhi_set_depth_bias(s_nr.dev, c, cl, s);
                }
            } else {
                rhi_bind_pipeline(s_nr.dev, g->utex);
            }
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, (uint32_t)ut_count[gi], (uint32_t)ut_first[gi]);
        }
        if (txg_count[gi] > 0 && s_nr.vram_tex && g->tex && !actor_front_skip) {
            /* MOBY SHADOWS (gi==1) prefer the DEPTH-AWARE textured decal when the
             * shared WORLD-opaque depth pre-pass is available this present — the
             * same fix Spyro's silhouette shadow got (shadow_decal_tex.frag owns
             * per-pixel wall/steep/platform occlusion; depth-test OFF, so the flat
             * fan never Z-fights the slanted/bumpy ground it drapes). All moby
             * shadows share one shadow-sprite material (g_MobyShadows uv0/uv1), so
             * the whole subtract range draws in one call. Fallback (device without
             * depth sampling / pipe unavailable): the plain depth-tested subtract
             * pipe + hardware bias below. */
            int moby_decal = (gi == 1 && s_od_ready && s_nr.opaque_depth &&
                              s_nr.scene_tex_sub_decal_pipe && moby_shadow_decal_enabled());
            if (moby_decal) {
                int base = (txg_first[1] - tx_start) / 3;
                const scene_tri_t *m0 = &scene[s_txscene[base]];   /* uniform material */
                shadow_decal_tex_pc pc;
                pc.col0x = s_shadow_decal.cam_col0[0];
                pc.col0y = s_shadow_decal.cam_col0[1];
                pc.col0z = s_shadow_decal.cam_col0[2];
                pc.col1x = s_shadow_decal.cam_col1[0];
                pc.col1y = s_shadow_decal.cam_col1[1];
                pc.col1z = s_shadow_decal.cam_col1[2];
                pc.col2x = s_shadow_decal.cam_col2[0];
                pc.col2y = s_shadow_decal.cam_col2[1];
                pc.col2z = s_shadow_decal.cam_col2[2];
                pc.cam_pos_up = s_shadow_decal.cam_pos_up;
                pc.ofx = s_shadow_decal.ofx; pc.ofy = s_shadow_decal.ofy;
                pc.h = s_shadow_decal.h;
                pc.wall_height = s_shadow_decal.wall_height;
                pc.steep_nup = s_shadow_decal.steep_nup;
                /* height+steep test when the feeder supplied valid camera params,
                 * else draw-all (band). Env override shares the Spyro decal knob. */
                float mode = s_shadow_decal.valid ? 1.0f : 0.0f;
                const char* mv = getenv("OPENPETE_SHADOW_DECAL_MODE");
                if (mv && *mv) mode = (float)atof(mv);
                pc.mode = mode;
                pc.band = shadow_decal_band();
                pc.plat_eps = s_shadow_decal.plat_eps;
                pc.plat_slack = s_shadow_decal.plat_slack;
                pc.uax = s_od_uax; pc.ubx = s_od_ubx;
                pc.vay = s_od_vay; pc.vby = s_od_vby;
                pc.pvw = s_od_vw;  pc.pvh = s_od_vh;
                pc.tpage_x = m0->tpage_x; pc.tpage_y = m0->tpage_y;
                pc.clut_x = m0->clut_x;   pc.clut_y = m0->clut_y;
                pc.bpp = m0->bpp;
                rhi_bind_pipeline(s_nr.dev, s_nr.scene_tex_sub_decal_pipe);
                rhi_bind_depth_sampler(s_nr.dev, s_nr.opaque_depth, 0); /* set 0 */
                rhi_bind_vram2(s_nr.dev, s_nr.vram_tex);                /* set 1 */
                rhi_push_constants(s_nr.dev, &pc, sizeof pc);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                rhi_draw(s_nr.dev, (uint32_t)txg_count[1], (uint32_t)txg_first[1]);
            } else {
                rhi_bind_pipeline(s_nr.dev, g->tex);
                if (gi == 1) {
                    float c, s, cl;
                    shadow_hw_bias(&c, &s, &cl);
                    rhi_set_depth_bias(s_nr.dev, c, cl, s);
                }
                rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                /* HD replacement per group family: opaque -> depth-tested cutout
                 * (WORLD's stencil-MARK variant gets the MARK twin so W2.6-c
                 * hole-fill coverage stays intact); alpha/additive -> the
                 * ztest/no-zwrite HD twins. SUBTRACTIVE (gi==1) stays SD — its
                 * draws depend on the caller's dynamic depth-bias state, which
                 * an HD rebind would invalidate. */
                rhi_pipeline *hd =
                    (g->blend == 0) ? (g->tex == s_nr.scene_tex_mark_pipe
                                           ? s_nr.hd_cutout_mark_pipe
                                           : s_nr.hd_cutout_pipe)
                  : (g->blend == 1) ? s_nr.hd_scene_blend_pipe
                  : (g->blend == 2) ? s_nr.hd_scene_add_pipe
                  : NULL;
                /* Material overrides only in the plain opaque groups: the
                 * stencil-marking WORLD variant (holefill) and the blend
                 * groups keep stock shading (scheduling contract). */
                int mat_ok = (gi == 2 || gi == 3) ||
                             (gi == 0 && g->tex == s_nr.scene_tex_pipe);
                draw_tex_runs(txg_first[gi], txg_count[gi], tx_start, scene, g->tex, hd, mat_ok);
            }
        }
        /* WORLD_FILL — terrain LP hole-fill (W2.6-c), right after the WORLD
         * group: depth-tested at TRUE LP depth, gated on stencil==0 so it can
         * only paint pixels the WORLD group's coverage MARK left empty (the
         * LP↔HP T-junction seams — where the sky currently shows through).
         * Must run before the portal sequence (gi==2), which clears stencil. */
        if (gi == 0 && holefill_on && ut_count[12] > 0) {
            rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_fill_pipe);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, (uint32_t)ut_count[12], (uint32_t)ut_first[12]);
        }
        /* SUBTRACT-WORLD — genuinely subtractive content (blend 3, category
         * != SHADOW): the smoke puffs (fragment shatter, flame-kill), ABR-2
         * terrain/moby faces. Drawn LAST of the world-space stack (after the
         * ALPHA/ADDITIVE groups, gi==5) through the PLAIN z-test subtract
         * pipes at NEUTRAL bias:
         *  - NOT the decal path (bucket 1): the ground-height discard ate the
         *    sky-borne puffs and the range-uniform shadow-sprite material
         *    sampled the wrong texture;
         *  - NOT at gi==1 (the first attempt): smoke writes no depth, so the
         *    ACTOR/PLAYER opaques drawn at gi==2/3 PAINTED OVER the already-
         *    subtracted pixels — the burn smoke vanished exactly where og
         *    shows it (on the flamed enemy). The .s renders particles at the
         *    END of the frame; og's smoke darkens the flame glow too, so
         *    after the ADD group is the faithful slot. */
        if (gi == 5) {
            { static int s_swd = -1;
              if (s_swd < 0) { const char* e = getenv("OPENPETE_PLY_VBDBG"); s_swd = (e && *e && *e != '0'); }
              if (s_swd) fprintf(stderr, "[subworld] ut13=%d txg8=%d utpipe=%d txpipe=%d vram=%d\n",
                                 ut_count[13], txg_count[8],
                                 s_nr.gouraud_sub_ztest_pipe != NULL,
                                 s_nr.scene_tex_sub_pipe != NULL, s_nr.vram_tex != NULL); }
            if (ut_count[13] > 0 && s_nr.gouraud_sub_ztest_pipe) {
                rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_sub_ztest_pipe);
                rhi_set_depth_bias(s_nr.dev, 0.0f, 0.0f, 0.0f);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                rhi_draw(s_nr.dev, (uint32_t)ut_count[13], (uint32_t)ut_first[13]);
            }
            if (txg_count[8] > 0 && s_nr.vram_tex && s_nr.scene_tex_sub_pipe) {
                rhi_bind_pipeline(s_nr.dev, s_nr.scene_tex_sub_pipe);
                rhi_set_depth_bias(s_nr.dev, 0.0f, 0.0f, 0.0f);
                rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                /* SD only (like the shadow group): the sub pipes ride the
                 * caller's dynamic depth-bias state, which an HD rebind
                 * would invalidate. */
                draw_tex_runs(txg_first[8], txg_count[8], tx_start, scene,
                              s_nr.scene_tex_sub_pipe, NULL, 0);
            }
        }
        /* Portal-sky previews: drawn AFTER the ACTOR group (gi==2), BEFORE the
         * PLAYER group — the native realization of the PSX sky bucket, which
         * painted over everything OT-behind it:
         *   - the sky depth-TESTS (LEQUAL at the portal-plane z, write OFF)
         *     against world+actor depth, so it repaints over mobys BEHIND the
         *     plane (they drew in the ACTOR group and wrote deeper z) while a
         *     moby IN FRONT of the arch keeps its pixels;
         *   - Spyro (PLAYER, drawn next) is never cut by it — he always sorts
         *     on top, incl. flying/walking through the portal (the earlier
         *     depth-STAMP variant clipped him mid-entry and was replaced by
         *     this repaint ordering);
         *   - the stencil mask confines everything to the aperture footprint
         *     (side-poke impossible), and intra-portal compositing stays
         *     painter order (backstops fed before the dome). */
        if (gi == 2 && s_nr.gouraud_noz_pipe && s_nr.portal_mask_pipe && psky_n > 0) {
            /* Per-portal stencil sequence, far→near by plane depth (a nearer
             * portal seen in front of a farther one paints last): clear
             * stencil → mask (invisible, depth-tested) → sky colours
             * (depth-tested, stencil-gated). A sky run without a matching
             * mask uses its own footprint as the mask. */
            int order[MAX_PORTAL_RUNS];
            for (int oi = 0; oi < psky_n; oi++) order[oi] = oi;
            for (int oi = 1; oi < psky_n; oi++) {          /* depth descending */
                int ov = order[oi], oj = oi - 1;
                while (oj >= 0 && psky[order[oj]].depth < psky[ov].depth) {
                    order[oj + 1] = order[oj]; oj--;
                }
                order[oj + 1] = ov;
            }
            char mused[MAX_PORTAL_RUNS] = {0};
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            for (int oi = 0; oi < psky_n; oi++) {
                const struct portal_run *sk = &psky[order[oi]];
                const struct portal_run *mk = NULL;
                for (int mj = 0; mj < pmask_n; mj++)
                    if (!mused[mj] && pmask[mj].group == sk->group) { mk = &pmask[mj]; mused[mj] = 1; break; }
                int mf = mk ? mk->first : sk->first;
                int mc = mk ? mk->count : sk->count;
                if (sk->count <= 0 || mc <= 0) continue;
                rhi_clear_stencil(s_nr.dev);
                rhi_bind_pipeline(s_nr.dev, s_nr.portal_mask_pipe);
                /* dynamic bias must be (re)set after THIS bind (see the note
                 * at the shadow group). Default neutral — the tie-break margin
                 * is already baked into the mask z (the feeder's world-unit
                 * PORTAL_SKY_EPS_VZ pull); OPENPETE_PORTAL_MASK_BIAS adds the
                 * optional close-range pull (see portal_mask_bias). NB a pull
                 * here moves only the MASK — the sky colours depth-test
                 * unbiased, so it cannot widen sky coverage on its own. */
                {
                    float c, s, cl;
                    portal_mask_bias(&c, &s, &cl);
                    rhi_set_depth_bias(s_nr.dev, c, cl, s);
                }
                rhi_draw(s_nr.dev, (uint32_t)mc, (uint32_t)mf);
                rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_noz_pipe);
                rhi_draw(s_nr.dev, (uint32_t)sk->count, (uint32_t)sk->first);
            }
            /* PORTAL_GUARD: near-band terrain protection redraw — the PSX
             * mask-bit mirror (see the category doc in the header). Drawn
             * once after ALL portal groups; the sky wrote no depth, so the
             * guard's normal LEQUAL re-wins exactly its own pixels. */
            if (ut_count[10] > 0 && s_nr.gouraud_pipe) {
                rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_pipe);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                rhi_draw(s_nr.dev, (uint32_t)ut_count[10], (uint32_t)ut_first[10]);
            }
            if (txg_count[7] > 0 && s_nr.vram_tex && s_nr.scene_tex_pipe) {
                rhi_bind_pipeline(s_nr.dev, s_nr.scene_tex_pipe);
                rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                draw_tex_runs(txg_first[7], txg_count[7], tx_start, scene, s_nr.scene_tex_pipe,
                              s_nr.hd_cutout_pipe, 0);
            }
        }
        /* GS_LevelTransition flyby ONLY (hud_under): draw the flat HUD layer here —
         * after the ACTOR group (gi==2) but BEFORE the PLAYER group (gi==3) — so the
         * fly-in Spyro draws in front of the revealed "THE ADVENTURE BEGINS" text.
         * In every other state the flat HUD draws OVER the player (after the loop). */
        if (gi == 2 && hud_under)
            draw_hud_flat_layer(scene, scene_n, ut_first, ut_count, tx_start, txg_first[6], txg_count[6]);
        /* Dragon-rescue cutscene (actor_front): draw the ACTOR buckets here —
         * after world/portal/player, before the ALPHA/ADDITIVE groups — behind
         * a mid-pass depth CLEAR (the HUD3D trick). The cutscene actors (the
         * rescue dragon / cutscene Spyro / crystal fragments — the .s cutscene
         * draw list holds ONLY these) Z-sort among their own faces but are
         * never occluded by terrain: the PSX's coarse OT bucketing painted
         * them over walls they interpenetrate (the spread-wing close-ups),
         * which true per-pixel Z would amputate. The transparent groups draw
         * AFTER, testing the actors' fresh depth, so rescue sparkles still
         * sort against the dragon; their terrain-depth occlusion is lost for
         * these frames — invisible in practice, since the cutscene draw list
         * has no level mobys (no torch glows etc. behind walls). */
        if (gi == 3 && actor_front) {
            rhi_clear_depth(s_nr.dev);
            if (s_nr.gouraud_pipe && ut_count[2] > 0) {
                rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_pipe);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                rhi_draw(s_nr.dev, (uint32_t)ut_count[2], (uint32_t)ut_first[2]);
            }
            if (txg_count[2] > 0 && s_nr.vram_tex && s_nr.scene_tex_pipe) {
                rhi_bind_pipeline(s_nr.dev, s_nr.scene_tex_pipe);
                rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                draw_tex_runs(txg_first[2], txg_count[2], tx_start, scene,
                              s_nr.scene_tex_pipe, s_nr.hd_cutout_pipe, 0);
            }
        }
    }

    /* Default: the flat 2D HUD/text draws OVER the whole scene incl. the player
     * (Spyro behind the HUD/text). Only the GS_LevelTransition flyby puts it under
     * the player (handled at gi==2 above). */
    if (!hud_under)
        draw_hud_flat_layer(scene, scene_n, ut_first, ut_count, tx_start, txg_first[6], txg_count[6]);

    /* HUD3D layer — the in-game 3D HUD icon mobys (gem/dragon/Spyro-head/key/
     * digits). They are real 3D models, so depth-off (the flat HUD layer) paints
     * their own faces inside-out. Draw them LAST, on top of the whole scene, but
     * with the depth buffer CLEARED first (rhi_clear_depth) so they Z-sort among
     * their own faces (depth-test ON via the opaque gouraud_pipe) while never being
     * occluded by world/actor/player geometry. Their vertex positions already match
     * the .s to <1px; this only fixes their internal depth ordering. */
    if (s_nr.gouraud_pipe && ut_count[9] > 0) {
        rhi_clear_depth(s_nr.dev);
        rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count[9], (uint32_t)ut_first[9]);
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
        /* Mode 2 uses gouraud_sub_FADE_pipe (plain subtractive, depth-off) —
         * NOT gouraud_sub_pipe, which is now the depth-aware shadow decal (it
         * would sample opaque_depth and height-discard, breaking the fade). */
        rhi_pipeline* fp = (fade_mode == 2) ? s_nr.gouraud_sub_fade_pipe
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

/* ---- native FPS overlay (vk live window) --------------------------------
 * The legacy GL FPS counter (fps_overlay.cpp) stamps PsyCross's hidden window —
 * invisible in vk, and in pure-vk PsyCross never even presents. This is its
 * native equivalent: the FPS counter drawn with Spyro's OWN 3D HUD text mobys
 * (the gold extruded glyph models), built game-side by text_moby_native and
 * pushed via openpete_native_set_overlay_glyphs. Pure display: drawn only on the
 * LIVE vk present (not screenshots, ab, or diff, so the comparison images stay
 * clean). Toggle at runtime via the SHOW FPS row in the settings menu (O), or
 * start off with --show-fps 0. */
static int s_fps_overlay  = 1;   /* enabled (default on) */
static int s_overlay_live = 0;   /* 1 only during the live present's compose_variant */
static uint64_t s_fps_win_start = 0;  /* rolling 0.5s FPS window */
static int      s_fps_frames    = 0;
static int      s_fps_value     = 0;

static int s_fps_in_record_pref = -1;  /* --record-fps-overlay: -1 unset, 0/1 explicit */
void openpete_native_set_fps_in_record(int pref) { s_fps_in_record_pref = pref; }

void openpete_native_set_fps_overlay(int on) { s_fps_overlay = on ? 1 : 0; }
int  openpete_native_fps_overlay_enabled(void) { return s_fps_overlay; }
int  openpete_native_fps_value(void) { return s_fps_value; }

/* Live presentation setters (in-game settings menu). Display-only: they touch
 * only s_nr (renderer state), consumed by the next native present; never
 * gameplay RAM. See notes/plans/openpete-ingame-settings-menu.md. */
void openpete_native_set_aspect(float w_over_h) {
    s_nr.aspect_wh = (w_over_h > 0.0f) ? w_over_h : 0.0f;
    /* update_layout()'s change-cache keys on window/ref/disp sizes, NOT aspect,
     * so force it to recompute the fitted panel rect on the next present. */
    s_nr.layout_win_w = -1;
    OPL_INFO_F("native", "aspect -> %.4f (%s)", s_nr.aspect_wh,
               s_nr.aspect_wh > 0.0f ? "Hor+ widen" : "4:3");
}

void openpete_native_set_ssaa(int factor) {
    if (factor < 1) factor = 1; else if (factor > 32) factor = 32;
    s_nr.ssaa = factor;   /* ensure_ss_rtt() reallocs the scene RT on the next present */
    OPL_INFO_F("native", "ssaa -> %dx (effective factor capped to GPU max next present)", factor);
}

void openpete_native_set_vsync(int on) {
    if (s_nr.dev) rhi_set_vsync(s_nr.dev, on ? 1 : 0);   /* swapchain rebuilds next frame */
    OPL_INFO_F("native", "vsync -> %s", on ? "ON (FIFO)" : "OFF (immediate/mailbox)");
}

void openpete_native_set_vertex_precision(int integer_snap) {
    s_vertex_snap = integer_snap ? 1 : 0;   /* consumed by the next add_tri_grouped feed */
    OPL_INFO_F("native", "vertex-precision -> %s",
               s_vertex_snap ? "integer (PSX wobble)" : "subpixel (smooth)");
}

/* Overlay geometry built from Spyro's own 3D HUD text mobys (see
 * openpete_native_render.h). The game-side emitter (text_moby_native) pushes a
 * triangle list each frame; draw_fps_overlay draws it on the live present, or
 * nothing when empty (the game side caches the glyph models, so this is empty
 * only before the font has ever loaded — there is no bitmap fallback). Currently
 * the FPS counter; reusable for other text overlays. */
static openpete_hud_vert s_overlay_glyph_verts[OVL_MAX_VERTS];
static int               s_overlay_glyph_n = 0;
void openpete_native_set_overlay_glyphs(const openpete_hud_vert *verts, int nverts) {
    if (nverts < 0) nverts = 0;
    if (nverts > OVL_MAX_VERTS) nverts = OVL_MAX_VERTS;
    if (nverts > 0 && verts) memcpy(s_overlay_glyph_verts, verts, (size_t)nverts * sizeof(verts[0]));
    s_overlay_glyph_n = nverts;
}

/* Mod draw-service overlay geometry (core/mods/mod_draw.c): rects + text
 * glyphs in the same overlay space as the FPS overlay, pushed once per
 * present by openpete_mods_present_flush. Drawn by draw_mod_overlay on the
 * vk variant, deliberately NOT s_overlay_live-gated so --record videos
 * include it (a HUD mod's drawing is content, not chrome). Empirical note:
 * --screenshot PNGs read the scene-only reference target and do NOT show
 * overlay layers (verified M6) — visual proof of mod HUD = live window or
 * a --record clip. */
static openpete_hud_vert s_mod_overlay_verts[MOD_OVL_MAX_VERTS];
static int               s_mod_overlay_n = 0;
void openpete_native_set_mod_overlay(const openpete_hud_vert *verts, int nverts) {
    if (nverts < 0) nverts = 0;
    if (nverts > MOD_OVL_MAX_VERTS) nverts = MOD_OVL_MAX_VERTS;
    if (nverts > 0 && verts) memcpy(s_mod_overlay_verts, verts, (size_t)nverts * sizeof(verts[0]));
    s_mod_overlay_n = nverts;
}

/* Window aspect (w/h) for mod HUD layout in overlay space: x spans
 * [0, aspect], y spans [0, 1]. Falls back to 4:3 pre-init. */
float openpete_native_overlay_aspect(void) {
    if (s_nr.win_w > 0 && s_nr.win_h > 0)
        return (float)s_nr.win_w / (float)s_nr.win_h;
    return 4.0f / 3.0f;
}

/* In-game menu glyph geometry (labels/values/title), pushed each frame by the
 * game-side builder. Drawn by draw_menu_overlay over the panel. */
static openpete_hud_vert s_menu_glyph_verts[MENU_GLYPH_MAX];
static int               s_menu_glyph_n = 0;
void openpete_native_set_menu_glyphs(const openpete_hud_vert *verts, int nverts) {
    if (nverts < 0) nverts = 0;
    if (nverts > MENU_GLYPH_MAX) nverts = MENU_GLYPH_MAX;
    if (nverts > 0 && verts) memcpy(s_menu_glyph_verts, verts, (size_t)nverts * sizeof(verts[0]));
    s_menu_glyph_n = nverts;
}

/* Draw the overlay built from Spyro's own 3D HUD text mobys, fed each frame by
 * the game side (text_moby_native -> openpete_native_set_overlay_glyphs). Verts
 * arrive in window-height-fraction overlay space (origin top-left, +y down) with
 * a depth in [0,1); map to NDC (x via win_h so glyphs stay square) and draw
 * depth-tested through the opaque gouraud pipe after a local depth clear, so the
 * glyphs z-sort among their own faces yet sit on top of the whole frame — the
 * same isolation trick as the in-game HUD3D pass. Returns true if it drew. */
static bool draw_overlay_glyphs_3d(float ax, float ay, float ox) {
    if (s_overlay_glyph_n < 3 || !s_nr.gouraud_pipe || !s_nr.overlay_vb) return false;
    static rhi_vertex buf[OVL_MAX_VERTS];
    int n = 0;
    for (int i = 0; i < s_overlay_glyph_n && n < OVL_MAX_VERTS; i++) {
        const openpete_hud_vert *v = &s_overlay_glyph_verts[i];
        /* Glyph coords are win_h-relative; ax/ay map them into the target's NDC.
         * Offscreen 1×: ax=2·win_h/win_w, ay=2. Overlay SS: ax=2/OVL_REGION_W,
         * ay=2/OVL_REGION_H — both keep glyphs square (ax/ay carry the same
         * win_h scale through the target's pixel dims). ox shifts the origin in
         * overlay-x (win_h units) — the 1× fallback path uses it to sit right of
         * the open menu; the SS prepass passes 0 (the composite quad moves). */
        buf[n].x = (v->x + ox) * ax - 1.0f;
        buf[n].y = 1.0f - v->y * ay;
        buf[n].z = v->z; buf[n].u = 0.0f; buf[n].v = 0.0f;
        buf[n].rgba = v->rgba; buf[n].tex = 0;
        n++;
    }
    n -= n % 3;
    if (n <= 0) return false;
    rhi_update_buffer(s_nr.overlay_vb, buf, (size_t)n * sizeof(rhi_vertex));
    rhi_reset_scissor(s_nr.dev);
    rhi_clear_depth(s_nr.dev);
    rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_pipe);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.overlay_vb);
    rhi_draw(s_nr.dev, (uint32_t)n, 0);
    return true;
}

/* Draw the mod draw-service overlay (rects + text glyphs) into the currently
 * open offscreen pass. Same overlay space + NDC mapping + depth-isolation
 * trick as draw_overlay_glyphs_3d, own vertex buffer, 1× direct draw (no
 * supersample pre-pass in v1). Runs in screenshot/record composes too — mod
 * HUD is content, not chrome. */
static void draw_mod_overlay(void) {
    if (s_mod_overlay_n < 3 || !s_nr.gouraud_pipe || !s_nr.mod_ovl_vb) return;
    if (s_nr.win_w <= 0 || s_nr.win_h <= 0) return;
    static rhi_vertex buf[MOD_OVL_MAX_VERTS];
    const float ax = 2.0f * (float)s_nr.win_h / (float)s_nr.win_w;
    const float ay = 2.0f;
    int n = 0;
    for (int i = 0; i < s_mod_overlay_n && n < MOD_OVL_MAX_VERTS; i++) {
        const openpete_hud_vert *v = &s_mod_overlay_verts[i];
        buf[n].x = v->x * ax - 1.0f;
        buf[n].y = 1.0f - v->y * ay;
        buf[n].z = v->z; buf[n].u = 0.0f; buf[n].v = 0.0f;
        buf[n].rgba = v->rgba; buf[n].tex = 0;
        n++;
    }
    n -= n % 3;
    if (n <= 0) return;
    rhi_update_buffer(s_nr.mod_ovl_vb, buf, (size_t)n * sizeof(rhi_vertex));
    rhi_reset_scissor(s_nr.dev);
    rhi_clear_depth(s_nr.dev);
    rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_pipe);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.mod_ovl_vb);
    rhi_draw(s_nr.dev, (uint32_t)n, 0);
}

/* True when the FPS overlay should render this frame: glyphs built, enabled, and
 * not suppressed by the record gating (kept out of recordings unless
 * OPENPETE_FPS_IN_RECORD lets the live look be captured for verification). */
static bool fps_overlay_should_draw(void) {
    if (!s_fps_overlay || s_nr.win_w <= 0 || s_nr.win_h <= 0 || s_overlay_glyph_n < 3) return false;
    if (openpete_record_active()) {
        static int s_in_rec = -1;
        if (s_in_rec < 0) {
            if (s_fps_in_record_pref >= 0) {
                s_in_rec = s_fps_in_record_pref;             /* --record-fps-overlay wins */
            } else {
                const char *e = getenv("OPENPETE_FPS_IN_RECORD");  /* env fallback */
                s_in_rec = (e && *e && *e != '0') ? 1 : 0;
            }
        }
        if (!s_in_rec) return false;
    }
    return true;
}

/* True when the 4× supersample path is usable (all resources up). */
static bool ovl_ss_ready(void) {
    return s_nr.ovl_resolve_pipe && s_nr.ovl_quad_vb && s_nr.ovl_ss;
}

/* Overlay-x position (win_h units) for the FPS counter's region: flush top-left
 * normally, but aligned to the settings-menu panel's right edge while it's open
 * so the two don't overlap. The offset is exactly panel_x1 (no extra gap): the
 * region's left edge then sits on the panel edge just as it sits on the window's
 * left edge when closed, so the FPS text's own left bearing reproduces the SAME
 * visual gap in both cases. Only the on-screen placement moves — the glyphs (and
 * the SS target they render into) are unchanged. */
static float fps_overlay_ox(void) {
    if (!openpete_menu_is_open()) return 0.0f;
    openpete_menu_layout_t L; openpete_menu_get_layout(&L);
    return L.panel_x1;
}

/* (Re)create ovl_ss at OVL_SS_FACTOR× the overlay region and rebuild the
 * composite quad on a window resize. Creates GPU resources / waits idle — call
 * OUTSIDE a frame (alongside ensure_ss_rtt). No-op if the SS pipeline/quad
 * failed at init (draw_fps_overlay then falls back to the 1× path). */
static void ensure_ovl_ss(void) {
    if (!s_nr.ovl_resolve_pipe || !s_nr.ovl_quad_vb || s_nr.win_w <= 0 || s_nr.win_h <= 0) return;
    int rw = (int)(OVL_REGION_W * (float)s_nr.win_h + 0.5f);
    int rh = (int)(OVL_REGION_H * (float)s_nr.win_h + 0.5f);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    int want_w = rw * OVL_SS_FACTOR, want_h = rh * OVL_SS_FACTOR;
    uint32_t maxdim = rhi_max_image_dimension(s_nr.dev);
    if ((uint32_t)want_w > maxdim) want_w = (int)maxdim;
    if ((uint32_t)want_h > maxdim) want_h = (int)maxdim;
    if (!s_nr.ovl_ss || s_nr.ovl_ss_w != want_w || s_nr.ovl_ss_h != want_h) {
        rhi_wait_idle(s_nr.dev);
        if (s_nr.ovl_ss) { rhi_destroy_texture(s_nr.dev, s_nr.ovl_ss); s_nr.ovl_ss = NULL; }
        s_nr.ovl_ss = rhi_create_render_target(s_nr.dev, (uint32_t)want_w, (uint32_t)want_h);
        s_nr.ovl_ss_w = want_w; s_nr.ovl_ss_h = want_h;
        if (s_nr.ovl_ss)
            OPL_INFO_F("native", "FPS overlay SS target %dx%d (%d×)", want_w, want_h, OVL_SS_FACTOR);
    }
    /* Composite quad — the overlay's screen rect in offscreen NDC. Left edge sits
     * at overlay-x = want_ox (0 = flush top-left; shifted right of the panel while
     * the menu is open, see fps_overlay_ox). Rect width is win_h-relative (glyph x
     * uses win_h), so its NDC width scales by win_h/win_w; UV 0..1 maps the whole
     * SS target (v0 top, top-down). Rebuilt on resize OR when the offset changes
     * (menu open/close) — never per-frame, so the wait_idle stays rare. */
    float want_ox = fps_overlay_ox();
    if (s_nr.ovl_quad_win_w != s_nr.win_w || s_nr.ovl_quad_win_h != s_nr.win_h ||
        s_nr.ovl_quad_ox != want_ox) {
        float ax = 2.0f * (float)s_nr.win_h / (float)s_nr.win_w;
        float rw_ndc = OVL_REGION_W * ax;
        float rh_ndc = OVL_REGION_H * 2.0f;
        float nx_left = want_ox * ax - 1.0f;
        rhi_vertex q[6];
        build_panel(q, nx_left + rw_ndc * 0.5f, 1.0f - rh_ndc * 0.5f,
                    rw_ndc * 0.5f, rh_ndc * 0.5f, 0.0f, 0.0f, 1.0f, 1.0f);
        /* Single (non-ring) VB rewritten only on resize / offset change: an
         * in-flight frame may still read it — fully sync first (like panels_vb). */
        rhi_wait_idle(s_nr.dev);
        rhi_update_buffer(s_nr.ovl_quad_vb, q, sizeof(q));
        s_nr.ovl_quad_win_w = s_nr.win_w; s_nr.ovl_quad_win_h = s_nr.win_h;
        s_nr.ovl_quad_ox = want_ox;
    }
}

/* Pre-pass: render the FPS glyphs at OVL_SS_FACTOR× into ovl_ss (cleared
 * transparent). Its own render-target pass, so call it BEFORE the offscreen pass
 * opens (no nested render passes). No-op unless the SS path is ready. */
static void prepare_fps_overlay_ss(void) {
    if (!ovl_ss_ready() || !fps_overlay_should_draw()) return;
    rhi_begin_render_target(s_nr.dev, s_nr.ovl_ss, 0.0f, 0.0f, 0.0f, 0.0f);  /* transparent */
    draw_overlay_glyphs_3d(2.0f / OVL_REGION_W, 2.0f / OVL_REGION_H, 0.0f);
    rhi_end_render_target(s_nr.dev, s_nr.ovl_ss);
}

/* Composite the FPS overlay into the currently-open offscreen pass. SS path:
 * box-downsample + straight-alpha-blend the ovl_ss the pre-pass built. Fallback:
 * draw the glyphs directly at 1× (the original crisp-but-aliased path). */
static void draw_fps_overlay(void) {
    if (!fps_overlay_should_draw()) return;
    if (ovl_ss_ready()) {
        int factor = OVL_SS_FACTOR;
        rhi_reset_scissor(s_nr.dev);
        rhi_bind_pipeline(s_nr.dev, s_nr.ovl_resolve_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.ovl_ss);
        rhi_push_constants(s_nr.dev, &factor, sizeof(factor));
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.ovl_quad_vb);
        rhi_draw(s_nr.dev, 6, 0);
    } else {
        draw_overlay_glyphs_3d(2.0f * (float)s_nr.win_h / (float)s_nr.win_w, 2.0f,
                               fps_overlay_ox());
    }
}

/* --- in-game settings menu overlay -----------------------------------------
 * Drawn on the live vk present's resolved 1× offscreen, same overlay space and
 * gating as the FPS overlay. Reproduces the PSX pause-menu frame: a subtractive
 * dark panel (matching the game's ABR-2 box), an opaque gold border, and an
 * alpha row highlight on the cursor. Glyph text lands in a later step. One
 * upload to menu_vb, drawn as blend-mode spans (single upload → all spans read
 * consistent data from the per-frame ring slot). */

/* Push an overlay-space rect (x in window-height units, y in [0,1], origin
 * top-left) as 2 tris mapped into the offscreen's NDC (same as the FPS overlay's
 * 1× path: ax=2·win_h/win_w, ay=2). Colour byte order is R,G,B,A. */
static int menu_push_quad(rhi_vertex* buf, int n, float ax, float ay,
                          float x0, float y0, float x1, float y1, uint32_t rgba) {
    if (n + 6 > MENU_MAX_VERTS) return n;
    const float nx0 = x0 * ax - 1.0f, nx1 = x1 * ax - 1.0f;
    const float ny0 = 1.0f - y0 * ay, ny1 = 1.0f - y1 * ay;
    const rhi_vertex tl = { nx0, ny0, 0.0f, 0,0, rgba, 0, 0,0 };
    const rhi_vertex tr = { nx1, ny0, 0.0f, 0,0, rgba, 0, 0,0 };
    const rhi_vertex bl = { nx0, ny1, 0.0f, 0,0, rgba, 0, 0,0 };
    const rhi_vertex br = { nx1, ny1, 0.0f, 0,0, rgba, 0, 0,0 };
    buf[n++] = tl; buf[n++] = tr; buf[n++] = bl;
    buf[n++] = tr; buf[n++] = br; buf[n++] = bl;
    return n;
}

/* Build + draw the menu glyph/border layer (s_menu_glyph_verts) into the current
 * render pass, mapped overlay→NDC by (ax,ay): x → x*ax-1, y → 1-y*ay, with a
 * local depth clear so the 3D glyph faces z-sort among themselves on top of
 * whatever is already there. The panel is flush at overlay origin (0,0), so the
 * SAME map serves both the 1× offscreen (ax=2·win_h/win_w, ay=2) and the menu_ss
 * supersample prepass (ax=2/panel_w, ay=2/panel_h → the panel fills the target). */
static void draw_menu_glyphs_3d(float ax, float ay) {
    if (s_menu_glyph_n < 3 || !s_nr.menu_glyph_vb || !s_nr.gouraud_pipe) return;
    static rhi_vertex gbuf[MENU_GLYPH_MAX];
    int gn = 0;
    for (int i = 0; i < s_menu_glyph_n && gn < MENU_GLYPH_MAX; i++) {
        const openpete_hud_vert *v = &s_menu_glyph_verts[i];
        gbuf[gn].x = v->x * ax - 1.0f;
        gbuf[gn].y = 1.0f - v->y * ay;
        gbuf[gn].z = v->z; gbuf[gn].u = 0.0f; gbuf[gn].v = 0.0f;
        gbuf[gn].rgba = v->rgba; gbuf[gn].tex = 0; gbuf[gn].sx = 0.0f; gbuf[gn].sy = 0.0f;
        gn++;
    }
    gn -= gn % 3;
    if (gn <= 0) return;
    rhi_update_buffer(s_nr.menu_glyph_vb, gbuf, (size_t)gn * sizeof(rhi_vertex));
    rhi_reset_scissor(s_nr.dev);
    rhi_clear_depth(s_nr.dev);
    rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_pipe);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.menu_glyph_vb);
    rhi_draw(s_nr.dev, (uint32_t)gn, 0);
}

/* True when the menu-glyph supersample path is usable (all resources up). */
static bool menu_ss_ready(void) {
    return s_nr.ovl_resolve_pipe && s_nr.menu_quad_vb && s_nr.menu_ss;
}

/* (Re)create menu_ss at MENU_SS_FACTOR× the PANEL rect + rebuild the composite
 * quad on resize. Lazy: only allocates once the menu is actually open (the target
 * is large), then keeps it. Creates GPU resources / waits idle — call OUTSIDE a
 * frame (alongside ensure_ovl_ss). No-op if the SS pipe/quad failed at init. */
static void ensure_menu_ss(void) {
    if (!s_nr.ovl_resolve_pipe || !s_nr.menu_quad_vb || s_nr.win_w <= 0 || s_nr.win_h <= 0) return;
    if (!openpete_menu_is_open()) return;   /* don't allocate the big target until needed */
    openpete_menu_layout_t L; openpete_menu_get_layout(&L);
    const float pw = L.panel_x1 - L.panel_x0, ph = L.panel_y1 - L.panel_y0;
    int rw = (int)(pw * (float)s_nr.win_h + 0.5f);
    int rh = (int)(ph * (float)s_nr.win_h + 0.5f);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    int want_w = rw * MENU_SS_FACTOR, want_h = rh * MENU_SS_FACTOR;
    uint32_t maxdim = rhi_max_image_dimension(s_nr.dev);
    if ((uint32_t)want_w > maxdim) want_w = (int)maxdim;
    if ((uint32_t)want_h > maxdim) want_h = (int)maxdim;
    if (!s_nr.menu_ss || s_nr.menu_ss_w != want_w || s_nr.menu_ss_h != want_h) {
        rhi_wait_idle(s_nr.dev);
        if (s_nr.menu_ss) { rhi_destroy_texture(s_nr.dev, s_nr.menu_ss); s_nr.menu_ss = NULL; }
        s_nr.menu_ss = rhi_create_render_target(s_nr.dev, (uint32_t)want_w, (uint32_t)want_h);
        s_nr.menu_ss_w = want_w; s_nr.menu_ss_h = want_h;
        if (s_nr.menu_ss)
            OPL_INFO_F("native", "settings-menu SS target %dx%d (%d×)", want_w, want_h, MENU_SS_FACTOR);
    }
    /* Composite quad — the panel rect (flush top-left) in offscreen NDC; UV 0..1
     * over the whole SS target (v0 top, top-down). Same construction as ovl_quad. */
    if (s_nr.menu_quad_win_w != s_nr.win_w || s_nr.menu_quad_win_h != s_nr.win_h) {
        float rw_ndc = pw * (float)s_nr.win_h / (float)s_nr.win_w * 2.0f;
        float rh_ndc = ph * 2.0f;
        rhi_vertex q[6];
        build_panel(q, -1.0f + rw_ndc * 0.5f, 1.0f - rh_ndc * 0.5f,
                    rw_ndc * 0.5f, rh_ndc * 0.5f, 0.0f, 0.0f, 1.0f, 1.0f);
        rhi_wait_idle(s_nr.dev);
        rhi_update_buffer(s_nr.menu_quad_vb, q, sizeof(q));
        s_nr.menu_quad_win_w = s_nr.win_w; s_nr.menu_quad_win_h = s_nr.win_h;
    }
}

/* Pre-pass: render the menu glyph/border layer at MENU_SS_FACTOR× into menu_ss
 * (cleared transparent). Its own render pass, so call it BEFORE the offscreen
 * pass opens. No-op unless the menu is open and the SS path is ready. */
static void prepare_menu_ss(void) {
    if (!menu_ss_ready() || !openpete_menu_is_open() || s_menu_glyph_n < 3) return;
    openpete_menu_layout_t L; openpete_menu_get_layout(&L);
    const float pw = L.panel_x1 - L.panel_x0, ph = L.panel_y1 - L.panel_y0;
    rhi_begin_render_target(s_nr.dev, s_nr.menu_ss, 0.0f, 0.0f, 0.0f, 0.0f);  /* transparent */
    draw_menu_glyphs_3d(2.0f / pw, 2.0f / ph);
    rhi_end_render_target(s_nr.dev, s_nr.menu_ss);
}

static void draw_menu_overlay(void) {
    if (!openpete_menu_is_open() || !s_nr.menu_vb || s_nr.win_w <= 0 || s_nr.win_h <= 0)
        return;
    openpete_menu_layout_t L;
    openpete_menu_get_layout(&L);
    const float ax = 2.0f * (float)s_nr.win_h / (float)s_nr.win_w;
    const float ay = 2.0f;

    static rhi_vertex buf[MENU_MAX_VERTS];
    int n = 0;

    /* Panel — subtractive gray RGB(112,112,112), the exact colour Spyro's own
     * dialog/message box uses (func_8001860C setRGB0 0x70,0x70,0x70 under ABR
     * mode 2 B-F; verified live as the Magic Crafters balloonist box: a cat=5
     * HUD_PANEL bl=3 tri, rgba=FF707070). The sub pipe ignores alpha. */
    const int sub0 = n;
    n = menu_push_quad(buf, n, ax, ay, L.panel_x0, L.panel_y0, L.panel_x1, L.panel_y1, 0xFF707070u);
    const int sub_n = n - sub0;

    /* No selection bar: the selected row is marked by its per-glyph yaw wobble
     * (like the game's pause menu), so there is no highlight quad. The gold
     * border is built game-side (text_moby_native_menu / emit_menu_border) as a
     * lit frame and arrives through the depth-isolated glyph pass below. */

    if (n <= 0) return;
    rhi_update_buffer(s_nr.menu_vb, buf, (size_t)n * sizeof(rhi_vertex));
    rhi_reset_scissor(s_nr.dev);

    if (sub_n > 0 && s_nr.gouraud_hud_sub_pipe) {
        rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_hud_sub_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.menu_vb);
        rhi_draw(s_nr.dev, (uint32_t)sub_n, (uint32_t)sub0);
    }

    /* Menu glyph text (row labels/values/title) + the lit gold frame, both pushed
     * game-side (text_moby_native_menu). SS path: composite the supersampled layer
     * that prepare_menu_ss built — box-downsample + straight-alpha blend over the
     * panel via the FPS overlay's resolve pipe (anti-aliased 3D text). 1× fallback:
     * draw the glyphs directly (depth-isolated), the original crisp-but-aliased
     * path. Either way the glyphs sit on top of the panel. */
    if (menu_ss_ready() && s_menu_glyph_n >= 3) {
        int factor = MENU_SS_FACTOR;
        rhi_reset_scissor(s_nr.dev);
        rhi_bind_pipeline(s_nr.dev, s_nr.ovl_resolve_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.menu_ss);
        rhi_push_constants(s_nr.dev, &factor, sizeof(factor));
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.menu_quad_vb);
        rhi_draw(s_nr.dev, 6, 0);
    } else {
        draw_menu_glyphs_3d(2.0f * (float)s_nr.win_h / (float)s_nr.win_w, 2.0f);
    }
}

/* Capture the freeze backdrop from the CLEAN windowed offscreen we just ended
 * (correctly projected/pillarboxed at every window size, and — under --ssaa —
 * already box-resolved back to 1× window resolution). Hardware-blit the
 * offscreen's 4:3 content region (cover_rect = the p0 sub-rect widened by
 * ws_factor, in window pixels) into frozen_tex; the pause/inventory/exit freeze
 * present then samples this clean snapshot. World frames only, so the snapshot
 * is the gameplay frame to freeze, not the menu drawn over it. Must be called
 * after rhi_end_offscreen (off_image in TRANSFER_SRC). */
static void capture_freeze_from_offscreen(bool world_frame)
{
    if (!(world_frame && s_nr.frozen_tex && s_nr.win_w > 0 && s_nr.win_h > 0)) return;
    float fhx = s_nr.p0_hx * s_nr.ws_factor;
    int sx = (int)(((s_nr.p0_cx - fhx) * 0.5f + 0.5f) * (float)s_nr.win_w + 0.5f);
    int sw = (int)(fhx * (float)s_nr.win_w + 0.5f);
    int sy = (int)(((1.0f - s_nr.p0_hy) * 0.5f) * (float)s_nr.win_h + 0.5f);
    int sh = (int)(s_nr.p0_hy * (float)s_nr.win_h + 0.5f);
    if (sw > 0 && sh > 0) {
        static int s_pt = -1;
        if (s_pt < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); s_pt = (e && *e && *e != '0'); }
        if (s_pt) { fprintf(stderr, "s"); fflush(stderr); }   /* freeze snapshot enter */
        rhi_snapshot_offscreen_to_rt(s_nr.dev, s_nr.frozen_tex, sx, sy, sw, sh);
        if (s_pt) { fprintf(stderr, "t"); fflush(stderr); }   /* freeze snapshot done */
        s_nr.frozen_valid = true;
    }
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
    int sn; uint32_t sbg, sfade, sfreeze;
    const scene_tri_t* scene = scene_present_buf(&sn, &sbg, &sfade, &sfreeze, NULL, NULL);
    uint32_t bg = sbg; if (bg == 0) bg = bg_from_ref_corner();  /* gamestate primary (== PSX clear); corner fallback */
    bool want_scene = (sn > 0) || (s_isolation && bg != 0);

    /* native_rtt is the OFF-DISPLAY reference-resolution render: the diff pass
     * compares it, and the freeze-backdrop snapshot copies it. The LIVE vk/ab panel
     * does NOT blit from it — it draws the scene DIRECTLY into the window-sized
     * offscreen below, so the native display keeps its FULL window resolution
     * (rendering into the ref-sized native_rtt and upscaling would soften textures
     * and drop fine UI). So native_rtt is rendered only when actually consumed:
     * every frame in diff mode, and on WORLD frames in vk/ab to refresh the freeze
     * snapshot (skip menu/freeze frames so the snapshot is the gameplay frame to
     * freeze, never the menu drawn over it). */
    bool world_frame = want_scene && !(sfreeze & (1u << 31)) && scene_has_world(scene, sn);
    bool rtt_drew = false;
    float br  = (float)((bg >>  0) & 0xFF) / 255.0f;
    float bgc = (float)((bg >>  8) & 0xFF) / 255.0f;
    float bb  = (float)((bg >> 16) & 0xFF) / 255.0f;
    /* native_rtt is the off-display, reference-resolution render consumed by the
     * diff pass (every frame) and the ab freeze-backdrop snapshot (world frames).
     * The vk freeze backdrop is NOT captured from here: this full-extent render
     * skews window-dependently — it squished the pause/inventory freeze at
     * non-native window sizes — so vk snapshots the freeze from the clean,
     * correctly-projected windowed offscreen instead (rhi_snapshot_offscreen_to_rt
     * after the offscreen pass below). */
    if (s_nr.native_rtt && want_scene &&
        (variant == NRV_DIFF || (variant == NRV_AB && world_frame))) {
        rhi_begin_render_target(s_nr.dev, s_nr.native_rtt, br, bgc, bb, 1.0f);
        rtt_drew = draw_native_scene(true);
        rhi_end_render_target(s_nr.dev, s_nr.native_rtt);

        if (variant == NRV_AB && rtt_drew && world_frame && s_nr.frozen_tex) {
            rhi_begin_render_target(s_nr.dev, s_nr.frozen_tex, 0.0f, 0.0f, 0.0f, 1.0f);
            rhi_bind_pipeline(s_nr.dev, s_nr.blit_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.native_rtt);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.fs_quad_vb);
            rhi_draw(s_nr.dev, 6, 0);
            rhi_end_render_target(s_nr.dev, s_nr.frozen_tex);
            s_nr.frozen_valid = true;
        }
    }

    /* Diff: |native_rtt - reference| into the offscreen (panel-0 rect). */
    if (variant == NRV_DIFF && rtt_drew && have_ref) {
        rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);
        rhi_bind_pipeline(s_nr.dev, s_nr.diff_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.native_rtt);  /* set 0 = native */
        rhi_bind_vram2(s_nr.dev, s_nr.ref_tex);    /* set 1 = reference */
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.diff_quad_vb);
        rhi_draw(s_nr.dev, 6, 0);
        rhi_end_offscreen(s_nr.dev);
        return true;
    }

    /* SSAA (--ssaa>1, vk mode): render the scene into the supersampled ss_rtt,
     * then a fragment resolve pass box-averages it (in linear light) into the 1×
     * offscreen — a true box filter at every factor and gamma-correct, unlike the
     * present blit's single bilinear tap. Scoped to vk (ab/diff stay reference-
     * resolution for the 1:1 PsyCross comparison). */
    if (variant == NRV_VK && s_nr.ss_rtt && s_nr.ss_factor > 1 && s_nr.layout_ref_w > 0) {
        rhi_begin_render_target(s_nr.dev, s_nr.ss_rtt, 0.0f, 0.0f, 0.0f, 1.0f);
        bool drew = draw_native_scene(false);   /* scene at ss_factor× (scissor auto-scaled) */
        rhi_end_render_target(s_nr.dev, s_nr.ss_rtt);

        rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);
        if (drew) {
            int n = s_nr.ss_factor;
            rhi_bind_pipeline(s_nr.dev, s_nr.resolve_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.ss_rtt);
            rhi_push_constants(s_nr.dev, &n, sizeof(n));
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.fs_quad_vb);
            rhi_draw(s_nr.dev, 6, 0);
        } else {
            /* Empty scene → PSX VRAM framebuffer fallback (1× direct, no SSAA). */
            int tx = 0, ty = 0;
            if (s_nr.disp_w > 0 && s_nr.clip_h > 0 && s_nr.clip_h <= s_nr.disp_h) {
                int dy = (s_nr.disp_h - s_nr.clip_h) / 2;
                tx = s_nr.disp_x; ty = s_nr.disp_y + dy;
            }
            const psx_material nd_mat = { tx, ty, 0, 0, 16 };
            rhi_bind_pipeline(s_nr.dev, s_nr.vram_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            rhi_push_constants(s_nr.dev, &nd_mat, sizeof(nd_mat));
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
            rhi_draw(s_nr.dev, 6, 0);
        }
        draw_mod_meshes();                        /* mod 3D (S7 — under mod HUD) */
        draw_mod_overlay();                       /* mod HUD (content — records too) */
        if (s_overlay_live) draw_fps_overlay();   /* on the resolved 1× offscreen, crisp */
        if (s_overlay_live) draw_menu_overlay();  /* settings menu on top */
        rhi_end_offscreen(s_nr.dev);
        /* The freeze snapshot lives in the fall-through path below, which this SSAA
         * branch skips — so capture it here too, from the box-resolved 1× offscreen
         * (window resolution, matching the non-SSAA snapshot). Without this the
         * pause/inventory freeze had no captured frame under --ssaa and showed the
         * clear/cyclorama colour instead of the frozen world. */
        capture_freeze_from_offscreen(world_frame);
        return false;
    }

    /* vk / ab (and the diff fallback on an empty scene): draw the scene DIRECTLY
     * into the window-sized offscreen at full resolution (the original display path,
     * unchanged by the freeze work). The freeze overlay — drawn inside
     * draw_native_scene sampling frozen_tex — composes here too on menu frames. An
     * empty scene falls back to re-displaying the PSX VRAM framebuffer (loading
     * screens / transitions / pre-scene bootstrap). */
    rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);
    if (s_nr.layout_ref_w > 0) { /* layout built (needs a known ref size) */
        /* tpage = the CURRENT front-buffer origin (flips per frame via the double
         * buffer) + the top overscan band so the sampled rows are the visible clip. */
        int tx = 0, ty = 0;
        if (s_nr.disp_w > 0 && s_nr.clip_h > 0 && s_nr.clip_h <= s_nr.disp_h) {
            int dy = (s_nr.disp_h - s_nr.clip_h) / 2;
            tx = s_nr.disp_x;
            ty = s_nr.disp_y + dy;
        }
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

    /* Mod HUD overlay — vk variant, live + record composes (content). */
    if (variant == NRV_VK) { draw_mod_meshes(); draw_mod_overlay(); }
    /* FPS overlay — vk live present only (kept off ab/diff so comparison images
     * stay clean, and off screenshots/dumps where s_overlay_live is 0). */
    if (variant == NRV_VK && s_overlay_live) draw_fps_overlay();
    if (variant == NRV_VK && s_overlay_live) draw_menu_overlay();

    rhi_end_offscreen(s_nr.dev);

    if (variant == NRV_VK) capture_freeze_from_offscreen(world_frame);
    return false;
}

/* ---- per-present phase timers (OPENPETE_PHASE_TIMERS=1) ---------------------
 * See openpete_native_render.h. Per-present µs accumulate into s_pt_cur; the
 * live present path rolls them into a ring and prints p50/p99/max once per
 * second. Screenshot/dump composes also add into s_pt_cur but never roll a
 * present, so their (rare, one-shot) contributions fold into the next live
 * present — acceptable for a diagnostic. */
#define PT_WINDOW 4096
static uint32_t s_pt_cur[OPPT_COUNT];
static uint32_t s_pt_ring[OPPT_COUNT][PT_WINDOW];
static uint32_t s_pt_cnt_ring[OPPC_COUNT][PT_WINDOW];
static int      s_pt_cnt_cur[OPPC_COUNT];
static int      s_pt_n = 0;
static uint64_t s_pt_last_report = 0;

int openpete_phase_timers_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_PHASE_TIMERS"); on = (e && *e && *e != '0'); }
    return on;
}
uint64_t openpete_phase_now_us(void) {
    static double us_per_tick = 0.0;
    if (us_per_tick == 0.0) us_per_tick = 1e6 / (double)SDL_GetPerformanceFrequency();
    return (uint64_t)((double)SDL_GetPerformanceCounter() * us_per_tick);
}
void openpete_phase_add(int phase, uint64_t us) {
    if ((unsigned)phase < OPPT_COUNT) s_pt_cur[phase] += (uint32_t)us;
}
void openpete_phase_set_count(int which, int n) {
    if ((unsigned)which < OPPC_COUNT) s_pt_cnt_cur[which] = n;
}

static int pt_cmp_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

/* Roll the current present's phase totals into the ring; report ~1/sec. */
static void phase_present_done(void)
{
    if (!openpete_phase_timers_on()) return;
    for (int p = 0; p < OPPT_COUNT; p++) { s_pt_ring[p][s_pt_n] = s_pt_cur[p]; s_pt_cur[p] = 0; }
    for (int c = 0; c < OPPC_COUNT; c++) s_pt_cnt_ring[c][s_pt_n] = (uint32_t)s_pt_cnt_cur[c];
    s_pt_n++;
    uint64_t now = openpete_phase_now_us();
    if (s_pt_last_report == 0) s_pt_last_report = now;
    if (s_pt_n < PT_WINDOW && now - s_pt_last_report < 1000000u) return;

    static const char* pname[OPPT_COUNT] =
        { "rex", "ter", "moby", "shdw", "spy", "prep", "comp", "gpu", "sim" };
    static uint32_t sorted[PT_WINDOW];
    char line[512]; int off = 0;
    off += snprintf(line + off, sizeof(line) - (size_t)off, "[phase f=%u n=%d]", s_scene_frame, s_pt_n);
    for (int p = 0; p < OPPT_COUNT && off < (int)sizeof(line); p++) {
        memcpy(sorted, s_pt_ring[p], (size_t)s_pt_n * sizeof(uint32_t));
        qsort(sorted, (size_t)s_pt_n, sizeof(uint32_t), pt_cmp_u32);
        uint32_t p50 = sorted[s_pt_n / 2];
        uint32_t p99 = sorted[(int)((s_pt_n - 1) * 0.99)];
        uint32_t mx  = sorted[s_pt_n - 1];
        off += snprintf(line + off, sizeof(line) - (size_t)off,
                        " %s %u/%u/%u", pname[p], p50, p99, mx);
    }
    static const char* cname[OPPC_COUNT] = { "mobys", "tris", "draws", "culled" };
    for (int c = 0; c < OPPC_COUNT && off < (int)sizeof(line); c++) {
        uint32_t mx = 0, sum = 0;
        for (int i = 0; i < s_pt_n; i++) { if (s_pt_cnt_ring[c][i] > mx) mx = s_pt_cnt_ring[c][i]; sum += s_pt_cnt_ring[c][i]; }
        off += snprintf(line + off, sizeof(line) - (size_t)off,
                        " | %s avg %u max %u", cname[c], sum / (uint32_t)s_pt_n, mx);
    }
    fprintf(stderr, "%s  (p50/p99/max us)\n", line);
    s_pt_n = 0; s_pt_last_report = now;
}

/* ── Mod postfx executor (S3, plans/openpete-mod-shaders.md) ─────────
 * Runs the enabled COMPOSITE-point chain over the composed frame:
 * input → ping/pong RTs → 1:1 blit back into the compose target, so
 * every consumer (present blit, --record, live screenshot, AND the
 * screenshot-all/headless native_rtt dump) sees the post-processed
 * frame — the native_rtt compose bypasses off_image, so the chain is
 * wired into both paths explicitly (the vacuous-gate trap from the
 * plan review). Engine UBO rides block 0, pass i's mod block 1+i.
 * S4 completes the engine-block fields (tick/alpha/canonical). */
#define PFX_MAX_PASSES 15
typedef struct {   /* C mirror of op_engine_ubo — std140, offsets load-bearing */
    float    time, alpha;
    uint32_t tick, present, canonical;
    float    aspect;
    float    res[2], win[2];
} pfx_engine_ubo_t;

/* The engine block (op_engine, set 3 binding 0) — shared fill for the
 * postfx and material UBOs. `w`,`h` = the pass/render resolution. */
static void write_engine_block(rhi_ubo* u, int blk, int w, int h)
{
    pfx_engine_ubo_t eng;
    memset(&eng, 0, sizeof eng);
    eng.time = (float)((double)openpete_phase_now_us() * 1e-6);
    eng.present = s_scene_frame;
    int canonical = 1;
    openpete_mods_present_ctx(&eng.tick, &eng.alpha, &canonical);
    eng.canonical = (uint32_t)canonical;
    int ww = 0, wh = 0;
    rhi_swapchain_extent(s_nr.dev, &ww, &wh);
    eng.res[0] = (float)(w > 0 ? w : ww);  eng.res[1] = (float)(h > 0 ? h : wh);
    eng.win[0] = (float)ww;                eng.win[1] = (float)wh;
    eng.aspect = eng.res[1] > 0 ? eng.res[0] / eng.res[1] : 1.0f;
    rhi_ubo_write(u, blk, &eng, sizeof eng);
}

/* S8b: (re)fill the material UBO — engine block 0 + each interned
 * per-instance slot's uniform bytes (blocks 1+slot). Called per present
 * before the scene draw. Lazy-creates the UBO on first override use;
 * a no-op beyond the engine block when no slots are interned. Block
 * count covers OPENPETE_MATERIAL_SLOTS interned slots + block 0. */
#define MAT_UBO_BLOCKS 129   /* 1 engine + 128 slots (MOD_MATERIAL_SLOTS) */
static void fill_material_ubo(void)
{
    if (!tag_split_on()) return;   /* no selector / dev split armed */
    if (!s_mat_ubo)
        s_mat_ubo = rhi_create_ubo(s_nr.dev, OPENPETE_MATERIAL_UNIFORMS_MAX,
                                   MAT_UBO_BLOCKS);
    if (!s_mat_ubo) return;
    write_engine_block(s_mat_ubo, 0, 0, 0);
    int nslots = openpete_mods_material_slot_count();   /* 1 + interned */
    /* Per-instance material census (OPENPETE_MATERIAL_STATS=1): the peak
     * interned-slot count is the structural proof that per-instance
     * callbacks produce distinct outcomes AND the batcher splits by
     * them — and the honest budget signal (each slot is an extra draw).
     * Prints once when a new peak is reached. */
    {
        static int stats = -1, peak = 0;
        if (stats < 0) { const char* e = getenv("OPENPETE_MATERIAL_STATS");
                         stats = (e && *e && *e != '0'); }
        if (stats && nslots - 1 > peak) {
            peak = nslots - 1;
            fprintf(stderr, "[material] per-instance slots interned this "
                            "present = %d (each = one extra draw)\n", peak);
        }
    }
    for (int s = 1; s < nslots && s < MAT_UBO_BLOCKS - 1; s++) {
        uint32_t len = 0;
        const void* p = openpete_mods_material_slot_uniforms(s, &len);
        if (p && len) rhi_ubo_write(s_mat_ubo, 1 + s, p, len);
    }
}

/* Ping-pong RT pool: two sizes in practice (window for live presents,
 * reference for screenshot dumps) — LRU-evict beyond that (rare; under
 * wait-idle). */
static struct { int w, h; rhi_texture* t[2]; uint32_t used; } s_pfx_pool[2];
static uint32_t s_pfx_stamp;
/* Previous present's final composite (set 2, temporal effects) —
 * snapshotted from off_image AFTER the live chain each present, so
 * during pass N of present P it holds present P-1's output. Cleared
 * at creation (first present would otherwise sample garbage). */
static rhi_texture* s_pfx_prev;
static int s_pfx_prev_w, s_pfx_prev_h;

static rhi_texture** pfx_pings(int w, int h) {
    int victim = 0;
    for (int i = 0; i < 2; i++) {
        if (s_pfx_pool[i].w == w && s_pfx_pool[i].h == h && s_pfx_pool[i].t[0]) {
            s_pfx_pool[i].used = ++s_pfx_stamp;
            return s_pfx_pool[i].t;
        }
        if (s_pfx_pool[i].used < s_pfx_pool[victim].used) victim = i;
    }
    if (s_pfx_pool[victim].t[0]) {
        rhi_wait_idle(s_nr.dev);   /* rare: a third size evicts (resize) */
        rhi_destroy_texture(s_nr.dev, s_pfx_pool[victim].t[0]);
        rhi_destroy_texture(s_nr.dev, s_pfx_pool[victim].t[1]);
        s_pfx_pool[victim].t[0] = s_pfx_pool[victim].t[1] = NULL;
    }
    s_pfx_pool[victim].t[0] = rhi_create_render_target(s_nr.dev, (uint32_t)w, (uint32_t)h);
    s_pfx_pool[victim].t[1] = rhi_create_render_target(s_nr.dev, (uint32_t)w, (uint32_t)h);
    if (!s_pfx_pool[victim].t[0] || !s_pfx_pool[victim].t[1]) {
        if (s_pfx_pool[victim].t[0]) rhi_destroy_texture(s_nr.dev, s_pfx_pool[victim].t[0]);
        if (s_pfx_pool[victim].t[1]) rhi_destroy_texture(s_nr.dev, s_pfx_pool[victim].t[1]);
        s_pfx_pool[victim].t[0] = s_pfx_pool[victim].t[1] = NULL;
        return NULL;
    }
    s_pfx_pool[victim].w = w; s_pfx_pool[victim].h = h;
    s_pfx_pool[victim].used = ++s_pfx_stamp;
    return s_pfx_pool[victim].t;
}

/* ── Mod custom-pipeline draws (S7) ──────────────────────────────────
 * Drains the present-hook draw_mesh queue inside the scene pass (called
 * beside draw_mod_overlay, after the world — mod 3D under mod HUD).
 * Vertices convert to rhi_vertex into a per-frame ring VB; pipelines
 * build lazily from the mod's SPIR-V (desc.mod_pipeline layout: std
 * attributes + engine UBO at set 3). Queue lifecycle is owned by the
 * present flush (cleared at flush start), so composes never re-drain
 * stale draws and headless runs can't accumulate. */
static rhi_buffer* s_mesh_vb;
static rhi_vertex  s_mesh_scratch[65536];

static void draw_mod_meshes(void)
{
    int nd = openpete_mods_drawmesh_count();
    if (nd <= 0) return;
    if (!s_pfx_ubo)
        s_pfx_ubo = rhi_create_ubo(s_nr.dev, OPENPETE_POSTFX_UNIFORMS_MAX,
                                   1 + PFX_MAX_PASSES);
    if (!s_pfx_ubo) return;
    if (!s_mesh_vb)
        s_mesh_vb = rhi_create_vertex_buffer_dynamic(s_nr.dev, sizeof s_mesh_scratch);
    if (!s_mesh_vb) return;

    /* Engine UBO block 0: written here so mesh-only presents (no postfx
     * chain) still see live values; the postfx run rewrites the same
     * frame slot with identical content later in the frame. */
    {
        pfx_engine_ubo_t eng;
        memset(&eng, 0, sizeof eng);
        eng.time = (float)((double)openpete_phase_now_us() * 1e-6);
        eng.present = s_scene_frame;
        int canonical = 1;
        openpete_mods_present_ctx(&eng.tick, &eng.alpha, &canonical);
        eng.canonical = (uint32_t)canonical;
        int ww = 0, wh = 0;
        rhi_swapchain_extent(s_nr.dev, &ww, &wh);
        eng.res[0] = eng.win[0] = (float)ww;
        eng.res[1] = eng.win[1] = (float)wh;
        eng.aspect = wh > 0 ? (float)ww / (float)wh : 1.0f;
        rhi_ubo_write(s_pfx_ubo, 0, &eng, sizeof eng);
    }

    /* Convert + pack every queued draw into the ring VB (one update). */
    int total = 0;
    for (int i = 0; i < nd; i++) {
        openpete_mod_pipe_gpu_t pd;
        const openpete_mod_vertex_t* mv; int n; uint16_t tex;
        if (openpete_mods_drawmesh_get(i, &pd, &mv, &n, &tex) != 0) continue;
        if (total + n > (int)(sizeof s_mesh_scratch / sizeof s_mesh_scratch[0]))
            break;
        for (int k = 0; k < n; k++) {
            rhi_vertex* v = &s_mesh_scratch[total + k];
            v->x = mv[k].x; v->y = mv[k].y; v->z = mv[k].z;
            v->u = mv[k].u; v->v = mv[k].v;
            v->rgba = mv[k].rgba;
            v->tex = 0; v->sx = 0.f; v->sy = 0.f;
        }
        total += n;
    }
    if (total == 0) return;
    rhi_update_buffer(s_mesh_vb, s_mesh_scratch, (size_t)total * sizeof(rhi_vertex));
    /* Precautionary parity with draw_mod_overlay: the scene path can leave
     * a sub-region scissor armed (HD-replacement clip rects). Not observed
     * to clip mod meshes in practice (the S7 bring-up "invisible triangle"
     * was blind INSTRUMENTS, not state — see the plan's S7 note), but a
     * clipped first mod draw would be miserable to rediscover. Depth is NOT
     * cleared: mesh pipelines with depth_test z-test against the world. */
    rhi_reset_scissor(s_nr.dev);
    rhi_bind_vertex_buffer(s_nr.dev, s_mesh_vb);

    int first = 0;
    for (int i = 0; i < nd; i++) {
        openpete_mod_pipe_gpu_t pd;
        const openpete_mod_vertex_t* mv; int n; uint16_t tex;
        if (openpete_mods_drawmesh_get(i, &pd, &mv, &n, &tex) != 0) continue;
        if (first + n > total) break;
        if (*pd.pipeline_slot == NULL) {
            rhi_pipeline_desc rd = {
                .blend = pd.blend == OP_BLEND_ADD ? RHI_BLEND_ADD
                       : pd.blend == OP_BLEND_ADD4 ? RHI_BLEND_ADD
                       : pd.blend == OP_BLEND_SUB ? RHI_BLEND_SUBTRACT
                       : pd.blend == OP_BLEND_AVG ? RHI_BLEND_ALPHA
                       : RHI_BLEND_NONE,
                .depth_test = pd.depth_test != 0,
                .depth_write_off = pd.depth_write == 0,
                .mod_pipeline = true,
            };
            rhi_pipeline* p = rhi_create_pipeline_spv(s_nr.dev, &rd,
                                                      pd.vert_spv, pd.vert_words,
                                                      pd.frag_spv, pd.frag_words);
            *pd.pipeline_slot = p ? (void*)p : PFX_PIPE_FAILED;
            if (!p)
                fprintf(stderr, "[native] draw_mesh: pipeline build FAILED "
                                "for %s\n", pd.owner);
        }
        if (*pd.pipeline_slot != PFX_PIPE_FAILED) {
            rhi_bind_pipeline(s_nr.dev, (rhi_pipeline*)*pd.pipeline_slot);
            /* set 0: the mod texture (texture_register id) through the HD
             * cache; untextured/missing binds the VRAM texture (inert for
             * shaders that don't sample). Sets 1/2 mirror it (contract:
             * postfx-only inputs are undefined for mesh pipelines). */
            rhi_texture* t0 = s_nr.vram_tex;
            if (tex) {
                int mw = 0, mh = 0;
                const unsigned char* px = openpete_mods_texture_get(tex, &mw, &mh);
                if (px) {
                    uint64_t key = (uint64_t)(uintptr_t)px;
                    rhi_texture* mt = hd_cache_lookup(key);
                    if (!mt) hd_pending_add(key, px, mw, mh);
                    else t0 = mt;
                }
            }
            rhi_bind_texture_set(s_nr.dev, t0, 0);
            rhi_bind_texture_set(s_nr.dev, t0, 1);
            rhi_bind_texture_set(s_nr.dev, t0, 2);
            rhi_ubo_bind(s_nr.dev, s_pfx_ubo, 0, 0);
            rhi_draw(s_nr.dev, (uint32_t)n, (uint32_t)first);
        }
        first += n;
    }
}

/* Destroy pipelines orphaned by a mod hot-reload sweep. Dev-mode only
 * path — the wait-idle is acceptable there. */
static void pfx_drain_retired(void) {
    void* dead[PFX_MAX_PASSES];
    int n = openpete_mods_postfx_take_retired(dead, PFX_MAX_PASSES);
    if (n <= 0) return;
    rhi_wait_idle(s_nr.dev);
    for (int i = 0; i < n; i++)
        rhi_destroy_pipeline(s_nr.dev, (rhi_pipeline*)dead[i]);
}

/* Run the chain. target == NULL → over off_image (live present);
 * else over the given compose RT (screenshot-all). Returns pass count. */
static int native_postfx_run(rhi_texture* target, int w, int h)
{
    openpete_postfx_pass_t pass[PFX_MAX_PASSES];
    int n = openpete_mods_postfx_collect(0 /* COMPOSITE */, pass, PFX_MAX_PASSES);
    if (n <= 0 || w <= 0 || h <= 0) return 0;
    if (!s_pfx_ubo)
        s_pfx_ubo = rhi_create_ubo(s_nr.dev, OPENPETE_POSTFX_UNIFORMS_MAX,
                                   1 + PFX_MAX_PASSES);
    rhi_texture** ping = pfx_pings(w, h);
    if (!s_pfx_ubo || !ping) return 0;

    pfx_engine_ubo_t eng;
    memset(&eng, 0, sizeof eng);
    eng.time = (float)((double)openpete_phase_now_us() * 1e-6);
    eng.present = s_scene_frame;
    {
        /* Same source as the present-hook ctx (mod_draw.c) — shaders
         * and mod C code can never disagree on the sub-tick phase. */
        int canonical = 1;
        openpete_mods_present_ctx(&eng.tick, &eng.alpha, &canonical);
        eng.canonical = (uint32_t)canonical;
        if (target) { eng.alpha = 0.0f; eng.canonical = 1; } /* dump compose */
    }
    eng.res[0] = (float)w;   eng.res[1] = (float)h;
    {
        int ww = 0, wh = 0;
        rhi_swapchain_extent(s_nr.dev, &ww, &wh);
        eng.win[0] = (float)ww; eng.win[1] = (float)wh;
    }
    eng.aspect = (h > 0) ? (float)w / (float)h : 1.0f;
    rhi_ubo_write(s_pfx_ubo, 0, &eng, sizeof eng);

    /* Prev-frame RT (live path only): (re)create cleared on size change. */
    if (!target && (s_pfx_prev_w != w || s_pfx_prev_h != h)) {
        if (s_pfx_prev) { rhi_wait_idle(s_nr.dev); rhi_destroy_texture(s_nr.dev, s_pfx_prev); s_pfx_prev = NULL; }
        s_pfx_prev = rhi_create_render_target(s_nr.dev, (uint32_t)w, (uint32_t)h);
        if (s_pfx_prev) {
            rhi_begin_render_target(s_nr.dev, s_pfx_prev, 0.f, 0.f, 0.f, 1.f);
            rhi_end_render_target(s_nr.dev, s_pfx_prev);
        }
        s_pfx_prev_w = w; s_pfx_prev_h = h;
    }
    for (int i = 0; i < n; i++)
        if (pass[i].uniforms_len)
            rhi_ubo_write(s_pfx_ubo, (uint32_t)(1 + i), pass[i].uniforms,
                          pass[i].uniforms_len);

    if (!target)
        rhi_offscreen_to_sampled(s_nr.dev);   /* barrier, outside any pass */

    int out_idx = 0, ran = 0;
    for (int i = 0; i < n; i++) {
        if (*pass[i].pipeline_slot == NULL) {
            rhi_pipeline_desc pd = {
                .vert_spv = "postfx_fs.vert.spv",
                .frag_spv = NULL,
                .blend = RHI_BLEND_NONE,
                .postfx = true,
            };
            rhi_pipeline* p = rhi_create_pipeline_spv(s_nr.dev, &pd, NULL, 0,
                                                      pass[i].frag_spv,
                                                      pass[i].frag_words);
            *pass[i].pipeline_slot = p ? (void*)p : PFX_PIPE_FAILED;
            if (!p)
                fprintf(stderr, "[native] postfx: pipeline build FAILED for "
                                "%s — pass disabled\n", pass[i].owner);
        }
        if (*pass[i].pipeline_slot == PFX_PIPE_FAILED) continue;
        rhi_texture* out = ping[out_idx];
        /* Per-mod GPU attribution (S9): one zone per pass, named by the
         * owning mod id (loader-stable string). Live path only — the dump
         * path runs outside the frame-loop zone budget. */
        if (!target) rhi_gpu_zone_begin(s_nr.dev, pass[i].owner);
        rhi_begin_render_target(s_nr.dev, out, 0.f, 0.f, 0.f, 1.f);
        rhi_bind_pipeline(s_nr.dev, (rhi_pipeline*)*pass[i].pipeline_slot);
        /* set 0 = previous pass (chain input on pass 0); set 1 = opaque
         * pre-pass depth (contract: WORLD-opaque only, the input color
         * as a documented fallback when the pre-pass is absent); set 2 =
         * previous present's composite (live path; dumps fall back to
         * the input). */
        rhi_texture* in0 = (ran == 0) ? target : ping[out_idx ^ 1];
        if (in0) rhi_bind_texture_set(s_nr.dev, in0, 0);
        else     rhi_bind_offscreen_tex(s_nr.dev, 0);
        if (s_nr.opaque_depth)
            rhi_bind_depth_sampler(s_nr.dev, s_nr.opaque_depth, 1);
        else if (in0) rhi_bind_texture_set(s_nr.dev, in0, 1);
        else          rhi_bind_offscreen_tex(s_nr.dev, 1);
        if (!target && s_pfx_prev)
            rhi_bind_texture_set(s_nr.dev, s_pfx_prev, 2);
        else if (in0) rhi_bind_texture_set(s_nr.dev, in0, 2);
        else          rhi_bind_offscreen_tex(s_nr.dev, 2);
        rhi_ubo_bind(s_nr.dev, s_pfx_ubo, 0, (uint32_t)(1 + i));
        rhi_draw(s_nr.dev, 3, 0);
        rhi_end_render_target(s_nr.dev, out);
        if (!target) rhi_gpu_zone_end(s_nr.dev);
        out_idx ^= 1;
        ran++;
    }
    if (!ran) {
        /* Every pass failed to build: restore off_image for the blit. */
        if (!target) rhi_offscreen_resume_blit_src(s_nr.dev);
        return 0;
    }
    rhi_texture* final_rt = ping[out_idx ^ 1];
    if (!target) {
        rhi_blit_rt_to_offscreen(s_nr.dev, final_rt);
        /* Temporal input for the NEXT present: off_image is TRANSFER_SRC
         * again — snapshot the post-chain composite. */
        if (s_pfx_prev)
            rhi_snapshot_offscreen_to_rt(s_nr.dev, s_pfx_prev, 0, 0, w, h);
    } else {
        rhi_blit_rt_to_rt(s_nr.dev, target, final_rt);
    }
    return ran;
}

/* GPU per-pass zone report (OPENPETE_GPU_TIMERS=1): EMA per zone name over
 * the frame-lagged results rhi_begin_frame harvests, printed on the same
 * ~1 s cadence as the phase report. Zone identity is the name POINTER (the
 * brackets pass literals), with strcmp as the collision fallback. WSLg /
 * lavapipe numbers measure the CPU rasterizer, not a GPU — real numbers come
 * from the Windows build (see plans/openpete-gpu-pass-timers.md). */
#define GPUZ_MAX 16
static struct { const char *name; double ema; } s_gpuz[GPUZ_MAX];
static int      s_gpuz_n = 0;
static uint64_t s_gpuz_last = 0;

static void gpu_zone_report(void)
{
    if (!rhi_gpu_timers_on(s_nr.dev)) return;
    rhi_gpu_zone_result res[RHI_GPU_ZONES_MAX];
    int n = rhi_gpu_zone_results(s_nr.dev, res, RHI_GPU_ZONES_MAX);
    for (int i = 0; i < n; i++) {
        int k = -1;
        for (int j = 0; j < s_gpuz_n; j++) {
            if (s_gpuz[j].name == res[i].name || strcmp(s_gpuz[j].name, res[i].name) == 0) {
                k = j; break;
            }
        }
        if (k < 0) {
            if (s_gpuz_n >= GPUZ_MAX) continue;
            k = s_gpuz_n++;
            s_gpuz[k].name = res[i].name;
            s_gpuz[k].ema  = res[i].us;      /* seed with the first sample */
        } else {
            s_gpuz[k].ema += 0.05 * (res[i].us - s_gpuz[k].ema);
        }
    }
    uint64_t now = openpete_phase_now_us();
    if (s_gpuz_last == 0) s_gpuz_last = now;
    if (s_gpuz_n == 0 || now - s_gpuz_last < 1000000u) return;
    char line[512]; int off = 0;
    off += snprintf(line + off, sizeof(line) - (size_t)off, "[gpu-pass f=%u]", s_scene_frame);
    for (int j = 0; j < s_gpuz_n && off < (int)sizeof(line); j++)
        off += snprintf(line + off, sizeof(line) - (size_t)off,
                        " %s %.0f", s_gpuz[j].name, s_gpuz[j].ema);
    fprintf(stderr, "%s  (ema us)\n", line);
    s_gpuz_last = now;
}

/* Game-registered sub-tick re-extract callback (high-FPS spine). Invoked by
 * the host loop on each intermediate present; rebuilds s_scene against an
 * interpolated gamestate view. NULL → no intermediates (present stays at sim
 * cadence). See openpete_native_render.h. */
static openpete_native_reextract_fn s_reextract_cb = NULL;
void openpete_native_register_reextract(openpete_native_reextract_fn fn) { s_reextract_cb = fn; }
openpete_native_reextract_fn openpete_native_get_reextract(void) { return s_reextract_cb; }

static openpete_native_commit_fn s_commit_cb = NULL;
void openpete_native_register_commit(openpete_native_commit_fn fn) { s_commit_cb = fn; }
openpete_native_commit_fn openpete_native_get_commit(void) { return s_commit_cb; }

/* --record async capture: persistent host buffer the readback ring swizzles
 * into before handing off to the encoder thread. Process-lifetime; freed in
 * shutdown. Shared by the per-present cycle and the shutdown drain. */
static unsigned char *s_recbuf = NULL;
static size_t         s_reccap = 0;
static unsigned char *recbuf_ensure(size_t need)
{
    if (need == 0) return NULL;
    if (need > s_reccap) {
        unsigned char *nb = realloc(s_recbuf, need);
        if (!nb) return NULL;
        s_recbuf = nb; s_reccap = need;
    }
    return s_recbuf;
}

/* Queue this present for async readback (tagged with its wall-clock PTS) and, if
 * a frame captured a couple presents ago is ready, hand it to the encoder. Rides
 * the frame's own fence — NO GPU stall, so the recording runs at real speed
 * (unlike the screenshot path's blocking rhi_read_offscreen). Called between the
 * blit and end_frame, while the frame slot is still current. */
static void record_cycle(uint64_t pts_us)
{
    unsigned char *buf = recbuf_ensure((size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u);
    if (!buf) return;
    int w = 0, h = 0; uint64_t tag = 0;
    if (rhi_cycle_readback(s_nr.dev, pts_us, buf, s_reccap, &w, &h, &tag) && w > 0 && h > 0)
        openpete_record_submit_frame(buf, w, h, /*top_down=*/1, tag);
}

void openpete_native_render_frame(uint64_t record_pts_us)
{
    if (!s_nr.enabled)
        return;

    /* Headless (W1.1): no window / swapchain, so there is no live present. The
     * scene is composed on demand by openpete_native_render_screenshot_all. */
    if (s_nr.headless)
        return;

    /* Rolling 0.5s FPS window for the overlay. Counts actual presents (canonical
     * + sub-tick intermediates), so it reflects the true on-screen present rate. */
    {
        uint64_t now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
        if (s_fps_win_start == 0) s_fps_win_start = now;
        s_fps_frames++;
        uint64_t el = now - s_fps_win_start;
        if (freq > 0 && el >= freq / 2u) {
            s_fps_value = (int)((double)s_fps_frames * (double)freq / (double)el + 0.5);
            s_fps_win_start = now; s_fps_frames = 0;
        }
    }

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
     * their own one-shot GPU submits, kept clear of our render pass. Called
     * UNCONDITIONALLY: sync_reference_texture self-no-ops when PsyCross hasn't
     * produced a scene (pure vk — the OT isn't rasterized — so GR_GetPresentSceneSize
     * returns 0 → early false), so it's free there; whenever PsyCross IS rendering
     * (ab/diff/screenshot) it must run, both to upload the reference AND because its
     * GL readback orders the GPU work the screenshot's rhi_read_offscreen then reads
     * (gating it off for vk blanked the captured native-intermediate frames). NRV_VK
     * never samples have_ref, so the value itself only matters for ab/diff. */
    sync_vram_texture();
    bool have_ref = sync_reference_texture();

    /* Reference resolution (the display shape for the window aspect-fit + the
     * native_rtt/freeze size) normally comes from PsyCross's present FBO via
     * sync_reference_texture. In pure vk PsyCross no longer rasterizes the OT, so
     * that returns nothing and ref_w/ref_h would stay 0 → update_layout bails →
     * compose draws nothing → black. Derive it from GAMESTATE instead: the
     * bars-free PSX display is 4:3 corrected for the vertical overscan crop
     * (aspect = 4/3 * disp_h/clip_h), at a ~2x-internal reference so the
     * off-display RT stays crisp. Only the aspect matters to the layout (fit_cell
     * cancels the absolute scale); the size feeds native_rtt. Gated vk-only —
     * ab/diff genuinely need the PsyCross reference, so they wait for it. */
    if (!have_ref && !s_scene_ref_compare) {
        int dh = s_nr.disp_h > 0 ? s_nr.disp_h : 240;
        int ch = s_nr.clip_h > 0 ? s_nr.clip_h : 224;
        int rh = ch * 2;
        int rw = (int)((double)rh * (4.0 / 3.0) * (double)dh / (double)ch + 0.5);
        if (rw > 0 && rh > 0) { s_nr.ref_w = rw; s_nr.ref_h = rh; }
    }
    update_layout();
    ensure_native_rtt(true);   /* native_rtt is the canonical frame in every mode */
    ensure_ss_rtt();           /* SSAA supersampled scene target (--ssaa>1, vk mode) */
    ensure_ovl_ss();           /* FPS-overlay supersample target + composite quad */
    ensure_menu_ss();          /* settings-menu glyph supersample (lazy, while open) */
    ensure_opaque_depth();     /* WORLD-opaque depth pre-pass target (shadow decal) */

    /* Present-path step trace (OPENPETE_LOAD_CENSUS=1): bisects a present that
     * never returns (the freeze-point hang) to the exact call. Terse; one
     * letter per step: B=begin_frame done, C=compose done, S=swapchain blit
     * done, E=end_frame done. A truncated sequence names the hung call. */
    static int s_ptrace = -1;
    if (s_ptrace < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); s_ptrace = (e && *e && *e != '0'); }

    const int pt_on = openpete_phase_timers_on();
    uint64_t pt_t0 = pt_on ? openpete_phase_now_us() : 0, pt_t1;
    if (!rhi_begin_frame(s_nr.dev))
        return; /* swapchain recreating or window minimized — skip this present */
    gpu_zone_report();  /* fold in the zone results begin_frame just harvested */
    /* Pending VRAM/reference uploads ride THIS frame's command buffer (W2.2)
     * — must precede the first render pass (the depth pre-pass below). */
    rhi_gpu_zone_begin(s_nr.dev, "upload");
    upload_synced_textures();
    hd_flush_pending();        /* HD replacement textures queued at draw time */
    rhi_gpu_zone_end(s_nr.dev);
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_GPU_SYNC, pt_t1 - pt_t0); pt_t0 = pt_t1; }
    if (s_ptrace) { fprintf(stderr, "[ptrace f=%u] B", s_scene_frame); fflush(stderr); }
    /* Shared WORLD-opaque depth pre-pass (its own render pass) — must record
     * before every consumer pass; one raster serves rtt/SSAA/offscreen alike. */
    rhi_gpu_zone_begin(s_nr.dev, "prepass");
    native_depth_prepass();
    rhi_gpu_zone_end(s_nr.dev);
    /* Moby occlusion grid: reduce + ring-readback (live presents only). */
    rhi_gpu_zone_begin(s_nr.dev, "occl");
    native_occl_downsample();
    rhi_gpu_zone_end(s_nr.dev);
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_PREPASS, pt_t1 - pt_t0); pt_t0 = pt_t1; }

    int variant = s_nr.diff_mode ? NRV_DIFF : s_nr.ab_mode ? NRV_AB : NRV_VK;
    s_overlay_live = 1;                       /* this is the live present → allow the FPS overlay */
    /* Render the FPS glyphs at OVL_SS_FACTOR× into ovl_ss now — its own render
     * pass, so it must precede compose_variant's offscreen pass. compose's
     * draw_fps_overlay then composites the downsampled result. */
    rhi_gpu_zone_begin(s_nr.dev, "ovl_ss");
    prepare_fps_overlay_ss();
    rhi_gpu_zone_end(s_nr.dev);
    rhi_gpu_zone_begin(s_nr.dev, "menu_ss");
    prepare_menu_ss();         /* menu glyphs at MENU_SS_FACTOR× → menu_ss (own pass) */
    rhi_gpu_zone_end(s_nr.dev);
    rhi_gpu_zone_begin(s_nr.dev, "compose");
    bool did_diff = compose_variant(variant, have_ref);
    rhi_gpu_zone_end(s_nr.dev);
    s_overlay_live = 0;                       /* screenshot/dump composes must not draw it */

    /* Mod postfx chain (COMPOSITE point) — vk variant only; the ab/diff
     * panes stay reference-pure. Runs on EVERY present (canonical +
     * intermediate). No outer GPU zone: the executor emits ONE ZONE PER
     * PASS NAMED BY THE OWNING MOD (zones don't nest), so a slow shader
     * shows up BY MOD ID in the [gpu-pass] report — the S9 budget
     * attribution. */
    if (variant == NRV_VK) {
        pfx_drain_retired();
        int pw = 0, ph = 0;
        rhi_swapchain_extent(s_nr.dev, &pw, &ph);
        native_postfx_run(NULL, pw, ph);
    }
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_COMPOSE, pt_t1 - pt_t0); pt_t0 = pt_t1;
                 openpete_phase_set_count(OPPC_TRIS, s_scene_n); }
    if (s_ptrace) { fprintf(stderr, "C"); fflush(stderr); }

    rhi_gpu_zone_begin(s_nr.dev, "blit");
    rhi_blit_to_swapchain(s_nr.dev);
    rhi_gpu_zone_end(s_nr.dev);
    if (s_ptrace) { fprintf(stderr, "S"); fflush(stderr); }

    /* --record: capture the on-screen native frame via the async ring (no-op
     * unless recording is active). MUST run before end_frame, while this frame
     * slot is still current — the copy is appended to the frame's command
     * buffer. */
    if (openpete_record_active()) {
        rhi_gpu_zone_begin(s_nr.dev, "record");
        record_cycle(record_pts_us);
        rhi_gpu_zone_end(s_nr.dev);
    }

    rhi_end_frame(s_nr.dev);
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_GPU_SYNC, pt_t1 - pt_t0);
                 openpete_phase_set_count(OPPC_DRAWS, (int)rhi_draw_calls(s_nr.dev));
                 phase_present_done(); }
    if (s_ptrace) { fprintf(stderr, "E\n"); fflush(stderr); }

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

static int native_render_screenshot_all_impl(const char *base);

/* Wrapper: the screenshot compose uses the reference-aligned scene depth (see
 * s_scene_shot_compose) so -vk/-ab/-diff show the SAME game frame as the -og
 * PsyCross capture beside them. Flag set/cleared here so the impl's early
 * returns can't leak it into the live present path. */
int openpete_native_render_screenshot_all(const char *base)
{
    s_scene_shot_compose = 1;
    int r = native_render_screenshot_all_impl(base);
    s_scene_shot_compose = 0;
    return r;
}

static int native_render_screenshot_all_impl(const char *base)
{
    if (!s_nr.enabled || !base) return 1;

    /* vk-only widescreen: the RTT path below renders the 4:3 CONTENT shape (the
     * RTT is ref-sized), which doesn't show the Hor+ widening. The just-presented
     * LIVE offscreen, however, already holds the full widescreen frame at window
     * size — grab it now, before this function opens a new frame, and use it for
     * the -vk image so the dump matches what the window shows. Only in vk-only
     * (ab/diff stay 4:3 to compare against the reference). */
    unsigned char *live = NULL; int live_w = 0, live_h = 0;
    if (!s_scene_ref_compare && s_nr.ws_factor > 1.0001f &&
        s_nr.win_w > 0 && s_nr.win_h > 0) {
        size_t lcap = (size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u;
        live = malloc(lcap);
        if (live && !rhi_read_offscreen(s_nr.dev, live, lcap, &live_w, &live_h)) {
            free(live); live = NULL;
        }
    }

    /* Re-sync VRAM + reference so the dump matches the just-presented frame. */
    sync_vram_texture();
    bool have_ref = sync_reference_texture();
    ensure_native_rtt(true);
    ensure_opaque_depth();     /* WORLD-opaque depth pre-pass target (shadow decal) */
    /* -ab/-diff need the PsyCross reference readback; without one (pure vk with
     * PsyCross in null-GPU library mode — no FBO exists at all) the dump writes
     * the -vk variant alone. ref_w/ref_h are still valid: the present path
     * derives them from gamestate when there is no reference (see the
     * !have_ref branch in openpete_native_render_frame). */
    bool with_ref = have_ref && s_nr.readback != NULL;
    if (!s_nr.native_rtt || s_nr.ref_w <= 0 || s_nr.ref_h <= 0)
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

    /* Background = the gamestate draw-env clear (matching the live native render);
     * scene_bg then corner as fallbacks. See draw_native_scene for the rationale. */
    int scene_n; uint32_t scene_bg, scene_fade;
    (void)scene_present_buf(&scene_n, &scene_bg, &scene_fade, NULL, NULL, NULL);
    uint32_t bg = s_drawenv_clear ? s_drawenv_clear : scene_bg;
    if (bg == 0) bg = bg_from_ref_corner();
    const float br  = (float)((bg >>  0) & 0xFF) / 255.0f;
    const float bgc = (float)((bg >>  8) & 0xFF) / 255.0f;
    const float bb  = (float)((bg >> 16) & 0xFF) / 255.0f;

    /* Render the native scene into the ref-sized RTT (full extent, no panels).
     * The offscreen + present are kept valid with a plain bg clear so the window
     * isn't disturbed mid-dump; the RTT (not the window) is the data source. */
    for (int attempt = 0; ; attempt++) {
        if (!rhi_begin_frame(s_nr.dev)) return 1;
        upload_synced_textures();  /* pending VRAM/ref uploads ride this frame (W2.2) */
        hd_flush_pending();        /* HD replacement textures queued at draw time */
        native_depth_prepass();    /* shared WORLD-opaque depth for the shadow decal */
        rhi_begin_render_target(s_nr.dev, s_nr.native_rtt, br, bgc, bb, 1.0f);
        draw_native_scene(true);
        rhi_end_render_target(s_nr.dev, s_nr.native_rtt);
        /* Mod postfx over the dump compose too — this path bypasses
         * off_image, so without this call a headless/screenshot-all gate
         * would pass vacuously (plan-review trap). */
        native_postfx_run(s_nr.native_rtt, W, H);
        rhi_begin_offscreen(s_nr.dev, br, bgc, bb, 1.0f);
        rhi_end_offscreen(s_nr.dev);
        rhi_blit_to_swapchain(s_nr.dev);
        rhi_end_frame(s_nr.dev);
        /* Self-heal a first sighting: an HD/mod texture seen for the first
         * time mid-draw only QUEUES its upload and draws nothing for those
         * faces (hd_pending_add). Live presents absorb that as one SD frame,
         * but in a headless run the only frames ARE these dumps — a single
         * screenshot bakes the gap into the PNG (mod-textured faces
         * invisible; cost a "chests don't render" ghost hunt, 2026-07-11).
         * If this draw queued anything new, flush-and-redraw once. */
        if (attempt > 0 || s_hd_pending_n == 0) break;
    }

    unsigned char *nat = malloc(frame_px * 4);          /* native, top-down    */
    unsigned char *ref = with_ref ? malloc(frame_px * 4)     : NULL;  /* reference, top-down  */
    unsigned char *ab  = with_ref ? malloc(frame_px * 4 * 2) : NULL;  /* native | reference   */
    unsigned char *dif = with_ref ? malloc(frame_px * 4)     : NULL;  /* |native - reference| */
    if (!nat || (with_ref && (!ref || !ab || !dif))) {
        free(nat); free(ref); free(ab); free(dif);
        return 1;
    }

    int nw = 0, nh = 0;
    if (!rhi_read_render_target(s_nr.dev, s_nr.native_rtt, nat, frame_px * 4, &nw, &nh)) {
        free(nat); free(ref); free(ab); free(dif);
        return 1;
    }
    if (with_ref) {
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
    }

    /* W1.3: machine-readable native-vs-PsyCross fidelity metric for the headless
     * CI gate (scripts/check_native_fidelity.sh). Computed on the SAME
     * frame-synced pair that -diff.png encodes (the native scene vs the PsyCross
     * readback), so it's only meaningful in a reference-compare mode — gate on
     * stats_enabled (= --renderer diff + --render-stats). Per-pixel max-channel
     * abs diff, matching the live --render-stats semantics (DIFF_STATS_TOL); the
     * line is keyed by the output path so the gate script maps it back to the
     * (scene, frame). Reuses the just-built `dif` (abs per-channel diff). */
    if (s_nr.stats_enabled && with_ref) {
        double sum = 0.0; int maxd = 0; uint64_t over = 0;
        for (size_t i = 0; i < frame_px; i++) {
            int dr = dif[i * 4 + 0], dg = dif[i * 4 + 1], db = dif[i * 4 + 2];
            int dm = dr > dg ? dr : dg; if (db > dm) dm = db;
            sum += dm; if (dm > maxd) maxd = dm;
            if (dm > DIFF_STATS_TOL) over++;
        }
        double mean = frame_px ? sum / (double)frame_px : 0.0;
        double pct  = frame_px ? 100.0 * (double)over / (double)frame_px : 0.0;
        OPL_INFO_F("native",
                   "diff-stats file=%s size=%dx%d mean=%.3f max=%d over%d=%.4f%%",
                   base, W, H, mean, maxd, DIFF_STATS_TOL, pct);
    }

    int rc = 0;
    char path[1100];
    path_with_suffix(base, "-vk", path, sizeof(path));
    if (live) {   /* vk-only widescreen: the live offscreen (window-sized, Hor+) */
        if (openpete_write_png_rgba(path, live, live_w, live_h) == 0)
            OPL_INFO_F("native", "vk screenshot %dx%d (widescreen) -> %s", live_w, live_h, path);
        else rc = 1;
    } else if (openpete_write_png_rgba(path, nat, W, H) == 0)
        OPL_INFO_F("native", "vk screenshot %dx%d -> %s", W, H, path);
    else rc = 1;
    if (with_ref) {
        path_with_suffix(base, "-ab", path, sizeof(path));
        if (openpete_write_png_rgba(path, ab, 2 * W, H) == 0)
            OPL_INFO_F("native", "ab screenshot %dx%d -> %s", 2 * W, H, path);
        else rc = 1;
        path_with_suffix(base, "-diff", path, sizeof(path));
        if (openpete_write_png_rgba(path, dif, W, H) == 0)
            OPL_INFO_F("native", "diff screenshot %dx%d -> %s", W, H, path);
        else rc = 1;
    } else {
        OPL_INFO_F("native", "%s", "-ab/-diff variants skipped (no PsyCross "
                   "reference — null-GPU library mode)");
    }

    free(nat); free(ref); free(ab); free(dif); free(live);
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

/* --record shutdown drain: the async ring still holds the last few presents
 * (queued but not yet cycled out). Flush them to the encoder, in PTS order,
 * before openpete_record_shutdown() finalizes the file — otherwise the tail of
 * the recording is lost. Blocks once (the device is torn down right after). */
void openpete_native_render_record_flush(void)
{
    if (!s_nr.enabled || !openpete_record_active())
        return;
    unsigned char *buf = recbuf_ensure((size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u);
    if (!buf) return;
    int w = 0, h = 0; uint64_t tag = 0;
    while (rhi_drain_readback(s_nr.dev, buf, s_reccap, &w, &h, &tag) && w > 0 && h > 0)
        openpete_record_submit_frame(buf, w, h, /*top_down=*/1, tag);
}

void openpete_native_render_shutdown(void)
{
    if (!s_nr.enabled)
        return;
    rhi_wait_idle(s_nr.dev);
    /* Mod postfx chain objects (S3): ping pool, UBO, cached pipelines
     * (both live registrations and the retirement list). */
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            if (s_pfx_pool[i].t[j]) {
                rhi_destroy_texture(s_nr.dev, s_pfx_pool[i].t[j]);
                s_pfx_pool[i].t[j] = NULL;
            }
    if (s_pfx_ubo) { rhi_destroy_ubo(s_nr.dev, s_pfx_ubo); s_pfx_ubo = NULL; }
    if (s_pfx_prev) { rhi_destroy_texture(s_nr.dev, s_pfx_prev); s_pfx_prev = NULL; }
    if (s_mesh_vb) { rhi_destroy_buffer(s_nr.dev, s_mesh_vb); s_mesh_vb = NULL; }
    {
        openpete_mods_postfx_retire_all();   /* incl. disabled passes' caches */
        openpete_mods_pipeline_retire_all();
        openpete_mods_material_retire_all();
        void* dead[PFX_MAX_PASSES];
        int n;
        while ((n = openpete_mods_postfx_take_retired(dead, PFX_MAX_PASSES)) > 0)
            for (int i = 0; i < n; i++)
                rhi_destroy_pipeline(s_nr.dev, (rhi_pipeline*)dead[i]);
    }
    if (s_nr.ref_tex)      rhi_destroy_texture(s_nr.dev, s_nr.ref_tex);
    if (s_nr.native_rtt)   rhi_destroy_texture(s_nr.dev, s_nr.native_rtt);
    if (s_nr.frozen_tex)   rhi_destroy_texture(s_nr.dev, s_nr.frozen_tex);
    if (s_nr.ss_rtt)       rhi_destroy_texture(s_nr.dev, s_nr.ss_rtt);
    if (s_nr.opaque_depth) rhi_destroy_texture(s_nr.dev, s_nr.opaque_depth);
    if (s_nr.occl_grid) rhi_destroy_texture(s_nr.dev, s_nr.occl_grid);
    if (s_nr.occl_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.occl_pipe);
    if (s_nr.prepass_vb)   rhi_destroy_buffer(s_nr.dev, s_nr.prepass_vb);
    if (s_nr.prepass_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.prepass_pipe);
    if (s_nr.gouraud_sub_ztest_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_sub_ztest_pipe);
    if (s_nr.gouraud_sub_fade_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_sub_fade_pipe);
    if (s_nr.ovl_ss)       rhi_destroy_texture(s_nr.dev, s_nr.ovl_ss);
    if (s_nr.ovl_quad_vb)  rhi_destroy_buffer(s_nr.dev, s_nr.ovl_quad_vb);
    if (s_nr.menu_ss)      rhi_destroy_texture(s_nr.dev, s_nr.menu_ss);
    if (s_nr.menu_quad_vb) rhi_destroy_buffer(s_nr.dev, s_nr.menu_quad_vb);
    if (s_nr.ovl_resolve_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.ovl_resolve_pipe);
    if (s_nr.vram_tex)     rhi_destroy_texture(s_nr.dev, s_nr.vram_tex);
    if (s_nr.diff_quad_vb) rhi_destroy_buffer(s_nr.dev, s_nr.diff_quad_vb);
    if (s_nr.fs_quad_vb)   rhi_destroy_buffer(s_nr.dev, s_nr.fs_quad_vb);
    if (s_nr.overlay_vb)   rhi_destroy_buffer(s_nr.dev, s_nr.overlay_vb);
    if (s_nr.menu_vb)      rhi_destroy_buffer(s_nr.dev, s_nr.menu_vb);
    if (s_nr.menu_glyph_vb) rhi_destroy_buffer(s_nr.dev, s_nr.menu_glyph_vb);
    if (s_nr.scene_vb)     rhi_destroy_buffer(s_nr.dev, s_nr.scene_vb);
    if (s_nr.panels_vb)    rhi_destroy_buffer(s_nr.dev, s_nr.panels_vb);
    if (s_nr.freeze_overlay_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.freeze_overlay_pipe);
    if (s_nr.resolve_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.resolve_pipe);
    if (s_nr.diff_pipe)    rhi_destroy_pipeline(s_nr.dev, s_nr.diff_pipe);
    if (s_nr.scene_tex_hud_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_hud_pipe);
    if (s_nr.hd_tex_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.hd_tex_pipe);
    if (s_nr.hd_cutout_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.hd_cutout_pipe);
    if (s_nr.hd_cutout_mark_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.hd_cutout_mark_pipe);
    if (s_nr.hd_scene_blend_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.hd_scene_blend_pipe);
    if (s_nr.hd_scene_add_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.hd_scene_add_pipe);
    hd_cache_shutdown();
    texture_pack_shutdown();
    if (s_nr.gouraud_hud_sub_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_hud_sub_pipe);
    if (s_nr.gouraud_hud_blend_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_hud_blend_pipe);
    if (s_nr.gouraud_hud_pipe)   rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_hud_pipe);
    if (s_nr.scene_tex_sub_decal_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_sub_decal_pipe);
    if (s_nr.scene_tex_sub_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_sub_pipe);
    if (s_nr.scene_tex_add_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_add_pipe);
    if (s_nr.scene_tex_blend_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_blend_pipe);
    if (s_nr.scene_tex_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_pipe);
    if (s_nr.gouraud_noz_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_noz_pipe);
    if (s_nr.portal_mask_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.portal_mask_pipe);
    if (s_nr.gouraud_mark_pipe)   rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_mark_pipe);
    if (s_nr.scene_tex_mark_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_mark_pipe);
    if (s_nr.gouraud_fill_pipe)   rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_fill_pipe);
    if (s_nr.gouraud_add_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_add_pipe);
    if (s_nr.gouraud_sub_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_sub_pipe);
    if (s_nr.gouraud_blend_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_blend_pipe);
    if (s_nr.gouraud_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_pipe);
    if (s_nr.vram_pipe)    rhi_destroy_pipeline(s_nr.dev, s_nr.vram_pipe);
    if (s_nr.blit_pipe)    rhi_destroy_pipeline(s_nr.dev, s_nr.blit_pipe);
    rhi_destroy(s_nr.dev);
    SDL_DestroyWindow(s_nr.window);
    free(s_nr.readback);
    /* s_nr.vram_mirror is the static native VRAM model (native_vram.c) — not freed. */
    free(s_recbuf); s_recbuf = NULL; s_reccap = 0;   /* --record async ring host buffer */

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
