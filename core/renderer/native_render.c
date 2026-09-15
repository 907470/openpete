/* core/renderer/native_render.c — the native Vulkan renderer: window, scene
 * ring, pipelines, the display path and the reference/diff panels.
 *
 * See openpete_native_render.h for the contract. The window hosts the RHI
 * with PsyCross's VRAM mirrored into a native R16_UINT texture each present;
 * panel 0 renders the PSX display region cropped to the visible viewport.
 * The DISPENV disp rect (512x240, from PutDispEnv) and the DRAWENV clip rect
 * (512x224, from PutDrawEnv) are captured; the panel samples the full disp
 * width but only clip.h rows, dropping the symmetric 8px overscan bands, and
 * is fitted at the reference aspect so it lines up with the reference panel.
 * The front-buffer origin follows the per-frame double-buffer flip via the
 * material tpage. Layout: [native-display | reference].
 *
 * Determinism: nothing here touches PSX RAM or the `.s` verdict producers.
 */

#include "openpete_native_render.h"
#include "openpete_present_thread.h"
#include "openpete_pace_log.h"
#include "openpete_perf_stats.h"
#include "openpete_profile.h"   /* --profile-out forwarding shim */
#include "openpete_menu.h"
#include "openpete_overlay_anchor.h"
#include "openpete_osd.h"
#include "openpete_knobs.h"   /* env-epoch for cached live knobs */
#include "openpete_devui.h"
#include "openpete_mods.h"   /* openpete_mods_texture_get (mod-texture runs) */

#include "openpete_log.h"
#include "openpete_time.h"   /* op_now — overlay's own sub-tick alpha (camera-independent) */
#include "openpete_subtick_view.h" /* engagement report at shutdown */
#include "openpete_ident_audit.h"  /* headless bring-up gate: the audit is a consumer */
#include "openpete_interp.h" /* deterministic present clock for the overlay sub-tick alpha */
#include "openpete_interp_objs.h" /* tick stamp + period for the overlay sub-tick alpha */
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
#include "openpete_terrain_cache.h" /* terrain sector-mesh GPU cache (retained terrain) */
#include "texture_pack.h"           /* texture-replacement registry */
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

/* Push constants for the HD texture-replacement pipes: the replaced texel box,
 * plus the per-channel fog transform applied to the sampled replacement
 * (see tp_fog_affine). a/b are vec4-aligned; the shaders read .rgb. */
/* Push block for the hd_tex*.frag pipes (must match their HdRect layout).
 * a/b = the per-face CLUT-ladder rung affine (identity when unfogged) — the
 * flat model. aN/bN/lad drive the CONTINUOUS model: aN.rgb/bN.rgb are the
 * affine at the DEEPEST authored row, aN[3] > 0 enables the mode, and the
 * shader fades identity->deepest by t = clamp(w*lad[0] + lad[1], 0, 1) per
 * fragment (w = true view-Z) — the same ramp og's per-face rows quantize. */
typedef struct {
    float u0, v0, uw, vh;
    float a[4]; float b[4];
    float aN[4]; float bN[4];
    float lad[4];
} hd_pc;

/* fog_delta channel: bits 7:0 = the face's CLUT-ladder row (0..15), plus the
 * feeder-declared flags for the continuous model above (values shared with
 * feeders via texture_pack.h). */
#define TP_FOG_DELTA_RUNG    0xFF
#define TP_FOG_DELTA_CONT    OPENPETE_TP_FOG_CONT
#define TP_FOG_DELTA_FOGBIAS OPENPETE_TP_FOG_FOGBIAS

/* Per-draw material for MOD MATERIAL pipelines — psx_material plus the HD
 * texture-pack fields (must match the op_psx_material push block in
 * include/openpete_psx_material.glsl, std430: the fog vec4s start at offset
 * 48, hence the explicit pad after the vec2 pair). hd == 1 means the draw's
 * replacement is bound at set 1 and hd_* describe it (file texel box in page
 * coords + CLUT-ladder fog affine); hd == 0 leaves op_pack_shade() on the
 * stock VRAM decode. */
typedef struct {
    int32_t tpage_x, tpage_y;
    int32_t clut_x, clut_y;
    int32_t bpp;
    int32_t hd;
    float   hd_u0, hd_v0;          /* pack file's texel box: origin */
    float   hd_uw, hd_vh;          /*   and size, page texel coords */
    float   _pad0, _pad1;
    float   fog_a[4], fog_b[4];
} psx_material_hd;

/* How many panels the window shows (VRAM + reference). */
#define NR_PANELS 2

/* Native 3D scene: screen-space triangles the game pushes in
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
#define OVL_MAX_VERTS 16384  /* overlay (3D text-moby glyph) vertex cap (see draw_perf_overlay);
                             * sized for the 4-line detailed readout, not a 1-line counter */
#define MOD_OVL_MAX_VERTS 16384 /* mod draw-service overlay verts (see draw_mod_overlay) */
#define MENU_MAX_VERTS 256   /* in-game menu panel/border/selection quads (see draw_menu_overlay) */
#define MENU_GLYPH_MAX 32768 /* in-game menu 3D glyph text verts (labels/values/title);
                              * must match MENU_TXT_MAX (text_moby_native.c) — 8 rows
                              * overflowed 16384 (truncated border/last rows) */
/* Stats-overlay supersampling: render the glyphs at OVL_SS_FACTOR× into a small
 * RGBA target covering the top-left ovl_region_w() × ovl_region_h() of the frame (in
 * win_h-fraction units, matching the glyph layout's win_h-relative coords), then
 * box-downsample + alpha-blend onto the frame so the corner counter is crisp at
 * any resolution. The region is generous enough for "FPS: 9999".
 *
 * Height follows the stats level: the detailed readout is four lines, and a
 * region sized for one would clip the rest. It is not simply always tall —
 * the supersample target holds OVL_SS_FACTOR^2 samples per pixel of it, so
 * the unused height would cost real GPU memory in the common case. */
#define OVL_SS_FACTOR  4
/* Fallback region, used only until the game-side builder pushes a real extent
 * (openpete_native_set_overlay_extent) — before the glyph font has ever
 * cached, nothing draws anyway. The builder owns the rect because it is the
 * side that can MEASURE the text; the renderer only reads it back. */
#define OVL_REGION_W   0.30f
#define OVL_REGION_H   0.05f
/* Settings-menu glyph supersample factor. The menu region is the whole (tall)
 * panel, so this is smaller than OVL_SS_FACTOR to keep the target memory sane at
 * high resolutions (2× = 4 samples/px, a clear anti-alias). */
#define MENU_SS_FACTOR 2
/* OSD toast supersample. 2x, like the menu and for the same reason: this
 * layer re-renders on EVERY sim tick (the gold shimmer is baked into vertex
 * colours at emit time, so it cannot be built once), and 4x over eight slots
 * buys nothing the eye can find. The grid is FIXED — max visible x max slot
 * extent — and reallocated only on a window resize, because every SS target
 * rebuild waits the device idle and toasts arrive, expire and repaint many
 * times a second. */
#define OSD_SS_FACTOR   2
#define OSD_SLOT_W      0.62f   /* max toast box width,  overlay units */
#define OSD_SLOT_H      0.075f  /* max toast box height, overlay units */
#define OSD_GLYPH_MAX   24576   /* 8 toasts of border + wrapped text        */
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
    float koff;     /* og OT key — see openpete_scene_tri */
    int prop;       /* authored embedded prop — see openpete_scene_tri */
    float rank;     /* og OT paint rank — see openpete_scene_tri */
    int mordinal;   /* present-local moby ordinal — see openpete_scene_tri */
    int category;   /* draw layer (OPENPETE_SCENE_*) — add_tri's explicit arg */
    int carry;      /* 2D-UI sub-tick carry: 0/1 from add_tri; 2 = re-applied marker
                     * stamped by apply_overlay so the next canonical capture skips it */
    int group;      /* per-portal grouping id (add_tri_grouped; plain add_tri: 0) */
    uint32_t tag;   /* provenance (mod-shaders S6): sticky set_tag value; joins
                     * the material key ONLY when tag_split_on() (else inert) */
    int fog_delta;  /* sticky set_fog_delta value — the CLUT-ladder row this
                     * tri's palette was shifted to (0 = unfogged base row) */
} scene_tri_t;
/* C99 compile-time prefix check: if a field is added to openpete_scene_tri
 * without mirroring it here (or vice versa), this array gets a negative size. */
typedef char nr_assert_scene_tri_prefix[
    (offsetof(scene_tri_t, category) == sizeof(openpete_scene_tri)) ? 1 : -1];
static scene_tri_t s_scene[MAX_SCENE_TRIS];      /* accumulating (frame N) */
static int                s_scene_n;

/* ---- eager scene ring ----------------------------------------------------
 *
 * Under eager production the thread running the guest extracts every scene
 * of a span ahead of time and a present thread composes them on the display
 * grid, through the next tick's execution. The two never share the live
 * accumulator: publish() copies the accumulator and EVERY other piece of
 * per-present state the compose reads (sidecars, draw-env, the mod overlay,
 * the interned material slots, the shadow-decal camera,
 * the present ctx) into a ring slot, and the compose reads the slot through
 * the accessors below. The live globals stay the source for the inline
 * (non-eager) compose, so the two paths differ only in where s_ps_slot
 * points.
 *
 * Slot lifecycle: FREE -> PENDING (published) -> PRESENTING (picked) ->
 * PRESENTED (its RHI frame submitted) -> FREE once that frame's fence has
 * been waited, or straight to FREE when a newer scene supersedes it
 * unpresented. The retained terrain cache asks stamp_live() before it lets a
 * freed arena range be rewritten, which is what keeps a burst's builds from
 * overwriting bytes a still-pending scene draws. */
enum { SS_FREE = 0, SS_PENDING, SS_PRESENTING, SS_PRESENTED };
#define SCENE_RING 48
typedef struct {
    scene_tri_t *tri; int cap, n;
    uint32_t bg, fade, freeze; int hud_under, actor_front;
    unsigned  frame;               /* openpete_frame at extraction */
    uint32_t  stamp;               /* terrain-cache build stamp (scene id) */
    uint32_t  drawenv_clear;
    int       geom[8];
    openpete_hud_vert *ovl; int ovl_cap, ovl_n;
    openpete_shadow_decal_params shadow;
    uint32_t  pctx_tick; float pctx_alpha; int pctx_canonical;
    openpete_mods_material_view_t matview;   /* interned material slots at publish */
    /* scheduling */
    uint32_t  scene_id;
    op_time_t target_ns;           /* the present instant alpha was computed for */
    double    alpha; int paired; int kind;   /* OP_PACE_CANONICAL / _INTERMEDIATE */
    int       state;
    uint64_t  presented_serial;    /* rhi frame serial of its present */
} scene_slot_t;
static scene_slot_t s_ring[SCENE_RING];
static int          s_ring_head = -1;          /* last published */
static SDL_Mutex   *s_ring_mx;
static const scene_slot_t *s_ps_slot;          /* compose source: NULL = live */
static uint32_t ps_drawenv_clear(void);
static const openpete_shadow_decal_params *ps_shadow(void);
static unsigned ps_frame(void);
static void     ps_apply_geometry(void);
static uint32_t     s_eager_stamp;             /* build stamp for the scene being extracted */
static uint32_t     s_scene_id_next = 1;
static struct { unsigned published, refused_full, superseded, presented; } s_ring_census;


/* Feed-time accumulation for the 2D-overlay carry (committed by
 * capture_overlay — see the carry block further down). The live scene is
 * wiped by EVERY sub-tick re-extract, so scavenging s_scene at capture time
 * raced those wipes: a 2D feed that landed mid-VBL with any intermediate
 * present between it and the canonical capture was wiped unseen, and that
 * tick's carry lost the sprite (the collected-egg HUD sprite froze/vanished
 * at high --render-fps — the deterministic egg.bk2 f3603 blink).
 * Carry-eligible tris are instead copied here AT ADD TIME (apply_overlay's
 * re-applies bypass add_tri, so re-applied carry never re-accumulates). */
#define OVERLAY_CARRY_MAX 1024
static scene_tri_t s_overlay_accum[OVERLAY_CARRY_MAX];
static int                s_overlay_accum_n;
static int                s_overlay_accum_dropped;
static void (*s_overlay_precapture)(void);

void openpete_native_scene_register_overlay_precapture(void (*fn)(void)) {
    s_overlay_precapture = fn;
}

/* Provenance-tag state (S6). s_tag_split_ext is armed by the material-
 * selector registry (S8); the env flags are the dev instruments. */
static uint32_t s_cur_tag;        /* sticky, stamped by add_tri */
/* Accepted tris per provenance-tag family (tag bits 31:28) in the scene
 * being accumulated. Reset with the scene. Read at the canonical commit by
 * the render-path feed audit: a family that fed geometry belongs to a path
 * that must be canon-drawn, or the sub-tick spine drops it. */
static uint32_t s_scene_fam_n[16];
/* The terrain fog ladder is exactly 16 rows (delta 0..15); anything outside
 * that is not a ladder row and must never reach the CLUT arithmetic, which
 * would index VRAM out of bounds. */
#define TP_FOG_DELTA_MAX 15
static int s_cur_fog_delta;       /* sticky, stamped by add_tri */
static int      s_tag_tint = -1;  /* OPENPETE_TAG_TINT: tint colours by tag */
static int      s_tag_split_env = -1;   /* OPENPETE_TAG_SPLIT: force key join */
static int      s_tag_split_ext = 0;    /* armed by registered selectors (S8) */

void openpete_native_scene_set_tag(uint32_t tag) { s_cur_tag = tag; }
void openpete_native_scene_tag_family_counts(uint32_t out[16]) {
    memcpy(out, s_scene_fam_n, sizeof s_scene_fam_n);
}
/* Sticky CLUT-ladder row (see openpete_native_scene_set_fog_delta). Ambient
 * state rather than a scene-tri field on purpose: the feeder-facing tri is
 * filled field-by-field by a dozen feeders that never memset it, so a new
 * struct member would read as garbage everywhere except the one feeder that
 * sets it — the layout assert catches a missing MIRROR, not a missing init. */
void openpete_native_scene_set_fog_delta(int delta) {
    /* Row in bits 7:0 as documented; feeders may add the TP_FOG_DELTA_CONT /
     * _FOGBIAS flags above the row byte (continuous pack-fog model). An
     * out-of-range row still zeroes the whole value, flags included. */
    int rung = delta & TP_FOG_DELTA_RUNG;
    s_cur_fog_delta = (rung <= TP_FOG_DELTA_MAX)
                      ? (delta & (TP_FOG_DELTA_RUNG | TP_FOG_DELTA_CONT |
                                  TP_FOG_DELTA_FOGBIAS))
                      : 0;
}
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

/* The DEV tag instruments only (OPENPETE_TAG_SPLIT forces the batch key
 * join, OPENPETE_TAG_TINT recolours by tag): their semantics live in the
 * CPU batcher, so they disqualify the retained-terrain cache for the whole
 * run. A selector-armed split does NOT — sectors a selector could override
 * are claimed back per sector by the terrain feeder. */
static int tag_split_dev(void) {
    (void)tag_split_on();          /* lazy env init lives there */
    return s_tag_split_env > 0 || s_tag_tint > 0;
}

/* Mod custom-pipeline drain (S7) — defined with the postfx executor
 * below; called from compose_variant (mod 3D under mod HUD). */
/* Postfx executor + SCENE-point split (S3/S5) — defined below the
 * postfx typedef; compose_variant (above them) calls via these decls. */
static int native_postfx_run(int point, rhi_texture* target, int w, int h);
static int native_postfx_point_active(int point);
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
/* Shared with the retained terrain cache: a terrain material pass binds the
 * same set-3 pair a walked material draw binds, so both residencies read one
 * uniform source. */
struct rhi_ubo *openpete_native_material_ubo(void) { return s_mat_ubo; }
static uint32_t           s_scene_bg;  /* panel-0 background fill (rgba); 0 = none */

/* Reference frame-sync: the native scene is built from the FRESHLY-computed
 * geometry (frame N), but PsyCross's reference shows its display FBO, which lags
 * the native geometry (the OT PsyCross draws is the one Spyro built a frame or
 * two earlier — PSX double-buffer + the build-vs-draw split). Comparing the
 * scene by an integer DEPTH: keep a ring of recent completed frames and present
 * the one `depth` frames back. Depth is 0 (the live frame) unless
 * OPENPETE_NATIVE_SCENE_DELAY=<n> forces an explicit integer depth. */
#define SCENE_HIST 4   /* ring of completed-frame snapshots; supports delay depth 0..3 */
static scene_tri_t s_scene_hist[SCENE_HIST][MAX_SCENE_TRIS]; /* completed-frame ring */
static int                s_scene_hist_n[SCENE_HIST];
static unsigned           s_scene_hist_frame[SCENE_HIST]; /* completed frame's stamp */
static uint32_t           s_scene_hist_bg[SCENE_HIST];
static uint32_t           s_scene_hist_fade[SCENE_HIST];  /* full-screen fade overlay, per snapshot */
static uint32_t           s_scene_hist_freeze[SCENE_HIST]; /* freeze-backdrop overlay, per snapshot */
static int                s_scene_hist_hud_under[SCENE_HIST]; /* flat-HUD-under-player flag, per snapshot */
static int                s_scene_hist_actor_front[SCENE_HIST]; /* actors-in-front (dragon rescue) flag, per snapshot */
static int                s_scene_hist_head = -1;  /* ring index of the most-recent completed frame */
static int                s_scene_hist_count = 0;  /* completed frames recorded (warmup guard) */
static int                s_scene_delay = -2;   /* -2 uninit; >=0 = depth */

/* 1 while openpete_native_render_screenshot_all composes: the -vk screenshot
 * sits beside the -og PsyCross capture. */
static int s_scene_shot_compose = 0;
/* 1 when the shot compose may use the lagged history slot (headless — the
 * canonical feeder walk maintains the ring there). Set with s_scene_shot_compose
 * by the screenshot_all wrapper (s_nr is not yet declared at this point). */
static int s_scene_shot_lag = 0;
/* Intermediate (sub-tick alpha=0.5) capture: the re-extract builds the
 * interpolated scene into the CURRENT s_scene and never pushes it to the frame
 * history, so the compose must read depth 0 — the normal shot_compose delay
 * would read a lagged history slot (an empty one, since the re-extract's
 * scene_begin pushes nothing). Set only around openpete_native_render_screenshot_all
 * for an intermediate dump. */
static int s_scene_shot_nodelay = 0;
void openpete_native_render_set_capture_nodelay(int on) { s_scene_shot_nodelay = on ? 1 : 0; }

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
    /* Intermediate capture reads the freshly-built current scene (see the flag). */
    if (s_scene_shot_nodelay) return 0;
    /* The screenshot compose reads the CURRENT scene: the frame keyed F shows
     * the newest tick's feed. The live present composes within a fraction of a
     * tick of that instant (the eager lag tracks the tick's execution tail),
     * so this is the fixed definition nearest the window. The PsyCross -og
     * capture is deferred one present to sit on the same tick. */
    (void)s_scene_shot_lag;
    return 0;
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

/* One-shot census: the DISCRETE half of a composed scene, hashed.
 *
 * A scene tri carries two kinds of field. Continuous ones — vertex position,
 * UV, colour — move with the interpolation alpha and are exactly what a
 * re-lerp of an already-composed scene would reproduce. Discrete ones —
 * whether a tri exists at all, its texture page, CLUT, bit depth, blend mode,
 * draw category, prop, portal group, tag, fog row — come from decisions the
 * game's own extraction makes: culling, LOD tier selection, palette choice.
 *
 * The question this answers: within ONE tick's span of alphas, does
 * re-running the extraction ever decide differently? If the signature is
 * constant across a tick's presents, then re-extracting per present buys
 * nothing a transform re-lerp could not, and a composed scene can be reused
 * for the presents the tick's own execution would otherwise leave frozen.
 *
 * OPENPETE_SCENE_SIG=1. Census only: no path depends on it. */
/* Two signatures, because "the scene changed" is not one question.
 *
 *   MATERIAL SET — the distinct discrete keys present (texture page, CLUT,
 *   bpp, blend, category, prop, group, tag, fog row). It moves only when the
 *   extraction DECIDES differently: a new texture, an LOD tier that picks a
 *   different page, a blend mode change, an object entering or leaving.
 *
 *   TRI MULTISET — the same keys with their counts. It also moves when the
 *   same objects simply contribute a different number of triangles, which is
 *   what sub-tick camera motion does at the frustum edge.
 *
 * A scene reuse that re-lerps transforms reproduces the second but not the
 * first, so the two must be counted apart. Both are order-independent (XOR
 * over distinct keys, sum over all tris), because tri order renumbers freely
 * as counts shift and an order-sensitive hash would report that as a change.
 */
#define SIG_TBL 4096u
static uint64_t scene_tri_key(const scene_tri_t *t)
{
    uint64_t k = (uint64_t)(uint32_t)t->tag;
    k = k * 31 + (uint64_t)(uint32_t)t->tpage_x;
    k = k * 31 + (uint64_t)(uint32_t)t->tpage_y;
    k = k * 31 + (uint64_t)(uint32_t)t->clut_x;
    k = k * 31 + (uint64_t)(uint32_t)t->clut_y;
    k = k * 31 + (uint64_t)(uint32_t)t->bpp;
    k = k * 31 + (uint64_t)(uint32_t)t->blend;
    k = k * 31 + (uint64_t)(uint32_t)t->prop;
    k = k * 31 + (uint64_t)(uint32_t)t->group;
    k = k * 31 + (uint64_t)(uint32_t)t->fog_delta;
    k = k * 31 + (uint64_t)(uint32_t)t->category;
    k = k * 31 + (uint64_t)(uint32_t)t->textured;
    return k ? k : 1;      /* 0 marks an empty slot */
}

static void scene_discrete_sig(uint64_t *set_h, uint64_t *cnt_h, int *distinct)
{
    static uint64_t tbl[SIG_TBL];
    memset(tbl, 0, sizeof tbl);
    uint64_t sh = 0, ch = 0;
    int dn = 0;
    for (int i = 0; i < s_scene_n; i++) {
        const uint64_t k = scene_tri_key(&s_scene[i]);
        ch += k * 1099511628211ull;                 /* order-independent */
        uint32_t slot = (uint32_t)(k % SIG_TBL);
        for (uint32_t probe = 0; probe < SIG_TBL; probe++) {
            uint32_t idx = (slot + probe) % SIG_TBL;
            if (tbl[idx] == k) break;               /* seen */
            if (tbl[idx] == 0) { tbl[idx] = k; sh ^= k * 0x9E3779B97F4A7C15ull; dn++; break; }
        }
    }
    *set_h = sh; *cnt_h = ch; *distinct = dn;
}

static int scene_sig_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OPENPETE_SCENE_SIG"); on = (e && *e && *e != '0'); }
    return on;
}

/* OPENPETE_SCENE_POS_SIG=1: the CONTINUOUS half, per draw category — an
 * order-independent hash over every vertex's screen position (exact float
 * bits) with the tri count, plus one bucket for the particle HUD bursts
 * (tag path 5, sel 4 and 5). A display policy that must leave the HUD
 * where the game put it (--fov) is checked by this: the HUD, HUD3D,
 * HUD_PANEL and OVERLAY hashes and the burst bucket are equal across the
 * policy's settings while the world categories differ. Census only. */
static int scene_pos_sig_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OPENPETE_SCENE_POS_SIG"); on = (e && *e && *e != '0'); }
    return on;
}
static void scene_pos_sig_report(int canonical)
{
    enum { NCAT = 16 };
    uint64_t h[NCAT] = {0}, hb = 0; int n[NCAT] = {0}, nb = 0;
    for (int t = 0; t < s_scene_n; t++) {
        const scene_tri_t *tr = &s_scene[t];
        uint64_t k = 0;
        for (int i = 0; i < 3; i++) {
            uint32_t bx, by;
            memcpy(&bx, &tr->v[i].sx, 4); memcpy(&by, &tr->v[i].sy, 4);
            k = (k + bx) * 0x9E3779B97F4A7C15ull;
            k = (k + by) * 0x9E3779B97F4A7C15ull;
        }
        k ^= k >> 29;
        int c = tr->category;
        if (c < 0 || c >= NCAT) c = NCAT - 1;
        h[c] += k; n[c]++;
        if ((tr->tag >> 28) == 5u) {
            unsigned sel = tr->tag & 0xFFu;
            if (sel == 4u || sel == 5u) { hb += k; nb++; }
        }
    }
    extern unsigned openpete_frame;
    fprintf(stderr, "[scene-pos] f=%u leg=%s", openpete_frame, canonical ? "CANON" : "inter");
    static const char *names[14] = { "WORLD", "ACTOR", "HUD", "PLAYER", "OVERLAY", "HUD_PANEL", "HUD3D",
                                     "PORTAL_CYC", "PORTAL_MASK", "PORTAL_GUARD", "SKY", "WORLD_FILL", "SHADOW", "ACTOR_FRONT" };
    for (int c = 0; c < 14; c++)
        fprintf(stderr, " %s=%016llx/%d", names[c], (unsigned long long)h[c], n[c]);
    fprintf(stderr, " BURST=%016llx/%d\n", (unsigned long long)hb, nb);
}

void openpete_native_scene_sig_report(int canonical)
{
    if (scene_pos_sig_on()) scene_pos_sig_report(canonical);
    if (!scene_sig_on()) return;
    extern unsigned openpete_frame;
    uint64_t set_h = 0, cnt_h = 0; int distinct = 0;
    scene_discrete_sig(&set_h, &cnt_h, &distinct);
    int cw = 0, ca = 0, cs = 0, cp = 0, ch2 = 0;
    for (int t = 0; t < s_scene_n; t++) {
        switch (s_scene[t].category) {
        case OPENPETE_SCENE_WORLD: cw++; break;
        case OPENPETE_SCENE_ACTOR: case OPENPETE_SCENE_ACTOR_FRONT: ca++; break;
        case OPENPETE_SCENE_SKY: cs++; break;
        case OPENPETE_SCENE_PLAYER: cp++; break;
        case OPENPETE_SCENE_HUD: ch2++; break;
        default: break;
        }
    }
    fprintf(stderr, "[scene-sig] f=%u leg=%s alpha=%.4f n=%d d=%d set=%016llx cnt=%016llx W=%d A=%d S=%d P=%d H=%d freeze=%08x stamp=%08x\n",
            openpete_frame, canonical ? "CANON" : "inter",
            openpete_pace_last_alpha(), s_scene_n, distinct,
            (unsigned long long)set_h, (unsigned long long)cnt_h, cw, ca, cs, cp, ch2, s_scene_freeze,
            s_eager_stamp ? s_eager_stamp : openpete_frame);
}

static void fov_latch(void);
void openpete_native_scene_begin(void) { fov_latch(); s_scene_n = 0; memset(s_scene_fam_n, 0, sizeof s_scene_fam_n); s_scene_bg = 0; s_scene_fade = 0; s_scene_freeze = 0; s_scene_hud_under = 0; s_scene_actor_front = 0; openpete_mods_material_frame_reset(); /* S8b: per-present interned slots */ }

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
        fov_latch();
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
            /* OPENPETE_SCENE_DROP_BLEND=<n> — drop every completed scene
             * triangle with that blend class (0 opaque / 1 alpha / 2 additive
             * / 3 subtractive). Isolates which transparency class owns an
             * artifact without touching any feeder. */
            { static int db = -2;
              if (db == -2) { const char* e = getenv("OPENPETE_SCENE_DROP_BLEND");
                  db = (e && *e) ? atoi(e) : -1; }
              if (db >= 0) {
                int w = 0;
                for (int ti = 0; ti < s_scene_n; ti++)
                    if (s_scene[ti].blend != db) { if (w != ti) s_scene[w] = s_scene[ti]; w++; }
                s_scene_n = w; } }
            alpha_census(s_scene, s_scene_n);  /* census the completed scene (runs even headless) */
            int slot = (s_scene_hist_head + 1) % SCENE_HIST;
            if (s_scene_n > 0) memcpy(s_scene_hist[slot], s_scene, (size_t)s_scene_n * sizeof(s_scene[0]));
            s_scene_hist_n[slot] = s_scene_n; s_scene_hist_bg[slot] = s_scene_bg;
            s_scene_hist_frame[slot] = s_scene_frame;
            s_scene_hist_fade[slot] = s_scene_fade;
            s_scene_hist_freeze[slot] = s_scene_freeze;
            s_scene_hist_hud_under[slot] = s_scene_hud_under;
            s_scene_hist_actor_front[slot] = s_scene_actor_front;
            s_scene_hist_head = slot;
            if (s_scene_hist_count < SCENE_HIST) s_scene_hist_count++;
        }
        s_scene_n = 0; memset(s_scene_fam_n, 0, sizeof s_scene_fam_n); s_scene_bg = 0; s_scene_fade = 0; s_scene_freeze = 0; s_scene_hud_under = 0; s_scene_actor_front = 0; s_scene_frame = frame;
        /* Per-instance material slots are per-feed-cycle: reset here (the
         * canonical present's reset) as well as in scene_begin (the sub-tick
         * one). Without this, a TIME-VARYING per-instance uniform (e.g. an
         * animated pulse) mints a fresh slot every canonical feed — constant
         * uniforms dedup so the leak stays invisible — until the table fills
         * and the material silently falls back to stock. */
        openpete_mods_material_frame_reset();
    }
}

/* Accumulating-scene save/restore. A re-extract rebuilds s_scene in place, so a
 * caller that drives one purely to look at the result must put the canonical
 * scene back: the next openpete_native_scene_begin_frame snapshots whatever
 * sits in s_scene into the frame-history ring, and censuses it on the way past.
 * Saves only the live prefix (s_scene_n tris); the buffer is grown on demand and
 * never shrinks, so a steady-state caller allocates once. Returns 0 on success;
 * a failed save returns nonzero and leaves nothing to restore. */
static scene_tri_t *s_scene_saved;
static int          s_scene_saved_cap;
static int          s_scene_saved_n = -1;   /* -1 = nothing saved */
static uint32_t     s_scene_saved_fam[16];
static uint32_t     s_scene_saved_bg, s_scene_saved_fade, s_scene_saved_freeze;
static int          s_scene_saved_hud_under, s_scene_saved_actor_front, s_scene_saved_dropped;

int openpete_native_scene_save(void) {
    if (s_scene_n > s_scene_saved_cap) {
        scene_tri_t *g = realloc(s_scene_saved, (size_t)s_scene_n * sizeof(scene_tri_t));
        if (!g) return 1;
        s_scene_saved = g; s_scene_saved_cap = s_scene_n;
    }
    if (s_scene_n > 0) memcpy(s_scene_saved, s_scene, (size_t)s_scene_n * sizeof(scene_tri_t));
    s_scene_saved_n = s_scene_n;
    memcpy(s_scene_saved_fam, s_scene_fam_n, sizeof s_scene_fam_n);
    s_scene_saved_bg = s_scene_bg; s_scene_saved_fade = s_scene_fade;
    s_scene_saved_freeze = s_scene_freeze;
    s_scene_saved_hud_under = s_scene_hud_under;
    s_scene_saved_actor_front = s_scene_actor_front;
    s_scene_saved_dropped = s_scene_dropped;
    return 0;
}

void openpete_native_scene_restore(void) {
    if (s_scene_saved_n < 0) return;
    if (s_scene_saved_n > 0) memcpy(s_scene, s_scene_saved, (size_t)s_scene_saved_n * sizeof(scene_tri_t));
    s_scene_n = s_scene_saved_n;
    memcpy(s_scene_fam_n, s_scene_saved_fam, sizeof s_scene_fam_n);
    s_scene_bg = s_scene_saved_bg; s_scene_fade = s_scene_saved_fade;
    s_scene_freeze = s_scene_saved_freeze;
    s_scene_hud_under = s_scene_saved_hud_under;
    s_scene_actor_front = s_scene_saved_actor_front;
    s_scene_dropped = s_scene_saved_dropped;
    s_scene_saved_n = -1;
}

/* The frame stamp of the scene currently accumulating in s_scene
 * (0xFFFFFFFF = none yet). Late feeders use it to append ONLY to a live
 * scene: calling begin_frame with a newer frame would snapshot-and-wipe a
 * held scene during load stalls. */
unsigned openpete_native_scene_frame(void) { return s_scene_frame; }

/* Stamp of the scene scene_present_buf() would return under the CURRENT
 * selection (delay/shot-lag included) — what the retained-terrain freshness
 * gate compares against, so a frozen/lagged compose keeps its retained
 * content and a stale-state compose (menus) drops it. */
static unsigned scene_present_stamp(void) {
    if (s_ps_slot) return s_ps_slot->stamp;
    int depth = scene_delay_depth();
    if (depth > 0 && depth <= s_scene_hist_count && s_scene_hist_head >= 0) {
        int slot = (s_scene_hist_head - (depth - 1) + SCENE_HIST * 4) % SCENE_HIST;
        return s_scene_hist_frame[slot];
    }
    return s_scene_frame;
}


/* --vertex-precision (`--vertex-precision integer`, live via the O-menu). When
 * set, openpete_native_scene_add_tri_grouped rounds each 3D-world vertex's PSX
 * screen coordinate (sx,sy — still in 512x240 display space at that point) to
 * the integer pixel grid, reproducing the original GTE's int16 SXY snap: the
 * classic PS1 polygon wobble. 0 = smooth sub-pixel (default). Depth (z) and UVs
 * are left alone, so the true Z-buffer + perspective-correct texturing survive.
 * A file-static (not an s_nr field) so the feed chokepoint above s_nr can read
 * it; seeded from opts->vertex_precision, mutated live by the setter below. */
static int s_vertex_snap = 0;
/* Sticky feed state (set_presnap): the current feeder emits coordinates that
 * are ALREADY snap-consistent, so the chokepoint snap below must not touch
 * them. The terrain feeder self-snaps (its near/HQ subdivide places boundary
 * midpoints at exact off-integer positions ON snapped parent edges — a
 * per-vertex snap here would break that collinearity and re-open T-junction
 * cracks); every other feeder leaves this 0 and gets the chokepoint snap. */
static int s_cur_presnap = 0;
/* --raster-quantize multiplier (0 = off): engine SCENE-point resample of the
 * composed 3D scene onto the 512Nx240N virtual PSX raster — an integer
 * multiple of the vertex-snap grid, so snapped edges always land on cell
 * boundaries (see native_postfx_run). One point sample per cell, as the
 * hardware rasterizer produced. */
static int s_raster_mult = 0;
static int s_dither = 0;       /* --dither: PS1 15-bit colour + 4x4 hardware dither (engine SCENE pass) */
/* --texture-mapping (0 = perspective, 1 = affine): the live mode plus the
 * queued one. The mode is a vertex spec constant baked into every scene
 * pipeline, so a change cannot apply mid-frame like the knobs above — the
 * setter queues it and texmap_apply_pending() re-bakes all pipelines at the
 * next present boundary (rhi_rebuild_pipelines under a device idle). */
static int s_texmap_affine  = 0;
static int s_notex          = 0;   /* untextured lighting view (spec-const 2) */
static int s_notex_pending  = 0;
static int s_texmap_pending = 0;

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
        s_scene_fam_n[s_cur_tag >> 28]++;
        s_scene[s_scene_n].fog_delta = s_cur_fog_delta;  /* sticky (set_fog_delta) */
        if (s_cur_tag && tag_tint_on()) {
            /* Coverage instrument: hash the tag to a stable tint and modulate
             * the vertex colours — "which source produced this pixel".
             * The tint bits are read out of the LOW 23 bits, so the hash has to
             * avalanche: a bare Knuth multiply leaves them zero for any tag
             * whose payload is zero (a whole-channel tag like 0x60000000 is
             * 3<<29, and 2^29*odd mod 2^32 keeps only the top bits), which
             * collapsed every payload-less channel to one identical tint. The
             * finalizer mixes the path nibble down into the readout bits so
             * distinct channels stay distinguishable. */
            uint32_t hsh = s_cur_tag * 2654435761u;
            hsh ^= hsh >> 15; hsh *= 2246822519u; hsh ^= hsh >> 13;
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
         * the PSX integer pixel grid — the polygon wobble. TRUNCATE (floor), not
         * round: the GTE derives SXY via MAC0 >> 16, an arithmetic shift = floor,
         * and the choice is load-bearing beyond fidelity — round-half-up measured
         * 2-3x MORE silhouette holes on real scenes (overlapping unstitched edges
         * carry correlated fractional parts that a .5 boundary splits apart).
         * X/Y only; depth and UVs are untouched (Z-buffer + perspective texturing
         * preserved). The grid is display-resolution-independent because sx/sy are
         * pre-normalize (scene_to_clip upscales afterward), so SSAA / widescreen
         * keep the exact 512x240 jitter the original GTE produced. */
        if (s_vertex_snap && !s_cur_presnap && category_snaps_vertices(category)) {
            openpete_scene_vert *sv = s_scene[s_scene_n].v;
            for (int k = 0; k < 3; k++) {
                sv[k].sx = floorf(sv[k].sx);
                sv[k].sy = floorf(sv[k].sy);
            }
        }
        /* OPENPETE_TRUE_W: kill switch / bisect mask for the stated perspective
         * W (openpete_scene_vert.pvz). 0 drops every feeder's value, restoring
         * the historical "derive W from the ordering depth" behaviour exactly;
         * a bitmask keeps it for selected categories only (1 = PLAYER,
         * 2 = ACTOR/HUD3D mobys, 4 = WORLD terrain), which is how a divergence
         * gets attributed to one feeder without a rebuild. Default = all on. */
        {
            /* The parse is masked to 0..7 so the cache stays non-negative and
             * the `< 0` first-use latch actually holds — a negative "unset"
             * value would re-enter getenv on every triangle. 7 keeps all three
             * categories, i.e. the default no-op, so the mask loop is skipped. */
            static int s_tw = -1;
            if (s_tw < 0) { const char* e = getenv("OPENPETE_TRUE_W");
                            s_tw = (e && *e) ? ((int)strtol(e, NULL, 0) & 7) : 7; }
            if (s_tw != 7) {
                int bit = (category == OPENPETE_SCENE_PLAYER) ? 1
                        : (category == OPENPETE_SCENE_ACTOR ||
                           category == OPENPETE_SCENE_ACTOR_FRONT ||
                           category == OPENPETE_SCENE_HUD3D ||
                           category == OPENPETE_SCENE_HUD) ? 2 : 4;
                if (!(s_tw & bit))
                    for (int k = 0; k < 3; k++) s_scene[s_scene_n].v[k].pvz = 0.0f;
            }
        }
        /* Feed-time carry accumulation (see s_overlay_accum): a fresh 2D-UI /
         * overlay tri joins this tick's carry set NOW, so it survives however
         * many sub-tick scene wipes land before the canonical capture. carry==2
         * (a re-applied carry tri routed through a feeder) never re-accumulates. */
        if ((category == OPENPETE_SCENE_OVERLAY || category == OPENPETE_SCENE_HUD_PANEL ||
             carry == 1) && carry != 2) {
            if (s_overlay_accum_n < OVERLAY_CARRY_MAX)
                s_overlay_accum[s_overlay_accum_n++] = s_scene[s_scene_n];
            else
                s_overlay_accum_dropped++;
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
 * flag (set_carry, stamped by add_tri) that the moby near-OT billboards leave 0.
 * (OVERLAY_CARRY_MAX + the feed-time accumulation buffer live up by s_scene.) */
static scene_tri_t s_overlay_carry[OVERLAY_CARRY_MAX];      /* curr (latest canonical capture) */
static int                s_overlay_carry_n;
static uint32_t           s_overlay_carry_freeze;
/* The previous canonical tick's freeze word, so the freeze fade (bits 30:24)
 * interpolates sub-tick like the fade word: the exit-level freeze darkens one
 * CLUT step per tick, and applying the newest word on every sub-tick present
 * put the intermediates a step ahead of the canonical frame. */
static uint32_t           s_overlay_prev_freeze;
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
        s_scene_n = 0; memset(s_scene_fam_n, 0, sizeof s_scene_fam_n);
    }
    s_overlay_prev_freeze  = prev_freeze;
    s_overlay_carry_freeze = new_freeze;
    s_game_freeze_seen = 0;
}

/* Lerp two packed freeze words (bit31 active, [30:24] fade shift, [23:0]
 * tint). Only the fade shift moves; a tint change or an activation edge snaps. */
static uint32_t freeze_lerp(uint32_t a, uint32_t b, double t) {
    if (!(b & (1u << 31))) return 0;
    if (!(a & (1u << 31))) return b;
    if ((a & 0xFFFFFFu) != (b & 0xFFFFFFu)) return b;
    int af = (int)((a >> 24) & 0x7Fu), bf = (int)((b >> 24) & 0x7Fu);
    int f = (int)(af + (bf - af) * t + 0.5);
    return (b & 0x80FFFFFFu) | ((uint32_t)f << 24);
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
    /* Let the 2D-UI feeder flush its deferred ("feed-previous") prim into this
     * tick's accumulation first — by the canonical present every post-link prim
     * mutation is done, so the flush is exact. Without it, a gameplay frame
     * whose LAST 2D prim is an FT4 (the collected-egg sprite is the ONLY 2D
     * prim then) defers that sprite a whole tick and misses this capture. */
    if (s_overlay_precapture) s_overlay_precapture();
    /* Roll curr -> prev so apply_overlay can interpolate animated 2D-UI positions
     * (the logo slide-in) across sub-ticks. Runs on the CANONICAL present only, so
     * prev/curr are consecutive canonical captures (one sim tick apart). */
    if (s_overlay_carry_n > 0)
        memcpy(s_overlay_prev, s_overlay_carry, (size_t)s_overlay_carry_n * sizeof(s_overlay_carry[0]));
    s_overlay_prev_n = s_overlay_carry_n;
    s_overlay_prev_ns = s_overlay_curr_ns;   /* roll the capture timestamp too */
    /* Stamp the capture with the tick's gridded instant, the same clock the
     * 3D per-object channels roll on, not the wall-clock time this code runs
     * at: the two differ by scheduling jitter, and a 2D slide phased by wall
     * time would judder against the 3D scene it sits over. */
    s_overlay_curr_ns = openpete_interp_tick_stamp();
    /* Commit the feed-time accumulation as this tick's carry set. Only the GAME
     * RENDER's fresh emission ever accumulates: OVERLAY/HUD_PANEL by category,
     * 2D-UI sprites/lines by carry==1, and apply_overlay's re-applied (carry==2)
     * copies bypass add_tri entirely — so no dedup pass is needed here, and a
     * sub-tick scene wipe between the feed and this capture can't lose a sprite. */
    memcpy(s_overlay_carry, s_overlay_accum, (size_t)s_overlay_accum_n * sizeof(s_overlay_accum[0]));
    s_overlay_carry_n = s_overlay_accum_n;
    s_overlay_accum_n = 0;
    /* Mirror the scene-cap warn-once: a busy title/menu that overruns the carry
     * buffer silently loses 2D-UI sprites sub-tick with no signal — warn so the
     * cap can be raised rather than leaving a view-dependent dropout in the wild. */
    if (s_overlay_accum_dropped > 0) {
        static int s_ov_warned = 0;
        if (!s_ov_warned) { s_ov_warned = 1;
            fprintf(stderr, "native: WARNING OVERLAY_CARRY_MAX %d exceeded (frame=%u, %d 2D-UI carry "
                    "tris dropped) — sprites will vanish sub-tick; raise OVERLAY_CARRY_MAX\n",
                    OVERLAY_CARRY_MAX, s_scene_frame, s_overlay_accum_dropped);
        }
        s_overlay_accum_dropped = 0;
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
    if (openpete_interp_deterministic()) {
        /* Deterministic capture: op_now() is not reproducible run-to-run, so the
         * wall-clock cadence below would make an animating overlay (a swimming
         * text moby) flicker between captures. Derive the sub-tick phase from the
         * PINNED present clock and the tick grid instead — the overlay captures
         * roll one canonical tick apart, so this yields the same phase the camera
         * bracket uses (0.5 at the forced midpoint) AND still advances when the
         * camera/world is static, because ticks don't stop just because nothing
         * moved. */
        op_time_t curr = openpete_interp_curr_tick_ns();
        op_time_t period = openpete_interp_det_period();
        if (curr > 0 && period > 0) {
            oalpha = (double)(openpete_interp_present_now() - curr) / (double)period;
            if (oalpha < 0.0) oalpha = 0.0; else if (oalpha > 1.0) oalpha = 1.0;
        }
    } else if (s_overlay_curr_ns > s_overlay_prev_ns && s_overlay_prev_ns > 0) {
        /* Interp spans one tick (1-tick lag, no extrapolation): cap the
         * denominator at one tick like the 3D brackets, so a carry resuming
         * after a spine-off stretch sweeps its delta over one tick instead of
         * crawling for the length of the gap. */
        op_time_t period = s_overlay_curr_ns - s_overlay_prev_ns;
        if (period > g_openpete_tick_period_ns) period = g_openpete_tick_period_ns;
        /* The VIRTUAL present clock, never op_now(): the scene may be extracted
         * well before the instant it displays (eager production builds the
         * whole span right after the tick), and the alpha is for the DISPLAY
         * instant. Measured against extraction time every intermediate sat at
         * alpha≈0 and the title logo's drop-in stepped at the tick rate. */
        oalpha = (double)((int64_t)openpete_interp_present_now() - (int64_t)s_overlay_curr_ns) / (double)period;
        if (oalpha < 0.0) oalpha = 0.0; else if (oalpha > 1.0) oalpha = 1.0;
    }
    /* OPENPETE_ALPHA_MAP_DBG: the overlay's present-time -> interp-time mapping
     * beside the 3D bracket's line (spyro_native_reextract.c), so a 2D-UI slide
     * stepping while the 3D scene glides is attributable in one log. */
    {
        static int s_oadbg = -1;
        if (s_oadbg < 0) { const char* e = getenv("OPENPETE_ALPHA_MAP_DBG"); s_oadbg = (e && *e && *e != '0'); }
        if (s_oadbg && s_overlay_carry_n > 0) {
            extern int g_openpete_canon_lag_leg;
            fprintf(stderr, "[overlay-alpha f=%u leg=%s] present_now=%.3fms now=%.3fms curr_ns=%.3fms prev_ns=%.3fms cam_alpha=%.3f oalpha=%.3f n=%d\n",
                    s_scene_frame, g_openpete_canon_lag_leg ? "CANON" : "inter",
                    (double)openpete_interp_present_now() / 1e6, (double)op_now() / 1e6, /* clock-guard: wall */ /* prints both */
                    (double)s_overlay_curr_ns / 1e6, (double)s_overlay_prev_ns / 1e6, alpha, oalpha, s_overlay_carry_n);
        }
    }
    /* Pair curr with prev by index only when the two captures are structurally the
     * same (equal count) — a count change (a blink toggling, a glyph added) makes
     * index pairing meaningless, so snap to curr that present. */
    int pairable = (s_overlay_prev_n == s_overlay_carry_n);
    for (int t = 0; t < s_overlay_carry_n && s_scene_n < MAX_SCENE_TRIS; t++) {
        scene_tri_t tri = s_overlay_carry[t];   /* curr; category already stamped */
        if (pairable) {
            const scene_tri_t *p = &s_overlay_prev[t];
            /* Same sprite? Two identities, either suffices:
             *   (a) same texture region (first vertex's UV) — a rigid or scaling
             *       element with a constant texture (the title-logo slide);
             *   (b) same SHAPE (edge vectors within a rasterisation epsilon) with
             *       a bounded per-tick move — an ANIMATED sprite advances its
             *       UV/CLUT every tick while translating rigidly (the
             *       collected-egg pickup slides down-and-out while its wobble
             *       frames cycle), so UV identity alone left its slide stepping
             *       at the tick rate. The texture frame still snaps to curr
             *       (frames are discrete); only the position lerps. A glyph swap
             *       at the same slot keeps shape+position, where the position
             *       lerp is a no-op — harmless. */
            int same_tex = p->v[0].u == tri.v[0].u && p->v[0].v == tri.v[0].v;
            int same_shape_near = 1;
            for (int k = 1; k < 3 && same_shape_near; k++) {
                float dex = (tri.v[k].sx - tri.v[0].sx) - (p->v[k].sx - p->v[0].sx);
                float dey = (tri.v[k].sy - tri.v[0].sy) - (p->v[k].sy - p->v[0].sy);
                if (dex > 0.5f || dex < -0.5f || dey > 0.5f || dey < -0.5f)
                    same_shape_near = 0;
            }
            {   /* bounded move: same guard value as the billboard interp (64 px/tick
                 * ≈ 4× the fastest observed 2D slide) — a relayout jump snaps. */
                float dx0 = tri.v[0].sx - p->v[0].sx, dy0 = tri.v[0].sy - p->v[0].sy;
                if (dx0 > 64.0f || dx0 < -64.0f || dy0 > 64.0f || dy0 < -64.0f)
                    same_shape_near = 0;
            }
            if (p->category == tri.category && p->textured == tri.textured &&
                (same_tex || same_shape_near)) {
                for (int k = 0; k < 3; k++) {
                    tri.v[k].sx = (float)(p->v[k].sx + (tri.v[k].sx - p->v[k].sx) * oalpha);
                    tri.v[k].sy = (float)(p->v[k].sy + (tri.v[k].sy - p->v[k].sy) * oalpha);
                }
            }
        }
        tri.carry = 2;                             /* mark re-applied so the next capture skips it */
        s_scene[s_scene_n++] = tri;
    }
    s_scene_freeze = freeze_lerp(s_overlay_prev_freeze, s_overlay_carry_freeze, oalpha);
    s_scene_fade   = fade_lerp(s_fade_prev, s_fade_curr, oalpha);
}

/* The scene buffer the present should draw: the depth-frames-back snapshot when
 * a reference-compare delay is set (A/B-synced with the lagged reference), else
 * the live accumulator (vk-only, runs ahead). Falls back to the live frame
 * during warmup before enough history has accumulated. */
static const scene_tri_t* scene_present_buf(int* out_n, uint32_t* out_bg,
                                                   uint32_t* out_fade, uint32_t* out_freeze,
                                                   int* out_hud_under, int* out_actor_front) {
    if (s_ps_slot) {
        *out_n = s_ps_slot->n; *out_bg = s_ps_slot->bg; *out_fade = s_ps_slot->fade;
        if (out_freeze) *out_freeze = s_ps_slot->freeze;
        if (out_hud_under) *out_hud_under = s_ps_slot->hud_under;
        if (out_actor_front) *out_actor_front = s_ps_slot->actor_front;
        return s_ps_slot->tri;
    }
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
            c == OPENPETE_SCENE_ACTOR_FRONT || c == OPENPETE_SCENE_SKY)
            return true;
    }
    return false;
}

/* Single native renderer instance (one window per process). */
/* Resolve the window size. Shared by bring-up and by the main-thread
 * precreate below so the two cannot disagree about how big the window is. */
static void nr_resolve_window_size(const openpete_options_t *opts,
                                   int win_w, int win_h,
                                   int *out_w, int *out_h)
{
    (void)opts;
    *out_w = (win_w > 0) ? win_w : NR_DEFAULT_W;
    *out_h = (win_h > 0) ? win_h : NR_DEFAULT_H;
}

/* A window created by the host before bring-up, adopted the same way the
 * first-run splash's window is. See openpete_native_precreate_window. */
static SDL_Window* s_precreated_win;

void openpete_native_precreate_window(const openpete_options_t *opts,
                                      int win_w, int win_h)
{
    if (s_precreated_win || opts->adopted_window) return;
    /* Every windowed mode now brings the native window up and displays through
     * it, so every windowed mode wants the single-window handover. Headless has
     * no window at all. */
    if (opts->headless) return;

    int w, h;
    nr_resolve_window_size(opts, win_w, win_h, &w, &h);

    const char *title = (opts->display_name && opts->display_name[0])
                        ? opts->display_name : "openpete";
    s_precreated_win = SDL_CreateWindow(title, w, h,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!s_precreated_win) {
        /* Not fatal: bring-up falls back to creating its own window. */
        OPL_WARN_F("native", "precreate SDL_CreateWindow failed: %s", SDL_GetError());
        return;
    }
    OPL_INFO_F("native", "window precreated on the host thread (%dx%d)", w, h);
}

/* Window icon pixels, handed over before bring-up (the decoded image lives in
 * the game module). Stored rather than applied immediately because the window
 * does not exist yet at the point the caller has them. */
static const unsigned char* s_icon_rgba;
static int s_icon_w, s_icon_h;

void openpete_native_set_icon(const void* rgba32, int w, int h)
{
    s_icon_rgba = (const unsigned char*)rgba32;
    s_icon_w = w;
    s_icon_h = h;
}

/* How long after an app-driven size change (a fullscreen toggle) the next size
 * change is taken as discrete. Long enough for the compositor to deliver the
 * new size, short enough that a drag beginning right afterwards still settles
 * normally. */
#define NR_RESIZE_IMMEDIATE_GRACE_NS (1000ull * 1000ull * 1000ull)

static struct {
    bool          enabled;
    /* Native scene extraction and interpolation are running: the feature
     * modules feed the scene ring, the sub-tick spine re-extracts, and compose
     * draws that scene. False in PsyCross display mode, where the native window
     * is up but the picture it shows is PsyCross's read-back frame and no
     * native scene exists to walk. */
    bool          scene_live;
    bool          headless;     /* W1.1: surfaceless capture (no window/present); screenshots only */
    SDL_Window   *window;
    rhi_device   *dev;
    int           win_w, win_h; /* last-known pixel size, for resize detection */
    /* A size seen but not yet adopted, and when it was first seen — a resize
     * is only taken once it has held still (see NR_RESIZE_SETTLE_NS). */
    int           pending_w, pending_h;
    Uint64        pending_since_ns;
    /* When the app itself last caused a size change (a fullscreen toggle).
     * Such a change arrives in a single step and so has nothing to settle;
     * waiting out the drag window there costs a third of a second of frozen
     * display for no benefit. Time-bounded rather than a plain flag so a
     * toggle that changes no size at all cannot leave the next genuine drag
     * unsettled. */
    Uint64        resize_immediate_at_ns;

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

    /* Native 3D render: gouraud triangles (quad.vert/quad.frag) for
     * the cyclorama/sky scene the game pushes in. Replaces the VRAM re-display
     * in panel 0 when a scene is present. */
    rhi_pipeline *gouraud_pipe;
    rhi_pipeline *gouraud_blend_pipe; /* 3b.2: semi-transparent UNTEXTURED gouraud (alpha, ABR mode 0) */
    rhi_pipeline *gouraud_sub_pipe;   /* 3b.2: subtractive UNTEXTURED gouraud (ABR mode 2, B-F) — the Spyro shadow as a DEPTH-AWARE decal (shadow_decal.frag samples opaque_depth at set 0; depth-test OFF, the shader owns occlusion) */
    rhi_pipeline *gouraud_sub_ztest_pipe; /* fallback subtractive pipe (plain depth test + W2.5 hw bias) when the depth pre-pass is unavailable */
    rhi_pipeline *gouraud_sub_fade_pipe;  /* subtractive, depth-test OFF, quad.frag — the full-screen mode-2 fade (NOT the decal pipe, which would sample/discard) */
    rhi_pipeline *gouraud_add_pipe;   /* 3b.3: additive UNTEXTURED gouraud (ABR mode 1, B+F) — glow rings */
    rhi_pipeline *gouraud_noz_pipe;   /* opaque UNTEXTURED gouraud, depth-test ON / write OFF ("noz" = no z WRITE) + stencil TEST — portal-sky colours inside the aperture mask (W2.6) */
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
    /* Dragon-rescue front pass: colour-write-off twins of the three ACTOR_FRONT
     * opaque pipes that REPLACE the stored depth (compare ALWAYS) — the
     * cutscene actors evict the terrain from their own pixels and nowhere
     * else. Same shaders as the twins they mirror, so the punched coverage is
     * texel-exact (identical key/cutout discards). Any NULL → the front pass
     * degrades to a plain in-place actor draw (normal terrain occlusion). */
    rhi_pipeline *gouraud_punch_pipe;
    rhi_pipeline *scene_tex_punch_pipe;
    rhi_pipeline *hd_cutout_punch_pipe;
    rhi_pipeline *hd_scene_blend_pipe;/* HD replacement, alpha scene group (ABR 0): ztest, no zwrite */
    rhi_pipeline *hd_scene_add_pipe;  /* HD replacement, additive scene group (ABR 1): ztest, no zwrite */
    rhi_texture  *mat_pack_dummy;     /* 1x1 opaque white bound at set 1 for material
                                       * draws with no pack replacement (op_pack_tex
                                       * needs a valid RGBA descriptor either way) */
    rhi_buffer   *scene_vb;     /* MAX_SCENE_TRIS*3 verts, rebuilt per frame */
    float         p0_cx, p0_hx, p0_hy; /* panel-0 fitted rect (clip space, cy=0) — the 4:3 CONTENT sub-rect */
    /* Target display aspect (W/H), from --aspect. 0 = AUTO (the default):
     * the render aspect follows the window/fullscreen shape live, filling it
     * with no bars (update_layout re-derives on every size change). >0 =
     * FIXED: the scene renders at this aspect and is letterboxed/pillarboxed
     * into the window (like PsyCross), NOT stretched to fill it. Set at init
     * from opts->aspect_w_over_h; live via openpete_native_set_aspect. */
    float         aspect_wh;
    /* Widescreen (Hor+) widen factor = render_aspect / 4:3, i.e. how much MORE
     * horizontal world the render aspect reveals vs 4:3 (1.0 = plain 4:3) —
     * window-derived under AUTO, --aspect-derived when fixed. The panel-0
     * content mapping (scene_to_clip via p0_hx) is the centred 4:3 sub-rect;
     * the reveal extends to ±p0_hx*ws_factor (the render rect), which fit_cell
     * fits into the window with bars (none under AUTO — the rect IS the
     * window shape). Applies to the native scene only; the psycross-mode
     * reference blit stays 4:3. See update_layout. */
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

    /* native_rtt: the canonical native frame each present, a sampleable render
     * target at the reference resolution. */
    rhi_texture  *native_rtt;
    int           rtt_w, rtt_h;

    /* Dedicated WIDESCREEN screenshot target: the native-only capture path
     * renders the Hor+ view into this (ref_w*cap_wf × ref_h) instead of the
     * ref-sized native_rtt, so the committed golden shows the widened content
     * at true 16:9 with no horizontal squish. Kept separate from native_rtt
     * (which the live/diff path keys on ref_w) so repeated captures don't churn
     * the shared target's allocation. Only allocated when a widescreen capture
     * runs; 4:3 captures use native_rtt as before. */
    rhi_texture  *shot_rtt;
    int           shot_w, shot_h;

    /* Capture-time freeze backdrop: the live path snapshots the last world frame
     * into frozen_tex only when there's a window (win_w>0), so a headless
     * screenshot of a freeze frame (pause / flight-results / level-exit) has no
     * backdrop and renders dark. During capture the driver instead renders each
     * pre-freeze WORLD frame into shot_frozen (at the capture's widescreen extent),
     * and the freeze overlay prefers it — so the green desaturated frozen-world
     * effect is captured headless. Holds the most recent world frame; once the
     * freeze bit is set the snapshot stops updating, freezing that frame. */
    rhi_texture  *shot_frozen;
    int           shot_frozen_w, shot_frozen_h;
    bool          shot_frozen_valid;

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
     * draw_perf_overlay. */
    rhi_texture  *ovl_ss;
    int           ovl_ss_w, ovl_ss_h;
    rhi_pipeline *ovl_resolve_pipe;   /* overlay_resolve.frag, box + straight alpha, RHI_BLEND_ALPHA */
    rhi_buffer   *ovl_quad_vb;        /* composite quad at the overlay's screen rect */
    int           ovl_quad_win_w, ovl_quad_win_h;
    float         ovl_quad_ox, ovl_quad_oy; /* overlay-space top-left the quad was built at (the anchor ledger's placement for the perf block) */
    float         ovl_quad_rw, ovl_quad_rh; /* region the quad was built at (follows the pushed extent) */

    /* Settings-menu glyph/border supersample: same idea as ovl_ss but sized to the
     * menu PANEL rect (much larger). The glyph+border layer renders into menu_ss at
     * MENU_SS_FACTOR×, then reuses ovl_resolve_pipe to box-downsample + alpha-blend
     * it onto the offscreen via menu_quad — so the 3D menu text is anti-aliased. */
    rhi_texture  *menu_ss;
    int           menu_ss_w, menu_ss_h;
    rhi_buffer   *menu_quad_vb;       /* composite quad at the panel rect */
    int           menu_quad_win_w, menu_quad_win_h;

    /* OSD toast supersample: a FIXED grid of OPENPETE_OSD_MAX slots, one per
     * possible live toast, rebuilt only on resize. Each present the compositor
     * draws one quad per toast sampling that toast's slot sub-rect, tinted by
     * its fade alpha (the resolve shader modulates by the quad's vertex
     * colour). Quads ride the per-present menu_vb ring: a sliding toast moves
     * its quad EVERY present, so a single buffer rewritten under wait_idle —
     * fine for chrome that only moves on a resize — would idle the device on
     * every frame of every animation. */
    rhi_texture  *osd_ss;
    int           osd_ss_w, osd_ss_h;
    /* One slot's 1x pixel extent, packed into a single word: written here on
     * the present thread when the grid is (re)built, read on the sim thread by
     * the glyph builder that lays out against it. Two ints would tear on a
     * resize — one tick of boxes laid out to a width and a height that were
     * never a pair — which is the same class as the perf region rect above,
     * and cheaper to close than to reason about. */
    uint64_t      osd_slot_px;   /* w | h << 32 */
    rhi_buffer   *osd_glyph_vb;       /* toast glyphs + frames (NDC, depth-tested) */
    rhi_buffer   *osd_quad_vb;        /* one composite quad per visible toast */
    rhi_buffer   *osd_panel_vb;       /* the toasts' subtractive panels */

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
    rhi_pipeline *terrain_key_pipe;   /* P6b HP key pass (terrain_key.vert) */
    rhi_pipeline *terrain_key_tex_pipe; /* textured keyed faces: alpha-aware
                                         * cutout (psx_key_cutout.frag) — keys
                                         * exist only on opaque texels */
    rhi_buffer   *terrain_key_vb;     /* HP-opaque clip geometry, per-present ring */
    rhi_texture  *terrain_key_depth;  /* terrain og-key image (set 2) */
    rhi_texture  *moby_key_depth;     /* prop TRUE-depth image (set 3) — split so
                                       * the near-band embed rule sees curated
                                       * prop faces only */
    rhi_texture  *actor_key_depth;    /* moby og-key image (set 1): opaque ACTOR
                                       * keys, split from set 2 so fragments can
                                       * scope a moby's paint-over by their own
                                       * TRUE depth while terrain-vs-terrain
                                       * keeps contesting authored keys */
    rhi_texture  *moby_rank_rt;       /* moby og-RANK image (set 4): RGBA8
                                       * (rank lo, rank hi, moby ordinal) of the
                                       * LAST-painted ranked face per pixel —
                                       * the same-moby painter select input */
    rhi_pipeline *moby_rank_pipe;     /* untextured ranked faces (moby_rank.vert/.frag) */
    rhi_pipeline *moby_rank_cutout_pipe; /* textured: alpha-aware cutout — rank
                                          * exists only on opaque texels */
    int           tk_w, tk_h;         /* current key-image allocations (shared dims) */
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

/* Fill a panel quad: clip-space rect centered at (cx,cy) with half-extents
 * (hx,hy); v0 is the UV-v at the TOP edge, v1 at the BOTTOM. (VRAM is uploaded
 * top-down so it uses v0<v1; the GL readback is bottom-up so the reference
 * uses v0>v1.) rgba is unused by the textured shaders. */
static void build_panel_tinted(rhi_vertex out[6], float cx, float cy, float hx, float hy,
                               float u0, float v0, float u1, float v1, uint32_t rgba)
{
    const rhi_vertex tl = { cx - hx, cy + hy, 0.0f, u0, v0, rgba, 0 };
    const rhi_vertex tr = { cx + hx, cy + hy, 0.0f, u1, v0, rgba, 0 };
    const rhi_vertex br = { cx + hx, cy - hy, 0.0f, u1, v1, rgba, 0 };
    const rhi_vertex bl = { cx - hx, cy - hy, 0.0f, u0, v1, rgba, 0 };
    out[0] = tl; out[1] = tr; out[2] = br;
    out[3] = tl; out[4] = br; out[5] = bl;
}
static void build_panel(rhi_vertex out[6], float cx, float cy, float hx, float hy,
                        float u0, float v0, float u1, float v1)
{
    build_panel_tinted(out, cx, cy, hx, hy, u0, v0, u1, v1, 0xFFFFFFFFu);
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
    if (!opts)
        return 0;

    /* PsyCross mode under --headless: PsyCross rasterizes for the oracle and
     * writes the -og capture off its own FBO. There is no native scene to
     * compose and nothing for a surfaceless device to draw, so native stays
     * off — the behaviour this mode had before the native window became its
     * display. Checked ahead of the vk headless guard below, which admits
     * surfaceless bring-up when screenshots or the identity audit are armed;
     * neither has a native consumer here. */
    if (opts->headless && opts->renderer == OPENPETE_RENDERER_PSYCROSS)
        return 0;

    /* W1.1: under --headless, bring the native renderer up SURFACELESS purely to
     * service screenshots (CI visual-regression capture under lavapipe) — but
     * only when a native renderer is selected (checked above) AND screenshots
     * are requested. Plain --headless (the oracle / determinism gates) keeps
     * native OFF, so those gates are unaffected. The live present loop never runs
     * headless; only openpete_native_render_screenshot_all is driven (run.c). */
    const bool headless = opts->headless;
    /* The identity audit is a headless native consumer too: it composes the
     * scene twice per canonical present and never writes a PNG, so it arms the
     * surfaceless bring-up on its own. */
    if (headless && opts->screenshot_frame_count <= 0 &&
                    opts->screenshot_intermediate_count <= 0 &&
                    !openpete_ident_audit_armed()) {
        OPL_WARN_F("native", "--renderer vk ignored under --headless (no screenshots requested)");
        return 0;
    }
    s_nr.headless = headless;

    /* --texpack-dir: point the HD texture-replacement scan at a chosen dir. Set
     * before openpete_vram_install_ops() below runs texture_pack_init(). NULL is
     * a no-op (falls back to OPENPETE_TEXPACK_DIR env, then the built-in dir). */
    texture_pack_set_dir_override(opts->texpack_dir);

    /* --texture-mapping: PSX-affine UV/colour interpolation (vertex spec-
     * constant 1, w=1 in the scene vertex stages; depth values identical in
     * both modes). Resolved here, before the dev-overlay init snapshots knob
     * boot values and before the pipelines below bake the constant. Live
     * changes queue via openpete_native_set_texture_mapping and re-bake all
     * pipelines at the next present. */
    s_texmap_affine = s_texmap_pending =
        (opts->texture_mapping == OPENPETE_TEXTURE_MAPPING_AFFINE);
    rhi_set_affine_uv(s_texmap_affine);
    if (s_texmap_affine)
        OPL_INFO_F("native", "%s", "texture-mapping: affine (PSX screen-linear)");

    /* OPENPETE_NOTEX: untextured lighting view (fragment spec-constant 2) —
     * textured scene fragments output the raw gouraud vertex colour. Debug
     * view; live changes queue via openpete_native_set_notex. */
    {
        const char* e = getenv("OPENPETE_NOTEX");
        s_notex = s_notex_pending = (e && *e && *e != '0');
        rhi_set_notex(s_notex);
        if (s_notex)
            OPL_INFO_F("native", "%s", "notex: untextured lighting view ON");
    }

    /* Render aspect from --aspect (W/H); 0 → AUTO (follow the window shape,
     * filling it live on resize). >0 fixes the Hor+ amount + letterbox/
     * pillarbox. */
    s_nr.aspect_wh = (opts->aspect_w_over_h > 0.0f) ? opts->aspect_w_over_h : 0.0f;

    /* SDL_INIT_VIDEO is already up (PsyX_Initialise). Create a SECOND window
     * for the Vulkan surface: a single SDL window can't carry both PsyCross's
     * GL context and our Vulkan surface, so the native renderer owns its own.
     * PsyCross's GL window is started hidden (see run.c), so this Vulkan
     * window is the single visible one. Match the game's resolved window size
     * so it isn't surprisingly larger than the PsyCross-only window. */
    int w, h;
    nr_resolve_window_size(opts, win_w, win_h, &w, &h);

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
        const char *title = (opts->display_name && opts->display_name[0])
                            ? opts->display_name : "openpete";
        /* Either the first-run splash's window or one the host precreated.
         * Both exist so the window is owned by the thread that will pump it:
         * Win32 delivers a window's messages only to its creating thread. */
        SDL_Window *adopt = (SDL_Window*)opts->adopted_window;
        const char *adopt_src = "splash";
        if (!adopt) { adopt = s_precreated_win; adopt_src = "precreated"; }
        if (adopt) {
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
            OPL_INFO_F("native", "adopted %s window (%dx%d)", adopt_src, w, h);
        } else {
            win = SDL_CreateWindow(title, w, h,
                                   SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
            if (!win) {
                OPL_ERROR_F("native", "SDL_CreateWindow(VULKAN) failed: %s", SDL_GetError());
                return 1;
            }
        }

        /* The window icon has to be set here, not on PsyCross's window: in the
         * default (null-GPU) path PsyCross never creates one, so the visible
         * window would ship icon-less on any platform without a resource-file
         * icon — i.e. everywhere but Windows. */
        if (s_icon_rgba && s_icon_w > 0 && s_icon_h > 0) {
            SDL_Surface* ico = SDL_CreateSurfaceFrom(
                s_icon_w, s_icon_h, SDL_PIXELFORMAT_RGBA32,
                (void*)s_icon_rgba, s_icon_w * 4);
            if (ico) {
                SDL_SetWindowIcon(win, ico);
                SDL_DestroySurface(ico);
                OPL_INFO_F("native", "window icon applied (%dx%d)",
                           s_icon_w, s_icon_h);
            }
        } else {
            /* Weakly linked: a build without the icon generator step resolves
             * the symbol to NULL and silently ships a default icon. Say so. */
            OPL_WARN_F("native", "%s", "no window icon available (icon data not linked)");
        }

        /* --fullscreen 1: launch in borderless-desktop fullscreen — the exact
         * state openpete_native_toggle_fullscreen switches into, so Alt+Enter /
         * the O-menu DISPLAY row toggle straight back to the windowed size
         * above. Before rhi_create so the swapchain sizes to the monitor. */
        if (opts->fullscreen) {
            SDL_SetWindowFullscreenMode(win, NULL);  /* borderless desktop */
            SDL_SetWindowFullscreen(win, true);
            OPL_INFO_F("native", "%s", "--fullscreen: borderless desktop at launch");
        }

        dev = rhi_create(win, "openpete-native", opts->vsync);
        if (!dev) {
            OPL_ERROR_F("native", "rhi_create failed; continuing without native A/B");
            SDL_DestroyWindow(win);
            return 1;
        }
        /* M-key dev config overlay: imgui context + SDL3/Vulkan backends bound
         * to this window/device. Failure just leaves the overlay unavailable
         * (logged inside); never fatal to the renderer. */
        openpete_devui_init(dev, win, opts);
    }
    /* On an unrecoverable device-loss (acquire/present), flag a clean shutdown
     * rather than spin failed frames forever (W0.6 RHI robustness). */
    rhi_set_device_lost_handler(openpete_request_shutdown);
    /* Live-window supersampling (--ssaa N, default 1=no-op): the ss_rtt + resolve
     * pass are set up lazily (ensure_ss_rtt) once the window size is known. */
    s_nr.ssaa = (opts->ssaa > 0) ? opts->ssaa : 1;
    /* Vertex-precision snap (--vertex-precision integer): re-introduce the PSX
     * integer-grid vertex snap in add_tri_grouped. Default smooth (subpixel). */
    s_vertex_snap = (opts->vertex_precision == OPENPETE_VERTEX_PRECISION_INTEGER) ? 1 : 0;
    /* Raster output quantize (--raster-quantize): -1 = auto-pair with the snap
     * (1x = the authentic 512x240 raster iff integer mode), so the wobble ships
     * with its seam treatment by default and each remains independently
     * overridable. */
    s_raster_mult = (opts->raster_quantize >= 0) ? opts->raster_quantize
                 : (s_vertex_snap ? 1 : 0);
    s_dither = opts->dither ? 1 : 0;   /* PS1 15-bit colour + hardware dither */

    /* Scene depth encoding (OPENPETE_DEPTH_AFFINE): drives the scene shaders'
     * vertex spec-constant 0. Must be set BEFORE the scene pipelines are built
     * below. 1 = screen-affine z_ndc=1-1/vz (exact depth interpolation, no near-
     * coplanar ordering flips) — the DEFAULT; 0 = legacy z=vz/(vz+4096). Screen-
     * affine is the correct depth curve (the shadow/occlusion pre-pass already
     * uses it) and it fixes real z-fighting on coplanar terrain/portal/cyclorama
     * surfaces; the env forces the legacy encoding as an A/B escape hatch. */
    { const char* e = getenv("OPENPETE_DEPTH_AFFINE");
      int affine = (e && *e) ? (*e != '0') : 1;   /* default ON; =0 reverts to legacy */
      rhi_set_depth_affine(affine);
      OPL_INFO_F("native", "depth: %s",
                 affine ? "screen-affine z_ndc=1-1/vz (default)"
                        : "legacy z=vz/(vz+4096) (OPENPETE_DEPTH_AFFINE=0)"); }
    /* (--texture-mapping was resolved near the top of this function — before
     * the dev overlay snapshotted knob boot values — so every pipeline below
     * bakes the right AFFINE_UV constant.) */

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
        .frag_spv = "quad_key.frag.spv",   /* + terrain LP key-resolve (set 2) */
        .blend = RHI_BLEND_NONE, .depth_test = true,  /* 3a.3: sort actors near→far */
        .uses_vram = false, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = 0,
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
        .frag_spv = "quad_key.frag.spv",   /* + terrain LP key-resolve (set 2) */
        .blend = RHI_BLEND_NONE, .depth_test = true, .stencil = RHI_STENCIL_MARK,
        .uses_vram = false, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = 0,
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
        .frag_spv = "quad_key.frag.spv",   /* + og-key contest (transparent LP terrain contests, never emits) */
        .blend = RHI_BLEND_ALPHA, .depth_test = true, .depth_write_off = true,
        .uses_vram = false, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = 0,
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
    /* Terrain HP KEY pass (P6b): rasterise opaque HP terrain with depth = the
     * og OT key (true view-Z + authored slot offset) into a private depth
     * image; the scene's LP fragments key-contest it (quad_key.frag /
     * psx_scene_key.frag). Rendered per scene TARGET at that target's size so
     * texelFetch(gl_FragCoord.xy) aligns exactly. */
    rhi_pipeline_desc terrain_key_desc = {
        .vert_spv = "terrain_key.vert.spv",
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
        .frag_spv = "quad_key.frag.spv",   /* + og-key contest (transparent LP terrain contests, never emits) */
        .blend = RHI_BLEND_ADD, .depth_test = true, .depth_write_off = true,
        .uses_vram = false, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = 0,
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
        .frag_spv = "psx_scene_key.frag.spv",   /* + terrain LP key-resolve (set 2) */
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .uses_vram = true, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = sizeof(psx_material),
    };
    /* Textured WORLD-group stencil-MARK variant (W2.6-c) — most terrain is
     * textured, so the coverage mark must come from this pipe too. */
    rhi_pipeline_desc scene_tex_mark_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "psx_scene_key.frag.spv",   /* + terrain LP key-resolve (set 2) */
        .blend = RHI_BLEND_NONE, .depth_test = true, .stencil = RHI_STENCIL_MARK,
        .uses_vram = true, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = sizeof(psx_material),
    };
    /* Semi-transparent (ABE) textured pipeline: alpha blend, reproducing PSX ABR
     * mode 0 ((B+F)/2) with per-texel STP gating via psx_scene.frag's STP alpha.
     * depth_write_off: transparent tris Z-TEST against opaque but do NOT write
     * depth, so they never occlude each other (correct ordering still needs the
     * back-to-front sort, 3c; this is the prerequisite half). */
    rhi_pipeline_desc scene_tex_blend_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene_key.frag.spv",   /* + og-key contest (transparent terrain contests, never emits) */
        .blend = RHI_BLEND_ALPHA, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = sizeof(psx_material),
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
        .frag_spv = "psx_scene_key.frag.spv",   /* + og-key contest (transparent terrain contests, never emits) */
        .blend = RHI_BLEND_ADD, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = sizeof(psx_material),
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
    /* HD texture-replacement pipe:
     * samples a host RGBA8 replacement (hd_tex.frag) with real per-texel alpha
     * instead of the PSX VRAM decode. Depth OFF (the PoC target is a HUD-layer
     * 2D quad); push constants carry the replaced texel box. */
    rhi_pipeline_desc hd_tex_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "hd_tex.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(hd_pc),
    };
    /* Opaque-scene HD replacement: depth-tested cutout (true blending on
     * opaque groups would break Z ordering); the MARK twin keeps the W2.6-c
     * hole-fill stencil coverage intact when replacing WORLD-group runs. */
    rhi_pipeline_desc hd_cutout_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "hd_tex_cutout.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .uses_vram = true, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = sizeof(hd_pc),
    };
    rhi_pipeline_desc hd_cutout_mark_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "hd_tex_cutout.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .stencil = RHI_STENCIL_MARK,
        .uses_vram = true, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = sizeof(hd_pc),
    };
    /* Dragon-rescue front pass, step 1 of 2 — the DEPTH PUNCH twins of the
     * three ACTOR_FRONT opaque pipes: same shaders (so the same fragments
     * survive the key/cutout discards), colour writes off, depth compare
     * ALWAYS. Rasterizing the actors through these replaces the stored depth
     * on exactly the pixels they cover; the normal draw that follows then wins
     * over terrain it interpenetrates, while every pixel the actors do NOT
     * cover keeps its true depth for the transparent groups drawn later. */
    rhi_pipeline_desc gouraud_punch_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad_key.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_overwrite = true,
        .color_write_off = true,
        .uses_vram = false, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = 0,
    };
    rhi_pipeline_desc scene_tex_punch_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "psx_scene_key.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_overwrite = true,
        .color_write_off = true,
        .uses_vram = true, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = sizeof(psx_material),
    };
    rhi_pipeline_desc hd_cutout_punch_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "hd_tex_cutout.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_overwrite = true,
        .color_write_off = true,
        .uses_vram = true, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = sizeof(hd_pc),
    };
    /* HD twins of the transparent scene pipes: alpha (ABR 0) and
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
        .uses_vram = true, .push_const_size = sizeof(hd_pc),
    };
    rhi_pipeline_desc hd_scene_add_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "hd_tex.frag.spv",
        .blend = RHI_BLEND_ADD, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .push_const_size = sizeof(hd_pc),
    };
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
     * glyphs over the scene. Optional — draw_perf_overlay falls back to a direct
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
    rhi_pipeline *gouraud_punch_pipe   = rhi_create_pipeline(dev, &gouraud_punch_desc);
    rhi_pipeline *scene_tex_punch_pipe = rhi_create_pipeline(dev, &scene_tex_punch_desc);
    rhi_pipeline *hd_cutout_punch_pipe = rhi_create_pipeline(dev, &hd_cutout_punch_desc);
    rhi_pipeline *hd_scene_blend_pipe = rhi_create_pipeline(dev, &hd_scene_blend_desc);
    rhi_pipeline *hd_scene_add_pipe   = rhi_create_pipeline(dev, &hd_scene_add_desc);
    rhi_pipeline *freeze_overlay_pipe = rhi_create_pipeline(dev, &freeze_overlay_desc);
    rhi_pipeline *resolve_pipe = rhi_create_pipeline(dev, &resolve_desc);
    /* Per-present-rewritten buffers (scene/prepass/overlay) are frame-in-flight
     * RINGS (W2.2): their every-present rhi_update_buffer used to race the
     * frame two presents ago, previously masked only by the per-present VRAM
     * upload's vkQueueWaitIdle drain. panels/ovl_quad/fs_quad update rarely
     * (init / re-layout) under rhi_wait_idle and stay single. */
    rhi_buffer   *panels_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * NR_PANELS * 6);
    /* One scene fill per frame, so a single generation. */
    rhi_buffer   *scene_vb  = rhi_create_vertex_buffer_dynamic_gens(dev, sizeof(rhi_vertex) * SCENE_VB_VERTS, 1u);
    rhi_buffer   *fs_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    rhi_buffer   *overlay_vb = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * OVL_MAX_VERTS);
    /* Mod draw-service overlay (optional — NULL just disables mod draws). */
    rhi_buffer   *mod_ovl_vb = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * MOD_OVL_MAX_VERTS);
    /* Optional overlay-supersample resources (graceful fallback if NULL). */
    rhi_pipeline *ovl_resolve_pipe = rhi_create_pipeline(dev, &ovl_resolve_desc);
    rhi_buffer   *ovl_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    rhi_buffer   *menu_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    /* OSD toast layer. Both are per-present RINGS: a sliding toast rewrites
     * its quad every present, and the glyph list is rebuilt every sim tick
     * (the shimmer is baked per vertex). A single buffer would need a device
     * idle per frame of every animation. */
    rhi_buffer   *osd_glyph_vb = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * OSD_GLYPH_MAX);
    rhi_buffer   *osd_quad_vb  = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * OPENPETE_OSD_MAX * 6);
    /* The toasts' panels get their OWN buffer rather than borrowing menu_vb.
     * Both would be updated and drawn inside one command buffer on any frame
     * where a toast is up while the menu is open — which is the common case,
     * since changing a setting is what raises a toast — and the second update
     * would overwrite the bytes the first draw reads at execution time. */
    rhi_buffer   *osd_panel_vb = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * OPENPETE_OSD_MAX * 6);
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
    rhi_buffer   *prepass_vb = rhi_create_vertex_buffer_dynamic_gens(dev, sizeof(rhi_vertex) * SCENE_VB_VERTS, 1u);
    /* Optional P6b key-pass resources (graceful fallback: NULL → the feeder
     * emits koff=0, LP fragments never sample, terrain contests at raw true
     * depth — the OT_BIAS=0 look). The key IMAGE allocates lazily per target
     * size (ensure_terrain_key_depth). */
    rhi_pipeline *terrain_key_pipe = rhi_create_pipeline(dev, &terrain_key_desc);
    rhi_buffer   *terrain_key_vb = rhi_create_vertex_buffer_dynamic_gens(dev, sizeof(rhi_vertex) * SCENE_VB_VERTS, 1u);
    if (!terrain_key_pipe || !terrain_key_vb) {
        if (terrain_key_pipe) { rhi_destroy_pipeline(dev, terrain_key_pipe); terrain_key_pipe = NULL; }
        if (terrain_key_vb)   { rhi_destroy_buffer(dev, terrain_key_vb);     terrain_key_vb = NULL; }
        OPL_ERROR_F("native", "terrain key pass unavailable; LP key-resolve disabled");
    }
    /* Alpha-aware key stage for TEXTURED keyed faces: same key depth as
     * terrain_key_desc, but the fragment samples the face's PSX texels and
     * drops the p==0 transparent ones, so a face's og key occupies exactly
     * the pixels its opaque texels paint. NULL degrades to the flat pipe
     * (full-footprint keys, the pre-cutout behaviour). */
    rhi_pipeline_desc terrain_key_tex_desc = {
        .vert_spv = "terrain_key.vert.spv",
        .frag_spv = "psx_key_cutout.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    rhi_pipeline *terrain_key_tex_pipe = terrain_key_pipe
        ? rhi_create_pipeline(dev, &terrain_key_tex_desc) : NULL;
    if (terrain_key_pipe && !terrain_key_tex_pipe)
        OPL_ERROR_F("native", "textured key cutout pipe unavailable; "
                              "textured faces key full-footprint");
    /* Moby RANK pass (same-moby painter select): rasterise ranked moby faces
     * with depth = their og OT walk rank into an RGBA8 (rank, ordinal) image;
     * ranked scene fragments discard where a LATER-ranked face of the SAME
     * moby covers the pixel. Optional (NULL → rank image stays cleared, every
     * ranked face decodes "no cover" and resolves at true depth). */
    rhi_pipeline_desc moby_rank_desc = {
        .vert_spv = "moby_rank.vert.spv",
        .frag_spv = "moby_rank.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .uses_vram = false, .push_const_size = 0,
    };
    rhi_pipeline_desc moby_rank_cutout_desc = {
        .vert_spv = "moby_rank.vert.spv",
        .frag_spv = "moby_rank_cutout.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    rhi_pipeline *moby_rank_pipe = terrain_key_pipe
        ? rhi_create_pipeline(dev, &moby_rank_desc) : NULL;
    rhi_pipeline *moby_rank_cutout_pipe = moby_rank_pipe
        ? rhi_create_pipeline(dev, &moby_rank_cutout_desc) : NULL;
    if (terrain_key_pipe && !moby_rank_pipe)
        OPL_ERROR_F("native", "moby rank pass unavailable; same-moby painter select disabled");
    rhi_texture  *vram_tex  = rhi_create_vram(dev, NR_VRAM_W, NR_VRAM_H);
    /* W2.4: the CPU mirror IS the owned native VRAM model (static storage in
     * native_vram.c) — written by the native ops at op time and shadow-synced
     * from PsyCross's mirror per dirty rect at present time. */
    uint16_t     *mirror    = openpete_vram_data();
    if (!blit_pipe || !vram_pipe || !gouraud_pipe || !gouraud_blend_pipe || !gouraud_sub_pipe || !gouraud_sub_ztest_pipe || !gouraud_sub_fade_pipe || !prepass_pipe || !gouraud_add_pipe || !scene_tex_pipe || !scene_tex_blend_pipe || !scene_tex_add_pipe || !scene_tex_sub_pipe || !gouraud_hud_pipe || !gouraud_hud_blend_pipe || !gouraud_hud_sub_pipe || !scene_tex_hud_pipe || !panels_vb || !scene_vb || !vram_tex || !mirror ||
        !freeze_overlay_pipe || !resolve_pipe || !fs_quad_vb || !overlay_vb || !prepass_vb) {
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
        if (mod_ovl_vb)    rhi_destroy_buffer(dev, mod_ovl_vb);
        if (ovl_quad_vb)   rhi_destroy_buffer(dev, ovl_quad_vb);
        if (menu_quad_vb)  rhi_destroy_buffer(dev, menu_quad_vb);
        if (fs_quad_vb)    rhi_destroy_buffer(dev, fs_quad_vb);
        if (vram_tex)      rhi_destroy_texture(dev, vram_tex);
        if (scene_vb)      rhi_destroy_buffer(dev, scene_vb);
        if (panels_vb)     rhi_destroy_buffer(dev, panels_vb);
        if (scene_tex_hud_pipe) rhi_destroy_pipeline(dev, scene_tex_hud_pipe);
        if (hd_tex_pipe) rhi_destroy_pipeline(dev, hd_tex_pipe);
        if (hd_cutout_pipe) rhi_destroy_pipeline(dev, hd_cutout_pipe);
        if (hd_cutout_mark_pipe) rhi_destroy_pipeline(dev, hd_cutout_mark_pipe);
        if (gouraud_punch_pipe) rhi_destroy_pipeline(dev, gouraud_punch_pipe);
        if (scene_tex_punch_pipe) rhi_destroy_pipeline(dev, scene_tex_punch_pipe);
        if (hd_cutout_punch_pipe) rhi_destroy_pipeline(dev, hd_cutout_punch_pipe);
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
    s_nr.scene_live  = (opts->renderer == OPENPETE_RENDERER_VK);
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
    s_nr.terrain_key_pipe = terrain_key_pipe;
    s_nr.terrain_key_tex_pipe = terrain_key_tex_pipe;
    s_nr.terrain_key_vb   = terrain_key_vb;
    s_nr.moby_rank_pipe = moby_rank_pipe;
    s_nr.moby_rank_cutout_pipe = moby_rank_cutout_pipe;
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
    s_nr.gouraud_punch_pipe = gouraud_punch_pipe;       /* may be NULL → front pass draws in place */
    s_nr.scene_tex_punch_pipe = scene_tex_punch_pipe;
    s_nr.hd_cutout_punch_pipe = hd_cutout_punch_pipe;
    s_nr.hd_scene_blend_pipe = hd_scene_blend_pipe;
    s_nr.hd_scene_add_pipe = hd_scene_add_pipe;
    /* Material draws bind op_pack_tex (set 1) unconditionally — a shader that
     * references it statically needs a valid RGBA descriptor even on draws
     * with no pack replacement. Opaque white keeps it inert if sampled. */
    s_nr.mat_pack_dummy = rhi_create_rgba8(dev, 1, 1);
    if (s_nr.mat_pack_dummy) {
        const unsigned char white[4] = { 255, 255, 255, 255 };
        rhi_update_rgba8(dev, s_nr.mat_pack_dummy, white);
    }
    s_nr.panels_vb    = panels_vb;
    s_nr.scene_vb     = scene_vb;
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
    s_nr.osd_glyph_vb = osd_glyph_vb;           /* may be NULL → no OSD layer  */
    s_nr.osd_quad_vb  = osd_quad_vb;
    s_nr.osd_panel_vb = osd_panel_vb;
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
    /* Terrain sector-mesh GPU cache (default ON; --terrain-cache 0 is the
     * kill switch — it skips the ~130 MB device-memory arenas too). Software
     * vk never draws the retained path (the usable() exclusion), so don't
     * spend the arenas there either — except under the FORCE waiver the
     * llvmpipe parity harness uses. A failed init logs + leaves the CPU
     * walk in place. */
    if (opts->terrain_cache) {
        const char* fe = getenv("OPENPETE_TERRAIN_CACHE_FORCE");
        if ((fe && *fe && *fe != '0') || !rhi_device_is_software(dev))
            openpete_tercache_init(dev);
    }

    /* Replacement atlas up front when a pack is active. Created lazily it
     * lands on the first frame that folds a replacement, and the driver's
     * allocation and residency work for a 2048^2 RGBA image does not show up
     * in the create call or in any GPU zone — it surfaces as ~90 ms inside
     * the next fence wait, which is past the pacer's slip clamp. Paying it
     * during bring-up costs the same time where no deadline is running. */
    if (openpete_native_hdfold_on()) openpete_native_hdfold_prime(dev);

    OPL_INFO_F("native", "Vulkan vk window up (%dx%d, device='%s'); PsyCross hidden",
               w, h, rhi_device_name(dev));
    return 0;
}

/* Can the terrain feeder route unclaimed sectors through the GPU cache this
 * run/present? ANDs the renderer-side conditions the feeder can't see: cache
 * up, no reference-compare scene delay (the cache draws the LIVE walk, not
 * the history ring), no dev tag instruments (their semantics live in the
 * CPU batcher), no PSX vertex wobble (the snap lives in add_tri_grouped),
 * and a hardware GPU (the verdict-heavy vertex shader crawls on llvmpipe).
 * HD texture packs and mod material selectors are NOT whole-run exclusions:
 * the terrain feeder claims affected sectors back per sector (tier 1 of the
 * exclusion→claim→fold ladder), so a UI-/moby-only pack keeps the full
 * cache. */
int openpete_native_tercache_usable(void)
{
    /* OPENPETE_TERRAIN_CACHE_FORCE=1: waive the software-GPU exclusion (dev —
     * llvmpipe renders the path correctly, just slowly; the WSL parity
     * harness diffs screenshots at low frame counts). */
    static int s_force = -1;
    if (s_force < 0) {
        const char* e = getenv("OPENPETE_TERRAIN_CACHE_FORCE");
        s_force = (e && *e && *e != '0');
    }
    return s_nr.enabled
        && openpete_tercache_ready()
        && scene_delay_depth() == 0
        && !tag_split_dev()
        && !s_vertex_snap
        /* fp64 in terrain.vert is what makes the mirror bit-exact vs the
         * CPU walk's double math — no fp64, no retained path. */
        && rhi_device_has_float64(s_nr.dev)
        && (s_force || !rhi_device_is_software(s_nr.dev));
}

uint64_t openpete_native_render_acquire_sheds(void)
{
    return s_nr.dev ? rhi_acquire_sheds(s_nr.dev) : 0;
}

uint64_t openpete_native_render_acquire_timeouts(void)
{
    return s_nr.dev ? rhi_acquire_timeouts(s_nr.dev) : 0;
}

uint32_t openpete_native_render_swapchain_images(void)
{
    return s_nr.dev ? rhi_swapchain_image_count(s_nr.dev) : 0;
}

/* Bounded wait for the last queued present to reach the display. Forwarded
 * rather than exposing the device, and callable only from the presenting
 * thread — see rhi_present_wait. */
int openpete_native_render_present_outstanding(void) {
    return s_nr.dev ? rhi_present_outstanding(s_nr.dev) : 0;
}
int openpete_native_render_present_wait(uint64_t timeout_ns, uint64_t *out_id,
                                        uint64_t *out_waited_ns)
{
    return s_nr.dev ? rhi_present_wait(s_nr.dev, timeout_ns, out_id, out_waited_ns)
                    : -1;
}

uint64_t openpete_native_render_fence_sheds(void)
{
    return s_nr.dev ? rhi_fence_sheds(s_nr.dev) : 0;
}

const char* openpete_native_render_gpu_name(void)
{
    return s_nr.dev ? rhi_device_name(s_nr.dev) : "unknown";
}

const char* openpete_native_render_driver_version(void)
{
    return s_nr.dev ? rhi_driver_version(s_nr.dev) : "unknown";
}

const char* openpete_native_render_present_mode(void)
{
    return s_nr.dev ? rhi_present_mode_name(s_nr.dev) : "none";
}

bool openpete_native_render_enabled(void)
{
    return s_nr.enabled;
}

bool openpete_native_scene_enabled(void)
{
    return s_nr.enabled && s_nr.scene_live;
}

/* Re-present the last composed frame without recomposing it.
 *
 * The offscreen image is persistent and still holds the pixels the previous
 * present blitted, so acquiring a swapchain image and blitting it again puts
 * the same frame back on the display. No scene walk, no extraction, no read
 * of guest RAM or GTE state -- which is the whole point: this is the only
 * present that is safe to issue from a thread other than the one executing
 * the guest.
 *
 * Returns 1 if a present was issued. A false begin_frame (swapchain
 * recreating, no image within the acquire budget, minimised window) is a
 * normal outcome, not an error. */
int openpete_native_render_repeat_last(void)
{
    /* The present thread owns the device while eager production is active;
     * a repeat present from any other thread would race it on the swapchain
     * and queue. Refuse rather than fault — under eager the display is being
     * fed continuously anyway, so there is nothing for a repaint to add. */
    if (openpete_present_thread_active()) return 0;
    if (!s_nr.enabled || !s_nr.dev) return 0;
    /* A pending swapchain rebuild drives SDL window queries and a full
     * teardown; that belongs to the thread that owns the window, so decline
     * and let the next canonical present do it. */
    if (rhi_needs_recreate(s_nr.dev)) return 0;
    if (!rhi_begin_frame(s_nr.dev)) return 0;
    rhi_blit_to_swapchain(s_nr.dev);
    rhi_end_frame(s_nr.dev);
    return 1;
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
    /* The size change this causes lands in ONE step, so neither settle window
     * applies to it: adopt it as soon as it is seen. */
    s_nr.resize_immediate_at_ns = SDL_GetTicksNS(); /* clock-guard: wall */ /* resize debounce */
    SDL_SetWindowFullscreen(s_nr.window, !fs);
    OPL_INFO_F("native", "fullscreen %s", !fs ? "on (borderless desktop)" : "off");
    /* Announced HERE rather than at the call sites: Alt+Enter, the fullscreen
     * binding, a window double-click, the O-menu DISPLAY row and the M-overlay
     * row all funnel through this one function, and the toast should read the
     * same whichever of them the player used. */
    openpete_osd_notify("host", "menu.display", 0,
                        "DISPLAY: %s", !fs ? "FULLSCREEN" : "WINDOWED");
}

/* The native window, or NULL before it exists or when the renderer is off
 * (headless, PsyCross-only). The mouse channel applies relative mode to it. */
SDL_Window* openpete_native_render_window(void)
{
    return s_nr.enabled ? s_nr.window : NULL;
}

/* Live fullscreen state, read straight from the SDL window flags — the O-menu
 * DISPLAY row displays this each frame (no shadow index), so it can never
 * drift from the Alt+Enter / double-click toggles. 0 when there is no native
 * window (headless / PsyCross-only). */
int openpete_native_fullscreen_active(void)
{
    if (!s_nr.enabled || !s_nr.window) return 0;
    return (SDL_GetWindowFlags(s_nr.window) & SDL_WINDOW_FULLSCREEN) != 0;
}

/* The display/clip rects arrive from the guest's PutDispEnv/PutDrawEnv on the
 * thread running the guest; the compose reads s_nr.disp_* and clip_* on the
 * thread presenting, which may be another one. The setters therefore write
 * a guest-side staging copy, and every compose entry copies its source's
 * rects into s_nr at its start (ps_apply_geometry): a scene composed from
 * the ring carries the rects that were current when it was extracted. */
static int s_geom_in[8];   /* disp x,y,w,h, clip x,y,w,h — guest-side */

void openpete_native_set_display_rect(int x, int y, int w, int h)
{
    /* Log size changes only; the disp origin flips every frame (double buffer). */
    if (s_nr.enabled && (w != s_geom_in[2] || h != s_geom_in[3]))
        OPL_INFO_F("native", "disp %dx%d @ (%d,%d) [VRAM px]", w, h, x, y);
    s_geom_in[0] = x; s_geom_in[1] = y; s_geom_in[2] = w; s_geom_in[3] = h;
}

void openpete_native_set_draw_clip(int x, int y, int w, int h)
{
    /* Log on size change; the clip origin flips per frame like disp. The
     * overscan band (per side) is derived from the sizes, not clip.y. */
    if (s_nr.enabled && (w != s_geom_in[6] || h != s_geom_in[7]))
        OPL_INFO_F("native", "draw clip %dx%d @ (%d,%d) [VRAM px] (overscan band=%d/side)",
                   w, h, x, y, s_geom_in[3] > 0 ? (s_geom_in[3] - h) / 2 : 0);
    s_geom_in[4] = x; s_geom_in[5] = y; s_geom_in[6] = w; s_geom_in[7] = h;
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

/* The VRAM journal: every mirror refresh is one entry, tagged with the id of
 * the scene about to be extracted and carrying its own copy of the refreshed
 * texels. A present applies the entries up to ITS scene's id, in order, and
 * no further -- so a scene extracted before a VRAM write is composed against
 * the VRAM it was extracted against, however much later it is presented.
 * Without this the GPU texture simply tracked the newest mirror, and under
 * eager production the world scenes still pending when a pause landed were
 * composed against the pause's VRAM (the framebuffer effect writes over the
 * texture pages the ground samples): the freeze backdrop lost its ground.
 * Inline presents apply everything (one scene, extracted and composed in the
 * same breath). A full refresh (level load, list overflow) carries no copy:
 * it uploads the mirror when applied, which is exact for the newest scene
 * and the best available for an older one. */
typedef struct {
    uint32_t  scene_id;
    int       full;                       /* upload the whole mirror */
    int       nrects;
    rhi_rect  rects[NR_VRAM_MAX_RECTS];
    uint8_t  *pix;                        /* rects' texels, packed as the upload wants them */
    size_t    pix_bytes;
} vram_jentry_t;
#define VRAM_JOURNAL 64
static vram_jentry_t s_vj[VRAM_JOURNAL];
static int  s_vj_head = 0, s_vj_n = 0;    /* ring: oldest at head */
static bool s_ref_dirty = false;          /* readback refreshed; upload pending */

static void vram_journal_pop(void)
{
    vram_jentry_t *e = &s_vj[s_vj_head];
    free(e->pix); e->pix = NULL; e->pix_bytes = 0;
    s_vj_head = (s_vj_head + 1) % VRAM_JOURNAL; s_vj_n--;
}
/* Apply one entry into the current frame (mutex held). */
static void vram_journal_apply(const vram_jentry_t *e)
{
    if (e->full) rhi_update_vram(s_nr.dev, s_nr.vram_tex, s_nr.vram_mirror);
    else if (e->nrects > 0 && e->pix)
        rhi_update_vram_rects_packed(s_nr.dev, s_nr.vram_tex, e->pix, e->pix_bytes, e->rects, e->nrects);
}

/* sync (guest side: drains the dirty rects, refreshes the mirror) and upload
 * (present side: records the mirror rows into the frame) may run on two
 * threads; the mirror ROWS are the payload, so the lock spans both bodies. */
static SDL_Mutex *s_vram_mx;
static SDL_Mutex *vram_mx(void) { if (!s_vram_mx) s_vram_mx = SDL_CreateMutex(); return s_vram_mx; }
static void sync_vram_texture_locked(void);
static void sync_vram_texture(void)
{
    SDL_LockMutex(vram_mx());
    sync_vram_texture_locked();
    SDL_UnlockMutex(s_vram_mx);
}
/* Guest-side entry for the eager producer: refresh the mirror while the
 * guest is idle so no VRAM write can land mid-drain. */
void openpete_native_vram_sync(void) { if (s_nr.enabled && !s_nr.headless) sync_vram_texture(); }
static void sync_vram_texture_locked(void)
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
    /* Refresh the owned mirror from PsyCross's, dirty regions only, and
     * journal the same regions (with their texels) for the presents to apply
     * in scene order (W2.2 staging-ring upload, per frame). */
    if (!s_vram_full && s_vram_nrects == 0) return;
    if (s_vj_n == VRAM_JOURNAL) {
        /* Ring full: the oldest entry is folded into a full refresh of the
         * next one -- an older pending scene then sees newer texels, which is
         * the pre-journal behaviour, once in a very long while. */
        vram_journal_pop();
        s_vram_full = true;
    }
    vram_jentry_t *e = &s_vj[(s_vj_head + s_vj_n) % VRAM_JOURNAL];
    e->scene_id = s_scene_id_next;
    e->pix = NULL; e->pix_bytes = 0; e->nrects = 0;
    if (s_vram_full) {
        GR_ReadVRAM(s_nr.vram_mirror, 0, 0, NR_VRAM_W, NR_VRAM_H);
        e->full = 1;
        s_vram_full = false;
        s_vram_nrects = 0;
        s_vj_n++;
        return;
    }
    e->full = 0;
    size_t need = 0;
    for (int i = 0; i < s_vram_nrects; i++) {
        const rhi_rect r = s_vram_rects[i];
        for (int row = 0; row < r.h; row++)
            GR_ReadVRAM(s_nr.vram_mirror + (size_t)(r.y + row) * NR_VRAM_W + r.x,
                        r.x, r.y + row, r.w, 1);
        need = (need + 3) & ~(size_t)3;
        need += (size_t)r.w * r.h * 2u;
    }
    e->pix = need ? (uint8_t *)malloc(need) : NULL;
    if (!e->pix) { e->full = 1; s_vram_nrects = 0; s_vj_n++; return; }   /* no memory: refresh whole */
    e->pix_bytes = need;
    size_t off = 0;
    for (int i = 0; i < s_vram_nrects; i++) {
        const rhi_rect r = s_vram_rects[i];
        off = (off + 3) & ~(size_t)3;
        for (int row = 0; row < r.h; row++)
            memcpy(e->pix + off + (size_t)row * r.w * 2u,
                   s_nr.vram_mirror + (size_t)(r.y + row) * NR_VRAM_W + r.x, (size_t)r.w * 2u);
        e->rects[e->nrects++] = r;
        off += (size_t)r.w * r.h * 2u;
    }
    s_vram_nrects = 0;
    s_vj_n++;
}

/* Record the pending VRAM/reference uploads into the CURRENT frame's command
 * buffer. Call right after a successful rhi_begin_frame, BEFORE the frame's
 * first render pass (the rhi appends transfer commands + barriers to the
 * frame cb — see rhi_update_vram's contract in rhi.h). */
static void upload_synced_textures(void)
{
    /* Up to and including the scene being composed; everything for an inline
     * present, which has no scene of its own in the ring. */
    const uint32_t upto = s_ps_slot ? s_ps_slot->scene_id : UINT32_MAX;
    SDL_LockMutex(vram_mx());
    while (s_vj_n > 0) {
        const vram_jentry_t *e = &s_vj[s_vj_head];
        if (upto != UINT32_MAX && (int32_t)(e->scene_id - upto) > 0) {
            static int s_held_log = 0;   /* diagnostic: a journal head held back by scene order */
            if (s_held_log < 20) {
                s_held_log++;
                fprintf(stderr, "[vram-journal] held: head scene_id=%u full=%d nrects=%d, present upto=%u, queued=%d\n",
                        e->scene_id, e->full, e->nrects, upto, s_vj_n);
            }
            break;
        }
        vram_journal_apply(e);
        vram_journal_pop();
    }
    if (s_ref_dirty) {
        rhi_update_rgba8(s_nr.dev, s_nr.ref_tex, s_nr.readback);
        s_ref_dirty = false;
    }
    SDL_UnlockMutex(s_vram_mx);
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

/* (Re)create the dedicated widescreen screenshot target at w×h. Grows only when
 * the requested size changes, so a fixed-cadence widescreen capture allocates
 * once and every subsequent dump reuses it (unlike native_rtt, whose ref_w
 * keying would fight a widened width). Must run OUTSIDE a frame. Returns the
 * target or NULL. */
static rhi_texture *ensure_shot_rtt(int w, int h)
{
    if (w <= 0 || h <= 0) return NULL;
    if (s_nr.shot_rtt && s_nr.shot_w == w && s_nr.shot_h == h) return s_nr.shot_rtt;
    rhi_wait_idle(s_nr.dev);
    if (s_nr.shot_rtt) { rhi_destroy_texture(s_nr.dev, s_nr.shot_rtt); s_nr.shot_rtt = NULL; }
    s_nr.shot_rtt = rhi_create_render_target(s_nr.dev, (uint32_t)w, (uint32_t)h);
    s_nr.shot_w = w; s_nr.shot_h = h;
    if (s_nr.shot_rtt)
        OPL_INFO_F("native", "widescreen screenshot target %dx%d", w, h);
    return s_nr.shot_rtt;
}

/* (Re)create ss_rtt — the SSAA supersampled scene target — at the EFFECTIVE
 * factor × the window. The requested --ssaa factor is capped to
 * OPENPETE_SSAA_MAX and to the GPU's max 2D image dimension, then stepped down
 * on an allocation failure (to 1 = SSAA off). No-op when --ssaa is 1, the
 * window size is unknown, or it already matches. Runs OUTSIDE a frame (creates
 * GPU resources / waits idle).
 *
 * NB the scene target is only a fifth of what the factor costs: the key-contest
 * targets are allocated at this same extent (native_terrain_key_pass). */
static void ensure_ss_rtt(void)
{
    int req = (s_nr.ssaa > 1) ? s_nr.ssaa : 1;
    if (req > OPENPETE_SSAA_MAX) req = OPENPETE_SSAA_MAX;   /* VRAM ceiling (openpete.h) */
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

    /* One full-window cell. */
    const int   np  = 1;
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

        /* Hor+ widen factor: how much wider the render view is than 4:3.
         * Native scene only; the reference panel is 4:3. FIXED --aspect (>0)
         * derives it from the flag; AUTO (0, the default) derives it from the
         * CELL shape so the render rect exactly fills the window — re-derived
         * here on every size change (resize/fullscreen), since the layout
         * change-cache keys on the window size. fit_w/fit_h is the crop-
         * corrected 4:3 display shape, so the AUTO ratio is exact regardless
         * of the overscan crop. Clamp ≥1 in both modes — Hor+ only ADDS
         * world; a narrower target just renders 4:3 and the window
         * letterboxes. */
        float wf = 1.0f;
        int wf_auto_fill = 0;   /* AUTO derived from the cell → rect == cell */
        if (s_nr.aspect_wh > 0.0f)
            wf = s_nr.aspect_wh / (4.0f / 3.0f);
        else if (fit_w > 0 && fit_h > 0) {
            wf = ((float)s_nr.win_w * (float)fit_h * chx) /
                 ((float)s_nr.win_h * (float)fit_w);
            wf_auto_fill = (wf >= 1.0f);
        }
        if (wf < 1.0f) wf = 1.0f;
        s_nr.ws_factor = wf;

        /* Fit the FULL render rect (4:3 content widened by wf) into the
         * window → letterbox/pillarbox bars where the window ≠ the fixed
         * --aspect (under AUTO the rect is the window shape → no bars). The
         * 4:3 CONTENT sub-rect (what scene_to_clip maps the canonical
         * 512-wide screen onto, unstretched) is 1/wf of that width and
         * centred; the Hor+ reveal fills the rest of the render rect. */
        float cx = -1.0f + chx, hx, hy;
        if (wf_auto_fill) {
            /* AUTO: wf was derived so the render rect IS the cell — set it
             * exactly instead of round-tripping through fit_cell's integer
             * target aspect, whose rounding left ~1px pillar slivers. */
            hx = chx; hy = 1.0f;
        } else {
            fit_cell(cx, 0.0f, chx, 1.0f, (int)((float)fit_w * wf + 0.5f), fit_h,
                     s_nr.win_w, s_nr.win_h, &hx, &hy);
        }
        s_nr.p0_cx = cx; s_nr.p0_hx = hx / wf; s_nr.p0_hy = hy; /* 4:3 sub-rect for the scene map */
        /* VRAM fallback panel (loading screens / OT-empty): the canonical 4:3
         * framebuffer, shown at the 4:3 sub-rect (unstretched, pillarboxed within
         * the --aspect area). */
        build_panel(&verts[0], cx, 0.0f, s_nr.p0_hx, hy, 0.0f, 0.0f, (float)uv_w, (float)uv_h);
    }
    /* Panel 1 — PsyCross's read-back frame, the display source when no native
     * scene is produced (--renderer psycross). Aspect-fit into the whole
     * window, so a window that is not the reference shape letterboxes rather
     * than stretching. RGBA8 sampled with texture(), so UVs are NORMALIZED,
     * and the GL readback is bottom-up, so v runs 1 at the top to 0 at the
     * bottom — both differ from panel 0, which addresses the VRAM texture in
     * texel units top-down. Drawing panel 0 with the reference bound samples
     * one clamped edge texel and fills the window with a flat colour. */
    if (!s_nr.scene_live) {
        float hx, hy;
        fit_cell(0.0f, 0.0f, 1.0f, 1.0f, s_nr.ref_w, s_nr.ref_h,
                 s_nr.win_w, s_nr.win_h, &hx, &hy);
        build_panel(&verts[6], 0.0f, 0.0f, hx, hy, 0.0f, 1.0f, 1.0f, 0.0f);
    }

    /* The VB may still be read by an in-flight frame; layout changes are rare
     * (startup / resize), so fully sync before rewriting it. */
    rhi_wait_idle(s_nr.dev);
    rhi_update_buffer(s_nr.panels_vb, verts, sizeof(verts));
    s_nr.layout_win_w = s_nr.win_w; s_nr.layout_win_h = s_nr.win_h;
    s_nr.layout_ref_w = s_nr.ref_w; s_nr.layout_ref_h = s_nr.ref_h;
    s_nr.layout_disp_w = s_nr.disp_w; s_nr.layout_clip_h = s_nr.clip_h;
    OPL_DEBUG_F("native", "layout: window %dx%d ref %dx%d wf=%.3f (%s)",
                s_nr.win_w, s_nr.win_h, s_nr.ref_w, s_nr.ref_h,
                (double)s_nr.ws_factor,
                s_nr.aspect_wh > 0.0f ? "fixed --aspect" : "AUTO");
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

/* The CURRENT scene_to_clip mapping's RAW inputs for the terrain-cache
 * vertex stage, which replicates the CPU expression term for term (a
 * probed/fitted affine loses ~5 subpixel LSBs of precision at the screen
 * edges — real rasterized-edge flips vs the CPU-fed draws). Reads the same
 * state scene_to_clip does, so full-extent / freeze_wf can never drift. */
static void tercache_clip_params(openpete_tercache_clip *out)
{
    out->disp_w = s_nr.disp_w > 0 ? (float)s_nr.disp_w : 512.0f;
    float disp_h = s_nr.disp_h > 0 ? (float)s_nr.disp_h : 240.0f;
    out->clip_h = s_nr.clip_h > 0 ? (float)s_nr.clip_h : 224.0f;
    out->band   = (disp_h - out->clip_h) * 0.5f;
    out->x_base = s_nr.p0_cx - s_nr.p0_hx;
    out->x_span = 2.0f * s_nr.p0_hx;
    out->y_base = 0.0f + s_nr.p0_hy;
    out->y_span = 2.0f * s_nr.p0_hy;
    out->freeze_wf = s_render_freeze_wf;
    out->full_extent = s_render_full_extent ? 1 : 0;
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
static openpete_tercache_clip s_od_taff;   /* terrain-cache prepass clip params */

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
    /* Re-resolved once per VBL: setenv is the live channel (dev overlay)
     * and this is called per moby per present — too hot for getenv. */
    extern unsigned openpete_frame;
    static int on = -1;
    static unsigned resolved_f = ~0u;
    if (on < 0 || resolved_f != openpete_frame) {
        const char* e = getenv("OPENPETE_MOBY_OCCLUSION");
        on = !(e && *e == '0');
        resolved_f = openpete_frame;
    }
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
    if (getenv("OPENPETE_DEPTH_PREPASS_DEBUG")) {
        static int last = -9;
        int why = scene_n <= 0 ? 1 : (freeze_ & (1u << 31)) ? 2 :
                  (!scene_has_spyro_shadow(scene, scene_n) && !occl_active()) ? 3 : 0;
        if (why != last) { last = why;
            extern unsigned openpete_frame;
            fprintf(stderr, "[depth-prepass] f=%u gate=%d (0=run 1=no-scene 2=freeze 3=no-consumer occl_active=%d)\n",
                    openpete_frame, why, occl_active()); }
    }
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
     * edge flicker (probe-confirmed sx∈[-316,0] rejects at F=2.67). Also gives the shadow decal terrain depth at the
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
            o->x = bq[k][0]; o->y = bq[k][1]; o->z = 0.999f; o->w = 0.0f;
            o->u = 0.0f; o->v = 0.0f; o->rgba = 0xFF000000u; o->koff = 0.0f;
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
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
            /* Vertex colour retained for vertex-struct completeness; the shadow
             * and occlusion consumers sample the pre-pass depth only. */
            o->u = 0.0f; o->v = 0.0f; o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
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
    /* Terrain-cache prepass affine: the same full-extent mapping, captured
     * while it is active (consumed by the arena draw inside the pass below). */
    tercache_clip_params(&s_od_taff);
    s_render_full_extent = prev_fe;
    s_render_freeze_wf = prev_wf;
    rhi_update_buffer(s_nr.prepass_vb, pbuf, sizeof(rhi_vertex) * (size_t)(m > 0 ? m : 1));

    if (getenv("OPENPETE_DEPTH_PREPASS_DEBUG")) {
        static int last_m = -1;
        if (m != last_m) { last_m = m;
            extern unsigned openpete_frame;
            fprintf(stderr, "[depth-prepass] f=%u WORLD-opaque tris=%d (of %d scene tris) raster %dx%d in %dx%d\n",
                    openpete_frame, m/3, scene_n, vw, vh, s_nr.od_w, s_nr.od_h); }
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
        if (pr && *pr && ps_shadow()->valid && printed < 40 &&
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
                    W[0][i] = ps_shadow()->cam_col0[i];
                    W[1][i] = ps_shadow()->cam_col1[i];
                    W[2][i] = ps_shadow()->cam_col2[i];
                }
                double P[3][3];
                for (int k = 0; k < 3; k++) {
                    double z = scene[t].v[k].z; if (z > 0.9999) z = 0.9999;
                    double vz = 4096.0 * z / (1.0 - z); if (vz < 1.0) vz = 1.0;
                    double M1 = (scene[t].v[k].sx - ps_shadow()->ofx) * vz / ps_shadow()->h;
                    double M2 = (scene[t].v[k].sy - ps_shadow()->ofy) * vz / ps_shadow()->h;
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

    /* Render the opaque depth (begin clears depth to far=1.0). Even with m==0
     * the cleared far-depth reads as "no occluder" — never a stale frame. */
    rhi_begin_render_target(s_nr.dev, s_nr.opaque_depth, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_set_viewport(s_nr.dev, vw, vh);
    if (m > 0) {
        rhi_bind_pipeline(s_nr.dev, s_nr.prepass_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.prepass_vb);
        rhi_draw(s_nr.dev, (uint32_t)m, 0);
    }
    /* Terrain-cache occluders: depth-only draw of the retained WORLD terrain
     * (unclaimed sectors), matching the CPU extraction's WORLD + WORLD_FILL
     * coverage above so the shadow-decal / occlusion consumers see the whole
     * opaque terrain regardless of which path fed it. */
    if (openpete_tercache_frame_open())
        openpete_tercache_draw(OPTC_DRAW_PREPASS, &s_od_taff, s_nr.vram_tex, 0, NULL, NULL);
    rhi_end_render_target(s_nr.dev, s_nr.opaque_depth);
    s_od_vw = (float)vw; s_od_vh = (float)vh;
    s_od_ready = 1;
}

/* ---- P6b: terrain HP KEY pass ----------------------------------------------
 *
 * og's terrain rule is a per-pixel KEY contest: a face's OT key is its view-Z
 * plus the authored slot offset (LP +0x40+8*bias, HP +4*bias, 128 world/slot),
 * and LP owns a pixel only where its key is nearer than HP's. The scene's
 * stored depth is TRUE view-Z (the select never enters the depth buffer), so
 * the contest needs its own image: rasterise opaque HP terrain with depth =
 * key view-Z (affine-encoded, terrain_key.vert) at the SCENE TARGET's exact
 * size, and let LP fragments texelFetch it at gl_FragCoord
 * (psx_scene_key.frag / quad_key.frag / hd_tex_cutout.frag).
 *
 * Called at each scene-target call site BEFORE the target's render pass
 * begins (a pass can't nest); `full_extent` mirrors the draw_native_scene
 * argument so scene_to_clip packs identical positions. */
int openpete_native_key_resolve_on(void)
{
    return s_nr.enabled && s_nr.terrain_key_pipe && s_nr.terrain_key_vb;
}

static void ensure_terrain_key_depth(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (s_nr.terrain_key_depth && s_nr.moby_key_depth && s_nr.actor_key_depth &&
        s_nr.moby_rank_rt && s_nr.tk_w >= w && s_nr.tk_h >= h) return;
    /* Never while a frame is recording. The scene pipelines' layouts declare
     * these four images as descriptor sets 1-4 and their shaders read them
     * unconditionally, so the recording command buffer may already reference
     * the current sets; destroying them here invalidates it (a driver fault
     * at submit, or at the next draw), and a set created here is bound to
     * nothing the frame has drawn so far. ensure_terrain_key_targets sizes
     * them in the per-present resource pass instead. */
    if (rhi_frame_active(s_nr.dev)) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            OPL_WARN_F("native", "key-contest targets %dx%d requested mid-frame (have %dx%d); "
                       "allocation deferred to the next resource pass", w, h, s_nr.tk_w, s_nr.tk_h);
        }
        return;
    }
    if (w < s_nr.tk_w) w = s_nr.tk_w;
    if (h < s_nr.tk_h) h = s_nr.tk_h;
    rhi_wait_idle(s_nr.dev);
    if (s_nr.terrain_key_depth) { rhi_destroy_texture(s_nr.dev, s_nr.terrain_key_depth); s_nr.terrain_key_depth = NULL; }
    if (s_nr.moby_key_depth)    { rhi_destroy_texture(s_nr.dev, s_nr.moby_key_depth);    s_nr.moby_key_depth = NULL; }
    if (s_nr.actor_key_depth)   { rhi_destroy_texture(s_nr.dev, s_nr.actor_key_depth);   s_nr.actor_key_depth = NULL; }
    if (s_nr.moby_rank_rt)      { rhi_destroy_texture(s_nr.dev, s_nr.moby_rank_rt);      s_nr.moby_rank_rt = NULL; }
    s_nr.terrain_key_depth = rhi_create_render_target_depth_sampled(s_nr.dev, (uint32_t)w, (uint32_t)h);
    s_nr.moby_key_depth    = rhi_create_render_target_depth_sampled(s_nr.dev, (uint32_t)w, (uint32_t)h);
    s_nr.actor_key_depth   = rhi_create_render_target_depth_sampled(s_nr.dev, (uint32_t)w, (uint32_t)h);
    s_nr.moby_rank_rt      = rhi_create_render_target(s_nr.dev, (uint32_t)w, (uint32_t)h);
    if (s_nr.terrain_key_depth && s_nr.moby_key_depth && s_nr.actor_key_depth &&
        s_nr.moby_rank_rt)
        rhi_texture_extent(s_nr.terrain_key_depth, &s_nr.tk_w, &s_nr.tk_h);
    else {
        /* All-or-nothing: the scene pipes' layouts sample sets 1/2/3/4 as one
         * contract, so a partial allocation is released rather than half
         * bound. This only turns the KEY PASS off (native_terrain_key_pass
         * checks for the images); the scene pipelines still read the four
         * sets unconditionally, so a scene drawn after an allocation failure
         * here is undefined behaviour on every driver. It is an out-of-memory
         * condition on a device that has just allocated the scene targets
         * themselves, and it is logged as such. */
        if (s_nr.terrain_key_depth) { rhi_destroy_texture(s_nr.dev, s_nr.terrain_key_depth); s_nr.terrain_key_depth = NULL; }
        if (s_nr.moby_key_depth)    { rhi_destroy_texture(s_nr.dev, s_nr.moby_key_depth);    s_nr.moby_key_depth = NULL; }
        if (s_nr.actor_key_depth)   { rhi_destroy_texture(s_nr.dev, s_nr.actor_key_depth);   s_nr.actor_key_depth = NULL; }
        if (s_nr.moby_rank_rt)      { rhi_destroy_texture(s_nr.dev, s_nr.moby_rank_rt);      s_nr.moby_rank_rt = NULL; }
        s_nr.tk_w = s_nr.tk_h = 0;
        OPL_WARN_F("native", "key-contest targets %dx%d could not be allocated; scene draws are undefined", w, h);
    }
}

/* Per-present resource pass: size the key-contest images for every scene
 * target this present can draw into, BEFORE the frame begins. The scene
 * pipelines (gouraud, textured, cutout and their blend variants) bind these
 * four images as descriptor sets 1-4 on every draw, chrome included — the
 * stats overlay's supersample pre-pass is a gouraud draw and, under eager
 * production, the first draw of the first present, ahead of any scene. A
 * draw whose pipeline statically reads a set that was never bound is
 * undefined behaviour: Mesa ANV faults inside vkCmdDraw on it. Sizing here
 * is the union of the extents the key pass is called with (native_rtt,
 * the SSAA target, the window), so the key pass never needs to allocate
 * mid-frame. */
static void ensure_terrain_key_targets(void)
{
    if (!openpete_native_key_resolve_on()) return;
    int w = s_nr.win_w, h = s_nr.win_h;
    if (s_nr.native_rtt) {
        int kw, kh; rhi_texture_extent(s_nr.native_rtt, &kw, &kh);
        if (kw > w) w = kw;
        if (kh > h) h = kh;
    }
    if (s_nr.ss_rtt) {
        int kw, kh; rhi_texture_extent(s_nr.ss_rtt, &kw, &kh);
        if (kw > w) w = kw;
        if (kh > h) h = kh;
    }
    ensure_terrain_key_depth(w, h);
}

/* Pack one scene target's key batch from `scene` under the `full_extent`
 * clip mapping — a PURE function of (scene snapshot, mapping), shared by the
 * key pass and the OPENPETE_TERRAIN_KEY_IDENT instrument, which repacks at
 * scene-draw time and byte-compares to prove pass and draw consumed the
 * same snapshot (the draw fetches scene_present_buf independently). */
/* prop_class 0 = TERRAIN og KEYS (the set-2 image): keyed WORLD faces as
 * their ABSOLUTE og key laid FLAT, og's per-pixel OT occupancy (the P6b
 * painter select).
 * prop_class 1 = PROP TRUE DEPTH (the set-3 image): curated embedded-prop
 * faces only (openpete_scene_tri.prop), per-vertex TRUE view-Z, no bias.
 * prop_class 2 = MOBY og KEYS (the set-1 image): keyed opaque ACTOR faces
 * as their ABSOLUTE og key laid FLAT (P6g). Split from set 2 so fragments
 * can hold a moby's paint-over to keys in front of their own TRUE surface
 * while terrain-vs-terrain keeps contesting authored (pushed) keys — a
 * moby pull and a terrain push must not compound into a punch-through
 * window wider than either authored intent.
 * The near-band embed rule reads it: no geometric quantity separates "prop
 * set into its ground" from "actor just behind a crest" — flat keys,
 * per-pixel keys, biased and unbiased margins all overlap across the two
 * classes — so prop-ness is AUTHORED (a class list in the moby feeder,
 * the k_lp_artfix seam pattern), and only props get the always-win-your-
 * ground divergence. */
/* Material runs of the TEXTURED tail of a key batch (see
 * pack_terrain_key_batch): [first, first+count) verts drawn through the
 * alpha-aware cutout pipe with `mat` pushed. A batch's verts are laid out
 * untextured-prefix first, textured material runs after. */
#define KEYTEX_MAX_SPANS 256
typedef struct { int first, count; psx_material mat; } keytex_span_t;

static int s_ktex_idx[MAX_SCENE_TRIS];
static void tex_mat_sort(const scene_tri_t* scene, int* idx, int n);

static void pack_key_vert(const scene_tri_t* T, int prop_class, int tex,
                          rhi_vertex* out)
{
    float K = T->koff - 0.5f;
    float zflat = K / (K + OPENPETE_DEPTH_VIEWZ_SCALE);
    for (int k = 0; k < 3; k++) {
        float cx, cy;
        scene_to_clip(T->v[k].sx, T->v[k].sy, &cx, &cy);
        rhi_vertex* o = &out[k];
        /* TRUE per-vertex view-Z: pvz where the tie pull shifted the stored
         * z, else the stored (canonical) z already IS true. Drives the prop
         * image's true-depth raster and the cutout stage's perspective UV. */
        float tv = T->v[k].pvz;
        if (tv <= 0.0f) {
            float sz = T->v[k].z;
            tv = sz < 1.0f ? OPENPETE_DEPTH_VIEWZ_SCALE * sz / (1.0f - sz) : 0.0f;
        }
        float z = zflat;
        if (prop_class == 1)
            z = tv > 0.0f ? tv / (tv + OPENPETE_DEPTH_VIEWZ_SCALE)
                          : T->v[k].z;
        o->x = cx; o->y = cy; o->z = z; o->w = tex ? tv : 0.0f;
        o->u = tex ? T->v[k].u : 0.0f;
        o->v = tex ? T->v[k].v : 0.0f;
        o->rgba = 0xFF000000u;
        o->koff = 0.5f;
    }
}

/* Pack one key batch: UNTEXTURED keyed faces first (full-footprint flat
 * raster — a gouraud face covers its whole triangle), then TEXTURED keyed
 * faces sorted into per-material runs for the cutout pipe, which drops the
 * PSX p==0 transparent texels so a face's key occupies exactly the pixels
 * its opaque texels paint (og OT occupancy — a cloak edge or fence lattice
 * must not key-discard what its holes show). *out_untex receives the
 * untextured prefix length; spans/nspans the textured runs. Span-table
 * overflow degrades the remainder to the untextured (full-footprint) pipe —
 * the pre-cutout behaviour, drawn as a trailing sentinel span (bpp < 0). */
static int pack_terrain_key_batch(const scene_tri_t* scene, int scene_n,
                                  int full_extent, int prop_class, rhi_vertex* out,
                                  int* out_untex, keytex_span_t* spans, int* nspans)
{
    int prev_fe = s_render_full_extent;
    s_render_full_extent = full_extent ? 1 : 0;
    int m = 0, tex_n = 0;
    *nspans = 0;
    for (int t = 0; t < scene_n && m + 3 <= SCENE_VB_VERTS; t++) {
        if (scene[t].blend != 0) continue;                       /* opaque only */
        if (prop_class == 1 ? !scene[t].prop
            : prop_class == 2 ? scene[t].category != OPENPETE_SCENE_ACTOR
                              : scene[t].category != OPENPETE_SCENE_WORLD) continue;
        if (scene[t].koff <= 0.25f) continue;   /* keyed faces / sky fans only */
        if (scene[t].textured) {
            if (tex_n < MAX_SCENE_TRIS) s_ktex_idx[tex_n++] = t;
            continue;
        }
        pack_key_vert(&scene[t], prop_class, 0, &out[m]);
        m += 3;
    }
    *out_untex = m;
    /* Material-run order for the textured tail — same intern/sort the scene
     * draw batches with, so runs are contiguous and few. */
    tex_mat_sort(scene, s_ktex_idx, tex_n);
    for (int i = 0; i < tex_n && m + 3 <= SCENE_VB_VERTS; i++) {
        const scene_tri_t* T = &scene[s_ktex_idx[i]];
        keytex_span_t* sp = (*nspans > 0) ? &spans[*nspans - 1] : NULL;
        if (!sp || sp->mat.bpp < 0 ||
            sp->mat.tpage_x != T->tpage_x || sp->mat.tpage_y != T->tpage_y ||
            sp->mat.clut_x != T->clut_x || sp->mat.clut_y != T->clut_y ||
            sp->mat.bpp != T->bpp) {
            if (sp && sp->mat.bpp < 0) {
                /* overflow sentinel already open: keep appending to it */
            } else if (*nspans < KEYTEX_MAX_SPANS - 1) {
                sp = &spans[(*nspans)++];
                sp->first = m; sp->count = 0;
                sp->mat.tpage_x = T->tpage_x; sp->mat.tpage_y = T->tpage_y;
                sp->mat.clut_x = T->clut_x; sp->mat.clut_y = T->clut_y;
                sp->mat.bpp = T->bpp;
            } else {
                /* table full: remainder rasters full-footprint (flat pipe) */
                sp = &spans[(*nspans)++];
                sp->first = m; sp->count = 0;
                sp->mat.bpp = -1;
            }
        }
        pack_key_vert(T, prop_class, sp->mat.bpp >= 0, &out[m]);
        m += 3;
        sp->count += 3;
    }
    s_render_full_extent = prev_fe;
    return m;
}

/* Moby RANK batch: ranked opaque moby faces rasterised with depth = their og
 * OT walk rank (moby_rank.vert encodes 1-(rank+1)/65536, so the LESS test
 * keeps og's LAST-painted face per pixel) into the RGBA8 (rank, ordinal)
 * image. Same untextured-prefix + textured-material-run layout as the key
 * batches; textured faces cut transparent texels out of the raster (rank
 * exists only on opaque texels — the alpha-aware key rule). */
static void pack_rank_vert(const scene_tri_t* T, int tex, rhi_vertex* out)
{
    for (int k = 0; k < 3; k++) {
        float cx, cy;
        scene_to_clip(T->v[k].sx, T->v[k].sy, &cx, &cy);
        rhi_vertex* o = &out[k];
        float tv = T->v[k].pvz;
        if (tv <= 0.0f) {
            float sz = T->v[k].z;
            tv = sz < 1.0f ? OPENPETE_DEPTH_VIEWZ_SCALE * sz / (1.0f - sz) : 0.0f;
        }
        /* w carries TRUE view-Z for every vertex: the cutout stage's UV
         * perspective (textured) AND the owner-depth the frag encodes into
         * the rank image alpha (the select's near-tie margin). */
        o->x = cx; o->y = cy; o->z = 0.0f; o->w = tv;
        o->u = tex ? T->v[k].u : 0.0f;
        o->v = tex ? T->v[k].v : 0.0f;
        o->rgba = 0xFF000000u;
        o->koff = 0.0f;
        o->sx = T->rank; o->sy = (float)T->mordinal;
    }
}

static int pack_moby_rank_batch(const scene_tri_t* scene, int scene_n,
                                int full_extent, rhi_vertex* out,
                                int* out_untex, keytex_span_t* spans, int* nspans)
{
    int prev_fe = s_render_full_extent;
    s_render_full_extent = full_extent ? 1 : 0;
    int m = 0, tex_n = 0;
    *nspans = 0;
    for (int t = 0; t < scene_n && m + 3 <= SCENE_VB_VERTS; t++) {
        if (scene[t].blend != 0) continue;      /* opaque only */
        if (scene[t].rank < 0.5f) continue;     /* ranked faces only */
        if (scene[t].textured) {
            if (tex_n < MAX_SCENE_TRIS) s_ktex_idx[tex_n++] = t;
            continue;
        }
        pack_rank_vert(&scene[t], 0, &out[m]);
        m += 3;
    }
    *out_untex = m;
    tex_mat_sort(scene, s_ktex_idx, tex_n);
    for (int i = 0; i < tex_n && m + 3 <= SCENE_VB_VERTS; i++) {
        const scene_tri_t* T = &scene[s_ktex_idx[i]];
        keytex_span_t* sp = (*nspans > 0) ? &spans[*nspans - 1] : NULL;
        if (!sp || sp->mat.bpp < 0 ||
            sp->mat.tpage_x != T->tpage_x || sp->mat.tpage_y != T->tpage_y ||
            sp->mat.clut_x != T->clut_x || sp->mat.clut_y != T->clut_y ||
            sp->mat.bpp != T->bpp) {
            if (sp && sp->mat.bpp < 0) {
                /* overflow sentinel already open: keep appending to it */
            } else if (*nspans < KEYTEX_MAX_SPANS - 1) {
                sp = &spans[(*nspans)++];
                sp->first = m; sp->count = 0;
                sp->mat.tpage_x = T->tpage_x; sp->mat.tpage_y = T->tpage_y;
                sp->mat.clut_x = T->clut_x; sp->mat.clut_y = T->clut_y;
                sp->mat.bpp = T->bpp;
            } else {
                /* table full: remainder rasters full-footprint (flat pipe) */
                sp = &spans[(*nspans)++];
                sp->first = m; sp->count = 0;
                sp->mat.bpp = -1;
            }
        }
        pack_rank_vert(T, sp->mat.bpp >= 0, &out[m]);
        m += 3;
        sp->count += 3;
    }
    s_render_full_extent = prev_fe;
    return m;
}

/* The last key-pass packs (terrain + prop + actor), recorded for the identity
 * instrument in draw_native_scene (m == -1 until a pass has run). */
static struct {
    const scene_tri_t* scene;
    int scene_n, full_extent, m, mm, am;
    const rhi_vertex* buf;
    const rhi_vertex* mbuf;
    const rhi_vertex* abuf;
} s_tk_last = { NULL, 0, -1, -1, -1, -1, NULL, NULL, NULL };

/* Draw one packed key batch into the currently-open key render pass:
 * untextured prefix through the flat full-footprint pipe, textured material
 * runs through the alpha-aware cutout pipe (transparent texels stamp no
 * key). `base` is the batch's bump-allocated offset in terrain_key_vb. */
static void draw_key_batch(int base, int m, int untex,
                           const keytex_span_t* spans, int nspans)
{
    if (m <= 0) return;
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.terrain_key_vb);
    if (untex > 0) {
        rhi_bind_pipeline(s_nr.dev, s_nr.terrain_key_pipe);
        rhi_draw(s_nr.dev, (uint32_t)untex, (uint32_t)base);
    }
    int tex_bound = 0;
    for (int i = 0; i < nspans; i++) {
        if (spans[i].count <= 0) continue;
        if (spans[i].mat.bpp < 0) {   /* span-table overflow: full-footprint */
            rhi_bind_pipeline(s_nr.dev, s_nr.terrain_key_pipe);
            rhi_draw(s_nr.dev, (uint32_t)spans[i].count,
                     (uint32_t)(base + spans[i].first));
            tex_bound = 0;
            continue;
        }
        if (!s_nr.terrain_key_tex_pipe) {   /* no cutout pipe: degrade flat */
            rhi_bind_pipeline(s_nr.dev, s_nr.terrain_key_pipe);
            rhi_draw(s_nr.dev, (uint32_t)spans[i].count,
                     (uint32_t)(base + spans[i].first));
            continue;
        }
        if (!tex_bound) {
            rhi_bind_pipeline(s_nr.dev, s_nr.terrain_key_tex_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            tex_bound = 1;
        }
        rhi_push_constants(s_nr.dev, &spans[i].mat, sizeof(spans[i].mat));
        rhi_draw(s_nr.dev, (uint32_t)spans[i].count,
                 (uint32_t)(base + spans[i].first));
    }
}

/* Draw one packed RANK batch into the currently-open rank render pass:
 * untextured prefix through the flat pipe, textured material runs through
 * the alpha-aware cutout pipe. Same span discipline as draw_key_batch. */
static void draw_rank_batch(int base, int m, int untex,
                            const keytex_span_t* spans, int nspans)
{
    if (m <= 0 || !s_nr.moby_rank_pipe) return;
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.terrain_key_vb);
    if (untex > 0) {
        rhi_bind_pipeline(s_nr.dev, s_nr.moby_rank_pipe);
        rhi_draw(s_nr.dev, (uint32_t)untex, (uint32_t)base);
    }
    int tex_bound = 0;
    for (int i = 0; i < nspans; i++) {
        if (spans[i].count <= 0) continue;
        if (spans[i].mat.bpp < 0 || !s_nr.moby_rank_cutout_pipe) {
            rhi_bind_pipeline(s_nr.dev, s_nr.moby_rank_pipe);
            rhi_draw(s_nr.dev, (uint32_t)spans[i].count,
                     (uint32_t)(base + spans[i].first));
            tex_bound = 0;
            continue;
        }
        if (!tex_bound) {
            rhi_bind_pipeline(s_nr.dev, s_nr.moby_rank_cutout_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            tex_bound = 1;
        }
        rhi_push_constants(s_nr.dev, &spans[i].mat, sizeof(spans[i].mat));
        rhi_draw(s_nr.dev, (uint32_t)spans[i].count,
                 (uint32_t)(base + spans[i].first));
    }
}

static void native_terrain_key_pass(int full_extent, int vw, int vh)
{
    if (!openpete_native_key_resolve_on()) return;
    /* The images were sized by ensure_terrain_key_targets before the frame
     * began; a request larger than that clamps to what exists rather than
     * reallocating inside the recording frame. */
    if (!s_nr.terrain_key_depth || !s_nr.moby_key_depth || !s_nr.actor_key_depth) return;
    if (vw > s_nr.tk_w) vw = s_nr.tk_w;
    if (vh > s_nr.tk_h) vh = s_nr.tk_h;
    int scene_n; uint32_t bg_, fade_, freeze_; int hud_;
    const scene_tri_t* scene = scene_present_buf(&scene_n, &bg_, &fade_, &freeze_, &hud_, NULL);
    static rhi_vertex kbuf[SCENE_VB_VERTS];
    static rhi_vertex mkbuf[SCENE_VB_VERTS];
    static rhi_vertex akbuf[SCENE_VB_VERTS];
    static rhi_vertex rbuf[SCENE_VB_VERTS];
    static keytex_span_t kspans[KEYTEX_MAX_SPANS], mspans[KEYTEX_MAX_SPANS],
                         aspans[KEYTEX_MAX_SPANS], rspans[KEYTEX_MAX_SPANS];
    int k_untex = 0, m_untex = 0, a_untex = 0, r_untex = 0,
        k_nspans = 0, m_nspans = 0, a_nspans = 0, r_nspans = 0;
    int m  = pack_terrain_key_batch(scene, scene_n, full_extent, 0, kbuf,
                                    &k_untex, kspans, &k_nspans);
    int mm = pack_terrain_key_batch(scene, scene_n, full_extent, 1, mkbuf,
                                    &m_untex, mspans, &m_nspans);
    int am = pack_terrain_key_batch(scene, scene_n, full_extent, 2, akbuf,
                                    &a_untex, aspans, &a_nspans);
    int rr = (s_nr.moby_rank_rt && s_nr.moby_rank_pipe)
           ? pack_moby_rank_batch(scene, scene_n, full_extent, rbuf,
                                  &r_untex, rspans, &r_nspans)
           : 0;
    { static int s_rd = -1;
      if (s_rd < 0) { const char* e = getenv("OPENPETE_MOBY_RANK_DBG"); s_rd = (e && *e && *e != '0'); }
      if (s_rd && rr > 0) {
        extern unsigned openpete_frame;
        fprintf(stderr, "[rankpass f=%u] rr=%d untex=%d nspans=%d cutout=%d\n",
                openpete_frame, rr/3, r_untex/3, r_nspans,
                s_nr.moby_rank_cutout_pipe != NULL);
      } }
    s_tk_last.scene = scene; s_tk_last.scene_n = scene_n;
    s_tk_last.full_extent = full_extent ? 1 : 0;
    s_tk_last.m = m; s_tk_last.buf = kbuf;
    s_tk_last.mm = mm; s_tk_last.mbuf = mkbuf;
    s_tk_last.am = am; s_tk_last.abuf = akbuf;
    { static int kdbg = -1;
      if (kdbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_KEY_DBG"); kdbg = (e && *e && *e != '0'); }
      if (kdbg) {
        extern unsigned openpete_frame;
        fprintf(stderr, "[keypass f=%u] vw=%d vh=%d tris=%d\n", openpete_frame, vw, vh, m/3);
        { const char* kd = getenv("OPENPETE_TERRAIN_KEY_DUMP");
          if (kd && *kd) { FILE* fp = fopen(kd, "a");
            if (fp) { fprintf(fp, "# pass vw=%d vh=%d fe=%d m=%d\n", vw, vh, full_extent, m);
              for (int t3 = 0; t3 + 3 <= m; t3 += 3)
                fprintf(fp, "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.8f\n",
                        (double)kbuf[t3].x,(double)kbuf[t3].y,(double)kbuf[t3+1].x,
                        (double)kbuf[t3+1].y,(double)kbuf[t3+2].x,(double)kbuf[t3+2].y,
                        (double)kbuf[t3].z);
              fclose(fp); } } }
        /* OPENPETE_TERRAIN_KEY_PX=wx,wy: every kbuf tri covering that WALK
         * point (via the pass's own scene_to_clip), with its flat key. */
        { static int kpx = -1; static float wx, wy;
          if (kpx < 0) { const char* e = getenv("OPENPETE_TERRAIN_KEY_PX");
              kpx = (e && *e && sscanf(e, "%f,%f", &wx, &wy) == 2) ? 1 : 0; }
          if (kpx) {
            float pcx, pcy;
            int pf = s_render_full_extent; s_render_full_extent = full_extent ? 1 : 0;
            scene_to_clip(wx, wy, &pcx, &pcy);
            s_render_full_extent = pf;
            for (int t3 = 0; t3 + 3 <= m; t3 += 3) {
                float x0=kbuf[t3].x, y0=kbuf[t3].y, x1=kbuf[t3+1].x, y1=kbuf[t3+1].y,
                      x2=kbuf[t3+2].x, y2=kbuf[t3+2].y;
                float det=(x1-x0)*(y2-y0)-(x2-x0)*(y1-y0);
                if (det > -1e-12f && det < 1e-12f) continue;
                float b1=((pcx-x0)*(y2-y0)-(x2-x0)*(pcy-y0))/det;
                float b2=((x1-x0)*(pcy-y0)-(pcx-x0)*(y1-y0))/det;
                float b0=1.0f-b1-b2;
                if (b0 < -0.001f || b1 < -0.001f || b2 < -0.001f) continue;
                float K = 4096.0f*kbuf[t3].z/(1.0f-kbuf[t3].z);
                fprintf(stderr, "[keycover] K=%.0f xy=(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f)\n",
                        K, x0,y0,x1,y1,x2,y2);
            }
          } }
        for (int t3 = 0; t3 + 3 <= m; t3 += 3) {
            float x0=kbuf[t3].x, x1=kbuf[t3+1].x, x2=kbuf[t3+2].x;
            float y0=kbuf[t3].y, y1=kbuf[t3+1].y, y2=kbuf[t3+2].y;
            float xmin=fminf(x0,fminf(x1,x2)), xmax=fmaxf(x0,fmaxf(x1,x2));
            float ymin=fminf(y0,fminf(y1,y2)), ymax=fmaxf(y0,fmaxf(y1,y2));
            float K = 4096.0f*kbuf[t3].z/(1.0f-kbuf[t3].z);
            /* wide-in-clip tris that overlap the viewport: the phantom class */
            if ((xmax-xmin) > 0.8f && xmax > -1.0f && xmin < 1.0f && ymax > -1.0f && ymin < 1.0f)
                fprintf(stderr, "[keytri] K=%.0f x=[%.2f,%.2f] y=[%.2f,%.2f]\n",
                        K, xmin, xmax, ymin, ymax);
        }
      } }
    /* BUMP-ALLOCATE within the ring slot: one rhi frame can run SEVERAL
     * scene targets (native_rtt and the windowed offscreen), each
     * with its own key batch — and a second same-frame rhi_update_buffer at
     * offset 0 would clobber the vertex data a RECORDED-but-unexecuted
     * earlier key pass reads at submit (one command buffer per frame). At
     * 4:3 every batch happened to pack identical bytes, masking it; mixed
     * widened/windowed mappings made the first target sample the second
     * target's geometry. Offsets reset when rhi_begin_frame's serial
     * changes. */
    static uint64_t s_tk_serial = 0;
    static int s_tk_base = 0;
    uint64_t serial = rhi_frame_serial(s_nr.dev);
    if (serial != s_tk_serial) { s_tk_serial = serial; s_tk_base = 0; }
    if (m + mm + am + rr > 0 && s_tk_base + m + mm + am + rr > SCENE_VB_VERTS) {
        /* Ring slot exhausted (several huge batches in one frame). The
         * earlier batches are recorded but NOT yet submitted, so resetting
         * the bump offset would clobber data a recorded pass still reads at
         * execution — and a mid-frame wait_idle cannot help, the work isn't
         * on the GPU yet. Degrade safely: leave this target's images clear
         * ("no cover" → contest OFF for this target only, keyed faces
         * resolve at true depth), and say so loudly — a silent skip would
         * read as coverage. */
        extern unsigned openpete_frame;
        fprintf(stderr, "[keypass f=%u] key VB exhausted (base=%d + m=%d+%d+%d+%d > %d): "
                        "contest OFF for this target\n",
                openpete_frame, s_tk_base, m, mm, am, rr, (int)SCENE_VB_VERTS);
        m = mm = am = rr = 0;
    }
    if (m > 0)
        rhi_update_buffer_range(s_nr.terrain_key_vb, kbuf,
                                sizeof(rhi_vertex) * (size_t)s_tk_base,
                                sizeof(rhi_vertex) * (size_t)m);
    if (mm > 0)
        rhi_update_buffer_range(s_nr.terrain_key_vb, mkbuf,
                                sizeof(rhi_vertex) * (size_t)(s_tk_base + m),
                                sizeof(rhi_vertex) * (size_t)mm);
    if (am > 0)
        rhi_update_buffer_range(s_nr.terrain_key_vb, akbuf,
                                sizeof(rhi_vertex) * (size_t)(s_tk_base + m + mm),
                                sizeof(rhi_vertex) * (size_t)am);
    if (rr > 0)
        rhi_update_buffer_range(s_nr.terrain_key_vb, rbuf,
                                sizeof(rhi_vertex) * (size_t)(s_tk_base + m + mm + am),
                                sizeof(rhi_vertex) * (size_t)rr);
    /* Even empty batches render: the cleared far depth reads as "no cover",
     * which is the correct contest input — never a stale image. */
    rhi_begin_render_target(s_nr.dev, s_nr.terrain_key_depth, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_set_viewport(s_nr.dev, vw, vh);
    draw_key_batch(s_tk_base, m, k_untex, kspans, k_nspans);
    /* Retained terrain emits its og keys here too — CPU-fed faces (and
     * mobys) must contest retained ground exactly as they contest CPU-fed
     * ground. Same target, same flat-key depth encoding (terrain.vert
     * pass_sel 2), the pass's own clip mapping. */
    if (openpete_tercache_frame_open()) {
        int prev_fe = s_render_full_extent;
        s_render_full_extent = full_extent ? 1 : 0;
        openpete_tercache_clip taff; tercache_clip_params(&taff);
        s_render_full_extent = prev_fe;
        openpete_tercache_draw(OPTC_DRAW_KEY, &taff, s_nr.vram_tex, 0, NULL, NULL);
    }
    rhi_end_render_target(s_nr.dev, s_nr.terrain_key_depth);
    rhi_begin_render_target(s_nr.dev, s_nr.moby_key_depth, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_set_viewport(s_nr.dev, vw, vh);
    draw_key_batch(s_tk_base + m, mm, m_untex, mspans, m_nspans);
    rhi_end_render_target(s_nr.dev, s_nr.moby_key_depth);
    rhi_begin_render_target(s_nr.dev, s_nr.actor_key_depth, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_set_viewport(s_nr.dev, vw, vh);
    draw_key_batch(s_tk_base + m + mm, am, a_untex, aspans, a_nspans);
    rhi_end_render_target(s_nr.dev, s_nr.actor_key_depth);
    if (s_nr.moby_rank_rt) {
        /* Rank image: colour clears to 0 (decodes rank 0 = "no cover"), depth
         * to far (rank -1); an empty batch still renders the clear. */
        rhi_begin_render_target(s_nr.dev, s_nr.moby_rank_rt, 0.0f, 0.0f, 0.0f, 0.0f);
        rhi_set_viewport(s_nr.dev, vw, vh);
        draw_rank_batch(s_tk_base + m + mm + am, rr, r_untex, rspans, r_nspans);
        rhi_end_render_target(s_nr.dev, s_nr.moby_rank_rt);
    }
    s_tk_base += m + mm + am + rr;
}

/* Bind a pipeline for scene drawing; the key-resolve pipes (layouts carrying
 * sampler set 2) additionally get the HP key image bound there — done at
 * pipeline-bind time so the invariant needs no per-site reasoning. Plain
 * passthrough for every other pipeline. */
static void bind_scene_pipe(rhi_pipeline* p)
{
    rhi_bind_pipeline(s_nr.dev, p);
    if (!s_nr.terrain_key_depth || !s_nr.moby_key_depth || !s_nr.actor_key_depth) return;
    if (p == s_nr.gouraud_pipe || p == s_nr.gouraud_mark_pipe ||
        p == s_nr.scene_tex_pipe || p == s_nr.scene_tex_mark_pipe ||
        p == s_nr.hd_cutout_pipe || p == s_nr.hd_cutout_mark_pipe ||
        p == s_nr.gouraud_blend_pipe || p == s_nr.gouraud_add_pipe ||
        p == s_nr.scene_tex_blend_pipe || p == s_nr.scene_tex_add_pipe ||
        p == s_nr.gouraud_punch_pipe || p == s_nr.scene_tex_punch_pipe ||
        p == s_nr.hd_cutout_punch_pipe) {
        rhi_bind_depth_sampler(s_nr.dev, s_nr.actor_key_depth, 1);
        rhi_bind_depth_sampler(s_nr.dev, s_nr.terrain_key_depth, 2);
        rhi_bind_depth_sampler(s_nr.dev, s_nr.moby_key_depth, 3);
        if (s_nr.moby_rank_rt)
            rhi_bind_texture_set(s_nr.dev, s_nr.moby_rank_rt, 4);
    }
}

/* The same four images for a MATERIAL pipeline, whose layout puts them at
 * sets 2 and 4..6 — above the UBO pair rather than replacing sets 1/2, so a
 * material's own uniform block keeps set 3 binding 1. The engine half of
 * openpete_psx_material.glsl runs the contest from these before the mod's
 * op_material() sees a colour; without them a material would silently
 * delete og's painter verdicts, which is what it used to do. */
static void bind_material_keys(void)
{
    if (!s_nr.terrain_key_depth || !s_nr.moby_key_depth || !s_nr.actor_key_depth) return;
    rhi_bind_depth_sampler(s_nr.dev, s_nr.terrain_key_depth, 2);
    rhi_bind_depth_sampler(s_nr.dev, s_nr.moby_key_depth, 4);
    rhi_bind_depth_sampler(s_nr.dev, s_nr.actor_key_depth, 5);
    if (s_nr.moby_rank_rt)
        rhi_bind_texture_set(s_nr.dev, s_nr.moby_rank_rt, 6);
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
    bind_scene_pipe(s_nr.occl_pipe);
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
    if (s_nr.ws_factor > 1.0f) return s_nr.ws_factor;
    /* Headless has no window, so update_layout never derives ws_factor from a
     * fixed --aspect. Honor the aspect here so native-only screenshot captures
     * extract the true Hor+ reveal (the edge geometry the feeders would
     * otherwise trivial-reject) rather than a stretched 4:3 frame. Windowed
     * runs always have ws_factor set by update_layout, so this never fires
     * there. */
    if (!s_nr.window && s_nr.aspect_wh > 0.0f) {
        float wf = s_nr.aspect_wh / (4.0f / 3.0f);
        return wf > 1.0f ? wf : 1.0f;
    }
    return 1.0f;
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

/* Build + draw the native 3D scene (cyclorama + actors). full_extent=false
 * draws into panel 0's window rect (the live present); full_extent=true draws
 * into the whole render target (rendering into native_rtt, where the target IS
 * the frame). Returns true if anything was drawn. */
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

/* Material sort, counting-sort form. Comparison-sorting ~15k tri indices per
 * present costs ~750us of every compose; the key space is actually a few
 * dozen distinct materials, so this interns each tri's material to a bucket
 * (one hash probe per tri), orders the UNIQUE materials with the comparator
 * semantics above (tpage_y, tpage_x, clut_y, clut_x, bpp[, tag] — the packed
 * key preserves exactly that priority), and scatters — O(n + m log m).
 * Stable within a material (submission order), which qsort never guaranteed;
 * grouping and cross-material order match tex_mat_cmp exactly. Falls back to
 * qsort if the bucket table overflows (pathological material count). */
#define TXMAT_BUCKETS 1024   /* power of two; distinct materials per scene ~dozens */
typedef struct { uint64_t key; uint32_t tag; int count, offset, used; } txmat_bucket_t;
static txmat_bucket_t s_txmat_b[TXMAT_BUCKETS];
static uint16_t       s_txmat_of[MAX_SCENE_TRIS];   /* tri -> bucket slot */
static int            s_txmat_out[MAX_SCENE_TRIS];  /* scatter target */

static int txmat_bucket_cmp(const void* pa, const void* pb) {
    const txmat_bucket_t* a = &s_txmat_b[*(const uint16_t*)pa];
    const txmat_bucket_t* b = &s_txmat_b[*(const uint16_t*)pb];
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    if (a->tag != b->tag) return a->tag < b->tag ? -1 : 1;
    return 0;
}

static void tex_mat_sort(const scene_tri_t* scene, int* idx, int n)
{
    if (n <= 1) return;
    const int with_tag = tag_split_on();
    static uint16_t slots[TXMAT_BUCKETS];   /* used slot list */
    int m = 0;
    for (int i = 0; i < n; i++) {
        const scene_tri_t* t = &scene[idx[i]];
        /* Field priority == tex_mat_cmp: tpage_y | tpage_x | clut_y | clut_x
         * | bpp, high to low. Widths cover VRAM extents (x<=1024, y<=512). */
        uint64_t key = ((uint64_t)(uint32_t)t->tpage_y << 43) |
                       ((uint64_t)(uint32_t)t->tpage_x << 32) |
                       ((uint64_t)(uint32_t)t->clut_y  << 22) |
                       ((uint64_t)(uint32_t)t->clut_x  << 11) |
                        (uint64_t)(uint32_t)t->bpp;
        uint32_t tag = with_tag ? t->tag : 0;
        uint64_t h = (key ^ ((uint64_t)tag * 0x9E3779B97F4A7C15ull)) *
                     0x9E3779B97F4A7C15ull >> 48;
        int s = (int)(h & (TXMAT_BUCKETS - 1));
        for (;;) {
            txmat_bucket_t* b = &s_txmat_b[s];
            if (b->used && b->key == key && b->tag == tag) break;
            if (!b->used) {
                if (m >= TXMAT_BUCKETS - 1) {   /* table full: comparison sort */
                    for (int j = 0; j < m; j++) s_txmat_b[slots[j]].used = 0;
                    s_txsort_scene = scene;
                    qsort(idx, (size_t)n, sizeof(int), tex_mat_cmp);
                    return;
                }
                b->used = 1; b->key = key; b->tag = tag; b->count = 0;
                slots[m++] = (uint16_t)s;
                break;
            }
            s = (s + 1) & (TXMAT_BUCKETS - 1);
        }
        s_txmat_of[i] = (uint16_t)s;
        s_txmat_b[s].count++;
    }
    qsort(slots, (size_t)m, sizeof(uint16_t), txmat_bucket_cmp);
    int off = 0;
    for (int j = 0; j < m; j++) {
        s_txmat_b[slots[j]].offset = off;
        off += s_txmat_b[slots[j]].count;
    }
    for (int i = 0; i < n; i++)
        s_txmat_out[s_txmat_b[s_txmat_of[i]].offset++] = idx[i];
    memcpy(idx, s_txmat_out, (size_t)n * sizeof(int));
    for (int j = 0; j < m; j++) s_txmat_b[slots[j]].used = 0;
}
/* ---- HD texture replacement ---------------------------------------------
 * Identity comes from the upload-time registry (texture_pack_resolve, per
 * TRI — run-union boxes vary with per-frame clipping); the pack match is
 * containment-based (texture_pack_match), so one tile-sized file covers LP
 * full-tile draws, HP sub-quad draws, and partially-clipped boxes alike.
 * (The original hardcoded title-logo PoC path is retired — the generic path
 * replaced it 1:1.) */

/* ---- HD replacement GPU-texture cache ------------------------------------
 * identity hashkey -> uploaded rhi texture (linear-filtered). Slot collision
 * keeps the incumbent and falls back to SD for the newcomer (destroying an
 * in-flight texture is not frame-safe; with 512 slots and pack sizes in the
 * dozens this is theoretical). */
#define HD_CACHE_SLOTS 8192   /* pack sizes reach thousands game-wide; probe-16 needs headroom */
static struct { uint64_t key; rhi_texture *tex; } s_hd_cache[HD_CACHE_SLOTS];

/* GPU uploads cannot record inside the offscreen render pass, so a first
 * sighting at draw time only QUEUES the image; hd_flush_pending() uploads at
 * the next frame start (one SD frame on first appearance). */
/* box_w/box_h: the texel box this image replaces. An image that is an exact
 * integer multiple of it samples NEAREST — block edges then coincide with
 * texel edges, so a dump fed straight back in renders bit-identically.
 * Anything else (arbitrary-resolution HD art) keeps LINEAR smoothing. */
static struct { uint64_t key; const unsigned char *px; int w, h, box_w, box_h; } s_hd_pending[512];
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

static void hd_pending_add(uint64_t key, const unsigned char *px, int w, int h,
                           int box_w, int box_h)
{
    if (s_hd_pending_n >= 512) return;
    for (int i = 0; i < s_hd_pending_n; i++)
        if (s_hd_pending[i].key == key) return;
    s_hd_pending[s_hd_pending_n++] =
        (__typeof__(s_hd_pending[0])){ key, px, w, h, box_w, box_h };
}

/* HD replacement textures created per frame. Each entry is its own image:
 * vkCreateImage plus a device allocation and a view, which is driver-side
 * work that shows up in neither a GPU zone nor the create call's own return.
 * A pack-covered level entered with the pack resident queues its whole
 * visible set at once — ~310 images, ~85 ms in one frame, past the pacer's
 * slip clamp and so a real loss of wall-clock from the game clock. Creating
 * a bounded number per frame spreads that over a handful of frames; entries
 * still pending resolve to stock for that frame, which is the same outcome
 * the cache-full path already produces.
 *
 * 12 is measured, not guessed: the burst's worst frame falls 85.8 -> 10.4 ms
 * at this cap (24 -> 15.1, 8 -> 9.1, 4 -> 7.4), and every value tested clears
 * the slip clamp. Returns flatten below 12 because ~7 ms of the burst is a
 * fixed cost no cap divides, while a lower cap only lengthens the run of
 * frames drawing stock. */
/* How long the window size must hold still before a resize is adopted. Long
 * enough to span a drag's per-pixel steps, short enough to feel immediate. */
#define NR_RESIZE_SETTLE_NS (150ull * 1000ull * 1000ull)

#define HD_CREATE_PER_FRAME 12

static void hd_flush_pending(void)   /* call OUTSIDE any render pass */
{
    static int cap = -1;
    if (cap < 0) {
        cap = HD_CREATE_PER_FRAME;
        const char *e = getenv("OPENPETE_HD_CREATE_CAP");
        if (e && *e) { int v = atoi(e); if (v > 0) cap = v; }
    }
    int made = 0;
    int i = 0;
    for (; i < s_hd_pending_n; i++) {
        if (made >= cap) break;
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
        /* Filter choice. An image that is an exact integer multiple of the
         * texel box it replaces samples NEAREST: its blocks align with texel
         * boundaries, so every screen pixel resolves to the texel the stock
         * VRAM sampler would have fetched and a dump dropped straight back in
         * renders bit-identically (measured: 0 differing pixels at 1x and at
         * 7x/12x). Everything else — arbitrary-resolution HD art, mod-
         * registered textures — keeps LINEAR, which is what it has always had.
         * Linear cannot reproduce stock at ANY scale: its error comes from
         * sample points straddling block edges, and that fraction is scale-
         * invariant (7x/12x and 14x/24x measured pixel-for-pixel identical). */
        {
            int bw = s_hd_pending[i].box_w, bh = s_hd_pending[i].box_h;
            int w  = s_hd_pending[i].w,     h  = s_hd_pending[i].h;
            int exact = bw > 0 && bh > 0 &&
                        w % bw == 0 && h % bh == 0 && w / bw > 0 && h / bh > 0;
            if (!exact) rhi_texture_set_filter_linear(s_nr.dev, t);
        }
        s_hd_cache[idx].key = key;
        s_hd_cache[idx].tex = t;
        made++;
    }
    /* Keep whatever the budget did not reach for the next frame. */
    if (i < s_hd_pending_n) {
        s_hd_pending_n -= i;
        memmove(s_hd_pending, s_hd_pending + i,
                (size_t)s_hd_pending_n * sizeof(s_hd_pending[0]));
    } else {
        s_hd_pending_n = 0;
    }
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
/* Per-axis texel replication that writes a dumped texture at square pixels.
 *
 * Screen-space 2D draws (the HUD categories) map 1 texel to 1 framebuffer
 * pixel. Spyro's framebuffer is 512 wide with clip_h visible lines, shown at
 * 4:3, so a texel is 4*clip_h : 3*disp_w as wide as it is tall — 7:12 at
 * 512x224, i.e. ~1.71x taller than wide. Their VRAM data is horizontally
 * pre-squashed to land on that grid, which is why a raw dump reads stretched
 * in an image viewer.
 *
 * The ratio is reduced to lowest terms and applied as INTEGER factors rather
 * than a rounded stretch: only then do the replicated blocks stay aligned to
 * texel boundaries, which is what lets a dump be fed straight back in as a
 * replacement and render bit-identically (measured: 7x/12x exact, a 1.71x
 * float stretch 3.6% off). A ratio that will not reduce to small factors is
 * dropped rather than emitting an enormous image.
 *
 * World geometry is projected through a matrix that already carries the same
 * aspect, so its textures are authored square and must be dumped untouched. */
static void tp_dump_scale(int category, int *sx, int *sy)
{
    *sx = *sy = 1;
    if (category != OPENPETE_SCENE_HUD && category != OPENPETE_SCENE_OVERLAY)
        return;
    int disp_w = s_nr.disp_w > 0 ? s_nr.disp_w : 512;
    int clip_h = s_nr.clip_h > 0 ? s_nr.clip_h : 224;
    int num = 4 * clip_h, den = 3 * disp_w;     /* texel width : texel height */
    if (num <= 0 || den <= 0 || num >= den) return;   /* already square or wider */
    int a = num, b = den;
    while (b) { int t = a % b; a = b; b = t; }  /* gcd */
    if (a <= 0) return;
    num /= a; den /= a;
    if (num > 64 || den > 64) return;           /* refuses to reduce sanely */
    *sx = num; *sy = den;
}

/* Terrain fog for a REPLACED texture.
 *
 * Unreplaced terrain fogs for free: the material simply points at a greyer,
 * pre-baked palette row further down the CLUT (base_y + delta). A replacement
 * has no palette — it is host RGBA — so that mechanism cannot reach it, and a
 * replaced tile would otherwise render at full saturation while the terrain
 * around it fades into the distance.
 *
 * The two palette rows ARE the fog specification for this texture, so derive
 * the transform from them instead of assuming a global fog colour: fit
 * out = a*in + b per channel by least squares over the palette entries. The
 * ladder turns out to be very close to affine (measured over 390 ladder steps
 * on 26 base CLUTs: mean error 2.7/255, p95 3.3, versus 34.0 for applying no
 * fog at all). Fitting per channel rather than as a blend-toward-one-colour
 * matters — the blend rate varies widely between textures (t at delta 15
 * measured 0.825 +- 0.326), so a single global fog constant would be wrong
 * for most of them.
 *
 * Entries that are the 0x0000 transparency sentinel in the base row carry no
 * colour and are excluded. A base row with no spread gives the slope no
 * signal, so fall back to a pure offset, which preserves the replacement's
 * own detail rather than flattening it to a constant. */
typedef struct { float a[3], b[3]; } tp_fog_xform;

static void tp_fog_affine(int clut_x, int base_y, int delta, int bpp,
                          tp_fog_xform *out)
{
    for (int c = 0; c < 3; c++) { out->a[c] = 1.0f; out->b[c] = 0.0f; }
    const uint16_t *vr = openpete_vram_data();
    if (!vr || delta <= 0 || delta > TP_FOG_DELTA_MAX) return;
    int fog_y = (base_y + delta) & 0x1FF;
    if (clut_x < 0 || base_y < 0 || base_y > 511) return;
    int n = (bpp == 8) ? 256 : 16;
    if (clut_x + n > OPENPETE_VRAM_W) return;
    const uint16_t *src = vr + (size_t)base_y * OPENPETE_VRAM_W + clut_x;
    const uint16_t *dst = vr + (size_t)fog_y  * OPENPETE_VRAM_W + clut_x;
    for (int c = 0; c < 3; c++) {
        int shift = c * 5;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int cnt = 0;
        for (int e = 0; e < n; e++) {
            if (src[e] == 0) continue;                 /* transparency sentinel */
            double x = (double)((src[e] >> shift) & 0x1F);
            double y = (double)((dst[e] >> shift) & 0x1F);
            sx += x; sy += y; sxx += x * x; sxy += x * y; cnt++;
        }
        if (cnt == 0) continue;
        double den = cnt * sxx - sx * sx;
        if (den > 1e-6) {
            double a = (cnt * sxy - sx * sy) / den;
            out->a[c] = (float)a;
            out->b[c] = (float)((sy - a * sx) / cnt / 31.0);
        } else {
            out->b[c] = (float)((sy - sx) / cnt / 31.0);   /* offset only */
        }
    }
}

/* Deferred untextured material spans (S8c). The untextured buckets are one
 * bulk gouraud draw each; a tri whose provenance tag resolves to a registered
 * material (an untextured moby with a per-instance slot, or the terrain
 * beyond-LOD gouraud band) is deferred during the bucket fill, appended
 * AFTER the plain span grouped by resolve-relevant tag, and drawn with the
 * mod pipeline (psx_scene stages, push-constant bpp 0 = untextured). Inert
 * without an armed selector (tag_split_on). */
#define UT_MAT_SPANS 96
/* The untextured buckets past the numbered groups (0..5): SKY draws before
 * the group loop, WORLD_FILL right after the WORLD group. */
#define UT_BUCKET_SKY 11
static struct { int gi; uint32_t tag; int first, count; } s_utmat[UT_MAT_SPANS];
static int s_utmat_n;
static int s_utdef_idx[MAX_SCENE_TRIS];

/* Resolve-relevant tag bits: terrain resolve ignores the sector id, so mask
 * it out — one span (and one memo entry) per material outcome, not per
 * sector. Moby tags resolve on all bits. */
static uint32_t ut_mask_tag(uint32_t tag) {
    return (tag >> 28) == 2u ? (tag & 0xFFFF0000u) : tag;
}

static const scene_tri_t* s_utsort_scene;
static int utdef_cmp(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    uint32_t tx = ut_mask_tag(s_utsort_scene[x].tag);
    uint32_t ty = ut_mask_tag(s_utsort_scene[y].tag);
    if (tx != ty) return tx < ty ? -1 : 1;
    return x - y;                                   /* stable within a tag */
}

/* Per-flush memo of "does this (masked) tag resolve to a material". */
static uint32_t s_utmemo_tag[32]; static int8_t s_utmemo_hit[32]; static int s_utmemo_n;
static int ut_tag_resolves(uint32_t tag) {
    uint32_t mt = ut_mask_tag(tag);
    for (int i = 0; i < s_utmemo_n; i++)
        if (s_utmemo_tag[i] == mt) return s_utmemo_hit[i];
    openpete_mod_material_gpu_t mg;
    int hit = openpete_mods_material_resolve(mt, &mg) >= 0;
    if (s_utmemo_n < 32) { s_utmemo_tag[s_utmemo_n] = mt; s_utmemo_hit[s_utmemo_n++] = (int8_t)hit; }
    return hit;
}

/* Emit `dn` deferred tris (scene indices in s_utdef_idx) as contiguous
 * tag-grouped spans for bucket `gi`, starting at vertex `n`. Returns the new
 * vertex count.
 *
 * ONE implementation, because there are two buckets that defer (the numbered
 * groups and SKY) and a third divergence between them is how this file grows
 * holes: a deferred tri is already out of the plain span, so anything that
 * fails to record it deletes it.
 *
 * That is also why the span table reserves a slot. The old "overflow → stock"
 * degradation zeroed the last span's TAG without extending its COUNT, so an
 * overflowing span's tris were written to the buffer and referenced by
 * nothing. Here the open span simply stops splitting once the table is nearly
 * full and absorbs the rest of the bucket at tag 0 — the whole tail draws
 * stock, which is what the degradation always meant to say.
 *
 * The reservation only makes ONE bucket fit, so the callers must also refuse
 * to DEFER once the table is full (see defer_ok) — otherwise the next bucket
 * hands tris here that no span can record. Measured: with the table shrunk to
 * 2 slots, that gap deleted 34370 px. Both halves are needed. */
static int ut_emit_deferred(int gi, const scene_tri_t* scene, rhi_vertex* buf,
                            int n, int dn)
{
    if (dn <= 0) return n;
    s_utsort_scene = scene;
    qsort(s_utdef_idx, (size_t)dn, sizeof(int), utdef_cmp);
    uint32_t cur = 0; int span_first = 0, open = 0;
    for (int d = 0; d < dn; d++) {
        int t = s_utdef_idx[d];
        if (n + 3 > SCENE_VB_VERTS) break;
        uint32_t mt = ut_mask_tag(scene[t].tag);
        if (!open) {
            cur = mt; span_first = n; open = 1;
        } else if (mt != cur) {
            if (s_utmat_n + 1 < UT_MAT_SPANS) {     /* keep one slot for the tail */
                s_utmat[s_utmat_n++] = (__typeof__(s_utmat[0]))
                    { gi, cur, span_first, n - span_first };
                cur = mt; span_first = n;
            } else {
                cur = 0;    /* absorbed: this span's tail draws stock */
            }
        }
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex* o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
            o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
            o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
        }
        n += 3;
    }
    if (open && s_utmat_n < UT_MAT_SPANS)
        s_utmat[s_utmat_n++] = (__typeof__(s_utmat[0]))
            { gi, cur, span_first, n - span_first };
    return n;
}

/* Does this untextured bucket DRAW its deferred material spans?
 *
 * Deferring a tri REMOVES it from the plain span, so a bucket that defers
 * and then does not draw the spans deletes the geometry outright — the
 * Spyro shadow vanished this way, because gi==1's untextured draw is the
 * depth-aware silhouette decal, which takes no material and skipped them.
 * One predicate answers both the deferral and the draw so the two cannot
 * disagree again.
 *
 * gi==1 (untextured SUBTRACT) is Spyro's shadow decal: its fragment stage
 * owns per-pixel occlusion, which a plain material would delete, and its
 * vertex interface is its own — so a material there needs a profile it does
 * not have yet, not a wider override path. Everything else takes spans:
 * the opaque WORLD/ACTOR/PLAYER buckets, every BLENDED bucket (flame,
 * particles, tracers — a channel that only ever emits blended geometry
 * would otherwise be unreachable by any material), and SKY, whose dome-wide
 * selector is one of the plainest things a mod can ask for. Blended spans
 * reshade only: draw_ut_mat_spans inherits the group's blend and
 * depth-write policy, so painter order is untouched.
 *
 * SKY's stock span and its material spans are drawn together under one
 * `sky_ok`, so they are already suppressed as a pair — but that was
 * ADJACENCY, not structure, and adjacency is what this predicate exists to
 * replace. Keep them paired: anything that draws ut_count[gi] owes
 * draw_ut_mat_spans(gi) under the same condition. */
static int ut_mat_spans_ok(int gi, int gb, int gc) {
    if (gi == 1) return 0;
    if (gi == UT_BUCKET_SKY) return 1;
    return gb != 0 || gc == OPENPETE_SCENE_WORLD ||
           gc == OPENPETE_SCENE_ACTOR || gc == OPENPETE_SCENE_PLAYER;
}

/* Draw one bucket's deferred untextured material spans (S8c). Builds the
 * material's pipeline on first use — psx_scene stages as stock fallbacks
 * (their vertex interface, vUV loc 0 / vColor loc 1, is the mod-material
 * frag contract for textured AND untextured draws), with a stencil-MARK
 * twin for the hole-fill WORLD/ACTOR groups so overridden coverage still
 * masks the LP fill. A span that fails to resolve or build draws with the
 * stock pipe — never invisible. */
static void draw_ut_mat_spans(int gi, rhi_pipeline* stock, int want_mark,
                              int blend_class) {
    /* Reshade-only: a material replaces the group's COLOUR MATH, never how
     * the group composites, so the pipeline it builds inherits this group's
     * blend. Anything else would let a mod silently reorder or re-composite
     * geometry the painter order depends on. */
    static const rhi_blend_mode k_blend[4] = {
        RHI_BLEND_NONE, RHI_BLEND_ALPHA, RHI_BLEND_ADD, RHI_BLEND_SUBTRACT
    };
    if (blend_class < 0 || blend_class > 3) blend_class = 0;
    int bound = 0;
    for (int si = 0; si < s_utmat_n; si++) {
        if (s_utmat[si].gi != gi || s_utmat[si].count <= 0) continue;
        if (!bound) { rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb); bound = 1; }
        rhi_pipeline* pipe = NULL;
        openpete_mod_material_gpu_t mg;
        if (s_utmat[si].tag && openpete_mods_material_resolve(s_utmat[si].tag, &mg) >= 0) {
            void** slot = want_mark ? mg.pipeline_mark_slot
                                    : mg.pipeline_slot[blend_class];
            if (*slot == NULL) {
                rhi_pipeline_desc md = {
                    .vert_spv = "psx_scene.vert.spv",
                    .frag_spv = "psx_scene.frag.spv",
                    .blend = k_blend[blend_class],
                    /* Blended groups are painter-ordered and depth-write off;
                     * inherit that too rather than punching the depth buffer. */
                    .depth_write_off = blend_class != 0,
                    .depth_test = true,
                    .stencil = want_mark ? RHI_STENCIL_MARK : RHI_STENCIL_NONE,
                    .material_pipeline = true,
                    .push_const_size = sizeof(psx_material_hd),
                };
                rhi_pipeline* np = rhi_create_pipeline_spv(
                    s_nr.dev, &md, mg.vert_spv, mg.vert_words,
                    mg.frag_spv, mg.frag_words);
                *slot = np ? (void*)np : PFX_PIPE_FAILED;
                if (!np)
                    fprintf(stderr, "[native] material: untextured pipeline "
                                    "build FAILED for %s\n", mg.owner);
            }
            if (*slot != PFX_PIPE_FAILED) {
                pipe = (rhi_pipeline*)*slot;
                bind_scene_pipe(pipe);
                rhi_bind_texture_set(s_nr.dev, s_nr.vram_tex, 0);
                if (s_nr.mat_pack_dummy)   /* op_pack_tex: no pack art for gouraud */
                    rhi_bind_texture_set(s_nr.dev, s_nr.mat_pack_dummy, 1);
                bind_material_keys();
                if (s_mat_ubo)
                    rhi_ubo_bind(s_nr.dev, s_mat_ubo, 0,
                                 mg.uniform_slot > 0 ? 1 + mg.uniform_slot : 0);
                psx_material_hd zm;                     /* bpp 0 = untextured */
                memset(&zm, 0, sizeof zm);
                rhi_push_constants(s_nr.dev, &zm, sizeof zm);
            }
        }
        if (!pipe) bind_scene_pipe(stock);
        rhi_draw(s_nr.dev, (uint32_t)s_utmat[si].count, (uint32_t)s_utmat[si].first);
    }
}

/* Deferred HD sub-runs (opaque depth-tested groups). Emitting each matched
 * sub-run inline costs two pipeline binds + two texture binds (draw, then
 * restore self/vram) PER SUB-RUN — a full pack under --lod extended produces
 * thousands of sub-runs per present and the rebind storm dominates frame
 * time. Depth testing makes opaque draw order irrelevant, so the cutout
 * groups batch instead: collect sub-runs during the walk, then draw them
 * sorted by texture — one pipeline bind for the whole batch, one texture
 * bind per file, and the SD walk never leaves `self`. Blend/HUD HD draws
 * stay inline: their order is meaningful and they are few. */
typedef struct { rhi_texture *tex; uint32_t count, first; hd_pc pc; } hd_defer_ent;
static hd_defer_ent *s_hd_defer;
static int s_hd_defer_n, s_hd_defer_cap;

static int hd_defer_cmp(const void *a, const void *b)
{
    const hd_defer_ent *x = a, *y = b;
    if (x->tex != y->tex) return x->tex < y->tex ? -1 : 1;
    return x->first < y->first ? -1 : (x->first > y->first);   /* deterministic */
}

static void hd_defer_push(rhi_texture *tex, const hd_pc *pc,
                          uint32_t count, uint32_t first)
{
    if (s_hd_defer_n == s_hd_defer_cap) {
        int cap = s_hd_defer_cap ? s_hd_defer_cap * 2 : 1024;
        void *p = realloc(s_hd_defer, (size_t)cap * sizeof *s_hd_defer);
        if (!p) return;                        /* sub-run drops for this present */
        s_hd_defer = p; s_hd_defer_cap = cap;
    }
    hd_defer_ent *e = &s_hd_defer[s_hd_defer_n++];
    e->tex = tex; e->pc = *pc; e->count = count; e->first = first;
}

static void hd_defer_flush(rhi_pipeline *hd_pipe, rhi_pipeline *self)
{
    if (!s_hd_defer_n) return;
    qsort(s_hd_defer, (size_t)s_hd_defer_n, sizeof *s_hd_defer, hd_defer_cmp);
    bind_scene_pipe(hd_pipe);
    rhi_texture *cur = NULL;
    for (int i = 0; i < s_hd_defer_n; i++) {
        hd_defer_ent *e = &s_hd_defer[i];
        if (e->tex != cur) { rhi_bind_vram(s_nr.dev, e->tex); cur = e->tex; }
        rhi_push_constants(s_nr.dev, &e->pc, sizeof e->pc);
        rhi_draw(s_nr.dev, e->count, e->first);
    }
    s_hd_defer_n = 0;
    bind_scene_pipe(self);
    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
}

/* Texpack instrument gates, shared by the SD walk and the material-override
 * walk (lazy env reads). */
static int s_tp_resolve_log = -1, s_tp_dump_on, s_tp_census_on;
static void tp_flags_init(void)
{
    if (s_tp_resolve_log >= 0) return;
    s_tp_resolve_log = getenv("OPENPETE_TEXPACK_RESOLVE_LOG") ? 1 : 0;
    s_tp_dump_on     = getenv("OPENPETE_TEXPACK_DUMP_DIR") ? 1 : 0;
    s_tp_census_on   = getenv("OPENPETE_TEXPACK_CATCENSUS") ? 1 : 0;
}

/* Per-tri texture-pack disposition: the tri's texel box -> registry identity
 * (with the census / dump / resolve-log instrument side-effects) -> pack-file
 * match. Returns 1 when a replacement file matched (id/repl filled). cly_out
 * gets the BASE ladder row — the forward fog shift decodes as
 * (base + delta) & 0x1FF, so the inverse masks identically (an unmasked
 * subtract goes negative on a wrapped row and indexes VRAM out of bounds).
 * u0/v0_out anchor the tri's page-relative box origin (the file-rect
 * conversion below needs it). */
static int tp_tri_match(const scene_tri_t* st, const psx_material* m,
                        int want_replace, texture_pack_id* id,
                        texture_pack_repl* repl, int* cly_out,
                        float* u0_out, float* v0_out)
{
    float u0 = st->v[0].u, v0 = st->v[0].v, u1 = u0, v1 = v0;
    for (int j = 1; j < 3; j++) {
        if (st->v[j].u < u0) u0 = st->v[j].u;
        if (st->v[j].u > u1) u1 = st->v[j].u;
        if (st->v[j].v < v0) v0 = st->v[j].v;
        if (st->v[j].v > v1) v1 = st->v[j].v;
    }
    const int cly = (m->clut_y - (st->fog_delta & TP_FOG_DELTA_RUNG)) & 0x1FF;
    *cly_out = cly; *u0_out = u0; *v0_out = v0;
    int hit = texture_pack_resolve(m->tpage_x, m->tpage_y, m->clut_x, cly,
                                   m->bpp, (int)u0, (int)v0, (int)u1, (int)v1, id);
    if (hit) texture_pack_cat_census(id, st->category, st->tag);
    if (hit && s_tp_dump_on) {
        int dsx, dsy;
        tp_dump_scale(st->category, &dsx, &dsy);
        texture_pack_dump_draw(id, m->tpage_x, m->tpage_y, m->clut_x, cly,
                               (int)u0, (int)v0, st->category, dsx, dsy);
    }
    static int s_logged = 0;
    if (s_tp_resolve_log && s_logged < 256) {
        s_logged++;
        fprintf(stderr, "[texpack] resolve tp=%d,%d clut=%d,%d bpp=%d uv=%d,%d-%d,%d -> %s\n",
                m->tpage_x, m->tpage_y, m->clut_x, m->clut_y, m->bpp,
                (int)u0, (int)v0, (int)u1, (int)v1, hit ? "hit" : "MISS");
    }
    return hit && want_replace && texture_pack_match(id, repl);
}

/* ---- per-texture pack retarget (stock -> pack affine fit) ------------------
 * The ladder affine maps STOCK base texels to STOCK fogged texels; applying
 * it to pack art fogs toward the STOCK ambience. The global fog shift (one
 * affine for the whole pack) retargets that only on average — a pack whose
 * grade varies by region lands visibly wrong per texture. The exact per-
 * texture correction follows from modelling the pack as an affine of stock,
 * p ~= g(x) = P*x + Q (texture_pack_grade_fit): the pack-side rung transform
 * is g(a*g^-1(p) + b) = a*p + (P*b + (1-a)*Q) — same slope, offset
 * re-derived from the texture's own art. Falls back to the global shift
 * when the fit has no signal (too few opaque pairs). */
static int tp_pack_fit(const psx_material* m, int cly,
                       const texture_pack_id* id, const texture_pack_repl* repl,
                       float pu0, float pv0, float p[3], float q[3])
{
    static int s_fit_en = -1;   /* OPENPETE_TEXPACK_FOG_FIT=0: global shift only (A/B) */
    if (s_fit_en < 0) { const char* e = getenv("OPENPETE_TEXPACK_FOG_FIT");
                        s_fit_en = !(e && *e == '0'); }
    if (!s_fit_en) return 0;
    return texture_pack_grade_fit(id, repl, m->tpage_x, m->tpage_y,
                                  m->clut_x, cly, m->bpp,
                                  (int)pu0, (int)pv0, p, q);
}

/* Fog transforms for a replaced draw. Flat model (pc->a/b): the affine of
 * the face's own ladder rung. Continuous model (pc->aN/bN/lad, HD pipes
 * only): the deepest-row affine plus the per-fragment ramp, enabled when the
 * feeder marked the face ladder-eligible (TP_FOG_DELTA_CONT) and published
 * the frame's ramp. Both offsets are retargeted per texture via tp_pack_fit
 * (global fog shift as fallback). pu0/pv0 anchor the FILE box in page texel
 * coords — the same values the shader rect uses. */
static void tp_pack_fog(const psx_material* m, int cly, int fog_delta,
                        const texture_pack_id* id, const texture_pack_repl* repl,
                        float pu0, float pv0, hd_pc* pc)
{
    static int s_cont_en = -1;  /* OPENPETE_TEXPACK_FOG_CONT=0: per-face rungs (A/B) */
    if (s_cont_en < 0) { const char* e = getenv("OPENPETE_TEXPACK_FOG_CONT");
                         s_cont_en = !(e && *e == '0'); }
    pc->a[0] = pc->a[1] = pc->a[2] = 1.f; pc->a[3] = 0.f;
    pc->b[0] = pc->b[1] = pc->b[2] = pc->b[3] = 0.f;
    memset(pc->aN, 0, sizeof pc->aN);
    memset(pc->bN, 0, sizeof pc->bN);
    memset(pc->lad, 0, sizeof pc->lad);

    float fp[3], fq[3];
    int   fit = tp_pack_fit(m, cly, id, repl, pu0, pv0, fp, fq);
    float fs[3], fo[3];
    int   shift = fit ? 0 : texture_pack_fog_shift_get(fs, fo);

    int rung = fog_delta & TP_FOG_DELTA_RUNG;
    if (rung > 0) {
        tp_fog_xform fx;
        tp_fog_affine(m->clut_x, cly, rung, m->bpp, &fx);
        for (int c = 0; c < 3; c++) {
            pc->a[c] = fx.a[c];
            pc->b[c] = fit   ? fp[c] * fx.b[c] + (1.0f - fx.a[c]) * fq[c]
                     : shift ? fs[c] * fx.b[c] + (1.0f - fx.a[c]) * (fo[c] / 255.0f)
                     :         fx.b[c];
        }
    }

    float ts, tb;
    if (s_cont_en && (fog_delta & TP_FOG_DELTA_CONT) &&
        texture_pack_fog_ladder_get(&ts, &tb)) {
        tp_fog_xform fN;
        tp_fog_affine(m->clut_x, cly, TP_FOG_DELTA_MAX, m->bpp, &fN);
        for (int c = 0; c < 3; c++) {
            pc->aN[c] = fN.a[c];
            pc->bN[c] = fit   ? fp[c] * fN.b[c] + (1.0f - fN.a[c]) * fq[c]
                      : shift ? fs[c] * fN.b[c] + (1.0f - fN.a[c]) * (fo[c] / 255.0f)
                      :         fN.b[c];
        }
        pc->aN[3] = 1.0f;
        pc->lad[0] = ts;
        /* The face's fog reference may carry the authored +0x2000 bias —
         * shift the ramp by the equivalent 8192/3840 in t. */
        pc->lad[1] = tb - ((fog_delta & TP_FOG_DELTA_FOGBIAS) ? 8192.0f / 3840.0f : 0.0f);
    }
}

/* ---- HD texture-replacement fold: pack art in the RETAINED terrain path.
 * The sub-run model above resolves per tri at draw time and rebinds per
 * file; the retained sector-mesh cache instead interns one ENTRY per
 * (file, material) at bake time — file pixels packed into one shared
 * atlas, the fog transform tp_pack_fog computes per sub-run baked per
 * entry — and the terrain fragment stage samples the atlas by the entry
 * id carried in the vertex stream. Entries ride the terrain globals UBO
 * (4 vec4 each: page rect / atlas rect / fogA / fogB). */
#define HDFOLD_ATLAS_DIM 2048
/* Replacement rects uploaded into the atlas per flush. */
#define HDFOLD_UPLOAD_PER_FRAME 96

typedef struct {
    const unsigned char *pixels;             /* dedup identity (file) */
    int tpx, tpy, clx, cly, bpp;             /* dedup identity (material) */
    float payload[16];                       /* the 4 UBO vec4s */
} hdfold_entry;
static hdfold_entry s_hdf_ent[OPENPETE_HDFOLD_MAX];
static float        s_hdf_tab[OPENPETE_HDFOLD_MAX][16];
static int          s_hdf_n = 0;
static rhi_texture *s_hdf_atlas = NULL;
static int   s_hdf_shelf_x, s_hdf_shelf_y, s_hdf_shelf_h;   /* shelf packer */
static rhi_tex_rect_src s_hdf_pending[512];   /* a full pack's load burst */
static int   s_hdf_pending_n = 0;
/* Bumped on every intern and every flush that drains the queue: sectors
 * claimed for fold overflow re-bake when it moves (tgpu_ensure). */
static uint32_t s_hdf_epoch = 1;

int openpete_native_hdfold_on(void)
{
    static int env = -1;
    if (env < 0) {
        const char *e = getenv("OPENPETE_TERRAIN_HDFOLD");
        int on = !(e && *e == '0');           /* default ON; =0 kill switch */
        /* The fold bakes the shipped pack-fog models; the A/B levers that
         * change the draw-time math disable it (tier-1 claims instead). */
        if (on) {
            const char *c = getenv("OPENPETE_TEXPACK_FOG_CONT");
            const char *f = getenv("OPENPETE_TEXPACK_FOG_FIT");
            if ((c && *c == '0') || (f && *f == '0')) on = 0;
        }
        env = on;
    }
    return env && texture_pack_replacements_active();
}

void openpete_native_hdfold_reset(void)
{
    s_hdf_n = 0;
    s_hdf_pending_n = 0;
    s_hdf_shelf_x = s_hdf_shelf_y = s_hdf_shelf_h = 0;
}


static SDL_Mutex *s_hdf_mx;
static SDL_Mutex *hdf_mx(void) { if (!s_hdf_mx) s_hdf_mx = SDL_CreateMutex(); return s_hdf_mx; }
int openpete_native_hdfold_intern(int tpx, int tpy, int clx, int cly, int bpp,
                                  const float us[3], const float vs[3])
{
    if (!openpete_native_hdfold_on()) return 0;
    float u0 = us[0], u1 = us[0], v0 = vs[0], v1 = vs[0];
    for (int k = 1; k < 3; k++) {
        if (us[k] < u0) u0 = us[k];
        if (us[k] > u1) u1 = us[k];
        if (vs[k] < v0) v0 = vs[k];
        if (vs[k] > v1) v1 = vs[k];
    }
    texture_pack_id id;
    if (!texture_pack_resolve(tpx, tpy, clx, cly, bpp,
                              (int)u0, (int)v0, (int)u1, (int)v1, &id))
        return 0;                             /* draw resolve misses too */
    texture_pack_repl repl;
    if (!texture_pack_match(&id, &repl)) return 0;
    for (int i = 0; i < s_hdf_n; i++) {       /* memoized per (file, material) */
        hdfold_entry *e = &s_hdf_ent[i];
        if (e->pixels == repl.pixels && e->tpx == tpx && e->tpy == tpy &&
            e->clx == clx && e->cly == cly && e->bpp == bpp)
            return i + 1;
    }
    if (s_hdf_n >= OPENPETE_HDFOLD_MAX) return -1;

    OP_PROF_ACCUM_SCOPE("tex.hdfold_new");   /* first use of this replacement */
    /* Shelf-pack the file image into the atlas. */
    if (repl.w > HDFOLD_ATLAS_DIM || repl.h > HDFOLD_ATLAS_DIM) return -1;
    /* 1-texel gutter between files: entry sampling clamps half a texel
     * inside its own rect, so a spacer row/column is enough to keep the
     * manual-bilinear taps from reading a neighbour. */
    if (s_hdf_shelf_x + repl.w > HDFOLD_ATLAS_DIM) {
        s_hdf_shelf_y += s_hdf_shelf_h;
        s_hdf_shelf_x = s_hdf_shelf_h = 0;
    }
    if (s_hdf_shelf_y + repl.h > HDFOLD_ATLAS_DIM) return -1;   /* atlas full */
    SDL_LockMutex(hdf_mx());
    if (s_hdf_pending_n >= (int)(sizeof(s_hdf_pending) / sizeof(s_hdf_pending[0]))) {
        SDL_UnlockMutex(s_hdf_mx);
        return -1;                            /* upload queue full: claim, retry next build */
    }
    int ax = s_hdf_shelf_x, ay = s_hdf_shelf_y;
    s_hdf_shelf_x += repl.w + 1;
    if (repl.h + 1 > s_hdf_shelf_h) s_hdf_shelf_h = repl.h + 1;
    s_hdf_pending[s_hdf_pending_n++] =
        (rhi_tex_rect_src){ repl.pixels, ax, ay, repl.w, repl.h };
    SDL_UnlockMutex(s_hdf_mx);

    /* The file's texel box anchored in page coords (the sub-run rect rule —
     * invariant across tris resolving the same identity+file). */
    float pu0 = u0 - (float)(id.box_x - repl.box_x);
    float pv0 = v0 - (float)(id.box_y - repl.box_y);

    /* Continuous-model fog affine at the deepest authored row, retargeted
     * per texture (fit) or by the global shift — tp_pack_fog's exact rule. */
    tp_fog_xform fN;
    tp_fog_affine(clx, cly, TP_FOG_DELTA_MAX, bpp, &fN);
    float fp[3], fq[3];
    int fit = texture_pack_grade_fit(&id, &repl, tpx, tpy, clx, cly, bpp,
                                     (int)pu0, (int)pv0, fp, fq);
    float fs[3], fo[3];
    int shift = fit ? 0 : texture_pack_fog_shift_get(fs, fo);

    hdfold_entry *e = &s_hdf_ent[s_hdf_n];
    e->pixels = repl.pixels;
    e->tpx = tpx; e->tpy = tpy; e->clx = clx; e->cly = cly; e->bpp = bpp;
    float *p = e->payload;
    p[0] = pu0;  p[1] = pv0;
    p[2] = (float)repl.box_w; p[3] = (float)repl.box_h;
    p[4] = (float)ax; p[5] = (float)ay;
    p[6] = (float)repl.w; p[7] = (float)repl.h;
    for (int c = 0; c < 3; c++) {
        p[8 + c]  = fN.a[c];
        p[12 + c] = fit   ? fp[c] * fN.b[c] + (1.0f - fN.a[c]) * fq[c]
                  : shift ? fs[c] * fN.b[c] + (1.0f - fN.a[c]) * (fo[c] / 255.0f)
                  :         fN.b[c];
    }
    /* Filter class (the per-file rule hd_flush_pending applies): an exact
     * integer multiple of its texel box samples NEAREST (bit-identical to
     * the stock fetch); everything else is LINEAR — the atlas sampler is
     * NEAREST, so linear entries bilinear-filter manually in the shader. */
    {
        int exact = repl.box_w > 0 && repl.box_h > 0 &&
                    repl.w % repl.box_w == 0 && repl.h % repl.box_h == 0 &&
                    repl.w / repl.box_w > 0 && repl.h / repl.box_h > 0;
        p[11] = exact ? 0.0f : 1.0f;
    }
    p[15] = 0.0f;
    memcpy(s_hdf_tab[s_hdf_n], e->payload, sizeof(e->payload));
    s_hdf_epoch++;
    return ++s_hdf_n;
}

const void *openpete_native_hdfold_table(int *count, float lad[2])
{
    if (count) *count = s_hdf_n;
    if (lad) {
        float ts, tb;
        if (texture_pack_fog_ladder_get(&ts, &tb)) { lad[0] = ts; lad[1] = tb; }
        else { lad[0] = 0.0f; lad[1] = 0.0f; }
    }
    return s_hdf_tab;
}

struct rhi_texture *openpete_native_hdfold_atlas(void)
{
    return s_hdf_atlas;
}

int openpete_native_hdfold_pending(void)
{
    return s_hdf_pending_n;
}

uint32_t openpete_native_hdfold_epoch(void)
{
    return s_hdf_epoch;
}

/* Create the atlas and force its first transition/residency now, so neither
 * lands on a frame the sim pacer is timing. Safe to call before any
 * replacement exists: the atlas contents are irrelevant until something
 * folds into it. */
void openpete_native_hdfold_prime(rhi_device *dev)
{
    if (s_hdf_atlas || !dev) return;
    s_hdf_atlas = rhi_create_rgba8(dev, HDFOLD_ATLAS_DIM, HDFOLD_ATLAS_DIM);
    if (!s_hdf_atlas) return;
    /* One 1x1 write: the upload path is what makes the image resident and
     * transitions it, and doing that here is the whole point of priming. */
    static const unsigned char px[4] = { 0, 0, 0, 0 };
    rhi_tex_rect_src seed = { px, 0, 0, 1, 1 };
    rhi_update_rgba8_rects(dev, s_hdf_atlas, &seed, 1);
    OPL_INFO_F("native", "hdfold atlas primed (%dx%d)",
               HDFOLD_ATLAS_DIM, HDFOLD_ATLAS_DIM);
}

static void hdfold_flush_locked(void);
void openpete_native_hdfold_flush(void)
{
    if (!s_hdf_pending_n || !s_nr.enabled) return;
    SDL_LockMutex(hdf_mx());
    hdfold_flush_locked();
    SDL_UnlockMutex(s_hdf_mx);
}
static void hdfold_flush_locked(void)
{
    if (!s_hdf_pending_n || !s_nr.enabled) return;
    if (!s_hdf_atlas) {
        /* NEAREST sampler (the default): exact-multiple files fetch stock-
         * identical texels; linear-class entries filter in-shader. */
        { OP_PROF_ACCUM_SCOPE("tex.atlas_create");
          s_hdf_atlas = rhi_create_rgba8(s_nr.dev, HDFOLD_ATLAS_DIM, HDFOLD_ATLAS_DIM); }
        if (!s_hdf_atlas) { s_hdf_pending_n = 0; return; }
    }
    /* One staging-slot batch per frame (96-rect upload cap); the rest waits
     * for the next flush. The screenshot compose retries its frame until
     * this queue drains, so captures never sample a half-filled atlas.
     *
     * The cap is a staging-slot bound, not a pacing lever: sweeping it 96 -> 4
     * moves the pack's first-render stall by a few ms and no more, because
     * that stall is not the rect copies. */
    const int cap = HDFOLD_UPLOAD_PER_FRAME;
    int n = s_hdf_pending_n > cap ? cap : s_hdf_pending_n;
    { OP_PROF_ACCUM_SCOPE("tex.atlas_upload");
      rhi_update_rgba8_rects(s_nr.dev, s_hdf_atlas, s_hdf_pending, n); }
    s_hdf_pending_n -= n;
    if (s_hdf_pending_n > 0)
        memmove(s_hdf_pending, s_hdf_pending + n,
                (size_t)s_hdf_pending_n * sizeof(s_hdf_pending[0]));
    else
        s_hdf_epoch++;                /* drained: fold-claimed sectors retry */
}

/* allow_override: 0 = stock only; 1 = material overrides via the plain
 * pipeline; 2 = via the stencil-MARK twin (the caller's group marks its
 * opaque coverage for hole-fill, so overridden runs must mark too — else
 * the LP fill paints over them).
 *
 * blend_class is the caller group's PSX ABR+1 class (0 opaque, 1 average,
 * 2 additive, 3 subtract). Reshade-only: an override inherits it rather
 * than choosing, so a material changes colour math and never how the run
 * composites. Blended groups are painter-ordered — the override swaps the
 * pipeline for a contiguous run in place and draws it in the same slot, so
 * submission order survives. */
static void draw_tex_runs(int first_vert, int vert_count, int tx_start,
                          const scene_tri_t* scene, rhi_pipeline* self,
                          rhi_pipeline* hd_pipe, int allow_override,
                          int blend_class) {
    static const rhi_blend_mode k_ov_blend[4] = {
        RHI_BLEND_NONE, RHI_BLEND_ALPHA, RHI_BLEND_ADD, RHI_BLEND_SUBTRACT
    };
    if (blend_class < 0 || blend_class > 3) blend_class = 0;
    const int hd_defer_ok = hd_pipe == s_nr.hd_cutout_pipe ||
                            hd_pipe == s_nr.hd_cutout_mark_pipe;
    const int ov_mark = (allow_override == 2);
    tp_flags_init();
    const int want_replace = hd_pipe && texture_pack_replacements_active();
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
        {   /* OPENPETE_TEXSEL_DBG2=<tid>: draw-side resolve forensics for one
             * terrain texture id. */
            static int s_ts2 = -2;
            if (s_ts2 == -2) { const char* e2 = getenv("OPENPETE_TEXSEL_DBG2");
                               s_ts2 = e2 ? atoi(e2) : -1; }
            extern unsigned int openpete_frame;
            if (s_ts2 >= 0 && (s0->tag >> 28) == 2u &&
                !(s0->tag & 0x02000000u) &&
                (int)((s0->tag >> 9) & 0x7Fu) == s_ts2 &&
                openpete_frame > 2000u) {
                openpete_mod_material_gpu_t mgd;
                static int pr = 0;
                if (pr++ < 24)
                    fprintf(stderr, "[texsel-draw f=%u] tag=%08X allow=%d split=%d "
                            "resolve=%d run=%d\n", openpete_frame, s0->tag,
                            allow_override, tag_split_on(),
                            openpete_mods_material_resolve(s0->tag, &mgd), run);
            }
        }
        if (allow_override && tag_split_on() && s0->tag) {
            openpete_mod_material_gpu_t mg;
            if (openpete_mods_material_resolve(s0->tag, &mg) >= 0) {
                /* One pipeline per (material, blend class): the MARK twin is
                 * opaque-only (hole-fill coverage), so a blended run never
                 * wants it. */
                void** ov_slot = ov_mark ? mg.pipeline_mark_slot
                                         : mg.pipeline_slot[blend_class];
                if (*ov_slot == NULL) {
                    rhi_pipeline_desc md = {
                        .vert_spv = "psx_scene.vert.spv",
                        .frag_spv = "psx_scene.frag.spv",
                        .blend = k_ov_blend[blend_class],
                        .depth_test = true,
                        /* Blended groups z-test against opaque but never
                         * occlude each other. No .depth_bias: binding a bias
                         * pipeline invalidates the dynamic bias state, so an
                         * override would have to re-set a value it doesn't
                         * have — and the one override-eligible subtract group
                         * (SUBTRACT-WORLD) draws at zero bias anyway. The
                         * biased group, moby shadows, takes no overrides. */
                        .depth_write_off = blend_class != 0,
                        .stencil = ov_mark ? RHI_STENCIL_MARK
                                           : RHI_STENCIL_NONE,
                        .material_pipeline = true,
                        .push_const_size = sizeof(psx_material_hd),
                    };
                    rhi_pipeline* p = rhi_create_pipeline_spv(
                        s_nr.dev, &md, mg.vert_spv, mg.vert_words,
                        mg.frag_spv, mg.frag_words);
                    *ov_slot = p ? (void*)p : PFX_PIPE_FAILED;
                    if (!p)
                        fprintf(stderr, "[native] material: pipeline build "
                                        "FAILED for %s\n", mg.owner);
                }
                if (*ov_slot != PFX_PIPE_FAILED) {
                    bind_scene_pipe((rhi_pipeline*)*ov_slot);
                    rhi_bind_texture_set(s_nr.dev, s_nr.vram_tex, 0);
                    rhi_texture* pack_bound = s_nr.mat_pack_dummy;
                    if (pack_bound)
                        rhi_bind_texture_set(s_nr.dev, pack_bound, 1);
                    bind_material_keys();
                    /* engine block 0 + the per-instance slot's mod block
                     * (S8b, block 1+slot; slot 0 = declarative binds the
                     * engine block as the mod block too — harmless). */
                    if (s_mat_ubo)
                        rhi_ubo_bind(s_nr.dev, s_mat_ubo, 0,
                                     mg.uniform_slot > 0 ? 1 + mg.uniform_slot : 0);
                    psx_material_hd mh;
                    memset(&mh, 0, sizeof mh);
                    mh.tpage_x = m.tpage_x; mh.tpage_y = m.tpage_y;
                    mh.clut_x = m.clut_x; mh.clut_y = m.clut_y;
                    mh.bpp = m.bpp;
                    /* Pack replacements for overridden runs (op_pack_shade):
                     * walk the run per tri through the same registry
                     * resolve/match as the SD path — tris matching different
                     * pack files (or none) need different set-1 bindings and
                     * file rects, so they draw as separate sub-runs. Not for
                     * bpp-32 runs (mod-registered textures have no registry
                     * identity). */
                    const int mat_walk = want_replace && s0->bpp != 32;
                    if (!mat_walk) {
                        rhi_push_constants(s_nr.dev, &mh, sizeof mh);
                        rhi_draw(s_nr.dev, (uint32_t)(run * 3),
                                 (uint32_t)(first_vert + i * 3));
                    } else {
                        int t = 0;
                        while (t < run) {
                            const scene_tri_t* st = &scene[s_txscene[base + i + t]];
                            texture_pack_id id; texture_pack_repl repl;
                            int cly; float bu0, bv0;
                            int matched = tp_tri_match(st, &m, 1, &id, &repl,
                                                       &cly, &bu0, &bv0);
                            int g = 1;
                            while (t + g < run) {
                                const scene_tri_t* sn = &scene[s_txscene[base + i + t + g]];
                                texture_pack_id nid; texture_pack_repl nrepl;
                                int ncly; float nu0, nv0;
                                int nmatched = tp_tri_match(sn, &m, 1, &nid, &nrepl,
                                                            &ncly, &nu0, &nv0);
                                if (nmatched != matched) break;
                                if (matched && nrepl.pixels != repl.pixels) break;
                                /* One fog transform per sub-run (see the SD
                                 * walk's identical break). */
                                if (matched && sn->fog_delta != st->fog_delta) break;
                                g++;
                            }
                            rhi_texture* want = s_nr.mat_pack_dummy;
                            mh.hd = 0;
                            if (matched) {
                                uint64_t key = (uint64_t)(uintptr_t)repl.pixels;
                                rhi_texture* tex = hd_cache_lookup(key);
                                if (!tex) {
                                    hd_pending_add(key, repl.pixels, repl.w, repl.h,
                                                   repl.box_w, repl.box_h);   /* next present */
                                } else {
                                    want = tex;
                                    mh.hd = 1;
                                    /* File box in page-texel coords: the tri box
                                     * (bu0 page-relative <-> id.box upload-
                                     * relative) anchors the conversion. */
                                    mh.hd_u0 = bu0 - (float)(id.box_x - repl.box_x);
                                    mh.hd_v0 = bv0 - (float)(id.box_y - repl.box_y);
                                    mh.hd_uw = (float)repl.box_w;
                                    mh.hd_vh = (float)repl.box_h;
                                    /* Mod-material pipes carry only the flat
                                     * per-rung affine (their fragment contract
                                     * has no perspective-W varying for the
                                     * continuous ramp) — they still get the
                                     * per-texture retarget. */
                                    hd_pc fpc;
                                    tp_pack_fog(&m, cly, st->fog_delta, &id, &repl,
                                                mh.hd_u0, mh.hd_v0, &fpc);
                                    memcpy(mh.fog_a, fpc.a, sizeof fpc.a);
                                    memcpy(mh.fog_b, fpc.b, sizeof fpc.b);
                                }
                            }
                            if (want && want != pack_bound) {
                                rhi_bind_texture_set(s_nr.dev, want, 1);
                                pack_bound = want;
                            }
                            rhi_push_constants(s_nr.dev, &mh, sizeof mh);
                            rhi_draw(s_nr.dev, (uint32_t)(g * 3),
                                     (uint32_t)(first_vert + (i + t) * 3));
                            t += g;
                        }
                    }
                    bind_scene_pipe(self);   /* HD-path restore discipline */
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
                    hd_pending_add(key, px, mw, mh, 0, 0);
                } else {
                    /* Mod-registered textures are not palette-laddered, so
                     * they push the identity fog transform. */
                    const hd_pc pc = { 0.f, 0.f, (float)mw, (float)mh,
                                       { 1.f, 1.f, 1.f, 0.f }, { 0.f, 0.f, 0.f, 0.f } };
                    bind_scene_pipe(hd_pipe);
                    rhi_bind_vram(s_nr.dev, tex);
                    rhi_push_constants(s_nr.dev, &pc, sizeof pc);
                    rhi_draw(s_nr.dev, (uint32_t)(run * 3), (uint32_t)(first_vert + i * 3));
                    bind_scene_pipe(self);
                    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                }
            }
            i += run;
            continue;
        }
        /* Registry query — validation (RESOLVE_LOG), dump (DUMP_DIR),
         * replacement. Identity is PER TRI (a run-union
         * box varies with per-frame clipping and tile composition); adjacent
         * tris matching the same pack file coalesce into HD sub-runs, the
         * rest draw SD in material-coalesced groups as before. */
        if (!(s_tp_resolve_log || s_tp_dump_on || s_tp_census_on || want_replace)) {
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
            texture_pack_id id;
            texture_pack_repl repl;
            int cly; float u0, v0;
            int matched = tp_tri_match(st, &m, want_replace, &id, &repl,
                                       &cly, &u0, &v0);
            /* Grow the sub-run: consecutive tris with the same disposition
             * (same pack file, or both unreplaced). */
            int g = 1;
            while (t + g < run) {
                const scene_tri_t* sn = &scene[s_txscene[base + i + t + g]];
                texture_pack_id nid;
                texture_pack_repl nrepl;
                int ncly; float nu0, nv0;
                int nmatched = tp_tri_match(sn, &m, want_replace, &nid, &nrepl,
                                            &ncly, &nu0, &nv0);
                if (nmatched != matched) break;
                if (matched && nrepl.pixels != repl.pixels) break;
                /* A replaced run carries ONE fog transform in its push
                 * constants, and canonicalising the CLUT means tris at
                 * different fog distances now resolve to the SAME pack file —
                 * so they must not coalesce, or the whole run would draw at
                 * the leading tri's fog depth. */
                if (matched && sn->fog_delta != st->fog_delta) break;
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
                    hd_pending_add(key, repl.pixels, repl.w, repl.h,
                                   repl.box_w, repl.box_h);   /* next frame */
                } else {
                    /* Shader rect = the FILE's box in page-texel coords: the
                     * query box (u0 page-relative <-> id.box upload-relative)
                     * anchors the conversion. */
                    hd_pc pc = {
                        u0 - (float)(id.box_x - repl.box_x),
                        v0 - (float)(id.box_y - repl.box_y),
                        (float)repl.box_w, (float)repl.box_h,
                        { 0 }, { 0 }, { 0 }, { 0 }, { 0 },
                    };
                    tp_pack_fog(&m, cly, st->fog_delta, &id, &repl,
                                pc.u0, pc.v0, &pc);
                    if (hd_defer_ok) {
                        hd_defer_push(tex, &pc, (uint32_t)(g * 3),
                                      (uint32_t)(first_vert + (i + t) * 3));
                    } else {
                        bind_scene_pipe(hd_pipe);
                        rhi_bind_vram(s_nr.dev, tex);
                        rhi_push_constants(s_nr.dev, &pc, sizeof pc);
                        rhi_draw(s_nr.dev, (uint32_t)(g * 3), (uint32_t)(first_vert + (i + t) * 3));
                        bind_scene_pipe(self);
                        rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                        m_pushed = 0;           /* push constants were overwritten */
                    }
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
    if (hd_defer_ok) hd_defer_flush(hd_pipe, self);
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
            if (pipe) { bind_scene_pipe(pipe); rhi_draw(s_nr.dev, 3, (uint32_t)voff); }
            voff += 3;
        }
    }
    if (hud_tex_count > 0 && s_nr.vram_tex && s_nr.scene_tex_hud_pipe) {  /* textured HUD (2D backdrops) */
        bind_scene_pipe(s_nr.scene_tex_hud_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        draw_tex_runs(hud_tex_first, hud_tex_count, tx_start, scene, s_nr.scene_tex_hud_pipe,
                      s_nr.hd_tex_pipe, 0, 0);  /* bucket 6, submission order + run-coalesce; HD = alpha, depth-off */
    }
    {   /* OPENPETE_HUD6_ZPIPE=1 (A/B): draw the glyph bucket through the opaque
         * depth-tested pipe after a depth clear — isolates gouraud_hud_pipe state. */
        static int s_zp = -1;
        if (s_zp < 0) { const char* e = getenv("OPENPETE_HUD6_ZPIPE"); s_zp = (e && *e && *e != '0'); }
        if (s_zp && s_nr.gouraud_pipe && ut_count[6] > 0) {
            rhi_clear_depth(s_nr.dev);
            bind_scene_pipe(s_nr.gouraud_pipe);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, (uint32_t)ut_count[6], (uint32_t)ut_first[6]);
            return;
        }
    }
    if (s_nr.gouraud_hud_pipe && ut_count[6] > 0) {                 /* untextured HUD text/glyphs */
        /* The slot-6 VB range is depth-sorted back-to-front at build time (see
         * the bucket-6 build in render_scene) so rotating near-OT mobys — the
         * inventory letters/icons — self-occlude correctly despite the
         * depth-off pipe. (The textured HUD range above stays submission-order:
         * its content is constant-z 2D sprites; extend the sort there if a
         * rotating TEXTURED near-OT moby ever appears.) */
        bind_scene_pipe(s_nr.gouraud_hud_pipe);
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

/* The freeze backdrop source. Prefer the capture-time shot_frozen (populated by
 * openpete_native_render_snapshot_frozen during a headless screenshot run) when
 * it holds a world frame; otherwise the live-present frozen_tex. Lets a headless
 * freeze-frame dump show the green frozen-world effect that frozen_tex (live-
 * window only) can't provide. */
static rhi_texture *active_frozen_tex(int *out_valid)
{
    if (s_nr.shot_frozen && s_nr.shot_frozen_valid) {
        if (out_valid) *out_valid = 1;
        return s_nr.shot_frozen;
    }
    if (out_valid) *out_valid = s_nr.frozen_valid ? 1 : 0;
    return s_nr.frozen_tex;
}

/* Backdrop pair: the far-plane bg fill (VB slot 0, built by the caller) and
 * the SKY (cyclorama dome, untextured bucket 11). Both z=1.0 through the
 * PLAIN opaque pipe (the dome never stencil-MARKs, so the W2.6-c hole-fill
 * can repaint sky showing through terrain seams); bg must precede the dome —
 * LESS_OR_EQUAL gives the later draw the z=1.0 ties, and the dome wins over
 * the fill. Called either before the group loop (draw-first order) or right
 * after the WORLD-opaque group (deferred: early-Z rejects covered pixels). */
static void draw_backdrop_fills(uint32_t bg, const int* ut_first,
                                const int* ut_count, int sky_ok)
{
    if (bg != 0 && s_nr.gouraud_pipe) {
        rhi_gpu_zone_begin(s_nr.dev, "sc.bg");
        bind_scene_pipe(s_nr.gouraud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, 6, 0);
        rhi_gpu_zone_end(s_nr.dev);
    }
    /* The plain span and the material spans are ONE bucket and must stay
     * under one condition: a deferred tri is out of the plain span, so a
     * `sky_ok` that suppressed only one of these would draw a hole or draw
     * a sky nothing suppressed. Keep them together (ut_mat_spans_ok). */
    if (sky_ok && s_nr.gouraud_pipe) {
        rhi_gpu_zone_begin(s_nr.dev, "sc.sky");
        if (ut_count[UT_BUCKET_SKY] > 0) {
            bind_scene_pipe(s_nr.gouraud_pipe);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, (uint32_t)ut_count[UT_BUCKET_SKY],
                     (uint32_t)ut_first[UT_BUCKET_SKY]);
        }
        draw_ut_mat_spans(UT_BUCKET_SKY, s_nr.gouraud_pipe, 0, 0); /* opaque backdrop */
        rhi_gpu_zone_end(s_nr.dev);
    }
}


/* ── Portal plane-cover invariant sweep ─────────────────────────────────
 * The plane-cover rule: inside an opening's footprint every pixel must
 * resolve to the preview or to content NEARER than plane − M. This sweep verifies it against the complete scene
 * list (terrain, actors, fills), independent of the feeders' own drop
 * logic: per interior pixel, the nearest pre-portal opaque
 * (WORLD/ACTOR/ACTOR_FRONT/WORLD_FILL) that sits BEHIND plane − M must be
 * beaten by a PORTAL_CYCLORAMA tri covering that pixel. PLAYER is exempt
 * (Spyro crossing the plane mid-entry legitimately overlaps the preview).
 * A scene-list mirror: feeder drops, category routing, and footprint
 * coverage are attested here; GPU stencil/pass wiring is the presentation
 * golden's job. */
static openpete_native_cover_plane s_cvp[8];
static int   s_cvp_n = 0;
static float s_cvp_m = 0.0f;
void openpete_native_set_cover_planes(const openpete_native_cover_plane *planes,
                                      int n, float margin_world)
{
    if (n > 8) n = 8;
    if (n > 0) memcpy(s_cvp, planes, sizeof(*planes) * (size_t)n);
    s_cvp_n = n;
    s_cvp_m = margin_world;
}
static void portal_cover_check(const scene_tri_t* sc, int n)
{
    static long chk = -2;
    if (chk == -2) { const char* e = getenv("OPENPETE_PORTAL_COVER_CHECK");
                     chk = (e && *e) ? atol(e) : -1; }
    if (chk < 0 || s_scene_frame != (unsigned)chk || s_cvp_n <= 0) return;
    for (int pi = 0; pi < s_cvp_n; pi++) {
        const openpete_native_cover_plane* P = &s_cvp[pi];
        if (P->pn < 3) continue;
        float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
        for (int k = 0; k < P->pn; k++) {
            if (P->px[k] < bx0) bx0 = P->px[k];
            if (P->px[k] > bx1) bx1 = P->px[k];
            if (P->py[k] < by0) by0 = P->py[k];
            if (P->py[k] > by1) by1 = P->py[k];
        }
        /* Candidate tris: bbox-overlap prefilter, split opaque vs preview. */
        static int cand[MAX_SCENE_TRIS]; int cn = 0;
        for (int t = 0; t < n && cn < MAX_SCENE_TRIS; t++) {
            const scene_tri_t* T = &sc[t];
            const int c = T->category;
            const int terr = (c == OPENPETE_SCENE_WORLD || c == OPENPETE_SCENE_ACTOR ||
                              c == OPENPETE_SCENE_ACTOR_FRONT || c == OPENPETE_SCENE_WORLD_FILL);
            if (!terr && c != OPENPETE_SCENE_PORTAL_CYCLORAMA) continue;
            float tx0 = 1e9f, ty0 = 1e9f, tx1 = -1e9f, ty1 = -1e9f;
            for (int k = 0; k < 3; k++) {
                if (T->v[k].sx < tx0) tx0 = T->v[k].sx;
                if (T->v[k].sx > tx1) tx1 = T->v[k].sx;
                if (T->v[k].sy < ty0) ty0 = T->v[k].sy;
                if (T->v[k].sy > ty1) ty1 = T->v[k].sy;
            }
            if (tx1 < bx0 || tx0 > bx1 || ty1 < by0 || ty0 > by1) continue;
            cand[cn++] = t;
        }
        int in_px = 0, prev_px = 0, front_px = 0, empty_px = 0, viol = 0, tviol = 0;
        float vpx = 0.0f, vpy = 0.0f, tvx = 0.0f, tvy = 0.0f;
        for (int py = (int)by0; py <= (int)(by1 + 1.0f); py++)
        for (int px = (int)bx0; px <= (int)(bx1 + 1.0f); px++) {
            const float X = (float)px + 0.5f, Y = (float)py + 0.5f;
            int in = 0;
            for (int a = 0, b = P->pn - 1; a < P->pn; b = a++) {
                if ((P->py[a] > Y) != (P->py[b] > Y) &&
                    X < (P->px[b] - P->px[a]) * (Y - P->py[a]) /
                        (P->py[b] - P->py[a]) + P->px[a])
                    in = !in;
            }
            if (!in) continue;
            in_px++;
            const double inv = P->a * X + P->b * Y + P->c;
            if (inv <= 1e-9) continue;                  /* plane behind camera here */
            double pvz = 1.0 / inv - (double)s_cvp_m;
            if (pvz < 1.0) pvz = 1.0;
            const float zlim = openpete_native_depth_from_viewz(
                                   openpete_worldvz_full((float)pvz));
            float zw = 2.0f, zc = 2.0f, zb = 2.0f;
            for (int ci = 0; ci < cn; ci++) {
                const scene_tri_t* T = &sc[cand[ci]];
                const float ax = T->v[0].sx, ay = T->v[0].sy;
                const float e1x = T->v[1].sx - ax, e1y = T->v[1].sy - ay;
                const float e2x = T->v[2].sx - ax, e2y = T->v[2].sy - ay;
                const float det = e1x * e2y - e2x * e1y;
                if (det > -1e-6f && det < 1e-6f) continue;
                const float rx = X - ax, ry = Y - ay;
                const float b1 = (rx * e2y - e2x * ry) / det;
                const float b2 = (e1x * ry - rx * e1y) / det;
                const float b0 = 1.0f - b1 - b2;
                if (b0 < -0.001f || b1 < -0.001f || b2 < -0.001f) continue;
                const float z = b0 * T->v[0].z + b1 * T->v[1].z + b2 * T->v[2].z;
                if (T->category == OPENPETE_SCENE_PORTAL_CYCLORAMA) {
                    if (z < zc) zc = z;
                } else if (T->blend != 0) {
                    if (z < zb) zb = z;
                } else if (z < zw) zw = z;
            }
            /* Transparent terrain draws AFTER the portal sequence and blends
             * over the preview — a behind-plane blended tri at this pixel
             * means the feeder's cover drop failed. */
            if (zb < 2.0f && zb > zlim + 1e-6f) {
                if (!tviol) { tvx = X; tvy = Y; }
                tviol++;
            }
            if (zw >= 2.0f) { empty_px++; continue; }   /* no opaque content */
            if (zw <= zlim + 1e-6f) { front_px++; continue; }  /* legit front */
            if (zc <= zw + 1e-6f) { prev_px++; continue; }     /* preview covers */
            if (!viol) { vpx = X; vpy = Y; }
            viol++;
        }
        fprintf(stderr, "[pcover f=%u] pi=%d pn=%d M=%.0f in=%d preview=%d "
                "front=%d empty=%d VIOL=%d first=(%.1f,%.1f) TVIOL=%d tfirst=(%.1f,%.1f)\n",
                s_scene_frame, pi, P->pn, (double)s_cvp_m, in_px, prev_px,
                front_px, empty_px, viol, (double)vpx, (double)vpy,
                tviol, (double)tvx, (double)tvy);
    }
}

/* Per-PRESENT scene instruments. Hosted here rather than in
 * openpete_native_scene_begin_frame because that block is gated on the VBL
 * number changing, and sub-tick presents REUSE the current VBL number — so a
 * probe living there never observes an intermediate present, exactly where the
 * sub-tick re-extract path differs from the canonical one. */
static void scene_probes(scene_tri_t* sc, int n)
{
    /* OPENPETE_SCENECAT_TINT=<category>[,<rgb hex>] — recolour every
     * completed scene triangle of one layer. Renderer-side, so it
     * shows what actually reaches the framebuffer for that layer
     * regardless of which feeder produced it (feeder-side tints miss
     * anything a feeder emits through a shared helper). */
    { static int tc = -2; static uint32_t trgb = 0x00FF00u;
      if (tc == -2) { const char* e = getenv("OPENPETE_SCENECAT_TINT");
          unsigned c, rgb;
          if (e && sscanf(e, "%u,%x", &c, &rgb) == 2) { tc = (int)c; trgb = rgb; }
          else if (e && sscanf(e, "%u", &c) == 1)     { tc = (int)c; }
          else tc = -1; }
      /* trgb is given as rrggbb (what a human types); scene vertex
       * colour is 0xAABBGGRR, so swap R and B rather than making every
       * caller pre-swizzle — passing rrggbb straight through paints the
       * complement and silently invalidates any colour-keyed count. */
      if (tc >= 0) {
        const uint32_t tv = 0xFF000000u
                          | ((trgb & 0x0000FFu) << 16)
                          | ((trgb & 0x00FF00u))
                          | ((trgb & 0xFF0000u) >> 16);
        for (int ti = 0; ti < n; ti++)
            if (sc[ti].category == tc) {
                sc[ti].textured = 0;
                for (int k = 0; k < 3; k++)
                    sc[ti].v[k].rgba = tv;
            } } }
    /* OPENPETE_SCENEDUMP=<frame> — dump EVERY completed scene triangle of the
     * first present labelled >= that frame: category/blend/tex/group + all
     * three vertices (sx, sy, z, pvz, rgba). The probe below answers "who
     * covers this point"; the dump answers "what is in the scene at all" with
     * no coverage math to be wrong. */
    { static int sd = -2; static unsigned sdf = 0;
      if (sd == -2) { const char* e = getenv("OPENPETE_SCENEDUMP");
          sd = (e && *e) ? 1 : -1; sdf = (e && *e) ? (unsigned)atoi(e) : 0; }
      if (sd == 1 && s_scene_frame >= sdf) {
        sd = -1;
        fprintf(stderr, "[scenedump f=%u] n=%d\n", s_scene_frame, n);
        for (int ti = 0; ti < n; ti++) {
            const scene_tri_t* T = &sc[ti];
            fprintf(stderr, "[sd] #%d cat=%d blend=%d tex=%d grp=%d ko=%.1f", ti,
                    T->category, T->blend, T->textured, T->group,
                    (double)T->koff);
            for (int k = 0; k < 3; k++)
                fprintf(stderr, " (%.2f,%.2f,z%.6f,w%.1f,%08X)",
                        (double)T->v[k].sx, (double)T->v[k].sy,
                        (double)T->v[k].z, (double)T->v[k].pvz, T->v[k].rgba);
            fprintf(stderr, "\n");
        }
      } }
    portal_cover_check(sc, n);
}

static bool draw_native_scene(bool full_extent)
{
    int scene_n; uint32_t scene_bg, scene_fade, scene_freeze; int hud_under = 0, actor_front = 0;
    const scene_tri_t* scene = scene_present_buf(&scene_n, &scene_bg, &scene_fade, &scene_freeze, &hud_under, &actor_front);
    scene_probes((scene_tri_t*)scene, scene_n);   /* per-present: sees intermediates too */

    /* OPENPETE_TERRAIN_KEY_IDENT=1: prove the hoisted key pass and this draw
     * packed from the SAME scene snapshot + clip mapping — repack here and
     * byte-compare against the pass's recorded batch. A mismatch means the
     * snapshot or mapping moved between pass and draw: the one failure class
     * a key-image capture cannot attribute (the shared image is sequentially
     * overwritten per target, so post-frame readbacks only ever show the
     * LAST pass's content). */
    { static int s_ident = -1;
      if (s_ident < 0) { const char* e = getenv("OPENPETE_TERRAIN_KEY_IDENT");
          s_ident = (e && *e && *e != '0'); }
      if (s_ident && s_tk_last.m >= 0 && openpete_native_key_resolve_on()
          && s_tk_last.full_extent == (full_extent ? 1 : 0)) {
          static rhi_vertex ibuf[SCENE_VB_VERTS];
          static keytex_span_t ispans[KEYTEX_MAX_SPANS];
          for (int cls = 0; cls < 3; cls++) {
              int lm = cls == 1 ? s_tk_last.mm : cls == 2 ? s_tk_last.am : s_tk_last.m;
              const rhi_vertex* lb = cls == 1 ? s_tk_last.mbuf
                                   : cls == 2 ? s_tk_last.abuf : s_tk_last.buf;
              int iuntex = 0, inspans = 0;
              int im = pack_terrain_key_batch(scene, scene_n, full_extent ? 1 : 0, cls, ibuf,
                                              &iuntex, ispans, &inspans);
              size_t db = 0, nb = sizeof(rhi_vertex) * (size_t)(im < lm ? im : lm);
              const uint8_t* a = (const uint8_t*)ibuf;
              const uint8_t* b = (const uint8_t*)lb;
              while (db < nb && a[db] == b[db]) db++;
              if (im != lm || db < nb)
                  fprintf(stderr, "[keyident f=%u] MISMATCH cls=%d fe=%d m=%d/%d scene=%p/%p n=%d/%d first-diff vert=%zu\n",
                          s_scene_frame, cls, full_extent ? 1 : 0, im, lm,
                          (const void*)scene, (const void*)s_tk_last.scene,
                          scene_n, s_tk_last.scene_n, db / sizeof(rhi_vertex));
              else
                  fprintf(stderr, "[keyident f=%u] OK cls=%d fe=%d m=%d\n",
                          s_scene_frame, cls, full_extent ? 1 : 0, im);
          }
      } }

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
                    cat[10], cat[11], blend, scene_fade, scene_freeze, ps_drawenv_clear(), scene_bg);
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
     * scene_bg is the fallback (no auto-clear). */
    uint32_t bg = ps_drawenv_clear() ? ps_drawenv_clear() : scene_bg;

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
        /* ACTOR opaques also MARK coverage (like WORLD): authored HP ground
         * holes under static decor (pedestal discs) must read as covered so
         * the LP hole-fill — drawn after this group — never paints over the
         * moby that hides them. */
        { 0, OPENPETE_SCENE_ACTOR,
              holefill_on ? s_nr.gouraud_mark_pipe   : s_nr.gouraud_pipe,
              holefill_on ? s_nr.scene_tex_mark_pipe : s_nr.scene_tex_pipe    },
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
    int ut_first[15], ut_count[15];
    /* CPU attribution (--profile-out): the compose phase splits into
     * build_ut (untextured bucketing incl. portal-run collection),
     * build_tex (textured bucketing + material sort), vb_tex (textured
     * vertex pack), upload (VB update) and record (draw recording). */
#define NR_PROF_BEGIN(lit) do { static int op_id_ = -1; \
        if (op_prof_on()) { if (op_id_ < 0) op_id_ = op_prof_intern(lit); \
                            op_prof_begin(op_id_); } } while (0)
#define NR_PROF_END() do { if (op_prof_on()) op_prof_end(); } while (0)

    NR_PROF_BEGIN("cmp.build_ut");
    s_utmat_n = 0; s_utmemo_n = 0;
    for (int gi = 0; gi < 6; gi++) {
        ut_first[gi] = n;
        int gb = groups[gi].blend, gc = groups[gi].gcat;
        /* Defer only what this bucket will actually draw — see
         * ut_mat_spans_ok — and only while the span table can still record
         * it. Deferring more than that DELETES it: the tri leaves the plain
         * span and nothing else draws it. */
        const int defer_ok = tag_split_on() && ut_mat_spans_ok(gi, gb, gc) &&
                             s_utmat_n < UT_MAT_SPANS;
        int dn = 0;
        for (int t = 0; t < scene_n; t++) {
            if (scene[t].textured || n + 3 > SCENE_VB_VERTS) continue;
            if (scene[t].category == OPENPETE_SCENE_HUD ||
                scene[t].category == OPENPETE_SCENE_HUD3D ||
                scene[t].category == OPENPETE_SCENE_HUD_PANEL) continue;  /* -> HUD passes */
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
            if (defer_ok && scene[t].tag && ut_tag_resolves(scene[t].tag)) {
                s_utdef_idx[dn++] = t;             /* appended after the plain span */
                continue;
            }
            for (int k = 0; k < 3; k++) {
                float cx, cy;
                scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
                rhi_vertex *o = &buf[n + k];
                o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
                o->u = is_shadow ? scene[t].v[k].sx : 0.0f;
                o->v = is_shadow ? scene[t].v[k].sy : 0.0f;
                o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
            }
            n += 3;
        }
        ut_count[gi] = n - ut_first[gi];           /* the PLAIN (stock-drawn) span */
        /* Deferred tris: grouped by resolve-relevant tag into contiguous
         * spans after the plain span (ut_emit_deferred). */
        n = ut_emit_deferred(gi, scene, buf, n, dn);
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
        static int s_h6sort = -1;   /* OPENPETE_HUD6_NOSORT=1: A/B — submission order */
        if (s_h6sort < 0) { const char* e = getenv("OPENPETE_HUD6_NOSORT"); s_h6sort = !(e && *e && *e != '0'); }
        if (s_h6sort)
            qsort(ho, (size_t)hn, sizeof ho[0], hud_paint_cmp);
        /* OPENPETE_HUD6_DBG=frameLo: paint-order forensics — every bucket-6 tri
         * whose v0 lands in a fixed probe box, in SORTED (drawn) order. */
        static int s_h6 = -2;
        if (s_h6 == -2) { const char* e = getenv("OPENPETE_HUD6_DBG"); s_h6 = (e && *e) ? atoi(e) : -1; }
        int h6_live = s_h6 >= 0 && (int)s_scene_frame >= s_h6 && (int)s_scene_frame < s_h6 + 2;
        for (int i = 0; i < hn && n + 3 <= SCENE_VB_VERTS; i++) {
            int t = ho[i].t;
            if (h6_live) {
                float x0 = scene[t].v[0].sx, x1 = x0, y0 = scene[t].v[0].sy, y1 = y0;
                for (int k = 1; k < 3; k++) {
                    float sx2 = scene[t].v[k].sx, sy2 = scene[t].v[k].sy;
                    if (sx2 < x0) x0 = sx2; if (sx2 > x1) x1 = sx2;
                    if (sy2 < y0) y0 = sy2; if (sy2 > y1) y1 = sy2;
                }
                if (i >= hn - 12 || (x1 >= 308.0f && x0 <= 336.0f && y1 >= 183.0f && y0 <= 214.0f)) {
                    float c[3][2];
                    for (int k = 0; k < 3; k++)
                        scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &c[k][0], &c[k][1]);
                    fprintf(stderr, "[hud6] f=%u i=%d/%d t=%d z=%.4f rgba=%08X "
                            "v0=%.1f,%.1f v1=%.1f,%.1f v2=%.1f,%.1f clip=(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f)\n",
                            s_scene_frame, i, hn, t, (double)ho[i].z, scene[t].v[0].rgba,
                            (double)scene[t].v[0].sx, (double)scene[t].v[0].sy,
                            (double)scene[t].v[1].sx, (double)scene[t].v[1].sy,
                            (double)scene[t].v[2].sx, (double)scene[t].v[2].sy,
                            (double)c[0][0], (double)c[0][1], (double)c[1][0], (double)c[1][1],
                            (double)c[2][0], (double)c[2][1]);
                }
            }
            for (int k = 0; k < 3; k++) {
                float cx, cy;
                scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
                rhi_vertex *o = &buf[n + k];
                o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
            o->u = 0.0f; o->v = 0.0f;
                o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
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
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
            o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
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
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
            o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
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
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
            o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
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
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
            o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
        }
        n += 3;
    }
    ut_count[10] = n - ut_first[10];
    /* SKY (cyclorama dome) + WORLD_FILL (terrain LP hole-fill) untextured
     * buckets (W2.6-c). SKY draws before the group loop with the PLAIN opaque
     * pipe (depth-tested backdrop, no stencil mark); FILL draws right after
     * the WORLD group with the TEST_ZERO pipe. Both untextured by
     * construction (gouraud dome / gouraud LP). */
    ut_first[UT_BUCKET_SKY] = n;
    {
        /* SKY takes the same material deferral as the numbered groups, and
         * through the same predicate — a dome-wide selector is one of the
         * plainest things a mod can ask for, and without this the sky is the
         * one opaque channel no material can reach. One tag, so at most one
         * deferred span. */
        const int sky_defer = tag_split_on() &&
            ut_mat_spans_ok(UT_BUCKET_SKY, 0, OPENPETE_SCENE_SKY) &&
            s_utmat_n < UT_MAT_SPANS;
        int sdn = 0;
        for (int t = 0; t < scene_n; t++) {
            if (scene[t].textured || scene[t].category != OPENPETE_SCENE_SKY ||
                n + 3 > SCENE_VB_VERTS) continue;
            if (sky_defer && scene[t].tag && ut_tag_resolves(scene[t].tag)) {
                s_utdef_idx[sdn++] = t;
                continue;
            }
            for (int k = 0; k < 3; k++) {
                float cx, cy;
                scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
                rhi_vertex *o = &buf[n + k];
                o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
                o->u = 0.0f; o->v = 0.0f;
                o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
            }
            n += 3;
        }
        ut_count[UT_BUCKET_SKY] = n - ut_first[UT_BUCKET_SKY];   /* PLAIN span */
        n = ut_emit_deferred(UT_BUCKET_SKY, scene, buf, n, sdn);
    }
    ut_first[12] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_WORLD_FILL || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
            o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
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
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
            o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
        }
        n += 3;
    }
    ut_count[13] = n - ut_first[13];
    /* ACTOR_FRONT untextured (opaque): the rescue actors' flat-shaded faces.
     * Same content class as ACTOR (blend 0, depth-tested opaque); split out so the
     * dragon-rescue pass can front-pass ONLY these. */
    ut_first[14] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_ACTOR_FRONT ||
            n + 3 > SCENE_VB_VERTS) continue;
        int b = scene[t].blend; if (b < 0 || b > 3) b = 0;
        if (b != 0) continue;   /* transparent rescue-actor faces route by blend (ALPHA/ADD) */
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
            o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
        }
        n += 3;
    }
    ut_count[14] = n - ut_first[14];
    /* PORTAL buckets (W2.6): sky colours (PORTAL_CYCLORAMA) + their stencil
     * aperture masks (PORTAL_MASK), collected per-portal via the feeder's group
     * id. Draw site (after the WORLD group): per group — clear stencil →
     * rasterize the mask invisibly, depth-TESTED against the world (the arch
     * stone / a nearer wall punches it out) → draw the colours stencil-gated
     * and ALSO depth-TESTED (write off — "noz" is no z WRITE, not no z test),
     * so a colour whose depth disagrees with the mask's silently drops out.
     * Groups draw far→near by plane depth so a nearer portal
     * paints over a farther one seen through it; within a group, submission
     * order = the feeder's painter order (flat base → dome). */
    struct portal_run { int group; int first, count; float depth; };
    enum { MAX_PORTAL_RUNS = 32 };
    struct portal_run pmask[MAX_PORTAL_RUNS], psky[MAX_PORTAL_RUNS];
    int pmask_n = 0, psky_n = 0;
    /* Material segments inside a sky run. The run itself must stay
     * one-per-portal — masks pair by group and groups sort by depth — so an
     * override splits the DRAW of a run, not the run. Segments are contiguous
     * and in submission order (flat base, then dome), which is what lets a
     * selector take the close dome and leave the far chip stock. */
    struct portal_seg { int run; int first, count; uint32_t tag; };
    enum { MAX_PORTAL_SEGS = 96 };
    struct portal_seg pseg[MAX_PORTAL_SEGS];
    int pseg_n = 0;
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
            /* Sky pass only: open a segment whenever the resolve-relevant tag
             * changes (or the run did), so one portal's flat base and dome can
             * take different materials. */
            if (cat && tag_split_on()) {
                uint32_t mt = ut_mask_tag(scene[t].tag);
                int fresh = (pseg_n == 0) || pseg[pseg_n - 1].run != cur ||
                            pseg[pseg_n - 1].tag != mt;
                if (fresh && pseg_n < MAX_PORTAL_SEGS)
                    pseg[pseg_n++] = (struct portal_seg){ cur, n, 0, mt };
            }
            for (int k = 0; k < 3; k++) {
                float cx, cy;
                scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
                rhi_vertex *o = &buf[n + k];
                o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->w = scene[t].v[k].pvz;
            o->u = 0.0f; o->v = 0.0f;
                o->rgba = scene[t].v[k].rgba; o->koff = scene[t].koff;
                o->sx = scene[t].rank; o->sy = (float)scene[t].mordinal;
            }
            runs[cur].count += 3;
            if (cat && pseg_n && pseg[pseg_n - 1].run == cur) pseg[pseg_n - 1].count += 3;
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
    NR_PROF_END();   /* cmp.build_ut */
    NR_PROF_BEGIN("cmp.build_tex");
    int tx_start = n;
    int txg_first[10], txg_count[10], txtri = 0;
    for (int b = 0; b < 10; b++) {
        txg_first[b] = tx_start + txtri * 3;
        int run_start = txtri;
        for (int t = 0; t < scene_n; t++) {
            if (!scene[t].textured) continue;
            if (tx_start + (txtri + 1) * 3 > SCENE_VB_VERTS) break;
            int cat = scene[t].category;
            int belongs;
            if (b == 6) belongs = (cat == OPENPETE_SCENE_HUD);
            else if (b == 9) {
                /* ACTOR_FRONT (opaque): the rescue actors' textured faces — same
                 * class as the ACTOR textured group (b==2), split for the front
                 * pass. */
                int bl = scene[t].blend; if (bl < 0 || bl > 3) bl = 0;
                belongs = cat == OPENPETE_SCENE_ACTOR_FRONT && bl == 0;
            }
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
        if (b == 0 || b == 2 || b == 3 || b == 9) {   /* opaque: material-sort (depth-correct) */
            NR_PROF_BEGIN("cmp.sort");
            tex_mat_sort(scene, &s_txscene[run_start], txtri - run_start);
            NR_PROF_END();
        }
        txg_count[b] = (tx_start + txtri * 3) - txg_first[b];
    }
    NR_PROF_END();   /* cmp.build_tex */
    NR_PROF_BEGIN("cmp.vb_tex");
    for (int i = 0; i < txtri; i++) {
        const scene_tri_t *st = &scene[s_txscene[i]];
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(st->v[k].sx, st->v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[tx_start + i * 3 + k];
            o->x = cx; o->y = cy; o->z = st->v[k].z; o->w = st->v[k].pvz;
            o->u = st->v[k].u; o->v = st->v[k].v;
            o->rgba = st->v[k].rgba; o->koff = st->koff;
            /* Location 3 is per-class: the textured SHADOW decal parks its PSX
             * screen coord there (shadow_decal_tex.vert's depth-pre-pass
             * lookup; scene_to_clip's INPUT is the PSX screen coord). Every
             * other textured tri carries the moby paint (rank, ordinal) pair —
             * psx_scene.vert forwards it to the same-moby painter select, and
             * unranked tris (rank 0) keep the select off. */
            if (st->category == OPENPETE_SCENE_SHADOW) {
                o->sx = st->v[k].sx; o->sy = st->v[k].sy;
            } else {
                o->sx = st->rank; o->sy = (float)st->mordinal;
            }
        }
    }
    n = tx_start + txtri * 3;

    /* Full-screen fade overlay (func_800190D4): a single full-panel quad at the
     * near plane (z=0 passes LESS_OR_EQUAL against everything), drawn dead last so
     * it tints the WHOLE composited frame — world, actors, HUD and 2D UI — exactly
     * as the PSX's semi-transparent full-screen POLY_F4 does. The vertex colour is
     * the fade colour; the blend mode (set below) reproduces the PSX ABR. */
    NR_PROF_END();   /* cmp.vb_tex */
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
    int frozen_ok = 0;
    (void)active_frozen_tex(&frozen_ok);
    if ((scene_freeze & (1u << 31)) && frozen_ok && s_nr.freeze_overlay_pipe
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

    {   /* OPENPETE_HUD6_DBG companion: verify the bucket-6 VB range still holds
         * the glyph verts at upload time (catches a later build overwriting it). */
        static int s_h6b = -2;
        if (s_h6b == -2) { const char* e = getenv("OPENPETE_HUD6_DBG"); s_h6b = (e && *e) ? atoi(e) : -1; }
        if (s_h6b >= 0 && (int)s_scene_frame >= s_h6b && (int)s_scene_frame < s_h6b + 2) {
            int hits = 0;
            for (int v = ut_first[6]; v < ut_first[6] + ut_count[6]; v++)
                if (buf[v].x > 0.20f && buf[v].x < 0.31f && buf[v].y > -0.82f && buf[v].y < -0.60f) hits++;
            fprintf(stderr, "[hud6vb] f=%u ut6=[%d+%d] n=%d glyph-verts-in-Vwindow=%d\n",
                    s_scene_frame, ut_first[6], ut_count[6], n, hits);
            /* the cap tris: any bucket-6 tri whose first vert x is ~0.2529 (f8a),
             * ~0.2695 (f8b) or ~0.2359 (f10) with y in the V band — dump the whole tri */
            for (int v = ut_first[6]; v + 2 < ut_first[6] + ut_count[6]; v += 3) {
                float x0 = buf[v].x, y0 = buf[v].y;
                if (y0 < -0.75f || y0 > -0.60f) continue;
                if ((x0 > 0.2515f && x0 < 0.2545f) || (x0 > 0.2345f && x0 < 0.2375f))
                    fprintf(stderr, "[hud6cap] f=%u vslot=%d (%.4f,%.4f,%.4f)(%.4f,%.4f,%.4f)(%.4f,%.4f,%.4f) rgba=%08X,%08X,%08X\n",
                            s_scene_frame, v,
                            (double)buf[v].x, (double)buf[v].y, (double)buf[v].z,
                            (double)buf[v+1].x, (double)buf[v+1].y, (double)buf[v+1].z,
                            (double)buf[v+2].x, (double)buf[v+2].y, (double)buf[v+2].z,
                            buf[v].rgba, buf[v+1].rgba, buf[v+2].rgba);
            }
        }
    }
    NR_PROF_BEGIN("cmp.upload");
    rhi_update_buffer(s_nr.scene_vb, buf, sizeof(rhi_vertex) * (size_t)(n > 0 ? n : 1));
    NR_PROF_END();
    NR_PROF_BEGIN("cmp.record");

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
    /* Backdrop deferral: the bg fill and the cyclorama dome are far-plane
     * (z=1.0) full-coverage fills that nearer opaque terrain almost entirely
     * overdraws — drawn first (the PSX painter order) every one of their
     * pixels is shaded and then repainted. Deferring the pair until after
     * the WORLD-opaque group lets early-Z reject every covered pixel, so
     * they shade only the gaps — identical output (both write z=1.0 and
     * everything nearer beats them; their bg -> dome relative order is kept,
     * LESS_OR_EQUAL gives the later dome the z=1.0 ties exactly as before).
     * A large win at SSAA/high-res scene targets. OPENPETE_BACKDROP_DEFER=0
     * restores draw-first; freeze frames skip the group loop, so they keep
     * the up-front order (bg under the freeze overlay, no dome). */
    /* Re-resolved on a live edit (registry epoch), never per VBL. */
    static int s_backdrop_defer = -1;
    static unsigned s_bd_resolved_ep = 0;
    const unsigned bd_ep = knob_registry_env_epoch();
    if (s_backdrop_defer < 0 || s_bd_resolved_ep != bd_ep) {
        const char* e = getenv("OPENPETE_BACKDROP_DEFER");
        s_backdrop_defer = !(e && *e && *e == '0');
        s_bd_resolved_ep = bd_ep;
    }
    if (!s_backdrop_defer || freeze_first >= 0)
        draw_backdrop_fills(bg, ut_first, ut_count,
                            /*sky*/ freeze_first < 0 && !s_backdrop_defer);

    /* Freeze backdrop FIRST (behind world/UI): the frozen native frame sampled
     * by freeze_overlay_pipe (set 0 = frozen_tex), desaturated + green-tinted. On
     * a freeze frame the world groups are empty (the game stops rendering the
     * world), so this fills the screen and the menu UI composites over it. */
    if (freeze_first >= 0) {
        rhi_texture *frz = active_frozen_tex(NULL);
        if (frz) {
            bind_scene_pipe(s_nr.freeze_overlay_pipe);
            rhi_bind_vram(s_nr.dev, frz);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, 6, (uint32_t)freeze_first);
        }
    }

    /* GPU attribution (OPENPETE_GPU_TIMERS=1): one nested zone per layered
     * group below, so the [gpu-pass]/trace "compose" cost splits by layer. */
    static const char* const k_scene_zone[6] =
        { "sc.world", "sc.shadow", "sc.actor", "sc.player", "sc.alpha", "sc.add" };

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
        rhi_gpu_zone_begin(s_nr.dev, k_scene_zone[gi]);
        const struct scene_group *g = &groups[gi];
        /* Dragon-rescue cutscene (actor_front): just BEFORE the PLAYER group,
         * depth-isolate the cutscene actors and draw them there so they win
         * over terrain they interpenetrate (Delvin's wing inside the tower).
         * BEFORE the player, so Spyro still sorts against the dragon by real Z.
         * The regular ACTOR group already drew normally at gi==2.
         *
         * Two passes over the SAME two buckets:
         *  1. PUNCH — the colour-write-off twins (depth compare ALWAYS) replace
         *     the stored depth on exactly the pixels the actors rasterize, so
         *     whatever terrain occluded them is evicted from those pixels and
         *     nowhere else. Texel-exact: same shaders, same discards.
         *  2. COLOUR — the ordinary opaque pipes. Every actor fragment nearer
         *     than the punched value passes, and the usual LESS_OR_EQUAL
         *     ordering picks the nearest of them, so the actors keep exact
         *     self-occlusion (the punched value is one of their own depths, so
         *     the nearest is never rejected).
         *
         * The depth left behind is min(true actor depth, world depth) — the
         * actors' own, on their own pixels; untouched everywhere else. That is
         * what the transparent groups at gi==4/5 test against, so semi-
         * transparent terrain buried behind the ground stays buried during a
         * cutscene, and distant level mobys keep true occlusion at any LOD.
         * (The predecessor cleared depth over the actors' whole screen bbox,
         * which handed every later-drawn transparent face a free pass through
         * the terrain inside that rect.) */
        if (gi == 3 && actor_front) {
            const int punch_ok = s_nr.gouraud_punch_pipe && s_nr.scene_tex_punch_pipe;
            for (int pass = punch_ok ? 0 : 1; pass < 2; pass++) {
                rhi_pipeline* upipe = pass ? s_nr.gouraud_pipe   : s_nr.gouraud_punch_pipe;
                rhi_pipeline* tpipe = pass ? s_nr.scene_tex_pipe : s_nr.scene_tex_punch_pipe;
                rhi_pipeline* hpipe = pass ? s_nr.hd_cutout_pipe : s_nr.hd_cutout_punch_pipe;
                if (upipe && ut_count[14] > 0) {
                    bind_scene_pipe(upipe);
                    rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                    rhi_draw(s_nr.dev, (uint32_t)ut_count[14], (uint32_t)ut_first[14]);
                }
                if (txg_count[9] > 0 && s_nr.vram_tex && tpipe) {
                    bind_scene_pipe(tpipe);
                    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                    rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                    /* The punch pass keeps mod material overrides OFF: its job
                     * is depth only, and a mod pipe would bring its own (and
                     * possibly different) discard rule to the coverage. */
                    draw_tex_runs(txg_first[9], txg_count[9], tx_start, scene,
                                  tpipe, hpipe, pass, 0);
                }
            }
        }
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
        if (g->utex && ut_count[gi] > 0) {
            if (gi == 1) {
                /* Untextured SUBTRACT = the Spyro silhouette shadow. Preferred
                 * path: the DEPTH-AWARE decal (shadow_decal.frag samples the
                 * shared WORLD-opaque pre-pass depth; per-pixel world-height
                 * wall rejection, no bias). Fallback (pre-pass unavailable this
                 * present / unsupported device): plain depth test + W2.5 bias. */
                if (s_od_ready && s_nr.opaque_depth) {
                    bind_scene_pipe(s_nr.gouraud_sub_pipe);
                    rhi_bind_depth_sampler(s_nr.dev, s_nr.opaque_depth, 0);
                    shadow_decal_pc pc;
                    pc.col0x = ps_shadow()->cam_col0[0];
                    pc.col0y = ps_shadow()->cam_col0[1];
                    pc.col0z = ps_shadow()->cam_col0[2];
                    pc.col1x = ps_shadow()->cam_col1[0];
                    pc.col1y = ps_shadow()->cam_col1[1];
                    pc.col1z = ps_shadow()->cam_col1[2];
                    pc.col2x = ps_shadow()->cam_col2[0];
                    pc.col2y = ps_shadow()->cam_col2[1];
                    pc.col2z = ps_shadow()->cam_col2[2];
                    pc.cam_pos_up = ps_shadow()->cam_pos_up;
                    pc.ofx = ps_shadow()->ofx; pc.ofy = ps_shadow()->ofy;
                    pc.h = ps_shadow()->h;
                    pc.ground_up = ps_shadow()->ground_up;
                    pc.wall_height = ps_shadow()->wall_height;
                    pc.steep_nup = ps_shadow()->steep_nup;
                    /* mode: env override for calibration; else height-test when
                     * the game supplied valid params, else draw-all (no mask). */
                    float mode = ps_shadow()->valid ? 1.0f : 0.0f;
                    const char* mv = getenv("OPENPETE_SHADOW_DECAL_MODE");
                    if (mv && *mv) mode = (float)atof(mv);
                    pc.mode = mode;
                    pc.band = shadow_decal_band();
                    pc.plat_eps = ps_shadow()->plat_eps;
                    pc.plat_slack = ps_shadow()->plat_slack;
                    pc.uax = s_od_uax; pc.ubx = s_od_ubx;
                    pc.vay = s_od_vay; pc.vby = s_od_vby;
                    pc.pvw = s_od_vw; pc.pvh = s_od_vh;
                    rhi_push_constants(s_nr.dev, &pc, sizeof pc);
                } else {
                    bind_scene_pipe(s_nr.gouraud_sub_ztest_pipe);
                    float c, s, cl;
                    shadow_hw_bias(&c, &s, &cl);
                    rhi_set_depth_bias(s_nr.dev, c, cl, s);
                }
            } else {
                bind_scene_pipe(g->utex);
            }
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, (uint32_t)ut_count[gi], (uint32_t)ut_first[gi]);
        }
        if (s_utmat_n && g->utex && ut_mat_spans_ok(gi, g->blend, g->gcat))
            draw_ut_mat_spans(gi, g->utex, holefill_on && (gi == 0 || gi == 2),
                              g->blend);
        if (txg_count[gi] > 0 && s_nr.vram_tex && g->tex) {
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
                const uint32_t s0_decal_tag = m0->tag;
                shadow_decal_tex_pc pc;
                pc.col0x = ps_shadow()->cam_col0[0];
                pc.col0y = ps_shadow()->cam_col0[1];
                pc.col0z = ps_shadow()->cam_col0[2];
                pc.col1x = ps_shadow()->cam_col1[0];
                pc.col1y = ps_shadow()->cam_col1[1];
                pc.col1z = ps_shadow()->cam_col1[2];
                pc.col2x = ps_shadow()->cam_col2[0];
                pc.col2y = ps_shadow()->cam_col2[1];
                pc.col2z = ps_shadow()->cam_col2[2];
                pc.cam_pos_up = ps_shadow()->cam_pos_up;
                pc.ofx = ps_shadow()->ofx; pc.ofy = ps_shadow()->ofy;
                pc.h = ps_shadow()->h;
                pc.wall_height = ps_shadow()->wall_height;
                pc.steep_nup = ps_shadow()->steep_nup;
                /* height+steep test when the feeder supplied valid camera params,
                 * else draw-all (band). Env override shares the Spyro decal knob. */
                float mode = ps_shadow()->valid ? 1.0f : 0.0f;
                const char* mv = getenv("OPENPETE_SHADOW_DECAL_MODE");
                if (mv && *mv) mode = (float)atof(mv);
                pc.mode = mode;
                pc.band = shadow_decal_band();
                pc.plat_eps = ps_shadow()->plat_eps;
                pc.plat_slack = ps_shadow()->plat_slack;
                pc.uax = s_od_uax; pc.ubx = s_od_ubx;
                pc.vay = s_od_vay; pc.vby = s_od_vby;
                pc.pvw = s_od_vw;  pc.pvh = s_od_vh;
                pc.tpage_x = m0->tpage_x; pc.tpage_y = m0->tpage_y;
                pc.clut_x = m0->clut_x;   pc.clut_y = m0->clut_y;
                pc.bpp = m0->bpp;
                /* Mod material on the DECAL PROFILE. The stock fragment
                 * stage here owns per-pixel occlusion, so a plain material
                 * would delete it (shadows would paint through walls) —
                 * hence a second contract, openpete_psx_decal.glsl, whose
                 * engine half hands the mod the visibility verdict to
                 * apply. Layout matches `mod_pipeline` exactly: sets 0/1
                 * samplers, set 3 the engine/mod UBO pair.
                 *
                 * The whole range is ONE draw because every moby shadow
                 * shares one sprite AND one tag, so there is nothing to
                 * split. A non-uniform tag would need run-splitting, so it
                 * falls back to stock rather than paint most of the range
                 * with the first tri's material. */
                rhi_pipeline* dpipe = s_nr.scene_tex_sub_decal_pipe;
                openpete_mod_material_gpu_t dmg;
                int dslot_ok = 0;
                if (tag_split_on() && s0_decal_tag) {
                    int ntri1 = txg_count[1] / 3, uniform = 1;
                    for (int q = 1; q < ntri1; q++)
                        if (scene[s_txscene[base + q]].tag != s0_decal_tag) { uniform = 0; break; }
                    if (uniform &&
                        openpete_mods_material_resolve(s0_decal_tag, &dmg) >= 0) {
                        void** ds = dmg.pipeline_slot[OPENPETE_MATERIAL_PIPE_DECAL];
                        if (*ds == NULL) {
                            rhi_pipeline_desc dd = {
                                .vert_spv = "shadow_decal_tex.vert.spv",
                                .frag_spv = "shadow_decal_tex.frag.spv",
                                .blend = RHI_BLEND_SUBTRACT,
                                .depth_test = false,
                                .mod_pipeline = true,
                                .push_const_size = sizeof(shadow_decal_tex_pc),
                            };
                            rhi_pipeline* np = rhi_create_pipeline_spv(
                                s_nr.dev, &dd, dmg.vert_spv, dmg.vert_words,
                                dmg.frag_spv, dmg.frag_words);
                            *ds = np ? (void*)np : PFX_PIPE_FAILED;
                            if (!np)
                                fprintf(stderr, "[native] material: decal pipeline "
                                                "build FAILED for %s\n", dmg.owner);
                        }
                        if (*ds != PFX_PIPE_FAILED) { dpipe = (rhi_pipeline*)*ds; dslot_ok = 1; }
                    }
                }
                bind_scene_pipe(dpipe);
                rhi_bind_depth_sampler(s_nr.dev, s_nr.opaque_depth, 0); /* set 0 */
                rhi_bind_vram2(s_nr.dev, s_nr.vram_tex);                /* set 1 */
                if (dslot_ok && s_mat_ubo)
                    rhi_ubo_bind(s_nr.dev, s_mat_ubo, 0,
                                 dmg.uniform_slot > 0 ? 1 + dmg.uniform_slot : 0);
                rhi_push_constants(s_nr.dev, &pc, sizeof pc);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                rhi_draw(s_nr.dev, (uint32_t)txg_count[1], (uint32_t)txg_first[1]);
            } else {
                bind_scene_pipe(g->tex);
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
                /* A group drawing through the stencil-MARK variant (hole-fill
                 * coverage) overrides via the material's MARK twin so
                 * overridden pixels still mark. The alpha/additive groups take
                 * plain overrides at their own blend class.
                 *
                 * gi==1 (moby shadows) stays stock: its preferred path is the
                 * depth-aware decal below, a different shader contract
                 * (shadow_decal_tex, set 0 = pre-pass depth) whose FRAGMENT
                 * stage owns occlusion — the very stage a material replaces.
                 * Allowing the override only on the non-decal fallback would
                 * make a mod's reach depend on the host's depth-sampling
                 * support, so neither path takes it. */
                int mat_ok = (gi == 1) ? 0
                           : (g->tex == s_nr.scene_tex_mark_pipe ? 2 : 1);
                draw_tex_runs(txg_first[gi], txg_count[gi], tx_start, scene, g->tex, hd,
                              mat_ok, g->blend);
            }
        }
        /* ACTOR_FRONT (rescue actors) at the normal actor layer — only when NO
         * cutscene is active (a stale g_DragonCutscene pointer can still match a
         * live moby outside states 1..6). During a cutscene they are front-passed
         * above (gi==3), so skip them here then. Same pipelines as the ACTOR
         * group: outside a cutscene this is pixel-identical to leaving them ACTOR. */
        if (gi == 2 && !actor_front) {
            /* ACTOR opaques also stencil-MARK their coverage when hole-fill is
             * on: the level authors cut HP ground holes under static decor
             * (dragon pedestal discs) — og never draws ground there, the moby
             * always covers it. Without the mark the FILL below reads those
             * pixels as uncovered and paints the coarse LP backing OVER the
             * moby (the LP surface sits nearer than the buried disc rim, so
             * depth can't save it). Mark pipes are raster-identical twins. */
            rhi_pipeline* apipe = holefill_on ? s_nr.gouraud_mark_pipe : s_nr.gouraud_pipe;
            rhi_pipeline* atex  = holefill_on ? s_nr.scene_tex_mark_pipe : s_nr.scene_tex_pipe;
            rhi_pipeline* acut  = holefill_on ? s_nr.hd_cutout_mark_pipe : s_nr.hd_cutout_pipe;
            if (apipe && ut_count[14] > 0) {
                bind_scene_pipe(apipe);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                rhi_draw(s_nr.dev, (uint32_t)ut_count[14], (uint32_t)ut_first[14]);
            }
            if (txg_count[9] > 0 && s_nr.vram_tex && atex) {
                bind_scene_pipe(atex);
                rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                draw_tex_runs(txg_first[9], txg_count[9], tx_start, scene,
                              atex, acut, holefill_on ? 2 : 1, 0);
            }
        }
        /* Terrain sector-mesh GPU cache: the retained WORLD terrain (unclaimed
         * sectors) draws in the same slot as the CPU-fed WORLD group — opaque
         * depth-write, stencil-MARK when hole-fill is armed (identical raster
         * roles; the vertex stage owns the per-face verdicts). */
        if (gi == 0 && openpete_tercache_frame_open()) {
            openpete_tercache_clip aff; tercache_clip_params(&aff);
            openpete_tercache_draw(OPTC_DRAW_WORLD, &aff, s_nr.vram_tex, holefill_on,
                                   s_nr.terrain_key_depth, s_nr.moby_key_depth);
        }
        /* WORLD_FILL — terrain LP hole-fill (W2.6-c): depth-tested at TRUE LP
         * depth, gated on stencil==0 so it can only paint pixels no opaque
         * coverage MARK claimed (LP↔HP T-junction seams — where the sky shows
         * through). Runs at gi==2 AFTER the ACTOR opaques so authored HP holes
         * hidden under static mobys (pedestal discs) stay covered by the moby,
         * not repainted with LP; still before the portal sequence below, which
         * clears stencil. (Under the dragon-rescue actor_front reorder the
         * actors draw at gi==3 and don't mark — cutscene draw lists carry no
         * fill-backed decor, so the seam fill stays correct there.) */
        if (gi == 2 && holefill_on && ut_count[12] > 0) {
            /* (The former sky-fan stencil pre-mark is retired with the
             * plane-cover rule: fills that land inside an aperture draw at
             * TRUE depth behind the opening plane, and the preview — testing
             * at plane − M — repaints over them like any other covered
             * opaque.) */
            bind_scene_pipe(s_nr.gouraud_fill_pipe);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, (uint32_t)ut_count[12], (uint32_t)ut_first[12]);
        }
        /* Terrain-cache LP hole-fill twin: stencil TEST_ZERO at true LP depth,
         * same slot as the scene-fed fill (before the portal stencil clear). */
        if (gi == 2 && holefill_on && openpete_tercache_frame_open()) {
            openpete_tercache_clip aff; tercache_clip_params(&aff);
            openpete_tercache_draw(OPTC_DRAW_FILL, &aff, s_nr.vram_tex, 0,
                                   s_nr.terrain_key_depth, s_nr.moby_key_depth);
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
        /* Terrain-cache blend classes ride their groups' slots: ABR-0 alpha
         * with the ALPHA group (gi==4), ABR-1 with ADDITIVE (gi==5, before
         * the subtract-world slot below — og's smoke darkens the glow). */
        if (gi == 4 && openpete_tercache_frame_open()) {
            openpete_tercache_clip aff; tercache_clip_params(&aff);
            openpete_tercache_draw(OPTC_DRAW_ALPHA, &aff, s_nr.vram_tex, 0,
                                   s_nr.terrain_key_depth, s_nr.moby_key_depth);
        }
        if (gi == 5 && openpete_tercache_frame_open()) {
            openpete_tercache_clip aff; tercache_clip_params(&aff);
            openpete_tercache_draw(OPTC_DRAW_ADD, &aff, s_nr.vram_tex, 0,
                                   s_nr.terrain_key_depth, s_nr.moby_key_depth);
        }
        if (gi == 5) {
            { static int s_swd = -1;
              if (s_swd < 0) { const char* e = getenv("OPENPETE_PLY_VBDBG"); s_swd = (e && *e && *e != '0'); }
              if (s_swd) fprintf(stderr, "[subworld] ut13=%d txg8=%d utpipe=%d txpipe=%d vram=%d\n",
                                 ut_count[13], txg_count[8],
                                 s_nr.gouraud_sub_ztest_pipe != NULL,
                                 s_nr.scene_tex_sub_pipe != NULL, s_nr.vram_tex != NULL); }
            if (ut_count[13] > 0 && s_nr.gouraud_sub_ztest_pipe) {
                bind_scene_pipe(s_nr.gouraud_sub_ztest_pipe);
                rhi_set_depth_bias(s_nr.dev, 0.0f, 0.0f, 0.0f);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                rhi_draw(s_nr.dev, (uint32_t)ut_count[13], (uint32_t)ut_first[13]);
            }
            if (txg_count[8] > 0 && s_nr.vram_tex && s_nr.scene_tex_sub_pipe) {
                bind_scene_pipe(s_nr.scene_tex_sub_pipe);
                rhi_set_depth_bias(s_nr.dev, 0.0f, 0.0f, 0.0f);
                rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                /* SD only (like the shadow group): the sub pipes ride the
                 * caller's dynamic depth-bias state, which an HD rebind
                 * would invalidate. */
                draw_tex_runs(txg_first[8], txg_count[8], tx_start, scene,
                              s_nr.scene_tex_sub_pipe, NULL, 1, 3);
            }
            /* Terrain-cache ABR-2 terrain faces: same SUBTRACT-WORLD slot. */
            if (openpete_tercache_frame_open()) {
                openpete_tercache_clip aff; tercache_clip_params(&aff);
                openpete_tercache_draw(OPTC_DRAW_SUB, &aff, s_nr.vram_tex, 0,
                                   s_nr.terrain_key_depth, s_nr.moby_key_depth);
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
                bind_scene_pipe(s_nr.portal_mask_pipe);
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
                /* Sky colours: material segments where a selector claimed
                 * them, stock elsewhere. Portal sky is opaque but depth-write
                 * OFF (it must not occlude what shows through the aperture),
                 * so overrides build that variant rather than the plain
                 * opaque one. */
                int drawn = 0;
                if (tag_split_on()) {
                    for (int sg = 0; sg < pseg_n; sg++) {
                        if (pseg[sg].run != order[oi] || pseg[sg].count <= 0) continue;
                        rhi_pipeline* pipe = NULL;
                        openpete_mod_material_gpu_t mg;
                        if (pseg[sg].tag &&
                            openpete_mods_material_resolve(pseg[sg].tag, &mg) >= 0) {
                            void** slot = mg.pipeline_slot[4];
                            if (*slot == NULL) {
                                rhi_pipeline_desc md = {
                                    .vert_spv = "psx_scene.vert.spv",
                                    .frag_spv = "psx_scene.frag.spv",
                                    .blend = RHI_BLEND_NONE,
                                    .depth_test = true,
                                    .depth_write_off = true,
                                    /* The aperture mask is what confines the
                                     * destination sky to the portal's opening;
                                     * an override that drops it paints sky over
                                     * the surrounding wall. Reshade-only means
                                     * inheriting the group's stencil state the
                                     * same way it inherits blend and depth. */
                                    .stencil = RHI_STENCIL_TEST,
                                    .material_pipeline = true,
                                    .push_const_size = sizeof(psx_material_hd),
                                };
                                rhi_pipeline* np = rhi_create_pipeline_spv(
                                    s_nr.dev, &md, mg.vert_spv, mg.vert_words,
                                    mg.frag_spv, mg.frag_words);
                                *slot = np ? (void*)np : PFX_PIPE_FAILED;
                            }
                            if (*slot != PFX_PIPE_FAILED) {
                                pipe = (rhi_pipeline*)*slot;
                                bind_scene_pipe(pipe);
                                rhi_bind_texture_set(s_nr.dev, s_nr.vram_tex, 0);
                                if (s_nr.mat_pack_dummy)
                                    rhi_bind_texture_set(s_nr.dev, s_nr.mat_pack_dummy, 1);
                                bind_material_keys();
                                if (s_mat_ubo)
                                    rhi_ubo_bind(s_nr.dev, s_mat_ubo, 0,
                                                 mg.uniform_slot > 0 ? 1 + mg.uniform_slot : 0);
                                psx_material_hd zm;
                                memset(&zm, 0, sizeof zm);
                                rhi_push_constants(s_nr.dev, &zm, sizeof zm);
                            }
                        }
                        if (!pipe) bind_scene_pipe(s_nr.gouraud_noz_pipe);
                        rhi_draw(s_nr.dev, (uint32_t)pseg[sg].count,
                                 (uint32_t)pseg[sg].first);
                        drawn += pseg[sg].count;
                    }
                }
                if (drawn < sk->count) {
                    /* No segments (nothing armed), or the segment table
                     * overflowed — draw the run stock. */
                    bind_scene_pipe(s_nr.gouraud_noz_pipe);
                    rhi_draw(s_nr.dev, (uint32_t)(drawn ? sk->count - drawn : sk->count),
                             (uint32_t)(drawn ? sk->first + drawn : sk->first));
                }
            }
            /* PORTAL_GUARD: near-band terrain protection redraw — the PSX
             * mask-bit mirror (see the category doc in the header). Drawn
             * once after ALL portal groups; the sky wrote no depth, so the
             * guard's normal LEQUAL re-wins exactly its own pixels. */
            if (ut_count[10] > 0 && s_nr.gouraud_pipe) {
                bind_scene_pipe(s_nr.gouraud_pipe);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                rhi_draw(s_nr.dev, (uint32_t)ut_count[10], (uint32_t)ut_first[10]);
            }
            if (txg_count[7] > 0 && s_nr.vram_tex && s_nr.scene_tex_pipe) {
                bind_scene_pipe(s_nr.scene_tex_pipe);
                rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
                draw_tex_runs(txg_first[7], txg_count[7], tx_start, scene, s_nr.scene_tex_pipe,
                              s_nr.hd_cutout_pipe, 0, 0);
            }
        }
        /* GS_LevelTransition flyby ONLY (hud_under): draw the flat HUD layer here —
         * after the ACTOR group (gi==2) but BEFORE the PLAYER group (gi==3) — so the
         * fly-in Spyro draws in front of the revealed "THE ADVENTURE BEGINS" text.
         * In every other state the flat HUD draws OVER the player (after the loop). */
        if (gi == 2 && hud_under)
            draw_hud_flat_layer(scene, scene_n, ut_first, ut_count, tx_start, txg_first[6], txg_count[6]);
        /* (Dragon-rescue front pass now runs at the TOP of gi==3, before PLAYER —
         * per-pixel depth punch, so it never wipes terrain depth for the
         * transparent groups below, and Spyro sorts against the dragon by
         * real Z.) */
        rhi_gpu_zone_end(s_nr.dev);
        /* Deferred backdrops: right after the WORLD opaques so early-Z spares
         * every covered pixel; still before the shadow/actor/portal layers,
         * which depth-test against the z=1.0 they write, exactly as before. */
        if (gi == 0 && s_backdrop_defer)
            draw_backdrop_fills(bg, ut_first, ut_count, 1);
    }

    /* Default: the flat 2D HUD/text draws OVER the whole scene incl. the player
     * (Spyro behind the HUD/text). Only the GS_LevelTransition flyby puts it under
     * the player (handled at gi==2 above). */
    rhi_gpu_zone_begin(s_nr.dev, "sc.hud");
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
        bind_scene_pipe(s_nr.gouraud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count[9], (uint32_t)ut_first[9]);
    }

    /* OVERLAY layer (HUD-OT 2D flat quads — the cutscene letterbox bars): drawn
     * after every scene group, over everything incl. the player, depth-test off
     * (gouraud_hud_pipe). Before the fade, matching the PSX HUD-OT order (the fade
     * F4 is linked after the border, so it draws on top of the bars). */
    if (s_nr.gouraud_hud_pipe && ut_count[7] > 0) {
        bind_scene_pipe(s_nr.gouraud_hud_pipe);
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
            bind_scene_pipe(fp);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, 6, (uint32_t)fade_first);
        }
    }
    rhi_gpu_zone_end(s_nr.dev);   /* sc.hud */
    NR_PROF_END();   /* cmp.record */

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
 * clean). Toggle at runtime via the SHOW FPS row in the settings menu (O), the
 * Performance stats row in the M overlay, or --perf-stats at launch.
 *
 * The overlay has three levels (openpete_perf_stats.h): off, the FPS counter,
 * and the counter plus a pacing readout. Only the level lives here — the
 * renderer needs it to gate the draw and to size the supersample region — and
 * the numbers themselves are built by perf_stats.c. */
static int s_perf_stats = OPENPETE_PERF_FPS;   /* level (default: FPS only) */
/* The level a bare ON restores. The O menu's SHOW FPS row is a two-state
 * mirror of a three-state setting, so turning it off and on again must not
 * silently demote a user who picked the detailed readout in the M overlay. */
static int s_perf_stats_last = OPENPETE_PERF_FPS;
static int s_overlay_live = 0;   /* 1 only during the live present's compose_variant */
static uint64_t s_fps_win_start = 0;  /* rolling 0.5s FPS window */
static int      s_fps_frames    = 0;
static int      s_fps_value     = 0;

static int s_fps_in_record_pref = -1;  /* --record-fps-overlay: -1 unset, 0/1 explicit */
static int s_fps_in_record = -1;       /* resolved lazily (pref, else env); setter resets */
void openpete_native_set_fps_in_record(int pref) {
    s_fps_in_record_pref = pref;
    s_fps_in_record = -1;   /* re-resolve on the next recorded frame (live toggle) */
}

void openpete_native_set_perf_stats(int level) {
    if (level < OPENPETE_PERF_OFF) level = OPENPETE_PERF_OFF;
    if (level >= OPENPETE_PERF_LEVELS) level = OPENPETE_PERF_LEVELS - 1;
    s_perf_stats = level;
    if (level != OPENPETE_PERF_OFF) s_perf_stats_last = level;
}
int  openpete_native_perf_stats_level(void) { return s_perf_stats; }

void openpete_native_set_fps_overlay(int on) {
    openpete_native_set_perf_stats(on ? s_perf_stats_last : OPENPETE_PERF_OFF);
}
int  openpete_native_fps_overlay_enabled(void) { return s_perf_stats != OPENPETE_PERF_OFF; }
int  openpete_native_fps_value(void) { return s_fps_value; }

/* Live presentation setters (in-game settings menu). Display-only: they touch
 * only s_nr (renderer state), consumed by the next native present; never
 * gameplay RAM. */
static float  s_fov_deg;          /* 0 = default */
static double s_fov_tan_pending;  /* tan(fov/2), 0 at the default; set live */
static double s_fov_tan;          /* latched at scene begin */
static void fov_latch(void) { s_fov_tan = s_fov_tan_pending; }

void openpete_native_set_fov(float deg) {
    if (deg > 0.0f && deg < 1.0f) deg = 1.0f;
    if (deg > 179.0f) deg = 179.0f;
    if (deg <= 0.0f) deg = 0.0f;
    if (deg == s_fov_deg) return;
    s_fov_deg = deg;
    s_fov_tan_pending = deg > 0.0f ? tan((double)deg * 0.5 * 3.14159265358979323846 / 180.0) : 0.0;
    /* The occlusion grid was built at the old H: every cached rect is wrong. */
    openpete_native_occl_suspend(2);
}
float openpete_native_fov_deg(void) { return s_fov_deg; }
int   openpete_native_visible_rows(void) { return s_nr.clip_h > 0 ? s_nr.clip_h : 224; }
/* The projection the feeders last built a view from: the label's inputs. */
static double s_fov_lbl_H, s_fov_lbl_yrow;
void openpete_native_fov_note_canon(double H_canon, double y_row) { s_fov_lbl_H = H_canon; s_fov_lbl_yrow = y_row; }
int openpete_native_fov_angles(double* v_deg, double* h_deg) {
    const double H = s_fov_lbl_H > 0.0 ? s_fov_lbl_H : 341.0, yr = s_fov_lbl_yrow > 0.0 ? s_fov_lbl_yrow : 0.625;
    const double pi = 3.14159265358979323846;
    double v = s_fov_deg > 0.0f ? (double)s_fov_deg
             : 2.0 * atan((0.5 * (double)openpete_native_visible_rows()) / (yr * H)) * 180.0 / pi;
    double s = s_fov_tan_pending > 0.0 ? ((0.5 * (double)openpete_native_visible_rows()) / (yr * H)) / s_fov_tan_pending : 1.0;
    double h = 2.0 * atan(256.0 * (double)openpete_native_widescreen_factor() / (H * s)) * 180.0 / pi;
    if (v_deg) *v_deg = v;
    if (h_deg) *h_deg = h;
    return s_fov_lbl_H > 0.0;
}
double openpete_native_fov_h_scale(double H_canon, double y_row) {
    if (s_fov_tan <= 0.0 || H_canon <= 0.0 || y_row <= 0.0) return 1.0;
    double tref = (0.5 * (double)openpete_native_visible_rows()) / (y_row * H_canon);
    return tref / s_fov_tan;
}
double openpete_native_fov_horizontal_deg(double H_canon, double y_row) {
    double H_eff = H_canon * openpete_native_fov_h_scale(H_canon, y_row);
    if (H_eff <= 0.0) return 0.0;
    return 2.0 * atan(256.0 * (double)openpete_native_widescreen_factor() / H_eff) * 180.0 / 3.14159265358979323846;
}

void openpete_native_set_aspect(float w_over_h) {
    s_nr.aspect_wh = (w_over_h > 0.0f) ? w_over_h : 0.0f;
    /* Fixed aspect picked live (O menu): resize the window width to match,
     * like --aspect does at launch — the render rect then fills the window
     * exactly, no bars. Skipped when fullscreen (can't resize — the fixed
     * aspect letterboxes/pillarboxes instead) and under AUTO (0 — the render
     * follows the window, which stays as the user left it). ref_w/ref_h is
     * the crop-corrected 4:3 display shape (synthesized in pure vk), so the
     * target window shape is rect = wf × ref shape — the same rect
     * update_layout fits, making the fit exact. Logical window units on both
     * sides of the resize so HiDPI scales cancel. */
    if (s_nr.aspect_wh > 0.0f && s_nr.window &&
        !(SDL_GetWindowFlags(s_nr.window) & SDL_WINDOW_FULLSCREEN)) {
        float wf = s_nr.aspect_wh / (4.0f / 3.0f);
        if (wf < 1.0f) wf = 1.0f;
        float rect_wh = (s_nr.ref_w > 0 && s_nr.ref_h > 0)
            ? wf * (float)s_nr.ref_w / (float)s_nr.ref_h
            : wf * (4.0f / 3.0f) * (240.0f / 224.0f); /* pre-first-frame fallback */
        int lw = 0, lh = 0;
        SDL_GetWindowSize(s_nr.window, &lw, &lh);
        int new_w = (int)((float)lh * rect_wh + 0.5f);
        if (lh > 0 && new_w > 0 && new_w != lw)
            SDL_SetWindowSize(s_nr.window, new_w, lh);
    }
    /* update_layout()'s change-cache keys on window/ref/disp sizes, NOT aspect,
     * so force it to recompute the fitted panel rect on the next present. */
    s_nr.layout_win_w = -1;
    OPL_INFO_F("native", "aspect -> %.4f (%s)", s_nr.aspect_wh,
               s_nr.aspect_wh > 0.0f ? "Hor+ widen (fixed)" : "AUTO (follow window)");
}

float openpete_native_get_aspect(void) { return s_nr.aspect_wh; }

void openpete_native_set_ssaa(int factor) {
    if (factor < 1) factor = 1;
    else if (factor > OPENPETE_SSAA_MAX) factor = OPENPETE_SSAA_MAX;
    s_nr.ssaa = factor;   /* ensure_ss_rtt() reallocs the scene RT on the next present */
    OPL_INFO_F("native", "ssaa -> %dx (effective factor capped to GPU max next present)", factor);
}

int openpete_native_get_ssaa(void) { return (s_nr.ssaa > 0) ? s_nr.ssaa : 1; }

void openpete_native_set_vsync(int on) {
    if (s_nr.dev) rhi_set_vsync(s_nr.dev, on ? 1 : 0);   /* swapchain rebuilds next frame */
    OPL_INFO_F("native", "vsync -> %s", on ? "ON (FIFO)" : "OFF (immediate/mailbox)");
}

int openpete_native_get_vsync(void) {
    return s_nr.dev ? rhi_get_vsync(s_nr.dev) : -1;
}

int openpete_native_vertex_snap_active(void) {
    return s_vertex_snap;   /* feeders branch on this for snap-consistent emits */
}

void openpete_native_scene_set_presnap(int on) {
    s_cur_presnap = on ? 1 : 0;   /* sticky, like set_tag — clear after the feed */
}

void openpete_native_set_vertex_precision(int integer_snap) {
    s_vertex_snap = integer_snap ? 1 : 0;   /* consumed by the next add_tri_grouped feed */
    OPL_INFO_F("native", "vertex-precision -> %s",
               s_vertex_snap ? "integer (PSX wobble)" : "subpixel (smooth)");
}

void openpete_native_set_raster_quantize(int mult) {
    s_raster_mult = mult > 0 ? mult : 0;     /* consumed at the next SCENE split */
    if (s_raster_mult)
        OPL_INFO_F("native", "raster-quantize -> %dx (%dx%d grid, %d visible lines)",
                   s_raster_mult, 512 * s_raster_mult, 240 * s_raster_mult,
                   224 * s_raster_mult);
    else
        OPL_INFO_F("native", "raster-quantize -> off");
}

int openpete_native_get_raster_quantize(void) { return s_raster_mult; }

/* --dither live toggle: the pass enable is checked per present at the SCENE
 * postfx split, so no pipeline rebuild is involved — the flag alone is the
 * whole switch. */
void openpete_native_set_dither(int on) {
    s_dither = on ? 1 : 0;
    OPL_INFO_F("native", "dither -> %s",
               s_dither ? "on (PS1 15-bit colour + 4x4 dither)" : "off");
}

int openpete_native_get_dither(void) { return s_dither; }

/* --texture-mapping live toggle: the mode is a vertex spec constant baked
 * into every scene pipeline, so the setter only QUEUES the change; it takes
 * effect at the next present boundary via texmap_apply_pending(), which
 * re-bakes all live pipelines in place (mod material/mesh caches included —
 * every rhi_pipeline* keeps its identity) under a one-time device idle. */
void openpete_native_set_texture_mapping(int affine) {
    s_texmap_pending = affine ? 1 : 0;
}

int openpete_native_get_texture_mapping(void) { return s_texmap_affine; }

static void texmap_apply_pending(void) {
    if (s_texmap_pending == s_texmap_affine || !s_nr.dev) return;
    rhi_set_affine_uv(s_texmap_pending);
    int fails = rhi_rebuild_pipelines(s_nr.dev);
    s_texmap_affine = s_texmap_pending;
    OPL_INFO_F("native", "texture-mapping -> %s (pipelines re-baked)",
               s_texmap_affine ? "affine (PSX screen-linear)" : "perspective");
    if (fails)
        OPL_ERROR_F("native", "texture-mapping: %d pipeline(s) failed to rebuild (kept previous mode)", fails);
}

/* Key-contest scoping (MOBY_TRUESCOPE, fragment spec-constant 3), DEFAULT ON:
 * a moby og key claims a pixel only when it also beats the covering
 * fragment's own TRUE view-Z. og's authored OT compounding (moby pull +
 * terrain push) paints mobys through terrain genuinely in front of them —
 * visible from ordinary gameplay angles (chests through the ledge lip they
 * sit behind), not just off-rail — so true depth is the standing rule and
 * OPENPETE_MOBY_TRUESCOPE=0 restores og's painter for A/B. Cutscene actors
 * are unaffected either way: the ACTOR_FRONT class never emits keys, and
 * their authored inside-terrain look (Delvin's wing through the tower) is
 * carried by the ACTOR_FRONT depth-punch pass. Same re-bake shape as
 * texture-mapping. */
static int s_moby_truescope = 1;   /* currently-baked value (pipelines start at 1) */
static void truescope_apply_pending(void) {
    if (!s_nr.dev) return;
    const char* e = getenv("OPENPETE_MOBY_TRUESCOPE");
    int want = (e && *e) ? (*e != '0') : 1;
    if (want == s_moby_truescope) return;
    rhi_set_moby_truescope(want);
    int fails = rhi_rebuild_pipelines(s_nr.dev);
    s_moby_truescope = want;
    OPL_INFO_F("native", "moby key contest -> %s (pipelines re-baked)",
               want ? "true-depth scoped" : "og compound painter (A/B)");
    if (fails)
        OPL_ERROR_F("native", "moby-truescope: %d pipeline(s) failed to rebuild (kept previous mode)", fails);
}

/* Untextured lighting view — same queued-rebuild shape as texture-mapping. */
void openpete_native_set_notex(int on) {
    s_notex_pending = on ? 1 : 0;
}

int openpete_native_get_notex(void) { return s_notex; }

static void notex_apply_pending(void) {
    if (s_notex_pending == s_notex || !s_nr.dev) return;
    rhi_set_notex(s_notex_pending);
    int fails = rhi_rebuild_pipelines(s_nr.dev);
    s_notex = s_notex_pending;
    OPL_INFO_F("native", "notex -> %s (pipelines re-baked)",
               s_notex ? "untextured lighting view" : "textured");
    if (fails)
        OPL_ERROR_F("native", "notex: %d pipeline(s) failed to rebuild (kept previous mode)", fails);
}

/* Overlay geometry built from Spyro's own 3D HUD text mobys (see
 * openpete_native_render.h). The game-side emitter (text_moby_native) pushes a
 * triangle list each frame; draw_perf_overlay draws it on the live present, or
 * nothing when empty (the game side caches the glyph models, so this is empty
 * only before the font has ever loaded — there is no bitmap fallback). Carries
 * the performance-stats readout; reusable for other text overlays. */
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
 * include it (a HUD mod's drawing is content, not chrome). Screenshot
 * dumps include it too: the windowed -vk is a readback of the presented
 * frame, and the recompose paths call draw_mod_overlay themselves. */
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

float openpete_native_overlay_px_per_unit(void) {
    return (s_nr.win_h > 0) ? (float)s_nr.win_h : 0.0f;
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

/* ---- OSD toast geometry channel ----------------------------------------
 * The one publish in this file that is SYNCHRONIZED, and deliberately so.
 * Its siblings above are plain memcpys into file statics with the count
 * stored separately, which is survivable for them: the perf readout rebuilds
 * only when its text changes and the menu builder runs only while the menu is
 * open, so a torn push is a rare one-present artifact. The OSD re-emits on
 * every sim tick a toast is live, and its table is what the compositor
 * resolves toast ids through — a half-copied table doesn't smear a glyph, it
 * points a quad at another toast's slot. Geometry and table therefore move
 * together under one lock. */
static SDL_Mutex        *s_osd_mx;
static openpete_hud_vert s_osd_glyph_verts[OSD_GLYPH_MAX];
static int               s_osd_glyph_n = 0;
static openpete_osd_slot_t s_osd_slots[OPENPETE_OSD_MAX];
static int               s_osd_slot_n = 0;

static SDL_Mutex *osd_pub_mx(void) {
    if (!s_osd_mx) s_osd_mx = SDL_CreateMutex();
    return s_osd_mx;
}

void openpete_native_set_osd_glyphs(const openpete_hud_vert *verts, int nverts,
                                    const openpete_osd_slot_t *slots, int nslots) {
    if (nverts < 0) nverts = 0;
    if (nverts > OSD_GLYPH_MAX) nverts = OSD_GLYPH_MAX;
    if (nslots < 0) nslots = 0;
    if (nslots > OPENPETE_OSD_MAX) nslots = OPENPETE_OSD_MAX;
    SDL_LockMutex(osd_pub_mx());
    if (nverts > 0 && verts)
        memcpy(s_osd_glyph_verts, verts, (size_t)nverts * sizeof(verts[0]));
    s_osd_glyph_n = nverts;
    if (nslots > 0 && slots)
        memcpy(s_osd_slots, slots, (size_t)nslots * sizeof(slots[0]));
    s_osd_slot_n = nslots;
    SDL_UnlockMutex(osd_pub_mx());
}

static void osd_slot_px(int *w, int *h) {
    const uint64_t v = s_nr.osd_slot_px;   /* one read: both halves from one publish */
    if (w) *w = (int)(uint32_t)v;
    if (h) *h = (int)(uint32_t)(v >> 32);
}

void openpete_native_osd_slot_extent(float *w, float *h) {
    /* The grid's slot extent in overlay units, quantized to the 1x pixel
     * counts the target was actually built from. The builder lays out against
     * these, so its slot origins line up exactly with the texel blocks the
     * resolve shader averages — a slot origin off the OSD_SS_FACTOR grid would
     * make edge blocks average in the neighbouring slot's texels. */
    const float ph = (s_nr.win_h > 0) ? (float)s_nr.win_h : 1.0f;
    int pw, phh;
    osd_slot_px(&pw, &phh);
    if (w) *w = (pw  > 0) ? (float)pw  / ph : OSD_SLOT_W;
    if (h) *h = (phh > 0) ? (float)phh / ph : OSD_SLOT_H;
}

/* Draw the overlay built from Spyro's own 3D HUD text mobys, fed each frame by
 * the game side (text_moby_native -> openpete_native_set_overlay_glyphs). Verts
 * arrive in window-height-fraction overlay space (origin top-left, +y down) with
 * a depth in [0,1); map to NDC (x via win_h so glyphs stay square) and draw
 * depth-tested through the opaque gouraud pipe after a local depth clear, so the
 * glyphs z-sort among their own faces yet sit on top of the whole frame — the
 * same isolation trick as the in-game HUD3D pass. Returns true if it drew. */
static bool draw_glyph_list_3d(const openpete_hud_vert *verts, int nverts,
                               rhi_buffer *vb, int cap,
                               float ax, float ay, float ox, float oy) {
    if (nverts < 3 || !verts || !vb || !s_nr.gouraud_pipe) return false;
    static rhi_vertex buf[OSD_GLYPH_MAX > OVL_MAX_VERTS ? OSD_GLYPH_MAX : OVL_MAX_VERTS];
    if (cap > (int)(sizeof buf / sizeof buf[0])) cap = (int)(sizeof buf / sizeof buf[0]);
    int n = 0;
    for (int i = 0; i < nverts && n < cap; i++) {
        const openpete_hud_vert *v = &verts[i];
        /* Glyph coords are win_h-relative; ax/ay map them into the target's NDC.
         * Offscreen 1×: ax=2·win_h/win_w, ay=2. Overlay SS: ax=2/ovl_region_w(),
         * ay=2/ovl_region_h() — both keep glyphs square (ax/ay carry the same
         * win_h scale through the target's pixel dims). (ox,oy) shifts the origin
         * in overlay space — the 1× fallback path uses it to land on the block's
         * anchored rect; the SS prepass passes (0,0), since there the composite
         * quad is what moves and the glyphs fill their region. */
        buf[n].x = (v->x + ox) * ax - 1.0f;
        buf[n].y = 1.0f - (v->y + oy) * ay;
        buf[n].z = v->z; buf[n].u = 0.0f; buf[n].v = 0.0f;
        buf[n].rgba = v->rgba; buf[n].koff = 0.0f; buf[n].w = 0.0f;
        buf[n].sx = 0.0f; buf[n].sy = 0.0f;   /* unranked: rank select off */
        n++;
    }
    n -= n % 3;
    if (n <= 0) return false;
    rhi_update_buffer(vb, buf, (size_t)n * sizeof(rhi_vertex));
    rhi_reset_scissor(s_nr.dev);
    rhi_clear_depth(s_nr.dev);
    bind_scene_pipe(s_nr.gouraud_pipe);
    rhi_bind_vertex_buffer(s_nr.dev, vb);
    rhi_draw(s_nr.dev, (uint32_t)n, 0);
    return true;
}

/* The stats readout's own glyph list, through the shared draw above. */
static bool draw_overlay_glyphs_3d(float ax, float ay, float ox, float oy) {
    return draw_glyph_list_3d(s_overlay_glyph_verts, s_overlay_glyph_n,
                              s_nr.overlay_vb, OVL_MAX_VERTS, ax, ay, ox, oy);
}

/* Draw the mod draw-service overlay (rects + text glyphs) into the currently
 * open offscreen pass. Same overlay space + NDC mapping + depth-isolation
 * trick as draw_overlay_glyphs_3d, own vertex buffer, 1× direct draw (no
 * supersample pre-pass in v1). Runs in screenshot/record composes too — mod
 * HUD is content, not chrome. */
static void draw_mod_overlay(void) {
    const openpete_hud_vert *ov = s_ps_slot ? s_ps_slot->ovl : s_mod_overlay_verts;
    const int ovn = s_ps_slot ? s_ps_slot->ovl_n : s_mod_overlay_n;
    if (ovn < 3 || !s_nr.gouraud_pipe || !s_nr.mod_ovl_vb) return;
    if (s_nr.win_w <= 0 || s_nr.win_h <= 0) return;
    static rhi_vertex buf[MOD_OVL_MAX_VERTS];
    const float ax = 2.0f * (float)s_nr.win_h / (float)s_nr.win_w;
    const float ay = 2.0f;
    int n = 0;
    for (int i = 0; i < ovn && n < MOD_OVL_MAX_VERTS; i++) {
        const openpete_hud_vert *v = &ov[i];
        buf[n].x = v->x * ax - 1.0f;
        buf[n].y = 1.0f - v->y * ay;
        buf[n].z = v->z; buf[n].u = 0.0f; buf[n].v = 0.0f;
        buf[n].rgba = v->rgba; buf[n].koff = 0.0f; buf[n].w = 0.0f;
        buf[n].sx = 0.0f; buf[n].sy = 0.0f;   /* unranked: rank select off */
        n++;
    }
    n -= n % 3;
    if (n <= 0) return;
    rhi_update_buffer(s_nr.mod_ovl_vb, buf, (size_t)n * sizeof(rhi_vertex));
    rhi_reset_scissor(s_nr.dev);
    rhi_clear_depth(s_nr.dev);
    bind_scene_pipe(s_nr.gouraud_pipe);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.mod_ovl_vb);
    rhi_draw(s_nr.dev, (uint32_t)n, 0);
}

/* Whether chrome is allowed into a --record video: --record-fps-overlay, else
 * the OPENPETE_FPS_IN_RECORD env, else no. Resolved lazily and cached; the
 * setter clears the cache so a live toggle re-resolves. */
static int fps_in_record_resolved(void) {
    if (s_fps_in_record < 0) {
        if (s_fps_in_record_pref >= 0) {
            s_fps_in_record = s_fps_in_record_pref;      /* --record-fps-overlay wins */
        } else {
            const char *e = getenv("OPENPETE_FPS_IN_RECORD");  /* env fallback */
            s_fps_in_record = (e && *e && *e != '0') ? 1 : 0;
        }
    }
    return s_fps_in_record;
}

/* True when the stats overlay should render this frame: glyphs built, on, and
 * not suppressed by the record gating (kept out of recordings unless
 * OPENPETE_FPS_IN_RECORD lets the live look be captured for verification). */
static bool perf_overlay_should_draw(void) {
    if (s_perf_stats == OPENPETE_PERF_OFF || s_nr.win_w <= 0 || s_nr.win_h <= 0 || s_overlay_glyph_n < 3) return false;
    if (openpete_record_active() && !fps_in_record_resolved()) return false;
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
/* Overlay region for the perf readout: the rect the supersample target, the
 * composite quad and the glyph projection must ALL agree on, or the text lands
 * in a shape other than the one it was rasterized into. Exactly one side owns
 * it — the game-side builder, which is the side that can measure a string —
 * and pushes it here; the renderer reads it back and never re-derives it. */
/* Published as ONE 64-bit word rather than two floats. The builder writes it
 * on the sim thread and the compose reads it on the present thread, and a torn
 * PAIR — new width, stale height — would build the composite quad for a
 * different rect than the supersample target holds, landing the readout in a
 * shape it was not rasterized into. An aligned 64-bit store is single-copy on
 * every target this builds for; two independent float stores are not one
 * event. (Same class as the glyph publish below, found by auditing the
 * sim->present statics rather than by seeing it go wrong.) */
static uint64_t s_ovl_extent =
    ((uint64_t)0) ;   /* 0 = never pushed; the OVL_REGION_* fallback applies */

static float ovl_ext_unpack(uint64_t v, int hi, float fallback) {
    if (v == 0) return fallback;
    uint32_t bits = (uint32_t)(hi ? (v >> 32) : v);
    float f;
    memcpy(&f, &bits, sizeof f);
    return (f > 0.0f) ? f : fallback;
}

void openpete_native_set_overlay_extent(float w, float h) {
    if (!(w > 0.0f) || !(h > 0.0f)) return;
    uint32_t bw, bh;
    memcpy(&bw, &w, sizeof bw);
    memcpy(&bh, &h, sizeof bh);
    s_ovl_extent = (uint64_t)bw | ((uint64_t)bh << 32);
}
void openpete_native_get_overlay_extent(float *w, float *h) {
    const uint64_t v = s_ovl_extent;   /* one read: both halves from one publish */
    if (w) *w = ovl_ext_unpack(v, 0, OVL_REGION_W);
    if (h) *h = ovl_ext_unpack(v, 1, OVL_REGION_H);
}
/* The extent LATCHED for this present. overlay_layout_update reads the
 * published word once, at the top of the per-present resource pass, and
 * everything downstream — the target size, the ledger claim, the composite
 * quad, the prepass projection — reads the latch. Reading the publish at each
 * of those would let a mid-present push change the rect between the target
 * that was allocated and the quad that samples it, which is the same torn-pair
 * failure one field down. */
static float s_perf_rw = OVL_REGION_W, s_perf_rh = OVL_REGION_H;
static float ovl_region_w(void) { return s_perf_rw; }
static float ovl_region_h(void) { return s_perf_rh; }

/* Where the user wants the perf block. Live-settable (--perf-position, the
 * knob row); the placement itself is resolved once per present below. */
static int s_perf_anchor = OPENPETE_ANCHOR_TOP_LEFT;
void openpete_native_set_perf_anchor(int a) {
    if (a >= 0 && a < OPENPETE_ANCHOR_COUNT) s_perf_anchor = a;
}
int openpete_native_perf_anchor(void) { return s_perf_anchor; }

/* The present's placement ledger. Reset at the top of the per-present resource
 * pass, then claimed in order: the persistent perf block first, the transient
 * toasts after, so chrome that is always there owns its corner and the thing
 * that comes and goes stacks away from it. */
static openpete_anchor_ledger_t s_anchor_ledger;
static float s_perf_ox, s_perf_oy;   /* the perf block's placed top-left */

/* Resolve where every anchored chrome block sits THIS present. Runs once, in
 * the per-present resource pass before ensure_ovl_ss, so the composite quads
 * are built against the placement they will actually be drawn at. Claim order
 * is the policy: the persistent perf block first, then the OSD's toasts, so a
 * shared corner pushes the transient thing out of the way of the permanent
 * one rather than the reverse. */
/* Round an overlay-space length/offset to a whole output pixel. */
static float snap_px(float v, float win_h) {
    if (win_h <= 0.0f) return v;
    return (float)(int)(v * win_h + (v >= 0.0f ? 0.5f : -0.5f)) / win_h;
}

static void overlay_layout_update(void) {
    const float ph = (s_nr.win_h > 0) ? (float)s_nr.win_h : 0.0f;
    openpete_native_get_overlay_extent(&s_perf_rw, &s_perf_rh);
    /* Quantize the REGION to whole output pixels. The supersample target is
     * allocated at round(region * win_h) * N texels, so a region whose pixel
     * size is fractional makes the composite quad cover a slightly different
     * area than the target holds — the resolve's N×N blocks then straddle
     * output pixels and the glyph edges soften. */
    s_perf_rw = snap_px(s_perf_rw, ph);
    s_perf_rh = snap_px(s_perf_rh, ph);

    float xlo, xhi;
    openpete_anchor_usable_x(openpete_native_overlay_aspect(), &xlo, &xhi);
    openpete_anchor_reset(&s_anchor_ledger, xlo, xhi);
    if (perf_overlay_should_draw()) {
        /* Margin 0: the perf REGION already carries its own padding
         * (PERF_ORIGIN_X/Y inset the text inside it), so the block sits flush
         * against the window edge exactly as it did before it moved onto the
         * ledger. Charging it the standard margin too moved the readout ~5 px
         * inward, off the frame edge and onto the picture — which reads as a
         * loss of crispness, because olive-gold text over a bright sky has far
         * less contrast than the same text over the frame's dark border. */
        openpete_anchor_place(&s_anchor_ledger, s_perf_anchor,
                              ovl_region_w(), ovl_region_h(), 0.0f,
                              &s_perf_ox, &s_perf_oy);
        /* And quantize the PLACEMENT. This is the half that regressed when the
         * block moved onto the ledger: it used to sit at overlay (0,0), which
         * is pixel-aligned by construction, and the ledger's margin put it at
         * a fractional offset (0.012 * 480 = 5.76 px at the default window).
         * A fractional translation shifts EVERY source block off its output
         * pixel, which reads as a soft counter — the same 1:1
         * output-pixel-to-block rule the toast quads already follow. */
        s_perf_ox = snap_px(s_perf_ox, ph);
        s_perf_oy = snap_px(s_perf_oy, ph);
    }
}

/* (Re)create ovl_ss at OVL_SS_FACTOR× the overlay region and rebuild the
 * composite quad on a window resize. Creates GPU resources / waits idle — call
 * OUTSIDE a frame (alongside ensure_ss_rtt). No-op if the SS pipeline/quad
 * failed at init (draw_perf_overlay then falls back to the 1× path). */
static void ensure_ovl_ss(void) {
    if (!s_nr.ovl_resolve_pipe || !s_nr.ovl_quad_vb || s_nr.win_w <= 0 || s_nr.win_h <= 0) return;
    int rw = (int)(ovl_region_w() * (float)s_nr.win_h + 0.5f);
    int rh = (int)(ovl_region_h() * (float)s_nr.win_h + 0.5f);
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
            OPL_INFO_F("native", "stats overlay SS target %dx%d (%d×)", want_w, want_h, OVL_SS_FACTOR);
    }
    /* Composite quad — the overlay's screen rect in offscreen NDC. Left edge sits
     * at overlay-x = want_ox (0 = flush top-left; shifted right of the panel while
     * the menu is open, see perf_overlay_ox). Rect width is win_h-relative (glyph x
     * uses win_h), so its NDC width scales by win_h/win_w; UV 0..1 maps the whole
     * SS target (v0 top, top-down). Rebuilt on resize OR when the offset changes
     * (menu open/close) — never per-frame, so the wait_idle stays rare. */
    float want_ox = s_perf_ox, want_oy = s_perf_oy;
    float want_rw = ovl_region_w();
    float want_rh = ovl_region_h();
    if (s_nr.ovl_quad_win_w != s_nr.win_w || s_nr.ovl_quad_win_h != s_nr.win_h ||
        s_nr.ovl_quad_ox != want_ox || s_nr.ovl_quad_oy != want_oy ||
        s_nr.ovl_quad_rw != want_rw || s_nr.ovl_quad_rh != want_rh) {
        float ax = 2.0f * (float)s_nr.win_h / (float)s_nr.win_w;
        float rw_ndc = want_rw * ax;
        float rh_ndc = want_rh * 2.0f;
        float nx_left = want_ox * ax - 1.0f;
        float ny_top  = 1.0f - want_oy * 2.0f;
        rhi_vertex q[6];
        build_panel(q, nx_left + rw_ndc * 0.5f, ny_top - rh_ndc * 0.5f,
                    rw_ndc * 0.5f, rh_ndc * 0.5f, 0.0f, 0.0f, 1.0f, 1.0f);
        /* Single (non-ring) VB rewritten only on resize / anchor move: an
         * in-flight frame may still read it — fully sync first (like panels_vb).
         * The anchor moves on a menu open/close, a stats-level switch or a
         * user setting, never per present, so the idle stays rare. */
        rhi_wait_idle(s_nr.dev);
        rhi_update_buffer(s_nr.ovl_quad_vb, q, sizeof(q));
        s_nr.ovl_quad_win_w = s_nr.win_w; s_nr.ovl_quad_win_h = s_nr.win_h;
        s_nr.ovl_quad_ox = want_ox; s_nr.ovl_quad_oy = want_oy;
        s_nr.ovl_quad_rw = want_rw; s_nr.ovl_quad_rh = want_rh;
    }
}

/* Pre-pass: render the overlay glyphs at OVL_SS_FACTOR× into ovl_ss (cleared
 * transparent). Its own render-target pass, so call it BEFORE the offscreen pass
 * opens (no nested render passes). No-op unless the SS path is ready. */
static void prepare_perf_overlay_ss(void) {
    if (!ovl_ss_ready() || !perf_overlay_should_draw()) return;
    rhi_begin_render_target(s_nr.dev, s_nr.ovl_ss, 0.0f, 0.0f, 0.0f, 0.0f);  /* transparent */
    draw_overlay_glyphs_3d(2.0f / ovl_region_w(), 2.0f / ovl_region_h(), 0.0f, 0.0f);
    rhi_end_render_target(s_nr.dev, s_nr.ovl_ss);
}

/* Composite the stats overlay into the currently-open offscreen pass. SS path:
 * box-downsample + straight-alpha-blend the ovl_ss the pre-pass built. Fallback:
 * draw the glyphs directly at 1× (the original crisp-but-aliased path). */
static void draw_perf_overlay(void) {
    if (!perf_overlay_should_draw()) return;
    if (ovl_ss_ready()) {
        int factor = OVL_SS_FACTOR;
        rhi_reset_scissor(s_nr.dev);
        bind_scene_pipe(s_nr.ovl_resolve_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.ovl_ss);
        rhi_push_constants(s_nr.dev, &factor, sizeof(factor));
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.ovl_quad_vb);
        rhi_draw(s_nr.dev, 6, 0);
    } else {
        draw_overlay_glyphs_3d(2.0f * (float)s_nr.win_h / (float)s_nr.win_w, 2.0f,
                               s_perf_ox, s_perf_oy);
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
        gbuf[gn].rgba = v->rgba; gbuf[gn].koff = 0.0f; gbuf[gn].sx = 0.0f; gbuf[gn].sy = 0.0f;
        gbuf[gn].w = 0.0f;
        gn++;
    }
    gn -= gn % 3;
    if (gn <= 0) return;
    rhi_update_buffer(s_nr.menu_glyph_vb, gbuf, (size_t)gn * sizeof(rhi_vertex));
    rhi_reset_scissor(s_nr.dev);
    rhi_clear_depth(s_nr.dev);
    bind_scene_pipe(s_nr.gouraud_pipe);
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
        bind_scene_pipe(s_nr.gouraud_hud_sub_pipe);
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
        bind_scene_pipe(s_nr.ovl_resolve_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.menu_ss);
        rhi_push_constants(s_nr.dev, &factor, sizeof(factor));
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.menu_quad_vb);
        rhi_draw(s_nr.dev, 6, 0);
    } else {
        draw_menu_glyphs_3d(2.0f * (float)s_nr.win_h / (float)s_nr.win_w, 2.0f);
    }
}

/* --- OSD toast overlay ---------------------------------------------------
 * Same machinery as the settings menu — 3D gold glyphs and a lit gold frame
 * supersampled into an offscreen RGBA layer, then box-downsampled and
 * alpha-composited over the frame — but with N independently-faded boxes
 * instead of one panel. The differences are the fixed slot grid (so nothing
 * reallocates while toasts come and go) and the per-toast alpha, which rides
 * the composite quad's VERTEX COLOUR rather than a push constant: the resolve
 * shader already had the input declared and unused, and the quads are rebuilt
 * every present anyway, so per-toast fade costs literally nothing extra. */

/* Toasts actually COMPOSITED, cumulative. A capture-based gate that greened
 * with zero here proved nothing about the OSD; reported at shutdown so that
 * cannot pass unnoticed. */
static unsigned s_osd_composited = 0;
unsigned openpete_native_osd_composited(void) { return s_osd_composited; }

static bool osd_ss_ready(void) {
    return s_nr.ovl_resolve_pipe && s_nr.osd_quad_vb && s_nr.osd_panel_vb &&
           s_nr.osd_ss;
}

/* --record video is ground truth for mod-shader A/Bs, and the codebase has
 * already ruled that chrome stays out of it: the stats overlay suppresses
 * itself while recording unless --record-fps-overlay asks otherwise. Toasts
 * follow the same rule, with the same override — a recording of the OSD
 * itself is a legitimate thing to want, just not the default. */
static bool osd_should_draw(void) {
    if (!openpete_osd_enabled()) return false;
    if (openpete_record_active() && !fps_in_record_resolved()) return false;
    return true;
}

/* (Re)create the slot grid on a window resize. Sized from the MAXIMUM toast
 * box, never from the live ones: sizing to content would rebuild the target —
 * and wait the device idle — on every arrival, expiry and coalescing repaint,
 * i.e. once per keypress while a value is held down. Call OUTSIDE a frame. */
static void ensure_osd_ss(void) {
    if (!s_nr.ovl_resolve_pipe || s_nr.win_w <= 0 || s_nr.win_h <= 0) return;
    if (!openpete_osd_enabled()) return;
    /* Lazy like menu_ss: the grid is large, and a session that never raises a
     * toast should never pay for it. */
    if (!s_nr.osd_ss && !openpete_osd_any_live()) return;

    int sw = (int)(OSD_SLOT_W * (float)s_nr.win_h + 0.5f);
    int sh = (int)(OSD_SLOT_H * (float)s_nr.win_h + 0.5f);
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    int want_w = sw * OSD_SS_FACTOR;
    int want_h = sh * OPENPETE_OSD_MAX * OSD_SS_FACTOR;
    uint32_t maxdim = rhi_max_image_dimension(s_nr.dev);
    if ((uint32_t)want_w > maxdim) { want_w = (int)maxdim; sw = want_w / OSD_SS_FACTOR; }
    if ((uint32_t)want_h > maxdim) {
        want_h = (int)maxdim;
        sh = want_h / (OPENPETE_OSD_MAX * OSD_SS_FACTOR);
        if (sh < 1) sh = 1;
    }
    if (s_nr.osd_ss && s_nr.osd_ss_w == want_w && s_nr.osd_ss_h == want_h) return;
    rhi_wait_idle(s_nr.dev);
    if (s_nr.osd_ss) { rhi_destroy_texture(s_nr.dev, s_nr.osd_ss); s_nr.osd_ss = NULL; }
    s_nr.osd_ss = rhi_create_render_target(s_nr.dev, (uint32_t)want_w, (uint32_t)want_h);
    s_nr.osd_ss_w = want_w; s_nr.osd_ss_h = want_h;
    s_nr.osd_slot_px = (uint64_t)(uint32_t)sw | ((uint64_t)(uint32_t)sh << 32);
    if (s_nr.osd_ss)
        OPL_INFO_F("native", "OSD SS grid %dx%d (%d slots of %dx%d px, %d×)",
                   want_w, want_h, OPENPETE_OSD_MAX, sw, sh, OSD_SS_FACTOR);
}

/* Pre-pass: every live toast's border + text at OSD_SS_FACTOR× into the grid.
 * Its own render pass, so it must run BEFORE the offscreen pass opens. The
 * glyphs arrive in TARGET-LOCAL overlay space (slot i at y = i * slot_h), so
 * one linear map covers the whole grid. */
static void prepare_osd_ss(void) {
    if (!osd_ss_ready() || !osd_should_draw()) return;
    int n;
    SDL_LockMutex(osd_pub_mx());
    n = s_osd_glyph_n;
    SDL_UnlockMutex(osd_pub_mx());
    if (n < 3) return;
    float sw, sh;
    openpete_native_osd_slot_extent(&sw, &sh);
    if (sw <= 0.0f || sh <= 0.0f) return;
    rhi_begin_render_target(s_nr.dev, s_nr.osd_ss, 0.0f, 0.0f, 0.0f, 0.0f);  /* transparent */
    /* The lock spans the draw rather than a copy-out: what it protects is a
     * ~400 KB vertex array, and copying it a second time every present to
     * shorten the hold is the more expensive trade. Inside is a buffer update
     * and a draw call — command-buffer recording, microseconds — and the
     * publish it can block happens once per sim tick. Nothing else is taken
     * under it, so there is no lock order to get wrong. */
    SDL_LockMutex(osd_pub_mx());
    draw_glyph_list_3d(s_osd_glyph_verts, s_osd_glyph_n,
                       s_nr.osd_glyph_vb, OSD_GLYPH_MAX,
                       2.0f / sw, 2.0f / (sh * (float)OPENPETE_OSD_MAX), 0.0f, 0.0f);
    SDL_UnlockMutex(osd_pub_mx());
    rhi_end_render_target(s_nr.dev, s_nr.osd_ss);
}

/* Composite the toasts into the currently-open offscreen pass: per toast a 1×
 * subtractive panel, then the slot's supersampled sub-rect at the toast's
 * alpha. Places through the anchor ledger, so the stats block (which claimed
 * first) is never overprinted. */
static void draw_osd_overlay(void) {
    if (s_nr.win_w <= 0 || s_nr.win_h <= 0) return;

    /* Take the published table first: it is what says which toasts have
     * geometry at all, and the state machine must not start a toast's timer
     * before it can be seen. */
    openpete_osd_slot_t slots[OPENPETE_OSD_MAX];
    int nslots;
    SDL_LockMutex(osd_pub_mx());
    nslots = s_osd_slot_n;
    memcpy(slots, s_osd_slots, sizeof slots);
    SDL_UnlockMutex(osd_pub_mx());
    if (nslots <= 0) return;

    openpete_osd_ready_t ready[OPENPETE_OSD_MAX];
    for (int i = 0; i < nslots; i++) {
        ready[i].id    = slots[i].id;
        ready[i].box_h = slots[i].box_h;
    }
    openpete_osd_view_t view[OPENPETE_OSD_MAX];
    const int nv = openpete_osd_present_update(openpete_interp_present_now(),
                                               ready, nslots,
                                               view, OPENPETE_OSD_MAX);
    /* The state machine is advanced ABOVE the draw gates, deliberately.
     * Returning early when the toasts are merely not being DRAWN — during a
     * --record capture, or before the supersample grid comes up — would stop
     * time for them: nothing would expire, the ring would fill with toasts
     * frozen mid-life, and the whole stale pile would appear at once when
     * drawing resumed. They now live and die on schedule; only the pixels are
     * suppressed. */
    if (nv <= 0 || !osd_ss_ready() || !osd_should_draw()) return;

    float slot_w, slot_h;
    openpete_native_osd_slot_extent(&slot_w, &slot_h);
    int slot_px_w, slot_px_h;
    osd_slot_px(&slot_px_w, &slot_px_h);
    if (slot_px_w <= 0 || slot_px_h <= 0) return;
    const int   anchor = openpete_osd_anchor();
    const float ax   = 2.0f * (float)s_nr.win_h / (float)s_nr.win_w;
    const float ay   = 2.0f;
    const float ph   = (float)s_nr.win_h;
    const float M    = OPENPETE_ANCHOR_MARGIN;
    const float base = s_anchor_ledger.used[anchor];   /* what the stats block took */
    const float lo   = s_anchor_ledger.x_min + M, hi = s_anchor_ledger.x_max - M;

    static rhi_vertex panels[OPENPETE_OSD_MAX * 6];
    int pn = 0;
    struct { float cx, cy, hx, hy, u0, v0, u1, v1; uint32_t rgba; } comp[OPENPETE_OSD_MAX];
    int cn = 0;
    int drawn = 0;

    for (int i = 0; i < nv; i++) {
        const openpete_osd_slot_t *sl = NULL;
        for (int j = 0; j < nslots; j++)
            if (slots[j].id == view[i].id) { sl = &slots[j]; break; }
        if (!sl) continue;   /* retired between the table read and now */

        /* Snap the box to whole output pixels, so the resolve's N×N blocks map
         * 1:1 onto output pixels. A quad landing on subpixel offsets changes
         * which fragment centres fall in which block and shimmers along the
         * glyph edges — the fade carries the easing instead. */
        int bw = (int)(sl->box_w * ph + 0.5f);
        int bh = (int)(sl->box_h * ph + 0.5f);
        if (bw < 1 || bh < 1) continue;
        if (bw > slot_px_w) bw = slot_px_w;
        if (bh > slot_px_h) bh = slot_px_h;
        const float w = (float)bw / ph, h = (float)bh / ph;

        /* Where it sits. x hugs the anchor's edge; y stacks from whatever the
         * persistent chrome already claimed at this anchor. The slide-in runs
         * along x for the side anchors and along y for the centred ones — a
         * centred toast has no near edge to come from. */
        float x = openpete_anchor_is_right(anchor)  ? hi - w
                : openpete_anchor_is_center(anchor) ? 0.5f * (lo + hi - w)
                                                    : lo;
        if (x < lo) x = lo;
        const int bottom = openpete_anchor_is_bottom(anchor);
        float y = bottom ? (1.0f - M - base - view[i].y - h)
                         : (M + base + view[i].y);

        const float travel = 1.0f - view[i].slide;
        if (openpete_anchor_is_center(anchor)) y += bottom ? travel * h : -travel * h;
        else if (openpete_anchor_is_right(anchor)) x += travel * (w + M);
        else                                       x -= travel * (w + M);

        /* Snap the placed rect to the pixel grid too — the same reason. */
        x = (float)(int)(x * ph + (x >= 0.0f ? 0.5f : -0.5f)) / ph;
        y = (float)(int)(y * ph + 0.5f) / ph;

        int a8 = (int)(view[i].alpha * 255.0f + 0.5f);
        if (a8 <= 0) continue;
        if (a8 > 255) a8 = 255;

        /* Panel — the game's own dialog-box subtractive grey. The sub pipe
         * ignores alpha, so the fade scales the SUBTRACTED RGB toward black:
         * at alpha 0 it subtracts nothing, which is the same thing as being
         * absent. */
        const int g = (112 * a8) / 255;
        const uint32_t sub = 0xFF000000u | (uint32_t)g | ((uint32_t)g << 8) | ((uint32_t)g << 16);
        pn = menu_push_quad(panels, pn, ax, ay, x, y, x + w, y + h, sub);

        /* Composite quad — the slot's used sub-rect, tinted by the fade. */
        const float u1 = (float)bw / (float)slot_px_w;
        const float v0 = (float)sl->slot / (float)OPENPETE_OSD_MAX;
        const float v1 = v0 + (float)bh / (float)(slot_px_h * OPENPETE_OSD_MAX);
        const float rw_ndc = w * ax, rh_ndc = h * ay;
        comp[cn].cx = x * ax - 1.0f + rw_ndc * 0.5f;
        comp[cn].cy = 1.0f - y * ay - rh_ndc * 0.5f;
        comp[cn].hx = rw_ndc * 0.5f; comp[cn].hy = rh_ndc * 0.5f;
        comp[cn].u0 = 0.0f; comp[cn].v0 = v0; comp[cn].u1 = u1; comp[cn].v1 = v1;
        comp[cn].rgba = 0x00FFFFFFu | ((uint32_t)a8 << 24);
        cn++;
        drawn++;
    }
    if (drawn <= 0) return;
    s_osd_composited += (unsigned)drawn;   /* engagement counter (a green must prove it engaged) */

    if (pn > 0 && s_nr.gouraud_hud_sub_pipe) {
        rhi_update_buffer(s_nr.osd_panel_vb, panels, (size_t)pn * sizeof(rhi_vertex));
        rhi_reset_scissor(s_nr.dev);
        bind_scene_pipe(s_nr.gouraud_hud_sub_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.osd_panel_vb);
        rhi_draw(s_nr.dev, (uint32_t)pn, 0);
    }

    /* One quad per toast, all in one upload of the per-present ring slot, then
     * one draw each (they differ only in their vertex data, so a single bind
     * serves them all). */
    static rhi_vertex quads[OPENPETE_OSD_MAX * 6];
    int qn = 0;
    for (int i = 0; i < cn; i++) {
        build_panel_tinted(&quads[qn], comp[i].cx, comp[i].cy, comp[i].hx, comp[i].hy,
                           comp[i].u0, comp[i].v0, comp[i].u1, comp[i].v1, comp[i].rgba);
        qn += 6;
    }
    if (qn <= 0 || !s_nr.osd_quad_vb) return;
    rhi_update_buffer(s_nr.osd_quad_vb, quads, (size_t)qn * sizeof(rhi_vertex));
    int factor = OSD_SS_FACTOR;
    rhi_reset_scissor(s_nr.dev);
    bind_scene_pipe(s_nr.ovl_resolve_pipe);
    rhi_bind_vram(s_nr.dev, s_nr.osd_ss);
    rhi_push_constants(s_nr.dev, &factor, sizeof(factor));
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.osd_quad_vb);
    for (int i = 0; i < cn; i++)
        rhi_draw(s_nr.dev, 6, (uint32_t)(i * 6));
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

/* Snapshot the freeze backdrop BEFORE any chrome lands in it.
 *
 * capture_freeze_from_offscreen reads off_image, and off_image is where the
 * stats block, the settings menu and the OSD's toasts have already been
 * drawn — so whatever chrome was on the world frame at pause entry got BAKED
 * into frozen_tex and then sat there, frozen, behind the pause screen long
 * after the live thing had gone. The perf counter has always carried that
 * latent bug, masked only because its live redraw overprints the same rect;
 * moving it off the top-left corner unmasks it, and a toast that expired
 * during the pause has nothing to overprint it at all.
 *
 * So: end the pass, take the snapshot, re-open LOADing what is there, and let
 * chrome draw into the re-opened pass. Same end/run/re-open shape the postfx
 * SCENE split already uses. Costs one extra pass boundary on world presents
 * that actually feed the freeze; returns true when it captured, so the
 * caller's tail capture stands down. Mod content is deliberately on the
 * BACKDROP side of the split: a mod's HUD is content, not chrome. */
static bool chrome_split_capture(bool world_frame)
{
    if (!(world_frame && s_nr.frozen_tex && s_overlay_live)) return false;
    rhi_end_offscreen(s_nr.dev);
    capture_freeze_from_offscreen(true);
    rhi_begin_offscreen_load(s_nr.dev);
    return true;
}

/* Compose the live present into the open frame's offscreen, ready for the
 * caller to rhi_blit_to_swapchain + rhi_end_frame. Assumes rhi_begin_frame
 * succeeded. The screenshot dump composes its own frame separately. */
/* SCENE injection point (S5): called INSIDE the live offscreen pass,
 * right after the scene is composed into off_image and BEFORE the HUD
 * draws. If a SCENE-point chain is enabled, end the pass, run the chain
 * over the scene (depth available at set 1), and re-begin LOADing the
 * post-processed scene so the HUD lands crisp on top. Inert (no pass
 * split, byte-identical) when no SCENE pass is enabled. */
static void scene_postfx_split(void)
{
    if (!native_postfx_point_active(1 /* OP_POSTFX_SCENE */)) return;
    int w = 0, h = 0;
    rhi_swapchain_extent(s_nr.dev, &w, &h);
    rhi_end_offscreen(s_nr.dev);                 /* scene → off_image (TRANSFER_SRC) */
    native_postfx_run(1 /* SCENE */, NULL, w, h);/* chain over off_image, blit back */
    rhi_begin_offscreen_load(s_nr.dev);          /* resume for HUD, scene preserved */
}

static void compose_variant(void)
{
    /* PsyCross display mode: there is no native scene to walk. The picture is
     * PsyCross's finished frame, pulled into ref_tex by
     * sync_reference_texture and uploaded with the VRAM textures, blitted
     * through panel 1, which update_layout aspect-fits to the whole window
     * (letterbox bars where the window is not the reference shape). Postfx, the mod HUD and chrome compose over it
     * exactly as they compose over a native scene, which is the whole point of
     * the mode: one display path for every game.
     *
     * No SSAA and no freeze snapshot: both resample or capture a native scene
     * render, and there is none. A missing ref_tex (PsyCross has not presented
     * a frame yet) leaves the clear, one present long. */
    if (!s_nr.scene_live) {
        rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);
        if (s_nr.ref_tex && s_nr.layout_ref_w > 0) {
            bind_scene_pipe(s_nr.blit_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.ref_tex);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
            rhi_draw(s_nr.dev, 6, 6);   /* panel 1: the reference panel */
        }
        scene_postfx_split();                     /* SCENE point (S5) */
        draw_mod_overlay();                       /* mod HUD (content) */
        if (s_overlay_live) {
            draw_perf_overlay();
            draw_osd_overlay();
            draw_menu_overlay();
        }
        if (s_overlay_live && !native_postfx_point_active(0))
            openpete_devui_render(s_nr.dev);
        rhi_end_offscreen(s_nr.dev);
        return;
    }

    int sn; uint32_t sbg, sfade, sfreeze;
    const scene_tri_t* scene = scene_present_buf(&sn, &sbg, &sfade, &sfreeze, NULL, NULL);
    uint32_t bg = sbg;   /* gamestate clear (== the PSX draw-env clear) */
    bool want_scene = (sn > 0) || (s_isolation && bg != 0);

    /* native_rtt is the OFF-DISPLAY reference-resolution render that the
     * freeze-backdrop snapshot copies. The LIVE panel does NOT blit from it — it
     * draws the scene DIRECTLY into the window-sized offscreen below, so the
     * native display keeps its FULL window resolution (rendering into the
     * ref-sized native_rtt and upscaling would soften textures and drop fine UI).
     * So native_rtt is rendered only when actually consumed: on WORLD frames, to
     * refresh the freeze snapshot (skip menu/freeze frames so the snapshot is the
     * gameplay frame to freeze, never the menu drawn over it). */
    /* With the retained terrain cache in use the terrain is not in the tri
     * list at all, and a scene whose terrain build the feeder SKIPPED (all
     * sectors claimed: the draws open but painting nothing) is not a world
     * frame however many mobys it carries. Eager production extracts such a
     * scene at the pause-entry transition -- the moby and sky feeders still
     * run while the terrain feed has cut out -- where the inline path
     * extracts nothing; snapshotting it froze a backdrop with the ground
     * missing. Read here, on the presenting thread, after frame_select. */
    bool world_frame = want_scene && !(sfreeze & (1u << 31)) && scene_has_world(scene, sn)
                       && openpete_tercache_frame_content() != 0;
    /* OPENPETE_FREEZE_DBG=1: one line per composed present -- which frames
     * qualify as the freeze backdrop and where their terrain came from. */
    static int s_fdbg = -1;
    if (s_fdbg < 0) { const char* e = getenv("OPENPETE_FREEZE_DBG"); s_fdbg = (e && *e && *e != '0'); }
    if (s_fdbg) {
        int cw = 0, ca = 0;
        for (int t = 0; t < sn; t++) {
            if (scene[t].category == OPENPETE_SCENE_WORLD) cw++;
            else if (scene[t].category == OPENPETE_SCENE_ACTOR || scene[t].category == OPENPETE_SCENE_ACTOR_FRONT) ca++;
        }
        int td, th, tl, tn, tg[8];
        openpete_tercache_frame_stats(&td, &th, &tl, &tn, tg);
        char gb[512]; openpete_tercache_frame_globals(gb, sizeof gb);
        /* Append: frame_slots writes from index 0, so it must be handed the
         * tail of the buffer or it overwrites the globals. */
        const size_t gblen = strlen(gb);
        openpete_tercache_frame_slots(gb + gblen, (int)(sizeof gb - gblen));
        fprintf(stderr, "[freeze f=%u stamp=%08x] world=%d n=%d W=%d A=%d freeze=%08x tc_src=%d drawn=%d | %s\n",
                s_scene_frame, scene_present_stamp(), (int)world_frame, sn, cw, ca, sfreeze,
                openpete_tercache_frame_source(), td, gb);
    }
    /* The freeze backdrop is snapshotted from the clean, correctly-projected
     * windowed offscreen (rhi_snapshot_offscreen_to_rt after the offscreen pass
     * below), never from a full-extent render into native_rtt: that skews
     * window-dependently and squished the pause/inventory freeze at non-native
     * window sizes. */
    /* SSAA (--ssaa>1, vk mode): render the scene into the supersampled ss_rtt,
     * then a fragment resolve pass box-averages it (in linear light) into the 1×
     * offscreen — a true box filter at every factor and gamma-correct, unlike the
     * present blit's single bilinear tap. */
    if (s_nr.ss_rtt && s_nr.ss_factor > 1 && s_nr.layout_ref_w > 0) {
        /* NB the black clear is deliberate: clearing to the scene bg colour
         * and skipping the bg fill quad measured as a WASH on NVIDIA — an
         * arbitrary clear colour defeats the fast-clear/compression path and
         * taxes every later colour write (~+130us on both the clear and the
         * WORLD group at 4K SSAA4), cancelling the saved quad. */
        rhi_gpu_zone_begin(s_nr.dev, "sc.scene");
        { int kw, kh; rhi_texture_extent(s_nr.ss_rtt, &kw, &kh);
          native_terrain_key_pass(0, kw, kh); }
        rhi_gpu_zone_begin(s_nr.dev, "sc.rtbegin");
        rhi_begin_render_target(s_nr.dev, s_nr.ss_rtt, 0.0f, 0.0f, 0.0f, 1.0f);
        rhi_gpu_zone_end(s_nr.dev);
        bool drew = draw_native_scene(false);   /* scene at ss_factor× (scissor auto-scaled) */
        rhi_end_render_target(s_nr.dev, s_nr.ss_rtt);
        rhi_gpu_zone_end(s_nr.dev);

        rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);
        if (drew) {
            int n = s_nr.ss_factor;
            rhi_gpu_zone_begin(s_nr.dev, "sc.resolve");
            bind_scene_pipe(s_nr.resolve_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.ss_rtt);
            rhi_push_constants(s_nr.dev, &n, sizeof(n));
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.fs_quad_vb);
            rhi_draw(s_nr.dev, 6, 0);
            rhi_gpu_zone_end(s_nr.dev);
        } else {
            /* Empty scene → PSX VRAM framebuffer fallback (1× direct, no SSAA). */
            int tx = 0, ty = 0;
            if (s_nr.disp_w > 0 && s_nr.clip_h > 0 && s_nr.clip_h <= s_nr.disp_h) {
                int dy = (s_nr.disp_h - s_nr.clip_h) / 2;
                tx = s_nr.disp_x; ty = s_nr.disp_y + dy;
            }
            const psx_material nd_mat = { tx, ty, 0, 0, 16 };
            bind_scene_pipe(s_nr.vram_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            rhi_push_constants(s_nr.dev, &nd_mat, sizeof(nd_mat));
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
            rhi_draw(s_nr.dev, 6, 0);
        }
        scene_postfx_split();                     /* SCENE point (S5): scene→chain→HUD */
        draw_mod_overlay();                       /* mod HUD (content — records too) */
        /* Freeze snapshot first, chrome after — see chrome_split_capture. */
        const bool froze = chrome_split_capture(world_frame);
        if (s_overlay_live) {
            draw_perf_overlay();                  /* on the resolved 1× offscreen, crisp */
            draw_osd_overlay();                   /* toasts above the readout    */
            draw_menu_overlay();                  /* settings menu on top        */
        }
        /* Dev overlay: under a live COMPOSITE chain, drawn post-chain
         * instead so screen filters never render over the panel. */
        if (s_overlay_live && !native_postfx_point_active(0))
            openpete_devui_render(s_nr.dev);      /* dev overlay topmost */
        rhi_end_offscreen(s_nr.dev);
        /* The freeze snapshot lives in the fall-through path below, which this SSAA
         * branch skips — so capture it here too, from the box-resolved 1× offscreen
         * (window resolution, matching the non-SSAA snapshot). Without this the
         * pause/inventory freeze had no captured frame under --ssaa and showed the
         * clear/cyclorama colour instead of the frozen world. Skipped when the
         * chrome split already took it. */
        if (!froze) capture_freeze_from_offscreen(world_frame);
        return false;
    }

    /* vk / ab (and the diff fallback on an empty scene): draw the scene DIRECTLY
     * into the window-sized offscreen at full resolution (the original display path,
     * unchanged by the freeze work). The freeze overlay — drawn inside
     * draw_native_scene sampling frozen_tex — composes here too on menu frames. An
     * empty scene falls back to re-displaying the PSX VRAM framebuffer (loading
     * screens / transitions / pre-scene bootstrap). */
    native_terrain_key_pass(0, s_nr.win_w, s_nr.win_h);
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
        rhi_gpu_zone_begin(s_nr.dev, "sc.scene");
        bool scene_drew = draw_native_scene(false);
        rhi_gpu_zone_end(s_nr.dev);
        if (!scene_drew) {
            const psx_material nd_mat = { tx, ty, 0, 0, 16 };
            bind_scene_pipe(s_nr.vram_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            rhi_push_constants(s_nr.dev, &nd_mat, sizeof(nd_mat));
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
            rhi_draw(s_nr.dev, 6, 0);
        }

    }

    /* SCENE point (S5) then mod HUD — vk variant, live + record composes. */
    scene_postfx_split();
    draw_mod_overlay();
    bool froze = chrome_split_capture(world_frame);   /* backdrop before chrome */
    /* Chrome — the live present only, never screenshots or dumps, where
     * s_overlay_live is 0. */
    if (s_overlay_live) {
        draw_perf_overlay();
        draw_osd_overlay();
        draw_menu_overlay();
    }
    /* Dev overlay: when a COMPOSITE postfx chain is live it would filter
     * the panel into illegibility (CRT masks) — skip here and re-draw on
     * TOP of the chain's output instead (present_native, post-chain load
     * pass). One imgui frame either way. */
    if (s_overlay_live && !native_postfx_point_active(0))
        openpete_devui_render(s_nr.dev);

    rhi_end_offscreen(s_nr.dev);

    if (!froze) capture_freeze_from_offscreen(world_frame);
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

/* Env-gated stderr report (the legacy [phase ...] once-per-sec line). Separate
 * from openpete_phase_timers_on() so a --profile-out-only run measures the
 * phases (and forwards them to the trace) without spamming stderr. */
static int pt_report_on(void) {
    /* Re-resolved on a live edit (the dev overlay's setenv bumps the
     * registry epoch), never per VBL: this runs at every phase-scope site
     * on both the extracting and the presenting thread. */
    static int on = -1;
    static unsigned resolved_ep = 0;
    const unsigned ep = knob_registry_env_epoch();
    if (on < 0 || resolved_ep != ep) {
        const char* e = getenv("OPENPETE_PHASE_TIMERS");
        on = (e && *e && *e != '0');
        resolved_ep = ep;
    }
    return on;
}
int openpete_phase_timers_on(void) {
    /* True when EITHER the env report is requested OR a perf trace is open —
     * both need the per-phase µs measured at the call sites. */
    return pt_report_on() || op_prof_on();
}
uint64_t openpete_phase_now_us(void) {
    static double us_per_tick = 0.0;
    if (us_per_tick == 0.0) us_per_tick = 1e6 / (double)SDL_GetPerformanceFrequency();
    return (uint64_t)((double)SDL_GetPerformanceCounter() * us_per_tick); /* clock-guard: wall */ /* phase timer */
}

/* Map the fixed OPPT/OPPC enums to interned trace-scope names (lazy). */
static const char* const s_oppt_names[OPPT_COUNT] = {
    "rex_total", "rex.terrain", "rex.moby", "rex.shadow", "rex.spyro",
    "prepass", "compose", "gpu_sync", "sim_tick"
};
static const char* const s_oppc_names[OPPC_COUNT] = { "mobys", "tris", "draws", "culled" };
static int s_oppt_id[OPPT_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int s_oppc_id[OPPC_COUNT] = { -1, -1, -1, -1 };

void openpete_phase_add(int phase, uint64_t us) {
    if ((unsigned)phase >= OPPT_COUNT) return;
    s_pt_cur[phase] += (uint32_t)us;
    if (op_prof_on()) {
        if (s_oppt_id[phase] < 0) s_oppt_id[phase] = op_prof_intern(s_oppt_names[phase]);
        op_prof_add_span(s_oppt_id[phase], us);
    }
}
void openpete_phase_set_count(int which, int n) {
    if ((unsigned)which >= OPPC_COUNT) return;
    s_pt_cnt_cur[which] = n;
    if (op_prof_on()) {
        if (s_oppc_id[which] < 0) s_oppc_id[which] = op_prof_intern(s_oppc_names[which]);
        op_prof_count(s_oppc_id[which], (uint64_t)n);
    }
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

    /* --profile-out-only runs roll+reset the ring (above/below) but skip the
     * stderr line; only OPENPETE_PHASE_TIMERS=1 prints it. */
    if (!pt_report_on()) { s_pt_n = 0; s_pt_last_report = now; return; }

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
    float    pad_rect[2];               /* std140: vec4 aligns to 16 */
    float    scene_rect[4], scene_grid[4];
} pfx_engine_ubo_t;

/* The engine block (op_engine, set 3 binding 0) — ONE fill for every
 * consumer (postfx chain, mod meshes, material UBO); a new field added
 * here reaches all of them. `w`,`h` = the pass/render resolution (0 =
 * window). */
static void fill_engine_block(pfx_engine_ubo_t* eng, int w, int h)
{
    memset(eng, 0, sizeof *eng);
    eng->time = (float)((double)openpete_phase_now_us() * 1e-6);
    eng->present = ps_frame();
    int canonical = 1;
    if (s_ps_slot) { eng->tick = s_ps_slot->pctx_tick; eng->alpha = s_ps_slot->pctx_alpha; canonical = s_ps_slot->pctx_canonical; }
    else openpete_mods_present_ctx(&eng->tick, &eng->alpha, &canonical);
    eng->canonical = (uint32_t)canonical;
    int ww = 0, wh = 0;
    rhi_swapchain_extent(s_nr.dev, &ww, &wh);
    eng->res[0] = (float)(w > 0 ? w : ww);  eng->res[1] = (float)(h > 0 ? h : wh);
    eng->win[0] = (float)ww;                eng->win[1] = (float)wh;
    eng->aspect = eng->res[1] > 0 ? eng->res[0] / eng->res[1] : 1.0f;
    /* op_scene_rect/op_scene_grid: where the canonical 512-wide PSX screen
     * maps in the pass image (mirrors scene_to_clip), so a postfx can align
     * exactly to the virtual raster the vertex snap quantizes to. */
    if (s_render_full_extent) {
        float wf = s_render_freeze_wf > 0.0f ? s_render_freeze_wf : 1.0f;
        eng->scene_rect[0] = 0.5f - 0.5f / wf;  eng->scene_rect[1] = 0.0f;
        eng->scene_rect[2] = 1.0f / wf;         eng->scene_rect[3] = 1.0f;
        eng->scene_grid[3] = wf;
    } else {
        eng->scene_rect[0] = (s_nr.p0_cx - s_nr.p0_hx + 1.0f) * 0.5f;
        eng->scene_rect[1] = (1.0f - s_nr.p0_hy) * 0.5f;
        eng->scene_rect[2] = s_nr.p0_hx;
        eng->scene_rect[3] = s_nr.p0_hy;
        eng->scene_grid[3] = s_nr.ws_factor > 0.0f ? s_nr.ws_factor : 1.0f;
    }
    eng->scene_grid[0] = s_nr.disp_w > 0 ? (float)s_nr.disp_w : 512.0f;
    eng->scene_grid[1] = s_nr.disp_h > 0 ? (float)s_nr.disp_h : 240.0f;
    eng->scene_grid[2] = s_nr.clip_h > 0 ? (float)s_nr.clip_h : 224.0f;
}
static void write_engine_block(rhi_ubo* u, int blk, int w, int h)
{
    pfx_engine_ubo_t eng;
    fill_engine_block(&eng, w, h);
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
    int nslots = s_ps_slot ? s_ps_slot->matview.slot_n : openpete_mods_material_slot_count();   /* 1 + interned */
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
        const void* p = s_ps_slot ? (len = s_ps_slot->matview.slots[s].len, (const void*)s_ps_slot->matview.slots[s].bytes)
                                  : openpete_mods_material_slot_uniforms(s, &len);
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
/* Any enabled postfx pass at `point`? Cheap gate for the SCENE split. */
/* Engine-owned SCENE pass (--raster-quantize): PSX-raster point-resample of the
 * composed scene. Rides the postfx executor as pass 0 with a pre-built
 * pipeline (embedded raster_quantize.frag) so it stacks under any mod chain
 * (a CRT filter processes the quantized scene, as it should). */
static void* s_rq_pipe;                        /* rhi_pipeline* / PFX_PIPE_FAILED */
static struct {                                /* std140 mirror of rq_params */
    float scale, od_ok;
    float od_uvmax[2];                         /* last valid alloc uv of the raster */
    float od_ax, od_bx, od_ay, od_by;          /* PSX screen -> opaque_depth ALLOC uv */
} s_rq_params;

static int raster_quantize_prepend(int point, openpete_postfx_pass_t* pass)
{
    if (point != 1 /* OP_POSTFX_SCENE */ || s_raster_mult <= 0) return 0;
    if (!s_rq_pipe) {
        rhi_pipeline_desc pd = {0};
        pd.vert_spv = "postfx_fs.vert.spv";
        pd.frag_spv = "raster_quantize.frag.spv";
        pd.blend    = RHI_BLEND_NONE;
        pd.postfx   = true;
        rhi_pipeline* p = rhi_create_pipeline(s_nr.dev, &pd);
        s_rq_pipe = p ? (void*)p : PFX_PIPE_FAILED;
        if (!p)
            fprintf(stderr, "[native] raster-quantize: pipeline build FAILED "
                            "— quantize disabled\n");
    }
    s_rq_params.scale = (float)s_raster_mult;
    /* Depth-majority vote input: the WORLD-opaque pre-pass depth, addressed
     * through its PSX-screen->uv affine scaled from the raster extent to the
     * grow-only allocation (the shadow-decal consumers' mapping — plain
     * screen uv would read outside the rastered sub-rect: all-far). */
    int od_ok = s_od_ready && s_nr.opaque_depth && s_nr.od_w > 0 && s_nr.od_h > 0;
    s_rq_params.od_ok = od_ok ? 1.0f : 0.0f;
    if (od_ok) {
        float su = s_od_vw / (float)s_nr.od_w, sv = s_od_vh / (float)s_nr.od_h;
        s_rq_params.od_ax = s_od_uax * su;  s_rq_params.od_bx = s_od_ubx * su;
        s_rq_params.od_ay = s_od_vay * sv;  s_rq_params.od_by = s_od_vby * sv;
        /* clamp taps inside the rastered sub-rect (outside = stale/undefined) */
        s_rq_params.od_uvmax[0] = (s_od_vw - 0.5f) / (float)s_nr.od_w;
        s_rq_params.od_uvmax[1] = (s_od_vh - 0.5f) / (float)s_nr.od_h;
    }
    memset(&pass[0], 0, sizeof pass[0]);
    pass[0].uniforms      = &s_rq_params;
    pass[0].uniforms_len  = (uint32_t)sizeof s_rq_params;
    pass[0].pipeline_slot = &s_rq_pipe;
    pass[0].owner         = "raster-quantize";
    return 1;
}

/* Engine-owned SCENE pass (--dither): PS1 15-bit colour — the 4x4 hardware
 * dither matrix on the PSX framebuffer lattice + RGB555 truncation
 * (psx_dither.frag). Runs AFTER the raster quantize (the console dithered
 * each framebuffer write, i.e. downstream of rasterisation) and BEFORE any
 * mod chain, so a CRT filter processes the dithered image — the authentic
 * dither -> composite -> CRT order that made the pattern invisible on
 * period hardware. Needs no custom uniforms: the grid mapping rides the
 * shared engine UBO (op_scene_rect/op_scene_grid). */
static void* s_dither_pipe;                    /* rhi_pipeline* / PFX_PIPE_FAILED */

static int psx_dither_append(int point, openpete_postfx_pass_t* pass)
{
    if (point != 1 /* OP_POSTFX_SCENE */ || !s_dither) return 0;
    if (!s_dither_pipe) {
        rhi_pipeline_desc pd = {0};
        pd.vert_spv = "postfx_fs.vert.spv";
        pd.frag_spv = "psx_dither.frag.spv";
        pd.blend    = RHI_BLEND_NONE;
        pd.postfx   = true;
        rhi_pipeline* p = rhi_create_pipeline(s_nr.dev, &pd);
        s_dither_pipe = p ? (void*)p : PFX_PIPE_FAILED;
        if (!p)
            fprintf(stderr, "[native] dither: pipeline build FAILED "
                            "— dither disabled\n");
    }
    memset(&pass[0], 0, sizeof pass[0]);
    pass[0].pipeline_slot = &s_dither_pipe;
    pass[0].owner         = "psx-dither";
    return 1;
}

static int native_postfx_point_active(int point)
{
    openpete_postfx_pass_t pass[PFX_MAX_PASSES];
    if (point == 1 /* OP_POSTFX_SCENE */ && (s_raster_mult > 0 || s_dither)) return 1;
    return openpete_mods_postfx_collect(point, pass, PFX_MAX_PASSES) > 0;
}

static int native_postfx_run(int point, rhi_texture* target, int w, int h)
{
    openpete_postfx_pass_t pass[PFX_MAX_PASSES];
    int n = raster_quantize_prepend(point, pass);
    n += psx_dither_append(point, pass + n);
    n += openpete_mods_postfx_collect(point, pass + n, PFX_MAX_PASSES - n);
    if (n <= 0 || w <= 0 || h <= 0) return 0;
    if (!s_pfx_ubo)
        s_pfx_ubo = rhi_create_ubo(s_nr.dev, OPENPETE_POSTFX_UNIFORMS_MAX,
                                   1 + PFX_MAX_PASSES);
    rhi_texture** ping = pfx_pings(w, h);
    if (!s_pfx_ubo || !ping) return 0;

    /* Same tick/alpha source as the present-hook ctx (mod_draw.c) —
     * shaders and mod C code can never disagree on the sub-tick phase. */
    pfx_engine_ubo_t eng;
    fill_engine_block(&eng, w, h);
    if (target) { eng.alpha = 0.0f; eng.canonical = 1; } /* dump compose */
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
        bind_scene_pipe((rhi_pipeline*)*pass[i].pipeline_slot);
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
#define GPUZ_MAX 40   /* pass skeleton + nested sc.* (verbose) + per-mod postfx */
static struct { const char *name; double ema; } s_gpuz[GPUZ_MAX];
static int      s_gpuz_n = 0;
static uint64_t s_gpuz_last = 0;

static void gpu_zone_report(void)
{
    if (!rhi_gpu_timers_on(s_nr.dev)) return;
    rhi_gpu_zone_result res[RHI_GPU_ZONES_MAX];
    int n = rhi_gpu_zone_results(s_nr.dev, res, RHI_GPU_ZONES_MAX);

    /* Forward the harvested (frame-lagged) zones to the perf trace, bound to the
     * present that submitted them via the token stored at that frame's begin. */
    if (op_prof_on() && n > 0) {
        uint32_t pidx = (uint32_t)rhi_gpu_results_token(s_nr.dev);
        for (int i = 0; i < n; i++) {
            int zid = op_prof_intern(res[i].name);
            op_prof_gpu_zone(zid, res[i].depth, pidx,
                             (uint32_t)(res[i].start_us + 0.5),
                             (uint32_t)(res[i].us + 0.5));
        }
    }

    /* The once-per-second stderr line folds DEPTH-0 zones only (the pass
     * skeleton + per-mod postfx attribution) — nested sub-zones (sc.*) would
     * double its length and their audience is the --profile-out trace, which
     * got every depth above. OPENPETE_GPU_TIMERS_VERBOSE=1 shows all depths. */
    /* Re-read per report (~1/s — cheap): setenv is the live channel. */
    const char* gpuz_env = getenv("OPENPETE_GPU_TIMERS_VERBOSE");
    int s_gpuz_verbose = (gpuz_env && *gpuz_env && *gpuz_env != '0');
    for (int i = 0; i < n; i++) {
        if (!s_gpuz_verbose && res[i].depth > 0) continue;
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
    char line[1024]; int off = 0;
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

/* ---- eager scene ring: operations (types above, by s_scene) ---- */
static void render_frame_common(uint64_t record_pts_us, bool have_ref);
static void pt_dump_maybe(int rendered, unsigned frame, uint32_t stamp, uint32_t freeze);
static int  s_last_frame_presented;
static SDL_Mutex *ring_mx(void) {
    if (!s_ring_mx) s_ring_mx = SDL_CreateMutex();
    return s_ring_mx;
}
static void ps_apply_geometry(void) {
    const int *g = s_ps_slot ? s_ps_slot->geom : s_geom_in;
    s_nr.disp_x = g[0]; s_nr.disp_y = g[1]; s_nr.disp_w = g[2]; s_nr.disp_h = g[3];
    s_nr.clip_x = g[4]; s_nr.clip_y = g[5]; s_nr.clip_w = g[6]; s_nr.clip_h = g[7];
}
static uint32_t ps_drawenv_clear(void)  { return s_ps_slot ? s_ps_slot->drawenv_clear : s_drawenv_clear; }
static const openpete_shadow_decal_params *ps_shadow(void) { return s_ps_slot ? &s_ps_slot->shadow : &s_shadow_decal; }
static unsigned ps_frame(void)          { return s_ps_slot ? s_ps_slot->frame : s_scene_frame; }

/* Stamp the terrain builder tags its build with: the scene id while an eager
 * scene is being extracted, else the VBL counter as before. Scene ids carry
 * the top bit so they never collide with a VBL number. */
uint32_t openpete_native_scene_build_stamp(void) {
    extern unsigned openpete_frame;
    return s_eager_stamp ? s_eager_stamp : (uint32_t)openpete_frame;
}
void openpete_native_scene_eager_begin(void) {
    s_eager_stamp = OPTC_STAMP_SCENE_BIT | (s_scene_id_next & ~OPTC_STAMP_SCENE_BIT);
}
static int slot_grow(scene_slot_t *sl, int n) {
    if (sl->cap >= n) return 1;
    int cap = sl->cap ? sl->cap : 4096;
    while (cap < n) cap *= 2;
    void *nt = realloc(sl->tri, (size_t)cap * sizeof(scene_tri_t));
    if (!nt) return 0;
    sl->tri = (scene_tri_t*)nt; sl->cap = cap;
    return 1;
}
/* Copy the accumulator and its companions into the next ring slot. Returns
 * the slot index, or -1 when the ring has no free slot (the producer is
 * ahead of the consumer by a whole ring) or the copy could not be sized —
 * a refused publish, never a truncated scene.
 *
 * Any FREE slot will do, not only the one after the last publish. Slot
 * position carries no meaning — pick orders by scene_id and retire/flush
 * scan the whole ring — while a PRESENTED slot can sit unretired for as
 * long as the swapchain refuses frames (a live resize): retirement runs
 * only after a successful begin_frame. If publishing required that exact
 * slot, one such straggler would refuse every publish once the ring
 * wrapped onto it, the consumer would drain pending and go idle, and with
 * no render there is no begin_frame to ever retire it: the display stays
 * frozen for good while the sim runs on. */
int openpete_native_scene_publish(int64_t target_ns, int kind, double alpha, int paired) {
    SDL_LockMutex(ring_mx());
    int idx = -1;
    for (int i = 1; i <= SCENE_RING; i++) {
        const int cand = (s_ring_head + i) % SCENE_RING;
        if (s_ring[cand].state == SS_FREE) { idx = cand; break; }
    }
    if (idx < 0) { s_ring_census.refused_full++; SDL_UnlockMutex(s_ring_mx); s_eager_stamp = 0; return -1; }
    scene_slot_t *sl = &s_ring[idx];
    SDL_UnlockMutex(s_ring_mx);   /* the slot is FREE: only this thread publishes */
    if (!slot_grow(sl, s_scene_n)) { s_eager_stamp = 0; return -1; }
    if (s_scene_n > 0) memcpy(sl->tri, s_scene, (size_t)s_scene_n * sizeof(scene_tri_t));
    sl->n = s_scene_n; sl->bg = s_scene_bg; sl->fade = s_scene_fade; sl->freeze = s_scene_freeze;
    sl->hud_under = s_scene_hud_under; sl->actor_front = s_scene_actor_front;
    sl->frame = s_scene_frame;
    sl->stamp = s_eager_stamp ? s_eager_stamp : s_scene_frame;
    sl->drawenv_clear = s_drawenv_clear;
    memcpy(sl->geom, s_geom_in, sizeof sl->geom);
    if (sl->ovl_cap < s_mod_overlay_n) {
        int cap = s_mod_overlay_n < 256 ? 256 : s_mod_overlay_n;
        void *nv = realloc(sl->ovl, (size_t)cap * sizeof(openpete_hud_vert));
        if (nv) { sl->ovl = (openpete_hud_vert*)nv; sl->ovl_cap = cap; }
    }
    sl->ovl_n = sl->ovl_cap >= s_mod_overlay_n ? s_mod_overlay_n : 0;
    if (sl->ovl_n > 0) memcpy(sl->ovl, s_mod_overlay_verts, (size_t)sl->ovl_n * sizeof(openpete_hud_vert));
    sl->shadow = s_shadow_decal;
    openpete_mods_present_ctx(&sl->pctx_tick, &sl->pctx_alpha, &sl->pctx_canonical);
    openpete_mods_material_view_capture(&sl->matview);
    sl->scene_id = s_scene_id_next++;
    sl->target_ns = target_ns; sl->alpha = alpha; sl->paired = paired; sl->kind = kind;
    sl->presented_serial = 0;
    s_eager_stamp = 0;
    SDL_LockMutex(s_ring_mx);
    sl->state = SS_PENDING;
    s_ring_head = idx;
    s_ring_census.published++;
    SDL_UnlockMutex(s_ring_mx);
    return idx;
}
/* Earliest pending target (0 = none pending). */
int64_t openpete_native_scene_next_target(void) {
    op_time_t t = 0;
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING; i++)
        if (s_ring[i].state == SS_PENDING && (t == 0 || s_ring[i].target_ns < t))
            t = s_ring[i].target_ns;
    SDL_UnlockMutex(s_ring_mx);
    return t;
}
/* Pick the pending scene whose target is nearest `at`; every pending scene
 * OLDER than the pick retires unpresented (superseded). Alpha binds here, at
 * consume time: the scene shown is the animation moment nearest the flip,
 * not the one a fixed slot mapping decided on at extraction. Returns the
 * slot (now PRESENTING) or -1. */
int openpete_native_scene_pick(int64_t at) {
    int best = -1; op_time_t bd = 0;
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING; i++) {
        if (s_ring[i].state != SS_PENDING) continue;
        op_time_t d = s_ring[i].target_ns > at ? s_ring[i].target_ns - at : at - s_ring[i].target_ns;
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    if (best >= 0) {
        for (int i = 0; i < SCENE_RING; i++)
            if (i != best && s_ring[i].state == SS_PENDING &&
                (int32_t)(s_ring[i].scene_id - s_ring[best].scene_id) < 0) {
                s_ring[i].state = SS_FREE; s_ring_census.superseded++;
            }
        s_ring[best].state = SS_PRESENTING;
    }
    SDL_UnlockMutex(s_ring_mx);
    return best;
}
/* Called by the present thread right after every rhi_begin_frame: frames
 * RHI_FRAMES_IN_FLIGHT behind the one just begun have had their fence
 * waited, so their scenes' GPU reads are over. */
void openpete_native_scene_retire(uint64_t begun_serial) {
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING; i++)
        if (s_ring[i].state == SS_PRESENTED &&
            s_ring[i].presented_serial + RHI_FRAMES_IN_FLIGHT <= begun_serial)
            s_ring[i].state = SS_FREE;
    SDL_UnlockMutex(s_ring_mx);
}
/* True while any unretired scene carries this build stamp. */
int openpete_native_scene_stamp_live(uint32_t stamp) {
    int live = 0;
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING && !live; i++)
        live = s_ring[i].state != SS_FREE && s_ring[i].stamp == stamp;
    SDL_UnlockMutex(s_ring_mx);
    return live;
}
/* Drop every pending scene (guest discontinuity: savestate, rewind, level
 * load). A scene mid-present finishes; its pixels are already committed. */
void openpete_native_scene_ring_flush(void) {
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING; i++)
        if (s_ring[i].state == SS_PENDING) { s_ring[i].state = SS_FREE; s_ring_census.superseded++; }
    SDL_UnlockMutex(s_ring_mx);
}
int openpete_native_scene_ring_pending(void) {
    int n = 0;
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING; i++) n += s_ring[i].state == SS_PENDING;
    SDL_UnlockMutex(s_ring_mx);
    return n;
}
int openpete_native_scene_ring_busy(void) {
    int n = 0;
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING; i++) n += s_ring[i].state != SS_FREE;
    SDL_UnlockMutex(s_ring_mx);
    return n;
}
void openpete_native_scene_slot_info(int slot, int64_t *target_ns, double *alpha, int *paired, int *kind) {
    if (slot < 0 || slot >= SCENE_RING) return;
    if (target_ns) *target_ns = s_ring[slot].target_ns;
    if (alpha) *alpha = s_ring[slot].alpha;
    if (paired) *paired = s_ring[slot].paired;
    if (kind) *kind = s_ring[slot].kind;
}
/* The display's refresh for the current swapchain (0 = unknown). */
float openpete_native_display_refresh_hz(void) { return s_nr.dev ? rhi_refresh_hz(s_nr.dev) : 0.0f; }
int   openpete_native_display_vsync(void)      { return s_nr.dev ? rhi_present_is_vsync(s_nr.dev) : 0; }
/* Free ring slots: the producer's back-pressure. */
int openpete_native_scene_ring_free(void) { return SCENE_RING - openpete_native_scene_ring_busy(); }
void openpete_native_scene_ring_report(void) {
    if (!s_ring_census.published) return;
    OPL_INFO_F("scene-ring", "%u scene(s) published, %u presented, %u superseded unpresented, %u refused (ring full)",
               s_ring_census.published, s_ring_census.presented,
               s_ring_census.superseded, s_ring_census.refused_full);
}

void openpete_native_render_frame(uint64_t record_pts_us)
{
    if (!s_nr.enabled)
        return;

    /* Headless (W1.1): no window / swapchain, so there is no live present. The
     * scene is composed on demand by openpete_native_render_screenshot_all. */
    if (s_nr.headless)
        return;

    /* Queued --texture-mapping change: re-bake the pipelines OUTSIDE the
     * frame, before any command buffer records against them. No-op normally. */
    texmap_apply_pending();
    notex_apply_pending();
    truescope_apply_pending();


    /* Mirror VRAM + read the reference back BEFORE opening our frame: these do
     * their own one-shot GPU submits, kept clear of our render pass. Called
     * UNCONDITIONALLY: sync_reference_texture self-no-ops when PsyCross hasn't
     * produced a scene (pure vk — the OT isn't rasterized — so GR_GetPresentSceneSize
     * returns 0 → early false), so it's free there; whenever PsyCross IS rendering
     * (screenshot) it must run, both to upload the reference AND because its
     * GL readback orders the GPU work the screenshot's rhi_read_offscreen then reads
     * (gating it off for vk blanked the captured native-intermediate frames). */
    sync_vram_texture();
    bool have_ref = sync_reference_texture();
    { const uint64_t b0 = rhi_frame_serial(s_nr.dev); render_frame_common(record_pts_us, have_ref); pt_dump_maybe(rhi_frame_serial(s_nr.dev) != b0, s_scene_frame, s_scene_frame, s_scene_freeze); }
}

/* Compose and present one ring slot. The present thread's entry: no VRAM or
 * reference sync here (the guest side did the sync while the guest was idle,
 * and the only mode that displays the reference — psycross — never runs
 * eager, since it starts no present thread), the mod
 * registry is held for reading across the compose, and the slot moves to
 * PRESENTED with the serial of the frame that carries it — or back to FREE
 * when the swapchain declined, which is a refused present, not a shown one.
 * Returns 1 if a present went out. */
/* The frame in progress ends in stage image `s_stage_target` instead of a
 * present (rhi_end_frame_to_stage) when >= 0. */
static int s_stage_target = -1;

/* OPENPETE_PT_DUMP=N (diagnostic): write the first render(s) of a scene
 * extracted at VBL >= N to OPENPETE_PT_DUMP_PATH (a printf template taking
 * the stamp), straight from the offscreen target, plus frozen_tex alongside
 * -- the actual output of either present path, which no screenshot flag
 * reaches. */
static void pt_dump_maybe(int rendered, unsigned frame, uint32_t stamp, uint32_t freeze)
{
    static int s_dump_at = -2;
    if (s_dump_at == -2) { const char *e = getenv("OPENPETE_PT_DUMP"); s_dump_at = (e && *e) ? atoi(e) : -1; }
    static int s_dump_left = -1;
    if (s_dump_left < 0) { const char *e = getenv("OPENPETE_PT_DUMP_COUNT"); s_dump_left = (e && *e) ? atoi(e) : 1; }
    if (s_dump_at >= 0 && rendered && (int)frame >= s_dump_at && s_dump_left > 0) {
        const char *tmpl = getenv("OPENPETE_PT_DUMP_PATH");
        char path[1024] = {0};
        if (tmpl && *tmpl) snprintf(path, sizeof path, tmpl, (unsigned)stamp);
        if (path[0]) {
            size_t cap = (size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u;
            unsigned char *buf = cap ? malloc(cap) : NULL;
            int w = 0, h = 0;
            if (buf && rhi_read_offscreen(s_nr.dev, buf, cap, &w, &h)) {
                for (size_t i = 0; i < (size_t)w * (size_t)h; i++) buf[i * 4 + 3] = 0xFF;
                openpete_write_png_rgba(path, buf, w, h);
                fprintf(stderr, "[pt-dump] scene stamp=%08x frame=%u freeze=%08x -> %s\n", stamp, frame, freeze, path);
            }
            free(buf);
            if (s_nr.frozen_tex) {
                int fw = 0, fh = 0; rhi_texture_extent(s_nr.frozen_tex, &fw, &fh);
                size_t fcap = (size_t)fw * (size_t)fh * 4u;
                unsigned char *fb = fcap ? malloc(fcap) : NULL;
                if (fb && rhi_read_render_target(s_nr.dev, s_nr.frozen_tex, fb, fcap, &fw, &fh)) {
                    for (size_t i = 0; i < (size_t)fw * (size_t)fh; i++) fb[i * 4 + 3] = 0xFF;
                    char fpath[1024]; snprintf(fpath, sizeof fpath, "%s.frozen.png", path);
                    openpete_write_png_rgba(fpath, fb, fw, fh);
                    fprintf(stderr, "[pt-dump] frozen_tex %dx%d valid=%d -> %s\n", fw, fh, (int)s_nr.frozen_valid, fpath);
                }
                free(fb);
            }
        }
        if (--s_dump_left <= 0) s_dump_at = -1;
    }
}

static int render_scene_impl(int slot, int stage, uint64_t record_pts_us)
{
    if (!s_nr.enabled || s_nr.headless || slot < 0 || slot >= SCENE_RING) return 0;
    scene_slot_t *sl = &s_ring[slot];
    rhi_set_present_blocking(s_nr.dev, true);
    texmap_apply_pending();
    notex_apply_pending();
    truescope_apply_pending();
    s_ps_slot = sl;
    openpete_mods_material_view_set(&sl->matview);
    s_stage_target = stage;
    const uint64_t before = rhi_frame_serial(s_nr.dev);
    render_frame_common(record_pts_us, false);
    const uint64_t after = rhi_frame_serial(s_nr.dev);
    { static int sync = -1;   /* OPENPETE_PT_SYNC=1 (diagnostic): device idle after every render */
      if (sync < 0) { const char *e = getenv("OPENPETE_PT_SYNC"); sync = (e && *e && *e != '0'); }
      if (sync) rhi_wait_idle(s_nr.dev); }
    { static int maxn = -1;   /* OPENPETE_PT_DUMP_MAXN: only scenes with at most this many tris */
      if (maxn < 0) { const char *e = getenv("OPENPETE_PT_DUMP_MAXN"); maxn = (e && *e) ? atoi(e) : 1 << 30; }
      if (sl->n <= maxn) pt_dump_maybe(after != before, sl->frame, sl->stamp, sl->freeze); }
    s_stage_target = -1;
    rhi_set_present_blocking(s_nr.dev, false);
    openpete_mods_material_view_set(NULL);
    s_ps_slot = NULL;
    const int did = after != before && s_last_frame_presented;
    /* PRESENTED here means "its GPU reads are in flight under render serial
     * `after`": the retirement window is keyed on render frames, whether the
     * frame went to the swapchain or to a stage image. */
    SDL_LockMutex(ring_mx());
    if (did) { sl->state = SS_PRESENTED; sl->presented_serial = after; s_ring_census.presented++; }
    else      sl->state = SS_FREE;
    SDL_UnlockMutex(s_ring_mx);
    return did;
}

int openpete_native_render_scene(int slot, uint64_t record_pts_us)
{
    return render_scene_impl(slot, -1, record_pts_us);
}

int openpete_native_render_scene_to_stage(int slot, int stage, uint64_t record_pts_us)
{
    return render_scene_impl(slot, stage, record_pts_us);
}

int openpete_native_present_stage_acquire(void)
{
    if (!s_nr.enabled || s_nr.headless || !s_nr.dev) return 0;
    rhi_set_present_blocking(s_nr.dev, true);
    const int ok = rhi_present_stage_acquire(s_nr.dev) ? 1 : 0;
    rhi_set_present_blocking(s_nr.dev, false);
    return ok;
}

int openpete_native_present_stage(int stage)
{
    if (!s_nr.enabled || s_nr.headless || !s_nr.dev) return 0;
    rhi_set_present_blocking(s_nr.dev, true);
    const int ok = rhi_present_stage_submit(s_nr.dev, stage) ? 1 : 0;
    rhi_set_present_blocking(s_nr.dev, false);
    return ok;
}

/* The calling thread takes ownership of the display (seam fence): the
 * present thread as it engages, the host loop as a drain hands it back. */
void openpete_native_display_claim(void)
{
    if (s_nr.dev) rhi_display_claim(s_nr.dev);
}

/* Release a swapchain acquire held for a staged present that will never be
 * submitted (the display is being handed back to the inline path). */
int openpete_native_present_stage_release(void)
{
    if (!s_nr.enabled || s_nr.headless || !s_nr.dev) return 1;
    rhi_set_present_blocking(s_nr.dev, true);
    const int ok = rhi_present_stage_release(s_nr.dev) ? 1 : 0;
    rhi_set_present_blocking(s_nr.dev, false);
    return ok;
}

int      openpete_native_stage_count(void) { return s_nr.dev ? rhi_stage_count() : 0; }
uint32_t openpete_native_stage_gen(void)   { return s_nr.dev ? rhi_stage_gen(s_nr.dev) : 0; }

static void render_frame_common(uint64_t record_pts_us, bool have_ref)
{
    s_last_frame_presented = 0;
    ps_apply_geometry();
    /* Detect window-size changes WITHOUT polling SDL events (the host loop
     * owns the event queue, and stealing events would starve PsyCross input). */
    int pw = 0, ph = 0;
    if (SDL_GetWindowSizeInPixels(s_nr.window, &pw, &ph) && pw > 0 && ph > 0 &&
        (pw != s_nr.win_w || ph != s_nr.win_h)) {
        /* Wait for the size to SETTLE before adopting it. A live resize walks
         * the width one pixel at a time, and adopting each step rebuilds the
         * SSAA scene target and the depth pre-pass every frame — dozens of
         * full teardowns per drag. Each is expensive enough to cost the sim a
         * tick, and the presenting thread may be the sim thread. Render at the old
         * size until the user pauses, then rebuild once. */
        Uint64 now_ns = SDL_GetTicksNS(); /* clock-guard: wall */ /* resize debounce */
        bool discrete = s_nr.resize_immediate_at_ns &&
                        now_ns - s_nr.resize_immediate_at_ns <
                            NR_RESIZE_IMMEDIATE_GRACE_NS;
        if (discrete) {
            /* How long the display was frozen for, in one number: both settle
             * windows used to run in series here, at 150 ms each. */
            OPL_INFO_F("native", "fullscreen: %dx%d adopted %.1f ms after the "
                       "toggle", pw, ph,
                       (double)(now_ns - s_nr.resize_immediate_at_ns) / 1e6);
            s_nr.resize_immediate_at_ns = 0;
            s_nr.win_w = s_nr.pending_w = pw;
            s_nr.win_h = s_nr.pending_h = ph;
            /* Tell the RHI it is discrete too, or its own settle window runs
             * in series with this one and the two sum. */
            rhi_notify_resize_immediate(s_nr.dev);
        } else if (pw != s_nr.pending_w || ph != s_nr.pending_h) {
            s_nr.pending_w = pw;
            s_nr.pending_h = ph;
            s_nr.pending_since_ns = now_ns;
        } else if (now_ns - s_nr.pending_since_ns >= NR_RESIZE_SETTLE_NS) {
            s_nr.win_w = pw;
            s_nr.win_h = ph;
            rhi_notify_resize(s_nr.dev);
        }
    }


    /* Rolling 0.5s FPS window for the overlay. Counts actual presents on
     * whichever thread makes them (inline, or the present thread under
     * eager production), so it reflects the true on-screen present rate. */
    {
        uint64_t now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency(); /* clock-guard: wall */ /* FPS window */
        if (s_fps_win_start == 0) s_fps_win_start = now;
        s_fps_frames++;
        uint64_t el = now - s_fps_win_start;
        if (freq > 0 && el >= freq / 2u) {
            s_fps_value = (int)((double)s_fps_frames * (double)freq / (double)el + 0.5);
            s_fps_win_start = now; s_fps_frames = 0;
        }
    }
    /* Reference resolution (the display shape for the window aspect-fit + the
     * native_rtt/freeze size) normally comes from PsyCross's present FBO via
     * sync_reference_texture. In pure vk PsyCross no longer rasterizes the OT, so
     * that returns nothing and ref_w/ref_h would stay 0 → update_layout bails →
     * compose draws nothing → black. Derive it from GAMESTATE instead: the
     * bars-free PSX display is 4:3 corrected for the vertical overscan crop
     * (aspect = 4/3 * disp_h/clip_h), at a ~2x-internal reference so the
     * off-display RT stays crisp. Only the aspect matters to the layout (fit_cell
     * cancels the absolute scale); the size feeds native_rtt.
     *
     * Gated on the SCENE predicate, and it has to be. In PsyCross display mode
     * the reference IS the picture, and its extent comes from PsyCross's
     * present FBO, which is sized from the window and the game's overscan crop
     * (4/3 * disp_h/clip_h). The estimate below lands on the same numbers only
     * when clip_h is 224 — Spyro 1. For a 216-row clip it derives 640x432
     * against PsyCross's 664x448, so a frame where PsyCross produced nothing
     * (a load, or before its first present) would overwrite the real extent
     * with a wrong one, and the next real frame would destroy and rebuild
     * ref_tex under rhi_wait_idle. Leave the last known reference extent
     * alone instead; update_layout is change-keyed and will not churn. */
    if (!have_ref && s_nr.scene_live) {
        int dh = s_nr.disp_h > 0 ? s_nr.disp_h : 240;
        int ch = s_nr.clip_h > 0 ? s_nr.clip_h : 224;
        int rh = ch * 2;
        int rw = (int)((double)rh * (4.0 / 3.0) * (double)dh / (double)ch + 0.5);
        if (rw > 0 && rh > 0) { s_nr.ref_w = rw; s_nr.ref_h = rh; }
    }
    update_layout();
    ensure_native_rtt(true);   /* native_rtt is the canonical frame in every mode */
    ensure_ss_rtt();           /* SSAA supersampled scene target (--ssaa>1, vk mode) */
    overlay_layout_update();   /* anchor ledger: where each chrome block sits this present */
    ensure_ovl_ss();           /* stats-overlay supersample target + composite quad */
    ensure_menu_ss();          /* settings-menu glyph supersample (lazy, while open) */
    ensure_osd_ss();           /* OSD toast slot grid (lazy, once a toast exists) */
    ensure_opaque_depth();     /* WORLD-opaque depth pre-pass target (shadow decal) */
    ensure_terrain_key_targets(); /* key-contest images: bound by every scene-pipe draw, chrome first */

    /* Present-path step trace (OPENPETE_LOAD_CENSUS=1): bisects a present that
     * never returns (the freeze-point hang) to the exact call. Terse; one
     * letter per step: B=begin_frame done, C=compose done, S=swapchain blit
     * done, E=end_frame done. A truncated sequence names the hung call. */
    static int s_ptrace = -1;
    if (s_ptrace < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); s_ptrace = (e && *e && *e != '0'); }

    const int pt_on = openpete_phase_timers_on();
    uint64_t pt_t0 = pt_on ? openpete_phase_now_us() : 0, pt_t1;
    int bf_ok;
    { OP_PROF_ACCUM_SCOPE("gpu.beginframe");   /* fence wait + swapchain acquire */
      bf_ok = s_stage_target >= 0 ? rhi_begin_frame_staged(s_nr.dev) : rhi_begin_frame(s_nr.dev); }
    if (!bf_ok)
        return; /* swapchain recreating or window minimized — skip this present */
    openpete_native_scene_retire(rhi_frame_serial(s_nr.dev));
    /* The mod registry is read for the rest of the frame's recording. Taken
     * AFTER begin_frame and released BEFORE end_frame: those two are where a
     * present blocks on the display, and a tick-boundary mutator on the
     * other thread must not wait behind a vblank. */
    openpete_mods_registry_read_lock();
    /* Retained terrain: select against the stamp of the scene this present
     * composes (freeze/lag included), so retained content follows the same
     * timeline and stale states never repaint. */
    openpete_tercache_frame_select(scene_present_stamp());
    /* Tag this frame's GPU work with the present it will become, so the
     * frame-lagged harvest (next few presents) can bind zones back to it. */
    if (op_prof_on()) rhi_gpu_frame_token(s_nr.dev, (uint64_t)op_prof_next_present_index());
    gpu_zone_report();  /* fold in the zone results begin_frame just harvested */
    /* Pending VRAM/reference uploads ride THIS frame's command buffer (W2.2)
     * — must precede the first render pass (the depth pre-pass below). */
    rhi_gpu_zone_begin(s_nr.dev, "upload");
    { OP_PROF_ACCUM_SCOPE("upl.synced");   upload_synced_textures(); }
    { OP_PROF_ACCUM_SCOPE("upl.hd");       hd_flush_pending(); }
    { OP_PROF_ACCUM_SCOPE("upl.hdfold");   openpete_native_hdfold_flush(); }
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

    s_overlay_live = 1;                       /* this is the live present → allow the stats overlay */
    /* Render the overlay glyphs at OVL_SS_FACTOR× into ovl_ss now — its own
     * render pass, so it must precede compose_variant's offscreen pass.
     * compose's draw_perf_overlay then composites the downsampled result. */
    rhi_gpu_zone_begin(s_nr.dev, "ovl_ss");
    prepare_perf_overlay_ss();
    rhi_gpu_zone_end(s_nr.dev);
    rhi_gpu_zone_begin(s_nr.dev, "menu_ss");
    prepare_menu_ss();         /* menu glyphs at MENU_SS_FACTOR× → menu_ss (own pass) */
    rhi_gpu_zone_end(s_nr.dev);
    rhi_gpu_zone_begin(s_nr.dev, "osd_ss");
    prepare_osd_ss();          /* toast slot grid at OSD_SS_FACTOR× (own pass) */
    rhi_gpu_zone_end(s_nr.dev);
    rhi_gpu_zone_begin(s_nr.dev, "compose");
    compose_variant();
    rhi_gpu_zone_end(s_nr.dev);
    const int overlay_was_live = s_overlay_live;
    s_overlay_live = 0;                       /* screenshot/dump composes must not draw it */

    /* Mod postfx chain (COMPOSITE point). Runs on EVERY present (canonical +
     * intermediate). No outer GPU zone: the executor emits ONE ZONE PER
     * PASS NAMED BY THE OWNING MOD (zones don't nest), so a slow shader
     * shows up BY MOD ID in the [gpu-pass] report — the S9 budget
     * attribution. */
    {
        pfx_drain_retired();
        int pw = 0, ph = 0;
        rhi_swapchain_extent(s_nr.dev, &pw, &ph);
        (void)native_postfx_run(0 /* COMPOSITE */, NULL, pw, ph);
        /* Dev overlay over the FILTERED composite: compose_variant skips
         * it whenever this chain is active (same predicate), so screen
         * filters (CRT masks) never render over the config panel — draw
         * it here in a load pass on the chain's output. A chain whose
         * passes all failed still lands here: off_image was restored for
         * the blit and the load pass draws on that. */
        if (native_postfx_point_active(0) && overlay_was_live &&
            openpete_devui_frame_wanted()) {
            rhi_begin_offscreen_load(s_nr.dev);
            openpete_devui_render(s_nr.dev);
            rhi_end_offscreen(s_nr.dev);
        }
    }
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_COMPOSE, pt_t1 - pt_t0); pt_t0 = pt_t1;
                 openpete_phase_set_count(OPPC_TRIS, s_ps_slot ? s_ps_slot->n : s_scene_n); }
    if (s_ptrace) { fprintf(stderr, "C"); fflush(stderr); }

    if (s_stage_target < 0) {
        rhi_gpu_zone_begin(s_nr.dev, "blit");
        rhi_blit_to_swapchain(s_nr.dev);
        rhi_gpu_zone_end(s_nr.dev);
    }
    if (s_ptrace) { fprintf(stderr, "S"); fflush(stderr); }

    /* --record: capture the on-screen native frame via the async ring (no-op
     * unless recording is active). MUST run before end_frame, while this frame
     * slot is still current — the copy is appended to the frame's command
     * buffer. */
    /* Scene-gated: without a native scene this offscreen is an aspect-fitted
     * blit of PsyCross's frame, and run.c records that mode from the present
     * FBO instead. Capturing here too would put two frames per present into
     * one encoder. */
    if (openpete_record_active() && s_nr.scene_live) {
        rhi_gpu_zone_begin(s_nr.dev, "record");
        record_cycle(record_pts_us);
        rhi_gpu_zone_end(s_nr.dev);
    }

    openpete_mods_registry_read_unlock();
    { OP_PROF_ACCUM_SCOPE("gpu.endframe");   /* submit + present (or submit into a stage) */
      if (s_stage_target >= 0) rhi_end_frame_to_stage(s_nr.dev, s_stage_target);
      else                     rhi_end_frame(s_nr.dev); }
    s_last_frame_presented = 1;
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_GPU_SYNC, pt_t1 - pt_t0);
                 openpete_phase_set_count(OPPC_DRAWS, (int)rhi_draw_calls(s_nr.dev));
                 phase_present_done(); }
    if (s_ptrace) { fprintf(stderr, "E\n"); fflush(stderr); }

}

/* Insert a suffix before the extension: ("shot-42.png","-vk") -> "shot-42-vk.png". */
static void path_with_suffix(const char *base, const char *suffix,
                             char *out, size_t cap)
{
    const char *dot = strrchr(base, '.');
    if (dot) snprintf(out, cap, "%.*s%s%s", (int)(dot - base), base, suffix, dot);
    else     snprintf(out, cap, "%s%s", base, suffix);
}

/* cap_out/cap_w/cap_h: when cap_out is non-NULL the composed native RGBA frame
 * is handed to the caller (malloc'd, top-down, alpha forced opaque) and no PNG
 * is written — the in-memory form of the same compose the -vk dump writes. */
static int native_render_screenshot_all_impl(const char *base, unsigned char **cap_out,
                                             int *cap_w, int *cap_h);

/* Capture-time freeze-backdrop maintenance. Render the CURRENT world frame into
 * shot_frozen so a later freeze-frame dump has the frozen-world backdrop to tint
 * green (the live path's frozen_tex snapshot is window-only). Called each present
 * during a headless capture run; a no-op on freeze frames (nothing to snapshot —
 * the world groups are empty) and on empty scenes, so once the game pauses,
 * shot_frozen holds the last world frame. cap_wf matches the capture's widen so
 * the backdrop lines up 1:1 with the widescreen dump. */
void openpete_native_render_snapshot_frozen(float cap_wf)
{
    if (!s_nr.enabled) return;
    ps_apply_geometry();
    if (cap_wf < 1.0f) cap_wf = 1.0f;
    /* Only WORLD frames: a freeze frame has no world geometry, and snapshotting it
     * would overwrite the good backdrop with an empty one. */
    int scene_n; uint32_t sbg, sfade, sfreeze;
    (void)scene_present_buf(&scene_n, &sbg, &sfade, &sfreeze, NULL, NULL);
    if (scene_n <= 0 || (sfreeze & (1u << 31))) return;
    /* Retained terrain must land in the frozen backdrop too. */
    openpete_tercache_frame_select(scene_present_stamp());

    /* Derive the reference display shape from gamestate (headless has no live
     * frame to source it from), same as the screenshot compose fallback. */
    if (s_nr.ref_w <= 0 || s_nr.ref_h <= 0) {
        int dh = s_nr.disp_h > 0 ? s_nr.disp_h : 240;
        int ch = s_nr.clip_h > 0 ? s_nr.clip_h : 224;
        int rh = ch * 2;
        int rw = (int)((double)rh * (4.0 / 3.0) * (double)dh / (double)ch + 0.5);
        if (rw > 0 && rh > 0) { s_nr.ref_w = rw; s_nr.ref_h = rh; }
    }
    if (s_nr.ref_w <= 0 || s_nr.ref_h <= 0) return;
    const int Wc = (int)((double)s_nr.ref_w * (double)cap_wf + 0.5);
    if (!(s_nr.shot_frozen && s_nr.shot_frozen_w == Wc && s_nr.shot_frozen_h == s_nr.ref_h)) {
        rhi_wait_idle(s_nr.dev);
        if (s_nr.shot_frozen) rhi_destroy_texture(s_nr.dev, s_nr.shot_frozen);
        s_nr.shot_frozen = rhi_create_render_target(s_nr.dev, (uint32_t)Wc, (uint32_t)s_nr.ref_h);
        s_nr.shot_frozen_w = Wc; s_nr.shot_frozen_h = s_nr.ref_h; s_nr.shot_frozen_valid = false;
    }
    if (!s_nr.shot_frozen) return;

    sync_vram_texture();
    ensure_native_rtt(true);
    ensure_opaque_depth();
    ensure_terrain_key_targets();
    uint32_t bg = s_drawenv_clear ? s_drawenv_clear : sbg;
    const float br  = (float)((bg >>  0) & 0xFF) / 255.0f;
    const float bgc = (float)((bg >>  8) & 0xFF) / 255.0f;
    const float bb  = (float)((bg >> 16) & 0xFF) / 255.0f;
    float prev_wf = s_render_freeze_wf;
    s_render_freeze_wf = cap_wf;
    if (rhi_begin_frame(s_nr.dev)) {
        upload_synced_textures();
        hd_flush_pending();
        openpete_native_hdfold_flush();
        native_depth_prepass();
        { int kw, kh; rhi_texture_extent(s_nr.shot_frozen, &kw, &kh);
          native_terrain_key_pass(1, kw, kh); }
        rhi_begin_render_target(s_nr.dev, s_nr.shot_frozen, br, bgc, bb, 1.0f);
        draw_native_scene(true);
        rhi_end_render_target(s_nr.dev, s_nr.shot_frozen);
        rhi_begin_offscreen(s_nr.dev, br, bgc, bb, 1.0f);
        rhi_end_offscreen(s_nr.dev);
        rhi_blit_to_swapchain(s_nr.dev);
        rhi_end_frame(s_nr.dev);
        s_nr.shot_frozen_valid = true;
    }
    s_render_freeze_wf = prev_wf;
}

/* Widescreen (Hor+) native-only -vk dump. Renders the scene at the widened
 * extent into the dedicated shot_rtt (W*cap_wf × H), matching the extraction-
 * time widen (openpete_native_widescreen_factor now honors --aspect headless),
 * and writes the single -vk PNG. Content-sized (no window letterbox); the ab/
 * diff variants are not produced (there is no PsyCross reference in native-only
 * mode). Returns 0 on success. base is the caller's screenshot path stem. */
static int native_shot_widescreen(const char *base, int H, float cap_wf,
                                  unsigned char **cap_out, int *cap_w, int *cap_h)
{
    const int Wc = (int)((double)s_nr.ref_w * (double)cap_wf + 0.5);
    rhi_texture *rt = ensure_shot_rtt(Wc, H);
    if (!rt) return 1;

    /* Background = the gamestate draw-env clear (matching the 4:3 compose). */
    int scene_n; uint32_t scene_bg, scene_fade;
    (void)scene_present_buf(&scene_n, &scene_bg, &scene_fade, NULL, NULL, NULL);
    uint32_t bg = ps_drawenv_clear() ? ps_drawenv_clear() : scene_bg;
    const float br  = (float)((bg >>  0) & 0xFF) / 255.0f;
    const float bgc = (float)((bg >>  8) & 0xFF) / 255.0f;
    const float bb  = (float)((bg >> 16) & 0xFF) / 255.0f;

    /* The full-extent scene map divides clip-x by s_render_freeze_wf, so the 4:3
     * content lands centred and the Hor+ reveal fills the widened sides — the
     * same divide the depth pre-pass already applies (it reads the widen from
     * openpete_native_widescreen_factor). Set it around the whole compose so the
     * pre-pass affine and the scene draw agree. */
    float prev_wf = s_render_freeze_wf;
    s_render_freeze_wf = cap_wf;
    for (int attempt = 0; ; attempt++) {
        if (!rhi_begin_frame(s_nr.dev)) { s_render_freeze_wf = prev_wf; return 1; }
        upload_synced_textures();
        hd_flush_pending();
        openpete_native_hdfold_flush();
        native_depth_prepass();
        /* Black clear + mod overlay + SCENE point: live-offscreen parity,
         * same rationale as the 4:3 dump compose (see screenshot_all_impl). */
        { int kw, kh; rhi_texture_extent(rt, &kw, &kh);
          native_terrain_key_pass(1, kw, kh); }
        rhi_begin_render_target(s_nr.dev, rt, 0.0f, 0.0f, 0.0f, 1.0f);
        draw_native_scene(true);
        draw_mod_overlay();
        rhi_end_render_target(s_nr.dev, rt);
        native_postfx_run(1 /* SCENE */, rt, Wc, H);
        native_postfx_run(0 /* COMPOSITE */, rt, Wc, H);
        rhi_begin_offscreen(s_nr.dev, br, bgc, bb, 1.0f);
        rhi_end_offscreen(s_nr.dev);
        rhi_blit_to_swapchain(s_nr.dev);
        rhi_end_frame(s_nr.dev);
        if (attempt >= 8 || (s_hd_pending_n == 0 &&
                             openpete_native_hdfold_pending() == 0)) break;
    }
    s_render_freeze_wf = prev_wf;

    const size_t px = (size_t)Wc * (size_t)H;
    unsigned char *nat = malloc(px * 4);
    if (!nat) return 1;
    int nw = 0, nh = 0;
    if (!rhi_read_render_target(s_nr.dev, rt, nat, px * 4, &nw, &nh)) { free(nat); return 1; }
    for (size_t i = 0; i < px; i++) nat[i * 4 + 3] = 0xFF;   /* GPU blend alpha → opaque */

    if (cap_out) {   /* in-memory readback: hand over the buffer, write no PNG */
        *cap_out = nat;
        if (cap_w) *cap_w = Wc;
        if (cap_h) *cap_h = H;
        return 0;
    }

    int rc = 0;
    char path[1100];
    path_with_suffix(base, "-vk", path, sizeof(path));
    if (openpete_write_png_rgba(path, nat, Wc, H) == 0)
        OPL_INFO_F("native", "vk screenshot %dx%d (widescreen wf=%.3f) -> %s", Wc, H, (double)cap_wf, path);
    else rc = 1;
    free(nat);
    return rc;
}

/* Wrapper: the screenshot compose uses the reference-aligned scene depth (see
 * s_scene_shot_compose) so -vk shows the SAME game frame as the -og
 * PsyCross capture beside it. Flag set/cleared here so the impl's early
 * returns can't leak it into the live present path. */
int openpete_native_render_screenshot_all(const char *base)
{
    /* Headless never runs openpete_native_render_frame, so the truescope
     * re-bake must happen here, outside the compose's command recording. */
    truescope_apply_pending();
    s_scene_shot_compose = 1;
    s_scene_shot_lag = s_nr.headless ? 1 : 0;
    /* Retained terrain rides the same scene selection: the lagged canonical
     * scene is exactly the tercache LIVE state (the staged/live promote
     * mirrors the canonical lag). */
    openpete_tercache_frame_select(scene_present_stamp());
    if (scene_pos_sig_on()) scene_pos_sig_report(1);   /* the scene this dump composes */
    int r = native_render_screenshot_all_impl(base, NULL, NULL, NULL);
    s_scene_shot_compose = 0;
    s_scene_shot_lag = 0;
    return r;
}

/* In-memory twin of openpete_native_render_screenshot_all: same compose, same
 * bracket, same capture target and widen — the frame lands in a malloc'd RGBA
 * buffer instead of a PNG. The caller owns *out. Returns 0 on success. */
int openpete_native_render_capture_rgba(unsigned char **out, int *w, int *h)
{
    if (!out) return 1;
    *out = NULL;
    truescope_apply_pending();
    s_scene_shot_compose = 1;
    s_scene_shot_lag = s_nr.headless ? 1 : 0;
    openpete_tercache_frame_select(scene_present_stamp());
    int r = native_render_screenshot_all_impl("ident-audit", out, w, h);
    s_scene_shot_compose = 0;
    s_scene_shot_lag = 0;
    if (r != 0 && *out) { free(*out); *out = NULL; }
    return r || !*out;
}

/* Blocking off_image readback → PNG. Shared by the A/B present capture and the
 * live -vk screenshot; `what` labels the log line. Returns 0 on success. */
static int shot_write_offscreen(const char *path, const char *what)
{
    size_t cap = (size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u;
    if (cap == 0) return 1;
    unsigned char *buf = malloc(cap);
    if (!buf) return 1;
    int w = 0, h = 0, rc = 1;
    if (rhi_read_offscreen(s_nr.dev, buf, cap, &w, &h)) {
        /* Blend ops write real alpha into off_image (driver-dependent; opaque
         * on llvmpipe, fractional on discrete GPUs). The display ignores it,
         * but a PNG keeps it and viewers then show transparency checkerboard —
         * force opaque so the file shows exactly what the window showed. */
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
            buf[i * 4 + 3] = 0xFF;
        rc = openpete_write_png_rgba(path, buf, w, h);
    }
    free(buf);
    if (rc == 0)
        OPL_INFO_F("native", "%s screenshot %dx%d -> %s", what, w, h, path);
    return rc;
}

static int native_render_screenshot_all_impl(const char *base, unsigned char **cap_out,
                                             int *cap_w, int *cap_h)
{
    ps_apply_geometry();
    if (!s_nr.enabled || !base) return 1;

    /* Live-present -vk (pure-vk window runs): off_image still holds the frame
     * the user is seeing — the present just blitted it to the swapchain — so
     * -vk is a ground-truth READBACK of the presented image, read BEFORE the
     * recomposes below open new frames over it. The recompose is a
     * reconstruction (canonical lagged scene, ref-sized RTT) and diverges from
     * the window whenever a live-only path is in play (sub-tick interp scenes,
     * SSAA, overlays). Not taken when:
     *   - headless: openpete_native_render_frame early-returns, no present;
     *   - intermediate capture (s_scene_shot_nodelay): the caller re-extracted
     *     a sub-tick scene precisely to have it RECOMPOSED deterministically. */
    bool live_vk = false;
    if (!s_nr.headless && !s_scene_shot_nodelay && !cap_out) {
        char lp[1100];
        path_with_suffix(base, "-vk", lp, sizeof(lp));
        live_vk = shot_write_offscreen(lp, "vk (live present)") == 0;
    }

    /* PsyCross display mode: the offscreen the present just blitted holds
     * PsyCross's frame with the mod HUD, postfx and chrome over it, which is
     * exactly what the window showed and the only image this mode has. The
     * recompose below builds a native scene at reference resolution; there is
     * no native scene here, so it would overwrite a truthful capture with a
     * blank one. */
    if (!s_nr.scene_live)
        return live_vk ? 0 : 1;

    /* Re-sync VRAM + reference so the dump matches the just-presented frame. */
    sync_vram_texture();
    bool have_ref = sync_reference_texture();
    /* Without a PsyCross reference (pure-vk / --screenshot-native-only, and
     * always under --headless where openpete_native_render_frame — which would
     * otherwise derive this — early-returns), ref_w/ref_h stay 0 and the dump
     * silently bails below. Derive the display shape from GAMESTATE exactly as
     * the live frame path does (4:3 corrected for vertical overscan, ~2x
     * internal). */
    if (!have_ref) {
        int dh = s_nr.disp_h > 0 ? s_nr.disp_h : 240;
        int ch = s_nr.clip_h > 0 ? s_nr.clip_h : 224;
        int rh = ch * 2;
        int rw = (int)((double)rh * (4.0 / 3.0) * (double)dh / (double)ch + 0.5);
        if (rw > 0 && rh > 0) { s_nr.ref_w = rw; s_nr.ref_h = rh; }
    }
    update_layout();
    ensure_native_rtt(true);
    ensure_opaque_depth();     /* WORLD-opaque depth pre-pass target (shadow decal) */
    ensure_terrain_key_targets();
    /* The reference readback only fills in for an EMPTY native scene below;
     * without one (pure vk with PsyCross in null-GPU library mode — no FBO
     * exists at all) the dump writes the native scene as composed. ref_w/ref_h
     * are still valid: the present path derives them from gamestate when there
     * is no reference (see the !have_ref branch in
     * openpete_native_render_frame). */
    bool with_ref = have_ref && s_nr.readback != NULL;
    if (!s_nr.native_rtt || s_nr.ref_w <= 0 || s_nr.ref_h <= 0)
        return 1;

    /* Live -vk written and no reference → nothing further to build; the
     * recompose would only overwrite the truthful capture. Done. */
    if (live_vk && !with_ref)
        return 0;

    /* Widescreen (Hor+) native-only capture: render the widened view into the
     * dedicated shot_rtt and write the single -vk image. The widen comes from a
     * fixed --aspect, surfaced headless by openpete_native_widescreen_factor. */
    if (!with_ref) {
        float cap_wf = openpete_native_widescreen_factor();
        if (cap_wf > 1.0001f)
            return native_shot_widescreen(base, s_nr.ref_h, cap_wf, cap_out, cap_w, cap_h);
    }

    /* Compose -vk at the REFERENCE resolution rather than the window size, so
     * the image is the true content size with no window letterbox. The native
     * scene is rendered full-extent into the ref-sized RTT and read straight
     * back. */
    const int W = s_nr.ref_w, H = s_nr.ref_h;
    const size_t frame_px = (size_t)W * (size_t)H;

    /* Background = the gamestate draw-env clear (matching the live native render);
     * scene_bg then corner as fallbacks. See draw_native_scene for the rationale. */
    int scene_n; uint32_t scene_bg, scene_fade;
    (void)scene_present_buf(&scene_n, &scene_bg, &scene_fade, NULL, NULL, NULL);
    uint32_t bg = ps_drawenv_clear() ? ps_drawenv_clear() : scene_bg;
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
        openpete_native_hdfold_flush();
        native_depth_prepass();    /* shared WORLD-opaque depth for the shadow decal */
        /* BLACK clear — live-offscreen parity. The live present clears black and
         * relies on the scene's own bg-fill quad; clearing to the bg colour here
         * painted over exactly the class of bug where that quad is built but
         * never drawn, making the dump unable to reproduce a live-visible black
         * region. Where the quad exists (every normal frame) the output is
         * identical. */
        { int kw, kh; rhi_texture_extent(s_nr.native_rtt, &kw, &kh);
          native_terrain_key_pass(1, kw, kh); }
        rhi_begin_render_target(s_nr.dev, s_nr.native_rtt, 0.0f, 0.0f, 0.0f, 1.0f);
        draw_native_scene(true);
        /* Mod draw-service content (3D meshes under the mod HUD) — the live
         * compose draws these into the offscreen after the scene; without them
         * the dump showed a mod-free frame while the window showed mod content.
         * Committed per tick and re-read here, so drawing after the live present
         * is idempotent. NB the live path runs the SCENE postfx point BEFORE
         * these; the RT pass can't re-open with a load-op, so the dump runs the
         * SCENE chain after them — mod content gets chained into SCENE passes
         * here (visible only when a SCENE-point pass AND a mod overlay are
         * both active). */
        draw_mod_overlay();
        rhi_end_render_target(s_nr.dev, s_nr.native_rtt);
        /* Postfx over the dump compose too — this path bypasses off_image, so
         * without these calls a headless/screenshot-all gate would pass
         * vacuously (plan-review trap). SCENE point first (--raster-quantize,
         * mod scene shaders — previously dump-bypassed), then COMPOSITE. */
        native_postfx_run(1 /* SCENE */, s_nr.native_rtt, W, H);
        native_postfx_run(0 /* COMPOSITE */, s_nr.native_rtt, W, H);
        rhi_begin_offscreen(s_nr.dev, br, bgc, bb, 1.0f);
        rhi_end_offscreen(s_nr.dev);
        rhi_blit_to_swapchain(s_nr.dev);
        rhi_end_frame(s_nr.dev);
        /* Self-heal a first sighting: an HD/mod texture seen for the first
         * time mid-draw only QUEUES its upload and draws nothing for those
         * faces (hd_pending_add). Live presents absorb that as one SD frame,
         * but in a headless run the only frames ARE these dumps — a single
         * screenshot bakes the gap into the PNG (mod-textured faces
         * invisible).
         * Flush-and-redraw until nothing new is queued: a full HD pack can
         * put hundreds of files in view at once, so one retry isn't enough. */
        if (attempt >= 8 || (s_hd_pending_n == 0 &&
                             openpete_native_hdfold_pending() == 0)) break;
    }

    unsigned char *nat = malloc(frame_px * 4);          /* native, top-down    */
    unsigned char *ref = with_ref ? malloc(frame_px * 4)     : NULL;  /* reference, top-down  */
    if (!nat || (with_ref && !ref)) {
        free(nat); free(ref);
        return 1;
    }

    int nw = 0, nh = 0;
    if (!rhi_read_render_target(s_nr.dev, s_nr.native_rtt, nat, frame_px * 4, &nw, &nh)) {
        free(nat); free(ref);
        return 1;
    }
    /* RT alpha is whatever the blend ops left (fractional on discrete GPUs) —
     * force opaque so the PNGs don't show viewer transparency checkerboard. */
    for (size_t i = 0; i < frame_px; i++) nat[i * 4 + 3] = 0xFF;
    if (cap_out) {   /* in-memory readback: hand over the buffer, write no PNG */
        *cap_out = nat;
        if (cap_w) *cap_w = W;
        if (cap_h) *cap_h = H;
        free(ref);
        return 0;
    }
    if (with_ref) {
        /* The PsyCross readback is GL bottom-up; flip to top-down to match the RTT. */
        for (int y = 0; y < H; y++)
            memcpy(&ref[(size_t)y * W * 4],
                   &s_nr.readback[(size_t)(H - 1 - y) * W * 4], (size_t)W * 4);
        for (size_t i = 0; i < frame_px; i++) ref[i * 4 + 3] = 0xFF;
        /* Empty native scene (load/transition frames): mirror the reference so
         * -vk shows the framebuffer instead of a flat clear. */
        if (scene_n <= 0) memcpy(nat, ref, frame_px * 4);
    }

    int rc = 0;
    char path[1100];
    /* -vk: the live capture already wrote it in a pure-vk window run; only the
     * headless / intermediate composes fall back to the recompose. */
    if (!live_vk) {
        path_with_suffix(base, "-vk", path, sizeof(path));
        if (openpete_write_png_rgba(path, nat, W, H) == 0)
            OPL_INFO_F("native", "vk screenshot %dx%d -> %s", W, H, path);
        else rc = 1;
    }
    free(nat); free(ref);
    return rc;
}

int openpete_native_render_screenshot(const char *path, const char *what)
{
    ps_apply_geometry();
    if (!s_nr.enabled || !path) return 1;
    return shot_write_offscreen(path, what ? what : "offscreen");
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
    /* Sub-tick view engagement. Reported unconditionally because a run that
     * never opened it cannot support any claim about the sub-tick path, and
     * that fact is invisible in the pixels. */
    {
        uint32_t opens = openpete_subtick_view_opens();
        uint32_t spans = 0, served = 0, writes = 0;
        openpete_subtick_view_stats(&spans, &served, &writes);
        OPL_INFO_F("subtick-view",
                   "presents=%u staged_spans=%u served_reads=%u (last present); "
                   "cpu_writes_while_open=%u",
                   opens, spans, served, writes);
        openpete_subtick_view_report_writes();
    }

    if (!s_nr.enabled)
        return;
    rhi_wait_idle(s_nr.dev);
    openpete_devui_shutdown();   /* imgui backend objects die before the device */
    openpete_tercache_shutdown(s_nr.dev);
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
    {
        openpete_mods_postfx_retire_all();   /* incl. disabled passes' caches */
        openpete_mods_material_retire_all();
        void* dead[PFX_MAX_PASSES];
        int n;
        while ((n = openpete_mods_postfx_take_retired(dead, PFX_MAX_PASSES)) > 0)
            for (int i = 0; i < n; i++)
                rhi_destroy_pipeline(s_nr.dev, (rhi_pipeline*)dead[i]);
    }
    if (s_nr.ref_tex)      rhi_destroy_texture(s_nr.dev, s_nr.ref_tex);
    if (s_nr.native_rtt)   rhi_destroy_texture(s_nr.dev, s_nr.native_rtt);
    if (s_nr.shot_rtt)     rhi_destroy_texture(s_nr.dev, s_nr.shot_rtt);
    if (s_nr.shot_frozen)  rhi_destroy_texture(s_nr.dev, s_nr.shot_frozen);
    if (s_nr.frozen_tex)   rhi_destroy_texture(s_nr.dev, s_nr.frozen_tex);
    if (s_nr.ss_rtt)       rhi_destroy_texture(s_nr.dev, s_nr.ss_rtt);
    if (s_nr.opaque_depth) rhi_destroy_texture(s_nr.dev, s_nr.opaque_depth);
    if (s_nr.occl_grid) rhi_destroy_texture(s_nr.dev, s_nr.occl_grid);
    if (s_nr.occl_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.occl_pipe);
    if (s_nr.prepass_vb)   rhi_destroy_buffer(s_nr.dev, s_nr.prepass_vb);
    if (s_nr.prepass_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.prepass_pipe);
    if (s_nr.terrain_key_depth) rhi_destroy_texture(s_nr.dev, s_nr.terrain_key_depth);
    if (s_nr.moby_key_depth)    rhi_destroy_texture(s_nr.dev, s_nr.moby_key_depth);
    if (s_nr.actor_key_depth)   rhi_destroy_texture(s_nr.dev, s_nr.actor_key_depth);
    if (s_nr.moby_rank_rt)      rhi_destroy_texture(s_nr.dev, s_nr.moby_rank_rt);
    if (s_nr.terrain_key_vb)   rhi_destroy_buffer(s_nr.dev, s_nr.terrain_key_vb);
    if (s_nr.terrain_key_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.terrain_key_pipe);
    if (s_nr.terrain_key_tex_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.terrain_key_tex_pipe);
    if (s_nr.moby_rank_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.moby_rank_pipe);
    if (s_nr.moby_rank_cutout_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.moby_rank_cutout_pipe);
    if (s_nr.gouraud_sub_ztest_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_sub_ztest_pipe);
    if (s_nr.gouraud_sub_fade_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_sub_fade_pipe);
    if (s_nr.ovl_ss)       rhi_destroy_texture(s_nr.dev, s_nr.ovl_ss);
    if (s_nr.ovl_quad_vb)  rhi_destroy_buffer(s_nr.dev, s_nr.ovl_quad_vb);
    if (s_nr.menu_ss)      rhi_destroy_texture(s_nr.dev, s_nr.menu_ss);
    if (s_nr.osd_ss)       rhi_destroy_texture(s_nr.dev, s_nr.osd_ss);
    if (s_nr.menu_quad_vb) rhi_destroy_buffer(s_nr.dev, s_nr.menu_quad_vb);
    if (s_nr.ovl_resolve_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.ovl_resolve_pipe);
    if (s_nr.vram_tex)     rhi_destroy_texture(s_nr.dev, s_nr.vram_tex);
    if (s_nr.fs_quad_vb)   rhi_destroy_buffer(s_nr.dev, s_nr.fs_quad_vb);
    if (s_nr.overlay_vb)   rhi_destroy_buffer(s_nr.dev, s_nr.overlay_vb);
    if (s_nr.mod_ovl_vb)   rhi_destroy_buffer(s_nr.dev, s_nr.mod_ovl_vb);
    if (s_nr.menu_vb)      rhi_destroy_buffer(s_nr.dev, s_nr.menu_vb);
    if (s_nr.menu_glyph_vb) rhi_destroy_buffer(s_nr.dev, s_nr.menu_glyph_vb);
    if (s_nr.osd_glyph_vb) rhi_destroy_buffer(s_nr.dev, s_nr.osd_glyph_vb);
    if (s_nr.osd_quad_vb)  rhi_destroy_buffer(s_nr.dev, s_nr.osd_quad_vb);
    if (s_nr.osd_panel_vb) rhi_destroy_buffer(s_nr.dev, s_nr.osd_panel_vb);
    if (s_nr.scene_vb)     rhi_destroy_buffer(s_nr.dev, s_nr.scene_vb);
    if (s_nr.panels_vb)    rhi_destroy_buffer(s_nr.dev, s_nr.panels_vb);
    if (s_nr.freeze_overlay_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.freeze_overlay_pipe);
    if (s_nr.resolve_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.resolve_pipe);
    if (s_nr.scene_tex_hud_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_hud_pipe);
    if (s_nr.hd_tex_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.hd_tex_pipe);
    if (s_nr.hd_cutout_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.hd_cutout_pipe);
    if (s_nr.hd_cutout_mark_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.hd_cutout_mark_pipe);
    if (s_nr.gouraud_punch_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_punch_pipe);
    if (s_nr.scene_tex_punch_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_punch_pipe);
    if (s_nr.hd_cutout_punch_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.hd_cutout_punch_pipe);
    if (s_nr.hd_scene_blend_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.hd_scene_blend_pipe);
    if (s_nr.hd_scene_add_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.hd_scene_add_pipe);
    hd_cache_shutdown();
    if (s_nr.mat_pack_dummy) { rhi_destroy_texture(s_nr.dev, s_nr.mat_pack_dummy); s_nr.mat_pack_dummy = NULL; }
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

    memset(&s_nr, 0, sizeof(s_nr));
    if (rhi_seam_violations())
        OPL_ERROR_F("native", "display-seam fence: %u call(s) reached the device "
                    "from a thread that did not own the display (see SEAM lines; "
                    "OPENPETE_SEAM_GUARD_FATAL=1 aborts at the first)",
                    rhi_seam_violations());
    OPL_INFO_F("native", "Vulkan native renderer window shut down");
}
