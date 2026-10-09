/* core/renderer/native_render.c - the native Vulkan renderer: window, scene
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

#include "openpete_glitch_mark.h"
#include "openpete_native_render.h"
#include "openpete_present_thread.h"
#include "openpete_pace_log.h"
#include "openpete_perf_stats.h"
#include "openpete_profile.h"   /* --profile-out forwarding shim */
#include "openpete_menu.h"
#include "openpete_overlay_anchor.h"
#include "openpete_osd.h"
#include "openpete_build_id.h"
#include "openpete_knobs.h"   /* env-epoch for cached live knobs */
#include "openpete_mutex_once.h"
#include "openpete_devui.h"
#include "openpete_mods.h"   /* openpete_mods_texture_get (mod-texture runs) */
#include "openpete_fb_snapshot.h"   /* framebuffer-window snapshot runs */
#include "openpete_paths.h"         /* pipeline cache directory */
#include "openpete_atomic_file.h"   /* pipeline cache write-back */

#include "openpete_log.h"
#include "openpete_time.h"   /* op_now - overlay's own sub-tick alpha (camera-independent) */
#include "openpete_subtick_view.h" /* engagement report at shutdown */
#include "openpete_ident_audit.h"  /* headless bring-up gate: the audit is a consumer */
#include "openpete_interp.h" /* deterministic present clock for the overlay sub-tick alpha */
#include "openpete_interp_objs.h" /* tick stamp + period for the overlay sub-tick alpha */
#include "openpete_record.h" /* native --record capture seam */
#include "rhi.h"
#include "mod_vec.h"      /* the scene triangle store and compose scratch runs */
#include "game_layout.h"  /* game_layout.terrain: the per-game ordering-table key policy */
#include "screenshot.h"  /* openpete_write_png_rgba */

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "openpete_window_restack.h"

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
#include "openpete_native_vram.h"   /* the owned VRAM model + native ops */
#include "openpete_terrain_cache.h" /* terrain sector-mesh GPU cache (retained terrain) */
#include "texture_pack.h"           /* texture-replacement registry */
extern void PsyX_SetVRAMDirtyCallback(void (*cb)(int x, int y, int w, int h));
static void nr_vram_dirty_cb(int x, int y, int w, int h);   /* defined below */
extern void GR_ReadVRAM(unsigned short *dst, int x, int y, int dst_w, int dst_h);

/* GL_RGBA enum - byte order R,G,B,A, matching VK_FORMAT_R8G8B8A8_UNORM. */
#define NR_GL_RGBA 0x1908u

#define NR_VRAM_W 1024
#define NR_VRAM_H 512
/* The GPU VRAM texture is twice the PSX height: rows 0-511 mirror the PSX
 * VRAM, rows 512-1023 hold the host-loaded auxiliary bank
 * (openpete_native_vram_aux_set). A material addresses the bank by adding
 * NR_VRAM_H to its texture-page and CLUT rows; nothing the guest writes
 * reaches those rows. */
#define NR_VRAM_TEX_H (2 * NR_VRAM_H)

/* Default window size when the caller passes none. */
#define NR_DEFAULT_W 1280
#define NR_DEFAULT_H 720

/* Per-draw PSX material - must match the push_constant block in
 * shaders/psx_sample.frag (five tightly-packed 32-bit ints). */
typedef struct {
    int32_t tpage_x, tpage_y;
    int32_t clut_x, clut_y;
    int32_t bpp;
} psx_material;

/* A scene triangle's blend class (openpete_scene_tri.blend) as the RHI blend
 * that reproduces it: 0 opaque, then PSX ABR 0 (B+F)/2, ABR 1 B+F, ABR 2 B-F,
 * ABR 3 B+F/4. The alpha class halves through the fragment alpha: the vertex
 * alpha (0x80) on an untextured triangle, the texel's STP bit on a textured
 * one (psx_scene.frag emits alpha 0.5 for an STP texel and 1.0 otherwise, so
 * an STP-clear texel of an ABR 0 primitive stays opaque, as on the GPU). The
 * ABR 1-3 classes read no alpha and blend every texel they are handed; a
 * textured 2D primitive of those modes is split by texel STP (the
 * OPENPETE_STP_* selection, k_stp_sel_classes) so only its STP-set texels
 * reach them. */
enum { NR_BLEND_OPAQUE = 0, NR_BLEND_AVG, NR_BLEND_ADD, NR_BLEND_SUB, NR_BLEND_QADD,
       NR_BLEND_CLASSES };
static const rhi_blend_mode k_blend_class_rhi[NR_BLEND_CLASSES] = {
    RHI_BLEND_NONE, RHI_BLEND_ALPHA, RHI_BLEND_ADD, RHI_BLEND_SUBTRACT,
    RHI_BLEND_ADD_QUARTER
};
/* The blend classes a family builds per texel STP selection (bit b = class b).
 * Row 0 builds every class and keeps every texel. An ABR 1-3 textured 2D primitive draws its
 * STP-clear texels through the opaque STP-clear member and its STP-set texels
 * through the STP-set member of its class; ABR 0 needs no split, its alpha
 * member reads STP through the texel alpha. A family with one selection row
 * builds row 0 alone. */
static const unsigned k_stp_sel_classes[OPENPETE_STP_SELS] = {
    (1u << NR_BLEND_CLASSES) - 1u,
    1u << NR_BLEND_OPAQUE,
    (1u << NR_BLEND_ADD) | (1u << NR_BLEND_SUB) | (1u << NR_BLEND_QADD),
};
/* The textured HUD family: one row of NR_BLEND_CLASSES members per selection,
 * indexed stp_sel * NR_BLEND_CLASSES + blend; members a row does not build are
 * NULL. */
#define NR_HUD_TEX_PIPES (OPENPETE_STP_SELS * NR_BLEND_CLASSES)

/* shadow_decal.frag push constant (must match the GLSL block field-for-field) -
 * the depth-aware shadow decal's per-pixel world-Y wall rejection. The fragment
 * recovers its own PSX screen coord from gl_FragCoord through the fax..fby
 * inverse affine of this pass (scene_fragcoord_affine), then looks the shared
 * pre-pass depth up through the uax..vby affine (the full-extent mapping the
 * pre-pass rasterized with). Both affines are exact, so full-extent, panel and
 * SSAA passes agree, and every fragment samples the texel that covers it. */
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
    float key_contest;             /* 1 = set 1 binding 1 holds this target's terrain keys; unread */
    float uax, ubx, vay, vby;      /* PSX screen -> pre-pass uv affine */
    float pvw, pvh;                /* pre-pass raster extent (texels) */
    float fax, fbx, fay, fby;      /* gl_FragCoord -> PSX screen affine, this pass */
} shadow_decal_pc;


/* shadow_decal_tex.frag push constant (must match the GLSL block field-for-field)
 * - the TEXTURED moby drop-shadow decal: the camera reconstruction params of the
 * Spyro decal (shadow_decal_pc, minus its single ground_up - the moby plane is
 * per-pixel from gl_FragCoord.z) plus the per-draw PSX material (all moby shadows
 * share one shadow-sprite tpage, so one material serves the whole batch). The
 * fragment recovers its own PSX screen coord from gl_FragCoord through the
 * fax..fby inverse affine of this pass, as the Spyro decal does. The five PSX
 * material fields are packed into two words (VRAM x 10 bits, y 9 bits, bpp code
 * 2 bits) so the block is 30 floats + 2 ints = 128 B, the guaranteed
 * maxPushConstantsSize. key_contest is written and not read by the stock
 * decal: the key image stays bound at set 1 binding 1 for the contract's
 * set layout. The layout is the mod decal contract
 * (core/include/openpete_psx_decal.glsl); a change to it recompiles every mod
 * material built from source, the interface hash covering the header. */
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
    float fax, fbx, fay, fby;      /* gl_FragCoord -> PSX screen affine, this pass */
    int32_t tpage;                 /* shadow-sprite texture page, x | y << 10 */
    int32_t clut;                  /* CLUT origin x | y << 10, bpp code << 20 */
    float key_contest;             /* 1 = set 1 binding 1 holds this target's terrain keys */
} shadow_decal_tex_pc;
_Static_assert(sizeof(shadow_decal_tex_pc) == 128,
               "shadow_decal_tex_pc mirrors OP_DECAL_PC (openpete_psx_decal.glsl)");

/** @brief Pack a texture-page origin into the decal contract's tpage word.
 *
 * VRAM coordinates are packed as 10 bits of x and 9 bits of y, x | y << 10,
 * the layout shadow_decal_tex_pc.tpage declares.
 * @param x Page x in VRAM halfwords.
 * @param y Page y in VRAM lines.
 * @return The packed word. */
static int32_t decal_pack_tpage(int x, int y)
{
    return (int32_t)((x & 0x3FF) | ((y & 0x1FF) << 10));
}
/** @brief Pack a CLUT origin and colour depth into the decal contract's clut word.
 *
 * x and y are packed as in decal_pack_tpage; the colour depth is the code
 * 0, 1 or 2 for 4, 8 or 16 bpp (bpp = 4 << code) at bit 20.
 * @param x CLUT x in VRAM halfwords.
 * @param y CLUT y in VRAM lines.
 * @param bpp Colour depth; 16 or more codes 2, 8 or more codes 1, else 0.
 * @return The packed word. */
static int32_t decal_pack_clut(int x, int y, int bpp)
{
    int code = bpp >= 16 ? 2 : (bpp >= 8 ? 1 : 0);
    return (int32_t)((x & 0x3FF) | ((y & 0x1FF) << 10) | (code << 20));
}

/* Push constants for the HD texture-replacement pipes: the replaced texel box,
 * plus the per-channel fog transform applied to the sampled replacement
 * (see tp_fog_affine). a/b are vec4-aligned; the shaders read .rgb. */
/* Push block for the hd_tex*.frag pipes (must match their HdRect layout).
 * a/b = the per-face CLUT-ladder rung affine (identity when unfogged) - the
 * flat model. aN/bN/lad drive the CONTINUOUS model: aN.rgb/bN.rgb are the
 * affine at the DEEPEST authored row, aN[3] > 0 enables the mode, and the
 * shader fades identity->deepest by t = clamp(w*lad[0] + lad[1], 0, 1) per
 * fragment (w = true view-Z) - the same ramp og's per-face rows quantize. */
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

/* Per-draw material for MOD MATERIAL pipelines - psx_material plus the HD
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
 * (cyclorama/sky from gamestate). Double-buffered isn't needed - the game
 * fills it during its render pass (before openpete_native_render_frame reads
 * it). begin() clears; add_tri() appends. */
/* The scene triangle store and every buffer sized from it grow with the
 * scene. A fixed cap drops the triangles past it, and which ones drop depends
 * on the view, so geometry appears and disappears with the camera; an
 * extended-LOD vista that routes every terrain face through the 4x4 cell path
 * feeds well past 150k triangles. Runs start at SCENE_RUN_INIT elements and
 * double; SCENE_TRIS_WAS is the former fixed capacity, past which each growth
 * is logged. An allocation failure keeps what fits and is logged. */
#define SCENE_RUN_INIT 4096
#define SCENE_TRIS_WAS 131072
#define SCENE_VB_EXTRA 18          /* +6 bg-fill, +6 fade overlay, +6 freeze backdrop */
#define SCENE_VB_WAS (SCENE_TRIS_WAS * 3 + SCENE_VB_EXTRA)

/* Moby occlusion-culling grid extent (cells over the PSX screen: ~8×5 PSX px
 * per cell at 512×240). Coarse on purpose - the CPU reads it back per present
 * (OCCL_GW*OCCL_GH*4 = 12 KB) and tests whole-moby screen rects against it. */
#define OCCL_GW 64
#define OCCL_GH 48
#define OVL_MAX_VERTS 16384  /* overlay (3D text-moby glyph) vertex cap (see draw_perf_overlay);
                             * sized for the 4-line detailed readout, not a 1-line counter */
#define MOD_OVL_MAX_VERTS 16384 /* mod draw-service overlay verts (see draw_mod_overlay) */
#define VER_MAX_VERTS 16384  /* build-version line glyph verts (see draw_version_overlay) */
#define MENU_MAX_VERTS 256   /* in-game menu panel/border/selection quads (see draw_menu_overlay) */
#define MENU_GLYPH_MAX OPENPETE_MENU_GLYPH_MAX /* in-game menu 3D glyph layer verts */
/* Stats-overlay supersampling: render the glyphs at OVL_SS_FACTOR× into a small
 * RGBA target covering the top-left ovl_region_w() × ovl_region_h() of the frame (in
 * win_h-fraction units, matching the glyph layout's win_h-relative coords), then
 * box-downsample + alpha-blend onto the frame so the corner counter is crisp at
 * any resolution. The region is generous enough for "FPS: 9999".
 *
 * Height follows the stats level: the detailed readout is four lines, and a
 * region sized for one would clip the rest. It is not simply always tall -
 * the supersample target holds OVL_SS_FACTOR^2 samples per pixel of it, so
 * the unused height would cost real GPU memory in the common case. */
#define OVL_SS_FACTOR  4
/* Fallback region, used only until the game-side builder pushes a real extent
 * (openpete_native_set_overlay_extent) - before the glyph font has ever
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
 * buys nothing the eye can find. The grid is FIXED - max visible x max slot
 * extent - and reallocated only on a window resize, because every SS target
 * rebuild waits the device idle and toasts arrive, expire and repaint many
 * times a second. */
#define OSD_SS_FACTOR   2
/* Dark outline width, in output pixels, of each supersampled text surface's
 * composite (overlay_resolve.frag): the settings menu's glyphs carry one; the
 * performance readout, the version line and the OSD toasts carry none. */
#define OVL_OUTLINE_PX  0
#define MENU_OUTLINE_PX 1
#define OSD_OUTLINE_PX  0
#define OSD_SLOT_W      0.62f   /* max toast box width,  overlay units */
#define OSD_SLOT_H      0.075f  /* max toast box height, overlay units */
#define OSD_GLYPH_MAX   24576   /* 8 toasts of border + wrapped text        */
/* The INTERNAL stored tri: the feeder-facing openpete_scene_tri plus
 * the routing state that travels through the add_tri arguments. Private to
 * this TU - feeders can't write routing fields (no such field exists on their
 * type), and no API ever hands a stored tri back out. The feeder tri is a
 * strict LAYOUT PREFIX (asserted below), so the store is a prefix memcpy. */
typedef struct {
    openpete_scene_vert v[3];
    int textured;
    int tpage_x, tpage_y, clut_x, clut_y, bpp;
    int blend;
    float koff;     /* og OT key - see openpete_scene_tri */
    int prop;       /* authored embedded prop - see openpete_scene_tri */
    float rank;     /* og OT paint rank - see openpete_scene_tri */
    int mordinal;   /* present-local moby ordinal - see openpete_scene_tri */
    float pkey;     /* player ordering key - see openpete_scene_tri */
    uint32_t mid;   /* moby identity - see openpete_scene_tri */
    int category;   /* draw layer (OPENPETE_SCENE_*) - add_tri's explicit arg */
    int carry;      /* 2D-UI sub-tick carry: 0/1 from add_tri; 2 = re-applied marker
                     * stamped by apply_overlay so the next canonical capture skips it */
    int group;      /* per-portal grouping id (add_tri_grouped; plain add_tri: 0) */
    uint32_t tag;   /* provenance: sticky set_tag value; joins
                     * the material key ONLY when tag_split_on() (else inert) */
    int fog_delta;  /* sticky set_fog_delta value - the CLUT-ladder row this
                     * tri's palette was shifted to (0 = unfogged base row) */
    uint32_t prim;  /* PSX address of the primitive this tri was decoded from
                     * (sticky set_prim value; 0 for everything that is not 2D).
                     * The ordering-table walk ranks a tri by finding this
                     * address in the chain, so every 2D emitter can keep feeding
                     * where it does and still be ordered by the traversal. */
    int stp_sel;    /* texel STP selection (sticky set_stp_select value,
                     * OPENPETE_STP_*): which texels a textured tri draws.
                     * Non-zero only on the two halves of a split ABR 1-3 2D
                     * primitive. */
    uint32_t fb_cell; /* the element of a framebuffer copy a 2D tri draws
                       * (sticky set_fb_cell value; 0 = none): one key per
                       * texel-coordinate triple of a draw buffer the tri
                       * samples through the VRAM path, or per border the
                       * copy's emitter draws around it, the same from tick
                       * to tick while the tri's screen box scales. The 2D
                       * carry pairs a scaled framebuffer copy by it
                       * (carry_pair_snapshot). */
    uint32_t seq;   /* the ordering-table traversal rank for this frame (sticky
                     * set_seq value; 0 for everything that is not 2D). The 2D
                     * layer is screen-space with the depth test off, so the
                     * order tris are composed in IS the painter order, and the
                     * two 2D emitters reach the scene at different points of
                     * the frame, one at the primitive's link, one when a glyph
                     * chain is built, one of them deferred to a later release.
                     * Composing by this number reproduces the order the game
                     * emitted them in, whenever each was fed. */
    int host_added; /* 1 = geometry the host added beyond the game's own render
                     * (sticky set_host_added value). The scene's content and
                     * world-frame verdicts skip these tris. */
    int portal_rim; /* 1 = opaque terrain the feed accepted as a portal's inner
                     * rim (sticky set_portal_rim value): it draws through the
                     * inner-rim twins (nr_rim_twins), which discard it inside
                     * the portal openings. */
    float kfl[3][2]; /* drain coordinate p per vertex and ...                 */
    uint32_t kc[8];  /* ... the face's drain sub-cell key table (sticky
                      * set_key_cells value; all zero = none): each fragment
                      * keys and contests at its own sub-cell's key, through
                      * the present's drain records (drain_records_build) */
    float kn[12];    /* the face's corners in the fold test's homogeneous screen
                      * space, TL TR BL BR (sticky set_key_fold value); read
                      * only when kfold */
    int kfold;       /* 1 = the triangle's quadrant folds (psx_key_contest.glsl
                      * op_drain_folded) and kn is set */
    int ter_after;  /* 1 = added at or after the start of this scene's terrain
                     * feed (sticky, set by openpete_native_scene_mark_terrain_feed,
                     * cleared with the scene). Places the feed in emission order
                     * when it emits no triangle of its own. */
} scene_tri_t;
/* C99 compile-time prefix check: if a field is added to openpete_scene_tri
 * without mirroring it here (or vice versa), this array gets a negative size. */
typedef char nr_assert_scene_tri_prefix[
    (offsetof(scene_tri_t, category) == sizeof(openpete_scene_tri)) ? 1 : -1];

/* The present's drain records (psx_key_contest.glsl op_drain_record): one
 * per scene triangle with a drain sub-cell key table, packed into the
 * records image the key-resolve stages read, indexed by the tag the
 * triangle's vertices carry in their rank ordinal. idx maps a triangle of
 * `scene` to its record (-1: none). */
#define DRAIN_REC_PER_ROW 64            /* 4 texels per record, 256 texels a row */
#define DRAIN_REC_ROWS    64
static struct {
    const void* scene; int n;           /* the scene the records describe       */
    uint64_t    serial;                 /* rhi frame serial of the build        */
    int*        idx; int idx_cap;
    int         nrec;
    int         untex;                  /* tables on triangles with no UV map   */
    uint32_t    words[DRAIN_REC_PER_ROW * DRAIN_REC_ROWS * 16];
} s_drain;

/** @brief Replace a vertex's rank ordinal with its triangle's drain-record reference when it has one.
 *
 * The ordinal travels in the vertex's sy slot. When @p t lies in the scene
 * s_drain was built for and has a drain record, sy becomes -(record + 1),
 * the tag the key-resolve stages index the records image with; otherwise
 * the vertex is left as the caller wrote it.
 * @param o Output vertex, its sy already holding the ordinal.
 * @param t Scene triangle the vertex belongs to.
 * @param k Vertex index within the triangle; unused. */
static inline void rv_drain(rhi_vertex* o, const scene_tri_t* t, int k)
{
    (void)k;
    const uintptr_t a = (uintptr_t)t, b = (uintptr_t)s_drain.scene;
    if (!s_drain.idx || a < b) return;
    const uintptr_t i = (a - b) / sizeof(scene_tri_t);
    if (i >= (uintptr_t)s_drain.n || s_drain.idx[i] < 0) return;
    o->sy = -(float)(s_drain.idx[i] + 1);
}
static mod_fvec           s_scene_v;              /* accumulating (frame N) */
#define s_scene ((scene_tri_t*)s_scene_v.data)    /* moves on growth: index, never hold */
static int                s_scene_n;

/** @brief Grow a scene-triangle run to hold n triangles and return how many it holds.
 *
 * The run is initialised on first use. The result is less than @p n only
 * when the allocation failed, which the vec logs; a negative @p n reserves
 * nothing.
 * @param v The run.
 * @param name Name the vec logs under.
 * @param n Triangles wanted.
 * @return @p n, or the run's capacity when that is smaller. */
static int scene_run_fit(mod_fvec *v, const char *name, int n) {
    if (!v->elem_size)
        mod_fvec_init(v, sizeof(scene_tri_t), SCENE_RUN_INIT, name, SCENE_TRIS_WAS);
    if (mod_fvec_reserve_noted(v, (size_t)(n > 0 ? n : 0)) == 0) return n;
    return (int)v->cap < n ? (int)v->cap : n;
}
/** @brief Exchange the storage (data, count, capacity) of two scene-triangle runs; each keeps its census node and name.
 *
 * Hands a finished scene to a history slot or a ring slot without copying
 * it: the accumulator takes the other run's buffer, whose contents it
 * overwrites from triangle 0.
 * @param a One run, initialised.
 * @param b The other run, initialised. */
static void scene_run_swap(mod_fvec *a, mod_fvec *b) {
    unsigned char *d = a->data; a->data = b->data; b->data = d;
    size_t c = a->count; a->count = b->count; b->count = c;
    size_t k = a->cap; a->cap = b->cap; b->cap = k;
}
/** @brief Grow the accumulating scene to hold n triangles.
 *
 * @param n Triangles wanted.
 * @return 1 when the scene holds @p n, 0 when the allocation fell short. */
static int scene_room(int n) { return scene_run_fit(&s_scene_v, "scene.tri", n) >= n; }

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
#define SCENE_RING OPENPETE_NATIVE_SCENE_RING
/* One CPU-fed scene pipeline and its portal twin: the same description
 * with the fragment stage's _cover variant (blended world) or _rim variant
 * (opaque inner-rim terrain; portal_cover_scene.glsl) and the cover planes'
 * UBO at set 5. A NULL twin (no variant for the stage, or a failed build)
 * draws through the stock pipeline without the test. */
typedef struct {
    rhi_pipeline *stock;
    rhi_pipeline *cover;
    int rim;                    /* 1: the inner-rim rule, 0: the blended cover rule */
} nr_cover_twin;
#define NR_COVER_TWINS 17
/* The portal opening planes as the _cover and _rim fragment stages read them
 * (the std140 PortalCover block of portal_cover_scene.glsl): fc, this pass's
 * gl_FragCoord -> PSX screen affine (fax, fbx, fay, fby), written at bind
 * time; cnt.x the plane count, cnt.y 1 when the blended cover is on;
 * margin.x the plane-cover margin;
 * abc the plane's screen field, w = outline point count; bbox and poly the
 * outline, 16 points per plane. Every member is 32-bit, so the stages need
 * no shaderFloat64; abc holds the planes' double fields rounded to float,
 * the precision the cover test computes in. */
#define NR_COVER_PLANES 6
typedef struct {
    float   fc[4];
    int32_t cnt[4];
    float   margin[4];
    float   abc[NR_COVER_PLANES][4];
    float   bbox[NR_COVER_PLANES][4];
    float   poly[NR_COVER_PLANES * 16][4];
} nr_cover_block;
typedef char nr_cover_block_layout_assert[
    (offsetof(nr_cover_block, margin) == 32 && offsetof(nr_cover_block, abc) == 48 &&
     offsetof(nr_cover_block, bbox) == 144 && offsetof(nr_cover_block, poly) == 240) ? 1 : -1];
static nr_cover_block s_cover_live;   /* this scene's terrain feed's planes (fc unset) */
static openpete_native_cover_plane s_cvp[8];   /* the same openings, for the cover check */
static int   s_cvp_n = 0;
/** @brief Drop the published portal openings at a scene wipe.
 *
 * Only the terrain feed publishes openings (openpete_native_set_cover_planes),
 * so a scene whose terrain does not feed (a level transition, a render-path
 * selection without terrain) carries none, instead of the openings of the
 * last scene that fed terrain. A zero count turns both fragment tests off. */
static void cover_planes_clear(void) {
    memset(&s_cover_live, 0, sizeof s_cover_live);
    s_cvp_n = 0;
}
/* The blended-world emission order a game-render scene recorded
 * (bw_key_order_record): keys in first-emission order and the terrain feed's
 * rank among them. The compose sorts re-extracted blended blocks by it. */
#define BW_KEY_MAX 16
typedef struct {
    uint32_t key[BW_KEY_MAX];      /* keys in first-emission order */
    int      n;
    int      ter_rank;             /* keys first emitted before the terrain feed began */
} bw_order_t;
typedef struct {
    scene_tri_t *tri; int n;        /* tri = tri_v's run, re-read after each growth */
    mod_fvec  tri_v;
    uint32_t bg, fade, freeze; int hud_under, actor_front;
    int replay;                    /* the scene was built by a re-extract, not the game render */
    unsigned  frame;               /* openpete_frame at extraction */
    uint32_t  stamp;               /* terrain-cache build stamp (scene id) */
    uint32_t  drawenv_clear;
    int       geom[8];
    openpete_hud_vert *ovl; int ovl_cap, ovl_n;
    openpete_shadow_decal_params shadow;
    openpete_shadow_footprint shadow_foot;   /* the player fan the decal re-rasterizes */
    nr_cover_block cover;                    /* the portal planes the blended world tests */
    openpete_occl_camera occl_cam;           /* camera this scene was projected from */
    bw_order_t bw;                           /* blended-world order recorded at publish */
    int       freeze_retained;               /* freeze-backdrop production mode at publish */
    openpete_glitch_walk_t gwalk;            /* census of the walk that built this scene */
    uint32_t  fam_n[16];                     /* triangles per provenance family at publish
                                              * (OPENPETE_PRESENT_TRACE only) */
    int       tc_built;                      /* drawn retained sectors its terrain build
                                              * held at publish (OPENPETE_PRESENT_TRACE only) */
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
static void        *s_ring_mx;   /* SDL_Mutex, openpete_mutex_once */
static int pc_trace_on(void);
static const scene_slot_t *s_ps_slot;          /* compose source: NULL = live */
static const bw_order_t *ps_bw(void);
static int ps_freeze_retained(void);
static uint32_t ps_drawenv_clear(void);
static const openpete_shadow_decal_params *ps_shadow(void);
static const openpete_shadow_footprint *ps_shadow_foot(void);
static const nr_cover_block *ps_cover(void);
static const openpete_occl_camera *ps_occl_cam(void);
static unsigned ps_frame(void);
static void     ps_apply_geometry(void);
static uint32_t     s_eager_stamp;             /* build stamp for the scene being extracted */
/* Build stamp of an eager scene the ring refused, which stays in the
 * accumulator, and the scene epoch it belongs to (scene_live_stamp). */
static uint32_t     s_scene_refused_stamp;
static uint32_t     s_scene_refused_epoch;
static uint32_t     s_scene_id_next = 1;
/* Bumped by every scene wipe (the frame-keyed reset and the sub-tick
 * scene_begin alike), so a tri recorded at feed time can be told from a tri
 * fed into a scene that has since been replaced. The 2D-overlay carry uses it
 * to know which of its entries the live scene still holds. */
static uint32_t     s_scene_epoch = 1;
static struct { unsigned published, refused_full, superseded, presented; } s_ring_census;


/* Feed-time accumulation for the 2D-overlay carry (committed by
 * capture_overlay - see the carry block further down). The live scene is
 * wiped by EVERY sub-tick re-extract, so scavenging s_scene at capture time
 * raced those wipes: a 2D feed that landed mid-VBL with any intermediate
 * present between it and the canonical capture was wiped unseen, and that
 * tick's carry lost the sprite (the collected-egg HUD sprite froze/vanished
 * at high --render-fps - the deterministic egg.bk2 f3603 blink).
 * Carry-eligible tris are instead copied here AT ADD TIME (apply_overlay's
 * re-applies bypass add_tri, so re-applied carry never re-accumulates). */
#define OVERLAY_CARRY_MAX 1024
static scene_tri_t s_overlay_accum[OVERLAY_CARRY_MAX];
static int                s_overlay_accum_n;
/* The scene epoch each accumulated tri was fed into (see s_scene_epoch). */
static uint32_t           s_overlay_accum_epoch[OVERLAY_CARRY_MAX];
static int                s_overlay_accum_dropped;
static void (*s_overlay_precapture)(void);

void openpete_native_scene_register_overlay_precapture(void (*fn)(void)) {
    s_overlay_precapture = fn;
}

/* Provenance-tag state. s_tag_split_ext is armed by the material-
 * selector registry; the env flags are the dev instruments. */
static uint32_t s_cur_tag;        /* sticky, stamped by add_tri */
/* The ordering-table traversal rank of the tris added next (see scene_tri_t::seq).
 * Sticky, cleared at each frame boundary; a feeder that never sets it leaves 0. */
static uint32_t s_cur_seq;
static uint32_t s_cur_prim;       /* sticky, stamped by add_tri (see scene_tri_t::prim) */
static int s_cur_stp_sel;         /* sticky, stamped by add_tri (see scene_tri_t::stp_sel) */
static uint32_t s_cur_fb_cell;    /* sticky, stamped by add_tri (see scene_tri_t::fb_cell) */
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
static int      s_tag_split_ext = 0;    /* armed by registered selectors */

void openpete_native_scene_set_tag(uint32_t tag) { s_cur_tag = tag; }
/* Sticky host-added origin (see openpete_native_scene_set_host_added). */
static int s_cur_host_added;
void openpete_native_scene_set_host_added(int on) { s_cur_host_added = on ? 1 : 0; }
/* Sticky portal inner-rim mark (see openpete_native_scene_set_portal_rim). */
static int s_cur_portal_rim;
void openpete_native_scene_set_portal_rim(int on) { s_cur_portal_rim = on ? 1 : 0; }
static float    s_cur_kfl[3][2];
static uint32_t s_cur_kc[8];
static float    s_cur_kn[12];
static int      s_cur_kfold;
void openpete_native_scene_set_key_cells(const float fl[6], const uint32_t kc[8])
{
    if (!fl || !kc) {
        memset(s_cur_kfl, 0, sizeof s_cur_kfl);
        memset(s_cur_kc, 0, sizeof s_cur_kc);
        s_cur_kfold = 0;
        return;
    }
    memcpy(s_cur_kfl, fl, sizeof s_cur_kfl);
    memcpy(s_cur_kc, kc, sizeof s_cur_kc);
}
void openpete_native_scene_set_key_fold(const float kn[12])
{
    s_cur_kfold = kn != NULL;
    if (kn) memcpy(s_cur_kn, kn, sizeof s_cur_kn);
}
/** @brief Stamp the sticky drain key state (set_key_cells, set_key_fold) on a scene triangle.
 *
 * The fold corners are copied only when the fold data is set; kfold says
 * whether they are valid.
 * @param t The triangle being added. */
static void scene_stamp_key_cells(scene_tri_t* t)
{
    memcpy(t->kfl, s_cur_kfl, sizeof s_cur_kfl);
    memcpy(t->kc, s_cur_kc, sizeof s_cur_kc);
    t->kfold = s_cur_kfold;
    if (s_cur_kfold) memcpy(t->kn, s_cur_kn, sizeof s_cur_kn);
}
/* Sticky terrain-feed mark (see openpete_native_scene_mark_terrain_feed). */
static int s_cur_ter_after;
void openpete_native_scene_mark_terrain_feed(void) { s_cur_ter_after = 1; }
void openpete_native_scene_tag_family_counts(uint32_t out[16]) {
    memcpy(out, s_scene_fam_n, sizeof s_scene_fam_n);
}
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
/* Armed by the mod material-selector registry; see tag_split_on. */
void openpete_native_set_tag_split(int on) { s_tag_split_ext = on ? 1 : 0; }

/** @brief Return whether provenance-tag batch splitting is armed.
 *
 * The first call reads OPENPETE_TAG_SPLIT and OPENPETE_TAG_TINT into their
 * caches. The result is the cached variable ORed with the flag set by
 * openpete_native_set_tag_split, which is read live. The lazy initialisation is
 * not synchronised. */
static int tag_split_on(void) {
    if (s_tag_split_env < 0) {
        const char* e = getenv("OPENPETE_TAG_SPLIT");
        s_tag_split_env = (e && *e && *e != '0');
        const char* t = getenv("OPENPETE_TAG_TINT");
        s_tag_tint = (t && *t && *t != '0');
    }
    return s_tag_split_env || s_tag_split_ext;
}

/** @brief Return whether OPENPETE_TAG_TINT requests recolouring triangles by provenance tag.
 *
 * The variable is parsed by tag_split_on, which this calls first. */
static int tag_tint_on(void) {
    (void)tag_split_on();          /* lazy env init lives there */
    return s_tag_tint > 0;
}

/** @brief Return whether a dev tag instrument, OPENPETE_TAG_SPLIT or OPENPETE_TAG_TINT, is armed.
 *
 * OPENPETE_TAG_SPLIT forces the batch key join and OPENPETE_TAG_TINT
 * recolours by tag. Their semantics live in the CPU batcher, so either one
 * disqualifies the retained terrain cache for the whole run
 * (openpete_native_tercache_usable). A split armed by a registered mod
 * material selector does not count: the terrain feeder claims the sectors a
 * selector could override back to the CPU walk one sector at a time.
 * @return 1 when either env instrument is armed, else 0. */
static int tag_split_dev(void) {
    (void)tag_split_on();          /* lazy env init lives there */
    return s_tag_split_env > 0 || s_tag_tint > 0;
}

/* OPENPETE_SCENE_BOXDBG="x0,y0,x1,y1,fA,fB": the box instrument. Per composed
 * present whose scene was extracted at a VBL in [fA,fB], it tests every
 * triangle of the composed list against the box (PSX display coordinates,
 * the scene's sx/sy space) and logs each triangle whose area overlaps it,
 * then one engagement line (tested, covering) even when nothing covers.
 * Parsed once; the armed or rejected spec is logged so a malformed value
 * cannot pass for an empty box. */
typedef struct { int armed; float x0, y0, x1, y1; unsigned fa, fb;
                 unsigned presents, tested, covering; } scene_box_cfg_t;
static scene_box_cfg_t s_box;
/** @brief Return the OPENPETE_SCENE_BOXDBG box instrument state, parsing the variable on the first call.
 *
 * The value is x0,y0,x1,y1,fA,fB; the armed or rejected spec is logged
 * once. The struct also holds the instrument's running counters.
 * @return The state, never NULL; armed is 0 when the variable is unset or malformed. */
static const scene_box_cfg_t *scene_box_cfg(void) {
    static int parsed = 0;
    if (parsed) return &s_box;
    parsed = 1;
    const char *e = getenv("OPENPETE_SCENE_BOXDBG");
    if (!e || !*e) return &s_box;
    s_box.armed = sscanf(e, "%f,%f,%f,%f,%u,%u", &s_box.x0, &s_box.y0, &s_box.x1,
                         &s_box.y1, &s_box.fa, &s_box.fb) == 6;
    if (s_box.armed)
        fprintf(stderr, "[sbox] armed box=[%.1f,%.1f]-[%.1f,%.1f] vbl=%u..%u "
                "(retained terrain cache off while armed)\n", (double)s_box.x0,
                (double)s_box.y0, (double)s_box.x1, (double)s_box.y1, s_box.fa, s_box.fb);
    else
        fprintf(stderr, "[sbox] REJECTED spec '%s' (want x0,y0,x1,y1,fA,fB)\n", e);
    return &s_box;
}
/** @brief Return whether the OPENPETE_SCENE_BOXDBG box instrument is armed.
 *
 * The retained terrain cache draws sectors that never enter the scene list,
 * so while the instrument is armed the cache reports itself unusable and all
 * terrain goes through the CPU walk, which the cache mirrors bit-exactly,
 * where the box can test it.
 * @return 1 when armed, else 0. */
static int scene_box_armed(void) { return scene_box_cfg()->armed; }
/** @brief Log that the armed box instrument will test nothing in this run, with the reason.
 *
 * Bring-up calls it when it declines the native renderer, so a run that
 * never composes still prints an engagement line (tested 0, covering 0).
 * No-op when the instrument is not armed.
 * @param why Reason the run composes nothing. */
static void scene_box_note_inactive(const char *why) {
    if (scene_box_armed())
        fprintf(stderr, "[sbox] INACTIVE: %s; tested=0 covering=0\n", why);
}
/** @brief Print the box instrument's run totals on stderr when it is armed.
 *
 * Called at native renderer shutdown. The line reports presents inside the VBL
 * window, triangles tested and triangles covering the box, and marks the run
 * VACUOUS when no present was composed in the window. */
static void scene_box_summary(void) {
    if (scene_box_armed())
        fprintf(stderr, "[sbox] total presents_in_window=%u tested=%u covering=%u%s\n",
                s_box.presents, s_box.tested, s_box.covering,
                s_box.presents ? "" : " (VACUOUS: no present composed in the window)");
}

/* Mod custom-pipeline drain - defined with the postfx executor
 * below; called from compose_variant (mod 3D under mod HUD). */
/* Postfx executor + SCENE-point split - defined below the
 * postfx typedef; compose_variant (above them) calls via these decls. */
static int native_postfx_run(int point, rhi_texture* target, int w, int h);
static int native_postfx_point_active(int point);
/* Shared with the postfx executor below: the build-failed pipeline
 * sentinel and the engine/mod UBO - draw_tex_runs' material overrides
 * bind them from earlier in the file. */
#define PFX_PIPE_FAILED OPENPETE_POSTFX_PIPE_FAILED
static rhi_ubo* s_pfx_ubo;
/* Material-selector UBO: block 0 = engine block, blocks 1+slot =
 * per-instance uniform blocks. Filled once per present at the top of
 * draw_native_scene (defined after the postfx typedef); draw_tex_runs'
 * override binds it. */
static rhi_ubo* s_mat_ubo;
static void fill_material_ubo(void);
/* Shared with the retained terrain cache's material pass. */
struct rhi_ubo *openpete_native_material_ubo(void) { return s_mat_ubo; }
static uint32_t           s_scene_bg;  /* panel-0 background fill (rgba); 0 = none */

/* Reference frame-sync: the native scene is built from the FRESHLY-computed
 * geometry (frame N), but PsyCross's reference shows its display FBO, which lags
 * the native geometry (the OT PsyCross draws is the one Spyro built a frame or
 * two earlier - PSX double-buffer + the build-vs-draw split). Comparing the
 * scene by an integer DEPTH: keep a ring of recent completed frames and present
 * the one `depth` frames back. Depth is 0 (the live frame) unless
 * OPENPETE_NATIVE_SCENE_DELAY=<n> forces an explicit integer depth. */
#define SCENE_HIST 4   /* ring of completed-frame snapshots: history depths 1..4 beside the live depth 0 */
static mod_fvec           s_scene_hist[SCENE_HIST];   /* completed-frame ring (scene_tri_t) */
static int                s_scene_hist_n[SCENE_HIST];
static unsigned           s_scene_hist_frame[SCENE_HIST]; /* completed frame's stamp */
static uint32_t           s_scene_hist_bg[SCENE_HIST];
static uint32_t           s_scene_hist_fade[SCENE_HIST];  /* full-screen fade overlay, per snapshot */
static uint32_t           s_scene_hist_freeze[SCENE_HIST]; /* freeze-backdrop overlay, per snapshot */
static int                s_scene_hist_replay[SCENE_HIST]; /* built by a re-extract, per snapshot */
static int                s_scene_hist_hud_under[SCENE_HIST]; /* flat-HUD-under-player flag, per snapshot */
static int                s_scene_hist_actor_front[SCENE_HIST]; /* actors-in-front (dragon rescue) flag, per snapshot */
static nr_cover_block     s_scene_hist_cover[SCENE_HIST]; /* the snapshot's portal openings */
static int                s_scene_hist_head = -1;  /* ring index of the most-recent completed frame */
static int                s_scene_hist_count = 0;  /* completed frames recorded (warmup guard) */
/* 1 after an eager publish took the accumulator's storage: s_scene holds the
 * ring slot's former contents until the next scene begins, so the frame
 * close must not push it into history. */
static int                s_scene_moved = 0;
/* The ring slot the moved scene went to and the scene id it carries there:
 * until that slot is republished it still holds the scene, which the
 * history and the live fallback read in place of a copy. */
static int                s_scene_moved_slot = -1;
static uint32_t           s_scene_moved_id;
/* Per history entry: the ring slot whose scene the entry names instead of
 * owning a run (-1 = the entry owns s_scene_hist[slot]), and that scene's id. */
static int                s_scene_hist_alias[SCENE_HIST] = { -1, -1, -1, -1 };
static uint32_t           s_scene_hist_alias_id[SCENE_HIST];
static int                s_scene_delay = -2;   /* -2 uninit; >=0 = depth */

/* 1 while openpete_native_render_screenshot_all composes: the -vk screenshot
 * sits beside the -og PsyCross capture. */
static int s_scene_shot_compose = 0;
/* 1 under --headless while a shot compose runs. Set with s_scene_shot_compose
 * by the screenshot_all and capture_rgba wrappers (s_nr is not yet declared at
 * this point); scene_delay_depth does not read it, so the compose selects the
 * same scene in every mode. */
static int s_scene_shot_lag = 0;
/* Intermediate (sub-tick alpha=0.5) capture: the re-extract builds the
 * interpolated scene into the CURRENT s_scene and never pushes it to the frame
 * history, so the compose reads depth 0 (an OPENPETE_NATIVE_SCENE_DELAY
 * override still wins) and the windowed live readback is skipped in favour of
 * a recompose. Set only around openpete_native_render_screenshot_all for an
 * intermediate dump. */
static int s_scene_shot_nodelay = 0;
void openpete_native_render_set_capture_nodelay(int on) { s_scene_shot_nodelay = on ? 1 : 0; }

/* The completed frame a framebuffer-window resolve must compose, 0 when no
 * resolve is in flight (see fbsnap_capture_crop). */
static unsigned s_fbsnap_want_frame;
/* 1 while that resolve composes the kept game-render scene (s_scene_game),
 * whose stamp equals s_fbsnap_want_frame. */
static int s_fbsnap_game_sel;

/* The openpete_frame stamp of the scene at a given delay depth. */
static unsigned scene_frame_at_depth(int d);
static int scene_hist_depth_max(void);

/** @brief Return the completed-scene ring depth the present composes: 0 is the live scene.
 *
 * A pending framebuffer-window resolve wins: the depth is the one whose stamp
 * equals s_fbsnap_want_frame (0 when the ring holds no such scene). Otherwise
 * OPENPETE_NATIVE_SCENE_DELAY, read once, forces a depth clamped to
 * 0..SCENE_HIST-1 in every mode. With no override the depth is 0, both for a
 * sub-tick capture (s_scene_shot_nodelay) and for the screenshot and live
 * composes.
 * @return Ring depth, 0 to SCENE_HIST-1; a framebuffer-window resolve may
 *         return up to SCENE_HIST. */
static int scene_delay_depth(void) {
    /* A framebuffer window holds what the game drew on the frame it copied, and
     * the present that resolves it runs a frame later: reach back through the
     * completed-scene ring to that frame. Wins over every other selection,
     * since the window's content is fixed by the copy, not by the present. */
    if (s_fbsnap_want_frame) {
        if (s_fbsnap_game_sel) return 0;
        for (int d = 0; d <= scene_hist_depth_max(); d++)
            if (scene_frame_at_depth(d) == s_fbsnap_want_frame) return d;
        return 0;
    }
    if (s_scene_delay < -1) {
        const char* e = getenv("OPENPETE_NATIVE_SCENE_DELAY");
        /* Explicit env = a forced depth (wins in every mode, incl. the
         * screenshot compose - that's how the alignment sweep is run);
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
 * and every emit path - including the sub-tick re-extract - is category-correct
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
 * terrain they interpenetrate (the spread wing inside a wall) - true per-pixel
 * Z would amputate them. Reset per frame. */
static int s_scene_actor_front = 0;

/* One-shot census: the DISCRETE half of a composed scene, hashed.
 *
 * A scene tri carries two kinds of field. Continuous ones - vertex position,
 * UV, colour - move with the interpolation alpha and are exactly what a
 * re-lerp of an already-composed scene would reproduce. Discrete ones -
 * whether a tri exists at all, its texture page, CLUT, bit depth, blend mode,
 * draw category, prop, portal group, tag, fog row - come from decisions the
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
 *   MATERIAL SET - the distinct discrete keys present (texture page, CLUT,
 *   bpp, blend, category, prop, group, tag, fog row). It moves only when the
 *   extraction DECIDES differently: a new texture, an LOD tier that picks a
 *   different page, a blend mode change, an object entering or leaving.
 *
 *   TRI MULTISET - the same keys with their counts. It also moves when the
 *   same objects simply contribute a different number of triangles, which is
 *   what sub-tick camera motion does at the frustum edge.
 *
 * A scene reuse that re-lerps transforms reproduces the second but not the
 * first, so the two must be counted apart. Both are order-independent (XOR
 * over distinct keys, sum over all tris), because tri order renumbers freely
 * as counts shift and an order-sensitive hash would report that as a change.
 */
#define SIG_TBL 4096u
/** @brief Hash a triangle's discrete material fields into one nonzero key.
 *
 * Folds tag, texture page, CLUT, bit depth, blend, prop, group, fog row,
 * category and the textured flag with a multiplier of 31. Position, UV and
 * colour are excluded. 0 is remapped to 1 because scene_discrete_sig uses 0 as
 * its empty-slot marker.
 * @param t The triangle.
 * @return The key, never 0. */
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

/** @brief Compute the material-set and triangle-multiset signatures of the composed scene.
 *
 * The set hash XORs one mixed term per distinct scene_tri_key, found through a
 * 4096-slot open-addressing table that is cleared on each call. The count hash
 * sums a mixed term over every triangle, so it depends on multiplicity but not
 * on order. Once the table holds SIG_TBL distinct keys a further new key is
 * neither inserted nor counted. The table is static, so the function is not
 * reentrant.
 * @param set_h Receives the distinct-key hash.
 * @param cnt_h Receives the all-triangle hash.
 * @param distinct Receives the number of distinct keys counted. */
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

/** @brief Return whether OPENPETE_SCENE_SIG requests the scene signature census.
 *
 * Read once and cached; unset, empty or 0 is off. */
static int scene_sig_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OPENPETE_SCENE_SIG"); on = (e && *e && *e != '0'); }
    return on;
}

/** @brief Return whether OPENPETE_SCENE_POS_SIG requests the per-category screen-position census.
 *
 * The census is the continuous counterpart of the OPENPETE_SCENE_SIG
 * discrete signature: per draw category, an order-independent hash over
 * every vertex's screen position (exact float bits) with the triangle
 * count, plus one bucket for the particle HUD bursts (tag path 5, selectors
 * 4 and 5). It checks a display policy that must leave the HUD where the
 * game put it (--fov): the HUD, HUD3D, HUD_PANEL and OVERLAY hashes and the
 * burst bucket stay equal across the policy's settings while the world
 * categories differ. Read once and cached.
 * @return 1 when the census is on, else 0. */
static int scene_pos_sig_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OPENPETE_SCENE_POS_SIG"); on = (e && *e && *e != '0'); }
    return on;
}
/** @brief Print the per-category screen-position census of the composed scene on stderr.
 *
 * One [scene-pos] line for the current VBL: for each of the 14 draw categories
 * an order-independent sum of a hash of every vertex's sx and sy float bits,
 * and the triangle count, then the same pair for the particle HUD bursts (tag
 * path 5, selectors 4 and 5). A category above 13 accumulates in a bucket
 * the line does not print.
 * @param canonical Nonzero labels the line CANON, zero labels it inter. */
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
/* The blended-world classes composite in emission order, and the blend
 * equations do not commute once a channel clamps (additive over subtractive
 * saturates where subtractive over additive does not). The game render emits in
 * the order the guest links its primitives into the ordering table; the
 * re-extract emits one producer at a time in its body's call order, which the
 * producers' data dependencies fix. Recording the game render's key order and
 * reordering the re-extract's block by it makes the two composite the same.
 *
 * The key is the provenance tag's path (bits 31:28) refined, for the particle
 * path, by the KIND byte: that path is shared by three feeders (the particle
 * pool, the glow rings, the gem glints) on Spyro 2 and Spyro 3, and only the
 * KIND separates them. The tracer path is shared by the tracer pool and the zap
 * ribbons with nothing in the tag to separate them, so those two keep their
 * relative call order. */
static int      blended_world_tri(const scene_tri_t* s);
static unsigned bw_rank_key(const scene_tri_t* t);
/* The live record, written by the extracting thread. The terrain rank is the
 * number of recorded keys first emitted before the feed began (the scene's
 * first triangle marked ter_after or tagged terrain; every key when neither
 * occurs). A published scene carries its own copy (scene_slot_t.bw), which
 * the compose reads through ps_bw(). */
static bw_order_t s_bw;
/** @brief Return a blended-world key's rank in a recorded first-emission order.
 *
 * The rank is the key's position in the recording game-render scene's
 * first-emission order, or O->n for a key that scene never emitted; such
 * keys sort after every recorded key and keep their relative order.
 * @param O The recorded order.
 * @param key Key from bw_rank_key.
 * @return The rank, 0 to O->n. */
static int bw_key_rank(const bw_order_t* O, uint32_t key) {
    for (int i = 0; i < O->n; i++) if (O->key[i] == key) return i;
    return O->n;
}
/** @brief Record a game-render scene's blended-world keys in first-emission order, and the terrain feed's rank, into s_bw. */
static void bw_key_order_record(const scene_tri_t* sc, int n) {
    bw_order_t* O = &s_bw;
    O->n = 0;
    int ter_rank = -1;
    for (int i = 0; i < n; i++) {
        if (ter_rank < 0 && (sc[i].ter_after || (sc[i].tag >> 28) == 2u)) ter_rank = O->n;
        if (!blended_world_tri(&sc[i])) continue;
        const uint32_t k = bw_rank_key(&sc[i]);
        if (bw_key_rank(O, k) == O->n && O->n < BW_KEY_MAX) O->key[O->n++] = k;
    }
    O->ter_rank = ter_rank < 0 ? O->n : ter_rank;
}

/* 1 while the scene being accumulated is a sub-tick / lag re-extract's
 * rebuild (openpete_native_scene_begin), 0 while it is the game render's
 * own accumulation (openpete_native_scene_begin_frame). The two emit the
 * same geometry in different orders: the game render in the order the guest
 * links its primitives, the re-extract in its fixed producer order. */
static int s_scene_replay;
static void scene_game_retain(void);
void openpete_native_scene_begin(void) {
    /* The scene about to be wiped is the game render's own accumulation the
     * first time a tick re-extracts; record its blended-world key order
     * before it goes, and keep the scene itself for a framebuffer copy that
     * names its frame. A later re-extract of the same tick must not
     * overwrite either with its own (already reordered) scene. */
    if (!s_scene_replay) { bw_key_order_record(s_scene, s_scene_n); scene_game_retain(); }
    s_scene_replay = 1; s_scene_moved = 0; fov_latch(); s_scene_epoch++; s_scene_n = 0; memset(s_scene_fam_n, 0, sizeof s_scene_fam_n); s_scene_bg = 0; s_scene_fade = 0; s_scene_freeze = 0; s_scene_hud_under = 0; s_scene_actor_front = 0; s_cur_ter_after = 0; cover_planes_clear(); openpete_mods_material_frame_reset(); /* per-present interned slots */
    /* Stamp the rebuild with the VBL it is built on, the stamp the terrain
     * builder tags its build with outside an eager scene
     * (openpete_native_scene_build_stamp). The present's retained-terrain
     * selection compares the scene stamp against that tag, so a rebuild left
     * under the canonical tick's stamp composes the canonical retained terrain
     * beside the rebuild's own feed. Stamping here keeps the stamp independent
     * of which producers the body runs; a producer's own begin_frame of the
     * same VBL is then a no-op and cannot clear what earlier producers fed. */
    extern unsigned openpete_frame;
    openpete_native_scene_begin_frame(openpete_frame);
}

/* VBL stamp of the scene accumulating in s_scene; 0xFFFFFFFF before the
 * first openpete_native_scene_begin_frame. */
static unsigned s_scene_frame = 0xFFFFFFFFu;
/* Stamp of the last begin_frame made outside the sub-tick view: the game's own
 * render, never a re-extract. 0xFFFFFFFF before the first. */
static unsigned s_scene_game_frame = 0xFFFFFFFFu;
static void (*s_scene_frame_close_cb)(unsigned frame);  /* see the frame-close section */
static void (*s_scene_superseded_cb)(unsigned frame);  /* see openpete_native_scene_register_superseded */
/* 1 from the game render's opening of a scene until the canonical capture
 * that presents it (openpete_native_scene_capture_overlay). Re-extract
 * rebuilds in between leave it set: they replace the accumulator, not the
 * game's render. */
static int s_game_scene_pending;
/* 1 from any begin_frame the game's render makes (a scene opened or fed)
 * until the canonical capture. */
static int s_game_fed;
/* The canonical present's repeat verdict (openpete_native_scene_canon_repeat),
 * -1 until it is first asked for and again after the capture. */
static int s_canon_repeat = -1;
static void alpha_census(const scene_tri_t* scene, int scene_n);  /* blend census, defined below */
static bool scene_has_world(const scene_tri_t* scene, int n);     /* defined below */
static bool scene_accum_has_world(void);                          /* defined below */
static int s_scene_dropped;       /* tris refused this frame (cap hit) - capdbg */
/** @brief Return the ring slot that still holds the scene an eager publish moved out of the accumulator, or NULL.
 *
 * Valid while the accumulator holds nothing appended since the publish and
 * the slot has not been republished (its scene id is unchanged). Only the
 * thread that publishes calls this, so the slot's triangles cannot change
 * under the caller.
 * @return The slot, or NULL. */
static const scene_slot_t *scene_moved_slot(void) {
    if (!s_scene_moved || s_scene_n != 0 || s_scene_moved_slot < 0) return NULL;
    const scene_slot_t *sl = &s_ring[s_scene_moved_slot];
    return sl->scene_id == s_scene_moved_id ? sl : NULL;
}
/** @brief Return the history slot at a delay depth, or -1 when the ring does not hold that depth.
 *
 * An entry that names a ring slot counts only while that slot still carries
 * the entry's scene.
 * @param depth Delay depth, 1 = the most recent completed scene.
 * @return The history slot, or -1. */
static int scene_hist_slot_at(int depth) {
    if (depth <= 0 || depth > s_scene_hist_count || s_scene_hist_head < 0) return -1;
    const int slot = (s_scene_hist_head - (depth - 1) + SCENE_HIST * 4) % SCENE_HIST;
    const int a = s_scene_hist_alias[slot];
    if (a >= 0 && s_ring[a].scene_id != s_scene_hist_alias_id[slot]) return -1;
    return slot;
}
/** @brief Return the triangles of a history slot that scene_hist_slot_at returned.
 *
 * @param slot History slot.
 * @return The entry's own run, or the ring slot's triangles it names. */
static const scene_tri_t *scene_hist_tris(int slot) {
    const int a = s_scene_hist_alias[slot];
    return a >= 0 ? s_ring[a].tri : (const scene_tri_t*)s_scene_hist[slot].data;
}
/** @brief Push the completed accumulator into the history ring by exchanging storage, not copying.
 *
 * The oldest slot's run becomes the accumulator's storage, which the next
 * frame refills from triangle 0. When an eager publish took the
 * accumulator's storage (s_scene_moved), the completed scene lives in the
 * ring slot it went to: with nothing fed since, the entry names that slot
 * (scene_hist_tris reads it while the slot keeps the scene id); otherwise
 * the slot's triangles and the ones fed since are copied into the entry.
 * A slot already republished leaves no scene to name, and the ring is
 * emptied so a delayed compose falls back as it does before the ring fills. */
static void scene_hist_push(void) {
    int slot = (s_scene_hist_head + 1) % SCENE_HIST;
    (void)scene_run_fit(&s_scene_hist[slot], "scene.hist", 0);   /* initialise on first use */
    s_scene_hist_alias[slot] = -1;
    if (s_scene_moved) {
        /* The completed scene is the one the publish moved to its ring slot,
         * followed by anything fed since. With nothing fed the entry names
         * the slot; otherwise the two parts are copied into the entry's run. */
        s_scene_moved = 0;
        const scene_slot_t *sl = &s_ring[s_scene_moved_slot];
        if (sl->scene_id != s_scene_moved_id) {   /* republished: nothing to name */
            s_scene_hist_head = -1;
            s_scene_hist_count = 0;
            return;
        }
        if (s_scene_n == 0) {
            s_scene_hist_alias[slot] = s_scene_moved_slot;
            s_scene_hist_alias_id[slot] = s_scene_moved_id;
            s_scene_hist_n[slot] = sl->n;
        } else {
            const int want = sl->n + s_scene_n;
            const int hn = scene_run_fit(&s_scene_hist[slot], "scene.hist", want);
            const int a = hn < sl->n ? hn : sl->n;
            scene_tri_t *d = (scene_tri_t*)s_scene_hist[slot].data;
            if (a > 0) memcpy(d, sl->tri, (size_t)a * sizeof *d);
            if (hn > a) memcpy(d + a, s_scene, (size_t)(hn - a) * sizeof *d);
            s_scene_hist_n[slot] = hn;
        }
        s_scene_hist_bg[slot] = sl->bg;
        s_scene_hist_frame[slot] = s_scene_frame;
        s_scene_hist_fade[slot] = sl->fade;
        s_scene_hist_freeze[slot] = sl->freeze;
        s_scene_hist_replay[slot] = sl->replay;
        s_scene_hist_hud_under[slot] = sl->hud_under;
        s_scene_hist_actor_front[slot] = sl->actor_front;
        s_scene_hist_cover[slot] = sl->cover;
        s_scene_hist_head = slot;
        if (s_scene_hist_count < SCENE_HIST) s_scene_hist_count++;
        return;
    }
    scene_run_swap(&s_scene_hist[slot], &s_scene_v);
    s_scene_hist_n[slot] = s_scene_n; s_scene_hist_bg[slot] = s_scene_bg;
    s_scene_hist_frame[slot] = s_scene_frame;
    s_scene_hist_fade[slot] = s_scene_fade;
    s_scene_hist_freeze[slot] = s_scene_freeze;
    s_scene_hist_replay[slot] = s_scene_replay;
    s_scene_hist_hud_under[slot] = s_scene_hud_under;
    s_scene_hist_actor_front[slot] = s_scene_actor_front;
    s_scene_hist_cover[slot] = s_cover_live;
    s_scene_hist_head = slot;
    if (s_scene_hist_count < SCENE_HIST) s_scene_hist_count++;
}

/** @brief Drop every completed-scene triangle of one blend class, under OPENPETE_SCENE_DROP_BLEND.
 *
 * OPENPETE_SCENE_DROP_BLEND=<n> names the class (0 opaque, 1 alpha, 2
 * additive, 3 subtractive), read once; unset leaves the scene whole. It
 * isolates which transparency class owns an artifact without touching any
 * feeder. */
static void scene_drop_blend_debug(void) {
    static int db = -2;
    if (db == -2) { const char* e = getenv("OPENPETE_SCENE_DROP_BLEND");
        db = (e && *e) ? atoi(e) : -1; }
    if (db < 0) return;
    int w = 0;
    for (int ti = 0; ti < s_scene_n; ti++)
        if (s_scene[ti].blend != db) { if (w != ti) s_scene[w] = s_scene[ti]; w++; }
    s_scene_n = w;
}

/** @brief Close the completed scene sitting in the accumulator and push it into the frame-history ring.
 *
 * Warns once when the scene store refused triangles, reports a new demand
 * peak under OPENPETE_SCENE_CAPDBG, applies the blend-class drop diagnostic,
 * runs the blend census, then pushes the scene (scene_hist_push). */
static void scene_close_completed(void) {
    /* Permanent safety net: if the scene cap is ever exceeded, terrain/
     * mobys silently vanish - warn once so the cap can be raised rather
     * than leaving a view-dependent dropout in the wild. */
    if (s_scene_dropped > 0) {
        static int s_warned = 0;
        if (!s_warned) { s_warned = 1;
            fprintf(stderr, "native: WARNING scene store could not grow past %zu tris "
                    "(frame=%u wanted=%d, %d tris dropped): geometry vanishes view-dependently\n",
                    s_scene_v.cap, s_scene_frame, s_scene_n + s_scene_dropped, s_scene_dropped); }
    }
    static int s_capdbg = -1; static int s_scene_peak = 0;
    if (s_capdbg < 0) { const char* e = getenv("OPENPETE_SCENE_CAPDBG"); s_capdbg = (e && *e && *e != '0'); }
    if (s_capdbg) {
        int want = s_scene_n + s_scene_dropped;   /* true demand (raised caps -> dropped=0) */
        if (want > s_scene_peak) { s_scene_peak = want;
            fprintf(stderr, "[scenecap] frame=%u NEW PEAK want=%d (accepted=%d/%zu dropped=%d)\n",
                    s_scene_frame, want, s_scene_n, s_scene_v.cap, s_scene_dropped); }
    }
    s_scene_dropped = 0;
    scene_drop_blend_debug();
    alpha_census(s_scene, s_scene_n);  /* census the completed scene (runs even headless) */
    scene_hist_push();
}

/* 1 while the frame-close callback runs: its feeds re-enter begin_frame with
 * the closing scene's stamp and must append to that scene. */
static int s_scene_close_cb_in;
void openpete_native_scene_begin_frame(unsigned frame) {
    const int game_feed = !openpete_subtick_view_active();
    if (game_feed) { s_scene_game_frame = frame; s_game_fed = 1; }
    /* The game's render owns the scene of its tick: its triangles never land
     * in a scene a re-extract built. A re-extract stamps its rebuild with the
     * VBL it runs on (openpete_native_scene_begin), so where the game ticks
     * on consecutive VBLs (a menu at VBL rate, a tick that slips onto a VBL
     * already re-extracted) the rebuild holds the stamp of the tick the game
     * is about to render. The game's first feed of that tick then closes the
     * rebuild into the history ring like any completed scene and opens its
     * own scene under the same stamp, with the replay flag clear, so the next
     * re-extract keeps it (scene_game_retain) for a framebuffer copy naming
     * the tick, and the canonical compose holds the game's render alone. The
     * closing rebuild fires no frame-close callback: that callback releases
     * the game's pending 2D batch into the scene of the VBL that built it,
     * which is the scene being opened, not the rebuild. */
    const int reclaim = game_feed && frame == s_scene_frame && s_scene_replay &&
                        !s_scene_close_cb_in;
    if (frame != s_scene_frame || reclaim) {
        /* The game opening a scene while its previous scene never reached a
         * canonical present: the present that follows shows this scene only,
         * so what the earlier render recorded for the sub-tick re-extract
         * belongs to an image no present showed. */
        if (game_feed) {
            if (s_game_scene_pending && s_scene_superseded_cb) s_scene_superseded_cb(frame);
            s_game_scene_pending = 1;
        }
        if (!reclaim && s_scene_frame != 0xFFFFFFFFu && s_scene_frame_close_cb && !s_scene_close_cb_in) {
            s_scene_close_cb_in = 1;   /* the callback feeds, which re-enters begin_frame */
            s_scene_frame_close_cb(s_scene_frame);
            s_scene_close_cb_in = 0;
        }
        fov_latch();
        if (s_scene_frame != 0xFFFFFFFFu)   /* a completed frame is sitting in s_scene */
            scene_close_completed();
        s_scene_epoch++;

        s_cur_seq = 0;
        s_cur_prim = 0;
        s_cur_fb_cell = 0;
        s_scene_n = 0; memset(s_scene_fam_n, 0, sizeof s_scene_fam_n); s_scene_bg = 0; s_scene_fade = 0; s_scene_freeze = 0; s_scene_hud_under = 0; s_scene_actor_front = 0; s_cur_ter_after = 0; cover_planes_clear(); s_scene_frame = frame;
        s_scene_replay = 0;
        /* Per-instance material slots are per-feed-cycle: reset here (the
         * canonical present's reset) as well as in scene_begin (the sub-tick
         * one). Without this, a TIME-VARYING per-instance uniform (e.g. an
         * animated pulse) mints a fresh slot every canonical feed - constant
         * uniforms dedup so the leak stays invisible - until the table fills
         * and the material silently falls back to stock. */
        openpete_mods_material_frame_reset();
    }
}
void openpete_native_scene_mark_replay(void) { s_scene_replay = 1; }

/* Accumulating-scene save/restore. A re-extract rebuilds s_scene in place, so a
 * caller that drives one purely to look at the result must put the canonical
 * scene back: the next openpete_native_scene_begin_frame snapshots whatever
 * sits in s_scene into the frame-history ring, and censuses it on the way past.
 * Saves only the live prefix (s_scene_n tris); the buffer is grown on demand and
 * never shrinks, so a steady-state caller allocates once. Returns 0 on success;
 * a failed save returns nonzero and leaves nothing to restore. */
static mod_fvec     s_scene_saved;          /* scene_tri_t */
static int          s_scene_saved_n = -1;   /* -1 = nothing saved */
static uint32_t     s_scene_saved_fam[16];
static uint32_t     s_scene_saved_bg, s_scene_saved_fade, s_scene_saved_freeze;
static int          s_scene_saved_hud_under, s_scene_saved_actor_front, s_scene_saved_dropped;
static int          s_scene_saved_ter_after;
static nr_cover_block s_scene_saved_cover;   /* the scene's portal openings */
static int          s_scene_saved_cvp_n;
static openpete_native_cover_plane s_scene_saved_cvp[8];
static int          s_scene_saved_moved;

int openpete_native_scene_save(void) {
    if (scene_run_fit(&s_scene_saved, "scene.saved", s_scene_n) < s_scene_n) return 1;
    if (s_scene_n > 0) memcpy(s_scene_saved.data, s_scene, (size_t)s_scene_n * sizeof(scene_tri_t));
    s_scene_saved_n = s_scene_n;
    memcpy(s_scene_saved_fam, s_scene_fam_n, sizeof s_scene_fam_n);
    s_scene_saved_bg = s_scene_bg; s_scene_saved_fade = s_scene_fade;
    s_scene_saved_freeze = s_scene_freeze;
    s_scene_saved_hud_under = s_scene_hud_under;
    s_scene_saved_actor_front = s_scene_actor_front;
    s_scene_saved_ter_after = s_cur_ter_after;
    s_scene_saved_cover = s_cover_live; s_scene_saved_cvp_n = s_cvp_n;
    memcpy(s_scene_saved_cvp, s_cvp, sizeof s_cvp);
    s_scene_saved_dropped = s_scene_dropped;
    s_scene_saved_moved = s_scene_moved;
    return 0;
}

void openpete_native_scene_restore(void) {
    if (s_scene_saved_n < 0) return;
    if (s_scene_saved_n > 0 && scene_room(s_scene_saved_n))
        memcpy(s_scene, s_scene_saved.data, (size_t)s_scene_saved_n * sizeof(scene_tri_t));
    s_scene_n = s_scene_saved_n;
    memcpy(s_scene_fam_n, s_scene_saved_fam, sizeof s_scene_fam_n);
    s_scene_bg = s_scene_saved_bg; s_scene_fade = s_scene_saved_fade;
    s_scene_freeze = s_scene_saved_freeze;
    s_scene_hud_under = s_scene_saved_hud_under;
    s_scene_actor_front = s_scene_saved_actor_front;
    s_cur_ter_after = s_scene_saved_ter_after;
    s_cover_live = s_scene_saved_cover; s_cvp_n = s_scene_saved_cvp_n;
    memcpy(s_cvp, s_scene_saved_cvp, sizeof s_cvp);
    s_scene_dropped = s_scene_saved_dropped;
    s_scene_moved = s_scene_saved_moved;   /* the slot still holds the moved scene */
    s_scene_saved_n = -1;
}

unsigned openpete_native_scene_frame(void) { return s_scene_frame; }
unsigned openpete_native_scene_game_frame(void) { return s_scene_game_frame; }

/* The game render's own scene of the last tick a re-extract replaced, kept for
 * a framebuffer copy: the copy names the frame the game drew into the buffer
 * (s_scene_game_frame), and a re-extract rebuilds that frame's scene under the
 * same stamp from the tick's records, which a paused tick does not hold. Only
 * a re-extract fills it, so a run without one (headless) composes from the
 * history and the accumulator as before. */
static struct {
    mod_fvec v;                 /* scene_tri_t */
    int      n;                 /* triangles held; 0 = nothing kept */
    unsigned frame;
    uint32_t bg, fade, freeze;
    int      hud_under, actor_front;
    nr_cover_block cover;       /* its portal openings */
} s_scene_game;

/** @brief Keep the game render's accumulated scene before a re-extract wipes it, by exchanging storage.
 *
 * The accumulator takes the kept scene's former run, which the re-extract
 * overwrites from triangle 0. Nothing is kept when the accumulator is empty
 * or its storage went to an eager ring slot (the slot holds the scene), so
 * the previous kept scene stands. */
static void scene_game_retain(void) {
    if (s_scene_n <= 0 || s_scene_moved) return;
    (void)scene_run_fit(&s_scene_game.v, "scene.game", 0);   /* initialise on first use */
    scene_run_swap(&s_scene_game.v, &s_scene_v);
    s_scene_game.n = s_scene_n;
    s_scene_game.frame = s_scene_frame;
    s_scene_game.bg = s_scene_bg; s_scene_game.fade = s_scene_fade;
    s_scene_game.freeze = s_scene_freeze;
    s_scene_game.hud_under = s_scene_hud_under;
    s_scene_game.actor_front = s_scene_actor_front;
    s_scene_game.cover = s_cover_live;
}

/* ---- frame-close notification ------------------------------------------- */
void openpete_native_scene_register_frame_close(void (*fn)(unsigned frame)) {
    s_scene_frame_close_cb = fn;
}

void openpete_native_scene_register_superseded(void (*fn)(unsigned frame)) {
    s_scene_superseded_cb = fn;
}

/** @brief Return the VBL stamp of the scene at a delay depth: the accumulator's at depth 0, a history entry's above it.
 *
 * @param d Delay depth.
 * @return The stamp, or 0xFFFFFFFF when the ring does not hold that depth. */
static unsigned scene_frame_at_depth(int d) {
    if (d <= 0) return s_scene_frame;
    const int slot = scene_hist_slot_at(d);
    return slot < 0 ? 0xFFFFFFFFu : s_scene_hist_frame[slot];
}

/** @brief Return the deepest scene-ring delay depth currently held.
 *
 * The count of completed scenes recorded, saturating at SCENE_HIST, so a depth
 * above it has no slot (scene_hist_slot_at). */
static int scene_hist_depth_max(void) { return s_scene_hist_count; }

/* --vertex-precision (`--vertex-precision integer`, live via the O-menu). When
 * set, openpete_native_scene_add_tri_grouped rounds each 3D-world vertex's PSX
 * screen coordinate (sx,sy - still in 512x240 display space at that point) to
 * the integer pixel grid, reproducing the original GTE's int16 SXY snap: the
 * classic PS1 polygon wobble. 0 = smooth sub-pixel (default). Depth (z) and UVs
 * are left alone, so the true Z-buffer + perspective-correct texturing survive.
 * A file-static (not an s_nr field) so the feed chokepoint above s_nr can read
 * it; seeded from opts->vertex_precision, mutated live by the setter below. */
static int s_vertex_snap = 0;
/* Sticky feed state (set_presnap): the current feeder emits coordinates that
 * are ALREADY snap-consistent, so the chokepoint snap below must not touch
 * them. The terrain feeder self-snaps (its near/HQ subdivide places boundary
 * midpoints at exact off-integer positions ON snapped parent edges - a
 * per-vertex snap here would break that collinearity and re-open T-junction
 * cracks); every other feeder leaves this 0 and gets the chokepoint snap. */
static int s_cur_presnap = 0;
/* --raster-quantize multiplier (0 = off): engine SCENE-point resample of the
 * composed 3D scene onto the 512Nx240N virtual PSX raster - an integer
 * multiple of the vertex-snap grid, so snapped edges always land on cell
 * boundaries (see native_postfx_run). One point sample per cell, as the
 * hardware rasterizer produced. */
static int s_raster_mult = 0;
static int s_dither = 0;       /* --dither: PS1 15-bit colour + 4x4 hardware dither (engine SCENE pass) */
/* --terrain-visibility: the terrain feeder's candidate-set policy
 * (openpete_terrain_visibility_t), seeded at init and moved by the setter. */
static int s_terrain_vis = OPENPETE_TERRAIN_VIS_FRUSTUM;
/* --texture-mapping (0 = perspective, 1 = affine): the live mode plus the
 * queued one. The mode is a vertex spec constant baked into every scene
 * pipeline, so a change cannot apply mid-frame like the knobs above - the
 * setter queues it and texmap_apply_pending() re-bakes all pipelines at the
 * next present boundary (rhi_rebuild_pipelines; the replaced objects are
 * destroyed once the frames in flight that may bind them retire). */
static int s_texmap_affine  = 0;
static int s_notex          = 0;   /* untextured lighting view (spec-const 2) */
static int s_notex_pending  = 0;
static int s_texmap_pending = 0;
static int s_wireframe         = 0;   /* line-mode scene pipelines + NOTEX colour */
static int s_wireframe_pending = 0;

/** @brief Return whether a draw layer takes the PSX integer-grid vertex snap of --vertex-precision integer.
 *
 * The 3D-projected layers wobbled on hardware because the GTE stored screen
 * X and Y as int16, so they snap. The flat 2D UI layers (HUD text and icons,
 * the dim HUD panel, and the OVERLAY layer drawn over everything) were placed
 * by the game at integer screen coordinates; snapping them adds sub-pixel
 * change and no wobble, so they do not snap. HUD3D, the spinning 3D icon
 * mobys, is a GTE projection and snaps.
 * @param category Scene category, an OPENPETE_SCENE_* value.
 * @return 0 for HUD, OVERLAY and HUD_PANEL, else 1. */
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

/* openpete_scene_vert.pvz is mandatory: every feeder states the vertex's
 * true view-Z. A vertex arriving without one (pvz <= 0) is counted, named
 * once per category (with its feeder tag) on stderr, and takes its ordering
 * view-Z, so its W is the depth's own view-Z. */
static unsigned s_pvz_missing = 0;
unsigned openpete_native_scene_pvz_missing(void) { return s_pvz_missing; }
/** @brief Repair vertices that arrive without a true view-Z and count them.
 *
 * For each of the three vertices with pvz not above 0, increments
 * s_pvz_missing, logs the first occurrence per category with the current tag on
 * stderr, and sets pvz to the vertex's ordering z. The category is reduced
 * modulo 32 for the once-per-category record.
 * @param v The triangle's three vertices; pvz is rewritten in place.
 * @param category Draw category, used only for the log record. */
static void scene_vert_require_pvz(openpete_scene_vert v[3], int category)
{
    static uint32_t s_named = 0;
    for (int k = 0; k < 3; k++) {
        if (v[k].pvz > 0.0f) continue;
        s_pvz_missing++;
        const uint32_t bit = 1u << ((unsigned)category & 31u);
        if (!(s_named & bit)) {
            s_named |= bit;
            OPL_ERROR_F("native", "scene vertex without true view-Z (pvz=%g, z=%g) "
                        "in category %d, tag %08x", (double)v[k].pvz, (double)v[k].z,
                        category, s_cur_tag);
        }
        v[k].pvz = v[k].z;
    }
}
/** @brief The blend class a stored triangle keeps: blend itself when it is 0..NR_BLEND_CLASSES-1, else 0 (opaque). */
static int scene_blend_class(int blend) {
    return (blend >= 0 && blend < NR_BLEND_CLASSES) ? blend : 0;
}

/** @brief Set vertex alpha 0x80 on an untextured triangle of the alpha blend class.
 *
 * PSX ABR 0 is (B+F)/2. An untextured triangle of class NR_BLEND_AVG draws
 * through a SRC_ALPHA / ONE_MINUS_SRC_ALPHA pipe that reads the vertex
 * alpha, so the intake states the half for every feeder. A textured
 * triangle is left alone: it takes its 0.5 from the texel's STP bit, and
 * its vertex alpha byte carries the moby fog band instead.
 * @param s Stored triangle whose blend class is already validated. */
static void scene_abr0_alpha(scene_tri_t *s) {
    if (s->textured || s->blend != NR_BLEND_AVG) return;
    for (int k = 0; k < 3; k++)
        s->v[k].rgba = (s->v[k].rgba & 0x00FFFFFFu) | (0x80u << 24);
}

void openpete_native_scene_add_tri_grouped(const openpete_scene_tri *tri, int category,
                                           int carry, int group) {
    if (!tri) return;
    if (s_scene_n < (int)s_scene_v.cap || scene_room(s_scene_n + 1)) {
        /* geometry+material prefix copy (the feeder tri IS the stored tri's
         * layout prefix - asserted at scene_tri_t), then stamp the routing
         * state from the explicit args. */
        memcpy(&s_scene[s_scene_n], tri, sizeof *tri);
        /* One validation of the blend class for every pass that indexes a
         * table by it; a class outside the table draws opaque, as each
         * pass's own clamp already maps it. */
        s_scene[s_scene_n].blend = scene_blend_class(s_scene[s_scene_n].blend);
        scene_abr0_alpha(&s_scene[s_scene_n]);
        s_scene[s_scene_n].category = category;  /* explicit layer (ignored for blend!=0) */
        s_scene[s_scene_n].carry = carry;        /* explicit 2D-UI sub-tick carry flag */
        s_scene[s_scene_n].group = group;        /* portal grouping id (PORTAL_* only) */
        s_scene[s_scene_n].tag = s_cur_tag;      /* sticky provenance (set_tag) */
        s_scene_fam_n[s_cur_tag >> 28]++;
        s_scene[s_scene_n].fog_delta = s_cur_fog_delta;  /* sticky (set_fog_delta) */
        s_scene[s_scene_n].seq = s_cur_seq;              /* sticky (set_seq) */
        s_scene[s_scene_n].prim = s_cur_prim;            /* sticky (set_prim) */
        s_scene[s_scene_n].stp_sel = s_cur_stp_sel;      /* sticky (set_stp_select) */
        s_scene[s_scene_n].fb_cell = s_cur_fb_cell;      /* sticky (set_fb_cell) */
        s_scene[s_scene_n].host_added = s_cur_host_added; /* sticky (set_host_added) */
        s_scene[s_scene_n].portal_rim = s_cur_portal_rim; /* sticky (set_portal_rim) */
        scene_stamp_key_cells(&s_scene[s_scene_n]);
        s_scene[s_scene_n].ter_after = s_cur_ter_after;   /* sticky (mark_terrain_feed) */
        if (s_cur_tag && tag_tint_on()) {
            /* Coverage instrument: hash the tag to a stable tint and modulate
             * the vertex colours - "which source produced this pixel".
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
         * the PSX integer pixel grid - the polygon wobble. TRUNCATE (floor), not
         * round: the GTE derives SXY via MAC0 >> 16, an arithmetic shift = floor,
         * and the choice is load-bearing beyond fidelity - round-half-up measured
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
        scene_vert_require_pvz(s_scene[s_scene_n].v, category);
        /* OPENPETE_TRUE_W: kill switch / bisect mask for the stated perspective
         * W (openpete_scene_vert.pvz). 0 replaces every feeder's value with the
         * vertex's ordering view-Z (W from the ordering depth);
         * a bitmask keeps it for selected categories only (1 = PLAYER,
         * 2 = ACTOR/HUD3D mobys, 4 = WORLD terrain), which is how a divergence
         * gets attributed to one feeder without a rebuild. Default = all on. */
        {
            /* The parse is masked to 0..7 so the cache stays non-negative and
             * the `< 0` first-use latch actually holds - a negative "unset"
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
                    for (int k = 0; k < 3; k++)
                        s_scene[s_scene_n].v[k].pvz = s_scene[s_scene_n].v[k].z;
            }
        }
        /* Feed-time carry accumulation (see s_overlay_accum): a fresh 2D-UI /
         * overlay tri joins this tick's carry set NOW, so it survives however
         * many sub-tick scene wipes land before the canonical capture. carry==2
         * (a re-applied carry tri routed through a feeder) never re-accumulates. */
        if ((category == OPENPETE_SCENE_OVERLAY || category == OPENPETE_SCENE_HUD_PANEL ||
             carry == 1) && carry != 2) {
            if (s_overlay_accum_n < OVERLAY_CARRY_MAX) {
                s_overlay_accum_epoch[s_overlay_accum_n] = s_scene_epoch;
                s_overlay_accum[s_overlay_accum_n++] = s_scene[s_scene_n];
            } else
                s_overlay_accum_dropped++;
        }
        s_scene_n++;
    } else {
        s_scene_dropped++;   /* cap hit - this tri (and the rest of the feed) is lost */
    }
}
void openpete_native_scene_add_tri(const openpete_scene_tri *tri, int category, int carry) {
    openpete_native_scene_add_tri_grouped(tri, category, carry, 0);
}
void openpete_native_scene_set_bg(uint32_t rgba) { s_scene_bg = rgba; }

/* Gamestate draw-env clear colour (the framebuffer background the canonical
 * render auto-clears to). Two producers: the PutDrawEnv thunk (fallback,
 * trails the OT build by one VSync - the game PutDrawEnvs an OT's env only
 * when it DRAWS it) and the game module's per-canonical-present sync from the
 * DRAWENV struct in RAM (spyro-1: cyclorama_native_env_clear_commit reads
 * g_CurDB->m_DrawEnv, the env the presented scene's DrawOTag will use -
 * without it the transition boundary flashed the stale clear for a present).
 * The primary bg source in draw_native_scene (preferred over g_Cyclorama,
 * which is stale in cyclorama-less states). rgba R,G,B,A; 0 = unset / no
 * auto-clear -> fall back to the scene-fed colour. Persists across sub-tick
 * presents (both producers fire on canonical frames only). */
static uint32_t s_drawenv_clear = 0;
void openpete_native_set_clear_color(int isbg, int r, int g, int b) {
    uint32_t v = isbg
        ? (0xFF000000u | (uint32_t)(r & 0xFF) | ((uint32_t)(g & 0xFF) << 8)
                       | ((uint32_t)(b & 0xFF) << 16))
        : 0u;   /* no auto-clear -> defer to the scene-fed bg / ref corner */
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
 * freeze never cleared when the menu closed -> the world stayed hidden on unpause. */
static uint32_t s_game_freeze = 0;
static int      s_game_freeze_seen = 0;
/* Freeze-backdrop production mode (see openpete_native_scene_set_freeze_retained):
 * 0 = the game emits a grayscale+tinted quad, 1 = the game suppresses the
 * framebuffer clear and the snapshot is drawn unaltered. */
static int      s_freeze_retained = 0;

void openpete_native_scene_set_freeze_retained(int retained) { s_freeze_retained = retained ? 1 : 0; }

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

void openpete_native_scene_set_seq(uint32_t seq) { s_cur_seq = seq; }
void openpete_native_scene_set_prim(uint32_t prim) { s_cur_prim = prim; }
void openpete_native_scene_set_fb_cell(uint32_t cell) { s_cur_fb_cell = cell; }
void openpete_native_scene_set_stp_select(int sel) {
    s_cur_stp_sel = (sel > 0 && sel < OPENPETE_STP_SELS) ? sel : OPENPETE_STP_ALL;
}

int openpete_native_scene_collect_2d_prims(uint32_t* out, int max) {
    int n = 0;
    if (!out || max <= 0) return 0;
    for (int t = 0; t < s_scene_n && n < max; t++)
        if (s_scene[t].prim) out[n++] = s_scene[t].prim;
    for (int t = 0; t < s_overlay_accum_n && n < max; t++)
        if (s_overlay_accum[t].prim) out[n++] = s_overlay_accum[t].prim;
    return n;
}

void openpete_native_scene_rank_2d(uint32_t (*rank_of)(uint32_t prim)) {
    if (!rank_of) return;
    for (int t = 0; t < s_scene_n; t++)
        if (s_scene[t].prim) s_scene[t].seq = rank_of(s_scene[t].prim);
    for (int t = 0; t < s_overlay_accum_n; t++)
        if (s_overlay_accum[t].prim) s_overlay_accum[t].seq = rank_of(s_overlay_accum[t].prim);
}

void openpete_native_scene_set_hud_under_player(int under) { s_scene_hud_under = under ? 1 : 0; }

void openpete_native_scene_set_actor_front(int front) { s_scene_actor_front = front ? 1 : 0; }

/* --- 2D-overlay carry across sub-tick re-extracts -------------------------
 * The fade overlay (func_800190D4) and the cutscene letterbox bars
 * (func_80018F30, OVERLAY-category F4s) + menu panel (HUD_PANEL) are produced
 * by the 2D-UI feeder while the game renders. They are NOT re-derivable from
 * gamestate by the sub-tick re-extract (it has no 2D-prim chokepoint to hook),
 * and the re-extract's scene_begin() wipes them - so without carrying them they
 * vanish on every re-extracted present (which, since the canonical present now
 * also routes through the re-extract for the 1-tick lag, means they vanish
 * outright). They're screen-space and tick-constant, so they are copied into
 * an accumulation buffer as they are fed (s_overlay_accum), committed as the
 * carry on the canonical present and re-applied after the rebuild. Categories that the
 * 3D feeders re-produce (HUD/HUD3D billboards, PLAYER near-OT text) are NOT
 * carried by category - they'd double. The 2D-UI HUD SPRITES/LINES (title logo,
 * "PRESS START", HUD glyph text, UI separators) live in OPENPETE_SCENE_HUD too,
 * but the 3D feeders never re-derive THEM, so they carry by an explicit per-tri
 * flag (the add_tri carry argument) that the moby near-OT billboards leave 0.
 * (OVERLAY_CARRY_MAX + the feed-time accumulation buffer live up by s_scene.) */
static scene_tri_t s_overlay_carry[OVERLAY_CARRY_MAX];      /* curr (latest canonical capture) */
static int                s_overlay_carry_n;
static uint32_t           s_overlay_carry_epoch[OVERLAY_CARRY_MAX];
static uint32_t           s_overlay_carry_freeze;
/* The previous canonical tick's freeze word, so the freeze fade (bits 30:24)
 * interpolates sub-tick like the fade word: the exit-level freeze darkens one
 * CLUT step per tick, and applying the newest word on every sub-tick present
 * put the intermediates a step ahead of the canonical frame. */
static uint32_t           s_overlay_prev_freeze;
/* The previous canonical carry set, so apply_overlay can interpolate the SCREEN
 * POSITION of an animated 2D-UI element across sub-ticks - the title-logo drop-in
 * slide moves a few px/tick; held (un-interpolated) it staircases at 30 Hz just
 * like the fade did. Rolled curr->prev by capture_overlay (canonical only); paired
 * by index when the two captures have equal count + matching sprite identity
 * (category/textured/uv), else that tri snaps to curr (a structural change - a
 * blink toggling the count, a glyph swap - is discrete anyway). */
static scene_tri_t s_overlay_prev[OVERLAY_CARRY_MAX];
static int                s_overlay_prev_n;
/* Tick-grid timestamps (openpete_interp_tick_stamp) of the two most-recent CANONICAL overlay captures, so
 * apply_overlay computes the position/fade interpolation alpha from the OVERLAY's
 * OWN cadence - NOT the camera-bracket alpha, which the re-extract leaves at 0
 * whenever the camera+Spyro ring dedupes (a STATIC camera: the title screen, a
 * pause menu). The logo drop-in slides while the title camera is still, so without
 * a camera-independent alpha it staircased at the canonical tick rate. (Same trap
 * the 3D HUD-icon spin/slide hit - fixed there with per-object timestamps.) */
static op_time_t          s_overlay_curr_ns, s_overlay_prev_ns;

/* The fade ramps per sim tick, so unlike the (static) letterbox/panel tris it is
 * interpolated rather than held: s_fade_prev/curr are the two most-recent
 * CANONICAL fades (rolled by commit_fade on each canonical present), and
 * apply_overlay lerps them by the spine alpha - a 1-tick-lagged smooth ramp,
 * consistent with the rest of the scene. Without this the fade staircases at
 * 30 Hz (intermediates hold the previous tick's brightness). */
static uint32_t s_fade_prev, s_fade_curr;

/** @brief Report whether OPENPETE_LOAD_CENSUS is set to a value other than one starting with `0`.
 *
 * Read once.
 * @return 1 when the load-window census is on, else 0. */
static int load_census_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); on = (e && *e && *e != '0'); }
    return on;
}

void openpete_native_scene_commit_fade(void) {
    /* A repeat present shows the previous scene, its fade and freeze with it.
     * The render that ends a run of repeats starts its fade and freeze from
     * no previous word, as a fresh fade does: the run may hide a scene change
     * (a level load), and a lerp from the held words would show the new scene
     * under the old screen's fade for part of a tick. */
    static int s_after_repeat;
    if (openpete_native_scene_canon_repeat()) {
        s_fade_prev = s_fade_curr;
        s_overlay_prev_freeze = s_overlay_carry_freeze;
        s_game_fade_seen = 0; s_game_freeze_seen = 0;
        s_after_repeat = 1;
        return;
    }
    if (s_after_repeat) { s_fade_curr = 0; s_overlay_carry_freeze = 0; s_after_repeat = 0; }
    s_fade_prev = s_fade_curr;
    /* The fade is active this tick only if the game actually called set_fade
     * since the last commit; else it ended (the game stopped calling) -> off. */
    s_fade_curr = s_game_fade_seen ? s_game_fade : 0;
    s_game_fade_seen = 0;
    /* Same one-tick liveness for the freeze backdrop carry: held while the game keeps
     * emitting the freeze FT4, cleared the tick it stops (menu closed -> unpause), so
     * the spine's apply_overlay turns the backdrop off and the world reappears. (This
     * replaces capturing it from s_scene_freeze, which fed back into itself.) */
    uint32_t prev_freeze = s_overlay_carry_freeze;
    uint32_t new_freeze  = s_game_freeze_seen ? s_game_freeze : 0;
    /* Bridge a 1-frame freeze gap at the GS_Paused -> GS_ExitLevel handoff: the pause
     * menu stops emitting its freeze FT4 the frame it closes, but GS_ExitLevel's
     * func_8001C694 hasn't drawn its own freeze yet - so for exactly one canonical
     * tick neither sets the freeze and the carry would drop, letting the spine's
     * re-extracted (reset) Spyro + bg flash through. The game doesn't flicker because
     * its frozen framebuffer persists in VRAM. A handoff frame carries only the reset
     * Spyro (PLAYER) + bg with no real WORLD/ACTOR geometry, so when the freeze is
     * dropping but the scene has no world, hold the previous freeze. A genuine unpause
     * feeds terrain that same tick, as WORLD triangles or as retained-cache sectors,
     * so scene_accum_has_world is true and the freeze drops correctly - the world
     * reappears with no lag. */
    /* Only the emitted-quad mode has such a gap. Under retention the arming
     * signal is the latched DRAWENV.isbg word: the tick it reads 1 again the
     * game has cleared the framebuffer and draws its own content, so holding
     * the previous freeze would blank that content for as long as the screen
     * stays world-less. */
    int held = 0;
    const int ter_fed = s_cur_ter_after;
    if (!s_freeze_retained &&
        new_freeze == 0 && prev_freeze != 0 && !scene_accum_has_world()) {
        held = 1;
        new_freeze = prev_freeze;
        /* The canonical present of this degenerate tick may show the RAW game-fiber
         * scene (the sub-tick re-extract that re-applies the carry is skipped on a
         * stuck/transition tick), which left the live s_scene_freeze at 0 - so force
         * it too, and drop the degenerate geometry (reset Spyro + bg) so only the
         * held freeze shows, exactly as on a settled freeze frame. */
        s_scene_freeze = new_freeze;
        s_scene_n = 0; memset(s_scene_fam_n, 0, sizeof s_scene_fam_n); s_cur_ter_after = 0;
    }
    if (load_census_on() && s_cover_live.cnt[0] > 0 && !ter_fed) {
        int blend = 0;
        for (int t = 0; t < s_scene_n; t++) blend += s_scene[t].blend != 0;
        fprintf(stderr, "[census f=%u COVER] stale planes=%d blend=%d n=%d\n",
                s_scene_frame, s_cover_live.cnt[0], blend, s_scene_n);
    }
    if ((held || new_freeze != prev_freeze) && load_census_on())
        fprintf(stderr, "[census f=%u FREEZE] prev=%08x new=%08x held=%d ter=%d game=%08x\n",
                s_scene_frame, prev_freeze, new_freeze, held, ter_fed,
                s_game_freeze_seen ? s_game_freeze : 0u);
    s_overlay_prev_freeze  = prev_freeze;
    s_overlay_carry_freeze = new_freeze;
    s_game_freeze_seen = 0;
}

/** @brief Interpolate two packed freeze words at a sub-tick fraction.
 *
 * A freeze word packs bit 31 active, bits 30..24 the fade shift and bits
 * 23..0 the tint. Only the fade shift is interpolated, rounded to the
 * nearest integer. An inactive @p b returns 0, an inactive @p a returns
 * @p b, and a tint change returns @p b.
 * @param a Freeze word of the previous tick.
 * @param b Freeze word of the current tick.
 * @param t Fraction from @p a (0) to @p b (1).
 * @return The interpolated freeze word. */
static uint32_t freeze_lerp(uint32_t a, uint32_t b, double t) {
    if (!(b & (1u << 31))) return 0;
    if (!(a & (1u << 31))) return b;
    if ((a & 0xFFFFFFu) != (b & 0xFFFFFFu)) return b;
    int af = (int)((a >> 24) & 0x7Fu), bf = (int)((b >> 24) & 0x7Fu);
    int f = (int)(af + (bf - af) * t + 0.5);
    return (b & 0x80FFFFFFu) | ((uint32_t)f << 24);
}

/** @brief Interpolate two packed fade words at a sub-tick fraction.
 *
 * A fade word packs bit 31 active, bits 25..24 the ABR blend mode and bits
 * 23..0 the RGB colour. Each colour channel is interpolated and rounded to
 * the nearest integer; the top byte is taken from @p b. An inactive @p b
 * returns 0, an inactive @p a returns @p b, and a blend-mode change returns
 * @p b.
 * @param a Fade word of the previous tick.
 * @param b Fade word of the current tick.
 * @param t Fraction from @p a (0) to @p b (1).
 * @return The interpolated fade word. */
static uint32_t fade_lerp(uint32_t a, uint32_t b, double t) {
    if (!(b & (1u << 31))) return 0;                         /* curr inactive -> no fade */
    if (!(a & (1u << 31))) return b;                         /* prev inactive -> snap on */
    if ((a & (3u << 24)) != (b & (3u << 24))) return b;      /* mode changed -> snap */
    int ar=(a>>16)&0xFF, ag=(a>>8)&0xFF, ab=a&0xFF;
    int br=(b>>16)&0xFF, bg=(b>>8)&0xFF, bb=b&0xFF;
    int r =(int)(ar+(br-ar)*t+0.5), g=(int)(ag+(bg-ag)*t+0.5), bl=(int)(ab+(bb-ab)*t+0.5);
    return (b & 0xFF000000u) | ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)bl;
}

/** @brief Return whether 2D emission-sequence ordering is on.
 *
 * OPENPETE_SEQ_SORT=0 turns it off at runtime, so an A/B that attributes a
 * divergence to the ordering runs on one binary. Read once and cached.
 * @return 1 when on (the default), 0 when off. */
static int seq_sort_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SEQ_SORT"); v = !(e && *e && *e == '0'); }
    return v;
}

/** @brief Sort each run of numbered triangles in the 2D carry by emission sequence, moving the epochs with them.
 *
 * A run is a contiguous stretch of triangles with a nonzero
 * scene_tri_t::seq; triangles with seq 0 stay in place and bound the runs.
 * Each run is sorted within itself by a stable insertion sort, which is
 * linear in practice because the sets are one screen of 2D (tens of
 * triangles) and nearly sorted. apply_overlay re-applies the carry block in
 * feed order, so a run keeps the same neighbours on the sub-tick leg as on
 * the canonical one and a blended sprite composes against the same
 * background in both. No-op when OPENPETE_SEQ_SORT=0.
 * @param tri Carry triangles.
 * @param epoch Per-triangle epochs, permuted in step with @p tri.
 * @param n Triangle count. */
static void seq_sort_carry(scene_tri_t* tri, uint32_t* epoch, int n) {
    if (!seq_sort_on()) return;
    /* Order each contiguous run of numbered tris within itself, exactly as the
     * scene is ordered. The carry keeps the feed's own sequence of runs, and
     * apply_overlay re-applies the block in that sequence, so a run's members
     * stay between the same neighbours on the sub-tick leg as on the canonical
     * one, and a blended sprite composes against the same background in both.
     * Gathering every numbered tri and permuting it across the whole carry
     * would let one cross the unnumbered tris separating two runs. */
    int i = 0;
    while (i < n) {
        if (!tri[i].seq) { i++; continue; }
        int j = i;
        while (j < n && tri[j].seq) j++;              /* [i, j) is one run */
        for (int a = i + 1; a < j; a++) {             /* stable insertion sort */
            scene_tri_t t = tri[a]; uint32_t e = epoch[a];
            int b = a - 1;
            while (b >= i && tri[b].seq > t.seq) { tri[b + 1] = tri[b]; epoch[b + 1] = epoch[b]; b--; }
            tri[b + 1] = t; epoch[b + 1] = e;
        }
        i = j;
    }
}

/** @brief Sort each run of numbered triangles in the scene by emission sequence, leaving every other triangle in place.
 *
 * The 2D layer draws in array order, so this restores the order in which
 * the game emitted the two 2D producers, including when one of them was fed
 * from a deferred release. A run is a contiguous stretch of triangles with a
 * nonzero scene_tri_t::seq, produced consecutively by one feed; sorting
 * within it (stable insertion sort) gives the painter order the ordering
 * table asks for, and no triangle crosses the unnumbered geometry between
 * two runs, which would change the background a blended sprite composes
 * against. No-op when OPENPETE_SEQ_SORT=0.
 * @param sc Scene triangles.
 * @param n Triangle count. */
static void seq_sort_scene_2d(scene_tri_t* sc, int n) {
    if (!seq_sort_on()) return;
    /* Order each contiguous run of numbered tris within itself. Gathering every
     * numbered tri and permuting it across the whole scene would let a 2D tri
     * move past unnumbered geometry lying between two runs, and a blended
     * sprite that crosses the geometry behind it composes against a different
     * background, which is a visible change nothing asked for. A run is what
     * one feed produced consecutively, so ordering inside it is exactly the
     * painter order the ordering table asks for and nothing else moves. */
    int i = 0;
    while (i < n) {
        if (!sc[i].seq) { i++; continue; }
        int j = i;
        while (j < n && sc[j].seq) j++;               /* [i, j) is one run */
        for (int a = i + 1; a < j; a++) {             /* stable insertion sort */
            scene_tri_t t = sc[a];
            int b = a - 1;
            while (b >= i && sc[b].seq > t.seq) { sc[b + 1] = sc[b]; b--; }
            sc[b + 1] = t;
        }
        i = j;
    }
}

/** @brief Count the scene's 2D faces: HUD, HUD panel and overlay categories plus carried overlay copies.
 * @param sc Scene triangles.
 * @param n Triangle count.
 * @return Number of 2D faces. */
int scene_2d_count_fwd(const scene_tri_t* sc, int n) {
    int c = 0;
    for (int t = 0; t < n; t++)
        if (sc[t].category == OPENPETE_SCENE_HUD || sc[t].category == OPENPETE_SCENE_HUD_PANEL ||
            sc[t].category == OPENPETE_SCENE_OVERLAY || sc[t].carry) c++;
    return c;
}

/* ---- retained draw-area content -----------------------------------------
 *
 * A draw area the game puts with auto-clear off keeps what it holds: the image
 * it held when the run began (the frozen frame the retained freeze backdrop
 * draws) plus every 2D primitive drawn into it since, in order. The game draws
 * each such primitive once (the black strips that fill the screen a shrinking
 * window has vacated) and the native scene would hold it for that one tick.
 * The area's held list keeps the 2D entries of every earlier tick of the run
 * and replays them under the tick's own 2D, on the canonical scene and on every
 * re-extract. An entry is dropped once a later tick paints over every pixel it
 * covers with opaque entries, which is what the buffer does with it, and keeps
 * the list bounded while a menu redraws itself. The list starts afresh when the
 * area's content epoch changes (openpete_fb_snapshot_retained_area). */
#define HELD_MAX   16384                 /* entries per area */
#define HELD_GRID_W 512                  /* coverage grid, PSX screen pixels */
#define HELD_GRID_H 256
typedef struct {
    scene_tri_t *t; int n, cap;
    unsigned epoch; int valid;
} held_list_t;
static held_list_t s_held[OPENPETE_FB_SNAPSHOT_AREA_MAX];
static int s_held_area = -1;   /* area whose list the tick's scenes replay, -1 none */
static int s_held_replay;      /* entries of that list from earlier ticks */
static uint8_t s_held_mask[HELD_GRID_H][HELD_GRID_W];
/* Opaque full-buffer primitives the game drew since the last capture that the
 * scene does not hold as triangles (the freeze backdrop's FT4s, which the
 * native draws as its backdrop): they paint over everything drawn before them. */
#define HELD_COVER_MAX 128
static scene_tri_t s_held_cover[HELD_COVER_MAX];
static int s_held_cover_n;

void openpete_native_scene_note_cover_quad(const float x[4], const float y[4]) {
    static const int idx[6] = { 0, 1, 2, 1, 3, 2 };
    for (int t = 0; t < 2 && s_held_cover_n < HELD_COVER_MAX; t++) {
        scene_tri_t q; memset(&q, 0, sizeof q);
        for (int k = 0; k < 3; k++) { q.v[k].sx = x[idx[t * 3 + k]]; q.v[k].sy = y[idx[t * 3 + k]]; }
        s_held_cover[s_held_cover_n++] = q;
    }
}

/** @brief Return whether a direct-colour (16 bpp) textured entry samples no transparent texel.
 *
 * The GPU leaves a pixel untouched where the texel is 0x0000, so the entry
 * covers its pixels only when no texel of its texel box in the VRAM model is
 * zero. A box inside a draw area samples a rendered frame and counts as
 * solid (a rendered pixel of exactly 0x0000 is the exception it misses). A
 * box reaching outside VRAM reports not solid.
 * @param q Entry.
 * @return 1 when every texel of the box is non-zero. */
static int held_texels_solid(const scene_tri_t* q) {
    float u0 = q->v[0].u, u1 = u0, v0 = q->v[0].v, v1 = v0;
    for (int k = 1; k < 3; k++) {
        u0 = fminf(u0, q->v[k].u); u1 = fmaxf(u1, q->v[k].u);
        v0 = fminf(v0, q->v[k].v); v1 = fmaxf(v1, q->v[k].v);
    }
    const int x0 = q->tpage_x + (int)floorf(u0), x1 = q->tpage_x + (int)ceilf(u1);
    const int y0 = q->tpage_y + (int)floorf(v0), y1 = q->tpage_y + (int)ceilf(v1);
    if (x0 < 0 || y0 < 0 || x1 > OPENPETE_VRAM_W || y1 > OPENPETE_VRAM_H) return 0;
    /* Texels inside a draw buffer are pixels the game rendered there, which
     * the VRAM model does not hold under a native render. */
    if (openpete_fb_snapshot_area_holds(x0, y0, x1 - x0, y1 - y0)) return 1;
    const uint16_t* vram = openpete_vram_data();
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (vram[(size_t)y * OPENPETE_VRAM_W + (size_t)x] == 0) return 0;
    return 1;
}

/** @brief Return whether a 2D entry paints every pixel it covers with no blend and no transparent texel.
 *
 * Untextured entries count when opaque (blend class 0) or drawn through the
 * alpha pipe at full alpha on all three vertices; textured entries when they
 * sample a framebuffer snapshot opaquely, or a direct-colour page opaquely
 * with no zero texel (held_texels_solid). A CLUT entry never counts.
 * @param q Entry.
 * @return 1 when opaque. */
static int held_opaque(const scene_tri_t* q) {
    if (q->bpp == OPENPETE_FB_SNAPSHOT_BPP) return q->blend == 0;
    if (q->textured) return q->blend == 0 && q->bpp == 16 && held_texels_solid(q);
    if (q->blend == 0) return 1;
    return q->blend == 1 && (q->v[0].rgba >> 24) == 0xFFu &&
           (q->v[1].rgba >> 24) == 0xFFu && (q->v[2].rgba >> 24) == 0xFFu;
}

/** @brief Walk the coverage-grid pixels whose centres lie inside a triangle, edges inclusive.
 *
 * With @p set the pixels are marked; otherwise the walk reports whether
 * every one is marked. A triangle reaching outside the grid reports
 * uncovered, so it is never dropped.
 * @param q Triangle in PSX screen coordinates.
 * @param set 1 to mark, 0 to test.
 * @return In test mode 1 when every covered pixel is marked; 1 in mark mode. */
static int held_raster(const scene_tri_t* q, int set) {
    const float x0 = q->v[0].sx, y0 = q->v[0].sy, x1 = q->v[1].sx, y1 = q->v[1].sy,
                x2 = q->v[2].sx, y2 = q->v[2].sy;
    const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area == 0.0f) return 1;
    const float sg = area > 0.0f ? 1.0f : -1.0f;
    const int bx0 = (int)floorf(fminf(x0, fminf(x1, x2))), bx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    const int by0 = (int)floorf(fminf(y0, fminf(y1, y2))), by1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    if (!set && (bx0 < 0 || by0 < 0 || bx1 > HELD_GRID_W || by1 > HELD_GRID_H)) return 0;
    for (int y = by0 < 0 ? 0 : by0; y < by1 && y < HELD_GRID_H; y++)
        for (int x = bx0 < 0 ? 0 : bx0; x < bx1 && x < HELD_GRID_W; x++) {
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            const float e0 = sg * ((x1 - x0) * (py - y0) - (y1 - y0) * (px - x0));
            const float e1 = sg * ((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1));
            const float e2 = sg * ((x0 - x2) * (py - y2) - (y0 - y2) * (px - x2));
            if (e0 < 0.0f || e1 < 0.0f || e2 < 0.0f) continue;
            if (set) s_held_mask[y][x] = 1;
            else if (!s_held_mask[y][x]) return 0;
        }
    return 1;
}

/** @brief Drop every held entry that the entries after it paint over completely, keeping the order of the rest.
 *
 * The list is walked newest first against a coverage grid of the opaque
 * entries already walked, so an entry is dropped only when everything drawn
 * after it, in its own tick or a later one, covers each pixel it covers. The
 * tick's cover primitives (openpete_native_scene_note_cover_quad) join the
 * grid between the tick's entries and the earlier ticks' ones.
 * @param L Held list.
 * @param old_n Entries of earlier ticks at the front of the list.
 * @return How many of those earlier entries survive; they stay at the front. */
static int held_prune(held_list_t* L, int old_n) {
    memset(s_held_mask, 0, sizeof s_held_mask);
    int keep_old = 0, w = L->n;
    for (int i = L->n - 1; i >= 0; i--) {
        if (i == old_n - 1)   /* the tick's cover primitives lie under its own entries */
            for (int c = 0; c < s_held_cover_n; c++) held_raster(&s_held_cover[c], 1);
        const scene_tri_t q = L->t[i];
        if (held_raster(&q, 0)) continue;      /* painted over by what follows */
        if (held_opaque(&q)) held_raster(&q, 1);
        L->t[--w] = q;
        if (i < old_n) keep_old++;
    }
    memmove(L->t, L->t + w, (size_t)(L->n - w) * sizeof *L->t);
    L->n -= w;
    return keep_old;
}

/** @brief Append a tick's 2D entries to a held list, oldest dropped when the list is full.
 *
 * Each entry is stored as a composed copy (carry 2, which no capture
 * re-accumulates) with its traversal rank cleared, so the scene's 2D order
 * sort never interleaves it with the tick that replays it.
 * @param L Held list.
 * @param tick The tick's 2D entries.
 * @param n Entry count. */
static void held_append(held_list_t* L, const scene_tri_t* tick, int n) {
    if (n > HELD_MAX) { tick += n - HELD_MAX; n = HELD_MAX; }
    if (L->n + n > HELD_MAX) {
        const int drop = L->n + n - HELD_MAX;
        memmove(L->t, L->t + drop, (size_t)(L->n - drop) * sizeof *L->t);
        L->n -= drop;
    }
    if (L->n + n > L->cap) {
        int cap = L->cap ? L->cap : 256;
        while (cap < L->n + n) cap *= 2;
        scene_tri_t* nt = (scene_tri_t*)realloc(L->t, (size_t)cap * sizeof *nt);
        if (!nt) return;
        L->t = nt; L->cap = cap;
    }
    for (int t = 0; t < n; t++) {
        scene_tri_t q = tick[t];
        q.carry = 2; q.seq = 0;
        L->t[L->n++] = q;
    }
}

/** @brief Insert triangles into the live scene in front of its first 2D triangle.
 *
 * @param src Triangles.
 * @param n Count. */
static void scene_insert_before_2d(const scene_tri_t* src, int n) {
    if (n <= 0 || !scene_room(s_scene_n + n)) return;
    int at = 0;
    while (at < s_scene_n && !(s_scene[at].category == OPENPETE_SCENE_HUD ||
                               s_scene[at].category == OPENPETE_SCENE_HUD_PANEL ||
                               s_scene[at].category == OPENPETE_SCENE_OVERLAY ||
                               s_scene[at].carry)) at++;
    memmove(&s_scene[at + n], &s_scene[at], (size_t)(s_scene_n - at) * sizeof(scene_tri_t));
    memcpy(&s_scene[at], src, (size_t)n * sizeof(scene_tri_t));
    s_scene_n += n;
}

static int s_front_mark = -1;   /* openpete_native_scene_front_begin's count, -1 = none */

void openpete_native_scene_front_begin(void) { s_front_mark = s_scene_n; }

void openpete_native_scene_front_end(void) {
    static mod_fvec tail;   /* scene_tri_t */
    const int first = s_front_mark;
    s_front_mark = -1;
    if (first < 0 || first >= s_scene_n) return;
    const int k = s_scene_n - first;
    if (scene_run_fit(&tail, "scene.front", k) < k) return;
    memcpy(tail.data, &s_scene[first], (size_t)k * sizeof(scene_tri_t));
    s_scene_n = first;
    scene_insert_before_2d((const scene_tri_t*)tail.data, k);
}

/** @brief Roll the retained area's held list at a canonical capture and replay it into the canonical scene.
 *
 * With the area being drawn retained, its list is reset on a new content
 * epoch or when the tick renders the world, and extended by the tick's entries; entries painted over by what
 * follows them are dropped (held_prune); the surviving entries of earlier
 * ticks are replayed in front of the scene's 2D. The replay count is kept for the tick's re-extracts
 * (openpete_native_scene_apply_overlay). OPENPETE_FBSNAP_CENSUS prints the
 * list per capture. */
static void held_capture(void) {
    unsigned ep = 0;
    const int a = openpete_fb_snapshot_retained_area(&ep);
    if (a < 0) s_held_cover_n = 0;
    s_held_area = a;
    s_held_replay = 0;
    if (a < 0) return;
    held_list_t* L = &s_held[a];
    if (!L->valid || L->epoch != ep) { L->n = 0; L->epoch = ep; L->valid = 1; }
    /* A tick that renders the world repaints the buffer under its 2D (the
     * world's sky and terrain cover the screen), so nothing earlier shows. */
    if (scene_accum_has_world()) L->n = 0;
    const int before = L->n;
    held_append(L, s_overlay_carry, s_overlay_carry_n);
    const int added = s_overlay_carry_n < HELD_MAX ? s_overlay_carry_n : HELD_MAX;
    const int old_n = L->n - added < before ? L->n - added : before;
    s_held_replay = held_prune(L, old_n > 0 ? old_n : 0);
    s_held_cover_n = 0;
    scene_insert_before_2d(L->t, s_held_replay);
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_FBSNAP_CENSUS"); on = (e && *e && *e != '0'); }
    if (on) fprintf(stderr, "[fbsnap] held f=%u area=%d epoch=%u replayed=%d held=%d\n",
                    s_scene_frame, a, ep, s_held_replay, L->n);
}

/** @brief Keep the 2D carry for a canonical present that shows the scene the previous capture took.
 *
 * The previous set becomes the current one and the current set stays, so
 * the sub-tick lerp holds every element where the scene draws it. The
 * capture timestamps stay those of the capture that took the scene: the
 * next fresh capture then interpolates from it over one tick of its own
 * cadence. The retained area's held list and replay count stay as that
 * capture left them, since the buffer holds the same content. */
static void overlay_carry_hold(void) {
    if (s_overlay_carry_n > 0)
        memcpy(s_overlay_prev, s_overlay_carry, (size_t)s_overlay_carry_n * sizeof(s_overlay_carry[0]));
    s_overlay_prev_n = s_overlay_carry_n;
}

int openpete_native_scene_canon_repeat(void) {
    if (s_canon_repeat < 0) {
        /* The 2D feeder's deferred primitive belongs to this present's tick:
         * flush it first, as the capture does. */
        if (s_overlay_precapture) s_overlay_precapture();
        s_canon_repeat = !s_game_fed && s_overlay_accum_n == 0;
    }
    return s_canon_repeat;
}

void openpete_native_scene_capture_overlay(void) {
    /* Let the 2D-UI feeder flush its deferred ("feed-previous") prim into this
     * tick's accumulation first - by the canonical present every post-link prim
     * mutation is done, so the flush is exact. Without it, a gameplay frame
     * whose LAST 2D prim is an FT4 (the collected-egg sprite is the ONLY 2D
     * prim then) defers that sprite a whole tick and misses this capture. The
     * repeat verdict runs that flush. */
    const int repeat = openpete_native_scene_canon_repeat();
    s_canon_repeat = -1;
    s_game_fed = 0;
    /* A repeat present shows the scene the previous capture took: the carry
     * stays that scene's 2D, standing still. */
    if (repeat) {
        overlay_carry_hold();
        openpete_fb_snapshot_note_scene_complete();
        return;
    }
    s_game_scene_pending = 0;
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
     * copies bypass add_tri entirely - so the accumulation never doubles, and a
     * sub-tick scene wipe between the feed and this capture can't lose a sprite. */
    memcpy(s_overlay_carry, s_overlay_accum, (size_t)s_overlay_accum_n * sizeof(s_overlay_accum[0]));
    memcpy(s_overlay_carry_epoch, s_overlay_accum_epoch,
           (size_t)s_overlay_accum_n * sizeof(s_overlay_accum_epoch[0]));
    s_overlay_carry_n = s_overlay_accum_n;
    s_overlay_accum_n = 0;
    /* The two 2D emitters reach the accumulation at different points of the
     * frame, so feed order is not the game's emission order; restore it. */
    seq_sort_carry(s_overlay_carry, s_overlay_carry_epoch, s_overlay_carry_n);
    /* Compose the carry into the scene this present is about to draw, when part
     * of it was fed into a scene that has since been replaced. A game whose 2D
     * layer emits on alternate VBLs (Spyro 3's menu panel and its glyphs) puts
     * that emission in one scene and rebuilds the scene before the canonical
     * present, so the canonical scene holds no 2D at all while the carry holds
     * the whole set; only the sub-tick re-extract's apply_overlay put it back,
     * leaving the canonical present and the sub-tick presents showing different
     * 2D layers. Drop the live scene's own carry-eligible tris and append the
     * carry in feed order, which is the set and the order apply_overlay
     * composes, so the two legs agree.
     *
     * A game that emits its 2D into the scene being presented (Spyro 1) has
     * every carry entry stamped with the live epoch, `stale` stays 0, and the
     * block is skipped: the scene keeps its own tris, in their own order,
     * untouched. Skipped too when the scene already holds re-applied carry
     * (carry == 2) - apply_overlay has composed this set into it already. */
    {
        int stale = 0, mismatch = 0;
        for (int t = 0; t < s_overlay_carry_n; t++)
            if (s_overlay_carry_epoch[t] != s_scene_epoch) { stale = 1; mismatch++; }
        int reapplied = 0;
        for (int t = 0; t < s_scene_n && !reapplied; t++)
            if (s_scene[t].carry == 2) reapplied = 1;
        /* OPENPETE_CARRY_EPOCH_PROBE=1: one line per canonical capture, zeros
         * included, naming how many carry entries carry an epoch other than the
         * scene's and whether that swapped the scene's carry-eligible tris for
         * the carry set. A feeder that emits at a different point of the frame
         * than the rest of the 2D changes those numbers. */
        { static int pz = -1;
          if (pz < 0) { const char* e = getenv("OPENPETE_CARRY_EPOCH_PROBE");
                        pz = (e && *e && *e != '0'); }
          if (pz) {
            int elig = 0;
            for (int t = 0; t < s_scene_n; t++) {
                const scene_tri_t *q = &s_scene[t];
                if ((q->category == OPENPETE_SCENE_OVERLAY ||
                     q->category == OPENPETE_SCENE_HUD_PANEL ||
                     q->carry == 1) && q->carry != 2) elig++;
            }
            fprintf(stderr, "[carry f=%u] epoch=%u carry_n=%d mismatch=%d stale=%d"
                            " reapplied=%d scene_n=%d eligible=%d swap=%d\n",
                    s_scene_frame, s_scene_epoch, s_overlay_carry_n, mismatch, stale,
                    reapplied, s_scene_n, elig, (stale && !reapplied) ? 1 : 0); } }
        if (stale && !reapplied) {
            int w = 0;
            for (int t = 0; t < s_scene_n; t++) {
                const scene_tri_t *q = &s_scene[t];
                int eligible = (q->category == OPENPETE_SCENE_OVERLAY ||
                                q->category == OPENPETE_SCENE_HUD_PANEL ||
                                q->carry == 1) && q->carry != 2;
                if (!eligible) { if (w != t) s_scene[w] = s_scene[t]; w++; }
            }
            s_scene_n = w;
            (void)scene_room(s_scene_n + s_overlay_carry_n);
            for (int t = 0; t < s_overlay_carry_n && s_scene_n < (int)s_scene_v.cap; t++) {
                scene_tri_t tri = s_overlay_carry[t];
                tri.carry = 2;                   /* composed copy: never re-accumulates */
                s_scene[s_scene_n++] = tri;
            }
            static int s_c2ddbg = -1;
            if (s_c2ddbg < 0) { const char* e = getenv("OPENPETE_ALPHA_MAP_DBG");
                                s_c2ddbg = (e && *e && *e != '0'); }
            if (s_c2ddbg)
                fprintf(stderr, "[canon-2d f=%u] composed=%d scene_n=%d\n",
                        s_scene_frame, s_overlay_carry_n, s_scene_n);
        }
    }
    seq_sort_scene_2d(s_scene, s_scene_n);   /* both paths end in emission order */
    held_capture();
    { extern int scene_2d_count_fwd(const scene_tri_t* sc, int n);
      static int pz = -1;
      if (pz < 0) { const char* e = getenv("OPENPETE_FREEZE_LEG_PROBE"); pz = (e && *e && *e != '0'); }
      if (pz) fprintf(stderr, "[canonscene f=%u] epoch=%u scene_n=%d twod=%d\n",
                      s_scene_frame, s_scene_epoch, s_scene_n, scene_2d_count_fwd(s_scene, s_scene_n)); }
    /* Mirror the scene-cap warn-once: a busy title/menu that overruns the carry
     * buffer silently loses 2D-UI sprites sub-tick with no signal - warn so the
     * cap can be raised rather than leaving a view-dependent dropout in the wild. */
    if (s_overlay_accum_dropped > 0) {
        static int s_ov_warned = 0;
        if (!s_ov_warned) { s_ov_warned = 1;
            fprintf(stderr, "native: WARNING OVERLAY_CARRY_MAX %d exceeded (frame=%u, %d 2D-UI carry "
                    "tris dropped) - sprites will vanish sub-tick; raise OVERLAY_CARRY_MAX\n",
                    OVERLAY_CARRY_MAX, s_scene_frame, s_overlay_accum_dropped);
        }
        s_overlay_accum_dropped = 0;
    }
    /* NOTE: the freeze backdrop carry is committed in commit_fade (the per-tick
     * overlay-signal commit), NOT here - capturing it from s_scene_freeze created a
     * feedback loop (apply_overlay re-set s_scene_freeze from the carry, so capture
     * read it straight back and the freeze never died after the menu closed -> the
     * world stayed hidden on unpause). See s_game_freeze_seen. */
    openpete_fb_snapshot_note_scene_complete();   /* the tick's 2D is in: the scene is complete */
}

/* --- pairing the 2D carry across two canonical captures --------------------
 * The sub-tick position lerp must slide one element from where it stood last
 * tick to where it stands this tick, so each entry of the current capture has
 * to be matched with the entry of the previous capture that is the same
 * element. Matching by array index under equal counts is not that: a primitive
 * appearing anywhere in the list shifts every later entry, and a shifted pair
 * of two identical sprites (a dialogue box's top and bottom corner quads are
 * the same 12x8 quad 58 px apart) passes a shape test and lerps one corner
 * onto the other for the length of the tick. Matching by the primitive's
 * address is the same fault at byte scale: the game allocates its 2D
 * primitives from a per-tick arena in emission order at a fixed per-type
 * stride, so one primitive inserted earlier in the run shifts every later
 * address by a whole stride and the key space re-aligns onto neighbours.
 *
 * A 2D body moves as one. A line of text and the tiles of a dialogue panel are
 * emitted consecutively and translate together, so the unit of identity is the
 * run: a maximal contiguous span of carried entries, in feed order, sharing a
 * draw layer, texturing and CLUT. A run pairs with the previous
 * run holding the same multiset of content keys - the same glyphs and tiles, in
 * any arrangement - nearest by centroid inside the move bound, and its entries
 * pair by ordinal. That pairing is admitted only when it is a rigid
 * translation: every entry's every vertex displaces by the run's centroid
 * displacement within half a pixel. A run that scrolls displaces all its glyphs
 * alike and glides; a run that re-flows (a page of text relaid, a word moving
 * past its neighbour) displaces them by different amounts, fails the test, and
 * snaps whole. The pairing is all or nothing: interpolating part of a body and
 * snapping the rest tears the body.
 *
 * Identity for a single entry is its content: draw layer, texture page and
 * CLUT, colour depth, blend mode, the three vertices' texture coordinates, and
 * the screen width and height quantized to the pixel. A content key only names
 * an element when it names one element, so an entry that is a whole run on its
 * own pairs with the previous capture's sole holder of its key inside the move
 * bound, and a key held by two or more entries on either side snaps every one
 * of them: whichever a nearest rule picks is a guess. An entry with no partner
 * snaps to its current position - a sprite that appeared this tick, or one
 * whose content changed, is discrete and has nothing to slide from.
 *
 * Between the two, position. An entry standing exactly where an entry of its
 * own layer stood last tick is that slot's occupant and is paired with it
 * whatever its content, a pairing that moves nothing: a screen slot whose
 * content changed (a countdown timer's digit, a glyph swapped in a re-laid-out
 * line of text) is a discrete change, and pairing it by content instead would
 * find the same glyph in the slot it came from and sweep it across the line.
 *
 * The three passes run rigid run, then occupancy, then single entry. The run
 * pass leads because a body translating by exactly one of its own tile periods
 * would otherwise have every tile claimed by the neighbour standing where it
 * lands and read as static; the rigid test over the whole body is the evidence
 * that separates that slide from a body that did not move, and an entry a run
 * did not take still reaches the occupancy pass. The converse case is a body
 * whose content shifted by one period while it stood still or moved less (a
 * row of identical sprites animated out of phase): the previous run of its
 * shape nearest by one rigid translation is its body, and it takes precedence
 * over a farther content partner (carry_run_pick).
 *
 * The content key of a tri fed with no primitive address (the fade overlay,
 * the letterbox bars) is its ordinal among the other address-less tris: that
 * set does not change composition, and its members are untextured full-width
 * bars whose content is identical.
 *
 * Two carve-outs, both visible in the census as a snap rather than a sweep:
 * a single-triangle element whose texture frame advances every tick while it
 * translates changes content key each tick and has no run to carry it, so it
 * snaps (a multi-triangle body carries by its geometric body); and a body
 * whose members displace by different amounts - a re-flow, or a scroll that
 * reveals one more glyph - snaps whole rather than tearing.
 */
#define CARRY_KEY_SLOTS 2048            /* power of two > OVERLAY_CARRY_MAX */
#define CARRY_MOVE_MAX  64.0f           /* per-tick move bound, px (billboard interp's) */
#define CARRY_RIGID_TOL 0.5f            /* a run entry's agreement with the run's translation, px */
/* Open-address table for the per-key multiplicities; power of two, loaded at
 * most half by one capture. */
#define CARRY_COUNT_SLOTS 4096

typedef struct {
    uint32_t key[OVERLAY_CARRY_MAX];    /* per prev entry */
    int32_t  next[OVERLAY_CARRY_MAX];   /* chain within a bucket, -1 end */
    int32_t  head[CARRY_KEY_SLOTS];
    /* Same entries bucketed by the integer cell of their first vertex and their
     * layer, for the occupancy pass. */
    int32_t  pnext[OVERLAY_CARRY_MAX];
    int32_t  phead[CARRY_KEY_SLOTS];
} carry_key_map_t;

/** @brief Fold one 32-bit word into a 32-bit FNV-1a hash state.
 *
 * The word is xored in whole, then the state is multiplied by the FNV prime.
 * @param h Hash state.
 * @param w Word to fold.
 * @return The new hash state. */
static uint32_t carry_hash(uint32_t h, uint32_t w) { h ^= w; return h * 16777619u; }

/** @brief Round a float to the nearest pixel as a hash word.
 *
 * @param f Value in pixels.
 * @return lrintf(f) as a uint32_t. */
static uint32_t carry_qpx(float f) { return (uint32_t)(int32_t)lrintf(f); }

/** @brief Return the content key of a carried 2D entry.
 *
 * The key is an FNV-1a hash of the draw layer, texturing, blend mode, colour
 * depth, texture page, CLUT, the three vertices' texture coordinates, and the
 * screen box's width and height quantized to the pixel, with the top bit
 * cleared. An address-less entry (prim NULL) is keyed by its ordinal among
 * the address-less entries with the top bit set, so the two key spaces never
 * meet. A hash collision merges two content groups; a merged key is held more
 * than once, so the single-entry pass (carry_pair_content) pairs none of its
 * holders.
 * @param tri Entry to key.
 * @param zero_ord Running ordinal of the address-less entries; incremented
 *        for each one keyed.
 * @return The content key. */
static uint32_t carry_key_of(const scene_tri_t* tri, int* zero_ord) {
    if (!tri->prim) return (1u << 31) | (uint32_t)(*zero_ord)++;
    uint32_t h = 2166136261u;
    h = carry_hash(h, (uint32_t)tri->category);
    h = carry_hash(h, (uint32_t)tri->textured);
    h = carry_hash(h, (uint32_t)tri->blend);
    h = carry_hash(h, (uint32_t)tri->bpp);
    h = carry_hash(h, (uint32_t)(tri->tpage_x & 0xFFFF) | ((uint32_t)tri->tpage_y << 16));
    h = carry_hash(h, (uint32_t)(tri->clut_x & 0xFFFF) | ((uint32_t)tri->clut_y << 16));
    float x0 = tri->v[0].sx, x1 = x0, y0 = tri->v[0].sy, y1 = y0;
    for (int k = 0; k < 3; k++) {
        h = carry_hash(h, carry_qpx(tri->v[k].u));
        h = carry_hash(h, carry_qpx(tri->v[k].v));
        if (tri->v[k].sx < x0) x0 = tri->v[k].sx;
        if (tri->v[k].sx > x1) x1 = tri->v[k].sx;
        if (tri->v[k].sy < y0) y0 = tri->v[k].sy;
        if (tri->v[k].sy > y1) y1 = tri->v[k].sy;
    }
    h = carry_hash(h, carry_qpx(x1 - x0));
    h = carry_hash(h, carry_qpx(y1 - y0));
    return h & 0x7FFFFFFFu;
}

/** @brief Return the occupancy bucket of a screen cell on one layer.
 *
 * Entries within 0.5 px of each other can fall in adjacent cells, so a
 * lookup (carry_slot_take) probes the 3x3 block around the cell.
 * @param cx Cell x, the floor of the first vertex's screen x plus any probe offset.
 * @param cy Cell y, the floor of the first vertex's screen y plus any probe offset.
 * @param cat Draw layer.
 * @param textured Nonzero for a textured entry.
 * @return A bucket index below CARRY_KEY_SLOTS. */
static uint32_t carry_pos_slot(int cx, int cy, int cat, int textured) {
    uint32_t h = 2166136261u;
    h = carry_hash(h, (uint32_t)cx);
    h = carry_hash(h, (uint32_t)cy);
    h = carry_hash(h, (uint32_t)cat);
    h = carry_hash(h, (uint32_t)textured);
    return h & (CARRY_KEY_SLOTS - 1);
}

/** @brief Return the occupancy bucket of an entry's first vertex, offset by whole cells.
 *
 * The cell is the floor of the first vertex's screen position plus the
 * offset; the layer and texturing come from the entry.
 * @param tri Entry.
 * @param dx Cell offset x, -1 to 1 for a 3x3 probe.
 * @param dy Cell offset y.
 * @return A bucket index below CARRY_KEY_SLOTS. */
static uint32_t carry_pos_slot_of(const scene_tri_t* tri, int dx, int dy) {
    return carry_pos_slot((int)floorf(tri->v[0].sx) + dx, (int)floorf(tri->v[0].sy) + dy,
                          tri->category, tri->textured);
}

/** @brief Compute the content keys of one capture in feed order.
 *
 * @param tris Capture entries.
 * @param n Entry count.
 * @param key Receives carry_key_of of each entry; the address-less entries
 *        are numbered from 0. */
static void carry_keys_build(const scene_tri_t* tris, int n, uint32_t* key) {
    int zero_ord = 0;
    for (int t = 0; t < n; t++) key[t] = carry_key_of(&tris[t], &zero_ord);
}

/** @brief Build the key and position chains of the previous capture.
 *
 * Computes the content key of each entry (carry_keys_build), then chains the
 * entries into m->head by key bucket and into m->phead by position bucket
 * (carry_pos_slot_of at zero offset). Each chain is built in reverse, so a
 * walk visits entries in feed order.
 * @param m Map to fill; every bucket is reset first.
 * @param prev Previous capture, at most OVERLAY_CARRY_MAX entries.
 * @param prev_n Entry count. */
static void carry_map_build(carry_key_map_t* m, const scene_tri_t* prev, int prev_n) {
    for (int i = 0; i < CARRY_KEY_SLOTS; i++) { m->head[i] = -1; m->phead[i] = -1; }
    carry_keys_build(prev, prev_n, m->key);
    /* Chain each bucket in reverse so a walk visits entries in feed order. */
    for (int t = prev_n - 1; t >= 0; t--) {
        uint32_t b = m->key[t] & (CARRY_KEY_SLOTS - 1);
        m->next[t] = m->head[b];
        m->head[b] = t;
        uint32_t p = carry_pos_slot_of(&prev[t], 0, 0);
        m->pnext[t] = m->phead[p];
        m->phead[p] = t;
    }
}

/** @brief Return the largest distance between corresponding vertices of two triangles in screen space.
 *
 * Vertex k of @p a is compared with vertex k of @p b.
 * @param a First triangle.
 * @param b Second triangle.
 * @return The largest per-vertex distance in PSX screen pixels. */
static float tri_pos_dist(const scene_tri_t* a, const scene_tri_t* b) {
    float w = 0.0f;
    for (int k = 0; k < 3; k++) {
        float dx = a->v[k].sx - b->v[k].sx, dy = a->v[k].sy - b->v[k].sy;
        float d = sqrtf(dx * dx + dy * dy);
        if (d > w) w = d;
    }
    return w;
}

/** @brief Count, for each entry of a capture, the entries holding its content key.
 *
 * The count uses an open-address table of CARRY_COUNT_SLOTS static slots
 * with linear probing, so the function is not reentrant.
 * @param key Content keys, one per entry.
 * @param n Entry count.
 * @param out Receives, at index i, the number of entries whose key equals key[i]. */
static void carry_key_counts(const uint32_t* key, int n, uint16_t* out) {
    static uint32_t slot_key[CARRY_COUNT_SLOTS];
    static uint16_t slot_n[CARRY_COUNT_SLOTS];
    static int8_t   slot_used[CARRY_COUNT_SLOTS];
    static int32_t  slot_of[OVERLAY_CARRY_MAX];
    memset(slot_used, 0, sizeof(slot_used));
    for (int i = 0; i < n; i++) {
        uint32_t s = key[i] & (CARRY_COUNT_SLOTS - 1);
        while (slot_used[s] && slot_key[s] != key[i]) s = (s + 1) & (CARRY_COUNT_SLOTS - 1);
        if (!slot_used[s]) { slot_used[s] = 1; slot_key[s] = key[i]; slot_n[s] = 0; }
        slot_n[s]++;
        slot_of[i] = (int32_t)s;
    }
    for (int i = 0; i < n; i++) out[i] = slot_n[slot_of[i]];
}

/* The match one present's three passes build. `match[t]` is the previous-capture
 * index paired to current entry `t`, or -1; `ckey[t]` its content key;
 * `occupied[t]` whether an entry of its layer stood on its slot; `anchored[t]`
 * whether the pair came from the rigid-run or the occupancy pass, both of which
 * name the partner by evidence the capture holds rather than by content alone;
 * `ambig[t]` whether the entry's content key is held by two or more entries in
 * either capture. */
typedef struct {
    int32_t  match[OVERLAY_CARRY_MAX];
    uint32_t ckey[OVERLAY_CARRY_MAX];
    int8_t   occupied[OVERLAY_CARRY_MAX];
    int8_t   prev_taken[OVERLAY_CARRY_MAX];
    int8_t   anchored[OVERLAY_CARRY_MAX];
    int8_t   ambig[OVERLAY_CARRY_MAX];
} carry_match_t;

/* The runs of one capture. `of[t]` is entry t's run; `lo` and `len` the run's
 * span in feed order; `sig` an order-independent hash of its entries' content
 * keys, so two runs sign alike exactly when they hold the same multiset;
 * `cx`/`cy` the mean of its entries' vertex positions; `taken` whether a
 * current run has already claimed this one. */
typedef struct {
    int32_t  lo[OVERLAY_CARRY_MAX];
    int32_t  len[OVERLAY_CARRY_MAX];
    uint32_t sig[OVERLAY_CARRY_MAX];
    float    cx[OVERLAY_CARRY_MAX], cy[OVERLAY_CARRY_MAX];
    int32_t  of[OVERLAY_CARRY_MAX];
    int8_t   taken[OVERLAY_CARRY_MAX];
    int      n;
} carry_runs_t;

/* Per-present run tallies for the census. */
typedef struct { int runs, lerped, disagree, segmented, segs; } carry_run_stats_t;

/** @brief Return whether two adjacent entries belong to one run.
 *
 * They join when they draw on the same layer, with the same texturing,
 * through the same CLUT. The texture page is not compared: a font's glyphs
 * are spread over several pages, so one line of text steps through three or
 * four of them, and keying on the page would cut the line into single
 * glyphs. The page is part of each entry's content key, so it still
 * separates the run's members.
 * @param a Earlier entry in feed order.
 * @param b The entry after it.
 * @return 1 when they join, else 0. */
static int carry_run_joins(const scene_tri_t* a, const scene_tri_t* b) {
    return a->category == b->category && a->textured == b->textured &&
           a->clut_x == b->clut_x && a->clut_y == b->clut_y;
}

/** @brief Partition a capture into runs of adjacent joinable entries and total each run.
 *
 * A new run opens at entry 0 and wherever carry_run_joins refuses the pair
 * of adjacent entries. Each run records its first entry, length, order-
 * independent content signature (the sum of the entries' hashed keys) and
 * the mean of all its vertex positions.
 * @param tris Capture entries, at most OVERLAY_CARRY_MAX.
 * @param key Content keys of the entries.
 * @param n Entry count.
 * @param r Receives the runs. */
static void carry_runs_build(const scene_tri_t* tris, const uint32_t* key, int n,
                             carry_runs_t* r) {
    r->n = 0;
    for (int t = 0; t < n; t++) {
        if (t == 0 || !carry_run_joins(&tris[t - 1], &tris[t])) {
            int s = r->n++;
            r->lo[s] = t; r->len[s] = 0; r->sig[s] = 0u;
            r->cx[s] = 0.0f; r->cy[s] = 0.0f; r->taken[s] = 0;
        }
        int s = r->n - 1;
        r->of[t] = s;
        r->len[s]++;
        r->sig[s] += carry_hash(2166136261u, key[t]);   /* summed: order-independent */
        for (int k = 0; k < 3; k++) {
            r->cx[s] += tris[t].v[k].sx;
            r->cy[s] += tris[t].v[k].sy;
        }
    }
    for (int s = 0; s < r->n; s++) {
        float m = 3.0f * (float)r->len[s];
        r->cx[s] /= m; r->cy[s] /= m;
    }
}

/** @brief Find the content partner of a current run: the nearest previous run of its length and content multiset.
 *
 * The partner's centroid lies inside CARRY_MOVE_MAX of the current run's. A
 * nearest run that another current run has already claimed yields -1 rather
 * than the next one out: a body emitted twice in one capture would otherwise
 * send its second copy to a partner elsewhere and sweep it across the screen,
 * while the first copy already draws where it belongs.
 * @param pr Runs of the previous capture.
 * @param cr Runs of the current capture.
 * @param ci Index of the current run.
 * @return The previous run's index, or -1. */
static int carry_run_partner(const carry_runs_t* pr, const carry_runs_t* cr, int ci) {
    int best = -1;
    float bd = CARRY_MOVE_MAX;
    for (int q = 0; q < pr->n; q++) {
        if (pr->len[q] != cr->len[ci] || pr->sig[q] != cr->sig[ci]) continue;
        float dx = cr->cx[ci] - pr->cx[q], dy = cr->cy[ci] - pr->cy[q];
        float d = sqrtf(dx * dx + dy * dy);
        if (d < bd) { bd = d; best = q; }
    }
    return (best >= 0 && pr->taken[best]) ? -1 : best;
}

/** @brief Return whether one translation carries every vertex of a previous span onto its ordinal partner in a current span.
 *
 * Each vertex must land within CARRY_RIGID_TOL of its partner.
 * @param prev Previous capture.
 * @param plo First entry of the previous span.
 * @param curr Current capture.
 * @param clo First entry of the current span.
 * @param len Entries in each span.
 * @param dx Translation x, px.
 * @param dy Translation y, px.
 * @return 1 when every vertex agrees, else 0. */
static int carry_run_rigid(const scene_tri_t* prev, int plo,
                           const scene_tri_t* curr, int clo, int len,
                           float dx, float dy) {
    for (int k = 0; k < len; k++)
        for (int j = 0; j < 3; j++) {
            float ex = curr[clo + k].v[j].sx - prev[plo + k].v[j].sx - dx;
            float ey = curr[clo + k].v[j].sy - prev[plo + k].v[j].sy - dy;
            if (ex * ex + ey * ey > CARRY_RIGID_TOL * CARRY_RIGID_TOL) return 0;
        }
    return 1;
}

/** @brief Compute the mean of an entry's three vertex displacements from its ordinal partner.
 *
 * @param p Previous entry.
 * @param c Current entry.
 * @param dx Receives the mean x displacement, c minus p.
 * @param dy Receives the mean y displacement, c minus p. */
static void carry_entry_delta(const scene_tri_t* p, const scene_tri_t* c,
                              float* dx, float* dy) {
    float sx = 0.0f, sy = 0.0f;
    for (int j = 0; j < 3; j++) {
        sx += c->v[j].sx - p->v[j].sx;
        sy += c->v[j].sy - p->v[j].sy;
    }
    *dx = sx / 3.0f; *dy = sy / 3.0f;
}

/** @brief Pair an ordinal-paired run that no single translation carries, span by span.
 *
 * The run is partitioned into maximal contiguous spans whose every vertex
 * displaces by the span's own translation within CARRY_RIGID_TOL, and each
 * span is paired whole by ordinal and marked anchored. A span's translation
 * is the mean vertex displacement of its first ordinal pair, and every later
 * member is admitted only against that same translation, so a span is one
 * displacement by construction. An entry whose own three vertices disagree
 * with their mean opens no span and joins none: it takes no pairing and
 * falls through to the occupancy and single-entry passes. A body carried by
 * one translation gives all its members the same displacement and lands in
 * one span, so two members of one contiguous body never take different
 * treatment, while a body sharing a run with a body that moves differently
 * keeps its own glide instead of both snapping.
 *
 * carry_pair_runs calls this only after the whole-run test has failed, so a
 * run carried by one translation reaches none of this code.
 * @param prev Previous capture.
 * @param plo First entry of the previous run.
 * @param curr Current capture.
 * @param clo First entry of the current run.
 * @param len Entries in each run.
 * @param mt Match under construction; match, prev_taken and anchored are written.
 * @param segs Incremented once per span carried.
 * @return The number of entries paired. */
static int carry_pair_run_segments(const scene_tri_t* prev, int plo,
                                   const scene_tri_t* curr, int clo, int len,
                                   carry_match_t* mt, int* segs) {
    int paired = 0;
    for (int k = 0; k < len; ) {
        float dx, dy;
        carry_entry_delta(&prev[plo + k], &curr[clo + k], &dx, &dy);
        if (!carry_run_rigid(prev, plo + k, curr, clo + k, 1, dx, dy)) { k++; continue; }
        int e = k + 1;
        while (e < len && carry_run_rigid(prev, plo + e, curr, clo + e, 1, dx, dy)) e++;
        for (int i = k; i < e; i++) {
            mt->match[clo + i] = plo + i;
            mt->prev_taken[plo + i] = 1;
            mt->anchored[clo + i] = 1;
        }
        paired += e - k;
        (*segs)++;
        k = e;
    }
    return paired;
}

/** @brief Find a current run's body by geometry alone: the unclaimed previous run one translation carries onto it, displaced least.
 *
 * The candidates are the unclaimed previous runs of the current run's layer,
 * texturing and length that carry onto it by one translation
 * (carry_run_rigid under their centroid displacement) inside CARRY_MOVE_MAX.
 *
 * Content alone misnames a body whose sprites animate out of phase: a row of
 * spinning eggs, each one frame behind its neighbour, hands each egg's frame to
 * its neighbour every tick, so the same-multiset partner is the neighbour one
 * egg-width away and every egg would slide one period sideways, whether the row
 * stands still or rises. The frame can live in the CLUT as well as the texture
 * coordinates, so the CLUT is no part of this match. The run of the same shape
 * nearest by a rigid translation is the row itself: a row that stands still is
 * displaced by zero, a row that rises by its rise, and both are nearer than one
 * period.
 * @param prev Previous capture.
 * @param pr Runs of the previous capture.
 * @param curr Current capture.
 * @param cr Runs of the current capture.
 * @param ci Index of the current run.
 * @param out_d Receives the chosen run's centroid displacement, or
 *        CARRY_MOVE_MAX when none qualifies.
 * @param out_unique Receives 0 when a second candidate lies within
 *        CARRY_RIGID_TOL of the chosen one's displacement, since two equally
 *        near bodies name neither; else 1 when a run was chosen.
 * @return The previous run's index, or -1. */
static int carry_run_body(const scene_tri_t* prev, const carry_runs_t* pr,
                          const scene_tri_t* curr, const carry_runs_t* cr, int ci,
                          float* out_d, int* out_unique) {
    const scene_tri_t* c0 = &curr[cr->lo[ci]];
    int best = -1;
    float bd = CARRY_MOVE_MAX, second = CARRY_MOVE_MAX;
    for (int q = 0; q < pr->n; q++) {
        const scene_tri_t* p0 = &prev[pr->lo[q]];
        if (pr->taken[q] || pr->len[q] != cr->len[ci] ||
            p0->category != c0->category || p0->textured != c0->textured) continue;
        float dx = cr->cx[ci] - pr->cx[q], dy = cr->cy[ci] - pr->cy[q];
        float d = sqrtf(dx * dx + dy * dy);
        if (d >= second) continue;
        if (!carry_run_rigid(prev, pr->lo[q], curr, cr->lo[ci], cr->len[ci], dx, dy)) continue;
        if (d < bd) { second = bd; bd = d; best = q; }
        else second = d;
    }
    *out_d = bd;
    *out_unique = best >= 0 && second - bd > CARRY_RIGID_TOL;
    return best;
}

/** @brief Return the smaller side of a current run's screen bounding box.
 *
 * @param curr Current capture.
 * @param cr Runs of the current capture.
 * @param ci Index of the run.
 * @return min(width, height) of the box over every vertex of the run, px. */
static float carry_run_extent(const scene_tri_t* curr, const carry_runs_t* cr, int ci) {
    const scene_tri_t* t0 = &curr[cr->lo[ci]];
    float x0 = t0->v[0].sx, x1 = x0, y0 = t0->v[0].sy, y1 = y0;
    for (int t = cr->lo[ci]; t < cr->lo[ci] + cr->len[ci]; t++)
        for (int k = 0; k < 3; k++) {
            x0 = fminf(x0, curr[t].v[k].sx); x1 = fmaxf(x1, curr[t].v[k].sx);
            y0 = fminf(y0, curr[t].v[k].sy); y1 = fmaxf(y1, curr[t].v[k].sy);
        }
    return fminf(x1 - x0, y1 - y0);
}

enum { CARRY_PICK_SNAP = 0, CARRY_PICK_BODY, CARRY_PICK_CONTENT };

/** @brief Decide how the run pass carries a current run: by its geometric body, by its content partner, or not at all.
 *
 * The geometric body (carry_run_body) is taken when it is unique and either
 * is the content partner or lies nearer than the run's own smaller side: two
 * sprites of one shape that close overlap, so the
 * nearer one is the sprite itself, whatever frame it shows. A body farther than
 * that with a different frame is some other sprite (an inventory egg above the
 * HUD row the moment the row appears), and does not name the run. The content
 * partner is taken only when no body, tied or not, lies as near as it: a row
 * whose layout jumps (eggs stacked two to a slot spread to one each) holds the
 * same frame one slot over at the same distance as the stacked twin, and the
 * pairing is a guess. Every other case snaps.
 * @param prev Previous capture.
 * @param pr Runs of the previous capture.
 * @param curr Current capture.
 * @param cr Runs of the current capture.
 * @param ci Index of the current run.
 * @param q The run's content partner (carry_run_partner), or -1.
 * @param out Receives the previous run for CARRY_PICK_BODY; untouched otherwise.
 * @return CARRY_PICK_BODY, CARRY_PICK_CONTENT (carry by @p q) or CARRY_PICK_SNAP. */
static int carry_run_pick(const scene_tri_t* prev, const carry_runs_t* pr,
                          const scene_tri_t* curr, const carry_runs_t* cr, int ci,
                          int q, int* out) {
    float gd, dq = CARRY_MOVE_MAX;
    int unique;
    const int g = carry_run_body(prev, pr, curr, cr, ci, &gd, &unique);
    if (q >= 0) dq = hypotf(cr->cx[ci] - pr->cx[q], cr->cy[ci] - pr->cy[q]);
    if (g >= 0 && unique && (g == q || gd <= carry_run_extent(curr, cr, ci))) {
        *out = g;
        return CARRY_PICK_BODY;
    }
    if (q < 0 || (g >= 0 && gd <= dq + CARRY_RIGID_TOL)) return CARRY_PICK_SNAP;
    return CARRY_PICK_CONTENT;
}

/** @brief Pair a current run with a previous run by ordinal, whole.
 *
 * Marks the previous run taken, and each pair's previous entry taken and
 * current entry anchored.
 * @param pr Runs of the previous capture; taken[q] is set.
 * @param q Index of the previous run.
 * @param cr Runs of the current capture.
 * @param ci Index of the current run.
 * @param mt Match under construction.
 * @return The number of entries paired, the run's length. */
static int carry_take_run(carry_runs_t* pr, int q, const carry_runs_t* cr, int ci,
                          carry_match_t* mt) {
    pr->taken[q] = 1;
    for (int k = 0; k < cr->len[ci]; k++) {
        mt->match[cr->lo[ci] + k] = pr->lo[q] + k;
        mt->prev_taken[pr->lo[q] + k] = 1;
        mt->anchored[cr->lo[ci] + k] = 1;
    }
    return cr->len[ci];
}

/** @brief Run the rigid-run pass, pairing each multi-entry current run whole with the previous run carry_run_pick names.
 *
 * The named run is the current run's geometric body, or its content partner
 * when one translation carries it. A run for which none is named takes no
 * pairing and every one of its entries falls through to the occupancy and
 * single-entry passes; a content partner whose entries displace by more than
 * one amount goes to the segment pass (carry_pair_run_segments), and only
 * the entries of it that no span carried fall through. Single-entry runs are
 * skipped.
 * @param prev Previous capture.
 * @param pr Runs of the previous capture; taken is written.
 * @param curr Current capture.
 * @param cr Runs of the current capture.
 * @param mt Match under construction.
 * @param st Receives the pass's run tallies for the census.
 * @return The number of entries paired. */
static int carry_pair_runs(const scene_tri_t* prev, carry_runs_t* pr,   /* pr->taken is written */
                           const scene_tri_t* curr, const carry_runs_t* cr,
                           carry_match_t* mt, carry_run_stats_t* st) {
    int paired = 0;
    st->runs = cr->n; st->lerped = 0; st->disagree = 0;
    st->segmented = 0; st->segs = 0;
    for (int i = 0; i < cr->n; i++) {
        int len = cr->len[i];
        if (len < 2) continue;
        int q = carry_run_partner(pr, cr, i), g = -1;
        int pick = carry_run_pick(prev, pr, curr, cr, i, q, &g);
        if (pick == CARRY_PICK_BODY) {
            paired += carry_take_run(pr, g, cr, i, mt); st->lerped++;
            continue;
        }
        if (pick == CARRY_PICK_SNAP) continue;
        float dx = cr->cx[i] - pr->cx[q], dy = cr->cy[i] - pr->cy[q];
        if (!carry_run_rigid(prev, pr->lo[q], curr, cr->lo[i], len, dx, dy)) {
            int segs = 0;
            int got = carry_pair_run_segments(prev, pr->lo[q], curr, cr->lo[i], len,
                                              mt, &segs);
            if (got == 0) { st->disagree++; continue; }
            pr->taken[q] = 1;
            paired += got; st->segmented++; st->segs += segs;
            continue;
        }
        paired += carry_take_run(pr, q, cr, i, mt); st->lerped++;
    }
    return paired;
}

/** @brief Claim the unclaimed previous entry of a current entry's layer and texturing standing on its slot.
 *
 * A previous entry stands on the slot when every vertex lies within 0.5 px
 * of the current entry's; the 3x3 block of occupancy buckets around the
 * first vertex's cell is probed. *occupied is set when the slot was occupied
 * at all, claimed or not: the same element emitted twice in one capture
 * leaves the second copy occupied and unmatched, which holds it still rather
 * than letting the content pass sweep it to a partner elsewhere.
 * @param m Previous capture's key map.
 * @param prev Previous capture.
 * @param curr Current entry.
 * @param prev_taken Per previous entry claim flags; the claimed entry's is set.
 * @param occupied Set to 1 when any previous entry stands on the slot.
 * @return The claimed previous entry's index, or -1. */
static int carry_slot_take(const carry_key_map_t* m, const scene_tri_t* prev,
                           const scene_tri_t* curr, int8_t* prev_taken,
                           int8_t* occupied) {
    for (int c = 0; c < 9; c++) {
        uint32_t b = carry_pos_slot_of(curr, c % 3 - 1, c / 3 - 1);
        for (int j = m->phead[b]; j >= 0; j = m->pnext[j]) {
            if (prev[j].category != curr->category ||
                prev[j].textured != curr->textured) continue;
            if (tri_pos_dist(&prev[j], curr) > 0.5f) continue;
            *occupied = 1;
            if (prev_taken[j]) continue;
            prev_taken[j] = 1;
            return j;
        }
    }
    return -1;
}

/** @brief Run the occupancy pass, pairing each entry no run took with the previous entry of its layer standing on its slot.
 *
 * Writes occupied for every current entry and marks each pair anchored.
 * @param m Previous capture's key map.
 * @param prev Previous capture.
 * @param curr Current capture.
 * @param curr_n Current entry count.
 * @param mt Match under construction.
 * @return The number of entries paired. */
static int carry_pair_occupancy(const carry_key_map_t* m, const scene_tri_t* prev,
                                const scene_tri_t* curr, int curr_n,
                                carry_match_t* mt) {
    int paired = 0;
    for (int t = 0; t < curr_n; t++) {
        mt->occupied[t] = 0;
        if (mt->match[t] >= 0) continue;
        mt->match[t] = carry_slot_take(m, prev, &curr[t], mt->prev_taken,
                                       &mt->occupied[t]);
        if (mt->match[t] >= 0) { mt->anchored[t] = 1; paired++; }
    }
    return paired;
}

/** @brief Run the single-entry pass, pairing each unmatched lone entry on an empty slot with the previous sole holder of its content key.
 *
 * An entry qualifies when it is a whole run on its own, stands on an empty
 * slot, and its key is held once in the current capture; its partner is the
 * previous capture's sole holder of the key, unclaimed and inside
 * CARRY_MOVE_MAX. This is where a lone 2D element that slid is matched; a
 * key held more than once on either side pairs nothing. The pass then writes
 * ambig for every current entry: its key is held more than once in the
 * current capture, or its partner's key more than once in the previous one.
 * @param m Previous capture's key map.
 * @param prev Previous capture.
 * @param prev_n Previous entry count.
 * @param curr Current capture.
 * @param curr_n Current entry count.
 * @param cr Runs of the current capture.
 * @param mt Match under construction.
 * @return The number of entries paired. */
static int carry_pair_content(const carry_key_map_t* m,
                              const scene_tri_t* prev, int prev_n,
                              const scene_tri_t* curr, int curr_n,
                              const carry_runs_t* cr, carry_match_t* mt) {
    static uint16_t pcount[OVERLAY_CARRY_MAX], ccount[OVERLAY_CARRY_MAX];
    carry_key_counts(m->key, prev_n, pcount);
    carry_key_counts(mt->ckey, curr_n, ccount);
    int paired = 0;
    for (int t = 0; t < curr_n; t++) {
        if (mt->match[t] >= 0 || mt->occupied[t]) continue;
        if (cr->len[cr->of[t]] != 1 || ccount[t] != 1) continue;
        for (int j = m->head[mt->ckey[t] & (CARRY_KEY_SLOTS - 1)]; j >= 0; j = m->next[j]) {
            if (m->key[j] != mt->ckey[t] || pcount[j] != 1) continue;
            if (mt->prev_taken[j]) break;   /* the one holder of the key is claimed */
            if (tri_pos_dist(&prev[j], &curr[t]) > CARRY_MOVE_MAX) break;
            mt->match[t] = j; mt->prev_taken[j] = 1; paired++;
            break;
        }
    }
    for (int t = 0; t < curr_n; t++)
        mt->ambig[t] = (int8_t)(ccount[t] != 1 ||
                                (mt->match[t] >= 0 && pcount[mt->match[t]] != 1));
    return paired;
}

/** @brief Return whether a carry entry belongs to a framebuffer copy: a stored snapshot image, a draw buffer's texels or a border drawn with them.
 *
 * @param t Entry.
 * @return 1 for a snapshot entry or an entry with a framebuffer cell key, else 0. */
static int carry_fb_entry(const scene_tri_t* t) {
    return t->bpp == OPENPETE_FB_SNAPSHOT_BPP || t->fb_cell != 0;
}

/** @brief Return whether two framebuffer-copy entries sample the same cell of the same image.
 *
 * Two snapshot entries agree on the snapshot index and the three vertices'
 * rect texel coordinates: the coordinates name the cell of the copied
 * image, which a shrinking or moving copy keeps from tick to tick while its
 * screen box changes size. Two entries that sample a draw buffer through the
 * VRAM path, or two borders drawn around such a copy, agree on the cell key
 * the feeder stamped (scene_tri_t::fb_cell: from the primitive's own texture
 * page and coordinates, or from a border's emission order), on the layer
 * and on the STP half; a cell's fed coordinates are adjusted to the screen
 * box and differ between ticks.
 * @param a One entry.
 * @param b The other.
 * @return 1 when both sample one cell, else 0. */
static int carry_snapshot_same_cell(const scene_tri_t* a, const scene_tri_t* b) {
    if (a->fb_cell || b->fb_cell)
        return a->fb_cell == b->fb_cell && a->category == b->category &&
               a->stp_sel == b->stp_sel;
    if (a->bpp != OPENPETE_FB_SNAPSHOT_BPP || b->bpp != OPENPETE_FB_SNAPSHOT_BPP) return 0;
    if (a->tpage_x != b->tpage_x || a->category != b->category) return 0;
    for (int k = 0; k < 3; k++)
        if (a->v[k].u != b->v[k].u || a->v[k].v != b->v[k].v) return 0;
    return 1;
}

/** @brief Run the snapshot pass, pairing each unmatched framebuffer-copy entry with the previous entry of its image cell.
 *
 * A copied image is drawn as quads whose texels stay fixed while their
 * screen box scales (the game shrinking a frozen frame into a window, or
 * growing a draw buffer's frame back to full size), so the content key,
 * which holds the box size, never matches across ticks. The cell
 * identifies it instead (carry_snapshot_same_cell);
 * the first unclaimed previous holder within CARRY_MOVE_MAX of every vertex
 * is taken, and the per-vertex lerp then scales the box with the image held.
 * The pair is anchored.
 * @param prev Previous capture.
 * @param prev_n Previous entry count.
 * @param curr Current capture.
 * @param curr_n Current entry count.
 * @param mt Match under construction.
 * @return The number of entries paired. */
static int carry_pair_snapshot(const scene_tri_t* prev, int prev_n,
                               const scene_tri_t* curr, int curr_n, carry_match_t* mt) {
    int paired = 0;
    for (int t = 0; t < curr_n; t++) {
        if (mt->match[t] >= 0 || !carry_fb_entry(&curr[t])) continue;
        for (int j = 0; j < prev_n; j++) {
            if (mt->prev_taken[j] || !carry_snapshot_same_cell(&prev[j], &curr[t])) continue;
            if (tri_pos_dist(&prev[j], &curr[t]) > CARRY_MOVE_MAX) continue;
            mt->match[t] = j; mt->prev_taken[j] = 1; mt->anchored[t] = 1; paired++;
            break;
        }
    }
    return paired;
}

/** @brief Match one capture against the previous one through the rigid-run, occupancy, single-entry and snapshot passes.
 *
 * Resets the match, computes the current content keys into mt->ckey, builds
 * both captures' runs, then runs the four passes in that order.
 * @param m Previous capture's key map, built by carry_map_build.
 * @param prev Previous capture.
 * @param prev_n Previous entry count.
 * @param curr Current capture.
 * @param curr_n Current entry count.
 * @param mt Receives the match.
 * @param st Receives the rigid-run pass's tallies.
 * @return The number of entries paired. */
static int carry_pair_all(const carry_key_map_t* m,
                          const scene_tri_t* prev, int prev_n,
                          const scene_tri_t* curr, int curr_n,
                          carry_match_t* mt, carry_run_stats_t* st) {
    static carry_runs_t s_prun, s_crun;
    for (int j = 0; j < prev_n; j++) mt->prev_taken[j] = 0;
    for (int t = 0; t < curr_n; t++) { mt->match[t] = -1; mt->anchored[t] = 0; }
    carry_keys_build(curr, curr_n, mt->ckey);
    carry_runs_build(prev, m->key, prev_n, &s_prun);
    carry_runs_build(curr, mt->ckey, curr_n, &s_crun);
    int paired = carry_pair_runs(prev, &s_prun, curr, &s_crun, mt, st);
    paired += carry_pair_occupancy(m, prev, curr, curr_n, mt);
    paired += carry_pair_content(m, prev, prev_n, curr, curr_n, &s_crun, mt);
    return paired + carry_pair_snapshot(prev, prev_n, curr, curr_n, mt);
}

/* OPENPETE_CARRY_CENSUS=1: per-present audit of the 2D carry's position lerp.
 * The pairing slides one sprite from where it was last tick to where it is this
 * tick, so a lerped tri's applied position must sit on that segment: within a
 * rasterisation tolerance of one bracket, or between two brackets that are
 * themselves close. A tri whose applied position is more than 2 px from both
 * bracket captures was interpolated between two different sprites; a pair whose
 * first-vertex UV region differs is the same fault read at the pairing instead
 * of at the result. `crossed` and `static_moved` name a mispair directly:
 * another previous entry stands where this one stands now, so that entry is the
 * real predecessor - `crossed` over any entry of the same layer and texturing,
 * `static_moved` over an entry of this one's own content key, which is the
 * element itself standing still while the pairing sweeps it. `ambiguous_lerp`
 * names the guess instead of its result: a moved pair taken from a content key
 * that two or more entries of either capture hold, with neither the rigid-run
 * nor the occupancy pass naming the partner. `nearer_twin` counts a moved
 * pair whose partner lies farther than another previous entry of the same
 * shape (carry_nearer_twin). All four count only a pair that actually moves. The run counters are the population the rigid pass sorts:
 * `runs` every run of the current capture, `runs_lerped` those carried by one
 * translation, `runs_segmented` those with a same-multiset partner that took
 * more than one translation, `segments_lerped` the spans those were carried by,
 * and `runs_snapped_disagree` those with a same-multiset partner that yielded
 * no translation at all, so every entry of them snapped. One line per present, zero counts
 * included, so a present that carried no 2D reads as no engagement rather than
 * as clean. */
typedef struct {
    int      lerped;        /* carried tris paired with a previous entry, moved or not */
    int      far_both;      /* of those, > 2 px from both brackets */
    int      uv_mismatch;   /* pairs whose first-vertex UV region differs */
    int      crossed;       /* pairs whose partner is another entry's predecessor */
    int      static_moved;  /* moved entries whose own content stood still */
    int      ambiguous;     /* moved pairs taken from a repeated content key */
    int      nearer_twin;   /* moved pairs past a nearer same-shape entry (carry_nearer_twin) */
    float    worst;         /* largest min(d_prev, d_curr) seen */
    int      worst_far, worst_uvmm, worst_cross, worst_cat;
    uint32_t worst_seq;
    float    worst_p[2], worst_c[2], worst_a[2];
} carry_census_t;

/** @brief Return whether OPENPETE_CARRY_CENSUS is set to a non-zero value.
 *
 * Read once and cached.
 * @return 1 when the carry census is on, else 0. */
static int carry_census_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_CARRY_CENSUS"); v = (e && *e && *e != '0'); }
    return v;
}

/** @brief Find a previous entry other than the partner that stands where a current entry stands now.
 *
 * Such an entry is the current entry's real predecessor, so a pair that
 * moves the current entry drags it along a segment between two different
 * sprites instead of holding it still. A genuine slide has no such twin: the
 * sprite's new position was occupied by nothing last tick. `key` selects
 * which twins count: previous entries holding content key `key` when it is
 * non-zero (the element itself stood still), else any entry of the same
 * layer and texturing (a crossing of two identities, whatever they are). An
 * entry stands there when every vertex lies within 0.5 px.
 * @param prev Previous capture.
 * @param prev_key Previous capture's content keys.
 * @param prev_n Previous entry count.
 * @param curr Current entry.
 * @param key Content key to match, or 0 to match layer and texturing.
 * @param self The current entry's partner, excluded.
 * @return The twin's index, or -1. */
static int carry_twin_of(const scene_tri_t* prev, const uint32_t* prev_key, int prev_n,
                         const scene_tri_t* curr, uint32_t key, int self) {
    for (int j = 0; j < prev_n; j++) {
        if (j == self) continue;
        if (key ? (prev_key[j] != key)
                : (prev[j].category != curr->category || prev[j].textured != curr->textured))
            continue;
        if (tri_pos_dist(&prev[j], curr) <= 0.5f) return j;
    }
    return -1;
}

/** @brief Return whether a previous entry of a current entry's shape stands nearer to it than its partner by more than 0.5 px.
 *
 * The candidate is any previous entry other than the partner, of the
 * current entry's layer and texturing and of its shape (every vertex's
 * offset from the first within 0.5 px). The nearer entry is the likelier
 * predecessor: `crossed` is the case where it stands on `curr` exactly, and a
 * body moving less than its own period (a row of identical
 * sprites rising) pairs with a neighbour one period over only past such an
 * entry.
 * @param prev Previous capture.
 * @param prev_n Previous entry count.
 * @param curr Current entry.
 * @param self The current entry's partner.
 * @return 1 when such an entry exists, else 0. */
static int carry_nearer_twin(const scene_tri_t* prev, int prev_n,
                             const scene_tri_t* curr, int self) {
    const float d0 = tri_pos_dist(&prev[self], curr);
    for (int j = 0; j < prev_n; j++) {
        if (j == self || prev[j].category != curr->category ||
            prev[j].textured != curr->textured) continue;
        int same_shape = 1;
        for (int k = 1; k < 3 && same_shape; k++) {
            float ex = (prev[j].v[k].sx - prev[j].v[0].sx) - (curr->v[k].sx - curr->v[0].sx);
            float ey = (prev[j].v[k].sy - prev[j].v[0].sy) - (curr->v[k].sy - curr->v[0].sy);
            same_shape = ex * ex + ey * ey <= 0.25f;
        }
        if (same_shape && tri_pos_dist(&prev[j], curr) + 0.5f < d0) return 1;
    }
    return 0;
}

/** @brief Add one paired carry entry to the census and track the worst pair.
 *
 * A pair is far_both when the applied triangle lies more than 2 px from both
 * brackets, uv_mismatch when both are textured and their first vertices'
 * texture coordinates differ, and static_moved when the flag is set and the
 * applied triangle is more than 0.5 px from both brackets. The worst pair is
 * the one with the largest smaller bracket distance.
 * @param cz Census being accumulated.
 * @param prev Previous entry of the pair.
 * @param curr Current entry of the pair.
 * @param app The entry's applied, interpolated triangle.
 * @param crossed Nonzero when the partner is another entry's predecessor.
 * @param static_moved Nonzero when an entry of the same content key stands
 *        where the current entry stands.
 * @param ambiguous Nonzero when the pair came from a repeated content key. */
static void carry_census_note(carry_census_t* cz, const scene_tri_t* prev,
                              const scene_tri_t* curr, const scene_tri_t* app,
                              int crossed, int static_moved, int ambiguous) {
    float dp = tri_pos_dist(app, prev), dc = tri_pos_dist(app, curr);
    float m = dp < dc ? dp : dc;
    int far_both = (dp > 2.0f && dc > 2.0f);
    int uvmm = (prev->textured && curr->textured &&
                (prev->v[0].u != curr->v[0].u || prev->v[0].v != curr->v[0].v));
    cz->lerped++;
    if (far_both) cz->far_both++;
    if (uvmm) cz->uv_mismatch++;
    if (crossed) cz->crossed++;
    if (ambiguous) cz->ambiguous++;
    /* The element stood still (a previous entry of its own content key is where
     * it now stands) yet the applied position is off both brackets: the lerp put
     * it where neither canonical tick drew it. */
    if (static_moved && dp > 0.5f && dc > 0.5f) cz->static_moved++;
    if (m > cz->worst) {
        cz->worst = m; cz->worst_far = far_both; cz->worst_uvmm = uvmm;
        cz->worst_cross = crossed;
        cz->worst_cat = curr->category; cz->worst_seq = curr->seq;
        cz->worst_p[0] = prev->v[0].sx; cz->worst_p[1] = prev->v[0].sy;
        cz->worst_c[0] = curr->v[0].sx; cz->worst_c[1] = curr->v[0].sy;
        cz->worst_a[0] = app->v[0].sx;  cz->worst_a[1] = app->v[0].sy;
    }
}

/** @brief Record one carried pair in the carry census.
 *
 * A pair that does not move is noted with no mispair flags. A moving pair is
 * noted with crossed (carry_twin_of over layer and texturing), static_moved
 * (carry_twin_of over its own content key), and ambiguous (an unanchored
 * pair from a repeated key), and nearer_twin is counted when
 * carry_nearer_twin holds.
 * @param cz Census being accumulated.
 * @param m Previous capture's key map.
 * @param mt This present's match.
 * @param t Current entry index; match[t] is its partner in s_overlay_prev.
 * @param app The entry's applied, interpolated triangle. */
static void carry_census_pair(carry_census_t* cz, const carry_key_map_t* m,
                              const carry_match_t* mt, int t, const scene_tri_t* app) {
    const int pi = mt->match[t];
    const scene_tri_t* p = &s_overlay_prev[pi];
    const scene_tri_t* c = &s_overlay_carry[t];
    if (!(tri_pos_dist(p, c) > 0.0f)) {
        carry_census_note(cz, p, c, app, 0, 0, 0);
        return;
    }
    carry_census_note(cz, p, c, app,
                      carry_twin_of(s_overlay_prev, m->key, s_overlay_prev_n, c, 0, pi) >= 0,
                      carry_twin_of(s_overlay_prev, m->key, s_overlay_prev_n, c,
                                    mt->ckey[t], pi) >= 0,
                      !mt->anchored[t] && mt->ambig[t]);
    if (carry_nearer_twin(s_overlay_prev, s_overlay_prev_n, c, pi)) cz->nearer_twin++;
}

/** @brief Print one present's carry census line on stderr.
 *
 * The line gives the present's index within its scene stamp (the counter
 * restarts when the stamp changes), the sub-tick alpha, the carried and
 * paired entry counts, the census counters and the run tallies. A present
 * with no lerped entry ends with NO-ENGAGEMENT; otherwise the line ends with
 * the worst pair's flags and its previous, current and applied positions.
 * @param cz Census accumulated over the present.
 * @param carried Carry entries this present holds.
 * @param paired Entries the pairing matched to a previous entry.
 * @param oalpha Sub-tick alpha of the overlay, 0 to 1.
 * @param st Run tallies of the present. */
static void carry_census_flush(const carry_census_t* cz, int carried, int paired,
                               double oalpha, const carry_run_stats_t* st) {
    static uint32_t s_frame = 0xFFFFFFFFu;
    static int s_ix = 0;
    if (s_scene_frame != s_frame) { s_frame = s_scene_frame; s_ix = 0; }
    fprintf(stderr, "[carrycensus vbl=%u present=%d alpha=%.3f] carried=%d paired=%d"
                    " lerped=%d far_both=%d uv_mismatch=%d crossed=%d static_moved=%d"
                    " runs=%d runs_lerped=%d runs_snapped_disagree=%d"
                    " runs_segmented=%d segments_lerped=%d ambiguous_lerp=%d nearer_twin=%d",
            s_scene_frame, s_ix++, oalpha, carried, paired,
            cz->lerped, cz->far_both, cz->uv_mismatch, cz->crossed, cz->static_moved,
            st->runs, st->lerped, st->disagree, st->segmented, st->segs,
            cz->ambiguous, cz->nearer_twin);
    if (cz->lerped == 0) { fprintf(stderr, " NO-ENGAGEMENT\n"); return; }
    fprintf(stderr, " worst=%.2f cat=%d seq=%u %s%s%sprev=(%.1f,%.1f)"
                    " curr=(%.1f,%.1f) applied=(%.1f,%.1f)\n",
            cz->worst, cz->worst_cat, cz->worst_seq,
            cz->worst_far ? "FAR-BOTH " : "", cz->worst_uvmm ? "UV-MISMATCH " : "",
            cz->worst_cross ? "CROSSED " : "",
            cz->worst_p[0], cz->worst_p[1], cz->worst_c[0], cz->worst_c[1],
            cz->worst_a[0], cz->worst_a[1]);
}

void openpete_native_scene_apply_overlay(double alpha) {
    /* The passed `alpha` is the camera-bracket alpha - 0 when the camera ring
     * dedupes (static camera). Animated 2D-UI (the logo drop-in) moves while the
     * camera is still, so derive the overlay's own alpha from its capture cadence:
     * 1-tick-lagged (now≈curr_ns => show prev, advance to curr over the next
     * capture period), exactly the camera bracket's convention but from the
     * overlay's timestamps. Falls back to the camera alpha if the timestamps
     * aren't primed yet (first frames). */
    double oalpha = alpha;
    if (openpete_interp_deterministic()) {
        /* Deterministic capture: op_now() is not reproducible run-to-run, so the
         * wall-clock cadence below would make an animating overlay (a swimming
         * text moby) flicker between captures. Derive the sub-tick phase from the
         * PINNED present clock and the tick grid instead - the overlay captures
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
     * beside the 3D bracket's line (reextract.c), so a 2D-UI slide
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
    static carry_key_map_t s_map;
    static carry_match_t   s_mt;
    carry_run_stats_t rst = {0};
    carry_map_build(&s_map, s_overlay_prev, s_overlay_prev_n);
    int paired = carry_pair_all(&s_map, s_overlay_prev, s_overlay_prev_n,
                                s_overlay_carry, s_overlay_carry_n, &s_mt, &rst);
    carry_census_t cz = {0};
    int census = carry_census_on();
    if (s_held_area >= 0)   /* the retained area's earlier ticks, under the carry */
        scene_insert_before_2d(s_held[s_held_area].t, s_held_replay);
    (void)scene_room(s_scene_n + s_overlay_carry_n);
    for (int t = 0; t < s_overlay_carry_n && s_scene_n < (int)s_scene_v.cap; t++) {
        scene_tri_t tri = s_overlay_carry[t];   /* curr; category already stamped */
        int pi = s_mt.match[t];
        if (pi >= 0) {
            const scene_tri_t *p = &s_overlay_prev[pi];
            for (int k = 0; k < 3; k++) {
                tri.v[k].sx = (float)(p->v[k].sx + (tri.v[k].sx - p->v[k].sx) * oalpha);
                tri.v[k].sy = (float)(p->v[k].sy + (tri.v[k].sy - p->v[k].sy) * oalpha);
            }
            if (census) carry_census_pair(&cz, &s_map, &s_mt, t, &tri);
        }
        tri.carry = 2;                             /* mark re-applied so the next capture skips it */
        s_scene[s_scene_n++] = tri;
    }
    if (census) carry_census_flush(&cz, s_overlay_carry_n, paired, oalpha, &rst);
    { static int pf = -2;
      if (pf == -2) { const char* e = getenv("OPENPETE_APPLY_PROBE"); pf = (e && *e) ? atoi(e) : -1; }
      if (pf >= 0 && (int)s_scene_frame >= pf - 4 && (int)s_scene_frame <= pf + 4) {
        unsigned lo = 0xFFFFFFFFu, hi = 0; int cat[16] = {0}, nz = 0;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int t = 0; t < s_overlay_carry_n; t++) {
            const scene_tri_t* q = &s_overlay_carry[t];
            if (q->seq) { if (q->seq < lo) lo = q->seq; if (q->seq > hi) hi = q->seq; } else nz++;
            if (q->category >= 0 && q->category < 16) cat[q->category]++;
            for (int k = 0; k < 3; k++) {
                if (q->v[k].sx < x0) x0 = q->v[k].sx; if (q->v[k].sx > x1) x1 = q->v[k].sx;
                if (q->v[k].sy < y0) y0 = q->v[k].sy; if (q->v[k].sy > y1) y1 = q->v[k].sy; } }
        fprintf(stderr, "[apply f=%u sort=%d] n=%d paired=%d seq=%u..%u zero=%d"
                        " bbox=[%.0f,%.0f]x[%.0f,%.0f] cats:", s_scene_frame, seq_sort_on(),
                s_overlay_carry_n, paired, lo, hi, nz, x0, x1, y0, y1);
        for (int c = 0; c < 16; c++) if (cat[c]) fprintf(stderr, " %d=%d", c, cat[c]);
        fprintf(stderr, "\n");
        if ((int)s_scene_frame == pf) for (int t = 0; t < s_overlay_carry_n; t++) {
            const scene_tri_t* q = &s_overlay_carry[t];
            fprintf(stderr, "[applytri %d] seq=%u prim=%08X cat=%d bl=%d tex=%d tp=(%d,%d) cl=(%d,%d) v=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) rgba=%08X\n",
                    t, q->seq, q->prim, q->category, q->blend, q->textured,
                    q->tpage_x, q->tpage_y, q->clut_x, q->clut_y,
                    q->v[0].sx, q->v[0].sy, q->v[1].sx, q->v[1].sy, q->v[2].sx, q->v[2].sy, q->v[0].rgba); } } }
    s_scene_freeze = freeze_lerp(s_overlay_prev_freeze, s_overlay_carry_freeze, oalpha);
    s_scene_fade   = fade_lerp(s_fade_prev, s_fade_curr, oalpha);
}

static unsigned scene_live_stamp(void);   /* defined beside the eager publish */

/* A scene and its per-scene words, as one selection of the present returns them.
 * stamp is the id its retained-terrain state is kept under (a scene id for an
 * eager scene, else its VBL); replay is set for a re-extracted scene. */
typedef struct {
    const scene_tri_t *tri; int n;
    uint32_t bg, fade, freeze;
    int hud_under, actor_front;
    unsigned stamp;
    int replay;
} scene_view_t;

/** @brief Select the scene the present draws: the eager ring slot, the kept game-render scene, a history entry or the live accumulator.
 *
 * The ring slot being composed wins under eager production; then the kept
 * game-render scene while a framebuffer-window resolve selects it; then the
 * history entry at the delay depth; then the ring slot an eager publish moved
 * the live scene to; else the live accumulator. The scene's stamp and
 * replay flag come from the same arm as its triangles, so the
 * retained-terrain selection keyed on the stamp matches what is drawn.
 * @return The selected scene and its words. */
static scene_view_t scene_present_view(void) {
    if (s_ps_slot)
        return (scene_view_t){ s_ps_slot->tri, s_ps_slot->n, s_ps_slot->bg, s_ps_slot->fade,
                               s_ps_slot->freeze, s_ps_slot->hud_under, s_ps_slot->actor_front,
                               s_ps_slot->stamp, s_ps_slot->replay };
    if (s_fbsnap_game_sel)
        return (scene_view_t){ (const scene_tri_t*)s_scene_game.v.data, s_scene_game.n,
                               s_scene_game.bg, s_scene_game.fade, s_scene_game.freeze,
                               s_scene_game.hud_under, s_scene_game.actor_front,
                               s_scene_game.frame, 0 };
    const int slot = scene_hist_slot_at(scene_delay_depth());
    if (slot >= 0)
        return (scene_view_t){ scene_hist_tris(slot), s_scene_hist_n[slot], s_scene_hist_bg[slot],
                               s_scene_hist_fade[slot], s_scene_hist_freeze[slot],
                               s_scene_hist_hud_under[slot], s_scene_hist_actor_front[slot],
                               s_scene_hist_frame[slot], s_scene_hist_replay[slot] };
    const scene_slot_t *ms = scene_moved_slot();
    if (ms)   /* an eager publish moved the live scene to this slot */
        return (scene_view_t){ ms->tri, ms->n, ms->bg, ms->fade, ms->freeze,
                               ms->hud_under, ms->actor_front, ms->stamp, ms->replay };
    return (scene_view_t){ s_scene, s_scene_n, s_scene_bg, s_scene_fade, s_scene_freeze,
                           s_scene_hud_under, s_scene_actor_front, scene_live_stamp(),
                           s_scene_replay };
}

/** @brief Return whether the scene scene_present_buf returns was built by a re-extract rather than by the game's render.
 *
 * Reads scene_present_view's selection, so the flag belongs to the scene
 * whose triangles the present draws.
 * @return Non-zero for a re-extracted scene. */
static int scene_present_replay(void) { return scene_present_view().replay; }

/** @brief Return the stamp of the scene scene_present_buf returns under the current selection.
 *
 * Reads scene_present_view's selection, so the stamp belongs to the scene
 * whose triangles the present draws, the moved eager slot included. The
 * retained-terrain selection and the freshness gate compare against it, so a
 * frozen or lagged compose keeps its retained content and a stale-state
 * compose (menus) drops it.
 * @return The scene's stamp. */
static unsigned scene_present_stamp(void) { return scene_present_view().stamp; }

/** @brief Return the scene the present should draw, with its per-scene words.
 *
 * The selection is scene_present_view's: the eager ring slot, the kept
 * game-render scene under a framebuffer-window resolve, the history entry at
 * the delay depth, the moved ring slot, else the live accumulator.
 * @param out_n Receives the triangle count.
 * @param out_bg Receives the background word.
 * @param out_fade Receives the fade word.
 * @param out_freeze Receives the freeze word; may be NULL.
 * @param out_hud_under Receives the HUD-under-player flag; may be NULL.
 * @param out_actor_front Receives the actor-front flag; may be NULL.
 * @return The scene's triangles. */
static const scene_tri_t* scene_present_buf(int* out_n, uint32_t* out_bg,
                                                   uint32_t* out_fade, uint32_t* out_freeze,
                                                   int* out_hud_under, int* out_actor_front) {
    const scene_view_t v = scene_present_view();
    *out_n = v.n; *out_bg = v.bg; *out_fade = v.fade;
    if (out_freeze) *out_freeze = v.freeze;
    if (out_hud_under) *out_hud_under = v.hud_under;
    if (out_actor_front) *out_actor_front = v.actor_front;
    return v.tri;
}


/** @brief Test whether a scene holds the game's own world geometry: WORLD, ACTOR, ACTOR_FRONT or SKY triangles.
 *
 * A gameplay frame, the kind worth snapshotting into frozen_tex as the pause
 * and inventory backdrop, carries terrain or mobys; menu and transition
 * frames (HUD and OVERLAY triangles only, or empty) do not. PLAYER alone
 * does not qualify: at pause entry the world feeds cut out one present
 * before the freeze FT4 is emitted, leaving a frame with only Spyro's body,
 * and snapshotting it freezes a near-empty image. SKY qualifies, so a frame
 * holding only the cyclorama dome counts as a gameplay frame. Host-added
 * triangles are ignored. The freeze-source test uses it, and the freeze
 * hold and the held-2D reset through scene_accum_has_world.
 * @param scene Scene triangles.
 * @param n Triangle count.
 * @return true when a qualifying triangle is present. */
static bool scene_has_world(const scene_tri_t* scene, int n) {
    for (int t = 0; t < n; t++) {
        if (scene[t].host_added) continue;   /* the game's own geometry decides */
        int c = scene[t].category;
        /* SKY counts as world: a dome-only frame is a gameplay frame. */
        if (c == OPENPETE_SCENE_WORLD || c == OPENPETE_SCENE_ACTOR ||
            c == OPENPETE_SCENE_ACTOR_FRONT || c == OPENPETE_SCENE_SKY)
            return true;
    }
    return false;
}

static uint32_t scene_accum_stamp(void);   /* defined with the build stamp */
/** @brief Test whether the scene accumulating in s_scene holds the game's own world, on either terrain route.
 *
 * scene_has_world's triangles, or a live sector in the retained terrain
 * state this scene's terrain build wrote: with the retained cache in use,
 * every sector the cache draws is skipped by the CPU walk, so a scene whose
 * view holds only cache-drawn terrain carries no WORLD triangle. The state
 * is found by the scene's build stamp, not by the present's selection,
 * since the scene has not been composed yet.
 * @return true when the scene holds world geometry. */
static bool scene_accum_has_world(void) {
    return scene_has_world(s_scene, s_scene_n) ||
           openpete_tercache_build_content(scene_accum_stamp()) > 0;
}

/** @brief Test whether the presented scene holds a triangle the game's own render produced.
 *
 * The scene-content test of the compose, the window render and the
 * freeze-source check: a scene without one composes the PSX VRAM
 * framebuffer instead, which is what an image the game drew nothing on
 * shows. Host-added geometry is excluded, so a moby the extended-LOD reveal
 * appended cannot turn such an image into a rendered scene; on the menu
 * screens that suppress the framebuffer clear, that would replace the
 * game's own output with the retained backdrop of an earlier image.
 * @param scene Scene triangles.
 * @param n Triangle count.
 * @return true when any triangle is not host-added. */
static bool scene_has_game_tris(const scene_tri_t* scene, int n) {
    for (int t = 0; t < n; t++)
        if (!scene[t].host_added) return true;
    return false;
}

/** @brief Test whether a scene carries the game's terrain as CPU triangles.
 *
 * Counts WORLD triangles that are not host-added: an appended moby is not
 * terrain. SKY does not count, since the cyclorama draws on a frame whose
 * ground feed cut out.
 * @param scene Scene triangles.
 * @param n Triangle count.
 * @return true when a qualifying WORLD triangle is present. */
static bool scene_has_terrain(const scene_tri_t* scene, int n) {
    for (int t = 0; t < n; t++)
        if (!scene[t].host_added && scene[t].category == OPENPETE_SCENE_WORLD) return true;
    return false;
}

/** @brief Test whether every triangle the game drew belongs to the 2D screen layer.
 *
 * The 2D layer is the HUD ordering table's prims (HUD, HUD_PANEL, OVERLAY)
 * and the composed carry copies of them; host-added and carry triangles are
 * skipped. Every other category is projected from world space, so one of
 * them present means the frame is a view of the level and not a screen of
 * 2D art. An empty scene passes.
 * @param scene Scene triangles.
 * @param n Triangle count.
 * @return true when no game triangle outside the 2D layer is present. */
static bool scene_is_2d_only(const scene_tri_t* scene, int n) {
    for (int t = 0; t < n; t++) {
        if (scene[t].host_added || scene[t].carry) continue;
        int c = scene[t].category;
        if (c != OPENPETE_SCENE_HUD && c != OPENPETE_SCENE_HUD_PANEL &&
            c != OPENPETE_SCENE_OVERLAY)
            return false;
    }
    return true;
}

/** @brief Return whether a composed scene qualifies as the freeze-backdrop source.
 *
 * Shared by the live compose and the capture-time snapshot so both use one
 * definition of a scene worth freezing.
 *
 * A source scene carries the game's own geometry and is not itself a freeze
 * scene (bit 31 of the freeze word clear). Past that the 3D content sorts it,
 * in three classes:
 *
 * - No 3D at all: the game composed a screen out of 2D prims alone (the atlas
 *   and options screens, whose level picture and title text are 2D quads).
 *   That screen is the frame the game means to freeze under the menu, so it
 *   qualifies.
 * - World content with terrain: an ordinary gameplay frame. Terrain reaches
 *   the present by one of two routes and either qualifies: the retained
 *   cache's selected state (frame_content() > 0, or < 0 when the retained path
 *   is not in use at all), or WORLD triangles in the scene when the terrain is
 *   batched on the CPU. Testing only the retained state rejects every present
 *   whenever the retained path is initialised but unusable - a vertex-snap
 *   raster grid, a delayed or split scene, a device without fp64 - because the
 *   feeder then stages an empty state per scene, which reads the same as a
 *   genuine per-scene skip.
 * - 3D without world: the degenerate pause-entry transition, where the terrain
 *   feed cuts out one present before the freeze prim is emitted and the frame
 *   holds the player's body or a moby over the HUD. Snapshotting it freezes a
 *   backdrop with no ground, so it is rejected and the last full gameplay
 *   frame stands.
 *
 * frame_content() reads the state selected for this present, so the caller
 * selects by stamp first.
 * @param scene   The present's scene triangle buffer.
 * @param n       Triangle count of @p scene.
 * @param sfreeze The present's freeze word; bit 31 set marks a freeze scene.
 * @return true when the scene is a freeze-backdrop source. */
static bool scene_is_freeze_source(const scene_tri_t* scene, int n, uint32_t sfreeze) {
    if (!scene_has_game_tris(scene, n) || (sfreeze & (1u << 31))) return false;
    if (scene_is_2d_only(scene, n)) return true;
    return scene_has_world(scene, n)
        && (openpete_tercache_frame_content() != 0 || scene_has_terrain(scene, n));
}

/** @brief Resolve the size of the game window.
 *
 * Shared by bring-up and by openpete_native_precreate_window so the two
 * create the window at the same size. A nonpositive dimension falls back to
 * NR_DEFAULT_W or NR_DEFAULT_H.
 * @param opts Session options; unused.
 * @param win_w Requested width in pixels.
 * @param win_h Requested height in pixels.
 * @param out_w Receives the width.
 * @param out_h Receives the height. */
static void nr_resolve_window_size(const openpete_options_t *opts,
                                   int win_w, int win_h,
                                   int *out_w, int *out_h)
{
    (void)opts;
    *out_w = (win_w > 0) ? win_w : NR_DEFAULT_W;
    *out_h = (win_h > 0) ? win_h : NR_DEFAULT_H;
}

/* --background: whether the window was created for an unattended run that
 * must not take focus. Latched at window creation and read afterwards only to
 * decide whether the state lines below are printed. */
static int s_background_requested;

/** @brief Return the creation flags of the game window and set the --background activation hints.
 *
 * The flags are SDL_WINDOW_VULKAN and SDL_WINDOW_RESIZABLE. --background
 * turns off activate-on-show and activate-on-raise, so the window maps at
 * its normal size without taking focus. It stays focusable: a not-focusable
 * window is WS_EX_NOACTIVATE on Win32, which also removes its taskbar
 * button, and below every other window it could then be neither found nor
 * closed. The hints are read by SDL_CreateWindow, so they are set here
 * rather than at option-parse time.
 * @param opts Session options; background above 0 sets the hints.
 * @return The window flags. */
static SDL_WindowFlags nr_window_flags(const openpete_options_t *opts)
{
    SDL_WindowFlags f = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (opts->background > 0) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
    }
    return f;
}


/** @brief Log the window state the window manager reports, for a harness to assert on.
 *
 * Called at bring-up and again at exit, since a window manager may focus or
 * restack a window on its own after it maps. The line carries the
 * --background request, input focus, focusability, hidden and minimized
 * flags, the size in pixels and the video driver. It is printed only for a
 * --background window or when OPENPETE_WINDOW_STATE is set to a value other
 * than 0.
 * @param win Window to report; NULL prints nothing.
 * @param when Label naming the call point, printed in the line. */
static void nr_log_window_state(SDL_Window *win, const char *when)
{
    if (!win) return;
    if (!s_background_requested) {
        /* OPENPETE_WINDOW_STATE=1 prints the same lines for a window created
         * without --background, which is what a comparison of the two needs. */
        const char *v = getenv("OPENPETE_WINDOW_STATE");
        if (!v || !*v || *v == '0') return;
    }
    const SDL_WindowFlags f = SDL_GetWindowFlags(win);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    OPL_INFO_F("native", "window state (%s): background=%d input_focus=%d "
               "focusable=%d hidden=%d minimized=%d pixels=%dx%d driver=%s",
               when, s_background_requested,
               (f & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0,
               (f & SDL_WINDOW_NOT_FOCUSABLE) ? 0 : 1,
               (f & SDL_WINDOW_HIDDEN) ? 1 : 0,
               (f & SDL_WINDOW_MINIMIZED) ? 1 : 0, pw, ph,
               SDL_GetCurrentVideoDriver());
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
    s_precreated_win = SDL_CreateWindow(title, w, h, nr_window_flags(opts));
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
    bool          headless;     /* surfaceless capture (no window/present); screenshots only */
    SDL_Window   *window;
    rhi_device   *dev;
    int           win_w, win_h; /* last-known pixel size, for resize detection */
    /* A size seen but not yet adopted, and when it was first seen - a resize
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
    rhi_texture  *drain_tex;   /* drain sub-cell key records (drain_records_build) */
    uint16_t     *vram_mirror;  /* NR_VRAM_W*NR_VRAM_H */

    /* Native 3D render: gouraud triangles (quad.vert/quad.frag) for
     * the cyclorama/sky scene the game pushes in. Replaces the VRAM re-display
     * in panel 0 when a scene is present. */
    rhi_pipeline *gouraud_pipe;
    rhi_pipeline *gouraud_blend_pipe; /* semi-transparent UNTEXTURED gouraud (alpha, ABR mode 0) */
    rhi_pipeline *gouraud_sub_pipe;   /* subtractive UNTEXTURED gouraud (ABR mode 2, B-F) - the Spyro shadow as a DEPTH-AWARE decal (shadow_decal.frag samples opaque_depth at set 0; depth-test OFF, the shader owns occlusion) */
    rhi_pipeline *gouraud_sub_ztest_pipe; /* fallback subtractive pipe (plain depth test + hw depth bias) when the depth pre-pass is unavailable */
    rhi_pipeline *gouraud_sub_fade_pipe;  /* subtractive, depth-test OFF, quad.frag - the full-screen mode-2 fade (NOT the decal pipe, which would sample/discard) */
    rhi_pipeline *gouraud_add_pipe;   /* additive UNTEXTURED gouraud (ABR mode 1, B+F) - glow rings */
    rhi_pipeline *gouraud_noz_pipe;   /* opaque UNTEXTURED gouraud, depth-test ON / write OFF ("noz" = no z WRITE) + stencil TEST - portal-sky colours inside the aperture mask */
    rhi_pipeline *portal_mask_pipe;   /* invisible (colour-write-off) depth-tested stencil-MARK of the portal aperture */
    rhi_pipeline *gouraud_mark_pipe;  /* WORLD-opaque untextured + stencil-MARK coverage (may be NULL -> plain pipe, no hole-fill) */
    rhi_pipeline *scene_tex_mark_pipe;/* WORLD-opaque textured + stencil-MARK coverage (may be NULL -> plain pipe, no hole-fill) */
    rhi_pipeline *gouraud_fill_pipe;  /* WORLD_FILL LP backing, stencil TEST_ZERO (paints only unmarked pixels; may be NULL -> no hole-fill) */
    rhi_pipeline *scene_tex_pipe;     /* opaque textured scene tris (VRAM sampler) */
    rhi_pipeline *scene_tex_blend_pipe; /* semi-transparent (ABE) textured tris (alpha, ABR mode 0) */
    rhi_pipeline *scene_tex_add_pipe; /* additive (ABR mode 1, B+F) textured tris - flame */
    rhi_pipeline *scene_tex_qadd_pipe; /* quarter-additive (ABR mode 3, B+F/4) textured tris - glass */
    rhi_pipeline *gouraud_qadd_pipe;  /* quarter-additive UNTEXTURED gouraud (ABR mode 3) */
    nr_cover_twin cover_twins[NR_COVER_TWINS]; /* every CPU-fed blended world pipe and its portal cover twin (cover_family_create) */
    int           cover_twin_n;
    rhi_pipeline *scene_tex_sub_pipe; /* subtractive (ABR mode 2, B-F) textured tris - moby shadows (plain depth test + bias; the fallback when the depth pre-pass is unavailable) */
    rhi_pipeline *scene_tex_sub_decal_pipe; /* moby shadows as a DEPTH-AWARE decal: shadow_decal_tex.frag samples opaque_depth (set 0) + VRAM (set 1); depth-test OFF, the shader owns wall/steep/platform occlusion. NULL -> fall back to scene_tex_sub_pipe */
    rhi_pipeline *gouraud_hud_pipes[NR_BLEND_CLASSES]; /* HUD overlay and panels: UNTEXTURED gouraud, depth-test OFF (on top), one pipe per blend class */
    rhi_pipeline *scene_tex_hud_pipes[NR_HUD_TEX_PIPES]; /* HUD overlay: textured (VRAM sampler), depth-test OFF (on top), one pipe per texel STP selection and blend class */
    rhi_pipeline *hd_tex_pipe;        /* HD replacement, HUD overlay: RGBA sampler, alpha blend, depth OFF */
    rhi_pipeline *hd_cutout_pipe;     /* HD replacement, opaque scene: depth-tested, alpha-cutout */
    rhi_pipeline *hd_cutout_mark_pipe;/* cutout twin with stencil MARK (WORLD group when hole-fill is on) */
    /* Dragon-rescue front pass: colour-write-off twins of the three ACTOR_FRONT
     * opaque pipes that REPLACE the stored depth (compare ALWAYS) - the
     * cutscene actors evict the terrain from their own pixels and nowhere
     * else. Same shaders as the twins they mirror, so the punched coverage is
     * texel-exact (identical key/cutout discards). Any NULL -> the front pass
     * degrades to a plain in-place actor draw (normal terrain occlusion). */
    rhi_pipeline *gouraud_punch_pipe;
    rhi_pipeline *scene_tex_punch_pipe;
    rhi_pipeline *hd_cutout_punch_pipe;
    rhi_pipeline *hd_scene_blend_pipe;/* HD replacement, alpha scene group (ABR 0): ztest, no zwrite */
    rhi_pipeline *hd_scene_add_pipe;  /* HD replacement, additive scene group (ABR 1): ztest, no zwrite */
    rhi_texture  *mat_pack_dummy;     /* 1x1 opaque white bound at set 1 for material
                                       * draws with no pack replacement (op_pack_tex
                                       * needs a valid RGBA descriptor either way) */
    rhi_buffer   *scene_vb;     /* the composed scene's verts, rebuilt per frame; grows with it */
    float         p0_cx, p0_hx, p0_hy; /* panel-0 fitted rect (clip space, cy=0) - the 4:3 CONTENT sub-rect */
    /* Target display aspect (W/H), from --aspect. 0 = AUTO (the default):
     * the render aspect follows the window/fullscreen shape live, filling it
     * with no bars (update_layout re-derives on every size change). >0 =
     * FIXED: the scene renders at this aspect and is letterboxed/pillarboxed
     * into the window (like PsyCross), NOT stretched to fill it. Set at init
     * from opts->aspect_w_over_h; live via openpete_native_set_aspect. */
    float         aspect_wh;
    /* Widescreen (Hor+) widen factor = render_aspect / 4:3, i.e. how much MORE
     * horizontal world the render aspect reveals vs 4:3 (1.0 = plain 4:3) -
     * window-derived under AUTO, --aspect-derived when fixed. The panel-0
     * content mapping (scene_to_clip via p0_hx) is the centred 4:3 sub-rect;
     * the reveal extends to ±p0_hx*ws_factor (the render rect), which fit_cell
     * fits into the window with bars (none under AUTO - the rect IS the
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
     * buffer) but don't change geometry - they ride the per-frame material
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
     * and the freeze overlay prefers it - so the green desaturated frozen-world
     * effect is captured headless. Holds the most recent world frame; once the
     * freeze bit is set the snapshot stops updating, freezing that frame. */
    rhi_texture  *shot_frozen;
    int           shot_frozen_w, shot_frozen_h;
    bool          shot_frozen_valid;

    /* SSAA (--ssaa N): the live vk scene renders into a supersampled render
     * target (ss_factor× the window per axis), then a fragment resolve pass
     * box-averages it in linear light into the 1× offscreen. ss_factor is the
     * EFFECTIVE factor - capped to the GPU's max image dimension (and reduced on
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
     * FT4 that samples PsyCross's VRAM scratch - so the green overlay shows OUR
     * render and survives the end-state where PsyCross's VRAM is gone. */
    rhi_texture  *frozen_tex;
    int           frozen_w, frozen_h;
    bool          frozen_valid;       /* a world frame has been captured */
    rhi_pipeline *freeze_overlay_pipe;/* samples frozen_tex, luma + tint (set 0) */
    rhi_pipeline *freeze_plain_pipe;  /* samples frozen_tex unaltered (set 0) */
    rhi_buffer   *fs_quad_vb;         /* full-target (-1..1) quad, UV 0..1 (RT copies) */
    rhi_buffer   *overlay_vb;         /* FPS overlay glyph quads (NDC), rebuilt per present */
    rhi_buffer   *version_vb;         /* build-version line glyphs (NDC), into its SS target */
    /* Version-line supersample: same route as the stats block, its own region
     * and placement. The composite quad is dynamic, unlike the stats block's:
     * its tint carries the fade, so it is rebuilt every present rather than
     * only on a resize. */
    rhi_texture  *ver_ss;
    int           ver_ss_w, ver_ss_h;
    rhi_buffer   *ver_quad_vb;
    rhi_buffer   *mod_ovl_vb;         /* mod draw-service overlay (rects + text glyphs, NDC), rebuilt per present */
    rhi_buffer   *menu_vb;            /* in-game settings menu panel/border/selection quads (NDC) */
    rhi_buffer   *menu_glyph_vb;      /* in-game settings menu 3D glyph text (NDC, depth-tested) */

    /* FPS-overlay 4× supersample: the glyphs render into ovl_ss (RGBA,
     * OVL_SS_FACTOR× the overlay region), then ovl_resolve_pipe box-downsamples +
     * straight-alpha-blends them onto the offscreen via the ovl_quad rect - so the
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
     * it onto the offscreen via menu_quad - so the 3D menu text is anti-aliased. */
    rhi_texture  *menu_ss;
    int           menu_ss_w, menu_ss_h;
    rhi_buffer   *menu_quad_vb;       /* composite quad at the panel rect */
    int           menu_quad_win_w, menu_quad_win_h;

    /* OSD toast supersample: a FIXED grid of OPENPETE_OSD_MAX slots, one per
     * possible live toast, rebuilt only on resize. Each present the compositor
     * draws one quad per toast sampling that toast's slot sub-rect, tinted by
     * its fade alpha (the resolve shader modulates by the quad's vertex
     * colour). Quads ride the per-present menu_vb ring: a sliding toast moves
     * its quad EVERY present, so a single buffer rewritten under wait_idle -
     * fine for chrome that only moves on a resize - would idle the device on
     * every frame of every animation. */
    rhi_texture  *osd_ss;
    int           osd_ss_w, osd_ss_h;
    /* One slot's 1x pixel extent, packed into a single word: written here on
     * the present thread when the grid is (re)built, read on the sim thread by
     * the glyph builder that lays out against it. Two ints would tear on a
     * resize - one tick of boxes laid out to a width and a height that were
     * never a pair - which is the same class as the perf region rect above,
     * and cheaper to close than to reason about. */
    uint64_t      osd_slot_px;   /* w | h << 32 */
    rhi_buffer   *osd_glyph_vb;       /* toast glyphs + frames (NDC, depth-tested) */
    rhi_buffer   *osd_quad_vb;        /* one composite quad per visible toast */
    rhi_buffer   *osd_panel_vb;       /* the toasts' subtractive panels */

    /* Depth-aware decal infrastructure: the WORLD-opaque depth PRE-PASS.
     * Rendered ONCE per present in the FULL-EXTENT PSX mapping at
     * max(ref, window) resolution; every consumer pass (native_rtt / SSAA /
     * offscreen panel / screenshots) samples it by PSX screen coord through the
     * same affine - no per-target re-render and no per-target alignment
     * contract. opaque_depth allocates lazily on the first present whose scene
     * carries a Spyro shadow (blend-3 untextured tris); NULL means the decal
     * falls back to the plain depth-tested subtractive pipe. Reusable by future
     * depth-aware passes (glow auras, soft particles). */
    rhi_pipeline *prepass_pipe;       /* WORLD pre-pass raster: depth + packed depth in colour */
    rhi_pipeline *prepass_depth_pipe; /* pre-pass full footprints + WORLD_FILL: depth only (colour-write-off) */
    rhi_pipeline *prepass_tex_pipe;   /* textured WORLD pre-pass raster: transparent-texel cutout */
    rhi_texture  *opaque_depth;       /* rhi_create_render_target_depth_sampled */
    int           od_w, od_h;         /* current opaque_depth allocation */
    rhi_buffer   *prepass_vb;         /* WORLD-opaque clip geometry, one batched draw */
    rhi_pipeline *terrain_key_pipe;   /* HP key pass (terrain_key.vert) */
    rhi_pipeline *terrain_key_tex_pipe; /* textured keyed faces: alpha-aware
                                         * cutout (psx_key_cutout.frag) - keys
                                         * exist only on opaque texels */
    rhi_buffer   *terrain_key_vb;     /* HP-opaque clip geometry, per-present ring */
    rhi_texture  *terrain_key_depth;  /* terrain og-key image (set 2) */
    rhi_texture  *moby_key_depth;     /* prop TRUE-depth image (set 3) - split so
                                       * the near-band embed rule sees curated
                                       * prop faces only */
    rhi_texture  *actor_key_depth;    /* moby og-key image (set 1): opaque ACTOR
                                       * keys, split from set 2 so fragments can
                                       * scope a moby's paint-over by their own
                                       * TRUE depth while terrain-vs-terrain
                                       * keeps contesting authored keys. Its
                                       * colour holds the moby identity owning
                                       * each key texel (op_akey_id) */
    rhi_texture  *moby_rank_rt;       /* moby og-RANK image (set 4): RGBA8
                                       * (rank lo, rank hi, moby ordinal) of the
                                       * LAST-painted ranked face per pixel -
                                       * the same-moby painter select input */
    rhi_pipeline *moby_rank_pipe;     /* untextured ranked faces (moby_rank.vert/.frag) */
    rhi_pipeline *moby_rank_cutout_pipe; /* textured: alpha-aware cutout - rank
                                          * exists only on opaque texels */
    int           tk_w, tk_h;         /* current key-image allocations (shared dims) */
    rhi_pipeline *occl_pipe;          /* occl_downsample.frag: prepass depth -> grid (may be NULL) */
    rhi_texture  *occl_grid;          /* OCCL_GW×OCCL_GH grid RT (may be NULL -> no culling) */
} s_nr;

/* When set, draw_native_scene maps the PSX screen to the FULL render target
 * (clip -1..1) instead of panel-0's window rect - used when rendering into
 * native_rtt for the diff pass (native_rtt IS the frame, no panel inset). */
static int s_render_full_extent = 0;
/* Horizontal divisor for the full-extent mapping. 1.0 = the 4:3 view fills the
 * target (native_rtt / diff). >1.0 = the Hor+ WIDESCREEN view fills the target:
 * the 4:3 content compresses to the centre (1/wf) and the revealed side world
 * fills out to ±1 - used to capture the freeze backdrop at the widescreen extent
 * so the pause/exit backdrop fills a widescreen window (not pillarboxed). */
static float s_render_freeze_wf = 1.0f;

/* Path-isolation mode (--render-paths, set by the game). When on, an empty
 * native scene presents the background instead of re-displaying the PSX VRAM
 * framebuffer - the VRAM is unfiltered, so falling back to it would defeat the
 * isolation. Generic core toggle; no-op until set. */
static bool s_isolation = false;
void openpete_native_set_isolation(bool on) { s_isolation = on; }

/** @brief Fill six vertices, two triangles, with a clip-space rect carrying one vertex colour.
 *
 * VRAM is uploaded top-down, so a VRAM panel passes v0 < v1; a bottom-up
 * GL readback passes v0 > v1. The colour is written to every vertex; the
 * version stamp and OSD toast quads use it for their tint and alpha, and
 * build_panel passes opaque white.
 * @param out Receives the vertices, triangles tl-tr-br and tl-br-bl.
 * @param cx Rect centre x, clip space.
 * @param cy Rect centre y, clip space.
 * @param hx Half-extent x.
 * @param hy Half-extent y.
 * @param u0 Texture u at the left edge.
 * @param v0 Texture v at the top edge.
 * @param u1 Texture u at the right edge.
 * @param v1 Texture v at the bottom edge.
 * @param rgba Vertex colour, packed RGBA8. */
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
/** @brief Build the six vertices of an untinted textured rectangle.
 *
 * build_panel_tinted with a white vertex colour.
 * @param out Receives two triangles, six vertices, at z 0.
 * @param cx Rect centre x, clip space.
 * @param cy Rect centre y, clip space.
 * @param hx Half-extent x, clip space.
 * @param hy Half-extent y, clip space.
 * @param u0 Texture u at the left edge.
 * @param v0 Texture v at the top edge.
 * @param u1 Texture u at the right edge.
 * @param v1 Texture v at the bottom edge. */
static void build_panel(rhi_vertex out[6], float cx, float cy, float hx, float hy,
                        float u0, float v0, float u1, float v1)
{
    build_panel_tinted(out, cx, cy, hx, hy, u0, v0, u1, v1, 0xFFFFFFFFu);
}

/** @brief Aspect-fit content inside a clip-space cell and return the fitted half-extents.
 *
 * The content is scaled uniformly by the smaller of the cell-to-content
 * width and height ratios, measured in window pixels, so it is letterboxed
 * or pillarboxed; the clear fills the bars.
 * @param cx Cell centre x in clip space; unused.
 * @param cy Cell centre y in clip space; unused.
 * @param chx Cell half-extent x in clip space.
 * @param chy Cell half-extent y in clip space.
 * @param content_w Content width in pixels.
 * @param content_h Content height in pixels.
 * @param win_w Window width in pixels.
 * @param win_h Window height in pixels.
 * @param out_hx Receives the fitted half-extent x in clip space.
 * @param out_hy Receives the fitted half-extent y in clip space. */
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

/** @brief Destroy every member of a blend family and clear its slots.
 *
 * @param dev Device owning the pipelines.
 * @param fam Family array, @p nsel times NR_BLEND_CLASSES slots; NULL slots
 *        are skipped.
 * @param nsel Number of texel STP selection rows. */
static void blend_family_destroy(rhi_device* dev, rhi_pipeline** fam, int nsel) {
    for (int k = 0; k < nsel * NR_BLEND_CLASSES; k++) {
        if (fam[k]) rhi_destroy_pipeline(dev, fam[k]);
        fam[k] = NULL;
    }
}

/** @brief Create a pipeline family from one description, one member per listed blend class and STP selection row.
 *
 * Member sel * NR_BLEND_CLASSES + b is created for each class b that
 * k_stp_sel_classes lists for row sel, with blend k_blend_class_rhi[b],
 * texel_stp_select sel, and every other field of @p base; unlisted members
 * are NULL. The family is all or none: a missing member would draw its
 * class through another member's blend, so on any failure the whole family
 * is destroyed and left NULL.
 * @param dev Device to create on.
 * @param base Pipeline description the members share.
 * @param fam Receives the members, @p nsel times NR_BLEND_CLASSES slots.
 * @param nsel Number of texel STP selection rows, starting at row 0. */
static void blend_family_create(rhi_device* dev, const rhi_pipeline_desc* base,
                                rhi_pipeline** fam, int nsel) {
    rhi_pipeline_desc d = *base;
    int ok = 1;
    for (int k = 0; k < nsel * NR_BLEND_CLASSES; k++) {
        const int sel = k / NR_BLEND_CLASSES, b = k % NR_BLEND_CLASSES;
        fam[k] = NULL;
        if (!((k_stp_sel_classes[sel] >> b) & 1u)) continue;
        d.blend = k_blend_class_rhi[b];
        d.texel_stp_select = sel;
        fam[k] = rhi_create_pipeline(dev, &d);
        ok = ok && fam[k];
    }
    if (!ok) blend_family_destroy(dev, fam, nsel);
}

/** @brief The _cover or _rim variant of a scene fragment stage, by its stock SPIR-V name.
 * @param frag The stock stage's .spv name.
 * @param rim Nonzero for the _rim variant.
 * @return The variant's .spv name, or NULL for a stage without one. */
static const char* cover_frag_of(const char* frag, int rim) {
    static const char* const k_map[][3] = {
        { "quad_key.frag.spv",      "quad_key_cover.frag.spv",      "quad_key_rim.frag.spv" },
        { "psx_scene_key.frag.spv", "psx_scene_key_cover.frag.spv", "psx_scene_key_rim.frag.spv" },
        { "quad.frag.spv",          "quad_cover.frag.spv",          "quad_rim.frag.spv" },
        { "psx_scene.frag.spv",     "psx_scene_cover.frag.spv",     NULL },
        { "hd_tex.frag.spv",        "hd_tex_cover.frag.spv",        NULL },
        { "hd_tex_cutout.frag.spv", NULL,                           "hd_tex_cutout_rim.frag.spv" },
    };
    for (size_t i = 0; frag && i < sizeof k_map / sizeof k_map[0]; i++)
        if (strcmp(frag, k_map[i][0]) == 0) return k_map[i][rim ? 2 : 1];
    return NULL;
}

/** @brief A stock terrain pipeline, the description it was built from, and which twin it takes (cover_family_create input). */
typedef struct {
    const rhi_pipeline_desc *desc;
    rhi_pipeline *stock;
    int rim;                    /* 1: the _rim twin (opaque), 0: the _cover twin (blended) */
} nr_cover_src;

/** @brief Build the portal twin of each scene pipeline that must yield to a portal preview.
 *
 * Each twin is its stock description with the fragment stage's _cover or
 * _rim variant (cover_frag_of) and the cover planes' UBO at set 5. A NULL
 * stock pipeline is skipped; a stage without the variant, and a twin that
 * fails to build, leave the twin NULL, and that pipeline draws without the
 * test.
 * @param dev Device.
 * @param src The stock pipelines and their descriptions.
 * @param n Entries in @p src; at most NR_COVER_TWINS are kept.
 * @param out Receives the twins.
 * @return The number of entries written to @p out. */
static int cover_family_create(rhi_device* dev, const nr_cover_src* src, int n,
                               nr_cover_twin* out) {
    int k = 0;
    for (int i = 0; i < n && k < NR_COVER_TWINS; i++) {
        if (!src[i].stock) continue;
        rhi_pipeline_desc d = *src[i].desc;
        d.frag_spv = cover_frag_of(src[i].desc->frag_spv, src[i].rim);
        d.uniform_set5 = true;
        out[k].stock = src[i].stock;
        out[k].rim = src[i].rim;
        out[k].cover = d.frag_spv ? rhi_create_pipeline(dev, &d) : NULL;
        k++;
    }
    return k;
}

/** @brief Destroy the twins cover_family_create built and empty the table.
 * @param dev Device.
 * @param tw The table.
 * @param n Its entry count; set to 0. */
static void cover_family_destroy(rhi_device* dev, nr_cover_twin* tw, int* n) {
    for (int i = 0; i < *n; i++) {
        if (tw[i].cover) rhi_destroy_pipeline(dev, tw[i].cover);
        tw[i].stock = tw[i].cover = NULL;
    }
    *n = 0;
}

/** @brief Point the RHI's pipeline cache at the per-user state "cache" directory, created on demand.
 *
 * Compiled pipelines then persist per GPU and driver across runs; when the
 * directory cannot be resolved the cache stays in memory. */
static void nr_set_pipeline_cache_dir(void)
{
    char dir[1024];
    rhi_set_pipeline_cache_dir(openpete_paths_state_dir("cache", dir, sizeof dir, 1) == 0 ? dir : NULL,
                               op_atomic_write_file);
}

/** @brief Log the wireframe view's raster mode when the view is on.
 *
 * With fillModeNonSolid the scene pipelines draw triangle edges in line
 * mode; without it the view falls back to the filled NOTEX view, logged as
 * a warning. No-op when the wireframe view is off.
 * @param dev Device whose features decide the mode. */
static void wireframe_report(const rhi_device *dev)
{
    if (!s_wireframe) return;
    if (rhi_device_has_fill_non_solid(dev))
        OPL_INFO_F("native", "%s", "wireframe: scene pipelines draw triangle edges (line mode)");
    else
        OPL_WARN_F("native", "%s", "wireframe: device lacks fillModeNonSolid; "
                                   "drawing the filled NOTEX view instead");
}

int openpete_native_render_init(const openpete_options_t *opts, int win_w, int win_h)
{
    if (!opts)
        return 0;

    /* PsyCross mode under --headless: PsyCross rasterizes for the oracle and
     * writes the -og capture off its own FBO. There is no native scene to
     * compose and nothing for a surfaceless device to draw, so native stays
     * off - the behaviour this mode had before the native window became its
     * display. Checked ahead of the vk headless guard below, which admits
     * surfaceless bring-up when screenshots or the identity audit are armed;
     * neither has a native consumer here. */
    if (opts->headless && opts->renderer == OPENPETE_RENDERER_PSYCROSS)
        return 0;

    /* Under --headless, bring the native renderer up SURFACELESS purely to
     * service screenshots (CI visual-regression capture under lavapipe) - but
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
        scene_box_note_inactive("native renderer not brought up (--headless without screenshots)");
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

    /* Untextured lighting view (--notex / OPENPETE_NOTEX, fragment
     * spec-constant 2): textured scene fragments output the raw gouraud
     * vertex colour. Live changes queue via openpete_native_set_notex. */
    s_notex = s_notex_pending = (opts->notex > 0);
    rhi_set_notex(s_notex);
    if (s_notex)
        OPL_INFO_F("native", "%s", "notex: untextured lighting view ON");
    s_wireframe = s_wireframe_pending = (opts->wireframe != 0);
    rhi_set_wireframe(s_wireframe);

    /* Render aspect from --aspect (W/H); 0 -> AUTO (follow the window shape,
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
    nr_set_pipeline_cache_dir();
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
             * missing SDL_WINDOW_VULKAN creation flag is fine - only the
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
            win = SDL_CreateWindow(title, w, h, nr_window_flags(opts));
            if (!win) {
                OPL_ERROR_F("native", "SDL_CreateWindow(VULKAN) failed: %s", SDL_GetError());
                return 1;
            }
        }

        /* The window icon has to be set here, not on PsyCross's window: in the
         * default (null-GPU) path PsyCross never creates one, so the visible
         * window would ship icon-less on any platform without a resource-file
         * icon - i.e. everywhere but Windows. */
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

        /* --fullscreen 1: launch in borderless-desktop fullscreen - the exact
         * state openpete_native_toggle_fullscreen switches into, so Alt+Enter /
         * the O-menu DISPLAY row toggle straight back to the windowed size
         * above. Before rhi_create so the swapchain sizes to the monitor. */
        s_background_requested = opts->background > 0;
        if (opts->fullscreen && s_background_requested) {
            /* A fullscreen window covers the screen, which is the one thing
             * the background window must not do, so the background wins. */
            OPL_WARN_F("native", "%s", "--background with --fullscreen 1: "
                       "--background wins, staying windowed");
        } else if (opts->fullscreen) {
            SDL_SetWindowFullscreenMode(win, NULL);  /* borderless desktop */
            SDL_SetWindowFullscreen(win, true);
            OPL_INFO_F("native", "%s", "--fullscreen: borderless desktop at launch");
        }
        if (s_background_requested)
            openpete_window_send_to_back(win);
        nr_log_window_state(win, "bring-up");

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
     * rather than spin failed frames forever. */
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
    /* --terrain-visibility: the terrain feeder's candidate set (frustum =
     * every sector inside the displayed frame, hidden or not). */
    s_terrain_vis = (opts->terrain_visibility == OPENPETE_TERRAIN_VIS_GAME)
                    ? OPENPETE_TERRAIN_VIS_GAME : OPENPETE_TERRAIN_VIS_FRUSTUM;

    /* Key-contest granularity (SLOT_CONTEST, fragment spec-constant 4): a
     * game whose draw submits mobys and terrain through ONE ordering table
     * has both classes' keys on one 128-world scale, so the contest compares
     * bucket indices and gives an equal bucket to the terrain (the moby pass
     * is submitted first, so the terrain face is appended after it). A game
     * whose two classes do not share a table keeps the world-distance rule.
     * Per-game data, baked here before the pipelines below are built. */
    rhi_set_slot_contest(game_layout.terrain.slot_ot_contest);
    /* (--texture-mapping was resolved near the top of this function - before
     * the dev overlay snapshotted knob boot values - so every pipeline below
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
     * (quad.vert/quad.frag) - exactly the cyclorama dome's shading. */
    rhi_pipeline_desc gouraud_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad_key.frag.spv",   /* + terrain LP key-resolve (set 2) */
        .blend = RHI_BLEND_NONE, .depth_test = true,  /* sort actors near->far */
        .uses_vram = false, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = 0,
    };
    /* WORLD-group variants that also stencil-MARK their coverage: the
     * WORLD-opaque group rasterizes normally AND writes stencil=1 wherever a
     * fragment passes the depth test, defining "pixels real world geometry
     * covered". The terrain LP hole-fill layer (WORLD_FILL, TEST_ZERO below)
     * then paints only the uncovered pixels - LP<->HP T-junction seams - and can
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
    /* Semi-transparent UNTEXTURED gouraud pipeline: the Spyro silhouette
     * shadow (func_80059A48) is a POLY_G3 strip with ABE set (ABR mode 0,
     * (B+F)/2) - untextured, so it can't go through the textured-blend pipe.
     * RHI_BLEND_ALPHA = src*a + dst*(1-a); the feeder sets vertex alpha = 128
     * (~0.5) so the result is (B+F)/2, reproducing mode 0. depth_write_off: the
     * shadow tests against opaque geometry (a wall/ground occludes it) but
     * never occludes another transparent layer. Same quad.vert/quad.frag as the
     * opaque gouraud pipe - quad.frag emits the full vertex rgba incl. alpha. */
    rhi_pipeline_desc gouraud_blend_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad_key.frag.spv",   /* + og-key contest (transparent LP terrain contests, never emits) */
        .blend = RHI_BLEND_ALPHA, .depth_test = true, .depth_write_off = true,
        .uses_vram = false, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = 0,
    };
    /* Subtractive UNTEXTURED gouraud pipeline (the Spyro silhouette
     * shadow): POLY_G3 with ABR mode 2 (dst = B - F). RHI_BLEND_SUBTRACT
     * reverse-subtracts the vertex colour from the framebuffer, darkening the
     * ground (the shadow grey is subtracted; black vertices leave it
     * unchanged).
     * DEPTH-AWARE DECAL: shadow_decal.frag samples the WORLD-opaque pre-pass
     * depth (opaque_depth, set 0) and rejects only pixels whose terrain stands
     * ABOVE the shadow's ground plane by more than wall_height (a wall/ledge)
     * - replacing the retired ×0.70 view-Z pull, which punched through walls.
     * depth_test = false: the shader OWNS occlusion, so the flat fan never
     * self-culls against its own bumpy/slanted ground (the artifact the pull
     * used to mask). */
    rhi_pipeline_desc gouraud_sub_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "shadow_decal.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = false, .wire_scene = true,
        /* set 0 = opaque_depth; set 1 = VRAM (unread) + the terrain key
         * image; set 2 = the player fan's footprint block */
        .uses_vram = true, .set1_vram_depth = true, .uniform_set2 = true,
        .push_const_size = sizeof(shadow_decal_pc),
    };
    /* Fallback for a device whose depth format is not sampleable (the pre-pass
     * RT can't be created): the pre-decal behaviour - plain depth test + the
     * hardware depth bias (correct against walls, may slice on steep ground). */
    rhi_pipeline_desc gouraud_sub_ztest_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = true, .depth_write_off = true,
        .depth_bias = true,
        .uses_vram = false, .push_const_size = 0,
    };
    /* The full-screen mode-2 fade (func_800190D4) is also ABR-2 subtractive but
     * is NOT a ground decal - it tints the whole frame. It used to share the
     * shadow pipe; now that the shadow pipe is the depth-aware decal (which
     * would sample opaque_depth and discard), the fade gets its own plain
     * subtractive pipe (quad.frag, depth-test off - the z=0 quad passed the
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
     * so the decal's neighbour-texel NORMAL reconstruction is exact - the
     * canonical vz/(vz+4096) curve interpolates screen-linearly with a
     * distorted local gradient across large terrain tris (read flat floors as
     * fronto-parallel). The fragment stage, terrain_prepass.frag (shared with
     * the retained terrain cache's pre-pass), also writes its depth packed to
     * 24 bits into the colour attachment; this pipe draws the backstop. The
     * WORLD occluders draw with colour on through the cutout pipe below, and
     * the depth pipe redraws textured WORLD faces over their whole footprint
     * and draws the WORLD_FILL backing with colour off, so the colour
     * attachment holds the depth of what the WORLD faces paint, which the
     * occlusion grid reduces, while the depth aspect (the decal's) holds the
     * full footprints and the backing. */
    rhi_pipeline_desc prepass_desc = {
        .vert_spv = "prepass.vert.spv",
        .frag_spv = "terrain_prepass.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_class = RHI_DEPTH_FORWARD,
        .uses_vram = false, .push_const_size = 0,
    };
    rhi_pipeline_desc prepass_depth_desc = prepass_desc;
    prepass_depth_desc.color_write_off = true;
    /* WORLD colour run, per material run: prepass_cutout.frag samples the
     * texel the scene draw does and drops the transparent texels it
     * discards, so the colour attachment marks an occluder only where a
     * face paints. */
    rhi_pipeline_desc prepass_tex_desc = prepass_desc;
    prepass_tex_desc.frag_spv = "prepass_cutout.frag.spv";
    prepass_tex_desc.uses_vram = true;
    prepass_tex_desc.push_const_size = sizeof(psx_material);
    /* Terrain HP KEY pass: rasterise opaque HP terrain with depth = the
     * og OT key (true view-Z + authored slot offset) into a private depth
     * image; the scene's LP fragments key-contest it (quad_key.frag /
     * psx_scene_key.frag). Rendered per scene TARGET at that target's size so
     * texelFetch(gl_FragCoord.xy) aligns exactly. */
    rhi_pipeline_desc terrain_key_desc = {
        .vert_spv = "terrain_key.vert.spv",
        .frag_spv = "key_ident.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_class = RHI_DEPTH_FORWARD,
        .uses_vram = false, .push_const_size = 0,
    };
    /* Additive UNTEXTURED gouraud pipeline (the glow rings): POLY_G3 with
     * ABR mode 1 (dst = B + F). RHI_BLEND_ADD = src ONE + dst ONE. The glow is a
     * gouraud fan, bright centre colour fading to black ring vertices, so the
     * additive accumulation peaks at the centre and vanishes at the edge - the
     * classic halo. depth_write_off like the other transparent passes. */
    rhi_pipeline_desc gouraud_add_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad_key.frag.spv",   /* + og-key contest (transparent LP terrain contests, never emits) */
        .blend = RHI_BLEND_ADD, .depth_test = true, .depth_write_off = true,
        .uses_vram = false, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = 0,
    };
    /* Opaque UNTEXTURED gouraud, depth-test ON but depth-WRITE OFF: the portal-sky
     * previews (portal.c, OPENPETE_SCENE_PORTAL_CYCLORAMA). They are a far
     * background seen through the arch window - the arch stone's depth (written by
     * the WORLD group, drawn first) must occlude them, but they must never write
     * depth, so the actors/player drawn afterward are never per-pixel cut by the
     * portal sky as Spyro flies into/through the portal. Same quad.vert/quad.frag
     * as the opaque pipe; only the depth-write differs. */
    rhi_pipeline_desc gouraud_noz_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        /* Portal-sky colours - depth-TEST ON (LEQUAL at the
         * portal-plane z, write OFF) + stencil TEST, drawn AFTER the ACTOR
         * group. The depth test is what lets the sky repaint over mobys
         * BEHIND the plane while losing to geometry/mobys in front of it;
         * the stencil mask confines it to the aperture footprint. (A fully
         * depth-free variant was tried and replaced - with a depth STAMP it
         * clipped Spyro mid-entry, and without one it needed the stamp.) */
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_write_off = true,
        .stencil = RHI_STENCIL_TEST,
        .uses_vram = false, .push_const_size = 0,
    };
    /* The portal aperture mask: rasterize the portal's sky footprint
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
    /* Textured scene pipeline: samples the PSX VRAM mirror via the ported
     * decoder + per-draw material, modulates by the vertex colour (tex*col/128).
     * Reuses psx_sample.vert (clip-space passthrough + uv + colour). */
    rhi_pipeline_desc scene_tex_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene_key.frag.spv",   /* + terrain LP key-resolve (set 2) */
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .uses_vram = true, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = sizeof(psx_material),
    };
    /* Textured WORLD-group stencil-MARK variant - most terrain is
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
     * depth, so they never occlude each other (ordering among them needs a
     * back-to-front sort on top of this). */
    rhi_pipeline_desc scene_tex_blend_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene_key.frag.spv",   /* + og-key contest (transparent terrain contests, never emits) */
        .blend = RHI_BLEND_ALPHA, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = sizeof(psx_material),
    };
    /* Additive (ABR mode 1, dst = B + F) textured pipeline: the first
     * additive consumer is Spyro's flame (func_80058D64 / r_flame.s), a textured
     * gouraud quad ribbon whose tpage selects ABR=1. RHI_BLEND_ADD = src ONE +
     * dst ONE; depth_write_off so the glow tests against opaque geometry (a wall
     * occludes the flame) but additive layers never occlude each other. The
     * grey-ramp vertex colour modulates the flame sprite (tex*col/128) and the
     * result accumulates into the framebuffer - bright core, never darkens. */
    rhi_pipeline_desc scene_tex_add_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene_key.frag.spv",   /* + og-key contest (transparent terrain contests, never emits) */
        .blend = RHI_BLEND_ADD, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true, .push_const_size = sizeof(psx_material),
    };
    /* Quarter-additive (ABR mode 3, dst = B + F/4) pipelines: the faint-overlay
     * class - glass, the butterfly-bottle panes. The modulated source is scaled
     * by the pipeline's blend constant, so the layer tints what is behind it
     * instead of blowing it out (full additive) or blacking it out (the
     * reverse-subtract an ABR-2 pipe would apply). depth_write_off with the
     * other transparent classes. */
    rhi_pipeline_desc scene_tex_qadd_desc = scene_tex_add_desc;
    scene_tex_qadd_desc.blend = RHI_BLEND_ADD_QUARTER;
    rhi_pipeline_desc gouraud_qadd_desc = gouraud_add_desc;
    gouraud_qadd_desc.blend = RHI_BLEND_ADD_QUARTER;
    /* Subtractive (ABR mode 2, dst = B - F) textured pipeline: moby
     * shadows (func_80059F8C), a textured FT3 fan whose tpage selects ABR=2.
     * RHI_BLEND_SUBTRACT reverse-subtracts the modulated shadow sprite from the
     * framebuffer (tex*col/128 darkens the ground), depth_write_off so the
     * shadow tests against opaque geometry but never occludes another layer. */
    rhi_pipeline_desc scene_tex_sub_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = true, .depth_write_off = true,
        .depth_bias = true, /* shadow decal: hardware coplanar tie-break */
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
        .blend = RHI_BLEND_SUBTRACT, .depth_test = false, .wire_scene = true,
        /* set 0 = opaque_depth; set 1 = VRAM + the terrain key image */
        .uses_vram = true, .set1_vram_depth = true,
        .push_const_size = sizeof(shadow_decal_tex_pc),
    };
    /* HUD overlay pipelines (depth_test = false): HUD/UI mobys are a 2D overlay
     * drawn LAST, ignoring the depth buffer so they sit on top of all world/actor
     * geometry regardless of their world-space view-Z (the PSX draws HUD in a
     * separate near-OT pass). Painter's order = submission order. One untextured
     * (gouraud) + one textured (VRAM sampler), mirroring the opaque pair.
     *
     * The untextured HUD pipe is a blend family (blend_family_create sets
     * .blend per member). Entry 0 draws the glyphs and the letterbox bars.
     * The semi-transparent 2D panels (0x2A POLY_F4) draw through the member of
     * the ABR their DR_MODE selects: ABR 0 (B+F)/2 is the alpha member at
     * vertex alpha 0x80 (quad.frag emits the full vertex rgba); ABR 2 (B-F) is
     * the reverse-subtract member, which darkens the framebuffer by the box
     * colour (the in-game shaded box behind HUD text, func_8001860C, and the
     * pause/inventory panel, func_8001A40C, both emit their F4 under
     * SetDrawMode(getTPage(0, 2, ..))); ABR 1 and 3 are the additive and
     * quarter-additive members. The subtract member also draws the menu and
     * toast panels. */
    rhi_pipeline_desc gouraud_hud_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = false, .push_const_size = 0,
    };
    /* Textured HUD layer: the screen-space 2D sprites and quads. A 2D
     * primitive carries its own semi-transparency bit and the ABR of its
     * texture page, so the layer is a family, one pipe per texel STP
     * selection and blend class (blend_family_create sets .blend and
     * .texel_stp_select per member; entry 0 is the opaque pipe). */
    rhi_pipeline_desc scene_tex_hud_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* HD texture-replacement pipe:
     * samples a host RGBA8 replacement (hd_tex.frag) with real per-texel alpha
     * instead of the PSX VRAM decode. Depth OFF (the target is a HUD-layer
     * 2D quad); push constants carry the replaced texel box. */
    rhi_pipeline_desc hd_tex_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "hd_tex.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(hd_pc),
    };
    /* Opaque-scene HD replacement: depth-tested cutout (true blending on
     * opaque groups would break Z ordering); the MARK twin keeps the
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
    /* Dragon-rescue front pass, step 1 of 2 - the DEPTH PUNCH twins of the
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
     * additive (ABR 1) - depth-tested, no depth write, mirroring their SD
     * counterparts. STP encoding: dumps bake STP texels at alpha 128, so
     * src-over on the alpha pipe reproduces (B+F)/2 while non-STP texels
     * stay opaque. The SUBTRACTIVE group keeps SD (its draws depend on the
     * caller's dynamic depth-bias state, which an HD rebind would
     * invalidate - and shadow-fan replacement is out of scope). */
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
    /* Retained-framebuffer freeze backdrop: the same full-panel quad over the same
     * snapshot, drawn through blit.frag so the frozen frame reaches the screen
     * unaltered (the game applied no CLUT and no tint - it simply did not clear). */
    rhi_pipeline_desc freeze_plain_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "blit.frag.spv",
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
     * (graceful fallback: no grid -> no culling). */
    rhi_pipeline_desc occl_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "occl_downsample.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = 4 * sizeof(int),
    };
    /* FPS-overlay supersample resolve: like resolve_desc but alpha-aware
     * (overlay_resolve.frag) and alpha-blended, to composite the downsampled
     * glyphs over the scene. Optional - draw_perf_overlay falls back to a direct
     * 1× draw if this (or ovl_quad_vb) didn't come up. */
    rhi_pipeline_desc ovl_resolve_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "overlay_resolve.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = false,
        .uses_vram = true, .push_const_size = 2 * sizeof(int),   /* ssaa, outline */
    };
    wireframe_report(dev);
    rhi_pipeline *blit_pipe    = rhi_create_pipeline(dev, &blit_desc);
    rhi_pipeline *vram_pipe    = rhi_create_pipeline(dev, &vram_desc);
    rhi_pipeline *gouraud_pipe = rhi_create_pipeline(dev, &gouraud_desc);
    rhi_pipeline *gouraud_blend_pipe = rhi_create_pipeline(dev, &gouraud_blend_desc);
    rhi_pipeline *gouraud_sub_pipe = rhi_create_pipeline(dev, &gouraud_sub_desc);
    rhi_pipeline *gouraud_sub_ztest_pipe = rhi_create_pipeline(dev, &gouraud_sub_ztest_desc);
    rhi_pipeline *gouraud_sub_fade_pipe = rhi_create_pipeline(dev, &gouraud_sub_fade_desc);
    rhi_pipeline *prepass_pipe = rhi_create_pipeline(dev, &prepass_desc);
    rhi_pipeline *prepass_depth_pipe = rhi_create_pipeline(dev, &prepass_depth_desc);
    rhi_pipeline *prepass_tex_pipe = rhi_create_pipeline(dev, &prepass_tex_desc);
    rhi_pipeline *gouraud_add_pipe = rhi_create_pipeline(dev, &gouraud_add_desc);
    rhi_pipeline *gouraud_noz_pipe = rhi_create_pipeline(dev, &gouraud_noz_desc);
    rhi_pipeline *portal_mask_pipe = rhi_create_pipeline(dev, &portal_mask_desc);
    rhi_pipeline *scene_tex_pipe = rhi_create_pipeline(dev, &scene_tex_desc);
    rhi_pipeline *scene_tex_blend_pipe = rhi_create_pipeline(dev, &scene_tex_blend_desc);
    rhi_pipeline *scene_tex_add_pipe = rhi_create_pipeline(dev, &scene_tex_add_desc);
    rhi_pipeline *scene_tex_qadd_pipe = rhi_create_pipeline(dev, &scene_tex_qadd_desc);
    rhi_pipeline *gouraud_qadd_pipe = rhi_create_pipeline(dev, &gouraud_qadd_desc);
    rhi_pipeline *scene_tex_sub_pipe = rhi_create_pipeline(dev, &scene_tex_sub_desc);
    rhi_pipeline *scene_tex_sub_decal_pipe = rhi_create_pipeline(dev, &scene_tex_sub_decal_desc); /* optional: NULL -> fall back to scene_tex_sub_pipe */
    rhi_pipeline *gouraud_hud_pipes[NR_BLEND_CLASSES];
    blend_family_create(dev, &gouraud_hud_desc, gouraud_hud_pipes, 1);
    rhi_pipeline *scene_tex_hud_pipes[NR_HUD_TEX_PIPES];
    blend_family_create(dev, &scene_tex_hud_desc, scene_tex_hud_pipes, OPENPETE_STP_SELS);
    rhi_pipeline *hd_tex_pipe = rhi_create_pipeline(dev, &hd_tex_desc);   /* HD pipes: allowed to fail (NULL = feature off) */
    rhi_pipeline *hd_cutout_pipe = rhi_create_pipeline(dev, &hd_cutout_desc);
    rhi_pipeline *hd_cutout_mark_pipe = rhi_create_pipeline(dev, &hd_cutout_mark_desc);
    rhi_pipeline *gouraud_punch_pipe   = rhi_create_pipeline(dev, &gouraud_punch_desc);
    rhi_pipeline *scene_tex_punch_pipe = rhi_create_pipeline(dev, &scene_tex_punch_desc);
    rhi_pipeline *hd_cutout_punch_pipe = rhi_create_pipeline(dev, &hd_cutout_punch_desc);
    rhi_pipeline *hd_scene_blend_pipe = rhi_create_pipeline(dev, &hd_scene_blend_desc);
    rhi_pipeline *hd_scene_add_pipe   = rhi_create_pipeline(dev, &hd_scene_add_desc);
    rhi_pipeline *freeze_overlay_pipe = rhi_create_pipeline(dev, &freeze_overlay_desc);
    rhi_pipeline *freeze_plain_pipe = rhi_create_pipeline(dev, &freeze_plain_desc);
    rhi_pipeline *resolve_pipe = rhi_create_pipeline(dev, &resolve_desc);
    /* Per-present-rewritten buffers (scene/prepass/overlay) are frame-in-flight
     * RINGS: their every-present rhi_update_buffer used to race the
     * frame two presents ago, previously masked only by the per-present VRAM
     * upload's vkQueueWaitIdle drain. panels/ovl_quad/fs_quad update rarely
     * (init / re-layout) under rhi_wait_idle and stay single. */
    rhi_buffer   *panels_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * NR_PANELS * 6);
    /* One scene fill per frame, so a single generation. */
    rhi_buffer   *scene_vb  = rhi_create_vertex_buffer_dynamic_gens(dev, sizeof(rhi_vertex) * SCENE_VB_WAS, 1u);
    rhi_buffer   *fs_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    rhi_buffer   *overlay_vb = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * OVL_MAX_VERTS);
    /* Mod draw-service overlay (optional - NULL just disables mod draws). */
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
     * where a toast is up while the menu is open - which is the common case,
     * since changing a setting is what raises a toast - and the second update
     * would overwrite the bytes the first draw reads at execution time. */
    rhi_buffer   *osd_panel_vb = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * OPENPETE_OSD_MAX * 6);
    /* Optional occlusion-grid resources (graceful fallback: NULL -> no culling). */
    rhi_pipeline *occl_pipe = rhi_create_pipeline(dev, &occl_desc);
    /* Optional hole-fill pipes (graceful fallback: any NULL -> WORLD uses
     * the plain pipes and the FILL layer is skipped - the pre-fill look). */
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
    rhi_buffer   *prepass_vb = rhi_create_vertex_buffer_dynamic_gens(dev, sizeof(rhi_vertex) * SCENE_VB_WAS, 1u);
    /* Optional key-pass resources (graceful fallback: NULL -> the feeder
     * emits koff=0, LP fragments never sample, terrain contests at raw true
     * depth - the OT_BIAS=0 look). The key IMAGE allocates lazily per target
     * size (ensure_terrain_key_depth). */
    rhi_pipeline *terrain_key_pipe = rhi_create_pipeline(dev, &terrain_key_desc);
    rhi_buffer   *terrain_key_vb = rhi_create_vertex_buffer_dynamic_gens(dev, sizeof(rhi_vertex) * SCENE_VB_WAS, 1u);
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
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_class = RHI_DEPTH_FORWARD,
        .uses_vram = true, .uses_vram2 = true,   /* set 1: drain records */
        .push_const_size = sizeof(psx_material),
    };
    rhi_pipeline *terrain_key_tex_pipe = terrain_key_pipe
        ? rhi_create_pipeline(dev, &terrain_key_tex_desc) : NULL;
    if (terrain_key_pipe && !terrain_key_tex_pipe)
        OPL_ERROR_F("native", "textured key cutout pipe unavailable; "
                              "textured faces key full-footprint");
    /* Moby RANK pass (same-moby painter select): rasterise ranked moby faces
     * with depth = their og OT walk rank into an RGBA8 (rank, ordinal) image;
     * ranked scene fragments discard where a LATER-ranked face of the SAME
     * moby covers the pixel. Optional (NULL -> rank image stays cleared, every
     * ranked face decodes "no cover" and resolves at true depth). */
    rhi_pipeline_desc moby_rank_desc = {
        .vert_spv = "moby_rank.vert.spv",
        .frag_spv = "moby_rank.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_class = RHI_DEPTH_FORWARD,
        .uses_vram = false, .push_const_size = 0,
    };
    rhi_pipeline_desc moby_rank_cutout_desc = {
        .vert_spv = "moby_rank.vert.spv",
        .frag_spv = "moby_rank_cutout.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_class = RHI_DEPTH_FORWARD,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    rhi_pipeline *moby_rank_pipe = terrain_key_pipe
        ? rhi_create_pipeline(dev, &moby_rank_desc) : NULL;
    rhi_pipeline *moby_rank_cutout_pipe = moby_rank_pipe
        ? rhi_create_pipeline(dev, &moby_rank_cutout_desc) : NULL;
    if (terrain_key_pipe && !moby_rank_pipe)
        OPL_ERROR_F("native", "moby rank pass unavailable; same-moby painter select disabled");
    rhi_texture  *vram_tex  = rhi_create_vram(dev, NR_VRAM_W, NR_VRAM_TEX_H);
    rhi_texture  *drain_tex = rhi_create_u32x4(dev, DRAIN_REC_PER_ROW * 4, DRAIN_REC_ROWS);
    /* The CPU mirror IS the owned native VRAM model (static storage in
     * native_vram.c) - written by the native ops at op time and shadow-synced
     * from PsyCross's mirror per dirty rect at present time. */
    uint16_t     *mirror    = openpete_vram_data();
    if (!blit_pipe || !vram_pipe || !gouraud_pipe || !gouraud_blend_pipe || !gouraud_sub_pipe || !gouraud_sub_ztest_pipe || !gouraud_sub_fade_pipe || !prepass_pipe || !prepass_depth_pipe || !prepass_tex_pipe || !gouraud_add_pipe || !scene_tex_pipe || !scene_tex_blend_pipe || !scene_tex_add_pipe || !scene_tex_sub_pipe || !gouraud_hud_pipes[0] || !scene_tex_hud_pipes[0] || !panels_vb || !scene_vb || !vram_tex || !mirror ||
        !freeze_overlay_pipe || !freeze_plain_pipe || !resolve_pipe || !fs_quad_vb || !overlay_vb || !prepass_vb) {
        OPL_ERROR_F("native", "RHI setup failed; continuing without native renderer");
        if (prepass_vb)    rhi_destroy_buffer(dev, prepass_vb);
        if (prepass_pipe)  rhi_destroy_pipeline(dev, prepass_pipe);
        if (prepass_depth_pipe) rhi_destroy_pipeline(dev, prepass_depth_pipe);
        if (prepass_tex_pipe) rhi_destroy_pipeline(dev, prepass_tex_pipe);
        if (gouraud_sub_fade_pipe) rhi_destroy_pipeline(dev, gouraud_sub_fade_pipe);
        if (gouraud_sub_ztest_pipe) rhi_destroy_pipeline(dev, gouraud_sub_ztest_pipe);
        if (resolve_pipe) rhi_destroy_pipeline(dev, resolve_pipe);
        if (freeze_overlay_pipe) rhi_destroy_pipeline(dev, freeze_overlay_pipe);
        if (freeze_plain_pipe) rhi_destroy_pipeline(dev, freeze_plain_pipe);
        if (overlay_vb)    rhi_destroy_buffer(dev, overlay_vb);
        if (mod_ovl_vb)    rhi_destroy_buffer(dev, mod_ovl_vb);
        if (ovl_quad_vb)   rhi_destroy_buffer(dev, ovl_quad_vb);
        if (menu_quad_vb)  rhi_destroy_buffer(dev, menu_quad_vb);
        if (fs_quad_vb)    rhi_destroy_buffer(dev, fs_quad_vb);
        if (vram_tex)      rhi_destroy_texture(dev, vram_tex);
        if (scene_vb)      rhi_destroy_buffer(dev, scene_vb);
        if (panels_vb)     rhi_destroy_buffer(dev, panels_vb);
        blend_family_destroy(dev, scene_tex_hud_pipes, OPENPETE_STP_SELS);
        if (hd_tex_pipe) rhi_destroy_pipeline(dev, hd_tex_pipe);
        if (hd_cutout_pipe) rhi_destroy_pipeline(dev, hd_cutout_pipe);
        if (hd_cutout_mark_pipe) rhi_destroy_pipeline(dev, hd_cutout_mark_pipe);
        if (gouraud_punch_pipe) rhi_destroy_pipeline(dev, gouraud_punch_pipe);
        if (scene_tex_punch_pipe) rhi_destroy_pipeline(dev, scene_tex_punch_pipe);
        if (hd_cutout_punch_pipe) rhi_destroy_pipeline(dev, hd_cutout_punch_pipe);
        if (hd_scene_blend_pipe) rhi_destroy_pipeline(dev, hd_scene_blend_pipe);
        if (hd_scene_add_pipe) rhi_destroy_pipeline(dev, hd_scene_add_pipe);
        blend_family_destroy(dev, gouraud_hud_pipes, 1);
        if (scene_tex_sub_decal_pipe) rhi_destroy_pipeline(dev, scene_tex_sub_decal_pipe);
        if (scene_tex_sub_pipe) rhi_destroy_pipeline(dev, scene_tex_sub_pipe);
        if (scene_tex_add_pipe) rhi_destroy_pipeline(dev, scene_tex_add_pipe);
        if (scene_tex_qadd_pipe) rhi_destroy_pipeline(dev, scene_tex_qadd_pipe);
        if (gouraud_qadd_pipe) rhi_destroy_pipeline(dev, gouraud_qadd_pipe);
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
    openpete_native_publish_fullscreen();
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
    s_nr.prepass_depth_pipe = prepass_depth_pipe;
    s_nr.prepass_tex_pipe = prepass_tex_pipe;
    s_nr.prepass_vb   = prepass_vb;
    s_nr.terrain_key_pipe = terrain_key_pipe;
    s_nr.terrain_key_tex_pipe = terrain_key_tex_pipe;
    s_nr.terrain_key_vb   = terrain_key_vb;
    s_nr.moby_rank_pipe = moby_rank_pipe;
    s_nr.moby_rank_cutout_pipe = moby_rank_cutout_pipe;
    s_nr.gouraud_add_pipe = gouraud_add_pipe;
    s_nr.gouraud_noz_pipe = gouraud_noz_pipe;
    s_nr.portal_mask_pipe = portal_mask_pipe;
    s_nr.gouraud_mark_pipe = gouraud_mark_pipe;     /* may be NULL -> no hole-fill */
    s_nr.scene_tex_mark_pipe = scene_tex_mark_pipe; /* may be NULL -> no hole-fill */
    s_nr.gouraud_fill_pipe = gouraud_fill_pipe;     /* may be NULL -> no hole-fill */
    s_nr.scene_tex_pipe = scene_tex_pipe;
    s_nr.scene_tex_blend_pipe = scene_tex_blend_pipe;
    s_nr.scene_tex_add_pipe = scene_tex_add_pipe;
    s_nr.scene_tex_qadd_pipe = scene_tex_qadd_pipe;
    s_nr.gouraud_qadd_pipe = gouraud_qadd_pipe;
    {
        const nr_cover_src cover_src[] = {
            { &gouraud_blend_desc, gouraud_blend_pipe, 0 }, { &gouraud_add_desc, gouraud_add_pipe, 0 },
            { &gouraud_sub_ztest_desc, gouraud_sub_ztest_pipe, 0 }, { &gouraud_qadd_desc, gouraud_qadd_pipe, 0 },
            { &scene_tex_blend_desc, scene_tex_blend_pipe, 0 }, { &scene_tex_add_desc, scene_tex_add_pipe, 0 },
            { &scene_tex_sub_desc, scene_tex_sub_pipe, 0 }, { &scene_tex_qadd_desc, scene_tex_qadd_pipe, 0 },
            { &hd_scene_blend_desc, hd_scene_blend_pipe, 0 }, { &hd_scene_add_desc, hd_scene_add_pipe, 0 },
            { &gouraud_desc, gouraud_pipe, 1 }, { &gouraud_mark_desc, gouraud_mark_pipe, 1 },
            { &scene_tex_desc, scene_tex_pipe, 1 }, { &scene_tex_mark_desc, scene_tex_mark_pipe, 1 },
            { &hd_cutout_desc, hd_cutout_pipe, 1 }, { &hd_cutout_mark_desc, hd_cutout_mark_pipe, 1 },
            { &gouraud_fill_desc, gouraud_fill_pipe, 1 },
        };
        s_nr.cover_twin_n = cover_family_create(dev, cover_src,
                                                (int)(sizeof cover_src / sizeof cover_src[0]),
                                                s_nr.cover_twins);
    }
    s_nr.scene_tex_sub_pipe = scene_tex_sub_pipe;
    s_nr.scene_tex_sub_decal_pipe = scene_tex_sub_decal_pipe; /* may be NULL -> plain sub pipe */
    memcpy(s_nr.gouraud_hud_pipes, gouraud_hud_pipes, sizeof gouraud_hud_pipes);
    memcpy(s_nr.scene_tex_hud_pipes, scene_tex_hud_pipes, sizeof scene_tex_hud_pipes);
    s_nr.hd_tex_pipe  = hd_tex_pipe;
    s_nr.hd_cutout_pipe = hd_cutout_pipe;
    s_nr.hd_cutout_mark_pipe = hd_cutout_mark_pipe;
    s_nr.gouraud_punch_pipe = gouraud_punch_pipe;       /* may be NULL -> front pass draws in place */
    s_nr.scene_tex_punch_pipe = scene_tex_punch_pipe;
    s_nr.hd_cutout_punch_pipe = hd_cutout_punch_pipe;
    s_nr.hd_scene_blend_pipe = hd_scene_blend_pipe;
    s_nr.hd_scene_add_pipe = hd_scene_add_pipe;
    /* Material draws bind op_pack_tex (set 1) unconditionally - a shader that
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
    s_nr.freeze_plain_pipe = freeze_plain_pipe;
    s_nr.resolve_pipe = resolve_pipe;
    s_nr.fs_quad_vb   = fs_quad_vb;
    s_nr.overlay_vb   = overlay_vb;
    s_nr.mod_ovl_vb   = mod_ovl_vb;
    /* Optional menu overlay buffer (init has already passed the fatal guard -
     * a NULL here just disables the menu draw, it doesn't fail the renderer). */
    s_nr.menu_vb      = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * MENU_MAX_VERTS);
    s_nr.menu_glyph_vb= rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * MENU_GLYPH_MAX);
    s_nr.ovl_resolve_pipe = ovl_resolve_pipe;   /* may be NULL -> 1× fallback */
    s_nr.ovl_quad_vb  = ovl_quad_vb;            /* may be NULL -> 1× fallback */
    s_nr.menu_quad_vb = menu_quad_vb;           /* may be NULL -> 1× fallback */
    s_nr.osd_glyph_vb = osd_glyph_vb;           /* may be NULL -> no OSD layer  */
    s_nr.osd_quad_vb  = osd_quad_vb;
    s_nr.osd_panel_vb = osd_panel_vb;
    s_nr.occl_pipe    = occl_pipe;              /* may be NULL -> no moby occlusion culling */
    s_nr.occl_grid    = occl_grid;              /* may be NULL -> no moby occlusion culling */
    s_nr.vram_tex     = vram_tex;
    s_nr.drain_tex    = drain_tex;
    s_nr.vram_mirror  = mirror;
    /* Subscribe to PsyCross VRAM writes (dirty-rect tracked mirror) and
     * install the native op implementations (verified via
     * OPENPETE_VRAM_VERIFY against PsyCross's mirror). */
    PsyX_SetVRAMDirtyCallback(nr_vram_dirty_cb);
    openpete_vram_install_ops();

    /* Static full-target quad (clip -1..1, UV 0..1 top-down) for render-target
     * copies - currently the native_rtt -> frozen_tex freeze-snapshot blit. */
    {
        rhi_vertex fsq[6];
        build_panel(fsq, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
        rhi_update_buffer(fs_quad_vb, fsq, sizeof(fsq));
    }
    /* Terrain sector-mesh GPU cache (default ON; --terrain-cache 0 is the
     * kill switch - it skips the ~130 MB device-memory arenas too). Software
     * vk never draws the retained path (the usable() exclusion), so don't
     * spend the arenas there either - except under the FORCE waiver the
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
     * in the create call or in any GPU zone - it surfaces as ~90 ms inside
     * the next fence wait, which is past the pacer's slip clamp. Paying it
     * during bring-up costs the same time where no deadline is running. */
    if (openpete_native_hdfold_on()) openpete_native_hdfold_prime(dev);
    rhi_depth_report(dev);

    OPL_INFO_F("native", "Vulkan vk window up (%dx%d, device='%s'); PsyCross hidden",
               w, h, rhi_device_name(dev));
    return 0;
}

/* Retained terrain path availability; documented at the declaration. */
int openpete_native_tercache_usable(void)
{
    /* OPENPETE_TERRAIN_CACHE_FORCE=1: waive the software-GPU exclusion (dev -
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
        && !scene_box_armed()
        /* fp64 in terrain.vert is what makes the mirror bit-exact vs the
         * CPU walk's double math - no fp64, no retained path. */
        && rhi_device_has_float64(s_nr.dev)
        && (s_force || !rhi_device_is_software(s_nr.dev));
}

void openpete_native_tercache_service_headless(void)
{
    /* The service's windowed site is the present entry, which headless never
     * reaches: both entries into the render frame return early on
     * s_nr.headless, so the host loop drives it here instead, once per VBL.
     * The headless predicate keeps exactly one of the two sites live in any
     * one mode, so the device is never serviced from two threads. */
    if (!s_nr.headless) return;
    if (!openpete_native_tercache_usable()) return;
    openpete_tercache_present_service();
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

/* Forwarded rather than exposing the device; presenting thread only. */
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

/* A false begin_frame is a normal outcome, not an error. */
int openpete_native_render_repeat_last(void)
{
    /* The present thread owns the device while eager production is active;
     * a repeat present from any other thread would race it on the swapchain
     * and queue. Refuse rather than fault - under eager the display is being
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

/* The native window's fullscreen state as the window-owning thread last saw
 * it: set by the toggle to the state it requested and by the owner on every
 * SDL enter/leave-fullscreen event. Other threads read this instead of the SDL
 * window flags, which the owner's event pump writes. */
static SDL_AtomicInt s_nr_fullscreen;

void openpete_native_publish_fullscreen(void)
{
    if (!s_nr.window) return;
    SDL_SetAtomicInt(&s_nr_fullscreen,
                     (SDL_GetWindowFlags(s_nr.window) & SDL_WINDOW_FULLSCREEN) != 0);
}

/* Fullscreen toggle; documented at the declaration. */
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
    SDL_SetAtomicInt(&s_nr_fullscreen, !fs);
    OPL_INFO_F("native", "fullscreen %s", !fs ? "on (borderless desktop)" : "off");
    /* Announced HERE rather than at the call sites: Alt+Enter, the fullscreen
     * binding, a window double-click, the O-menu DISPLAY row and the M-overlay
     * row all funnel through this one function, and the toast should read the
     * same whichever of them the player used. */
    openpete_osd_notify("host", "menu.display", 0,
                        "DISPLAY: %s", !fs ? "FULLSCREEN" : "WINDOWED");
}

SDL_Window* openpete_native_render_window(void)
{
    return s_nr.enabled ? s_nr.window : NULL;
}

/* Reads the state the window-owning thread published. */
int openpete_native_fullscreen_active(void)
{
    if (!s_nr.enabled || !s_nr.window) return 0;
    return SDL_GetAtomicInt(&s_nr_fullscreen);
}

/* The display/clip rects arrive from the guest's PutDispEnv/PutDrawEnv on the
 * thread running the guest; the compose reads s_nr.disp_* and clip_* on the
 * thread presenting, which may be another one. The setters therefore write
 * a guest-side staging copy, and every compose entry copies its source's
 * rects into s_nr at its start (ps_apply_geometry): a scene composed from
 * the ring carries the rects that were current when it was extracted. */
static int s_geom_in[8];   /* disp x,y,w,h, clip x,y,w,h - guest-side */

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
    /* game_layout.display is a claim about the game, read from its source and
     * used before boot to size the default window; these values are the truth.
     * A disagreement means the constant is wrong for this build or this game
     * changes its display environment at runtime. The window is not resized:
     * the present blit fits the runtime clip either way, so the only symptom
     * is bars. Once per run. */
    static int s_clip_checked = 0;
    if (s_nr.enabled && !s_clip_checked && w > 0 && h > 0) {
        s_clip_checked = 1;
        if (h != game_layout.display.clip_h ||
            s_geom_in[2] != game_layout.display.disp_w ||
            s_geom_in[3] != game_layout.display.disp_h)
            OPL_WARN_F("native",
                       "display env %dx%d disp / %d clip rows disagrees with the "
                       "game layout %dx%d disp / %d clip rows; the default window "
                       "was sized from the layout and may letterbox",
                       s_geom_in[2], s_geom_in[3], h,
                       game_layout.display.disp_w, game_layout.display.disp_h,
                       game_layout.display.clip_h);
    }
    s_geom_in[4] = x; s_geom_in[5] = y; s_geom_in[6] = w; s_geom_in[7] = h;
}

/* ---- native VRAM model: dirty-rect tracked mirror ------------------------
 * PsyCross notifies every write to its CPU VRAM mirror (LoadImage/MoveImage/
 * ClearImage, DR_LOAD/DR_MOVE, framebuffer copyback, savestate restore) via
 * PsyX_SetVRAMDirtyCallback with the effective post-clamp rect. We accumulate
 * the rects, refresh ONLY those regions of our own mirror at present time,
 * and upload only those regions (rhi_update_vram_rects) - replacing the old
 * full 1 MiB GR_ReadVRAM copy + full-texture upload every present. The list
 * overflows to a full-frame refresh (correct, just slower). The callback runs
 * on the game thread synchronously with the write; the loop is
 * single-threaded (game fiber + present interleave), so no locking. */
#define NR_VRAM_MAX_RECTS 64
static rhi_rect s_vram_rects[NR_VRAM_MAX_RECTS];
static int      s_vram_nrects  = 0;
static bool     s_vram_full    = true;    /* first sync = full snapshot */

/** @brief Accumulate a VRAM write rectangle reported by PsyCross.
 *
 * The rectangle is clamped to the 1024x512 mirror and dropped when empty or
 * when a full refresh is already pending. A 65th rectangle in one interval
 * turns the pending refresh into a full one.
 * @param x Left edge.
 * @param y Top edge.
 * @param w Width.
 * @param h Height. */
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
/* The auxiliary VRAM bank: columns NR_VRAM_AUX_X..1023 of the texture rows
 * NR_VRAM_H..2*NR_VRAM_H-1, uploaded once per set and kept for the session. */
#define NR_VRAM_AUX_X 512
static uint16_t *s_vram_aux;              /* host copy, row-major, (1024 - AUX_X) wide */
static bool s_vram_aux_pending;           /* set; upload at the next frame */

/** @brief Free the oldest VRAM journal entry's texel buffer and drop the entry from the ring.
 *
 * The caller holds the VRAM lock and guarantees the ring is not empty. */
static void vram_journal_pop(void)
{
    vram_jentry_t *e = &s_vj[s_vj_head];
    free(e->pix); e->pix = NULL; e->pix_bytes = 0;
    s_vj_head = (s_vj_head + 1) % VRAM_JOURNAL; s_vj_n--;
}
/** @brief Record one VRAM journal entry's upload into the current frame's command buffer.
 *
 * A full entry uploads the whole mirror; any other entry uploads its packed
 * rect texels. The caller holds the VRAM mutex.
 * @param e Journal entry. */
static void vram_journal_apply(const vram_jentry_t *e)
{
    if (e->full) {   /* the mirror covers the PSX rows only, not the auxiliary bank */
        const rhi_rect all = { 0, 0, NR_VRAM_W, NR_VRAM_H };
        rhi_update_vram_rects(s_nr.dev, s_nr.vram_tex, s_nr.vram_mirror, &all, 1);
    } else if (e->nrects > 0 && e->pix)
        rhi_update_vram_rects_packed(s_nr.dev, s_nr.vram_tex, e->pix, e->pix_bytes, e->rects, e->nrects);
}

/* sync (guest side: drains the dirty rects, refreshes the mirror) and upload
 * (present side: records the mirror rows into the frame) may run on two
 * threads; the mirror ROWS are the payload, so the lock spans both bodies. */
static void *s_vram_mx;   /* SDL_Mutex, openpete_mutex_once */
/** @brief The VRAM mirror lock, created on first use without a creation race. */
static SDL_Mutex *vram_mx(void) { return openpete_mutex_once(&s_vram_mx); }
static void sync_vram_texture_locked(void);
/** @brief Drain the dirty VRAM rects into the mirror under the VRAM lock (sync_vram_texture_locked). */
static void sync_vram_texture(void)
{
    SDL_LockMutex(vram_mx());
    sync_vram_texture_locked();
    SDL_UnlockMutex(vram_mx());
}
/* Guest-side entry for the eager producer: refresh the mirror while the
 * guest is idle so no VRAM write can land mid-drain. */
void openpete_native_vram_sync(void) { if (s_nr.enabled && !s_nr.headless) sync_vram_texture(); }
/** @brief Refresh the VRAM mirror from PsyCross's VRAM and journal the dirty regions.
 *
 * Runs the OPENPETE_VRAM_CENSUS, OPENPETE_VRAM_VERIFY and OPENPETE_VRAM_PEEK
 * instruments first. Returns when no region is dirty. A full journal ring pops
 * its oldest entry and forces a full refresh. A full refresh reads the whole of
 * VRAM into the mirror and journals a full entry. Otherwise each dirty rect is
 * read into the mirror and its texels are copied, 4-byte aligned, into one
 * malloc'd buffer owned by the entry; a failed allocation degrades the entry to
 * a full upload. The caller holds the VRAM mutex. */
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
    /* OPENPETE_VRAM_VERIFY: before the shadow copy overwrites the
     * model with PsyCross's bytes, memcmp every op-written rect (exact-rect
     * match against the ops' post-clamp log) - proves the native op
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
     * the native mirror for a rect (fog palette forensics). */
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
     * in scene order (staging-ring upload, per frame). */
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

/* A framebuffer-window resolve composes the scene its copy names, which the
 * game drew before the copy overwrote the destination page; the game reuses
 * that page as scratch while the world is frozen, so the scene's textures and
 * CLUTs may lie in it. While a resolve is armed (s_vram_under) every inline
 * upload puts the overwritten texels back over the rect after the journal;
 * the first keeps the texels the GPU held there (s_vram_restore), which the
 * first upload after the resolve puts back before the journal applies. */
static struct { const uint16_t *pix; rhi_rect r; } s_vram_under;
static struct { uint16_t *pix; rhi_rect r; } s_vram_restore;

/** @brief Upload the armed window-resolve texels over their rect, keeping the texels they replace first.
 *
 * Called by upload_synced_textures after an inline present applied the whole
 * journal, so the mirror rows of the rect are what the GPU holds. The caller
 * holds the VRAM mutex. A failed allocation of the kept copy skips the
 * override, so the GPU never loses the texels it held. */
static void vram_under_apply(void)
{
    const rhi_rect r = s_vram_under.r;
    if (!s_vram_restore.pix) {
        uint16_t *keep = (uint16_t *)malloc((size_t)r.w * (size_t)r.h * sizeof *keep);
        if (!keep) return;
        for (int row = 0; row < r.h; row++)
            memcpy(keep + (size_t)row * r.w,
                   s_nr.vram_mirror + (size_t)(r.y + row) * NR_VRAM_W + r.x,
                   (size_t)r.w * sizeof *keep);
        s_vram_restore.pix = keep; s_vram_restore.r = r;
    }
    rhi_update_vram_rects_packed(s_nr.dev, s_nr.vram_tex, (const uint8_t*)s_vram_under.pix,
                                 (size_t)r.w * (size_t)r.h * 2u, &r, 1);
}

/** @brief Record the pending VRAM journal entries and the pending reference image into the current frame's command buffer.
 *
 * Journal entries are applied in order up to and including the scene being
 * composed (s_ps_slot's scene id); an inline present, which has no ring
 * scene, applies all of them. A held-back head is logged (at most 20
 * lines). The reference image is uploaded when sync_reference_texture left
 * it pending. Runs under the VRAM mutex. Call right after a successful
 * rhi_begin_frame and before the frame's first render pass, since the rhi
 * appends transfer commands and barriers to the frame's command buffer
 * (rhi_update_vram's contract in rhi.h). */
static void upload_synced_textures(void)
{
    /* Up to and including the scene being composed; everything for an inline
     * present, which has no scene of its own in the ring. */
    const uint32_t upto = s_ps_slot ? s_ps_slot->scene_id : UINT32_MAX;
    SDL_LockMutex(vram_mx());
    if (s_vram_restore.pix && !s_vram_under.pix) {   /* a window resolve is over */
        rhi_update_vram_rects_packed(s_nr.dev, s_nr.vram_tex, (const uint8_t*)s_vram_restore.pix,
                                     (size_t)s_vram_restore.r.w * (size_t)s_vram_restore.r.h * 2u,
                                     &s_vram_restore.r, 1);
        free(s_vram_restore.pix); s_vram_restore.pix = NULL;
    }
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
    if (s_vram_under.pix && !s_ps_slot) vram_under_apply();
    if (s_ref_dirty) {
        rhi_update_rgba8(s_nr.dev, s_nr.ref_tex, s_nr.readback);
        s_ref_dirty = false;
    }
    if (s_vram_aux_pending && s_vram_aux) {
        const rhi_rect r = { NR_VRAM_AUX_X, NR_VRAM_H, NR_VRAM_W - NR_VRAM_AUX_X, NR_VRAM_H };
        rhi_update_vram_rects_packed(s_nr.dev, s_nr.vram_tex, (const uint8_t*)s_vram_aux,
                                     sizeof(uint16_t) * (size_t)r.w * (size_t)r.h, &r, 1);
        s_vram_aux_pending = false;
    }
    SDL_UnlockMutex(vram_mx());
}

void openpete_native_vram_aux_set(const uint8_t* image, size_t bytes)
{
    const size_t need = sizeof(uint16_t) * (size_t)(NR_VRAM_W - NR_VRAM_AUX_X) * NR_VRAM_H;
    if (!image || bytes < need) return;
    uint16_t* copy = (uint16_t*)malloc(need);
    if (!copy) return;
    memcpy(copy, image, need);
    SDL_LockMutex(vram_mx());
    free(s_vram_aux);
    s_vram_aux = copy;
    s_vram_aux_pending = true;
    SDL_UnlockMutex(vram_mx());
}

/** @brief Read PsyCross's finished image from its present FBO into the host readback buffer and mark it for upload.
 *
 * On a size change the reference texture ref_tex and the readback buffer
 * are recreated at the new size, after a wait for idle. The upload into
 * ref_tex is deferred to upload_synced_textures, which records it inside a
 * frame.
 * @return true when a reference image is ready to draw this present; false
 *         when PsyCross has produced no scene yet, an allocation failed, or
 *         the read failed. */
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

/** @brief Create or resize native_rtt and frozen_tex at the reference resolution.
 *
 * native_rtt holds the canonical native image of each present: the diff
 * pass compares it and the live vk and ab panels blit from it. frozen_tex
 * holds the last world image for the pause, inventory and flight freeze
 * backdrop; a resize clears frozen_valid. No-op until the reference size is
 * known or while both already match it. Creates GPU resources and waits
 * idle, so it runs outside a frame.
 * @param want Allocation gate; every caller passes true, since the
 *        canonical-image model needs both targets in every mode. */
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

/** @brief Create or resize the widescreen screenshot target.
 *
 * The target is recreated only when the requested size changes, so a
 * widescreen capture at a fixed cadence allocates once and every later dump
 * reuses it. native_rtt cannot serve, since it is keyed to the reference
 * width and a widened width would recreate it on every capture. Creates GPU
 * resources and waits idle, so it runs outside a frame.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @return The target, or NULL when a dimension is not positive or the
 *         allocation failed. */
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

/** @brief Create, resize or release ss_rtt, the SSAA scene target, at the effective factor times the window.
 *
 * The requested --ssaa factor is capped to OPENPETE_SSAA_MAX and to the
 * GPU's maximum 2D image dimension, then stepped down on each allocation
 * failure; at 1 SSAA is off and ss_factor is 1. With --ssaa 1 or an unknown
 * window size an existing target is released. No-op when the target already
 * matches. Creates GPU resources and waits idle, so it runs outside a frame.
 *
 * The scene target is one fifth of what the factor costs: the four
 * key-contest targets are allocated at this same extent
 * (ensure_terrain_key_targets). */
static void ensure_ss_rtt(void)
{
    int req = (s_nr.ssaa > 1) ? s_nr.ssaa : 1;
    if (req > OPENPETE_SSAA_MAX) req = OPENPETE_SSAA_MAX;   /* VRAM ceiling (openpete.h) */
    int win_w = s_nr.win_w, win_h = s_nr.win_h;
    if (req <= 1 || win_w <= 0 || win_h <= 0) {              /* SSAA off -> drop any RT */
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
        s_nr.ss_w = s_nr.ss_h = 0; s_nr.ss_factor = 1;   /* couldn't allocate -> SSAA off */
    }
}

/** @brief Rebuild the panel vertex buffer and the scene mapping when the window, reference or display size changes.
 *
 * The layout is one full-window cell. Panel 0 is the native scene's rect:
 * the Hor+ widen factor ws_factor comes from a fixed --aspect, or under AUTO
 * from the cell shape so the render rect fills the window, clamped to at
 * least 1. The 4:3 content sub-rect (p0_cx, p0_hx, p0_hy), which
 * scene_to_clip maps the 512-wide screen onto, is 1/ws_factor of the render
 * rect's width and centred; the panel 0 quad samples the VRAM texture in
 * texel units over disp_w by clip_h (the whole VRAM before the display
 * environment is set) as the loading-screen fallback. Panel 1, built only
 * when no native scene is live (--renderer psycross), is PsyCross's
 * readback aspect-fit into the window, with normalised, bottom-up UVs.
 * The vertex buffer is rewritten after a wait for idle, so the call runs
 * outside a frame. No-op until the window and reference sizes are known or
 * while the cached sizes match. */
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

    /* Panel 0 - native render of the PSX display region, sampled from the VRAM
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
         * CELL shape so the render rect exactly fills the window - re-derived
         * here on every size change (resize/fullscreen), since the layout
         * change-cache keys on the window size. fit_w/fit_h is the crop-
         * corrected 4:3 display shape, so the AUTO ratio is exact regardless
         * of the overscan crop. Clamp ≥1 in both modes - Hor+ only ADDS
         * world; a narrower target just renders 4:3 and the window
         * letterboxes. */
        float wf = 1.0f;
        int wf_auto_fill = 0;   /* AUTO derived from the cell -> rect == cell */
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
         * window -> letterbox/pillarbox bars where the window ≠ the fixed
         * --aspect (under AUTO the rect is the window shape -> no bars). The
         * 4:3 CONTENT sub-rect (what scene_to_clip maps the canonical
         * 512-wide screen onto, unstretched) is 1/wf of that width and
         * centred; the Hor+ reveal fills the rest of the render rect. */
        float cx = -1.0f + chx, hx, hy;
        if (wf_auto_fill) {
            /* AUTO: wf was derived so the render rect IS the cell - set it
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
    /* Panel 1 - PsyCross's read-back frame, the display source when no native
     * scene is produced (--renderer psycross). Aspect-fit into the whole
     * window, so a window that is not the reference shape letterboxes rather
     * than stretching. RGBA8 sampled with texture(), so UVs are NORMALIZED,
     * and the GL readback is bottom-up, so v runs 1 at the top to 0 at the
     * bottom - both differ from panel 0, which addresses the VRAM texture in
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

/** @brief Map a PSX screen point to clip space for the current render target.
 *
 * The symmetric overscan band is cropped: y is normalised over the clip_h
 * visible lines, so the native scene lines up with the overscan-cropped
 * reference. Under s_render_full_extent the target is the whole image and
 * x maps to [-1, 1] divided by s_render_freeze_wf, which keeps the 4:3
 * content centred in a widened freeze capture; otherwise the point maps
 * into panel 0's 4:3 sub-rect (p0_cx, p0_hx, p0_hy). Unset display sizes
 * default to 512 by 240 with 224 visible lines.
 * @param sx Screen x, 0 to disp_w.
 * @param sy Screen y, 0 to disp_h.
 * @param cx Receives clip x.
 * @param cy Receives clip y, positive up. */
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
         * (wf==1 -> the plain 4:3 full-extent for native_rtt / diff). */
        *cx = (-1.0f + nx * 2.0f) / s_render_freeze_wf;
        *cy =  1.0f - ny * 2.0f;        /* screen y down -> clip y up */
    } else {
        *cx = (s_nr.p0_cx - s_nr.p0_hx) + nx * (2.0f * s_nr.p0_hx);
        *cy = (0.0f + s_nr.p0_hy)       - ny * (2.0f * s_nr.p0_hy);
    }
}

/** @brief Return the moby value a scene face carries in its rank pair's second channel.
 *
 * An unranked opaque actor face (category ACTOR, rank sentinel -1 or -1.25)
 * carries its moby identity (openpete_scene_tri.mid), the owner the key
 * contest compares against the actor key image's owner colour
 * (psx_key_contest.glsl op_akey_owner); every other face carries its
 * present-local moby ordinal, the rank select's. */
static inline float scene_rank_moby(const scene_tri_t *st)
{
    const int actor = st->category == OPENPETE_SCENE_ACTOR &&
                      st->rank < -0.5f && st->rank > -1.5f;
    return actor ? (float)st->mid : (float)st->mordinal;
}

/** @brief Pack scene vertex k of st into o: clip-space position through scene_to_clip, depth, the given UV, colour, key offset, moby paint pair and drain fields.
 *
 * The one vertex layout every scene pass of draw_native_scene and the depth
 * pre-pass upload.
 * @param o Destination vertex.
 * @param st Scene triangle.
 * @param k Vertex index, 0 to 2.
 * @param u Texture U, 0 for an untextured pass.
 * @param v Texture V, 0 for an untextured pass. */
static inline void pack_scene_vert(rhi_vertex *o, const scene_tri_t *st, int k, float u, float v)
{
    float cx, cy;
    scene_to_clip(st->v[k].sx, st->v[k].sy, &cx, &cy);
    o->x = cx; o->y = cy; o->z = st->v[k].z; o->w = st->v[k].pvz;
    o->u = u; o->v = v;
    o->rgba = st->v[k].rgba; o->koff = st->koff;
    o->sx = st->rank; o->sy = scene_rank_moby(st);
    rv_drain(o, st, k);
}

/** @brief Pack the three vertices of st into o[0..2] with UV 0 (pack_scene_vert). */
static inline void pack_scene_tri(rhi_vertex *o, const scene_tri_t *st)
{
    for (int k = 0; k < 3; k++) pack_scene_vert(&o[k], st, k, 0.0f, 0.0f);
}

/* The current pass's inverse of scene_to_clip, composed with its viewport:
 * a fragment at gl_FragCoord (x,y) sits at PSX screen (fax*x + fbx,
 * fay*y + fby). scene_to_clip is affine in (sx,sy), and every pass begin sets
 * an origin-0, Y-flipped viewport of the whole render area, so the window
 * transform is x = W*(cx+1)/2, y = H*(1-cy)/2 and the composition inverts
 * exactly under any aspect, widescreen factor or SSAA factor. The mapping is
 * Probed, not re-derived, so full-extent and panel passes cannot drift apart.
 *
 * This is how the shadow decals recover the screen coord their pre-pass lookup
 * needs. Carrying it as an interpolated vertex attribute instead put the
 * lookup on a neighbouring texel wherever the rasterizer's interpolation of
 * that attribute disagreed with the fragment's own position (clipped slivers),
 * and the decal then read a neighbour's surface normal. */
typedef struct { float fax, fbx, fay, fby; } scene_fc_affine;
/** @brief Fill o with the map from gl_FragCoord to PSX screen coordinates for this pass.
 *
 * Probes scene_to_clip at (0,0) and (1,1) for the clip units per screen pixel
 * and the origin, and reads the pass extent from rhi_pass_extent. A zero probe
 * delta counts as 1 and a zero extent as 1 pixel, which yields a finite but
 * meaningless map.
 * @param o Receives fax, fbx, fay, fby such that screen x = fax * fragx + fbx
 *        and screen y = fay * fragy + fby. */
static void scene_fragcoord_affine(scene_fc_affine *o)
{
    float cx0, cy0, cx1, cy1;
    scene_to_clip(0.0f, 0.0f, &cx0, &cy0);
    scene_to_clip(1.0f, 1.0f, &cx1, &cy1);
    int vw = 0, vh = 0;
    rhi_pass_extent(s_nr.dev, &vw, &vh);
    float ax = cx1 - cx0, ay = cy1 - cy0;   /* clip units per PSX screen pixel */
    float w  = vw > 0 ? (float)vw : 1.0f;
    float h  = vh > 0 ? (float)vh : 1.0f;
    if (ax == 0.0f) ax = 1.0f;
    if (ay == 0.0f) ay = 1.0f;
    o->fax =  2.0f / (w * ax); o->fbx = (-1.0f - cx0) / ax;
    o->fay = -2.0f / (h * ay); o->fby = ( 1.0f - cy0) / ay;
}

/** @brief Fill the terrain cache's screen-to-clip parameters with the raw inputs of the current scene_to_clip mapping.
 *
 * terrain.vert replicates the CPU expression term for term from these
 * inputs; a probed or fitted affine loses about 5 subpixel LSBs at the
 * screen edges, enough to flip rasterized edge pixels against the CPU-fed
 * draws. The function reads the state scene_to_clip reads (display sizes,
 * panel 0 rect, s_render_full_extent, s_render_freeze_wf), so the two
 * mappings cannot drift apart. The pass's gl_FragCoord -> PSX screen affine
 * (scene_fragcoord_affine) rides every draw for the inner-rim discard; the
 * blended cover flag is cleared, and the blended draws set it
 * (bw_draw_tercache).
 * @param out Receives the parameters. */
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
    out->cover = 0;
    scene_fc_affine fa;
    scene_fragcoord_affine(&fa);
    out->fc[0] = fa.fax; out->fc[1] = fa.fbx; out->fc[2] = fa.fay; out->fc[3] = fa.fby;
}

/* ---- depth-aware shadow decal: the shared WORLD-opaque depth pre-pass ------ */

/* Camera reconstruction params from the game (openpete_native_render.h); the
 * shadow feeder refreshes them every extract (incl. the sub-tick re-extract, so
 * they track the interpolated camera). Each ring slot snapshots them at
 * publish; the delayed history does not, so a compose of it reads the live
 * copy: a 1-present staleness moves the reconstruction by the camera's
 * per-present delta (a few world units), far under the wall_height threshold. */
static openpete_shadow_decal_params s_shadow_decal;
/* Documented at the declaration. */
void openpete_native_set_shadow_decal_params(const openpete_shadow_decal_params *p)
{
    if (p) s_shadow_decal = *p; else s_shadow_decal.valid = 0;
}

/* The player fan's footprint block (openpete_native_render.h). Only a player
 * shadow feed emits the untextured SHADOW triangles that read it, and that
 * feed writes it in the same call, so a scene never pairs its hull with
 * another extraction's fan. Snapshotted into each scene slot. */
static openpete_shadow_footprint s_shadow_foot;
/* Documented at the declaration. */
void openpete_native_set_shadow_footprint(const openpete_shadow_footprint *f)
{
    if (f) s_shadow_foot = *f;
}

/* Per-present pre-pass state consumed by draw_native_scene's SUBTRACT group:
 * whether the pre-pass rendered this present, the raster extent inside the
 * (grow-only) opaque_depth allocation, and the PSX-screen->uv affine of the
 * full-extent mapping the pre-pass rasterized with. */
static int   s_od_ready = 0;
static float s_od_vw = 0.0f, s_od_vh = 0.0f;
static float s_od_uax = 0.0f, s_od_ubx = 0.0f, s_od_vay = 0.0f, s_od_vby = 0.0f;
static openpete_tercache_clip s_od_taff;   /* terrain-cache prepass clip params */

/** @brief Return the fallback view-Z band for the shadow decal's mode-0 path.
 *
 * shadow_decal.frag uses the band only when the camera reconstruction
 * params are invalid; the height test does not read it.
 * OPENPETE_SHADOW_BAND overrides the default of 256; read once and cached.
 * @return The band in view-Z units. */
static float shadow_decal_band(void)
{
    static float b = -1.0f;
    if (b < 0.0f) { const char* e = getenv("OPENPETE_SHADOW_BAND"); b = (e && *e) ? (float)atof(e) : 256.0f; }
    return b;
}

/** @brief Return whether moby drop shadows draw through the depth-aware textured decal.
 *
 * On by default. OPENPETE_MOBY_SHADOW_DECAL=0 selects the plain
 * depth-tested subtract pipe with hardware depth bias, for an A/B of the
 * decal against the flat shadow fan, which clips into slopes. The value is
 * read once. Display-only.
 * @return 1 when the decal path is enabled, else 0. */
static int moby_shadow_decal_enabled(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_SHADOW_DECAL"); v = !(e && *e && *e == '0'); }
    return v;
}

/** @brief Whether OPENPETE_DEPTH_PREPASS_DEBUG is set; a debug knob, read once. */
static int depth_prepass_debug(void) {
    static int on = -1;
    if (on < 0) on = getenv("OPENPETE_DEPTH_PREPASS_DEBUG") != NULL;
    return on;
}

/** @brief The shadow decal shader mode: OPENPETE_SHADOW_DECAL_MODE when set, else dflt.
 *
 * A live registry row, so the value is re-read when the registry env epoch
 * changes and never per bind.
 * @param dflt The mode the feeder's camera parameters select.
 * @return The mode. */
static float shadow_decal_mode(float dflt) {
    static unsigned seen = 0;
    static int have = 0;
    static float v = 0.0f;
    if (knob_env_epoch_stale(&seen)) {
        const char* mv = getenv("OPENPETE_SHADOW_DECAL_MODE");
        have = (mv && *mv) ? 1 : 0;
        v = have ? (float)atof(mv) : 0.0f;
    }
    return have ? v : dflt;
}

/** @brief Return whether moby occlusion culling is enabled.
 *
 * On by default; OPENPETE_MOBY_OCCLUSION=0 disables it. The variable is
 * re-read when the registry env epoch changes (knob_env_epoch_stale), since
 * a live registry row writing the environment is the live channel and the
 * function runs per moby per present.
 * Display-only: culling skips only the native re-extraction and drawing of
 * mobys whose screen rectangle lies behind nearer WORLD-opaque depth; the
 * canonical cull pass and every gameplay-readable byte it writes are
 * untouched.
 * @return 1 when enabled, else 0. */
static int occl_cull_enabled(void)
{
    /* Re-resolved when the registry env epoch moves: a live row's env write
     * is the live channel (dev overlay) and this is called per moby per
     * present - too hot for getenv. */
    static int on = 1;
    static unsigned seen = 0;
    if (knob_env_epoch_stale(&seen)) {
        const char* e = getenv("OPENPETE_MOBY_OCCLUSION");
        on = !(e && *e == '0');
    }
    return on;
}
/** @brief Return whether moby occlusion culling can run this present.
 *
 * @return Nonzero when occl_pipe and occl_grid were created and
 *         occl_cull_enabled is on. */
static int occl_active(void)
{
    return s_nr.occl_pipe && s_nr.occl_grid && occl_cull_enabled();
}

/** @brief Test whether a scene carries a Spyro silhouette shadow.
 *
 * Mirrors the untextured subtractive group's membership test: an
 * untextured triangle of blend class 3 outside the HUD, HUD3D and HUD_PANEL
 * categories. With moby occlusion culling it gates the opaque_depth
 * allocation and the per-present depth pre-pass, so scenes without either
 * consumer (menus, cutscenes, shadowless scenes) allocate and draw neither.
 * @param scene Scene triangles.
 * @param scene_n Triangle count.
 * @return 1 when a shadow triangle is present, else 0. */
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

/* Per-present compose scratch, sized from the scene being composed by
 * compose_reserve. Vertex runs hold 3 vertices per scene triangle plus the
 * full-screen quads; index runs one entry per scene triangle. The two runs
 * only an armed instrument reads (s_cz_ident, s_cz_cand) are sized by that
 * instrument. Every compose
 * runs on the thread that owns the display and composes never overlap, so the
 * runs have one owner; a growth moves a run, so no pointer into one is held
 * across a compose_reserve call. */
static mod_fvec s_cz_vb, s_cz_prepass, s_cz_key[4], s_cz_ident;   /* rhi_vertex */
static mod_fvec s_cz_ktex, s_cz_txscene, s_cz_txout, s_cz_utdef,
                s_cz_bwsort, s_cz_cand;                            /* int */
static mod_fvec s_cz_bwdef;                                        /* uint8_t */
static mod_fvec s_cz_txof;                                         /* uint16_t */
static mod_fvec s_cz_hud;                                          /* hud_paint_t */
static int s_cz_tris;    /* entries every index run holds */
static int s_cz_verts;   /* vertices every vertex run and scene GPU buffer holds */
static int compose_reserve(int scene_n);
/** @brief Grow an instrument-only compose run to hold n elements, initializing it on first use.
 *
 * The run is initialized with SCENE_RUN_INIT capacity on its first call.
 * On failure mod_fvec_reserve_noted logs the refusal and the instrument
 * skips this present.
 * @param v The run.
 * @param elem Element size in bytes, used on first use.
 * @param name Census name, used on first use.
 * @param was Former fixed capacity for the census line, used on first use.
 * @param n Elements required; a negative value reserves 0.
 * @return 1 when the run holds @p n elements, 0 when it cannot. */
static int cz_instrument_run(mod_fvec *v, size_t elem, const char *name,
                             unsigned long was, int n)
{
    if (!v->elem_size) mod_fvec_init(v, elem, SCENE_RUN_INIT, name, was);
    return mod_fvec_reserve_noted(v, (size_t)(n > 0 ? n : 0)) == 0;
}

/** @brief Create or grow opaque_depth, the world-opaque depth pre-pass target, to the larger of the reference and window sizes.
 *
 * The presented pixel grid bounds the useful discard-edge precision, so the
 * SSAA extent is not covered: consumers sample by UV, and a coarser
 * pre-pass texel quantizes the wall edge by at most one window pixel. The
 * target only grows. No-op until the present scene carries a Spyro shadow
 * or moby occlusion culling is active, the two consumers of the pre-pass.
 * A device without a sampled depth target logs once and the function stays
 * a no-op for the run, leaving the Spyro shadow on the depth-tested
 * fallback. Creates GPU resources and waits idle, so it runs outside a
 * frame beside ensure_ss_rtt. */
static void ensure_opaque_depth(void)
{
    static int s_failed = 0;
    if (s_failed) return;               /* device can't sample depth - fallback pipe */
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
    rhi_set_render_target_depth_class(s_nr.opaque_depth, RHI_DEPTH_FORWARD);
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


/** @brief Pack the present scene's opaque triangles of one category into the pre-pass vertex run.
 *
 * @param scene Scene triangles.
 * @param scene_n Triangle count.
 * @param pbuf Pre-pass vertex run, s_cz_verts vertices.
 * @param m Vertices already in @p pbuf.
 * @param category OPENPETE_SCENE_WORLD or OPENPETE_SCENE_WORLD_FILL.
 * @return The vertex count after the packed triangles. */
static int prepass_pack_world(const scene_tri_t *scene, int scene_n, rhi_vertex *pbuf,
                              int m, int category)
{
    for (int t = 0; t < scene_n && m + 3 <= s_cz_verts; t++) {
        int b = scene[t].blend; if (b < 0 || b > 4) b = 0;
        if (b != 0 || scene[t].category != category) continue;   /* opaque only */
        pack_scene_tri(&pbuf[m], &scene[t]);
        m += 3;
    }
    return m;
}

static int prepass_pack_world_runs(const scene_tri_t *scene, int scene_n, rhi_vertex *out);
static void prepass_draw_world_runs(void);
static void prepass_draw_world_footprints(void);

/* The pre-pass far backstop's view-Z: 4096 * 0.999 / (1 - 0.999) in float,
 * the view-Z whose hyperbolic depth is 0.999. */
#define NR_PREPASS_BACKSTOP_VZ 0x1.f381a6p+21f
/** @brief Rasterizes the present scene's opaque WORLD geometry depth-only into opaque_depth, once per present.
 *
 * The occluders are terrain, walls and props (actors are excluded; layering
 * handles them), drawn in the full-extent PSX mapping. Every consumer pass
 * samples the result by PSX screen coordinate through the recorded affine,
 * so one raster serves native_rtt, SSAA, the offscreen panel and
 * screenshots. Records before the first consumer pass, in its own render
 * pass. Display-only. */
static void native_depth_prepass(void)
{
    s_od_ready = 0;
    if (!s_nr.opaque_depth || !s_nr.prepass_vb || !s_nr.prepass_pipe) return;
    int scene_n; uint32_t bg_, fade_, freeze_; int hud_;
    const scene_tri_t* scene = scene_present_buf(&scene_n, &bg_, &fade_, &freeze_, &hud_, NULL);
    if (depth_prepass_debug()) {
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
    scene_n = compose_reserve(scene_n);
    if (!scene_has_spyro_shadow(scene, scene_n) && !occl_active()) return;

    /* Raster extent = the ensure_opaque_depth sizing, clamped to the allocation. */
    int vw = s_nr.ref_w, vh = s_nr.ref_h;
    if (s_nr.win_w > vw) vw = s_nr.win_w;
    if (s_nr.win_h > vh) vh = s_nr.win_h;
    if (vw > s_nr.od_w) vw = s_nr.od_w;
    if (vh > s_nr.od_h) vh = s_nr.od_h;
    if (vw <= 0 || vh <= 0) return;

    /* Extract the opaque WORLD occluders' clip geometry with the FULL-EXTENT
     * mapping (positions only - the pipe writes depth, never colour).
     * WIDENED full extent under Hor+ (--aspect): the freeze_wf divide maps the
     * whole widened PSX x-band [256-256F, 256+256F] across the target, so the
     * pre-pass covers every column the widescreen view actually shows. Without
     * this the recorded affine spanned only the 4:3 [0,512] rect and the
     * occl-query offscreen reject culled edge mobys that ARE visible - on
     * sub-tick presents only (canonical never occl-culls) = the widescreen
     * edge flicker (probe-confirmed sx∈[-316,0] rejects at F=2.67). Also gives the shadow decal terrain depth at the
     * revealed edges (it samples through the same recorded affine). */
    int prev_fe = s_render_full_extent;
    float prev_wf = s_render_freeze_wf;
    s_render_full_extent = 1;
    s_render_freeze_wf = openpete_native_widescreen_factor();
    rhi_vertex *pbuf = (rhi_vertex *)s_cz_prepass.data;
    /* WORLD occluders first: untextured faces, then textured faces in
     * material runs (prepass_pack_world_runs). */
    const int mt = prepass_pack_world_runs(scene, scene_n, pbuf);
    int m = mt;
    /* Far BACKSTOP: a fullscreen quad at view-Z ≈ 4.09M (the far sentinel)
     * drawn under everything, so every pre-pass texel is
     * DEFINED. Without it, texels no WORLD tri covers (sky, cyclorama gaps)
     * hold only the pass CLEAR - and sampling never-drawn cleared depth
     * returned 0.0 (= nearest possible!) on lavapipe, making the SKY an
     * infinitely-near occluder: every moby seen against sky was culled (the
     * stone-hill tree flicker). Same driver-dependence class as the June
     * storeOp=DONT_CARE trap - make it explicit, not implicit.
     * W must stay comfortably below OPENPETE_DEPTH_VIEWZ_FAR: prepass.vert
     * writes 1 - 1/W, and at W ~4.1e7, where fp32's ulp is 4, W-1 rounds
     * back to W, NDC depth lands on EXACTLY 1.0 and every fragment fails the
     * LESS test against the 1.0 clear (a silent no-op backstop). At
     * W = 4091956.75 (the view-Z whose hyperbolic depth is 0.999), depth
     * ≈ 1-2.4e-7 < 1.0 - draws everywhere. */
    {
        static const float bq[6][2] =
            { {-1,-1}, {1,-1}, {1,1}, {-1,-1}, {1,1}, {-1,1} };
        for (int k = 0; k < 6; k++) {
            rhi_vertex* o = &pbuf[m + k];
            o->x = bq[k][0]; o->y = bq[k][1];
            o->z = NR_PREPASS_BACKSTOP_VZ; o->w = NR_PREPASS_BACKSTOP_VZ;
            o->u = 0.0f; o->v = 0.0f; o->rgba = 0xFF000000u; o->koff = 0.0f;
        }
        m += 6;
    }
    /* Then the WORLD_FILL LP backing. The decal counts the backing as
     * coverage: at the fogged waterline much of the painted terrain IS the
     * hole-fill layer, at true LP depth. The occlusion grid must not: the
     * main pass paints a fill face only into pixels no WORLD fragment
     * covered, so a fill face in front of a WORLD sector is drawn exactly
     * where that sector is not, and counting it would cull the sector it
     * yields to. The fill run draws with colour off, so the colour attachment
     * the grid reduces holds WORLD depth only. */
    const int mw = m;
    m = prepass_pack_world(scene, scene_n, pbuf, m, OPENPETE_SCENE_WORLD_FILL);
    /* Record the PSX-screen->uv affine of THIS mapping for the consumers
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

    if (depth_prepass_debug()) {
        static int last_m = -1;
        if (m != last_m) { last_m = m;
            extern unsigned openpete_frame;
            fprintf(stderr, "[depth-prepass] f=%u WORLD-opaque tris=%d (of %d scene tris) raster %dx%d in %dx%d\n",
                    openpete_frame, m/3, scene_n, vw, vh, s_nr.od_w, s_nr.od_h); }
    }
    /* OPENPETE_PREPASS_RECON_DEBUG=SX,SY: reconstruct the WORLD-space vertices
     * of terrain tris covering the given PSX screen point through the shadow
     * decal's camera model (the pushed inverse) and print them + the implied
     * surface normal - a CPU-side consistency check of the FED terrain
     * geometry against the model, with no GPU in the loop. */
    {
        static int pr_on = -1;
        static float pr_sx = 300.0f, pr_sy = 145.0f;
        static unsigned pr_fmin = 0;
        if (pr_on < 0) {   /* debug knob: read once */
            const char* pr = getenv("OPENPETE_PREPASS_RECON_DEBUG");
            pr_on = (pr && *pr) ? 1 : 0;
            if (pr_on) sscanf(pr, "%f,%f,%u", &pr_sx, &pr_sy, &pr_fmin);
        }
        static int printed = 0;
        extern unsigned openpete_frame;
        const float psx = pr_sx, psy = pr_sy; const unsigned fmin = pr_fmin;
        if (pr_on && ps_shadow()->valid && printed < 40 &&
            openpete_frame >= fmin) {
            for (int t = 0; t < scene_n && printed < 40; t++) {
                int b = scene[t].blend; if (b < 0 || b > 4) b = 0;
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
                    double vz = scene[t].v[k].z;
                    if (vz > OPENPETE_DEPTH_VIEWZ_FAR) vz = OPENPETE_DEPTH_VIEWZ_FAR;
                    if (vz < 1.0) vz = 1.0;
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
                        (double)scene[t].v[0].z, (double)scene[t].v[1].z,
                        (double)scene[t].v[2].z,
                        P[0][1], P[1][1], P[2][1],
                        nl > 1e-9 ? fabs(N[1])/nl : -1.0,
                        P[0][2], P[1][2], P[2][2],
                        nl > 1e-9 ? fabs(N[2])/nl : -1.0);
                printed++;
            }
        }
    }

    /* Render the opaque depth (begin clears depth to far=1.0). Even with m==0
     * the cleared far-depth reads as "no occluder" - never a stale frame. */
    /* Two runs. The colour run draws each WORLD occluder where the main pass
     * paints it (textured faces with their transparent texels cut out), the
     * scene's and the terrain cache's; colour clears to white, the packed far
     * sentinel the grid reads as no occluder, and the backstop rewrites it
     * inside the viewport. The depth run then draws, colour off, the
     * textured WORLD faces over their whole footprint and the WORLD_FILL
     * backing, so the depth aspect the decal reads keeps both. It runs after
     * every colour draw, so no colour-writing fragment is depth-rejected by a
     * depth-run face and the colour keeps the nearest painted WORLD depth. */
    rhi_begin_render_target(s_nr.dev, s_nr.opaque_depth, 1.0f, 1.0f, 1.0f, 1.0f);
    rhi_set_viewport(s_nr.dev, vw, vh);
    prepass_draw_world_runs();
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.prepass_vb);
    rhi_bind_pipeline(s_nr.dev, s_nr.prepass_pipe);
    rhi_draw(s_nr.dev, (uint32_t)(mw - mt), (uint32_t)mt);   /* the backstop */
    const int tc_open = openpete_tercache_frame_open();
    if (tc_open)
        openpete_tercache_draw(OPTC_DRAW_PREPASS, &s_od_taff, s_nr.vram_tex, 0, NULL, NULL);
    prepass_draw_world_footprints();
    if (m > mw) {
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.prepass_vb);
        rhi_bind_pipeline(s_nr.dev, s_nr.prepass_depth_pipe);
        rhi_draw(s_nr.dev, (uint32_t)(m - mw), (uint32_t)mw);
    }
    if (tc_open)
        openpete_tercache_draw(OPTC_DRAW_PREPASS_DEPTH, &s_od_taff, s_nr.vram_tex, 0, NULL, NULL);
    rhi_end_render_target(s_nr.dev, s_nr.opaque_depth);
    s_od_vw = (float)vw; s_od_vh = (float)vh;
    s_od_ready = 1;
}

/* ---- Terrain HP KEY pass ---------------------------------------------------
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
/* Key-pass availability; documented at the declaration. */
int openpete_native_key_resolve_on(void)
{
    return s_nr.enabled && s_nr.terrain_key_pipe && s_nr.terrain_key_vb;
}

/** @brief Return whether OPENPETE_TERRAIN_KEY_READBACK is set.
 *
 * When set, the terrain key image is created readable and every native
 * screenshot writes it beside the PNG (terrain_key_dump). Read once and
 * cached.
 * @return 1 when set, else 0. */
static int terrain_key_readback_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TERRAIN_KEY_READBACK");
                 v = (e && *e && *e != '0'); }
    return v;
}

/** @brief Release every key-contest image and leave its slot NULL.
 *
 * The caller has waited for the device to idle. */
static void key_images_release(void)
{
    rhi_texture** slots[] = { &s_nr.terrain_key_depth, &s_nr.moby_key_depth,
                              &s_nr.actor_key_depth, &s_nr.moby_rank_rt };
    for (size_t i = 0; i < sizeof slots / sizeof slots[0]; i++) {
        rhi_destroy_texture(s_nr.dev, *slots[i]);
        *slots[i] = NULL;
    }
}

/** @brief Return whether every key-contest image exists. */
static int key_images_complete(void)
{
    return s_nr.terrain_key_depth && s_nr.moby_key_depth && s_nr.actor_key_depth &&
           s_nr.moby_rank_rt;
}

/** @brief Grow the four key-contest images to at least w by h.
 *
 * The terrain, prop and actor key depth images and the rank target are
 * recreated together at the larger of the request and the current extent;
 * an existing set that already covers the request is kept, and the terrain
 * image is created readable under OPENPETE_TERRAIN_KEY_READBACK. Recording
 * frames may reference the current images, so a call during a frame only
 * warns once and defers to the per-present resource pass. The allocation is
 * all-or-nothing: on a partial failure every image is released, tk_w and
 * tk_h are zeroed and the failure is logged. Waits for the device to idle
 * before destroying. Non-positive extents are ignored.
 * @param w Requested width in pixels; the extent never shrinks below the
 *        current one.
 * @param h Requested height in pixels; the extent never shrinks below the
 *        current one. */
static void ensure_terrain_key_depth(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (key_images_complete() && s_nr.tk_w >= w && s_nr.tk_h >= h) return;
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
    key_images_release();
    s_nr.terrain_key_depth = terrain_key_readback_on()
        ? rhi_create_render_target_depth_readable(s_nr.dev, (uint32_t)w, (uint32_t)h)
        : rhi_create_render_target_depth_sampled(s_nr.dev, (uint32_t)w, (uint32_t)h);
    s_nr.moby_key_depth    = rhi_create_render_target_depth_sampled(s_nr.dev, (uint32_t)w, (uint32_t)h);
    s_nr.actor_key_depth   = rhi_create_render_target_depth_sampled(s_nr.dev, (uint32_t)w, (uint32_t)h);
    s_nr.moby_rank_rt      = rhi_create_render_target(s_nr.dev, (uint32_t)w, (uint32_t)h);
    rhi_set_render_target_depth_class(s_nr.terrain_key_depth, RHI_DEPTH_FORWARD);
    rhi_set_render_target_depth_class(s_nr.moby_key_depth, RHI_DEPTH_FORWARD);
    rhi_set_render_target_depth_class(s_nr.actor_key_depth, RHI_DEPTH_FORWARD);
    rhi_set_render_target_depth_class(s_nr.moby_rank_rt, RHI_DEPTH_FORWARD);
    if (key_images_complete())
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
        key_images_release();
        s_nr.tk_w = s_nr.tk_h = 0;
        OPL_WARN_F("native", "key-contest targets %dx%d could not be allocated; scene draws are undefined", w, h);
    }
}

/** @brief Size the key-contest images for every scene target this present can draw into, before the frame begins.
 *
 * The scene pipelines (gouraud, textured, cutout and their blend variants)
 * bind these four images as descriptor sets 1-4 on every draw, chrome
 * included: the stats overlay's supersample pre-pass is a gouraud draw and,
 * under eager production, the first draw of the first present, ahead of any
 * scene. A draw whose pipeline statically reads a set that was never bound
 * is undefined behaviour; Mesa ANV faults inside vkCmdDraw on it. The size
 * is the union of the extents the key pass is called with (native_rtt, the
 * SSAA target, the window), so the key pass never allocates mid-frame.
 * No-op when openpete_native_key_resolve_on is off. */
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
 * clip mapping - a PURE function of (scene snapshot, mapping), shared by the
 * key pass and the OPENPETE_TERRAIN_KEY_IDENT instrument, which repacks at
 * scene-draw time and byte-compares to prove pass and draw consumed the
 * same snapshot (the draw fetches scene_present_buf independently). */
/* prop_class 0 = TERRAIN og KEYS (the set-2 image): keyed WORLD faces as
 * their ABSOLUTE og key laid FLAT, og's per-pixel OT occupancy (the
 * painter select).
 * prop_class 1 = PROP TRUE DEPTH (the set-3 image): curated embedded-prop
 * faces only (openpete_scene_tri.prop), per-vertex TRUE view-Z, no bias.
 * prop_class 2 = MOBY og KEYS (the set-1 image): keyed opaque ACTOR faces
 * as their ABSOLUTE og key laid FLAT. Split from set 2 so fragments
 * can hold a moby's paint-over to keys in front of their own TRUE surface
 * while terrain-vs-terrain keeps contesting authored (pushed) keys - a
 * moby pull and a terrain push must not compound into a punch-through
 * window wider than either authored intent.
 * The near-band embed rule reads it: no geometric quantity separates "prop
 * set into its ground" from "actor just behind a crest" - flat keys,
 * per-pixel keys, biased and unbiased margins all overlap across the two
 * classes - so prop-ness is AUTHORED (a class list in the moby feeder,
 * the k_lp_artfix seam pattern), and only props get the always-win-your-
 * ground divergence. */
/* Material runs of the TEXTURED tail of a key batch (see
 * pack_terrain_key_batch): [first, first+count) verts drawn through the
 * alpha-aware cutout pipe with `mat` pushed. A batch's verts are laid out
 * untextured-prefix first, textured material runs after. */
#define KEYTEX_MAX_SPANS 256
typedef struct { int first, count; psx_material mat; } keytex_span_t;

#define s_ktex_idx ((int *)s_cz_ktex.data)
static void tex_mat_sort(const scene_tri_t* scene, int* idx, int n);

/* The screen boxes of this scene's keyed additive glow fans. The player's
 * ordering key rasters only inside them: a player texel displaces the actor
 * key under it in the shared image (psx_key_contest.glsl), and the fan is
 * the only contest that reads it, so outside a fan's reach the stamp would
 * move contests that must not move. A scene with no keyed fan (every game
 * but Spyro 1, and Spyro 1 with no glow on screen) yields no boxes and
 * stamps no player key at all. */
#define KEY_AURA_BOX_MAX 64
typedef struct { float x0, y0, x1, y1; } key_box_t;

/** @brief Return the screen-space bounding box of a triangle.
 * @param t The triangle. */
static key_box_t tri_box(const scene_tri_t* t)
{
    key_box_t b = { t->v[0].sx, t->v[0].sy, t->v[0].sx, t->v[0].sy };
    for (int k = 1; k < 3; k++) {
        if (t->v[k].sx < b.x0) b.x0 = t->v[k].sx;
        if (t->v[k].sx > b.x1) b.x1 = t->v[k].sx;
        if (t->v[k].sy < b.y0) b.y0 = t->v[k].sy;
        if (t->v[k].sy > b.y1) b.y1 = t->v[k].sy;
    }
    return b;
}

/** @brief Collect the screen boxes of the scene's keyed additive glow fans.
 *
 * A face counts when it is additive (blend 2), keyed (koff above 0.25) and
 * ranked no farther than OPENPETE_SCENE_RANK_AURA. Once KEY_AURA_BOX_MAX
 * boxes are held, each further box widens the last one to the union, so
 * the table stays a superset of the fans.
 * @param scene Scene triangles.
 * @param scene_n Triangle count.
 * @param out Receives up to KEY_AURA_BOX_MAX boxes.
 * @return Number of boxes written. */
static int scene_aura_boxes(const scene_tri_t* scene, int scene_n, key_box_t* out)
{
    int n = 0;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].blend != 2 || scene[t].koff <= 0.25f ||
            scene[t].rank > OPENPETE_SCENE_RANK_AURA + 0.5f) continue;
        key_box_t b = tri_box(&scene[t]);
        if (n < KEY_AURA_BOX_MAX) { out[n++] = b; continue; }
        /* Past the table, widen the last box: a superset keeps the rule
         * (the fan still reads its own pixels) and never drops a stamp. */
        key_box_t* l = &out[KEY_AURA_BOX_MAX - 1];
        if (b.x0 < l->x0) l->x0 = b.x0;
        if (b.y0 < l->y0) l->y0 = b.y0;
        if (b.x1 > l->x1) l->x1 = b.x1;
        if (b.y1 > l->y1) l->y1 = b.y1;
    }
    return n;
}

/** @brief Return whether a screen box overlaps any box of a table.
 *
 * Boxes that share only an edge or a corner overlap. */
static int box_hits_any(key_box_t b, const key_box_t* boxes, int n)
{
    for (int i = 0; i < n; i++)
        if (b.x0 <= boxes[i].x1 && b.x1 >= boxes[i].x0 &&
            b.y0 <= boxes[i].y1 && b.y1 >= boxes[i].y0) return 1;
    return 0;
}

/** @brief Return whether a scene face rasters into the key image of a class.
 *
 * Only opaque faces (blend 0) are admitted. Class 0, the terrain key
 * image, takes keyed WORLD faces. Class 1, the prop image, takes keyed
 * curated embedded props (scene_tri_t.prop), which raster true per-vertex
 * depth. Class 2, the actor image, takes keyed ACTOR faces and PLAYER faces
 * carrying an ordering slot (pkey) whose screen box overlaps an aura box,
 * which raster into the image's player band. A face is keyed when koff
 * exceeds 0.25.
 * @param t Scene face.
 * @param prop_class Key image class, 0, 1 or 2.
 * @param aura Aura boxes of the scene (scene_aura_boxes).
 * @param naura Aura box count.
 * @return 1 when the face is admitted, else 0. */
static int key_batch_admits(const scene_tri_t* t, int prop_class,
                            const key_box_t* aura, int naura)
{
    if (t->blend != 0) return 0;
    if (prop_class == 1) return t->prop && t->koff > 0.25f;
    if (prop_class == 2) {
        if (t->category == OPENPETE_SCENE_ACTOR) return t->koff > 0.25f;
        return t->category == OPENPETE_SCENE_PLAYER && t->pkey > 0.25f &&
               box_hits_any(tri_box(t), aura, naura);
    }
    return t->category == OPENPETE_SCENE_WORLD && t->koff > 0.25f;
}

/* The player band's C mirror (psx_key_contest.glsl, op_pkey_is): slot s
 * stores as OP_PKEY_BASE + s*OP_PKEY_STEP, below every reachable real key,
 * so the band wins the image's depth test and every contest but the aura's
 * filters it back out. */
#define NR_PKEY_BASE 0.000001f
#define NR_PKEY_STEP 0.00000001f
#define NR_PKEY_MAX_SLOT 4095.0f

/** @brief Map a player ordering slot plus one to its depth in the player key band.
 *
 * The slot is pkey - 1 clamped to [0, NR_PKEY_MAX_SLOT]; the depth is
 * NR_PKEY_BASE + slot * NR_PKEY_STEP.
 * @param pkey Scene triangle player key, slot + 1 (0 means none).
 * @return Clip-space depth of the slot in the key image. */
static float pack_player_key_z(float pkey)
{
    float slot = pkey - 1.0f;                  /* the +1 that makes 0 "none" */
    if (slot < 0.0f) slot = 0.0f;
    if (slot > NR_PKEY_MAX_SLOT) slot = NR_PKEY_MAX_SLOT;
    return NR_PKEY_BASE + slot * NR_PKEY_STEP;
}

/** @brief Return whether actor fragments skip their own moby's keys; OPENPETE_ACTOR_OWN_KEY=0 turns the skip off for an in-binary A/B.
 *
 * Read once. Off, the actor key pass writes no owner identity, so no
 * fragment matches the owner of a key texel and every contest is the one
 * the stored key alone gives. */
static int actor_own_key_on(void)
{
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_ACTOR_OWN_KEY"); s_on = !(e && *e == '0'); }
    return s_on;
}

/** @brief Write one keyed scene face's three vertices for a key batch.
 *
 * Each vertex carries the clip position, the true view-Z in w, the UV when
 * textured (else 0), a colour, koff 0.5 and the drain record fields from
 * rv_drain. The colour is opaque black, except an ACTOR face's in the actor
 * class while actor_own_key_on, whose r, g, b carry its moby identity (bits
 * 0-7, 8-15, 16-23 of openpete_scene_tri.mid) for the key image's owner
 * colour. The depth is the per-vertex view-Z for the prop
 * class; otherwise it is one flat value per face, the player band depth
 * when the face has a player key (pkey above 0.25), else koff - 0.5.
 * @param T Scene triangle.
 * @param prop_class Batch class, 0 to 2; 1 selects true per-vertex depth.
 * @param tex Non-zero to carry the UVs.
 * @param out Receives three vertices. */
static void pack_key_vert(const scene_tri_t* T, int prop_class, int tex,
                          rhi_vertex* out)
{
    float zflat = T->pkey > 0.25f ? pack_player_key_z(T->pkey)
                                  : T->koff - 0.5f;   /* the key K as view-Z */
    const int owner = prop_class == 2 && T->category == OPENPETE_SCENE_ACTOR &&
                      actor_own_key_on();
    const uint32_t rgba = 0xFF000000u | (owner ? (T->mid & 0xFFFFFFu) : 0u);
    for (int k = 0; k < 3; k++) {
        float cx, cy;
        scene_to_clip(T->v[k].sx, T->v[k].sy, &cx, &cy);
        rhi_vertex* o = &out[k];
        /* TRUE per-vertex view-Z. Drives the prop image's true-depth raster
         * and the cutout stage's perspective UV. */
        float tv = T->v[k].pvz;
        float z = prop_class == 1 ? tv : zflat;
        /* w: the cutout UV's perspective W, and the drain face-local
         * coordinate's (terrain_key.vert), so every face carries it. */
        o->x = cx; o->y = cy; o->z = z; o->w = tv;
        o->u = tex ? T->v[k].u : 0.0f;
        o->v = tex ? T->v[k].v : 0.0f;
        o->rgba = rgba;
        o->koff = 0.5f;
        o->sx = 0.0f; o->sy = 0.0f;
        rv_drain(o, T, k);
    }
}

/* Per-face admission and vertex writer of a key-layout batch
 * (pack_keyrun_batch). The writer receives the span the face is appended
 * to, NULL for the untextured prefix. */
typedef int (*keyrun_admit_fn)(const scene_tri_t* t, const void* ctx);
typedef void (*keyrun_write_fn)(const scene_tri_t* t, const keytex_span_t* sp,
                                const void* ctx, rhi_vertex* out);

/** @brief Return the span a textured face at vertex offset m appends to, opening a material run or the overflow sentinel when needed.
 *
 * The last span is reused when it is the overflow sentinel (bpp < 0) or
 * holds the face's material (texture page, CLUT and bpp). Otherwise a new
 * span opens at @p m with the face's material while fewer than
 * KEYTEX_MAX_SPANS - 1 spans exist; past that the last slot opens as the
 * overflow sentinel, material zeroed and bpp -1, and takes every later
 * face.
 * @param T Textured scene face.
 * @param m Vertex offset the face's vertices go to.
 * @param spans Span table, capacity KEYTEX_MAX_SPANS.
 * @param nspans In: spans in use. Out: incremented when a span opens.
 * @return The span to append the face to. */
static keytex_span_t* keytex_span_for(const scene_tri_t* T, int m,
                                      keytex_span_t* spans, int* nspans)
{
    keytex_span_t* sp = (*nspans > 0) ? &spans[*nspans - 1] : NULL;
    if (sp && (sp->mat.bpp < 0 ||
               (sp->mat.tpage_x == T->tpage_x && sp->mat.tpage_y == T->tpage_y &&
                sp->mat.clut_x == T->clut_x && sp->mat.clut_y == T->clut_y &&
                sp->mat.bpp == T->bpp)))
        return sp;
    sp = &spans[(*nspans)++];
    sp->first = m; sp->count = 0;
    memset(&sp->mat, 0, sizeof sp->mat);
    if (*nspans < KEYTEX_MAX_SPANS) {
        sp->mat.tpage_x = T->tpage_x; sp->mat.tpage_y = T->tpage_y;
        sp->mat.clut_x = T->clut_x; sp->mat.clut_y = T->clut_y;
        sp->mat.bpp = T->bpp;
    } else {
        sp->mat.bpp = -1;   /* table full: the remainder rasters full-footprint */
    }
    return sp;
}

/** @brief Pack a key-layout batch: admitted untextured faces first, then admitted textured faces in per-material runs.
 *
 * Faces are visited in scene order; @p admit selects them. Untextured faces
 * are written at once; textured ones are collected (up to the compose
 * triangle capacity), ordered by tex_mat_sort, the intern and sort the
 * scene draw batches with, so runs are contiguous and few, and appended
 * span by span (keytex_span_for). Output stops at the compose vertex
 * capacity. The full-extent mapping is set for the pack and restored
 * after.
 * @param scene Scene triangles.
 * @param scene_n Triangle count.
 * @param full_extent Non-zero to map in the full-extent PSX space.
 * @param admit Face admission test.
 * @param write Vertex writer, given the face's span or NULL in the prefix.
 * @param ctx Passed to @p admit and @p write.
 * @param out Output vertices.
 * @param out_untex Receives the untextured prefix length in vertices.
 * @param spans Receives the textured runs, capacity KEYTEX_MAX_SPANS.
 * @param nspans Receives the run count.
 * @return Vertices written. */
static int pack_keyrun_batch(const scene_tri_t* scene, int scene_n, int full_extent,
                             keyrun_admit_fn admit, keyrun_write_fn write,
                             const void* ctx, rhi_vertex* out, int* out_untex,
                             keytex_span_t* spans, int* nspans)
{
    int prev_fe = s_render_full_extent;
    s_render_full_extent = full_extent ? 1 : 0;
    int m = 0, tex_n = 0;
    *nspans = 0;
    for (int t = 0; t < scene_n && m + 3 <= s_cz_verts; t++) {
        if (!admit(&scene[t], ctx)) continue;
        if (scene[t].textured) {
            if (tex_n < s_cz_tris) s_ktex_idx[tex_n++] = t;
            continue;
        }
        write(&scene[t], NULL, ctx, &out[m]);
        m += 3;
    }
    *out_untex = m;
    tex_mat_sort(scene, s_ktex_idx, tex_n);
    for (int i = 0; i < tex_n && m + 3 <= s_cz_verts; i++) {
        const scene_tri_t* T = &scene[s_ktex_idx[i]];
        keytex_span_t* sp = keytex_span_for(T, m, spans, nspans);
        write(T, sp, ctx, &out[m]);
        m += 3;
        sp->count += 3;
    }
    s_render_full_extent = prev_fe;
    return m;
}

/* Key-batch class and the scene's aura boxes, the context of the key
 * batch's admit and write callbacks. */
typedef struct {
    int prop_class;
    const key_box_t* aura;
    int naura;
} key_run_ctx_t;

/** @brief Admit a face into a key batch by its class (key_batch_admits).
 * @param t Scene face.
 * @param ctx The batch's key_run_ctx_t.
 * @return 1 when the face is admitted, else 0. */
static int key_run_admit(const scene_tri_t* t, const void* ctx)
{
    const key_run_ctx_t* c = (const key_run_ctx_t*)ctx;
    return key_batch_admits(t, c->prop_class, c->aura, c->naura);
}

/** @brief Write a key-batch face's vertices, with UVs on every textured span.
 *
 * The overflow sentinel keeps its UVs, since the cutout pipe keys a drain
 * record's sub-cell by UV on that span too.
 * @param t Scene face.
 * @param sp The face's span, NULL in the untextured prefix.
 * @param ctx The batch's key_run_ctx_t.
 * @param out Receives three vertices. */
static void key_run_write(const scene_tri_t* t, const keytex_span_t* sp,
                          const void* ctx, rhi_vertex* out)
{
    const key_run_ctx_t* c = (const key_run_ctx_t*)ctx;
    pack_key_vert(t, c->prop_class, sp != NULL, out);
}

/** @brief Pack one key batch's vertices: untextured keyed faces first, then textured ones in per-material runs.
 *
 * Admission is by class: 0 opaque keyed WORLD faces, 1 opaque keyed props,
 * 2 opaque keyed ACTOR faces and PLAYER faces with a player key that overlap
 * an aura box. Untextured faces raster their full footprint, since a
 * gouraud face covers its whole triangle. Textured faces are sorted into
 * material runs for the cutout pipe, which drops the PSX p==0 transparent
 * texels so a face's key occupies exactly the pixels its opaque texels
 * paint: a cloak edge or fence lattice does not key-discard what its holes
 * show. When the span table fills, the remainder goes into a trailing
 * sentinel span (bpp < 0) that the cutout pipe draws without the texel
 * cutout while still keying a drain record's sub-cell by its UV. The
 * layout and limits are pack_keyrun_batch's.
 * @param scene Scene triangles.
 * @param scene_n Triangle count.
 * @param full_extent Non-zero to map in the full-extent PSX space.
 * @param prop_class Batch class, 0 to 2.
 * @param out Output vertices.
 * @param out_untex Receives the untextured prefix length in vertices.
 * @param spans Receives the textured runs.
 * @param nspans Receives the run count.
 * @return Vertices written. */
static int pack_terrain_key_batch(const scene_tri_t* scene, int scene_n,
                                  int full_extent, int prop_class, rhi_vertex* out,
                                  int* out_untex, keytex_span_t* spans, int* nspans)
{
    key_box_t aura[KEY_AURA_BOX_MAX];
    key_run_ctx_t c = { prop_class, aura,
                        prop_class == 2 ? scene_aura_boxes(scene, scene_n, aura) : 0 };
    return pack_keyrun_batch(scene, scene_n, full_extent, key_run_admit,
                             key_run_write, &c, out, out_untex, spans, nspans);
}

/** @brief Write one moby rank-batch triangle's three vertices.
 *
 * The rank batch rasterises ranked opaque moby faces into the RGBA8
 * (rank, ordinal) image at depth equal to their OT walk rank:
 * moby_rank.vert encodes 1 - (rank + 1) / 65536, so the LESS test keeps the
 * last-painted face per pixel. It uses the key batches' layout of an
 * untextured prefix and textured material runs, and textured faces cut
 * transparent texels out of the raster, so a rank exists only on opaque
 * texels. Each vertex carries its clip position, z 0, the true view-Z in w
 * (the cutout stage's UV perspective and the owner depth the fragment
 * stage encodes into the rank image's alpha), the UV when textured, and
 * the (rank, ordinal) pair in sx and sy.
 * @param T Scene triangle.
 * @param tex Non-zero to carry the UVs.
 * @param out Receives three vertices. */
static void pack_rank_vert(const scene_tri_t* T, int tex, rhi_vertex* out)
{
    for (int k = 0; k < 3; k++) {
        float cx, cy;
        scene_to_clip(T->v[k].sx, T->v[k].sy, &cx, &cy);
        rhi_vertex* o = &out[k];
        float tv = T->v[k].pvz;
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

/** @brief Admit a face into the rank batch when it is opaque (blend 0) and not ranked below 0.5.
 * @param t Scene face.
 * @param ctx Unused.
 * @return 1 when the face is admitted, else 0. */
static int rank_run_admit(const scene_tri_t* t, const void* ctx)
{
    (void)ctx;
    return t->blend == 0 && !(t->rank < 0.5f);
}

/** @brief Write a rank-batch face's vertices, with UVs only on a material span.
 *
 * The overflow sentinel (bpp < 0) draws through the flat pipe, which reads
 * no UV, so its vertices carry none.
 * @param t Scene face.
 * @param sp The face's span, NULL in the untextured prefix.
 * @param ctx Unused.
 * @param out Receives three vertices. */
static void rank_run_write(const scene_tri_t* t, const keytex_span_t* sp,
                           const void* ctx, rhi_vertex* out)
{
    (void)ctx;
    pack_rank_vert(t, sp != NULL && sp->mat.bpp >= 0, out);
}

/** @brief Pack the rank batch: ranked opaque moby faces, untextured prefix first, then textured material runs.
 *
 * A face is packed when it is opaque (blend 0) and ranked (rank at least
 * 0.5); vertices come from pack_rank_vert. The layout, the material runs,
 * the trailing sentinel span (bpp -1) on span table overflow and the
 * limits are pack_keyrun_batch's.
 * @param scene Scene triangles.
 * @param scene_n Triangle count.
 * @param full_extent Non-zero to map in the full-extent PSX space.
 * @param out Output vertices.
 * @param out_untex Receives the untextured prefix length in vertices.
 * @param spans Receives the textured runs, capacity KEYTEX_MAX_SPANS.
 * @param nspans Receives the run count.
 * @return Vertices written. */
static int pack_moby_rank_batch(const scene_tri_t* scene, int scene_n,
                                int full_extent, rhi_vertex* out,
                                int* out_untex, keytex_span_t* spans, int* nspans)
{
    return pack_keyrun_batch(scene, scene_n, full_extent, rank_run_admit,
                             rank_run_write, NULL, out, out_untex, spans, nspans);
}

/* The last key-pass packs (terrain + prop + actor), recorded for the identity
 * instrument in draw_native_scene (m == -1 until a pass has run). */
static struct {
    const scene_tri_t* scene;
    int scene_n, full_extent, m, mm, am;
    const mod_fvec* buf;     /* runs, not bases: a later compose_reserve may move them */
    const mod_fvec* mbuf;
    const mod_fvec* abuf;
    int vw, vh;                     /* the pass's viewport in the key image */
    openpete_tercache_clip clip;    /* the pass's screen->clip mapping */
} s_tk_last = { NULL, 0, -1, -1, -1, -1, NULL, NULL, NULL, 0, 0, { 0 } };

/** @brief Count the triangles of a packed key batch that key per drain sub-cell.
 *
 * rv_drain marks such a triangle by storing minus one minus its drain
 * record index in the first vertex's sy, so a triangle counts when that sy
 * is below -0.5. Read by the OPENPETE_TERRAIN_KEY_DBG line.
 * @param kbuf Packed key batch vertices.
 * @param m Vertex count.
 * @return The triangle count. */
static int key_batch_drain_tris(const rhi_vertex* kbuf, int m)
{
    int n = 0;
    for (int t3 = 0; t3 + 3 <= m; t3 += 3) n += kbuf[t3].sy < -0.5f;
    return n;
}

/* The pipelines one key-layout batch draws through (draw_keyrun_batch). */
typedef struct {
    rhi_pipeline* flat;     /* untextured prefix, and spans drawn flat      */
    rhi_pipeline* cutout;   /* material spans; NULL draws every span flat   */
    rhi_texture*  set1;     /* bound at set 1 with the cutout pipe, or NULL */
    int overflow_flat;      /* non-zero: bpp < 0 spans draw through flat    */
} keyrun_pipes_t;

/** @brief Draw one packed key-layout batch into the currently open render pass.
 *
 * The untextured prefix draws through the flat pipe. Each material span
 * draws through the cutout pipe with VRAM at set 0, @p p->set1 at set 1
 * when given, and the span's material pushed; the binds are issued once
 * per run of cutout spans. A span draws through the flat pipe instead when
 * there is no cutout pipe, or when its bpp is negative and
 * @p p->overflow_flat is set; the next cutout span then rebinds.
 * @param p The batch's pipelines.
 * @param base The batch's bump-allocated vertex offset in terrain_key_vb.
 * @param m Vertices in the batch; nothing draws when 0 or less.
 * @param untex Vertices in the untextured prefix.
 * @param spans Textured material spans, offsets relative to @p base.
 * @param nspans Span count. */
static void draw_keyrun_batch(const keyrun_pipes_t* p, int base, int m, int untex,
                              const keytex_span_t* spans, int nspans)
{
    if (m <= 0) return;
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.terrain_key_vb);
    if (untex > 0) {
        rhi_bind_pipeline(s_nr.dev, p->flat);
        rhi_draw(s_nr.dev, (uint32_t)untex, (uint32_t)base);
    }
    int tex_bound = 0;
    for (int i = 0; i < nspans; i++) {
        if (spans[i].count <= 0) continue;
        if (!p->cutout || (p->overflow_flat && spans[i].mat.bpp < 0)) {
            rhi_bind_pipeline(s_nr.dev, p->flat);
            rhi_draw(s_nr.dev, (uint32_t)spans[i].count,
                     (uint32_t)(base + spans[i].first));
            tex_bound = 0;
            continue;
        }
        if (!tex_bound) {
            rhi_bind_pipeline(s_nr.dev, p->cutout);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            if (p->set1) rhi_bind_texture_set(s_nr.dev, p->set1, 1);
            tex_bound = 1;
        }
        rhi_push_constants(s_nr.dev, &spans[i].mat, sizeof(spans[i].mat));
        rhi_draw(s_nr.dev, (uint32_t)spans[i].count,
                 (uint32_t)(base + spans[i].first));
    }
}

/** @brief Draw one packed key batch into the currently open key render pass.
 *
 * The untextured prefix draws through the flat full-footprint pipe
 * (terrain_key_pipe) and each textured material span through the
 * alpha-aware cutout pipe (terrain_key_tex_pipe, VRAM at set 0 and the
 * drain records at set 1), where transparent texels stamp no key; the
 * overflow sentinel (bpp < 0) takes the cutout pipe too, which skips the
 * texel cutout on it but keys drain sub-cells by UV. Without the cutout
 * pipe the spans draw flat.
 * @param base The batch's bump-allocated vertex offset in terrain_key_vb.
 * @param m Vertices in the batch; nothing draws when 0 or less.
 * @param untex Vertices in the untextured prefix.
 * @param spans Textured material spans, offsets relative to @p base.
 * @param nspans Span count. */
static void draw_key_batch(int base, int m, int untex,
                           const keytex_span_t* spans, int nspans)
{
    keyrun_pipes_t p = { s_nr.terrain_key_pipe, s_nr.terrain_key_tex_pipe,
                         s_nr.drain_tex, 0 };
    draw_keyrun_batch(&p, base, m, untex, spans, nspans);
}

/** @brief Draw one packed rank batch into the currently open rank render pass.
 *
 * The untextured prefix draws through moby_rank_pipe and each textured
 * material span through the alpha-aware moby_rank_cutout_pipe; a span with
 * negative bpp, or every span when the cutout pipe is absent, draws through
 * the flat pipe. Nothing draws without moby_rank_pipe.
 * @param base The batch's bump-allocated vertex offset in terrain_key_vb.
 * @param m Vertices in the batch; nothing draws when 0 or less.
 * @param untex Vertices in the untextured prefix.
 * @param spans Textured material spans, offsets relative to @p base.
 * @param nspans Span count. */
static void draw_rank_batch(int base, int m, int untex,
                            const keytex_span_t* spans, int nspans)
{
    if (!s_nr.moby_rank_pipe) return;
    keyrun_pipes_t p = { s_nr.moby_rank_pipe, s_nr.moby_rank_cutout_pipe, NULL, 1 };
    draw_keyrun_batch(&p, base, m, untex, spans, nspans);
}

/* The depth pre-pass's WORLD batch layout, written by
 * prepass_pack_world_runs and drawn by prepass_draw_world_runs within one
 * native_depth_prepass. */
static keytex_span_t s_pp_spans[KEYTEX_MAX_SPANS];
static int s_pp_nspans, s_pp_untex, s_pp_m;

/** @brief Admit an opaque WORLD face into the pre-pass WORLD batch.
 * @param t Scene face.
 * @param ctx Unused.
 * @return 1 when the face is opaque (blend 0) WORLD, else 0. */
static int prepass_run_admit(const scene_tri_t* t, const void* ctx)
{
    (void)ctx;
    return t->blend == 0 && t->category == OPENPETE_SCENE_WORLD;
}

/** @brief Write a pre-pass WORLD face's vertices (pack_scene_vert), with UVs on a material span.
 * @param t Scene face.
 * @param sp The face's span, NULL in the untextured prefix.
 * @param ctx Unused.
 * @param out Receives three vertices. */
static void prepass_run_write(const scene_tri_t* t, const keytex_span_t* sp,
                              const void* ctx, rhi_vertex* out)
{
    (void)ctx;
    for (int k = 0; k < 3; k++)
        pack_scene_vert(&out[k], t, k, sp ? t->v[k].u : 0.0f, sp ? t->v[k].v : 0.0f);
}

/** @brief Pack the depth pre-pass's WORLD occluders: opaque untextured faces first, then textured faces in material runs.
 *
 * The layout and limits are pack_keyrun_batch's, in the full-extent
 * mapping; the runs are kept for prepass_draw_world_runs.
 * @param scene Scene triangles.
 * @param scene_n Triangle count.
 * @param out Pre-pass vertex run.
 * @return Vertices written. */
static int prepass_pack_world_runs(const scene_tri_t *scene, int scene_n, rhi_vertex *out)
{
    s_pp_m = pack_keyrun_batch(scene, scene_n, 1, prepass_run_admit, prepass_run_write,
                               NULL, out, &s_pp_untex, s_pp_spans, &s_pp_nspans);
    return s_pp_m;
}

/** @brief Draw the packed pre-pass WORLD batch into the open pre-pass render pass, the colour run.
 *
 * Every face draws through prepass_tex_pipe: the untextured prefix with
 * bpp -1 pushed, which covers each face's footprint, and each material run
 * with its material, whose transparent texels are cut (prepass_cutout.frag);
 * the overflow tail carries bpp -1 and covers its footprint. */
static void prepass_draw_world_runs(void)
{
    if (s_pp_m <= 0) return;
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.prepass_vb);
    rhi_bind_pipeline(s_nr.dev, s_nr.prepass_tex_pipe);
    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
    const psx_material flat = { 0, 0, 0, 0, -1 };
    if (s_pp_untex > 0) {
        rhi_push_constants(s_nr.dev, &flat, sizeof flat);
        rhi_draw(s_nr.dev, (uint32_t)s_pp_untex, 0);
    }
    for (int i = 0; i < s_pp_nspans; i++) {
        if (s_pp_spans[i].count <= 0) continue;
        rhi_push_constants(s_nr.dev, &s_pp_spans[i].mat, sizeof(s_pp_spans[i].mat));
        rhi_draw(s_nr.dev, (uint32_t)s_pp_spans[i].count, (uint32_t)s_pp_spans[i].first);
    }
}

/** @brief Draw the packed pre-pass WORLD batch's textured runs again over their whole footprints, depth only.
 *
 * Keeps the depth aspect the shadow decal reads as it was before the colour
 * run cut transparent texels: the decal treats a textured face as covering
 * its footprint. */
static void prepass_draw_world_footprints(void)
{
    if (s_pp_m <= s_pp_untex) return;
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.prepass_vb);
    rhi_bind_pipeline(s_nr.dev, s_nr.prepass_depth_pipe);
    rhi_draw(s_nr.dev, (uint32_t)(s_pp_m - s_pp_untex), (uint32_t)s_pp_untex);
}

/** @brief Record a key pass's viewport and screen-to-clip mapping for terrain_key_dump.
 *
 * The mapping is taken with s_render_full_extent set to @p full_extent, so
 * it matches the positions the pass packs, and the global is then
 * restored.
 * @param full_extent Nonzero when the pass maps to the full target.
 * @param vw Viewport width.
 * @param vh Viewport height. */
static void terrain_key_pass_record(int full_extent, int vw, int vh)
{
    int prev_fe = s_render_full_extent;
    s_render_full_extent = full_extent ? 1 : 0;
    tercache_clip_params(&s_tk_last.clip);
    s_render_full_extent = prev_fe;
    s_tk_last.vw = vw; s_tk_last.vh = vh;
}

/** @brief Pack a triangle's drain record: its key table, then the affine map from texture coordinate to drain coordinate.
 *
 * Words 0 to 7 are the triangle's kc table. Words 8 to 13 hold the 2x3
 * float affine from (u, v) to the drain coordinate (kfl), solved from the
 * three vertices; word 14 is 1 (valid) and word 15 is 0 (drain_records_build
 * sets it when fold data follows). The layout is the one op_drain_record in
 * psx_key_contest.glsl reads.
 * @param t Triangle.
 * @param w Receives the 16-word record; words 8 to 15 stay 0 when no map exists.
 * @return 1 when packed with a map, 0 when the triangle is untextured or its
 *         texture coordinates are collinear. */
static int drain_record_pack(const scene_tri_t* t, uint32_t w[16])
{
    memcpy(w, t->kc, 8 * sizeof(uint32_t));
    memset(w + 8, 0, 8 * sizeof(uint32_t));
    const double u0 = t->v[0].u, v0 = t->v[0].v;
    const double du1 = t->v[1].u - u0, dv1 = t->v[1].v - v0;
    const double du2 = t->v[2].u - u0, dv2 = t->v[2].v - v0;
    const double det = du1 * dv2 - du2 * dv1;
    if (!t->textured || fabs(det) < 1e-6) return 0;
    float m[6];
    for (int c = 0; c < 2; c++) {
        const double p0 = t->kfl[0][c];
        const double dp1 = t->kfl[1][c] - p0, dp2 = t->kfl[2][c] - p0;
        const double a = (dp1 * dv2 - dp2 * dv1) / det;
        const double b = (du1 * dp2 - du2 * dp1) / det;
        m[c * 3 + 0] = (float)a;
        m[c * 3 + 1] = (float)b;
        m[c * 3 + 2] = (float)(p0 - a * u0 - b * v0);
    }
    memcpy(w + 8, m, 6 * sizeof(float));
    w[14] = 1u;   /* valid */
    return 1;
}

/** @brief Pack the two extension records of a folded quadrant's triangle: the face's corners, then the affine map from texture coordinate to fold position.
 *
 * Words 0 to 11 are the corners t->kn. Words 12 to 20 hold the 3x3 float
 * affine from (u, v) to the fold position D, the homogeneous screen point
 * (sx pvz, sy pvz, pvz) of the fragment, solved from the three vertices:
 * D projects to the vertex's own screen position, so the fold test
 * (psx_key_contest.glsl op_drain_record_fold) runs on the pixel's ray. The
 * rest is 0.
 * @param t Triangle with a texture map (drain_record_pack returned 1).
 * @param w Receives the 32-word extension. */
static void drain_record_fold_pack(const scene_tri_t* t, uint32_t w[32])
{
    memset(w, 0, 32 * sizeof(uint32_t));
    memcpy(w, t->kn, 12 * sizeof(float));
    const double u0 = t->v[0].u, v0 = t->v[0].v;
    const double du1 = t->v[1].u - u0, dv1 = t->v[1].v - v0;
    const double du2 = t->v[2].u - u0, dv2 = t->v[2].v - v0;
    const double det = du1 * dv2 - du2 * dv1;
    double D[3][3];
    for (int k = 0; k < 3; k++) {
        const double z = t->v[k].pvz;
        D[k][0] = (double)t->v[k].sx * z;
        D[k][1] = (double)t->v[k].sy * z;
        D[k][2] = z;
    }
    float m[9];
    for (int c = 0; c < 3; c++) {
        const double p0 = D[0][c];
        const double dp1 = D[1][c] - p0, dp2 = D[2][c] - p0;
        const double a = (dp1 * dv2 - dp2 * dv1) / det;
        const double b = (du1 * dp2 - du2 * dp1) / det;
        m[c * 3 + 0] = (float)a;
        m[c * 3 + 1] = (float)b;
        m[c * 3 + 2] = (float)(p0 - a * u0 - b * v0);
    }
    memcpy(w + 12, m, sizeof m);
}

/** @brief Build and upload the drain records of a scene, once per scene and frame serial.
 *
 * Runs before any render pass opens. One record is packed per triangle
 * whose kc table is set, up to DRAIN_REC_PER_ROW * DRAIN_REC_ROWS records;
 * a triangle past the capacity keeps its face's least key. A triangle of a
 * folded quadrant (kfold) takes the two records after its own for the fold
 * data (drain_record_fold_pack) and sets word 15 of its own; without room
 * for them it keeps the plain record. s_drain.idx maps
 * each triangle to its record or -1, and s_drain.untex counts the tables
 * dropped because the triangle had no texture map. The records upload to
 * drain_tex when any were packed and the image exists.
 * @param scene Scene triangles.
 * @param scene_n Triangle count. */
static void drain_records_build(const scene_tri_t* scene, int scene_n)
{
    const uint64_t serial = rhi_frame_serial(s_nr.dev);
    if (s_drain.scene == scene && s_drain.n == scene_n && s_drain.serial == serial) return;
    if (scene_n > s_drain.idx_cap) {
        int* g = (int*)realloc(s_drain.idx, (size_t)scene_n * sizeof(int));
        if (!g) { s_drain.scene = NULL; s_drain.n = 0; return; }
        s_drain.idx = g; s_drain.idx_cap = scene_n;
    }
    s_drain.scene = scene; s_drain.n = scene_n; s_drain.serial = serial;
    s_drain.nrec = 0; s_drain.untex = 0;
    const int cap = DRAIN_REC_PER_ROW * DRAIN_REC_ROWS;
    for (int t = 0; t < scene_n; t++) {
        s_drain.idx[t] = -1;
        if (!scene[t].kc[0] || s_drain.nrec >= cap) continue;
        uint32_t* w = &s_drain.words[(size_t)s_drain.nrec * 16];
        if (!drain_record_pack(&scene[t], w)) {
            s_drain.untex++;
            continue;
        }
        s_drain.idx[t] = s_drain.nrec++;
        if (scene[t].kfold && s_drain.nrec + 2 <= cap) {
            drain_record_fold_pack(&scene[t], w + 16);
            w[15] = 1u;   /* fold data follows */
            s_drain.nrec += 2;
        }
    }
    if (s_drain.nrec == 0 || !s_nr.drain_tex) return;
    const int rows = (s_drain.nrec + DRAIN_REC_PER_ROW - 1) / DRAIN_REC_PER_ROW;
    rhi_rect r = { 0, 0, DRAIN_REC_PER_ROW * 4, rows };
    rhi_update_vram_rects_packed(s_nr.dev, s_nr.drain_tex, (const uint8_t*)s_drain.words,
                                 (size_t)rows * DRAIN_REC_PER_ROW * 64u, &r, 1);
}

/** @brief Pack and render the key images for the present's scene.
 *
 * Returns when the key pass is unavailable or the images do not exist; the
 * viewport clamps to the extent the images already have. The scene's faces
 * are packed into the terrain, prop, actor and rank batches, and the batches
 * append to terrain_key_vb at a bump offset that restarts each RHI frame, so
 * several scene targets in one frame keep their own vertices. When the
 * buffer cannot grow the batches are emptied: the images clear to no cover
 * and the contest is off for this target. The terrain image also receives
 * the retained terrain's key draw; each actor face writes its moby identity
 * into the actor key image's colour beside its key. The four images render
 * even from empty batches, so a stale image is never sampled. The OPENPETE_TERRAIN_KEY_*
 * and OPENPETE_MOBY_RANK_DBG knobs add stderr and file dumps of the packed
 * batches.
 * @param full_extent Nonzero for the full-extent (widescreen) mapping.
 * @param vw Viewport width in the key images.
 * @param vh Viewport height in the key images. */
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
    scene_n = compose_reserve(scene_n);
    drain_records_build(scene, scene_n);
    rhi_vertex *kbuf  = (rhi_vertex *)s_cz_key[0].data;
    rhi_vertex *mkbuf = (rhi_vertex *)s_cz_key[1].data;
    rhi_vertex *akbuf = (rhi_vertex *)s_cz_key[2].data;
    rhi_vertex *rbuf  = (rhi_vertex *)s_cz_key[3].data;
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
    s_tk_last.m = m; s_tk_last.buf = &s_cz_key[0];
    s_tk_last.mm = mm; s_tk_last.mbuf = &s_cz_key[1];
    s_tk_last.am = am; s_tk_last.abuf = &s_cz_key[2];
    terrain_key_pass_record(full_extent, vw, vh);
    { static int kdbg = -1;
      if (kdbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_KEY_DBG"); kdbg = (e && *e && *e != '0'); }
      if (kdbg) {
        extern unsigned openpete_frame;
        fprintf(stderr, "[keypass f=%u] vw=%d vh=%d tris=%d cells=%d records=%d untex=%d\n",
                openpete_frame, vw, vh, m/3, key_batch_drain_tris(kbuf, m), s_drain.nrec,
                s_drain.untex);
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
                float K = kbuf[t3].z;
                fprintf(stderr, "[keycover] K=%.0f xy=(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f)\n",
                        K, x0,y0,x1,y1,x2,y2);
            }
          } }
        for (int t3 = 0; t3 + 3 <= m; t3 += 3) {
            float x0=kbuf[t3].x, x1=kbuf[t3+1].x, x2=kbuf[t3+2].x;
            float y0=kbuf[t3].y, y1=kbuf[t3+1].y, y2=kbuf[t3+2].y;
            float xmin=fminf(x0,fminf(x1,x2)), xmax=fmaxf(x0,fmaxf(x1,x2));
            float ymin=fminf(y0,fminf(y1,y2)), ymax=fmaxf(y0,fmaxf(y1,y2));
            float K = kbuf[t3].z;
            /* wide-in-clip tris that overlap the viewport: the phantom class */
            if ((xmax-xmin) > 0.8f && xmax > -1.0f && xmin < 1.0f && ymax > -1.0f && ymin < 1.0f)
                fprintf(stderr, "[keytri] K=%.0f x=[%.2f,%.2f] y=[%.2f,%.2f]\n",
                        K, xmin, xmax, ymin, ymax);
        }
      } }
    /* BUMP-ALLOCATE within the ring slot: one rhi frame can run SEVERAL
     * scene targets (native_rtt and the windowed offscreen), each
     * with its own key batch - and a second same-frame rhi_update_buffer at
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
    if (m + mm + am + rr > 0 &&
        !rhi_vertex_buffer_reserve(s_nr.dev, s_nr.terrain_key_vb,
                                   sizeof(rhi_vertex) * (size_t)(s_tk_base + m + mm + am + rr))) {
        /* The ring slot could not grow to hold this target's batches. The
         * earlier batches are recorded but not yet submitted, so resetting
         * the bump offset would clobber data a recorded pass still reads at
         * execution. Degrade: leave this target's images clear ("no cover":
         * contest off for this target only, keyed faces resolve at true
         * depth), and say so; a silent skip would read as coverage. */
        extern unsigned openpete_frame;
        fprintf(stderr, "[keypass f=%u] key VB could not grow (base=%d + m=%d+%d+%d+%d): "
                        "contest OFF for this target\n",
                openpete_frame, s_tk_base, m, mm, am, rr);
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
     * which is the correct contest input - never a stale image. */
    rhi_begin_render_target(s_nr.dev, s_nr.terrain_key_depth, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_set_viewport(s_nr.dev, vw, vh);
    draw_key_batch(s_tk_base, m, k_untex, kspans, k_nspans);
    /* Retained terrain emits its og keys here too - CPU-fed faces (and
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

/** @brief Bind set 1 of the bound shadow-decal pipeline and return whether binding 1 holds the terrain keys.
 *
 * Binding 0 is the VRAM image. Binding 1 is the terrain key depth image
 * when the key pass is available (openpete_native_key_resolve_on) and the
 * image exists, else the opaque pre-pass depth, so the set stays complete.
 * @return 1.0 when binding 1 holds the terrain keys, else 0.0; the caller
 *         stores it in the decal's key_contest push constant. */
static float shadow_decal_bind_keys(void)
{
    const bool keys = openpete_native_key_resolve_on() && s_nr.terrain_key_depth;
    rhi_bind_vram_depth_pair(s_nr.dev, s_nr.vram_tex,
                             keys ? s_nr.terrain_key_depth : s_nr.opaque_depth, 1);
    return keys ? 1.0f : 0.0f;
}

/** @brief The cover twin table entry whose twin is a pipeline.
 * @param p Pipeline.
 * @return The entry, or NULL when @p p is no cover twin. */
static const nr_cover_twin* nr_cover_entry(const rhi_pipeline* p)
{
    for (int i = 0; p && i < s_nr.cover_twin_n; i++)
        if (p == s_nr.cover_twins[i].cover) return &s_nr.cover_twins[i];
    return NULL;
}

/** @brief The pipeline a blended world run draws through in place of a stock pipeline.
 * @param stock Stock pipeline, or NULL.
 * @return Its cover twin, or @p stock when it has none. */
static rhi_pipeline* nr_cover_twin_of(rhi_pipeline* stock)
{
    for (int i = 0; stock && i < s_nr.cover_twin_n; i++)
        if (stock == s_nr.cover_twins[i].stock && s_nr.cover_twins[i].cover)
            return s_nr.cover_twins[i].cover;
    return stock;
}

/** @brief Whether a pipeline is a key-resolve pipe, whose fragment stage samples the key images at sets 1 to 4.
 * @param p Pipeline.
 * @return 1 for the opaque, marked, cutout, blended, additive,
 *         quarter-additive and punch variants of the gouraud, scene-texture
 *         and HD cutout pipes, else 0. */
static int nr_is_key_pipe(const rhi_pipeline* p)
{
    return p == s_nr.gouraud_pipe || p == s_nr.gouraud_mark_pipe ||
           p == s_nr.scene_tex_pipe || p == s_nr.scene_tex_mark_pipe ||
           p == s_nr.hd_cutout_pipe || p == s_nr.hd_cutout_mark_pipe ||
           p == s_nr.gouraud_blend_pipe || p == s_nr.gouraud_add_pipe ||
           p == s_nr.scene_tex_blend_pipe || p == s_nr.scene_tex_add_pipe ||
           p == s_nr.gouraud_qadd_pipe || p == s_nr.scene_tex_qadd_pipe ||
           p == s_nr.gouraud_punch_pipe || p == s_nr.scene_tex_punch_pipe ||
           p == s_nr.hd_cutout_punch_pipe;
}

static rhi_ubo* s_cover_ubo;
/** @brief Bind the composed scene's portal cover planes at one set of a portal pipeline.
 *
 * The two-block UBO is created on first use. Both blocks are the scene's
 * planes (ps_cover) with this pass's gl_FragCoord -> PSX screen affine;
 * block 1 also sets cnt.z, which selects the inner-rim rule in a stage that
 * picks the rule at run time (a material's portal profile). They are
 * written on the first bind of each RHI frame and again whenever they
 * differ from the last ones written, so a panel pass and a full-extent pass
 * of one present each read their own affine. Returns without binding when
 * the UBO cannot be created.
 * @param set The descriptor set: 5 on the engine's twins, 7 on a material's
 *        portal profile.
 * @param rim Nonzero to bind the inner-rim block. */
static void bind_cover_block(uint32_t set, int rim)
{
    static uint64_t s_serial = UINT64_MAX;
    static nr_cover_block s_last;
    if (!s_cover_ubo) s_cover_ubo = rhi_create_ubo(s_nr.dev, (uint32_t)sizeof(nr_cover_block), 2);
    if (!s_cover_ubo) return;
    nr_cover_block b = *ps_cover();
    scene_fc_affine fa;
    scene_fragcoord_affine(&fa);
    b.fc[0] = fa.fax; b.fc[1] = fa.fbx; b.fc[2] = fa.fay; b.fc[3] = fa.fby;
    const uint64_t fs = rhi_frame_serial(s_nr.dev);
    if (fs != s_serial || memcmp(&b, &s_last, sizeof b) != 0) {
        rhi_ubo_write(s_cover_ubo, 0, &b, (uint32_t)sizeof b);
        nr_cover_block r = b;
        r.cnt[2] = 1;
        rhi_ubo_write(s_cover_ubo, 1, &r, (uint32_t)sizeof r);
        s_last = b;
        s_serial = fs;
    }
    rhi_ubo_bind_at(s_nr.dev, s_cover_ubo, set, rim ? 1u : 0u);
}

/** @brief Bind a pipeline for scene drawing, with the key images a key-resolve pipe samples.
 *
 * When all three key depth images exist and @p p is a key-resolve pipe
 * (nr_is_key_pipe), the bind also sets the actor key depth paired with its
 * owner colour at set 1, the terrain key depth paired with the drain
 * texture at set 2, the moby key depth at set 3 and, when it exists, the
 * moby rank target at set 4. A blended-world cover twin takes, at set 5,
 * the scene's portal cover planes (bind_cover_block), and the key sets
 * when its stock pipeline is a key-resolve pipe. Binding them here keeps
 * every call site correct without per-site handling. Any other pipeline is
 * bound alone.
 * @param p Pipeline to bind. */
static void bind_scene_pipe(rhi_pipeline* p)
{
    rhi_bind_pipeline(s_nr.dev, p);
    const nr_cover_twin* const tw = nr_cover_entry(p);
    if (tw) bind_cover_block(5, tw->rim);
    if (!s_nr.terrain_key_depth || !s_nr.moby_key_depth || !s_nr.actor_key_depth) return;
    if (nr_is_key_pipe(tw ? tw->stock : p)) {
        rhi_bind_depth_tex_pair(s_nr.dev, s_nr.actor_key_depth, s_nr.actor_key_depth, 1);
        rhi_bind_depth_tex_pair(s_nr.dev, s_nr.terrain_key_depth, s_nr.drain_tex, 2);
        rhi_bind_depth_sampler(s_nr.dev, s_nr.moby_key_depth, 3);
        if (s_nr.moby_rank_rt)
            rhi_bind_texture_set(s_nr.dev, s_nr.moby_rank_rt, 4);
    }
}

static rhi_ubo* s_shfoot_ubo;
/** @brief Bind the player fan's footprint block at set 2 of the shadow decal pipe.
 *
 * The one-block UBO is created on first use. The block is written on the
 * first bind of each RHI frame and again when the composed scene's block
 * differs from the last one written (a rewrite after a bind moves to a fresh
 * generation), so every consumer pass of one present reads the block its own
 * scene carries. Returns without binding when the UBO cannot be created. */
static void bind_shadow_footprint(void)
{
    static uint64_t s_serial = UINT64_MAX;
    static openpete_shadow_footprint s_last;
    if (!s_shfoot_ubo)
        s_shfoot_ubo = rhi_create_ubo(s_nr.dev, (uint32_t)sizeof(openpete_shadow_footprint), 1);
    if (!s_shfoot_ubo) return;
    const openpete_shadow_footprint* f = ps_shadow_foot();
    const uint64_t fs = rhi_frame_serial(s_nr.dev);
    if (fs != s_serial || memcmp(f, &s_last, sizeof s_last) != 0) {
        rhi_ubo_write(s_shfoot_ubo, 0, f, (uint32_t)sizeof *f);
        s_last = *f;
        s_serial = fs;
    }
    rhi_ubo_bind_at(s_nr.dev, s_shfoot_ubo, 2, 0);
}

/** @brief Bind the player shadow decal pipe (shadow_decal.frag) with its push block and footprint block.
 *
 * Binds gouraud_sub_pipe with the opaque depth at set 0, then pushes the
 * camera reconstruction, the depth-rule thresholds, the key-contest flag,
 * the pre-pass lookup affine and this pass's fragment-coordinate affine,
 * and binds the player fan's footprint block at set 2.
 * OPENPETE_SHADOW_DECAL_MODE overrides the mode for the calibration and
 * census views; otherwise mode 1 (the depth rule) runs when the game
 * supplied valid parameters and mode 0 (the plain fan) when it did not. */
static void bind_player_shadow_decal(void)
{
    bind_scene_pipe(s_nr.gouraud_sub_pipe);
    rhi_bind_depth_sampler(s_nr.dev, s_nr.opaque_depth, 0);
    const openpete_shadow_decal_params* sp = ps_shadow();
    shadow_decal_pc pc;
    pc.col0x = sp->cam_col0[0]; pc.col0y = sp->cam_col0[1]; pc.col0z = sp->cam_col0[2];
    pc.col1x = sp->cam_col1[0]; pc.col1y = sp->cam_col1[1]; pc.col1z = sp->cam_col1[2];
    pc.col2x = sp->cam_col2[0]; pc.col2y = sp->cam_col2[1]; pc.col2z = sp->cam_col2[2];
    pc.cam_pos_up = sp->cam_pos_up;
    pc.ofx = sp->ofx; pc.ofy = sp->ofy; pc.h = sp->h;
    pc.ground_up = sp->ground_up;
    pc.wall_height = sp->wall_height;
    pc.steep_nup = sp->steep_nup;
    pc.mode = shadow_decal_mode(sp->valid ? 1.0f : 0.0f);
    pc.band = shadow_decal_band();
    pc.plat_eps = sp->plat_eps;
    pc.plat_slack = sp->plat_slack;
    pc.key_contest = shadow_decal_bind_keys();
    pc.uax = s_od_uax; pc.ubx = s_od_ubx;
    pc.vay = s_od_vay; pc.vby = s_od_vby;
    pc.pvw = s_od_vw; pc.pvh = s_od_vh;
    scene_fc_affine fc; scene_fragcoord_affine(&fc);
    pc.fax = fc.fax; pc.fbx = fc.fbx;
    pc.fay = fc.fay; pc.fby = fc.fby;
    rhi_push_constants(s_nr.dev, &pc, sizeof pc);
    bind_shadow_footprint();
}

/** @brief Bind the key images bind_scene_pipe binds, at a material pipeline's set numbers.
 *
 * A material layout puts them above the UBO pair rather than replacing sets
 * 1 and 2, so a material's own uniform block keeps set 3 binding 1: the
 * terrain key depth paired with the drain texture at set 2, the moby key
 * depth at set 4, the actor key depth paired with its owner colour at set 5 and, when it exists, the moby
 * rank target at set 6. The engine half of openpete_psx_material.glsl
 * runs the key contest from these before the mod's op_material() sees a
 * colour, so a material keeps the painter verdicts of the stock pipes. No
 * effect unless all three key depth images exist. */
static void bind_material_keys(void)
{
    if (!s_nr.terrain_key_depth || !s_nr.moby_key_depth || !s_nr.actor_key_depth) return;
    rhi_bind_depth_tex_pair(s_nr.dev, s_nr.terrain_key_depth, s_nr.drain_tex, 2);
    rhi_bind_depth_sampler(s_nr.dev, s_nr.moby_key_depth, 4);
    rhi_bind_depth_tex_pair(s_nr.dev, s_nr.actor_key_depth, s_nr.actor_key_depth, 5);
    if (s_nr.moby_rank_rt)
        rhi_bind_texture_set(s_nr.dev, s_nr.moby_rank_rt, 6);
}

/* ---- moby occlusion culling: grid pass + readback + query -------------------
 *
 * Per LIVE present: one tiny render pass reduces the WORLD-opaque depth
 * pre-pass to an OCCL_GW×OCCL_GH grid of the FARTHEST depth per cell
 * (occl_downsample.frag - conservative: any sky/gap texel makes its cell
 * uncullable), which rides the frame's ring readback (rhi_cycle_rt_readback,
 * no GPU stall) back to the CPU RHI_FRAMES_IN_FLIGHT presents later. The moby
 * feeder then asks openpete_native_occl_query BEFORE re-extracting a record:
 * "is this screen rect at this nearest view-Z provably behind nearer terrain
 * everywhere?" - yes => it skips the model walk, scene tris and draws for a
 * moby the depth test would erase anyway.
 *
 * Staleness discipline: the grid is a FEW PRESENTS OLD, so every consumer is
 * conservative - the query inflates the rect by one cell, demands the
 * occluder be nearer by a slack (OPENPETE_MOBY_OCCL_SLACK, default 1500 world
 * units - also absorbing the terrain feeder's measured off-anchor depth skew),
 * clamps the inflated rect to the grid, since its off-grid part is outside
 * the visible extent, refuses a query whose camera moved or turned past the
 * grid's camera, and refuses grids older than 8 presents
 * (freeze/menu/minimized). The feeder additionally suspends
 * culling around camera cuts (openpete_native_occl_suspend) and widens its
 * rects with camera pan speed. The affine mapping PSX-screen->grid-uv is
 * FIFO'd alongside each queued copy so a retrieved grid decodes with the
 * mapping it was rendered under (it only changes on window/layout changes).
 *
 * Display-only by construction: consumers skip native drawing of provably
 * hidden geometry; nothing here reads back into gameplay state. */
/** @brief Return the view-Z margin by which occluding terrain must be nearer than a queried moby.
 *
 * OPENPETE_MOBY_OCCL_SLACK sets it, default 1500 world units; the value is
 * read once. openpete_native_occl_query subtracts it from the rect's
 * nearest view-Z and refuses the query when the result is 1 or less.
 * @return The slack in view-Z units. */
static float occl_slack(void)
{
    static float s = -1.0f;
    if (s < 0.0f) { const char* e = getenv("OPENPETE_MOBY_OCCL_SLACK"); s = (e && *e) ? (float)atof(e) : 1500.0f; }
    return s;
}

/** @brief Return the camera translation bound of an occlusion query.
 *
 * OPENPETE_OCCL_CAM_MOVE sets it in world units, default 24; the value is
 * read once. occl_cam_in_bound refuses a query whose camera lies farther than
 * this from the grid's camera.
 * @return The bound in world units. */
static float occl_cam_move(void)
{
    static float m = -1.0f;
    if (m < 0.0f) { const char* e = getenv("OPENPETE_OCCL_CAM_MOVE"); m = (e && *e) ? (float)atof(e) : 24.0f; }
    return m;
}
/** @brief Return the camera rotation bound of an occlusion query as a matrix trace.
 *
 * For two rotations trace(Rq Rg^T) = 1 + 2 cos(theta), so occl_cam_in_bound
 * tests one dot product over the nine matrix elements against this
 * threshold, with no trigonometry per query. OPENPETE_OCCL_CAM_TURN sets
 * the angle in degrees, default 0.35; the value is read once.
 * @return 1 + 2 cos of the bound angle. */
static float occl_cam_turn_trace(void)
{
    static float t = -1e30f;
    if (t < -1e29f) {
        const char* e = getenv("OPENPETE_OCCL_CAM_TURN");
        double deg = (e && *e) ? atof(e) : 0.35;
        t = (float)(1.0 + 2.0 * cos(deg * 3.14159265358979323846 / 180.0));
    }
    return t;
}

/** @brief Return whether OPENPETE_OCCL_CAMDBG is set to a non-zero value.
 *
 * Enables the occlusion query camera diagnostics. Read once and cached.
 * @return 1 when on, else 0. */
static int occl_camdbg(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_OCCL_CAMDBG"); v = (e && *e && *e != '0'); }
    return v;
}

/* The camera the walk currently feeding the scene projects with. Published by
 * the terrain feeder every extract (sub-tick re-extracts included) and
 * snapshotted per scene, so the grid built from a present's depth pre-pass
 * carries the camera that pre-pass was rasterized under. */
static openpete_occl_camera s_occl_cam_live;
void openpete_native_occl_set_camera(const openpete_occl_camera *c)
{
    if (c) s_occl_cam_live = *c; else s_occl_cam_live.valid = 0;
}

typedef struct { float uax, ubx, vay, vby; unsigned stamp;
                 openpete_occl_camera cam; } occl_meta_t;
#define OCCL_FIFO_N 16
/* One decoded occluder grid: the cell depths with the screen affine, the
 * present stamp and the camera they were rendered under. The fields are
 * only meaningful together (a cell depth read through another grid's
 * affine or camera breaks the conservative-cull guarantee), so they
 * cross threads as one struct. */
typedef struct {
    float vz[OCCL_GW * OCCL_GH];        /* decoded farthest view-Z per cell */
    float uax, ubx, vay, vby;           /* PSX screen -> extent affine */
    unsigned stamp;                     /* present the grid was captured at */
    openpete_occl_camera cam;           /* camera the grid was rendered from */
    int have;                           /* 0 until the first grid decodes */
} occl_grid_t;
/* Publish protocol. The presenting thread decodes into s_occl_dec, then
 * copies it into s_occl_pub under s_occl_pub_mx and bumps s_occl_pub_seq
 * (release). The querying thread (the extraction on the guest thread)
 * copies s_occl_pub into s_occl_rd under the same mutex when the sequence
 * moved, then answers every query from s_occl_rd, which only it touches.
 * The present counter and the cut suspension are single atomics. */
static occl_grid_t   s_occl_dec;                 /* presenting thread only */
static occl_grid_t   s_occl_pub;                 /* under s_occl_pub_mx */
static occl_grid_t   s_occl_rd;                  /* querying thread only */
static void *s_occl_pub_mx;                   /* SDL_Mutex, openpete_mutex_once */
static SDL_AtomicInt s_occl_pub_seq;             /* bumped per publish */
static int           s_occl_rd_seq;              /* querying thread: sequence copied */
static SDL_AtomicInt s_occl_presents;            /* live present counter */
static SDL_AtomicInt s_occl_suspend;             /* presents left of cut suspension */
static occl_meta_t s_occl_fifo[OCCL_FIFO_N];     /* affine+stamp queued with each copy */
static unsigned    s_occl_fifo_head = 0, s_occl_fifo_tail = 0;


/** @brief Publish the presenting thread's decoded grid for the querying thread. */
static void occl_grid_publish(void)
{
    SDL_Mutex *mx = openpete_mutex_once(&s_occl_pub_mx);
    SDL_LockMutex(mx);
    s_occl_pub = s_occl_dec;
    SDL_UnlockMutex(mx);
    SDL_AddAtomicInt(&s_occl_pub_seq, 1);
}

/** @brief Bring the querying thread's grid copy up to the latest publish.
 *
 * @return The grid every query of this call reads. */
static const occl_grid_t *occl_grid_reader(void)
{
    const int seq = SDL_GetAtomicInt(&s_occl_pub_seq);
    if (seq != s_occl_rd_seq) {
        SDL_Mutex *mx = openpete_mutex_once(&s_occl_pub_mx);
        SDL_LockMutex(mx);
        s_occl_rd = s_occl_pub;
        SDL_UnlockMutex(mx);
        s_occl_rd_seq = seq;
    }
    return &s_occl_rd;
}

/** @brief Decrement an atomic counter that is above zero; a counter at zero stays. */
static void occl_atomic_dec_positive(SDL_AtomicInt *a)
{
    for (;;) {
        const int v = SDL_GetAtomicInt(a);
        if (v <= 0 || SDL_CompareAndSwapAtomicInt(a, v, v - 1)) return;
    }
}

/** @brief Reduce this present's depth pre-pass into the grid, retrieve and publish an older grid, and queue this present's metadata.
 *
 * Runs on the presenting thread. The retrieved grid is decoded into
 * s_occl_dec with the affine, stamp and camera queued with it, then
 * published whole (occl_grid_publish). */
static void native_occl_downsample(void)
{
    const unsigned presents = (unsigned)SDL_AddAtomicInt(&s_occl_presents, 1) + 1u;
    occl_atomic_dec_positive(&s_occl_suspend);
    if (!occl_active() || !s_od_ready || !s_nr.fs_quad_vb) return;

    /* Reduce this present's pre-pass into the grid RT (viewport = full RT). */
    struct { int gw, gh, pvw, pvh; } pc =
        { OCCL_GW, OCCL_GH, (int)s_od_vw, (int)s_od_vh };
    rhi_begin_render_target(s_nr.dev, s_nr.occl_grid, 0.0f, 0.0f, 0.0f, 1.0f);
    bind_scene_pipe(s_nr.occl_pipe);
    rhi_bind_texture_set(s_nr.dev, s_nr.opaque_depth, 0);   /* WORLD-only packed depth */
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
            s_occl_dec.vz[i] = (q >= 16777200u) ? 3.4e38f
                             : 1.0f / (1.0f - (float)q / 16777215.0f);
        }
        s_occl_dec.uax = m->uax; s_occl_dec.ubx = m->ubx;
        s_occl_dec.vay = m->vay; s_occl_dec.vby = m->vby;
        s_occl_dec.stamp = m->stamp;
        s_occl_dec.cam = m->cam;
        s_occl_dec.have = 1;
        occl_grid_publish();
        if (occl_camdbg())
            fprintf(stderr, "[occl-grid] sframe=%u present=%u stamp=%u age=%u valid=%d "
                    "gcam=(%.0f,%.0f,%.0f)\n", s_scene_frame, presents, m->stamp,
                    presents - m->stamp, s_occl_dec.cam.valid,
                    s_occl_dec.cam.pos[0], s_occl_dec.cam.pos[1], s_occl_dec.cam.pos[2]);
    } else if (got) {
        s_occl_fifo_tail = s_occl_fifo_head;   /* desync (shouldn't happen) - resync empty */
    }
    if (s_occl_fifo_head - s_occl_fifo_tail >= OCCL_FIFO_N) s_occl_fifo_tail++; /* overflow: drop oldest */
    occl_meta_t* mm = &s_occl_fifo[s_occl_fifo_head % OCCL_FIFO_N];
    mm->uax = s_od_uax; mm->ubx = s_od_ubx; mm->vay = s_od_vay; mm->vby = s_od_vby;
    mm->stamp = presents;
    mm->cam = *ps_occl_cam();
    if (occl_camdbg())
        fprintf(stderr, "[occl-queue] sframe=%u present=%u slot=%d valid=%d "
                "qcam=(%.0f,%.0f,%.0f)\n", s_scene_frame, presents, s_ps_slot ? 1 : 0,
                mm->cam.valid, mm->cam.pos[0], mm->cam.pos[1], mm->cam.pos[2]);
    s_occl_fifo_head++;
}

static long s_occl_qstat[OPENPETE_OCCL_Q_COUNT];   /* query-outcome census */
static float s_occl_last_scan_max;                 /* diagnostics (last CULLED scan) */

/** @brief Return whether a querying camera is close enough to the grid's camera for the grid's cell depths to describe its view.
 *
 * The translation bound (occl_cam_move, the parallax term) is tested first,
 * then the relative rotation against occl_cam_turn_trace. Either side
 * lacking a valid camera is a mismatch, so an unpublished camera culls
 * nothing.
 * @param G The grid.
 * @param q Querying camera; NULL is a mismatch.
 * @return 1 when both bounds hold, else 0. */
static int occl_cam_in_bound(const occl_grid_t *G, const openpete_occl_camera *q)
{
    const openpete_occl_camera *g = &G->cam;
    if (!q || !q->valid || !g->valid) return 0;
    float dx = q->pos[0] - g->pos[0], dy = q->pos[1] - g->pos[1], dz = q->pos[2] - g->pos[2];
    float mv = occl_cam_move();
    if (dx*dx + dy*dy + dz*dz > mv*mv) return 0;
    float tr = 0.0f;
    for (int k = 0; k < 9; k++) tr += q->M[k] * g->M[k];
    return tr >= occl_cam_turn_trace();
}

/** @brief Return the query-outcome index that refuses every occlusion query on the held grid.
 *
 * OPENPETE_OCCL_Q_NOGRID when no grid is held, OPENPETE_OCCL_Q_SUSPENDED
 * while grid use is suspended, OPENPETE_OCCL_Q_STALE when the grid is more
 * than 8 presents old.
 * @param G The grid.
 * @return The refusing outcome index, or -1 when the grid and its screen
 *         affine are usable. */
static int occl_grid_refusal(const occl_grid_t *G)
{
    if (!G->have) return OPENPETE_OCCL_Q_NOGRID;
    if (SDL_GetAtomicInt(&s_occl_suspend) > 0) return OPENPETE_OCCL_Q_SUSPENDED;
    if ((unsigned)SDL_GetAtomicInt(&s_occl_presents) - G->stamp > 8u) return OPENPETE_OCCL_Q_STALE;
    return -1;
}

typedef struct { float u0, u1, v0, v1; } occl_uv_rect;

/** @brief Map a PSX screen rectangle through the held grid's affine into extent coordinates.
 *
 * The visible extent spans [0,1] on each axis; each axis of the result is
 * ordered low to high.
 * @param G The grid.
 * @param sx0 First x edge in PSX screen pixels.
 * @param sy0 First y edge in PSX screen pixels.
 * @param sx1 Second x edge in PSX screen pixels.
 * @param sy1 Second y edge in PSX screen pixels.
 * @return The mapped rectangle. */
static occl_uv_rect occl_rect_uv(const occl_grid_t *G, float sx0, float sy0, float sx1, float sy1)
{
    occl_uv_rect r = { G->uax * sx0 + G->ubx, G->uax * sx1 + G->ubx,
                       G->vay * sy0 + G->vby, G->vay * sy1 + G->vby };
    if (r.u1 < r.u0) { float t = r.u0; r.u0 = r.u1; r.u1 = t; }
    if (r.v1 < r.v0) { float t = r.v0; r.v0 = r.v1; r.v1 = t; }
    return r;
}

/** @brief Return whether a mapped rectangle lies wholly outside the visible extent.
 *
 * Such a rectangle contributes zero pixels regardless of depth; the visible
 * extent is the one the grid's affine maps, widened under Hor+. The game's
 * view cone is wider than the screen, so enclosed areas queue many of
 * these. A projection behind the camera or straddling the near plane has
 * meaningless screen coordinates, so a rectangle whose nearest view-Z is
 * 256 or less is never reported offscreen; @p vz_near already carries the
 * caller's staleness slack.
 * @param r Rectangle in extent coordinates (occl_rect_uv).
 * @param vz_near Nearest view-Z of the geometry.
 * @return 1 when offscreen, else 0. */
static int occl_uv_offscreen(const occl_uv_rect *r, float vz_near)
{
    return vz_near > 256.0f && (r->u1 < 0.0f || r->u0 > 1.0f || r->v1 < 0.0f || r->v0 > 1.0f);
}

int openpete_native_occl_query(float sx0, float sy0, float sx1, float sy1, float vz_near,
                               const openpete_occl_camera *cam)
{
    const occl_grid_t *G = occl_grid_reader();
    int refusal = occl_grid_refusal(G);
    if (refusal >= 0) { s_occl_qstat[refusal]++; return 0; }
    occl_uv_rect r = occl_rect_uv(G, sx0, sy0, sx1, sy1);
    if (occl_uv_offscreen(&r, vz_near)) {
        s_occl_qstat[OPENPETE_OCCL_Q_OFFSCREEN]++;
        return 1;
    }
    /* Past the frustum reject, which is a test on the caller's own rect and
     * needs no depth: every verdict below reads the grid's cells, so it is
     * only valid while the two cameras agree. */
    if (!occl_cam_in_bound(G, cam)) { s_occl_qstat[OPENPETE_OCCL_Q_MOVED]++; return 0; }
    if (occl_camdbg() && cam) {
        const openpete_occl_camera *gc = &G->cam;
        float dx = cam->pos[0] - gc->pos[0];
        float dy = cam->pos[1] - gc->pos[1];
        float dz = cam->pos[2] - gc->pos[2];
        float tr = 0.0f; for (int k = 0; k < 9; k++) tr += cam->M[k] * gc->M[k];
        fprintf(stderr, "[occl-inbound] sframe=%u age=%u d=%.1f tr=%.5f "
                "gcam=(%.0f,%.0f,%.0f) qcam=(%.0f,%.0f,%.0f)\n",
                s_scene_frame, (unsigned)SDL_GetAtomicInt(&s_occl_presents) - G->stamp,
                sqrtf(dx*dx + dy*dy + dz*dz), tr,
                gc->pos[0], gc->pos[1], gc->pos[2],
                cam->pos[0], cam->pos[1], cam->pos[2]);
    }
    float need = vz_near - occl_slack();
    if (need <= 1.0f) { s_occl_qstat[OPENPETE_OCCL_Q_TOONEAR]++; return 0; }
    /* One-cell inflation, then CLAMP to the grid: the off-grid part of a rect
     * is outside the visible extent (zero pixels by definition), so only the
     * on-grid slice needs the depth test. All-near cells there => every pixel
     * the moby could produce is behind nearer opaque WORLD => cullable. */
    int cx0 = (int)floorf(r.u0 * (float)OCCL_GW) - 1, cx1 = (int)floorf(r.u1 * (float)OCCL_GW) + 1;
    int cy0 = (int)floorf(r.v0 * (float)OCCL_GH) - 1, cy1 = (int)floorf(r.v1 * (float)OCCL_GH) + 1;
    if (cx0 < 0) cx0 = 0; if (cy0 < 0) cy0 = 0;
    if (cx1 >= OCCL_GW) cx1 = OCCL_GW - 1;
    if (cy1 >= OCCL_GH) cy1 = OCCL_GH - 1;
    float scan_max = 0.0f;
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++) {
            float c = G->vz[cy * OCCL_GW + cx];
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

int openpete_native_occl_grid_age(void)
{
    /* Any thread (the present census asks too): read the published grid
     * under its mutex, not the querying thread's copy. */
    SDL_Mutex *mx = openpete_mutex_once(&s_occl_pub_mx);
    SDL_LockMutex(mx);
    const int have = s_occl_pub.have;
    const unsigned stamp = s_occl_pub.stamp;
    SDL_UnlockMutex(mx);
    return have ? (int)((unsigned)SDL_GetAtomicInt(&s_occl_presents) - stamp) : -1;
}

void openpete_native_occl_qstats(long out[OPENPETE_OCCL_Q_COUNT])
{
    for (int i = 0; i < OPENPETE_OCCL_Q_COUNT; i++) out[i] = s_occl_qstat[i];
}

void openpete_native_occl_suspend(int presents)
{
    for (;;) {   /* raise to presents; the presenting thread decrements concurrently */
        const int v = SDL_GetAtomicInt(&s_occl_suspend);
        if (presents <= v || SDL_CompareAndSwapAtomicInt(&s_occl_suspend, v, presents)) return;
    }
}

/* Hor+ widen factor; documented at the declaration. */
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
/* Visible PSX screen-X range; documented at the declaration. */
void openpete_native_widescreen_x_bounds(float *xlo, float *xhi)
{
    float F = openpete_native_widescreen_factor();
    if (xlo) *xlo = 256.0f - 256.0f * F;
    if (xhi) *xhi = 256.0f + 256.0f * F;
}

/* Visible PSX screen-Y range; documented at the declaration. */
void openpete_native_visible_y_bounds(float *ylo, float *yhi)
{
    const float disp_h = s_geom_in[3] > 0 ? (float)s_geom_in[3] : 240.0f;
    const float clip_h = s_geom_in[7] > 0 ? (float)s_geom_in[7] : 224.0f;
    const float band = (disp_h - clip_h) * 0.5f;
    if (ylo) *ylo = band;
    if (yhi) *yhi = band + clip_h;
}

/** @brief Compute the clip-space rect the full-frame cover quads span.
 *
 * The cover quads are the background fill, the fade and the freeze
 * backdrop. The rect is the --aspect render rect, the 4:3 sub-rect widened
 * by the Hor+ factor, so the revealed side strips get a background behind
 * the sky dome and world while the letterbox and pillarbox bars beyond it
 * stay black. In full-extent mode (native_rtt) it is the whole target.
 * @param full_extent Nonzero to span the whole target.
 * @param l Receives the left edge.
 * @param r Receives the right edge.
 * @param t Receives the top edge.
 * @param b Receives the bottom edge. */
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
 * is linked before the text/icon glyphs - e.g. the flight results green backdrop
 * UNDER the "CRASHED" text). Drawn either UNDER the player (gi==2, GS_LevelTransition
 * flyby so Spyro flies in front of the revealed text) or, by default, OVER the player
 * (after the whole scene) - see hud_under in draw_native_scene. The 3D HUD icons
 * (HUD3D) are a separate depth-isolated pass drawn after this either way. */
/* Textured-tri material batching (see the bucket build in render_scene). The
 * scene index of each textured block-tri, in VB order, so the draw side can read
 * a tri's material and coalesce adjacent same-material runs into one draw. */
#define s_txscene ((int *)s_cz_txscene.data)
/* Textured draw buckets the composer partitions the scene into (see the
 * bucket table above the partition loop in draw_native_scene). */
#define TX_BUCKETS 14
/* Set for a scene triangle the untextured bucket build deferred into a mod
 * material span. A deferred triangle draws from that span at its group's slot,
 * so the emission-ordered blended-world block leaves it out. */
#define s_bw_deferred ((uint8_t *)s_cz_bwdef.data)
/* Flat-HUD within-layer painter order (see the bucket-6 build in render_scene):
 * back-to-front by centroid z, ties broken by submission index so equal-z runs
 * (all the 2D-UI content at z=0) keep submission order - a STABLE descending
 * sort built on qsort. */
typedef struct { float z; int t; } hud_paint_t;
/** @brief Return a HUD triangle's painter key.
 *
 * The key is the mean of its vertices' hyperbolic depths
 * vz / (vz + OPENPETE_DEPTH_VIEWZ_SCALE), the curve that orders the flat
 * HUD layer back to front.
 * @param T HUD triangle.
 * @return The painter key. */
static float hud_paint_depth(const scene_tri_t* T)
{
    float d = 0.0f;
    for (int k = 0; k < 3; k++)
        d += T->v[k].z / (T->v[k].z + OPENPETE_DEPTH_VIEWZ_SCALE);
    return d * (1.0f / 3.0f);
}
typedef struct { mod_fvec *v; size_t elem; int verts; const char *name; } cz_run_t;
/** @brief Grow every compose scratch run and the scene and pre-pass GPU buffers to hold a scene.
 *
 * Vertex runs are sized 3 per triangle plus SCENE_VB_EXTRA and index runs one
 * per triangle; a run is initialized on its first call. A growth moves a
 * run, so no pointer into one survives the call. The result is bounded by
 * the smallest capacity any run or GPU buffer slot reached, and records the
 * smallest vertex and triangle capacities in s_cz_verts and s_cz_tris.
 * @param scene_n Scene triangle count; a negative count is 0.
 * @return The triangle count the compose can draw: scene_n unless an
 *         allocation failed, in which case the prefix that fits; the failure
 *         is logged by the run or the RHI with the run's name. */
static int compose_reserve(int scene_n)
{
    const cz_run_t runs[] = {
        { &s_cz_vb,       sizeof(rhi_vertex), 1, "compose.scene_vb" },
        { &s_cz_prepass,  sizeof(rhi_vertex), 1, "compose.prepass_vb" },
        { &s_cz_key[0],   sizeof(rhi_vertex), 1, "compose.key_terrain" },
        { &s_cz_key[1],   sizeof(rhi_vertex), 1, "compose.key_prop" },
        { &s_cz_key[2],   sizeof(rhi_vertex), 1, "compose.key_actor" },
        { &s_cz_key[3],   sizeof(rhi_vertex), 1, "compose.key_rank" },
        { &s_cz_ktex,     sizeof(int),        0, "compose.key_tex_idx" },
        { &s_cz_txscene,  sizeof(int),        0, "compose.tex_scene_idx" },
        { &s_cz_txout,    sizeof(int),        0, "compose.tex_sort_out" },
        { &s_cz_txof,     sizeof(uint16_t),   0, "compose.tex_sort_slot" },
        { &s_cz_utdef,    sizeof(int),        0, "compose.ut_deferred_idx" },
        { &s_cz_bwdef,    sizeof(uint8_t),    0, "compose.bw_deferred" },
        { &s_cz_bwsort,   sizeof(int),        0, "compose.bw_sort_tmp" },
        { &s_cz_hud,      sizeof(hud_paint_t), 0, "compose.hud_paint" },
    };
    if (scene_n < 0) scene_n = 0;
    const int want_v = scene_n * 3 + SCENE_VB_EXTRA;
    int held_v = want_v, held_t = scene_n;
    for (size_t i = 0; i < sizeof runs / sizeof runs[0]; i++) {
        const cz_run_t *r = &runs[i];
        if (!r->v->elem_size)
            mod_fvec_init(r->v, r->elem, r->verts ? SCENE_RUN_INIT * 3 : SCENE_RUN_INIT,
                          r->name, r->verts ? SCENE_VB_WAS : SCENE_TRIS_WAS);
        (void)mod_fvec_reserve_noted(r->v, (size_t)(r->verts ? want_v : scene_n));
        int *held = r->verts ? &held_v : &held_t;
        if ((int)r->v->cap < *held) *held = (int)r->v->cap;
    }
    rhi_buffer *const gpu[] = { s_nr.scene_vb, s_nr.prepass_vb };
    for (size_t i = 0; i < sizeof gpu / sizeof gpu[0]; i++) {
        if (!gpu[i] || rhi_vertex_buffer_reserve(s_nr.dev, gpu[i], sizeof(rhi_vertex) * (size_t)want_v))
            continue;
        const int slot_v = (int)(rhi_vertex_buffer_slot_bytes(gpu[i]) / sizeof(rhi_vertex));
        if (slot_v < held_v) held_v = slot_v;
    }
    s_cz_verts = held_v; s_cz_tris = held_t;
    int fit = (held_v - SCENE_VB_EXTRA) / 3;
    if (held_t < fit) fit = held_t;
    if (fit < 0) fit = 0;
    return fit < scene_n ? fit : scene_n;
}

/** @brief Order HUD painter entries farther first, ties by submission index.
 *
 * qsort comparator over hud_paint_t: descending z, then ascending scene
 * index, so equal-depth runs keep submission order. */
static int hud_paint_cmp(const void* pa, const void* pb) {
    const hud_paint_t* a = (const hud_paint_t*)pa;
    const hud_paint_t* b = (const hud_paint_t*)pb;
    if (a->z > b->z) return -1;   /* farther first */
    if (a->z < b->z) return 1;
    return (a->t < b->t) ? -1 : (a->t > b->t) ? 1 : 0;
}

static const scene_tri_t* s_txsort_scene;   /* qsort context (set per call) */
/** @brief Order two scene triangle indices by material for qsort.
 *
 * Compares tpage_y, tpage_x, clut_y, clut_x and bpp in that priority, then the
 * provenance tag while tag_split_on holds. The tag term must match the run
 * break in draw_tex_runs. Ties compare equal. Reads the scene through
 * s_txsort_scene.
 * @param pa Pointer to an int scene index.
 * @param pb Pointer to an int scene index.
 * @return Negative, zero or positive as a sorts before, with or after b. */
static int tex_mat_cmp(const void* pa, const void* pb) {
    const scene_tri_t* a = &s_txsort_scene[*(const int*)pa];
    const scene_tri_t* b = &s_txsort_scene[*(const int*)pb];
    if (a->tpage_y != b->tpage_y) return a->tpage_y < b->tpage_y ? -1 : 1;
    if (a->tpage_x != b->tpage_x) return a->tpage_x < b->tpage_x ? -1 : 1;
    if (a->clut_y  != b->clut_y ) return a->clut_y  < b->clut_y  ? -1 : 1;
    if (a->clut_x  != b->clut_x ) return a->clut_x  < b->clut_x  ? -1 : 1;
    if (a->bpp     != b->bpp    ) return a->bpp     < b->bpp     ? -1 : 1;
    /* Provenance tag joins the key ONLY when a split consumer is armed
     * (registered selector / OPENPETE_TAG_SPLIT) - MUST stay in lockstep
     * with the coalesce run-break in draw_tex_runs, or batches that this
     * sort separates get re-merged (or worse, interleave and split into
     * many runs). Inert => byte-identical batching without mods. */
    if (tag_split_on() && a->tag != b->tag)
        return a->tag < b->tag ? -1 : 1;
    return 0;
}

/* Material sort, counting-sort form. Comparison-sorting ~15k tri indices per
 * present costs ~750us of every compose; the key space is actually a few
 * dozen distinct materials, so this interns each tri's material to a bucket
 * (one hash probe per tri), orders the UNIQUE materials with the comparator
 * semantics above (tpage_y, tpage_x, clut_y, clut_x, bpp[, tag] - the packed
 * key preserves exactly that priority), and scatters - O(n + m log m).
 * Stable within a material (submission order), which qsort never guaranteed;
 * grouping and cross-material order match tex_mat_cmp exactly. Falls back to
 * qsort if the bucket table overflows (pathological material count). */
#define TXMAT_BUCKETS 1024   /* power of two; distinct materials per scene ~dozens */
typedef struct { uint64_t key; uint32_t tag; int count, offset, used; } txmat_bucket_t;
static txmat_bucket_t s_txmat_b[TXMAT_BUCKETS];
#define s_txmat_of  ((uint16_t *)s_cz_txof.data)    /* tri -> bucket slot */
#define s_txmat_out ((int *)s_cz_txout.data)        /* scatter target */

/** @brief Order two material-bucket slots by key, then tag, for qsort.
 *
 * Reads the buckets through s_txmat_b.
 * @param pa Pointer to a uint16_t bucket slot.
 * @param pb Pointer to a uint16_t bucket slot.
 * @return Negative, zero or positive as a sorts before, with or after b. */
static int txmat_bucket_cmp(const void* pa, const void* pb) {
    const txmat_bucket_t* a = &s_txmat_b[*(const uint16_t*)pa];
    const txmat_bucket_t* b = &s_txmat_b[*(const uint16_t*)pb];
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    if (a->tag != b->tag) return a->tag < b->tag ? -1 : 1;
    return 0;
}

/** @brief Reorder scene indices so triangles of one material are contiguous, in material-key order.
 *
 * Interns each triangle's material (packed tpage_y, tpage_x, clut_y, clut_x,
 * bpp, plus the tag while tag_split_on holds) into a 1024-bucket open-
 * addressing table, sorts the distinct materials by key, then scatters the
 * indices, stable within a material. The packed key gives the same grouping and
 * order as tex_mat_cmp for fields within VRAM extents. When the table reaches
 * TXMAT_BUCKETS - 1 materials it falls back to qsort with tex_mat_cmp. Uses
 * static tables and the scratch runs s_cz_txof and s_cz_txout, so it is not
 * reentrant.
 * @param scene Scene triangles.
 * @param idx Scene indices, reordered in place.
 * @param n Number of indices; at most 1 does nothing. */
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
 * TRI - run-union boxes vary with per-frame clipping); the pack match is
 * containment-based (texture_pack_match), so one tile-sized file covers LP
 * full-tile draws, HP sub-quad draws, and partially-clipped boxes alike. */

/* ---- HD replacement GPU-texture cache ------------------------------------
 * identity hashkey -> uploaded rhi texture, open addressing with a 16-slot
 * linear probe. Pack files and mod textures key on their pixel pointer,
 * which their owners keep for the session, so those entries stay resident.
 * Framebuffer-snapshot entries key on the snapshot store's per-image key
 * and are evicted once the store no longer holds that image
 * (hd_evict_fb_snapshots); an evicted slot becomes a tombstone so probe
 * chains through it stay intact, and its texture is retired on the frame
 * fence schedule (rhi_destroy_texture_deferred). A full neighbourhood keeps
 * the incumbents and the newcomer draws stock. */
#define HD_CACHE_SLOTS 8192   /* pack sizes reach thousands game-wide; probe-16 needs headroom */
#define HD_KEY_TOMB UINT64_MAX   /* evicted slot: probing continues past it */
static struct { uint64_t key; rhi_texture *tex; } s_hd_cache[HD_CACHE_SLOTS];
/* Cache slots holding framebuffer-snapshot textures. The store holds at
 * most 8 images at once, so live entries stay far below the bound; an
 * insert past it is dropped and that run draws stock. */
#define HD_FB_MAX 32
static int s_hd_fb_slot[HD_FB_MAX];
static int s_hd_fb_n;

/* GPU uploads cannot record inside the offscreen render pass, so a first
 * sighting at draw time only QUEUES the image; hd_flush_pending() uploads at
 * the next frame start (one SD frame on first appearance). */
/* box_w/box_h: the texel box this image replaces. An image that is an exact
 * integer multiple of it samples NEAREST - block edges then coincide with
 * texel edges, so a dump fed straight back in renders bit-identically.
 * Anything else (arbitrary-resolution HD art) keeps LINEAR smoothing. */
/* own: a copy the queue owns and frees (framebuffer snapshots, whose
 * source image the guest thread may free before the upload); NULL for
 * pack and mod pixels, which their owners keep for the session. */
static struct {
    uint64_t key; const unsigned char *px; unsigned char *own;
    int w, h, box_w, box_h;
} s_hd_pending[512];
static int s_hd_pending_n = 0;

/** @brief Return the uploaded texture cached under an identity key, or NULL.
 *
 * The probe starts at the multiplicative hash of the key and visits at most
 * 16 consecutive slots. It stops at the first empty slot (key 0) and passes
 * tombstones.
 * @param key Identity key; 0 marks an empty slot and is never stored.
 * @return The texture, or NULL when the key is absent or its probe window
 *         is full. */
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

/** @brief Return whether a key is queued for upload.
 *
 * @param key Identity key.
 * @return 1 when queued, else 0. */
static int hd_pending_has(uint64_t key)
{
    for (int i = 0; i < s_hd_pending_n; i++)
        if (s_hd_pending[i].key == key) return 1;
    return 0;
}

/** @brief Queue an image for upload at the next flush, unless its key is already queued or the queue is full.
 *
 * @param key Identity key.
 * @param px Pixels the owner keeps alive until the upload (pack, mod), or NULL with @p own.
 * @param own Pixels the queue takes ownership of, or NULL; freed when not queued.
 * @param w Width.
 * @param h Height.
 * @param box_w Width of the texel box the image replaces, 0 for none.
 * @param box_h Height of that box. */
static void hd_pending_add(uint64_t key, const unsigned char *px, unsigned char *own,
                           int w, int h, int box_w, int box_h)
{
    if (s_hd_pending_n >= 512 || hd_pending_has(key)) { free(own); return; }
    s_hd_pending[s_hd_pending_n++] =
        (__typeof__(s_hd_pending[0])){ key, own ? own : px, own, w, h, box_w, box_h };
}

/** @brief Return the slot a key would take in the HD cache.
 *
 * @param key Identity key.
 * @return The first empty or tombstone slot of the key's probe window, or
 *         -1 when the key is already cached or the window is full. */
static int hd_cache_insert_slot(uint64_t key)
{
    const uint64_t base = key * 0x9E3779B97F4A7C15ull >> 32;
    int free_slot = -1;
    for (int p = 0; p < 16; p++) {
        const int c = (int)((base + (uint64_t)p) & (HD_CACHE_SLOTS - 1));
        if (s_hd_cache[c].key == key) return -1;
        if (s_hd_cache[c].key == HD_KEY_TOMB) { if (free_slot < 0) free_slot = c; continue; }
        if (s_hd_cache[c].key == 0) return free_slot >= 0 ? free_slot : c;
    }
    return free_slot;
}

/** @brief Retire the cached textures of framebuffer-snapshot images the store no longer holds, and drop their queued uploads.
 *
 * Runs on the presenting thread at flush time; the texture of an evicted
 * entry is destroyed once the frames that may still sample it retire. */
static void hd_evict_fb_snapshots(void)
{
    for (int i = 0; i < s_hd_fb_n; ) {
        const int c = s_hd_fb_slot[i];
        if (openpete_fb_snapshot_key_live(s_hd_cache[c].key)) { i++; continue; }
        rhi_texture *t = s_hd_cache[c].tex;
        rhi_destroy_texture_deferred(s_nr.dev, t);
        s_hd_cache[c].tex = NULL;
        s_hd_cache[c].key = HD_KEY_TOMB;
        s_hd_fb_slot[i] = s_hd_fb_slot[--s_hd_fb_n];
    }
    int n = 0;
    for (int i = 0; i < s_hd_pending_n; i++) {
        if (s_hd_pending[i].own && !openpete_fb_snapshot_key_live(s_hd_pending[i].key)) {
            free(s_hd_pending[i].own);
            continue;
        }
        s_hd_pending[n++] = s_hd_pending[i];
    }
    s_hd_pending_n = n;
}

/* HD replacement textures created per frame. Each entry is its own image:
 * vkCreateImage plus a device allocation and a view, which is driver-side
 * work that shows up in neither a GPU zone nor the create call's own return.
 * A pack-covered level entered with the pack resident queues its whole
 * visible set at once - ~310 images, ~85 ms in one frame, past the pacer's
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

/** @brief Create and upload queued HD replacement textures, at most the per-present cap, after evicting dead snapshot textures.
 *
 * Call outside any render pass. Entries past the cap stay queued for the
 * next present; an owned copy is freed once staged or dropped.
 */
static void hd_flush_pending(void)
{
    static int cap = -1;
    if (cap < 0) {
        cap = HD_CREATE_PER_FRAME;
        const char *e = getenv("OPENPETE_HD_CREATE_CAP");
        if (e && *e) { int v = atoi(e); if (v > 0) cap = v; }
    }
    hd_evict_fb_snapshots();
    int made = 0;
    int i = 0;
    for (; i < s_hd_pending_n; i++) {
        if (made >= cap) break;
        uint64_t key = s_hd_pending[i].key;
        unsigned char *own = s_hd_pending[i].own;
        int idx = hd_cache_insert_slot(key);
        if (idx >= 0 && own && s_hd_fb_n >= HD_FB_MAX) idx = -1;
        if (idx < 0) {                          /* already uploaded, or no room */
            static int drops = 0;
            if (drops < 8 && hd_cache_lookup(key) == NULL)
                fprintf(stderr, "[texpack] GPU cache insert dropped (key=%016llx, %d)\n",
                        (unsigned long long)key, ++drops);
            free(own);
            continue;
        }
        rhi_texture *t = rhi_create_rgba8(s_nr.dev, (uint32_t)s_hd_pending[i].w,
                                          (uint32_t)s_hd_pending[i].h);
        if (!t) { free(own); continue; }
        rhi_update_rgba8(s_nr.dev, t, s_hd_pending[i].px);
        free(own);                              /* staged: the upload copied it */
        /* Filter choice. An image that is an exact integer multiple of the
         * texel box it replaces samples NEAREST: its blocks align with texel
         * boundaries, so every screen pixel resolves to the texel the stock
         * VRAM sampler would have fetched and a dump dropped straight back in
         * renders bit-identically (measured: 0 differing pixels at 1x and at
         * 7x/12x). Everything else - arbitrary-resolution HD art, mod-
         * registered textures - keeps LINEAR, which is what it has always had.
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
        if (own) s_hd_fb_slot[s_hd_fb_n++] = idx;
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

static rhi_texture *s_hdf_atlas;   /* HD-fold atlas, defined with the fold below */

/** @brief Destroy every HD replacement texture, the HD-fold atlas and every queued upload's owned copy.
 *
 * Runs at renderer shutdown with the device idle, before the device is
 * destroyed. */
static void hd_cache_shutdown(void)
{
    for (int i = 0; i < HD_CACHE_SLOTS; i++) {
        if (s_hd_cache[i].tex) rhi_destroy_texture(s_nr.dev, s_hd_cache[i].tex);
        s_hd_cache[i].tex = NULL; s_hd_cache[i].key = 0;
    }
    s_hd_fb_n = 0;
    for (int i = 0; i < s_hd_pending_n; i++) free(s_hd_pending[i].own);
    s_hd_pending_n = 0;
    rhi_destroy_texture(s_nr.dev, s_hdf_atlas);
    s_hdf_atlas = NULL;
}

/* Draw a bucket's textured vert range [first_vert, first_vert+vert_count),
 * coalescing adjacent same-material tris into one rhi_draw (one push-constant per
 * run, not per tri). Caller binds the pipeline + vram + vertex buffer; `self` is
 * the pipeline the caller bound, restored after an HD-replaced run (an HD run
 * swaps pipeline + texture, so batching survives: a replaced material is its own
 * run by construction). The same restore covers declarative and
 * per-instance material overrides (a resolved run swaps to the mod
 * pipeline, then restores).
 * NB: `self` is never a depth_bias pipeline at any current call site
 * (those are the shadow-decal/stencil pipes, not the scene_tex* pipes
 * passed here), so the dynamic-bias-invalidation-on-restore gap is
 * unreachable today - a depth_bias caller would have to re-apply its
 * bias after this function returns. */
/** @brief Return the per-axis texel replication that writes a dumped texture at square pixels.
 *
 * Screen-space 2D draws (the HUD and OVERLAY categories) map 1 texel to 1
 * framebuffer pixel. Spyro's framebuffer is 512 wide with clip_h visible
 * lines, shown at 4:3, so a texel is 4*clip_h : 3*disp_w as wide as it is
 * tall, 7:12 at 512x224. Their VRAM data is horizontally pre-squashed to
 * land on that grid, which is why a raw dump reads stretched in an image
 * viewer.
 *
 * The ratio is reduced to lowest terms and applied as integer factors
 * rather than a rounded stretch, so the replicated blocks stay aligned to
 * texel boundaries and a dump fed back in as a replacement renders
 * bit-identically. A ratio whose reduced terms exceed 64 yields 1:1. World
 * geometry is projected through a matrix that already carries the aspect,
 * so its textures are authored square and dump at 1:1.
 * @param category Draw category of the triangle.
 * @param sx Receives the horizontal factor.
 * @param sy Receives the vertical factor. */
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
 * has no palette - it is host RGBA - so that mechanism cannot reach it, and a
 * replaced tile would otherwise render at full saturation while the terrain
 * around it fades into the distance.
 *
 * The two palette rows ARE the fog specification for this texture, so derive
 * the transform from them instead of assuming a global fog colour: fit
 * out = a*in + b per channel by least squares over the palette entries. The
 * ladder turns out to be very close to affine (measured over 390 ladder steps
 * on 26 base CLUTs: mean error 2.7/255, p95 3.3, versus 34.0 for applying no
 * fog at all). Fitting per channel rather than as a blend-toward-one-colour
 * matters - the blend rate varies widely between textures (t at delta 15
 * measured 0.825 +- 0.326), so a single global fog constant would be wrong
 * for most of them.
 *
 * Entries that are the 0x0000 transparency sentinel in the base row carry no
 * colour and are excluded. A base row with no spread gives the slope no
 * signal, so fall back to a pure offset, which preserves the replacement's
 * own detail rather than flattening it to a constant. */
typedef struct { float a[3], b[3]; } tp_fog_xform;

/** @brief Fit the per-channel linear transform between a CLUT row and its fogged row.
 *
 * Least-squares fit of out = a * in + b over the 5-bit channels of the entries
 * that are not the 0x0000 sentinel, in the base row against the row at (base_y
 * + delta) & 0x1FF. A base row with no spread gives an offset-only transform.
 * The output is the identity when VRAM is unavailable, delta is outside
 * 1..TP_FOG_DELTA_MAX, clut_x or base_y is out of range, or the row would run
 * past the VRAM width.
 * @param clut_x CLUT x in VRAM.
 * @param base_y CLUT row of the unfogged palette, 0 to 511.
 * @param delta Fog rung, the row offset of the fogged palette.
 * @param bpp 8 reads 256 entries, any other value 16.
 * @param out Receives a[c] and b[c] per RGB channel; b is in 0..1 colour units. */
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

/* Deferred untextured material spans. The untextured buckets are one
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
#define s_utdef_idx ((int *)s_cz_utdef.data)

/** @brief Mask a scene tag down to the bits the material resolve reads.
 *
 * Terrain resolve ignores the sector id, so a terrain tag (path 2) keeps
 * bits 31 to 16 only and one span and one memo entry stand for each
 * material outcome rather than each sector. Moby and other tags resolve on
 * all bits.
 * @param tag Scene tag.
 * @return The masked tag. */
static uint32_t ut_mask_tag(uint32_t tag) {
    return (tag >> 28) == 2u ? (tag & 0xFFFF0000u) : tag;
}

static const scene_tri_t* s_utsort_scene;
/** @brief Order two deferred untextured scene indices by masked tag, then by index, for qsort.
 *
 * Reads tags through s_utsort_scene.
 * @param a Pointer to an int scene index.
 * @param b Pointer to an int scene index.
 * @return Negative, zero or positive as a sorts before, with or after b. */
static int utdef_cmp(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    uint32_t tx = ut_mask_tag(s_utsort_scene[x].tag);
    uint32_t ty = ut_mask_tag(s_utsort_scene[y].tag);
    if (tx != ty) return tx < ty ? -1 : 1;
    return x - y;                                   /* stable within a tag */
}

/* Per-flush memo of "does this (masked) tag resolve to a material". */
static uint32_t s_utmemo_tag[32]; static int8_t s_utmemo_hit[32]; static int s_utmemo_n;
/** @brief Return whether a triangle tag resolves to a registered mod material.
 *
 * Asks openpete_mods_material_resolve for the masked tag and memoises the
 * answer in a 32-entry table; once the table is full further tags are resolved
 * each time. The table is reset by the flush that owns it.
 * @param tag Scene tag. */
static int ut_tag_resolves(uint32_t tag) {
    uint32_t mt = ut_mask_tag(tag);
    for (int i = 0; i < s_utmemo_n; i++)
        if (s_utmemo_tag[i] == mt) return s_utmemo_hit[i];
    openpete_mod_material_gpu_t mg;
    int hit = openpete_mods_material_resolve(mt, &mg) >= 0;
    if (s_utmemo_n < 32) { s_utmemo_tag[s_utmemo_n] = mt; s_utmemo_hit[s_utmemo_n++] = (int8_t)hit; }
    return hit;
}

/** @brief Emit a bucket's deferred untextured triangles as contiguous tag-grouped material spans.
 *
 * The deferred triangles (scene indices in s_utdef_idx) are sorted by
 * masked tag, stable within a tag, written from vertex @p n, and each tag
 * group is recorded as one s_utmat span for bucket @p gi. Both deferring
 * buckets, the numbered groups and SKY, go through this one function: a
 * deferred triangle is already out of the plain span, so a path that failed
 * to record it would delete it. The span table keeps one slot in reserve:
 * once only that slot is left, the open span stops splitting and absorbs
 * the rest of the bucket at tag 0, so the tail draws with the stock pipe.
 * The reserve covers one bucket only, so the callers also refuse to defer
 * once the table is full (defer_ok). Emission stops when the vertex buffer
 * is full.
 * @param gi Bucket index the spans are recorded under.
 * @param scene Scene triangles.
 * @param buf Scene vertex buffer.
 * @param n First free vertex in @p buf.
 * @param dn Number of deferred triangles.
 * @return The new vertex count. */
static int ut_emit_deferred(int gi, const scene_tri_t* scene, rhi_vertex* buf,
                            int n, int dn)
{
    if (dn <= 0) return n;
    s_utsort_scene = scene;
    qsort(s_utdef_idx, (size_t)dn, sizeof(int), utdef_cmp);
    uint32_t cur = 0; int span_first = 0, open = 0;
    for (int d = 0; d < dn; d++) {
        int t = s_utdef_idx[d];
        if (n + 3 > s_cz_verts) break;
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
        pack_scene_tri(&buf[n], &scene[t]);
        n += 3;
    }
    if (open && s_utmat_n < UT_MAT_SPANS)
        s_utmat[s_utmat_n++] = (__typeof__(s_utmat[0]))
            { gi, cur, span_first, n - span_first };
    return n;
}

/** @brief Return whether an untextured bucket draws its deferred material spans.
 *
 * Deferring a triangle removes it from the plain span, so one predicate
 * decides both the deferral and the draw, and a bucket that defers always
 * draws the spans. Group 1, the untextured SUBTRACT group, is Spyro's
 * depth-aware silhouette decal: its fragment stage owns per-pixel
 * occlusion, which a plain material would delete, and its vertex interface
 * is its own, so it takes no spans. Blend class 4 (quarter-additive) has no
 * material slot. SKY takes spans. Every other group takes spans when it is
 * blended or its category is WORLD, ACTOR or PLAYER; blended spans reshade
 * only, since draw_ut_mat_spans inherits the group's blend and depth-write
 * policy. SKY's stock span and its material spans draw under the same
 * condition; any path that draws ut_count[gi] also draws
 * draw_ut_mat_spans(gi) under that condition.
 * @param gi Group index, or UT_BUCKET_SKY.
 * @param gb The group's blend class.
 * @param gc The group's category.
 * @return 1 when the bucket defers and draws material spans, else 0. */
static int ut_mat_spans_ok(int gi, int gb, int gc) {
    if (gi == 1 || gb == 4) return 0;
    if (gi == UT_BUCKET_SKY) return 1;
    return gb != 0 || gc == OPENPETE_SCENE_WORLD ||
           gc == OPENPETE_SCENE_ACTOR || gc == OPENPETE_SCENE_PLAYER;
}

/** @brief Draw one untextured bucket's deferred mod-material spans.
 *
 * Builds each material's pipeline on first use, with the psx_scene stages
 * as stock fallbacks (their vertex interface, vUV at location 0 and vColor
 * at location 1, is the mod-material fragment contract for textured and
 * untextured draws), and a stencil-MARK twin for the hole-fill WORLD and
 * ACTOR groups so overridden coverage still masks the LP fill. A span whose
 * material fails to resolve or build draws with the stock pipe. The
 * pipeline inherits the group's blend class, so a material changes colour
 * math and never how the group composites; the quarter-add class has no
 * material slot, and a group drawn with it draws no spans here.
 * @param gi Untextured bucket index.
 * @param stock Pipe a span falls back to.
 * @param want_mark Nonzero to draw through the stencil-MARK twin.
 * @param blend_class The group's blend class, 0 opaque to 4 quarter-add;
 *        out of range reads as 0. */
static void draw_ut_mat_spans(int gi, rhi_pipeline* stock, int want_mark,
                              int blend_class) {
    /* Reshade-only: a material replaces the group's COLOUR MATH, never how
     * the group composites, so the pipeline it builds inherits this group's
     * blend. Anything else would let a mod silently reorder or re-composite
     * geometry the painter order depends on. */
    if (blend_class < 0 || blend_class > 4) blend_class = 0;
    /* The material ABI caches one pipeline per blend slot and has no slot for
     * the quarter-add class, so a group drawn that way keeps the stock pipe. */
    if (blend_class == 4) return;
    int bound = 0;
    for (int si = 0; si < s_utmat_n; si++) {
        if (s_utmat[si].gi != gi || s_utmat[si].count <= 0) continue;
        if (!bound) { rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb); bound = 1; }
        rhi_pipeline* pipe = NULL;
        openpete_mod_material_gpu_t mg;
        if (s_utmat[si].tag && openpete_mods_material_resolve(s_utmat[si].tag, &mg) >= 0) {
            void** slot = want_mark ? mg.pipeline_mark_slot
                                    : mg.pipeline_slot[blend_class];
            rhi_pipe_site_visit(RHI_PIPE_SITE_MOD_MATERIAL, 1);
            if (*slot == NULL) {
                rhi_pipeline_desc md = {
                    .vert_spv = "psx_scene.vert.spv",
                    .frag_spv = "psx_scene.frag.spv",
                    .blend = k_blend_class_rhi[blend_class],
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
 * restore self/vram) PER SUB-RUN - a full pack under --lod extended produces
 * thousands of sub-runs per present and the rebind storm dominates frame
 * time. Depth testing makes opaque draw order irrelevant, so the cutout
 * groups batch instead: collect sub-runs during the walk, then draw them
 * sorted by texture - one pipeline bind for the whole batch, one texture
 * bind per file, and the SD walk never leaves `self`. Blend/HUD HD draws
 * stay inline: their order is meaningful and they are few. */
typedef struct { rhi_texture *tex; uint32_t count, first; hd_pc pc; } hd_defer_ent;
static hd_defer_ent *s_hd_defer;
static int s_hd_defer_n, s_hd_defer_cap;

/** @brief Order deferred HD sub-runs by texture pointer, then by first vertex.
 *
 * qsort comparator over hd_defer_ent. Equal textures group into one bind. */
static int hd_defer_cmp(const void *a, const void *b)
{
    const hd_defer_ent *x = a, *y = b;
    if (x->tex != y->tex) return x->tex < y->tex ? -1 : 1;
    return x->first < y->first ? -1 : (x->first > y->first);   /* deterministic */
}

/** @brief Queue one HD sub-run for hd_defer_flush.
 *
 * The queue starts at 1024 entries and doubles; when the growth fails the
 * sub-run is dropped for this present.
 * @param tex Replacement texture.
 * @param pc Push constants of the sub-run, copied.
 * @param count Vertex count.
 * @param first First vertex. */
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

/** @brief Draw the queued HD sub-runs sorted by texture, then empty the queue.
 *
 * Binds hd_pipe once, binds each texture once, pushes each sub-run's
 * constants and draws it. Afterwards the SD pipeline @p self and the VRAM
 * texture are bound again. No effect on an empty queue.
 * @param hd_pipe Pipeline for the replacement textures.
 * @param self Pipeline to restore. */
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
/** @brief Read the texture-pack instrument environment variables on the first call.
 *
 * Sets the resolve-log, dump and category-census gates from the presence of
 * OPENPETE_TEXPACK_RESOLVE_LOG, OPENPETE_TEXPACK_DUMP_DIR and
 * OPENPETE_TEXPACK_CATCENSUS. Later calls do nothing. */
static void tp_flags_init(void)
{
    if (s_tp_resolve_log >= 0) return;
    s_tp_resolve_log = getenv("OPENPETE_TEXPACK_RESOLVE_LOG") ? 1 : 0;
    s_tp_dump_on     = getenv("OPENPETE_TEXPACK_DUMP_DIR") ? 1 : 0;
    s_tp_census_on   = getenv("OPENPETE_TEXPACK_CATCENSUS") ? 1 : 0;
}

/** @brief Resolve a triangle's texture-pack identity and match a replacement file.
 *
 * The triangle's texel box and its base CLUT row resolve to a registry
 * identity, with the category census, the dump (OPENPETE_TEXPACK_DUMP_DIR)
 * and the resolve log (OPENPETE_TEXPACK_RESOLVE_LOG, 256 lines) as side
 * effects, and the identity is then matched against the pack's files. The
 * base row inverts the forward fog shift, (base + delta) & 0x1FF, with the
 * same mask, so a wrapped row does not go negative and index VRAM out of
 * bounds.
 * @param st Triangle.
 * @param m The run's material.
 * @param want_replace Nonzero when a replacement may be matched.
 * @param id Receives the identity on a resolve hit.
 * @param repl Receives the replacement on a match.
 * @param cly_out Receives the base CLUT row.
 * @param u0_out Receives the box's page-relative u origin.
 * @param v0_out Receives the box's page-relative v origin.
 * @return 1 when @p want_replace is set and a replacement file matched,
 *         else 0. */
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

/* ---- per-texture pack retarget (stock -> pack affine fit) ------------------ */
/** @brief Fit a replaced texture's pack art as an affine of its stock art, for the per-texture fog retarget.
 *
 * The ladder affine maps stock base texels to stock fogged texels, so
 * applied to pack art it fogs toward the stock ambience. The global fog
 * shift, one affine for the whole pack, corrects that only on average. The
 * per-texture correction models the pack as an affine of stock,
 * p ~= g(x) = P*x + Q (texture_pack_grade_fit); the pack-side rung
 * transform is then g(a*g^-1(p) + b) = a*p + (P*b + (1-a)*Q), the same
 * slope with the offset re-derived from the texture's own art.
 * OPENPETE_TEXPACK_FOG_FIT=0 disables the fit, read once.
 * @param m Material.
 * @param cly Base CLUT row.
 * @param id Pack identity.
 * @param repl Replacement file.
 * @param pu0 File box u origin in page texel coordinates.
 * @param pv0 File box v origin in page texel coordinates.
 * @param p Receives the per-channel slope P.
 * @param q Receives the per-channel offset Q.
 * @return Nonzero when @p p and @p q hold a fit; 0 when the fit has no
 *         signal (too few opaque pairs) or is disabled, and the caller
 *         falls back to the global fog shift. */
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

/** @brief Fill a replaced draw's fog transforms in its push constants.
 *
 * Flat model (pc->a, pc->b): the affine of the face's own ladder rung.
 * Continuous model (pc->aN, pc->bN, pc->lad; HD pipes only): the
 * deepest-row affine plus the per-fragment ramp, filled when the feeder
 * marked the face ladder-eligible (TP_FOG_DELTA_CONT), the ramp is
 * published, and OPENPETE_TEXPACK_FOG_CONT is not 0; a face carrying
 * TP_FOG_DELTA_FOGBIAS shifts the ramp by 8192/3840. Both offsets are
 * retargeted per texture through tp_pack_fit, with the global fog shift as
 * the fallback. Rung 0 leaves the identity transform.
 * @param m Material.
 * @param cly Base CLUT row.
 * @param fog_delta The face's fog delta word (rung and TP_FOG_DELTA flags).
 * @param id Pack identity.
 * @param repl Replacement file.
 * @param pu0 File box u origin in page texel coordinates, as the shader rect uses.
 * @param pv0 File box v origin in page texel coordinates.
 * @param pc Receives the transforms. */
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
        /* The face's fog reference may carry the authored +0x2000 bias -
         * shift the ramp by the equivalent 8192/3840 in t. */
        pc->lad[1] = tb - ((fog_delta & TP_FOG_DELTA_FOGBIAS) ? 8192.0f / 3840.0f : 0.0f);
    }
}

/* ---- HD texture-replacement fold: pack art in the RETAINED terrain path.
 * The sub-run model above resolves per tri at draw time and rebinds per
 * file; the retained sector-mesh cache instead interns one ENTRY per
 * (file, material) at bake time - file pixels packed into one shared
 * atlas, the fog transform tp_pack_fog computes per sub-run baked per
 * entry - and the terrain fragment stage samples the atlas by the entry
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
static SDL_AtomicInt s_hdf_epoch = { 1 };

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

static SDL_Mutex *s_hdf_mx;
/** @brief Return the mutex guarding the fold's upload queue and shelf packer, creating it on first use.
 *
 * The guest thread (intern, reset) and the presenting thread (flush) both
 * reach it first; a compare-and-swap publishes exactly one mutex and the
 * loser destroys its own.
 * @return The mutex. */
static SDL_Mutex *hdf_mx(void)
{
    SDL_Mutex *m = (SDL_Mutex *)SDL_GetAtomicPointer((void **)&s_hdf_mx);
    if (m) return m;
    SDL_Mutex *n = SDL_CreateMutex();
    if (!SDL_CompareAndSwapAtomicPointer((void **)&s_hdf_mx, NULL, n)) SDL_DestroyMutex(n);
    return (SDL_Mutex *)SDL_GetAtomicPointer((void **)&s_hdf_mx);
}

void openpete_native_hdfold_reset(void)
{
    /* The presenting thread's flush reads the pending count, uploads, then
     * subtracts what it uploaded; clearing the queue between those steps
     * without the lock drives the count negative. */
    SDL_LockMutex(hdf_mx());
    s_hdf_n = 0;
    s_hdf_pending_n = 0;
    s_hdf_shelf_x = s_hdf_shelf_y = s_hdf_shelf_h = 0;
    SDL_UnlockMutex(s_hdf_mx);
}

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

    /* The file's texel box anchored in page coords (the sub-run rect rule -
     * invariant across tris resolving the same identity+file). */
    float pu0 = u0 - (float)(id.box_x - repl.box_x);
    float pv0 = v0 - (float)(id.box_y - repl.box_y);

    /* Continuous-model fog affine at the deepest authored row, retargeted
     * per texture (fit) or by the global shift - tp_pack_fog's exact rule. */
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
     * the stock fetch); everything else is LINEAR - the atlas sampler is
     * NEAREST, so linear entries bilinear-filter manually in the shader. */
    {
        int exact = repl.box_w > 0 && repl.box_h > 0 &&
                    repl.w % repl.box_w == 0 && repl.h % repl.box_h == 0 &&
                    repl.w / repl.box_w > 0 && repl.h / repl.box_h > 0;
        p[11] = exact ? 0.0f : 1.0f;
    }
    p[15] = 0.0f;
    memcpy(s_hdf_tab[s_hdf_n], e->payload, sizeof(e->payload));
    SDL_AddAtomicInt(&s_hdf_epoch, 1);
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
    SDL_LockMutex(hdf_mx());
    const int n = s_hdf_pending_n;
    SDL_UnlockMutex(s_hdf_mx);
    return n;
}

uint32_t openpete_native_hdfold_epoch(void)
{
    return (uint32_t)SDL_GetAtomicInt(&s_hdf_epoch);
}

/* Safe before any replacement exists: the content is unread until an entry folds in. */
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
    if (!s_nr.enabled) return;
    SDL_LockMutex(hdf_mx());
    hdfold_flush_locked();
    SDL_UnlockMutex(s_hdf_mx);
}
/** @brief Upload up to HDFOLD_UPLOAD_PER_FRAME pending rects into the fold atlas; the caller holds s_hdf_mx.
 *
 * Creates the atlas on first use. Returns at once with nothing pending, so
 * the epoch moves only when a flush drains a non-empty queue, which makes
 * fold-claimed sectors retry. */
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
        SDL_AddAtomicInt(&s_hdf_epoch, 1);   /* drained: fold-claimed sectors retry */
}

/* One draw run of a textured bucket: `ntri` consecutive entries of s_txscene
 * from `tri0`, whose vertices start at `vert0` in the scene vertex buffer, all
 * sharing the material `m`. */
typedef struct {
    const scene_tri_t* scene;
    int          tri0, ntri;
    uint32_t     vert0;
    psx_material m;
} tex_run;

/** @brief Return the scene triangle at position t of a textured draw run.
 * @param r The run.
 * @param t Position within the run, 0 to ntri-1. */
static const scene_tri_t* tex_run_tri(const tex_run* r, int t) {
    return &r->scene[s_txscene[r->tri0 + t]];
}

/** @brief Return whether triangle s1 continues the textured draw run that s0 heads.
 *
 * It does when it has the same material (texture page, CLUT and bit
 * depth); the same provenance tag while the tag split is armed, in
 * lockstep with tex_mat_cmp's tag term, since coalescing across tags would
 * undo the split the sort made; and, with @p split_blend, the same blend
 * class and texel STP selection, so a run never straddles two blend
 * equations or two texel selections.
 * @param s0 Run head.
 * @param s1 Candidate triangle.
 * @param split_blend Nonzero when the bucket draws each blend class and
 *        STP selection through its own pipe.
 * @return 1 when @p s1 joins the run, else 0. */
static int tex_run_continues(const scene_tri_t* s0, const scene_tri_t* s1, int split_blend) {
    if (s1->tpage_x != s0->tpage_x || s1->tpage_y != s0->tpage_y ||
        s1->clut_x != s0->clut_x || s1->clut_y != s0->clut_y || s1->bpp != s0->bpp) return 0;
    if (tag_split_on() && s1->tag != s0->tag) return 0;
    return !split_blend || (s1->blend == s0->blend && s1->stp_sel == s0->stp_sel);
}

/** @brief Return the pipeline a textured run headed by s draws through.
 *
 * The table is indexed stp_sel * NR_BLEND_CLASSES + blend, the textured HUD
 * family's per-selection, per-class layout. Entry 0 serves an opaque
 * selection-0 head, a one-entry table (a bucket whose triangles share one
 * class), a class or index outside the table, and a member the table did
 * not build.
 * @param pipes Pipeline table.
 * @param npipes Table length.
 * @param s Run head.
 * @return The pipeline. */
static rhi_pipeline* tex_run_pipe(rhi_pipeline* const* pipes, int npipes, const scene_tri_t* s) {
    const int k = s->stp_sel * NR_BLEND_CLASSES + s->blend;
    return (s->blend > 0 || s->stp_sel > 0) && s->blend >= 0 && s->blend < NR_BLEND_CLASSES &&
           k >= 0 && k < npipes && pipes[k] ? pipes[k] : pipes[0];
}

/** @brief Return the length of the texture-pack sub-run that starts at triangle t of a run.
 *
 * The sub-run is the consecutive triangles with the head's disposition:
 * all unreplaced, or all the same pack file at the same fog delta. A
 * replaced sub-run carries one fog transform in its push constants, and the
 * canonical CLUT maps triangles at different fog distances to the same pack
 * file, so those do not coalesce. Every triangle examined runs
 * tp_tri_match with its census, dump and resolve-log side effects, the
 * triangle that ends the sub-run included.
 * @param r The run.
 * @param t Index in the run of the sub-run's head.
 * @param want_replace Nonzero when replacements may be matched.
 * @param matched The head's tp_tri_match result.
 * @param repl The head's replacement.
 * @param st The head triangle.
 * @return The sub-run length, at least 1. */
static int tp_subrun_len(const tex_run* r, int t, int want_replace, int matched,
                         const texture_pack_repl* repl, const scene_tri_t* st) {
    int g = 1;
    while (t + g < r->ntri) {
        const scene_tri_t* sn = tex_run_tri(r, t + g);
        texture_pack_id nid;
        texture_pack_repl nrepl;
        int ncly; float nu0, nv0;
        int nmatched = tp_tri_match(sn, &r->m, want_replace, &nid, &nrepl,
                                    &ncly, &nu0, &nv0);
        if (nmatched != matched) break;
        if (matched && (nrepl.pixels != repl->pixels || sn->fog_delta != st->fog_delta)) break;
        g++;
    }
    return g;
}

/** @brief Log the draw-side material resolve of a terrain run whose texture record is OPENPETE_TEXSEL_DBG2.
 *
 * Logs a run whose head has a terrain tag (path 2) with bit 25 clear and
 * texture record (bits 15 to 9) equal to the variable's value, once
 * openpete_frame passes 2000: the tag, @p allow_override, the tag-split
 * state, openpete_mods_material_resolve's result and @p run, at most 24
 * lines. Inert when the variable is unset.
 * @param s0 Run head.
 * @param allow_override Nonzero when the caller permits a material override
 *        for the run.
 * @param run Run length in triangles. */
static void texsel_draw_forensics(const scene_tri_t* s0, int allow_override, int run) {
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

/** @brief Return a mod material's pipeline for one blend class, building it on first use.
 *
 * The pipeline is cached in the material's slot for the blend class, or in
 * its single MARK slot. The MARK variant writes the stencil hole-fill mark
 * for opaque coverage; draw_tex_runs requests it only for opaque groups, so
 * the MARK slot is not keyed by blend class. A blended class builds with
 * depth writes off. A failed build is logged, cached as PFX_PIPE_FAILED and
 * not retried.
 *
 * With @p portal set the pipeline is the material's portal profile (the
 * fragment stage compiled with OP_PROFILE_PORTAL, the cover planes at set
 * 7), cached in its portal slots: per blend class for the cover rule, one
 * slot and its MARK twin for the inner-rim rule. A material without a
 * portal profile, and a device that binds fewer than 8 descriptor sets,
 * have no such pipeline: the run then draws through the engine's portal
 * twin without the material.
 * @param mg Resolved mod material.
 * @param ov_mark Nonzero for the stencil-MARK variant.
 * @param blend_class Blend class index into k_blend_class_rhi, 0 opaque.
 * @param portal 0 none, 1 the blended cover rule, 2 the inner-rim rule.
 * @return The pipeline, or NULL when it has no portal profile or its build failed. */
static rhi_pipeline* material_run_pipe(const openpete_mod_material_gpu_t* mg,
                                       int ov_mark, int blend_class, int portal) {
    if (portal && (!mg->frag_portal_spv || rhi_device_max_descriptor_sets(s_nr.dev) < 8u))
        return NULL;
    const int pslot = portal == 2 ? (ov_mark ? OPENPETE_MATERIAL_PORTAL_RIM_MARK
                                             : OPENPETE_MATERIAL_PORTAL_RIM)
                                  : blend_class;
    void** ov_slot = portal ? mg->pipeline_portal_slot[pslot]
                   : ov_mark ? mg->pipeline_mark_slot
                             : mg->pipeline_slot[blend_class];
    rhi_pipe_site_visit(RHI_PIPE_SITE_MOD_MATERIAL, 1);
    if (*ov_slot == NULL) {
        rhi_pipeline_desc md = {
            .vert_spv = "psx_scene.vert.spv",
            .frag_spv = "psx_scene.frag.spv",
            .blend = k_blend_class_rhi[blend_class],
            .depth_test = true,
            /* Blended groups z-test against opaque but never occlude each
             * other. No .depth_bias: binding a bias pipeline invalidates the
             * dynamic bias state, so an override would have to re-set a value
             * it doesn't have - and the one override-eligible subtract group
             * (SUBTRACT-WORLD) draws at zero bias anyway. The biased group,
             * moby shadows, takes no overrides. */
            .depth_write_off = blend_class != 0,
            .stencil = ov_mark ? RHI_STENCIL_MARK : RHI_STENCIL_NONE,
            .material_pipeline = true,
            .uniform_set7 = portal != 0,
            .push_const_size = sizeof(psx_material_hd),
        };
        rhi_pipeline* p = rhi_create_pipeline_spv(
            s_nr.dev, &md, mg->vert_spv, mg->vert_words,
            portal ? mg->frag_portal_spv : mg->frag_spv,
            portal ? mg->frag_portal_words : mg->frag_words);
        *ov_slot = p ? (void*)p : PFX_PIPE_FAILED;
        if (!p)
            fprintf(stderr, "[native] material: pipeline build "
                            "FAILED for %s\n", mg->owner);
    }
    return *ov_slot == PFX_PIPE_FAILED ? NULL : (rhi_pipeline*)*ov_slot;
}

/** @brief Draw a material-overridden run with its texture-pack replacements (op_pack_shade).
 *
 * The run is walked per triangle through the same registry resolve and
 * match as the stock path; triangles matching different pack files, or
 * none, need different set 1 bindings and file rects, so they draw as
 * separate sub-runs. A replacement not yet uploaded is queued for the next
 * present and its sub-run draws with the dummy pack texture.
 * @param r The run.
 * @param mh The run's material push block; hd, the file rect and the fog
 *        affine are rewritten per sub-run.
 * @param pack_bound Texture bound at set 1 on entry. */
static void draw_material_pack_walk(const tex_run* r, psx_material_hd* mh,
                                    rhi_texture* pack_bound) {
    int t = 0;
    while (t < r->ntri) {
        const scene_tri_t* st = tex_run_tri(r, t);
        texture_pack_id id; texture_pack_repl repl;
        int cly; float bu0, bv0;
        int matched = tp_tri_match(st, &r->m, 1, &id, &repl, &cly, &bu0, &bv0);
        int g = tp_subrun_len(r, t, 1, matched, &repl, st);
        rhi_texture* want = s_nr.mat_pack_dummy;
        mh->hd = 0;
        if (matched) {
            uint64_t key = (uint64_t)(uintptr_t)repl.pixels;
            rhi_texture* tex = hd_cache_lookup(key);
            if (!tex) {
                hd_pending_add(key, repl.pixels, NULL, repl.w, repl.h,
                               repl.box_w, repl.box_h);   /* next present */
            } else {
                want = tex;
                mh->hd = 1;
                /* File box in page-texel coords: the tri box (bu0
                 * page-relative <-> id.box upload-relative) anchors the
                 * conversion. */
                mh->hd_u0 = bu0 - (float)(id.box_x - repl.box_x);
                mh->hd_v0 = bv0 - (float)(id.box_y - repl.box_y);
                mh->hd_uw = (float)repl.box_w;
                mh->hd_vh = (float)repl.box_h;
                /* Mod-material pipes carry only the flat per-rung affine
                 * (their fragment contract has no perspective-W varying for
                 * the continuous ramp); they still get the per-texture
                 * retarget. */
                hd_pc fpc;
                tp_pack_fog(&r->m, cly, st->fog_delta, &id, &repl,
                            mh->hd_u0, mh->hd_v0, &fpc);
                memcpy(mh->fog_a, fpc.a, sizeof fpc.a);
                memcpy(mh->fog_b, fpc.b, sizeof fpc.b);
            }
        }
        if (want && want != pack_bound) {
            rhi_bind_texture_set(s_nr.dev, want, 1);
            pack_bound = want;
        }
        rhi_push_constants(s_nr.dev, mh, sizeof *mh);
        rhi_draw(s_nr.dev, (uint32_t)(g * 3), r->vert0 + (uint32_t)(t * 3));
        t += g;
    }
}

/** @brief Draw a textured run with its mod material when the run's provenance tag resolves to a registered selector.
 *
 * The whole run draws with the mod pipeline in the same bucket, pass and
 * depth; scheduling is engine-owned, and runs are tag-pure because the
 * split key is armed whenever a selector exists. The stock-stage fallbacks
 * are psx_scene.vert and psx_scene.frag, forked from sdk/shaders/. The
 * material UBO binds the engine block and the instance slot's mod block.
 * Afterwards @p restore and VRAM are rebound for the caller's stock draws.
 * When @p restore is a portal twin (nr_cover_entry) the run draws through
 * the material's portal profile with the cover planes at set 7, so it keeps
 * the twin's discard; a material without one leaves the run to the twin.
 * @param r The run.
 * @param ov_mark Nonzero to build the stencil-MARK pipeline variant.
 * @param blend_class The caller group's blend class, inherited by the pipeline.
 * @param want_replace Nonzero when texture-pack replacements apply; the run
 *        then draws through draw_material_pack_walk, except a bpp-32 run.
 * @param restore Pipe the caller's stock draws continue on.
 * @return 1 when the run was drawn, 0 when tag splitting is off, the run
 *         has no tag, no selector matches, or the pipeline failed to build. */
static int draw_run_material(const tex_run* r, int ov_mark, int blend_class,
                             int want_replace, rhi_pipeline* restore) {
    const scene_tri_t* s0 = tex_run_tri(r, 0);
    if (!tag_split_on() || !s0->tag) return 0;
    openpete_mod_material_gpu_t mg;
    if (openpete_mods_material_resolve(s0->tag, &mg) < 0) return 0;
    const nr_cover_twin* tw = nr_cover_entry(restore);
    const int portal = tw ? (tw->rim ? 2 : 1) : 0;
    rhi_pipeline* pipe = material_run_pipe(&mg, ov_mark, blend_class, portal);
    if (!pipe) return 0;
    bind_scene_pipe(pipe);
    if (portal) bind_cover_block(7, portal == 2);
    rhi_bind_texture_set(s_nr.dev, s_nr.vram_tex, 0);
    rhi_texture* pack_bound = s_nr.mat_pack_dummy;
    if (pack_bound)
        rhi_bind_texture_set(s_nr.dev, pack_bound, 1);
    bind_material_keys();
    /* engine block 0 + the per-instance slot's mod block (block 1+slot;
     * slot 0 = declarative binds the engine block as the mod block too). */
    if (s_mat_ubo)
        rhi_ubo_bind(s_nr.dev, s_mat_ubo, 0,
                     mg.uniform_slot > 0 ? 1 + mg.uniform_slot : 0);
    psx_material_hd mh;
    memset(&mh, 0, sizeof mh);
    mh.tpage_x = r->m.tpage_x; mh.tpage_y = r->m.tpage_y;
    mh.clut_x = r->m.clut_x; mh.clut_y = r->m.clut_y;
    mh.bpp = r->m.bpp;
    /* Not for bpp-32 runs: mod-registered textures have no registry
     * identity. */
    if (want_replace && s0->bpp != 32) {
        draw_material_pack_walk(r, &mh, pack_bound);
    } else {
        rhi_push_constants(s_nr.dev, &mh, sizeof mh);
        rhi_draw(s_nr.dev, (uint32_t)(r->ntri * 3), r->vert0);
    }
    bind_scene_pipe(restore);   /* HD-path restore discipline */
    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
    return 1;
}

/** @brief Draw a framebuffer-window run through its snapshot's HD texture, queueing the texture's upload on first sight.
 *
 * The run's texture page carries the snapshot index; its UVs are pixels of
 * the stored image (texel_space 0), so the replaced box is the image, or
 * texels of the stored rect for a quad fed before the capture resolved
 * (texel_space 1), so the box is the rect. A snapshot not yet uploaded is
 * queued as a copy taken under the store's lock (the guest thread may free
 * the image before the flush) and draws nothing this present; so does an
 * index with no image, since its texels name no VRAM. Without the HD pipe the
 * run draws through the stock pipeline it was batched under.
 * @param r The run.
 * @param idx Snapshot index (the run head's tpage_x).
 * @param texel_space The run head's tpage_y.
 * @param hd_pipe HD pipe, or NULL.
 * @param self Pipeline bound by the caller, restored after an HD draw. */
static void draw_fb_snapshot_run(const tex_run* r, int idx, int texel_space,
                                 rhi_pipeline* hd_pipe, rhi_pipeline* self) {
    if (!hd_pipe) {
        rhi_push_constants(s_nr.dev, &r->m, sizeof r->m);
        rhi_draw(s_nr.dev, (uint32_t)(r->ntri * 3), r->vert0);
        return;
    }
    int sw = 0, sh = 0, rw = 0, rh = 0;
    uint64_t key = 0;
    if (!openpete_fb_snapshot_image(idx, &key, &sw, &sh) ||
        !openpete_fb_snapshot_rect(idx, &rw, &rh)) return;
    rhi_texture* tex = hd_cache_lookup(key);
    if (!tex) {
        if (!hd_pending_has(key)) {
            unsigned char* copy = openpete_fb_snapshot_copy(idx, key, &sw, &sh);
            if (copy) hd_pending_add(key, NULL, copy, sw, sh, 0, 0);
        }
        return;
    }
    const float bw = texel_space ? (float)rw : (float)sw, bh = texel_space ? (float)rh : (float)sh;
    const hd_pc pc = { 0.f, 0.f, bw, bh,
                       { 1.f, 1.f, 1.f, 0.f }, { 0.f, 0.f, 0.f, 0.f } };
    bind_scene_pipe(hd_pipe);
    rhi_bind_vram(s_nr.dev, tex);
    rhi_push_constants(s_nr.dev, &pc, sizeof pc);
    rhi_draw(s_nr.dev, (uint32_t)(r->ntri * 3), r->vert0);
    bind_scene_pipe(self);
    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
}

/** @brief Draw one textured bucket, coalescing adjacent same-material triangles into runs.
 *
 * `pipes` is the bucket's pipe table indexed by blend class (see
 * tex_run_pipe); the caller binds pipes[0] before the call. A bucket whose
 * triangles share one class passes a one-entry table and every run draws
 * through it. A multi-entry table (the 2D HUD layer, where each primitive
 * carries its own ABR) breaks runs on the blend class and switches pipe per
 * run in submission order, so painter order inside the bucket is kept.
 *
 * allow_override: 0 = stock only; 1 = material overrides via the plain
 * pipeline; 2 = via the stencil-MARK twin (the caller's group marks its
 * opaque coverage for hole-fill, so overridden runs must mark too - else
 * the LP fill paints over them).
 *
 * blend_class is the caller group's PSX ABR+1 class (0 opaque, 1 average,
 * 2 additive, 3 subtract, 4 quarter-add; out of range reads as 0). An
 * override inherits it rather than choosing, so a material changes colour
 * math and never how the run composites; quarter-add has no material slot
 * and disables overrides. Blended groups are painter-ordered: the override
 * swaps the pipeline for a contiguous run in place and draws it in the same
 * slot, so submission order survives.
 *
 * Framebuffer-snapshot runs and bpp-32 mod-texture runs draw through
 * hd_pipe with their own image; with texture-pack replacements active,
 * matched triangles draw their replacement through hd_pipe, deferred and
 * sorted by texture for the cutout pipes.
 * @param first_vert First vertex of the bucket in scene_vb.
 * @param vert_count Vertices in the bucket.
 * @param tx_start First vertex of the textured block that s_txscene indexes.
 * @param scene Scene triangles.
 * @param pipes Pipe table indexed by blend class.
 * @param npipes Entries in @p pipes.
 * @param hd_pipe Pipe for HD, snapshot and mod-texture runs, or NULL.
 * @param allow_override Material-override mode, 0, 1 or 2 as above.
 * @param blend_class The caller group's blend class. */
static void draw_tex_runs(int first_vert, int vert_count, int tx_start,
                          const scene_tri_t* scene, rhi_pipeline* const* pipes,
                          int npipes, rhi_pipeline* hd_pipe, int allow_override,
                          int blend_class) {
    if (blend_class < 0 || blend_class > 4) blend_class = 0;
    if (blend_class == 4) allow_override = 0;   /* no material slot for quarter-add */
    const int hd_defer_ok = hd_pipe == s_nr.hd_cutout_pipe ||
                            hd_pipe == s_nr.hd_cutout_mark_pipe;
    const int ov_mark = (allow_override == 2);
    tp_flags_init();
    const int want_replace = hd_pipe && texture_pack_replacements_active();
    const int split_blend = npipes > 1;
    rhi_pipeline* self = pipes[0];              /* bound by the caller */
    int base = (first_vert - tx_start) / 3, ntri = vert_count / 3, i = 0;
    while (i < ntri) {
        const scene_tri_t* s0 = &scene[s_txscene[base + i]];
        int run = 1;
        while (i + run < ntri &&
               tex_run_continues(s0, &scene[s_txscene[base + i + run]], split_blend))
            run++;
        const tex_run r = { scene, base + i, run, (uint32_t)(first_vert + i * 3),
                            { s0->tpage_x, s0->tpage_y, s0->clut_x, s0->clut_y, s0->bpp } };
        rhi_pipeline* const pipe = tex_run_pipe(pipes, npipes, s0);
        if (pipe != self) {
            bind_scene_pipe(pipe);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            self = pipe;
        }
        texsel_draw_forensics(s0, allow_override, run);
        if (allow_override && draw_run_material(&r, ov_mark, blend_class, want_replace, self)) {
            i += run;
            continue;
        }
        if (s0->bpp == OPENPETE_FB_SNAPSHOT_BPP) {
            draw_fb_snapshot_run(&r, s0->tpage_x, s0->tpage_y, hd_pipe, self);
            i += run;
            continue;
        }
        if (s0->bpp == 32) {
            /* mod-texture run (openpete_mods_texture_register): tpage_x
             * carries the registered id; bind the RGBA texture through
             * the HD pipe, UVs raw texel coords -> rect (0,0,w,h). A
             * pending upload (first sighting) or a missing registration
             * draws NOTHING this frame - SD would sample VRAM noise. */
            int mw = 0, mh = 0;
            const unsigned char* px =
                openpete_mods_texture_get((uint16_t)s0->tpage_x, &mw, &mh);
            if (px && hd_pipe) {
                uint64_t key = (uint64_t)(uintptr_t)px;   /* stable per registration */
                rhi_texture* tex = hd_cache_lookup(key);
                if (!tex) {
                    hd_pending_add(key, px, NULL, mw, mh, 0, 0);
                } else {
                    /* Mod-registered textures are not palette-laddered, so
                     * they push the identity fog transform. */
                    const hd_pc pc = { 0.f, 0.f, (float)mw, (float)mh,
                                       { 1.f, 1.f, 1.f, 0.f }, { 0.f, 0.f, 0.f, 0.f } };
                    bind_scene_pipe(hd_pipe);
                    rhi_bind_vram(s_nr.dev, tex);
                    rhi_push_constants(s_nr.dev, &pc, sizeof pc);
                    rhi_draw(s_nr.dev, (uint32_t)(run * 3), r.vert0);
                    bind_scene_pipe(self);
                    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
                }
            }
            i += run;
            continue;
        }
        /* Registry query - validation (RESOLVE_LOG), dump (DUMP_DIR),
         * replacement. Identity is PER TRI (a run-union
         * box varies with per-frame clipping and tile composition); adjacent
         * tris matching the same pack file coalesce into HD sub-runs, the
         * rest draw SD in material-coalesced groups as before. */
        if (!(s_tp_resolve_log || s_tp_dump_on || s_tp_census_on || want_replace)) {
            rhi_push_constants(s_nr.dev, &r.m, sizeof r.m);
            rhi_draw(s_nr.dev, (uint32_t)(run * 3), r.vert0);
            i += run;
            continue;
        }
        int t = 0;
        int m_pushed = 0;                       /* material push valid for SD draws */
        while (t < run) {
            /* This tri's texel box -> identity -> pack match. */
            const scene_tri_t* st = tex_run_tri(&r, t);
            texture_pack_id id;
            texture_pack_repl repl;
            int cly; float u0, v0;
            int matched = tp_tri_match(st, &r.m, want_replace, &id, &repl,
                                       &cly, &u0, &v0);
            int g = tp_subrun_len(&r, t, want_replace, matched, &repl, st);
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
                    hd_pending_add(key, repl.pixels, NULL, repl.w, repl.h,
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
                    tp_pack_fog(&r.m, cly, st->fog_delta, &id, &repl,
                                pc.u0, pc.v0, &pc);
                    if (hd_defer_ok) {
                        hd_defer_push(tex, &pc, (uint32_t)(g * 3),
                                      r.vert0 + (uint32_t)(t * 3));
                    } else {
                        bind_scene_pipe(hd_pipe);
                        rhi_bind_vram(s_nr.dev, tex);
                        rhi_push_constants(s_nr.dev, &pc, sizeof pc);
                        rhi_draw(s_nr.dev, (uint32_t)(g * 3), r.vert0 + (uint32_t)(t * 3));
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
                rhi_push_constants(s_nr.dev, &r.m, sizeof r.m);
                m_pushed = 1;
            }
            rhi_draw(s_nr.dev, (uint32_t)(g * 3), r.vert0 + (uint32_t)(t * 3));
            t += g;
        }
        i += run;
    }
    if (hd_defer_ok) hd_defer_flush(hd_pipe, self);
}

/** @brief Report whether two triangles' screen bounding boxes overlap with nonzero area.
 *
 * @param a First triangle.
 * @param b Second triangle.
 * @return 1 when the boxes overlap, else 0. */
static int tri_bbox_overlap(const scene_tri_t* a, const scene_tri_t* b)
{
    float ax0 = a->v[0].sx, ax1 = ax0, ay0 = a->v[0].sy, ay1 = ay0;
    float bx0 = b->v[0].sx, bx1 = bx0, by0 = b->v[0].sy, by1 = by0;
    for (int j = 1; j < 3; j++) {
        ax0 = fminf(ax0, a->v[j].sx); ax1 = fmaxf(ax1, a->v[j].sx);
        ay0 = fminf(ay0, a->v[j].sy); ay1 = fmaxf(ay1, a->v[j].sy);
        bx0 = fminf(bx0, b->v[j].sx); bx1 = fmaxf(bx1, b->v[j].sx);
        by0 = fminf(by0, b->v[j].sy); by1 = fmaxf(by1, b->v[j].sy);
    }
    return ax0 < bx1 && bx0 < ax1 && ay0 < by1 && by0 < ay1;
}

/** @brief Report whether one 2D triangle precedes another in the ordering-table walk.
 *
 * Both must carry a walk rank (scene_tri_t::seq, set by
 * openpete_native_scene_rank_2d); a triangle the walk did not rank has no
 * known place in the chain, so it precedes nothing and nothing precedes it.
 * @param a The earlier candidate.
 * @param b The later candidate.
 * @return 1 when both are ranked and @p a ranks before @p b, else 0. */
static int tri_walk_before(const scene_tri_t* a, const scene_tri_t* b)
{
    return a->seq && b->seq && a->seq < b->seq;
}

/* A cursor through the textured HUD range (bucket 6), which keeps scene order
 * (tx_bucket_order leaves it alone). idx maps its k-th triangle to its scene
 * index; triangles [0, done) are drawn. The HUD layer draws the range in runs
 * between the panel triangles, each run ending at the first triangle the
 * ordering-table walk does not rank before the next panel triangle. */
typedef struct {
    const scene_tri_t* scene;
    const int* idx;      /* s_txscene from the range's first triangle */
    int n;               /* triangles in the range; 0 when it cannot draw */
    int done;            /* triangles drawn */
    int tx_start;        /* first vertex of the textured block */
    int first_vert;      /* first vertex of the range */
} hud_tex_cursor;

/** @brief Open a cursor at the start of the textured HUD range.
 *
 * The range is empty when it holds no vertices or its VRAM texture or base
 * pipe is absent, as the HUD layer then draws none of it.
 * @param scene Scene triangles.
 * @param tx_start First vertex of the textured block that draw_tex_runs
 *        indexes.
 * @param first_vert First vertex of the textured HUD range.
 * @param vert_count Vertex count of the textured HUD range.
 * @return The cursor. */
static hud_tex_cursor hud_tex_cursor_open(const scene_tri_t* scene, int tx_start,
                                          int first_vert, int vert_count)
{
    const int ok = vert_count > 0 && s_nr.vram_tex && s_nr.scene_tex_hud_pipes[0];
    hud_tex_cursor c = { scene, NULL, ok ? vert_count / 3 : 0, 0, tx_start, first_vert };
    if (c.n) c.idx = &s_txscene[(first_vert - tx_start) / 3];
    return c;
}

/** @brief Return the end of the undrawn run of textured HUD triangles the walk ranks before a panel triangle.
 *
 * @param c The cursor.
 * @param panel The panel triangle, or NULL for the rest of the range.
 * @return The index one past the run's last triangle; c->done when the run
 *         is empty. */
static int hud_tex_run_end(const hud_tex_cursor* c, const scene_tri_t* panel)
{
    if (!panel) return c->n;
    int k = c->done;
    while (k < c->n && tri_walk_before(&c->scene[c->idx[k]], panel)) k++;
    return k;
}

/** @brief Report whether a panel triangle lies under an overlapping opaque textured HUD triangle the walk ranks before it and the cursor has not drawn.
 *
 * @param c The cursor, positioned where the panel triangle draws.
 * @param from First textured HUD triangle counted; 0 counts every one, as a
 *        panels-first schedule would draw them all after the panel.
 * @param panel The panel triangle.
 * @return 1 when one such triangle exists, else 0. */
static int hud_panel_inverted(const hud_tex_cursor* c, int from, const scene_tri_t* panel)
{
    for (int i = from; i < c->n; i++) {
        const scene_tri_t* q = &c->scene[c->idx[i]];
        if (q->blend == 0 && tri_walk_before(q, panel) && tri_bbox_overlap(q, panel)) return 1;
    }
    return 0;
}

/** @brief Report whether the HUD-panel order census is on, reading OPENPETE_HUD_ORDER_CENSUS once.
 *
 * @return 1 when on, else 0. */
static int hud_order_census_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_HUD_ORDER_CENSUS"); on = (e && *e && *e != '0'); }
    return on;
}

/** @brief Census the HUD panel draw order against the textured HUD range, on stderr, under OPENPETE_HUD_ORDER_CENSUS.
 *
 * Replays the schedule draw_hud_panels_and_tex draws on a copy of the
 * cursor and prints the panel triangles, those drawn after at least one
 * textured triangle (engagement), and the inverted ones (hud_panel_inverted)
 * under a panels-first schedule and under the drawn one. A scene without
 * panel triangles prints nothing.
 * @param scene Scene triangles.
 * @param scene_n Triangle count.
 * @param panels Panel triangles in the slot-8 vertex range.
 * @param c The cursor at the start of the range. */
static void hud_order_census(const scene_tri_t* scene, int scene_n, int panels,
                             hud_tex_cursor c)
{
    if (!hud_order_census_on() || panels <= 0) return;
    int n = 0, after = 0, inv_first = 0, inv_drawn = 0;
    for (int t = 0; t < scene_n && n < panels; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD_PANEL) continue;
        c.done = hud_tex_run_end(&c, &scene[t]);
        n++; after += c.done > 0;
        inv_first += hud_panel_inverted(&c, 0, &scene[t]);
        inv_drawn += hud_panel_inverted(&c, c.done, &scene[t]);
    }
    fprintf(stderr, "[hudorder] stamp=%u panels=%d after_tex=%d inverted_panel_first=%d "
                    "inverted_drawn=%d\n", scene_present_stamp(), n, after, inv_first, inv_drawn);
}

/** @brief Draw the cursor's textured HUD triangles up to an index through the per-class textured HUD pipes, and advance it.
 *
 * @param c The cursor.
 * @param end Index one past the last triangle to draw; at or below
 *        c->done draws nothing. */
static void hud_tex_draw_to(hud_tex_cursor* c, int end)
{
    if (end <= c->done) return;
    bind_scene_pipe(s_nr.scene_tex_hud_pipes[0]);
    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
    draw_tex_runs(c->first_vert + c->done * 3, (end - c->done) * 3, c->tx_start, c->scene,
                  s_nr.scene_tex_hud_pipes, NR_HUD_TEX_PIPES, s_nr.hd_tex_pipe, 0, 0);
    c->done = end;
}

/** @brief Draw the HUD panel boxes and the textured HUD range, interleaved by ordering-table walk rank.
 *
 * Before each panel triangle (bucket 8), the run of not yet drawn textured
 * HUD triangles the walk ranks before it (tri_walk_before) draws, so a box
 * the game links after a textured quad paints over it, and a backing linked
 * first, or a panel the walk did not rank, stays under the textured layer.
 * Each panel triangle carries the blend class of its PSX ABR mode (read from
 * the box's DR_MODE in ui_sprite_native) and draws through that member of
 * the untextured HUD family, alpha for a class outside it. The slot-8 vertex
 * range is in scene order, so walking the scene for panel triangles tracks
 * the vertex offset in lockstep. The textured triangles left after the last
 * panel draw last.
 * @param scene Scene triangles.
 * @param scene_n Triangle count.
 * @param ut_first First vertex of each untextured bucket.
 * @param ut_count Vertex count of each untextured bucket.
 * @param c The cursor at the start of the textured HUD range. */
static void draw_hud_panels_and_tex(const scene_tri_t* scene, int scene_n,
                                    const int* ut_first, const int* ut_count, hud_tex_cursor c)
{
    hud_order_census(scene, scene_n, ut_count[8] / 3, c);
    int voff = ut_first[8];
    const int vend = ut_first[8] + ut_count[8];
    for (int t = 0; t < scene_n && voff + 3 <= vend; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD_PANEL) continue;
        hud_tex_draw_to(&c, hud_tex_run_end(&c, &scene[t]));
        const int b = scene[t].blend;
        rhi_pipeline *pipe = s_nr.gouraud_hud_pipes[(b > 0 && b < NR_BLEND_CLASSES) ? b : NR_BLEND_AVG];
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        if (pipe) { bind_scene_pipe(pipe); rhi_draw(s_nr.dev, 3, (uint32_t)voff); }
        voff += 3;
    }
    hud_tex_draw_to(&c, hud_tex_run_end(&c, NULL));
}

/** @brief Draw the flat 2D HUD layer: panels, textured backdrops and glyphs.
 *
 * The layer draws depth-off. The panel boxes and the textured HUD range draw
 * first, interleaved by ordering-table walk rank (draw_hud_panels_and_tex).
 * Bucket 6 (untextured HUD text and glyphs, depth-sorted at build time)
 * draws last through the base untextured HUD pipe.
 * OPENPETE_HUD6_ZPIPE draws bucket 6 through the opaque gouraud pipe after a
 * depth clear and returns without the default bucket 6 draw. Steps whose
 * pipes or vertices are absent are skipped.
 * @param scene Scene triangles.
 * @param scene_n Triangle count.
 * @param ut_first First vertex of each untextured bucket.
 * @param ut_count Vertex count of each untextured bucket.
 * @param tx_start First vertex of the textured block that draw_tex_runs
 *        indexes.
 * @param hud_tex_first First vertex of the textured HUD range.
 * @param hud_tex_count Vertex count of the textured HUD range. */
static void draw_hud_flat_layer(const scene_tri_t* scene, int scene_n,
                                const int* ut_first, const int* ut_count, int tx_start,
                                int hud_tex_first, int hud_tex_count)
{
    draw_hud_panels_and_tex(scene, scene_n, ut_first, ut_count,
                            hud_tex_cursor_open(scene, tx_start, hud_tex_first, hud_tex_count));
    {   /* OPENPETE_HUD6_ZPIPE=1 (A/B): draw the glyph bucket through the opaque
         * depth-tested pipe after a depth clear - isolates the HUD pipe state. */
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
    if (s_nr.gouraud_hud_pipes[0] && ut_count[6] > 0) {                 /* untextured HUD text/glyphs */
        /* The slot-6 VB range is depth-sorted back-to-front at build time (see
         * the bucket-6 build in render_scene) so rotating near-OT mobys - the
         * inventory letters/icons - self-occlude correctly despite the
         * depth-off pipe. (The textured HUD range above stays submission-order:
         * its content is constant-z 2D sprites; extend the sort there if a
         * rotating TEXTURED near-OT moby ever appears.) */
        bind_scene_pipe(s_nr.gouraud_hud_pipes[0]);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count[6], (uint32_t)ut_first[6]);
    }
}

/** @brief Census a completed scene's blend classes and the depth order of its alpha triangles, on stderr.
 *
 * Enabled by OPENPETE_ALPHA_CENSUS (read once); no effect otherwise. Called
 * on each completed scene, headless included. A BLEND line gives the
 * per-class triangle histogram (opaque, alpha, additive, subtractive) with
 * a running maximum per class; it prints for the first four non-empty
 * scenes, whenever a class sets a new maximum, and whenever the scene holds
 * an alpha triangle. For the alpha class (blend 1, ABR mode 0, (B+F)/2) an
 * ALPHA-CENSUS line gives the count (at most 4096 are examined), how many
 * are textured, the centroid depth span, the number of screen-bbox
 * overlapping pairs, and the inversions among them: pairs where the
 * earlier-emitted triangle is nearer, which a back-to-front sort would
 * reorder. Additive and subtractive equations commute without clamping,
 * so the alpha class is the one a depth sort can change. Reads the scene
 * only.
 * @param scene Completed scene triangles.
 * @param scene_n Triangle count. */
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

/** @brief Return the hardware depth-bias factors for the shadow subtract pipes.
 *
 * The subtract pipes are the fallback when the depth-aware decal pipes are
 * unavailable. vkCmdSetDepthBias semantics: bias = constant*r + slope*m,
 * floored at clamp, all given in the forward sign (negative is toward the
 * camera; rhi_set_depth_bias flips them under reversed depth). m is the
 * largest per-pixel depth slope; slope -2 grows the pull at grazing angles
 * and clamp -0.002 bounds it, with the same view-Z meaning in either
 * encoding. r is the stored value's resolvable step, so the constant is per
 * encoding: forward (the D24 fallback) r = 2^-24 near z = 1 and -64 pulls
 * 64*2^-24*vz^2 in view-Z (15 at vz 2000); reversed r is 2^-24 to 2^-23 of
 * z = 1/vz, so the constant pulls a fraction of the distance, and -131072
 * (0.8% to 1.6% of vz) keeps drawn the moby shadow fans that ride a few
 * units under the ground. OPENPETE_SHADOW_HW_BIAS="constant,slope,clamp"
 * overrides all three; read once.
 * @param c Receives the constant factor.
 * @param s Receives the slope factor.
 * @param cl Receives the clamp. */
static void shadow_hw_bias(float* c, float* s, float* cl)
{
    static const float k_constant[2] = { -64.0f, -131072.0f };  /* forward, reversed */
    static int init = 0;
    static float vc, vs = -2.0f, vcl = -0.002f;
    if (!init) {
        vc = k_constant[(int)rhi_depth_reversed(s_nr.dev)];
        const char* e = getenv("OPENPETE_SHADOW_HW_BIAS");
        if (e && *e) sscanf(e, "%f,%f,%f", &vc, &vs, &vcl);
        init = 1;
    }
    *c = vc; *s = vs; *cl = vcl;
}

/** @brief Return the portal mask's depth bias: constant, slope and clamp.
 *
 * The bias pulls the portal mask toward the camera. The mask is confined to
 * the portal footprint, so a pull cannot reach past the arch; it only
 * decides whether near lips inside the aperture (the arch threshold step at
 * very close range) beat the sky preview, which reproduces the PSX painter
 * result where the sky bucket covered the step. The default is 0, 0, 0: real
 * near geometry occludes the preview. OPENPETE_PORTAL_MASK_BIAS, three
 * comma-separated floats in the forward sign (negative toward the camera,
 * the constant in units of the stored depth's step as in shadow_hw_bias),
 * overrides it. Read once and cached.
 * @param c Receives the constant term.
 * @param s Receives the slope term.
 * @param cl Receives the clamp. */
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

/* (The earlier portal depth-STAMP mechanism - writing plane depth into the
 * aperture so later actor draws depth-fail - was replaced by drawing the
 * portal sky AFTER the ACTOR group with the depth test on: the stamp clipped
 * Spyro mid-entry (PLAYER draws after ACTOR and tested the fake depth too),
 * and its far-sentinel fallback let level mobys reappear once the camera
 * crossed the plane. See the portal draw site in draw_native_scene.) */

/** @brief Return the freeze backdrop texture: the screenshot-time capture when valid, else the live one.
 *
 * shot_frozen is written by openpete_native_render_snapshot_frozen during a
 * screenshot run and is preferred whenever it exists and is valid; frozen_tex
 * is written by the live present (capture_freeze_from_offscreen), so a
 * headless dump would otherwise have no backdrop.
 * @param out_valid Receives 1 when the returned texture holds a capture;
 *        may be NULL.
 * @return shot_frozen or frozen_tex; frozen_tex may be NULL. */
static rhi_texture *active_frozen_tex(int *out_valid)
{
    if (s_nr.shot_frozen && s_nr.shot_frozen_valid) {
        if (out_valid) *out_valid = 1;
        return s_nr.shot_frozen;
    }
    if (out_valid) *out_valid = s_nr.frozen_valid ? 1 : 0;
    return s_nr.frozen_tex;
}

/** @brief Draw the backdrop pair: the far-plane background fill, then the sky dome.
 *
 * The fill is the quad at scene_vb vertex 0, built by the caller; the sky
 * is the cyclorama dome, untextured bucket UT_BUCKET_SKY, with its deferred
 * material spans. Both draw at z 1.0 through the plain opaque gouraud pipe;
 * the dome never stencil-MARKs, so the hole-fill can repaint sky showing
 * through terrain seams. The fill precedes the dome because LESS_OR_EQUAL
 * gives the later draw the z 1.0 ties. Called either before the group loop
 * or right after the WORLD-opaque group, where early-Z rejects the covered
 * pixels.
 * @param bg Background colour; 0 draws no fill.
 * @param ut_first First vertex of each untextured bucket.
 * @param ut_count Vertex count of each untextured bucket.
 * @param sky_ok Nonzero to draw the sky bucket and its material spans. */
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
/** @brief Fill a cover block's planes (every field but fc) from the published openings.
 *
 * At most NR_COVER_PLANES planes are kept and at most 16 outline points per
 * plane; a plane's bounding box is its outline's. n 0 leaves the count 0,
 * which turns both fragment tests off.
 * @param b Receives the planes.
 * @param planes The openings; read only when @p n > 0.
 * @param n Plane count.
 * @param margin_world The plane-cover margin in world units.
 * @param blend_cover Nonzero to turn the blended-terrain discard on. */
static void cover_block_fill(nr_cover_block *b, const openpete_native_cover_plane *planes,
                             int n, float margin_world, int blend_cover)
{
    memset(b, 0, sizeof *b);
    if (n > NR_COVER_PLANES) n = NR_COVER_PLANES;
    b->cnt[0] = n;
    b->cnt[1] = blend_cover ? 1 : 0;
    b->margin[0] = margin_world;
    for (int i = 0; i < n; i++) {
        const openpete_native_cover_plane *pp = &planes[i];
        const int pn = pp->pn > 16 ? 16 : pp->pn;
        b->abc[i][0] = (float)pp->a; b->abc[i][1] = (float)pp->b;
        b->abc[i][2] = (float)pp->c; b->abc[i][3] = (float)pn;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (int k = 0; k < pn; k++) {
            b->poly[i * 16 + k][0] = pp->px[k]; b->poly[i * 16 + k][1] = pp->py[k];
            x0 = fminf(x0, pp->px[k]); x1 = fmaxf(x1, pp->px[k]);
            y0 = fminf(y0, pp->py[k]); y1 = fmaxf(y1, pp->py[k]);
        }
        b->bbox[i][0] = x0; b->bbox[i][1] = y0; b->bbox[i][2] = x1; b->bbox[i][3] = y1;
    }
}
static float s_cvp_m = 0.0f;
void openpete_native_set_cover_planes(const openpete_native_cover_plane *planes,
                                      int n, float margin_world, int blend_cover)
{
    if (n > 8) n = 8;
    if (n > 0) memcpy(s_cvp, planes, sizeof(*planes) * (size_t)n);
    s_cvp_n = n;
    s_cvp_m = margin_world;
    cover_block_fill(&s_cover_live, planes, n, margin_world, blend_cover);
}
/** @brief Verify the portal plane-cover rule on one composed scene and log a census per opening.
 *
 * Runs only when OPENPETE_PORTAL_COVER_CHECK equals this scene's VBL and
 * openings were published. For each opening of at least 3 points, every
 * pixel centre inside the polygon (even-odd) is tested. The plane's
 * view-Z, less the margin and at least 1, is the limit. The nearest opaque
 * WORLD, ACTOR, ACTOR_FRONT or WORLD_FILL face beyond the limit must be
 * beaten by a PORTAL_CYCLORAMA face; a pixel is counted as empty, front,
 * preview-covered or a violation (VIOL). A blended face beyond the limit
 * counts as TVIOL. Prints one [pcover] line per opening. The scene is only
 * read, and the check stops when the candidate list cannot be reserved.
 * @param sc Composed scene triangles.
 * @param n Triangle count. */
static void portal_cover_check(const scene_tri_t* sc, int n)
{
    static long chk = -2;
    if (chk == -2) { const char* e = getenv("OPENPETE_PORTAL_COVER_CHECK");
                     chk = (e && *e) ? atol(e) : -1; }
    if (chk < 0 || s_scene_frame != (unsigned)chk || s_cvp_n <= 0) return;
    if (!cz_instrument_run(&s_cz_cand, sizeof(int), "compose.portal_cover_cand",
                           SCENE_TRIS_WAS, n)) return;
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
        int *cand = (int *)s_cz_cand.data; int cn = 0;
        for (int t = 0; t < n; t++) {
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
            /* View-Z throughout; HUGE_VALF = no content at the pixel. */
            const float zlim = openpete_native_scene_viewz(
                                   openpete_worldvz_full((float)pvz));
            const float ztol = zlim * 1e-6f;
            float zw = HUGE_VALF, zc = HUGE_VALF, zb = HUGE_VALF;
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
                /* 1/vz is screen-affine: interpolate it, not vz. */
                const float z = 1.0f / (b0 / T->v[0].z + b1 / T->v[1].z +
                                        b2 / T->v[2].z);
                if (T->category == OPENPETE_SCENE_PORTAL_CYCLORAMA) {
                    if (z < zc) zc = z;
                } else if (T->blend != 0) {
                    if (z < zb) zb = z;
                } else if (z < zw) zw = z;
            }
            /* Transparent terrain draws AFTER the portal sequence and blends
             * over the preview - a behind-plane blended tri at this pixel
             * means the feeder's cover drop failed. */
            if (zb < HUGE_VALF && zb > zlim + ztol) {
                if (!tviol) { tvx = X; tvy = Y; }
                tviol++;
            }
            if (zw >= HUGE_VALF) { empty_px++; continue; }   /* no opaque content */
            if (zw <= zlim + ztol) { front_px++; continue; }  /* legit front */
            if (zc <= zw * (1.0f + 1e-6f)) { prev_px++; continue; }  /* preview covers */
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

/** @brief Return the VBL at which the scene the compose is reading was extracted.
 *
 * The scene is the ring slot being composed, else the delayed history
 * entry, else the live accumulator.
 * @return The VBL number, 0xFFFFFFFF before the first scene. */
static unsigned scene_present_frame(void) {
    if (s_ps_slot) return s_ps_slot->frame;
    const int slot = scene_hist_slot_at(scene_delay_depth());
    if (slot >= 0) return s_scene_hist_frame[slot];
    return s_scene_frame;
}

/** @brief Test whether a triangle edge separates the probe box from the triangle.
 *
 * True when all four box corners lie strictly on the side of edge a->b away
 * from the third vertex c. A degenerate edge (c on the line) never
 * separates.
 * @param a Edge start.
 * @param b Edge end.
 * @param c The triangle's third vertex.
 * @param B Probe box, screen space.
 * @return 1 when the edge separates, 0 otherwise. */
static int box_outside_edge(const openpete_scene_vert *a, const openpete_scene_vert *b,
                            const openpete_scene_vert *c, const scene_box_cfg_t *B) {
    const float ex = b->sx - a->sx, ey = b->sy - a->sy;
    const float sc = ex * (c->sy - a->sy) - ey * (c->sx - a->sx);
    const float cx[4] = { B->x0, B->x1, B->x0, B->x1 };
    const float cy[4] = { B->y0, B->y0, B->y1, B->y1 };
    for (int k = 0; k < 4; k++) {
        const float s = ex * (cy[k] - a->sy) - ey * (cx[k] - a->sx);
        if (s * sc >= 0.0f) return 0;
    }
    return 1;
}

/** @brief Return whether a screen triangle overlaps the box instrument's box.
 *
 * Separating-axis test: the box axes (a bounding-box test), then the
 * triangle's three edge normals. A zero-area triangle falls back to its
 * bounding box.
 * @param T Triangle in PSX screen coordinates.
 * @param B The box.
 * @return 1 when they overlap, else 0. */
static int tri_overlaps_box(const scene_tri_t *T, const scene_box_cfg_t *B) {
    const openpete_scene_vert *v = T->v;
    const float xmin = fminf(v[0].sx, fminf(v[1].sx, v[2].sx));
    const float xmax = fmaxf(v[0].sx, fmaxf(v[1].sx, v[2].sx));
    const float ymin = fminf(v[0].sy, fminf(v[1].sy, v[2].sy));
    const float ymax = fmaxf(v[0].sy, fmaxf(v[1].sy, v[2].sy));
    if (xmax < B->x0 || xmin > B->x1 || ymax < B->y0 || ymin > B->y1) return 0;
    return !box_outside_edge(&v[0], &v[1], &v[2], B) &&
           !box_outside_edge(&v[1], &v[2], &v[0], B) &&
           !box_outside_edge(&v[2], &v[0], &v[1], B);
}

/* Tag path names, indexed by bits 31:28 (OPENPETE_TAG_* in the header). */
static const char *const k_tag_path_name[16] = {
    "untagged", "moby", "terrain", "player", "flame", "particle", "sky", "shadow",
    "tracer", "portal", "p10", "p11", "p12", "p13", "p14", "p15" };

/** @brief Log one triangle the box instrument found overlapping its box on stderr.
 *
 * One [sbox] line: index in the composed list, category, blend, texture state,
 * texture page and CLUT, bit depth, provenance tag with its decoded owner,
 * ordering fields (mordinal, rank, koff, seq, prim) and the three vertices
 * (screen x, y, z, colour). The owner decode reads tag bits 31:28 as the path;
 * a terrain tag (path 2) prints sector, LP tier, env-anim bit and texture
 * record, any other path its name from k_tag_path_name with id and slot.
 * @param f VBL the scene was extracted at.
 * @param idx Triangle index in the composed list.
 * @param T The triangle. */
static void scene_box_log_tri(unsigned f, int idx, const scene_tri_t *T) {
    const uint32_t tag = T->tag, path = tag >> 28;
    char who[96];
    if (path == 2u)   /* terrain: sector, LP tier, env-anim, texture record */
        snprintf(who, sizeof who, "terrain sec=%u lp=%u env=%u texrec=%u",
                 tag & 0x1FFu, (tag >> 26) & 1u, (tag >> 27) & 1u, (tag >> 9) & 0x7Fu);
    else
        snprintf(who, sizeof who, "%s id=%u slot=%u", k_tag_path_name[path],
                 tag & 0xFFFFu, (tag >> 16) & 0xFFFu);
    fprintf(stderr, "[sbox] f=%u #%d cat=%d bl=%d tex=%d tp=%d,%d cl=%d,%d bpp=%d "
            "tag=%08X %s mord=%d rank=%.0f koff=%.1f seq=%u prim=%08X "
            "v=(%.1f,%.1f,z%.5f,%08X)(%.1f,%.1f,z%.5f,%08X)(%.1f,%.1f,z%.5f,%08X)\n",
            f, idx, T->category, T->blend, T->textured, T->tpage_x, T->tpage_y,
            T->clut_x, T->clut_y, T->bpp, tag, who, T->mordinal, (double)T->rank,
            (double)T->koff, T->seq, T->prim,
            (double)T->v[0].sx, (double)T->v[0].sy, (double)T->v[0].z, T->v[0].rgba,
            (double)T->v[1].sx, (double)T->v[1].sy, (double)T->v[1].z, T->v[1].rgba,
            (double)T->v[2].sx, (double)T->v[2].sy, (double)T->v[2].z, T->v[2].rgba);
}

/** @brief Run the box instrument over one composed scene.
 *
 * Active when OPENPETE_SCENE_BOXDBG is armed and the scene was extracted at
 * a VBL inside its window (spec at scene_box_cfg). Logs every triangle whose
 * area overlaps the box, by its position in the composed list, then one
 * engagement line with the tested and covering counts and whether retained
 * terrain was open, and adds the counts to the run totals.
 * @param sc Composed scene triangles.
 * @param n Triangle count. */
static void scene_box_probe(const scene_tri_t *sc, int n) {
    const scene_box_cfg_t *B = scene_box_cfg();
    if (!B->armed) return;
    const unsigned f = scene_present_frame();
    if (f < B->fa || f > B->fb) return;
    unsigned covering = 0;
    for (int t = 0; t < n; t++) {
        if (!tri_overlaps_box(&sc[t], B)) continue;
        covering++;
        scene_box_log_tri(f, t, &sc[t]);
    }
    s_box.presents++;
    s_box.tested += (unsigned)n;
    s_box.covering += covering;
    fprintf(stderr, "[sbox] f=%u present tested=%d covering=%u retained_terrain=%d\n",
            f, n, covering, openpete_tercache_frame_open() ? 1 : 0);
}

/* Per-PRESENT scene instruments. Hosted here rather than in
 * openpete_native_scene_begin_frame because that block is gated on the VBL
 * number changing, and sub-tick presents REUSE the current VBL number - so a
 * probe living there never observes an intermediate present, exactly where the
 * sub-tick re-extract path differs from the canonical one. */
/** @brief Test whether a scene triangle belongs to the blended-world block.
 *
 * The block holds blend classes 1 to 4 (ABR 0 to 3: alpha, additive,
 * subtractive, quarter-additive; an out-of-range class counts as opaque)
 * except the subtractive shadow decals, which own the depth-aware decal
 * path, and the HUD and HUD3D layers, which draw depth-test off in their
 * own passes. Untextured HUD-panel triangles and textured portal-guard
 * triangles are also excluded. The guest links these classes into the
 * ordering table interleaved and their equations do not commute once a
 * channel clamps, so the block is packed in emission order rather than one
 * class after another.
 * @param s Scene triangle.
 * @return 1 when the triangle is in the block, 0 otherwise. */
static int blended_world_tri(const scene_tri_t* s) {
    int bl = s->blend; if (bl < 0 || bl > 4) bl = 0;
    if (bl == 0) return 0;
    const int c = s->category;
    if (c == OPENPETE_SCENE_HUD || c == OPENPETE_SCENE_HUD3D) return 0;
    if (s->textured) { if (c == OPENPETE_SCENE_PORTAL_GUARD) return 0; }
    else if (c == OPENPETE_SCENE_HUD_PANEL) return 0;
    if (bl == 3 && c == OPENPETE_SCENE_SHADOW) return 0;
    return 1;
}

/** @brief Draw one run of the emission-ordered blended-world block: one class and texturing mode, one pipeline bind.
 *
 * The pipeline tables are indexed by blend class minus one (alpha,
 * additive, subtractive, quarter-additive); the last two classes have no
 * HD replacement twin. Every SD and HD pipeline is replaced by its cover
 * twin (nr_cover_twin_of), whose fragment stage discards what a portal
 * preview covers: the guest links the preview picture at a shallow OT key,
 * so any blended 3D prim behind the opening plane (terrain, glow fans,
 * glints, particles, mobys) draws before it and is painted over. The
 * subtractive pipes carry the dynamic depth bias
 * the shadow decals set, so a subtractive run resets it to zero. A
 * textured run binds VRAM and coalesces by material inside draw_tex_runs.
 * Nothing is drawn when the pipeline is missing, or for a textured run
 * without the VRAM texture.
 * @param scene Scene triangles.
 * @param tx_start First vertex of the packed textured range.
 * @param first_vert First vertex of the run.
 * @param vert_count Vertex count of the run.
 * @param bl Blend class, 1 to 4.
 * @param textured Nonzero for a textured run. */
static void bw_draw_run(const scene_tri_t* scene, int tx_start, int first_vert,
                        int vert_count, int bl, int textured) {
    rhi_pipeline* const utp[4] = { s_nr.gouraud_blend_pipe, s_nr.gouraud_add_pipe,
                                   s_nr.gouraud_sub_ztest_pipe, s_nr.gouraud_qadd_pipe };
    rhi_pipeline* const txp[4] = { s_nr.scene_tex_blend_pipe, s_nr.scene_tex_add_pipe,
                                   s_nr.scene_tex_sub_pipe, s_nr.scene_tex_qadd_pipe };
    rhi_pipeline* const hdp[4] = { s_nr.hd_scene_blend_pipe, s_nr.hd_scene_add_pipe,
                                   NULL, NULL };
    const int c = (bl >= 1 && bl <= 4) ? bl - 1 : 0;
    rhi_pipeline* pipe = nr_cover_twin_of(textured ? txp[c] : utp[c]);
    rhi_pipeline* hd = nr_cover_twin_of(hdp[c]);
    if (!pipe || (textured && !s_nr.vram_tex)) return;
    bind_scene_pipe(pipe);
    /* The subtract pipes enable the dynamic depth bias the shadow decals set;
     * subtractive world content draws at neutral bias. */
    if (bl == 3) rhi_set_depth_bias(s_nr.dev, 0.0f, 0.0f, 0.0f);
    if (!textured) {
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)vert_count, (uint32_t)first_vert);
        return;
    }
    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
    draw_tex_runs(first_vert, vert_count, tx_start, scene, &pipe, 1, hd, 1, bl);
}

/** @brief Draw one blended class of the retained terrain cache.
 *
 * The draw carries the portal cover flag: terrain.frag discards a fragment
 * a portal preview covers, reading the cover planes from the cache's
 * globals block and the fragment affine from tercache_clip_params.
 * @param c Class: 0 alpha, 1 additive, 2 subtractive.
 * @param tercache_on Nonzero when the cache is live this present; zero
 *        draws nothing. */
static void bw_draw_tercache(int c, int tercache_on) {
    static const int k_pass[3] = { OPTC_DRAW_ALPHA, OPTC_DRAW_ADD, OPTC_DRAW_SUB };
    if (!tercache_on) return;
    openpete_tercache_clip aff;
    tercache_clip_params(&aff);
    aff.cover = 1;
    openpete_tercache_draw(k_pass[c], &aff, s_nr.vram_tex, 0,
                           s_nr.terrain_key_depth, s_nr.moby_key_depth);
}

/** @brief Whether a scene triangle came from the terrain feed (its provenance tag's top nibble is 2).
 * @param s Scene triangle.
 * @return 1 for terrain, else 0. */
static int bw_is_terrain(const scene_tri_t* s) { return (s->tag >> 28) == 2u; }

/** @brief Place a blended-world triangle against the terrain feed in the order the pass draws.
 *
 * The game render's block draws in scene order, where every triangle added
 * after the feed began carries ter_after. A re-extract's block draws in
 * recorded key rank, where the feed sits at the recorded terrain rank.
 * @param scene Scene triangles.
 * @param t Scene index of the triangle.
 * @param replay Nonzero for a re-extract's block.
 * @return 0 before the feed, 1 a terrain triangle, 2 after the feed. */
static int bw_ter_phase(const scene_tri_t* scene, int t, int replay) {
    if (bw_is_terrain(&scene[t])) return 1;
    if (replay) return bw_key_rank(ps_bw(), bw_rank_key(&scene[t])) < ps_bw()->ter_rank ? 0 : 2;
    return scene[t].ter_after ? 2 : 0;
}

/** @brief Compute the block position before which each terrain-cache class draws.
 *
 * The walk emits the cache's sectors inside the terrain feed, so a cache
 * class draws inside the feed's span: after every block triangle emitted
 * before the feed, and before the first triangle after it. Inside the span
 * a class draws after the feed's triangles of its own or an earlier class
 * and before the first of a later class. The walk interleaves its blended
 * terrain classes by sector; one draw per cache class does not reproduce
 * that interleave where two blended terrain faces of different classes
 * overlap.
 * @param scene Scene triangles.
 * @param base Offset of the block in s_txscene.
 * @param ntri Triangle count of the block.
 * @param pos Receives, per class (alpha, additive, subtractive), the block
 *        index it draws before; @p ntri means after the block. */
static void bw_tercache_slots(const scene_tri_t* scene, int base, int ntri, int pos[3]) {
    pos[0] = pos[1] = pos[2] = ntri;
    const int replay = scene_present_replay();
    for (int i = 0; i < ntri && pos[2] == ntri; i++) {
        const int t = s_txscene[base + i];
        const int ph = bw_ter_phase(scene, t, replay);
        if (ph == 0) continue;
        for (int c = 0; c < 3; c++)
            if (pos[c] == ntri && (ph == 2 || scene[t].blend > c + 1)) pos[c] = i;
    }
}

/** @brief Draw the blended world block in emission order, one draw per run of one class and texturing mode.
 *
 * This replaces one fixed class draw after another. The guest links these
 * classes into the ordering table
 * interleaved -- at the tracer mesh the subtractive pass is chained ahead of
 * the additive pass over the same quads -- and the equations do not commute
 * once a channel clamps, so a fixed class order composites something the GPU
 * never produced. The block is packed in emission order; each maximal run of
 * one class and one texturing mode is one bind and one draw range. A run also
 * ends where a terrain-cache class draws (bw_tercache_slots). Every run takes
 * the portal cover discard (bw_draw_run).
 * @param scene Scene triangles.
 * @param tx_start First vertex of the textured block that s_txscene indexes.
 * @param first_vert First vertex of the blended world block.
 * @param vert_count Vertices in the block.
 * @param tercache_on Nonzero when the terrain cache draws its blended
 *        classes inside this pass. */
static void draw_blended_world(const scene_tri_t* scene, int tx_start,
                               int first_vert, int vert_count, int tercache_on) {
    const int base = (first_vert - tx_start) / 3, ntri = vert_count / 3;
    int pos[3] = { ntri, ntri, ntri };
    if (tercache_on) bw_tercache_slots(scene, base, ntri, pos);
    int c = 0, i = 0;
    while (i < ntri) {
        for (; c < 3 && pos[c] == i; c++) bw_draw_tercache(c, tercache_on);
        const int stop = c < 3 ? pos[c] : ntri;
        const scene_tri_t* s0 = &scene[s_txscene[base + i]];
        const int bl = s0->blend, textured = s0->textured;
        int run = 1;
        while (i + run < stop) {
            const scene_tri_t* sn = &scene[s_txscene[base + i + run]];
            if (sn->blend != bl || sn->textured != textured) break;
            run++;
        }
        bw_draw_run(scene, tx_start, first_vert + i * 3, run * 3, bl, textured);
        i += run;
    }
    for (; c < 3; c++) bw_draw_tercache(c, tercache_on);
}

/** @brief Count the draw ranges one order costs over a scene's blended-world triangles.
 *
 * Emission order costs one range per material run inside each class run;
 * the fixed class order costs one per material run inside each whole
 * class. An untextured run is one range whatever its materials; a textured
 * one splits where draw_tex_runs splits. The difference is the
 * pipeline-switch cost of emission order, counted on one scene.
 * @param sc Scene triangles.
 * @param n Triangle count.
 * @param ordered Nonzero for emission order, zero for the fixed class order.
 * @return The number of draw ranges. */
static int bw_draw_ranges(const scene_tri_t* sc, int n, int ordered) {
    int draws = 0;
    for (int bl = 1; bl <= 4; bl++)
        for (int tex = 0; tex < 2; tex++) {
            int prev = -1;
            for (int t = 0; t < n; t++) {
                if (!blended_world_tri(&sc[t]) || sc[t].blend != bl ||
                    sc[t].textured != tex) continue;
                /* Ordered: a triangle of another class between two of this one
                 * ends the run, so the emission gap is the run break. */
                /* An untextured run is one draw whatever its materials; a
                 * textured one splits where draw_tex_runs splits. */
                const int broke = prev < 0 || (ordered && t != prev + 1) ||
                                  (tex && !tex_run_continues(&sc[prev], &sc[t], 0));
                if (broke) draws++;
                prev = t;
            }
        }
    return draws;
}

/** @brief Compute the emission-rank key of a blended-world triangle.
 *
 * The provenance tag's family (bits 31:28) names the feeder path. Family 5
 * (particles) is shared by feeders emitted at separate points of the
 * guest's render (the particle pool and the glow rings among them), so the
 * particle KIND byte (tag bits 15:8) refines it. Family 8 (tracers) is
 * shared by the tracer pool and the zap ribbons with no discriminator in
 * the tag; those two collapse to one key.
 * @param t Scene triangle.
 * @return The family in bits 11:8, and for family 5 the KIND byte in bits
 *         7:0. */
static unsigned bw_rank_key(const scene_tri_t* t) {
    const unsigned fam = t->tag >> 28;
    return (fam << 8) | (fam == 5u ? ((t->tag >> 8) & 0xFFu) : 0u);
}

/** @brief Print one partition of a scene's blended-world triangles into bursts per key, on stderr.
 *
 * Called by bw_fam_census for presents in the OPENPETE_BWFAMCENSUS range. A
 * burst is a maximal run of consecutive blended-world triangles sharing one
 * key. A key with more than one burst is a producer the guest interleaves
 * with another, which a replay that emits one producer at a time cannot
 * reproduce by producer order alone. One summary line gives the triangle,
 * key and split-key counts, then one line per key its bursts, triangles
 * and first scene index. At most 32 keys are tracked.
 * @param sc Scene triangles.
 * @param n Triangle count.
 * @param refined Nonzero to key by bw_rank_key, zero by the tag family alone.
 * @param label Tag written into each line. */
static void bw_fam_census_pass(const scene_tri_t* sc, int n, int refined,
                               const char* label) {
    enum { BWF_MAX = 32 };
    unsigned key[BWF_MAX]; int bursts[BWF_MAX], tris[BWF_MAX], first[BWF_MAX];
    int nk = 0, have = 0, split = 0, bw = 0;
    unsigned prev = 0;
    for (int i = 0; i < n; i++) {
        if (!blended_world_tri(&sc[i])) continue;
        const unsigned k = refined ? bw_rank_key(&sc[i]) : (sc[i].tag >> 28) << 8;
        int s = -1;
        for (int q = 0; q < nk; q++) if (key[q] == k) { s = q; break; }
        if (s < 0) {
            if (nk >= BWF_MAX) continue;
            s = nk++; key[s] = k; bursts[s] = 0; tris[s] = 0; first[s] = i;
        }
        if (!have || prev != k) bursts[s]++;
        tris[s]++; prev = k; have = 1; bw++;
    }
    for (int q = 0; q < nk; q++) if (bursts[q] > 1) split++;
    fprintf(stderr, "[bwfam f=%u key=%s] bw=%d keys=%d split=%d\n",
            s_scene_frame, label, bw, nk, split);
    for (int q = 0; q < nk; q++)
        fprintf(stderr, "[bwf %s] k=%04X bursts=%d tris=%d first=#%d\n",
                label, key[q], bursts[q], tris[q], first[q]);
}

/** @brief Print the burst census of a completed scene, on stderr, for OPENPETE_BWFAMCENSUS.
 *
 * OPENPETE_BWFAMCENSUS=N or N-M selects the scene stamps censused; the value
 * is read once. A present in range prints a header line, then bw_fam_census_pass
 * keyed by tag family alone ("fam") and by bw_rank_key ("rank"). */
static void bw_fam_census(const scene_tri_t* sc, int n) {
    static int on = -2; static unsigned lo = 0, hi = 0;
    if (on == -2) { const char* e = getenv("OPENPETE_BWFAMCENSUS");
        on = (e && *e) ? 1 : -1;
        unsigned a = 0, b = 0;
        if (e && sscanf(e, "%u-%u", &a, &b) == 2) { lo = a; hi = b; }
        else if (e && *e) { lo = (unsigned)atoi(e); hi = lo; }
    }
    if (on != 1 || s_scene_frame < lo || s_scene_frame > hi) return;
    fprintf(stderr, "[bwfam f=%u leg=%s] n=%d\n", s_scene_frame,
            s_scene_replay ? "replay" : "inline", n);
    bw_fam_census_pass(sc, n, 0, "fam");
    bw_fam_census_pass(sc, n, 1, "rank");
}

#define NR_PROF_BEGIN(lit) do { static int op_id_ = -1; \
        if (op_prof_on()) { if (op_id_ < 0) op_id_ = op_prof_intern(lit); \
                            op_prof_begin(op_id_); } } while (0)
#define NR_PROF_END() do { if (op_prof_on()) op_prof_end(); } while (0)

/** @brief Print a blended-world block as the ordered pass draws it, on stderr, for OPENPETE_BWDRAWORDER.
 *
 * OPENPETE_BWDRAWORDER=N or N-M selects the scene stamps dumped.
 * One line per maximal run of one provenance key, blend class and texturing
 * mode, with its triangle count, then the totals. Scene indices are left
 * out: the game render and the re-extract build different scenes, so only
 * the run sequence is comparable, and that sequence is what the blend
 * equations see. The game render and the re-extract of one tick are
 * expected to produce identical dumps.
 * @param scene Scene triangles.
 * @param idx Scene indices of the block, in draw order.
 * @param n Block length. */
static void bw_draw_order_dump(const scene_tri_t* scene, const int* idx, int n) {
    static int on = -2; static unsigned lo = 0, hi = 0;
    if (on == -2) { const char* e = getenv("OPENPETE_BWDRAWORDER");
        on = (e && *e) ? 1 : -1;
        unsigned a = 0, b = 0;
        if (e && sscanf(e, "%u-%u", &a, &b) == 2) { lo = a; hi = b; }
        else if (e && *e) { lo = (unsigned)atoi(e); hi = lo; }
    }
    if (on != 1 || s_scene_frame < lo || s_scene_frame > hi) return;
    int runs = 0, tris = 0, i = 0;
    while (i < n) {
        const scene_tri_t* s0 = &scene[idx[i]];
        const unsigned k = bw_rank_key(s0);
        const int bl = s0->blend, tex = s0->textured;
        int cnt = 0;
        while (i + cnt < n) {
            const scene_tri_t* sn = &scene[idx[i + cnt]];
            if (bw_rank_key(sn) != k || sn->blend != bl || sn->textured != tex) break;
            cnt++;
        }
        fprintf(stderr, "[bwdo] run=%d key=%04X blend=%d tex=%d tris=%d\n",
                runs, k, bl, tex, cnt);
        runs++; tris += cnt; i += cnt;
    }
    fprintf(stderr, "[bwdraworder f=%u] runs=%d tris=%d\n", s_scene_frame, runs, tris);
}


#define s_bw_sort_tmp ((int *)s_cz_bwsort.data)
/** @brief Reorder a re-extract's blended-world block into the recorded emission order.
 *
 * Stable per key: triangles of one key keep their relative order and are
 * gathered into one run, in the recorded key order. A block whose keys are
 * each contiguous and already in recorded order comes back unchanged; a key
 * emitted in more than one burst is merged into one. Keys the record lacks
 * go last. No effect for fewer than two triangles or an empty record.
 * @param scene Scene triangles.
 * @param idx Scene indices of the block, reordered in place.
 * @param n Block length. */
static void bw_rank_sort(const scene_tri_t* scene, int* idx, int n) {
    const bw_order_t* O = ps_bw();
    if (n <= 1 || O->n <= 0) return;
    int w = 0;
    for (int r = 0; r <= O->n; r++)
        for (int i = 0; i < n; i++)
            if (bw_key_rank(O, bw_rank_key(&scene[idx[i]])) == r) s_bw_sort_tmp[w++] = idx[i];
    if (w != n) return;   /* every triangle must land exactly once */
    memcpy(idx, s_bw_sort_tmp, (size_t)n * sizeof *idx);
}

/** @brief Put one packed textured bucket into its draw order.
 *
 * This is the whole per-bucket ordering policy. The opaque buckets (0, 2,
 * 3, 9 and 12) sort by material: depth resolves visibility whatever the
 * order, so the sort costs no correctness and coalesces pipeline binds.
 * The blended-world block (bucket 11) keeps emission order, because the
 * guest links its classes into the ordering table interleaved and the
 * blend equations do not commute once a channel clamps; a re-extract emits
 * one producer at a time, so for a re-extracted scene (scene_present_replay)
 * bw_rank_sort restores the order the game render emitted the producers
 * in, and the block is then passed to bw_draw_order_dump. Every other
 * bucket keeps submission order (ABR0 alpha is rare and single-layer, and
 * the flat-HUD backdrops need painter's order).
 * @param b Bucket index.
 * @param scene Scene triangles.
 * @param idx The bucket's scene indices, reordered in place.
 * @param n Number of indices. */
static void tx_bucket_order(int b, const scene_tri_t* scene, int* idx, int n) {
    if (b == 0 || b == 2 || b == 3 || b == 9 || b == 12) {
        NR_PROF_BEGIN("cmp.sort");
        tex_mat_sort(scene, idx, n);
        NR_PROF_END();
        return;
    }
    if (b != 11) return;
    if (scene_present_replay()) {
        NR_PROF_BEGIN("cmp.sort");
        bw_rank_sort(scene, idx, n);
        NR_PROF_END();
    }
    bw_draw_order_dump(scene, idx, n);
}

/** @brief Print a scene's blended-world class runs in emission order, on stderr, for OPENPETE_BLENDORDER.
 *
 * OPENPETE_BLENDORDER=N-M dumps each present whose scene stamp is at or
 * after N, through the first one at or after M; a bare N dumps the first
 * present at or after N. One line per run of consecutive blended-world
 * triangles with one blend class and texturing mode: the emission index
 * span, the class, the mode, the triangle count, and the first triangle's
 * category, rank and moby ordinal. A final line gives the totals and the
 * draw ranges emission order and the fixed class order cost on that scene
 * (bw_draw_ranges).
 * @param sc Scene triangles.
 * @param n Triangle count. */
static void blend_order_dump(const scene_tri_t* sc, int n) {
    static int bo = -2; static unsigned bof = 0, bol = 0;
    if (bo == -2) { const char* e = getenv("OPENPETE_BLENDORDER");
        bo = (e && *e) ? 1 : -1;
        unsigned a = 0, b = 0;
        if (e && sscanf(e, "%u-%u", &a, &b) == 2) { bof = a; bol = b; }
        else if (e && *e) { bof = (unsigned)atoi(e); bol = 0; }
    }
    if (bo != 1 || s_scene_frame < bof) return;
    if (s_scene_frame >= bol) bo = -1;   /* past the range: one dump left */
    int runs = 0, tris = 0, i = 0;
    fprintf(stderr, "[blendorder f=%u] n=%d\n", s_scene_frame, n);
    while (i < n) {
        if (!blended_world_tri(&sc[i])) { i++; continue; }
        const int bl = sc[i].blend, tex = sc[i].textured;
        int j = i, cnt = 0;
        while (j < n && blended_world_tri(&sc[j]) &&
               sc[j].blend == bl && sc[j].textured == tex) { j++; cnt++; }
        fprintf(stderr, "[bo] run=%d #%d..#%d blend=%d tex=%d tris=%d cat=%d "
                        "rank=%.1f mord=%d\n",
                runs, i, j - 1, bl, tex, cnt, sc[i].category,
                (double)sc[i].rank, sc[i].mordinal);
        runs++; tris += cnt; i = j;
    }
    fprintf(stderr, "[blendorder f=%u] runs=%d tris=%d draws_ordered=%d "
                    "draws_fixed=%d\n", s_scene_frame, runs, tris,
            bw_draw_ranges(sc, n, 1), bw_draw_ranges(sc, n, 0));
}

/** @brief Run the per-present scene instruments over the composed scene.
 *
 * Every instrument is off unless its environment variable is set.
 * OPENPETE_SCENECAT_TINT=<category>[,<rrggbb>] rewrites every triangle of that
 * category in place to untextured with the colour, swapping the given rrggbb
 * into the vertex colour order. OPENPETE_SCENEDUMP=<vbl> dumps every triangle
 * once, at the first present at or after that VBL. Then the blend-order dump,
 * the blended-world census, the portal cover check and the box instrument run.
 * @param sc Composed scene triangles; the tint instrument writes to them.
 * @param n Triangle count. */
static void scene_probes(scene_tri_t* sc, int n)
{
    /* OPENPETE_SCENECAT_TINT=<category>[,<rgb hex>] - recolour every
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
       * caller pre-swizzle - passing rrggbb straight through paints the
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
    /* OPENPETE_SCENEDUMP=<frame> - dump EVERY completed scene triangle of the
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
            fprintf(stderr, "[sd] #%d cat=%d blend=%d tex=%d grp=%d ko=%.1f "
                            "rk=%.1f pk=%.1f tag=%08X", ti,
                    T->category, T->blend, T->textured, T->group,
                    (double)T->koff, (double)T->rank, (double)T->pkey, T->tag);
            for (int k = 0; k < 3; k++)
                fprintf(stderr, " (%.2f,%.2f,z%.6f,w%.1f,%08X)",
                        (double)T->v[k].sx, (double)T->v[k].sy,
                        (double)T->v[k].z, (double)T->v[k].pvz, T->v[k].rgba);
            fprintf(stderr, "\n");
        }
      } }
    blend_order_dump(sc, n);
    bw_fam_census(sc, n);
    portal_cover_check(sc, n);
    scene_box_probe(sc, n);
}

/* OPENPETE_DEPTH_TRANSPORT_CENSUS=1: per screenshot, the triangles the last
 * scene draw composed (untextured / textured) and the boot-cumulative count
 * of scene vertices that arrived without a true view-Z. */
static int s_dtc_on = -1, s_dtc_tris = 0, s_dtc_tex = 0;
/** @brief Record the triangle and textured-triangle counts of the scene just composed.
 *
 * No effect unless OPENPETE_DEPTH_TRANSPORT_CENSUS is set to a non-zero
 * value; the variable is read once. */
static void depth_transport_census_scene(const scene_tri_t* scene, int n)
{
    if (s_dtc_on < 0) { const char* e = getenv("OPENPETE_DEPTH_TRANSPORT_CENSUS");
                        s_dtc_on = (e && *e && *e != '0'); }
    if (!s_dtc_on) return;
    s_dtc_tris = n; s_dtc_tex = 0;
    for (int t = 0; t < n; t++) s_dtc_tex += scene[t].textured ? 1 : 0;
}
/** @brief Print the depth-transport line for one screenshot, on stderr.
 *
 * The line gives the counts depth_transport_census_scene last recorded and
 * s_pvz_missing, the scene vertices that arrived without a true view-Z. No
 * effect unless the census is on. */
static void depth_transport_census_shot(const char* path)
{
    if (s_dtc_on == 1)
        fprintf(stderr, "[depth-transport] shot=%s scene_tris=%d textured=%d pvz_missing=%u\n",
                path, s_dtc_tris, s_dtc_tex, s_pvz_missing);
}

/** @brief Return whether scene triangle t belongs to textured bucket b.
 *
 * Bucket 11 is the emission-ordered blended-world block and the one bucket
 * that also carries untextured triangles, so its runs keep the order the
 * guest linked the two texturing modes in; an untextured triangle deferred
 * to material spans (s_bw_deferred) is left out. Buckets 4, 5, 8 and 10 are
 * empty; their content is in bucket 11. Bucket 13 holds the opaque WORLD
 * faces carrying the portal inner-rim mark, which the WORLD group leaves
 * out. Bucket 6 is HUD, 7 PORTAL_GUARD, 9
 * the opaque ACTOR_FRONT faces of the rescue actors, and 12 the opaque
 * textured faces of the 3D HUD icon models. The layered groups (0 to 3)
 * take textured triangles outside those categories whose blend class
 * equals @p gblend; an opaque group with @p gcat set takes only that
 * category, and blend class 3 takes only SHADOW. A blend class outside 0
 * to 4 counts as 0.
 * @param b Bucket index.
 * @param scene Scene triangles.
 * @param t Triangle index.
 * @param gblend The layered group's blend class.
 * @param gcat The layered group's category, or negative for any.
 * @return 1 when the triangle belongs, else 0. */
static int tx_bucket_belongs(int b, const scene_tri_t* scene, int t, int gblend, int gcat) {
    const scene_tri_t* st = &scene[t];
    if (b == 11) return blended_world_tri(st) && !(!st->textured && s_bw_deferred[t]);
    if (!st->textured) return 0;
    const int cat = st->category;
    const int bl = (st->blend < 0 || st->blend > 4) ? 0 : st->blend;
    switch (b) {
    case 4: case 5: case 8: case 10: return 0;
    /* PORTAL_RIM (opaque): WORLD faces carrying the portal inner-rim mark,
     * drawn beside the WORLD group through its inner-rim twins. */
    case 13: return cat == OPENPETE_SCENE_WORLD && bl == 0 && st->portal_rim;
    case 6:  return cat == OPENPETE_SCENE_HUD;
    case 7:  return cat == OPENPETE_SCENE_PORTAL_GUARD;
    /* ACTOR_FRONT (opaque): the rescue actors' textured faces, the ACTOR
     * textured group's class split out for the front pass. */
    case 9:  return cat == OPENPETE_SCENE_ACTOR_FRONT && bl == 0;
    /* HUD3D (opaque): the textured faces of the 3D HUD icon models, drawn by
     * the HUD3D layer beside its untextured faces. */
    case 12: return cat == OPENPETE_SCENE_HUD3D && bl == 0;
    default: break;
    }
    return cat != OPENPETE_SCENE_HUD && cat != OPENPETE_SCENE_HUD3D &&
           cat != OPENPETE_SCENE_PORTAL_GUARD &&
           bl == gblend &&
           !(gblend == 0 && gcat >= 0 && cat != gcat) &&
           !(gblend == 0 && st->portal_rim) &&
           !(gblend == 3 && cat != OPENPETE_SCENE_SHADOW);
}

/** @brief Print the textured HUD3D census line of one present under OPENPETE_LOAD_CENSUS.
 *
 * The line carries the textured HUD3D triangles the scene fed, the number
 * the HUD3D layer drew, and the fed ones whose blend is 1 to 4, not opaque.
 * OPENPETE_LOAD_CENSUS is read once; unset or 0 prints nothing.
 * @param scene Scene triangles.
 * @param scene_n Scene triangle count.
 * @param drawn Textured triangles draw_hud3d_layer drew. */
static void hud3d_tex_census(const scene_tri_t* scene, int scene_n, int drawn) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); s_on = (e && *e && *e != '0'); }
    if (!s_on) return;
    int fed = 0, blended = 0;
    for (int t = 0; t < scene_n; t++) {
        if (!scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD3D) continue;
        fed++;
        if (scene[t].blend >= 1 && scene[t].blend <= 4) blended++;
    }
    fprintf(stderr, "[census f=%u HUD3D-TEX] fed=%d drawn=%d blended=%d\n",
            s_scene_frame, fed, drawn, blended);
}

/** @brief Draw the HUD3D layer, the in-game 3D HUD icon mobys, in its own depth pass on top of the scene.
 *
 * The icons (gem, dragon, Spyro head, key, digits, orb) are 3D models, so
 * the depth-off flat HUD layer would paint their faces inside-out. They draw
 * on top of the whole scene with the depth buffer cleared first, so they
 * Z-sort among their own faces while no world, actor or player geometry
 * occludes them. The untextured faces (untextured bucket 9, opaque
 * gouraud_pipe) and the opaque textured faces (textured bucket 12,
 * scene_tex_pipe) share that one depth pass, so a textured band in front of
 * an untextured face hides it by Z.
 * @param scene Scene triangles.
 * @param tx_start First vertex of the textured block that s_txscene indexes.
 * @param ut_first First vertex of the untextured faces.
 * @param ut_count Vertices of the untextured faces.
 * @param tx_first First vertex of the textured faces.
 * @param tx_count Vertices of the textured faces.
 * @return The textured triangles drawn. */
static int draw_hud3d_layer(const scene_tri_t* scene, int tx_start,
                            int ut_first, int ut_count, int tx_first, int tx_count) {
    const int ut_ok = s_nr.gouraud_pipe && ut_count > 0;
    const int tx_ok = s_nr.vram_tex && s_nr.scene_tex_pipe && tx_count > 0;
    if (!ut_ok && !tx_ok) return 0;
    rhi_clear_depth(s_nr.dev);
    if (ut_ok) {
        bind_scene_pipe(s_nr.gouraud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count, (uint32_t)ut_first);
    }
    if (!tx_ok) return 0;
    bind_scene_pipe(s_nr.scene_tex_pipe);
    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
    draw_tex_runs(tx_first, tx_count, tx_start, scene, &s_nr.scene_tex_pipe, 1,
                  s_nr.hd_cutout_pipe, 0, 0);
    return tx_count / 3;
}

/** @brief Whether an opaque layered group leaves out an untextured triangle of its blend class.
 *
 * An opaque group (blend class 0) with a category takes only that
 * category, and no opaque group takes a face carrying the portal inner-rim
 * mark, which packs into UTC_WORLD_RIM or UTC_FILL_RIM instead.
 * @param gb The group's blend class.
 * @param gc The group's category, or negative for any.
 * @param st The triangle.
 * @return 1 when the group skips it, else 0. */
static int ut_opaque_group_skips(int gb, int gc, const scene_tri_t* st) {
    if (gb != 0) return 0;
    return st->portal_rim || (gc >= 0 && st->category != gc);
}

/** @brief Draw the opaque WORLD faces carrying the portal inner-rim mark through the WORLD pipelines' inner-rim twins.
 *
 * The twins' fragment stage (the _rim variants, portal_cover_scene.glsl)
 * discards a fragment inside a published opening outline, where the portal
 * preview paints, and keeps it elsewhere; with hole-fill armed they are the
 * stencil-MARK twins, so a kept fragment marks its coverage like the rest of
 * the WORLD group. A pipeline without a twin draws through its stock
 * pipeline. A mod material applies through its portal profile, which keeps
 * the discard (draw_run_material); a material without one leaves the run
 * stock.
 * @param scene Scene triangles.
 * @param tx_start First vertex of the textured block that s_txscene indexes.
 * @param ut_first First vertex of the untextured rim faces.
 * @param ut_count Their vertex count.
 * @param tx_first First vertex of the textured rim faces (textured bucket 13).
 * @param tx_count Their vertex count.
 * @param holefill_on Nonzero when the WORLD group draws through the MARK pipes.
 * @param gi The layered group being drawn; only the WORLD group (0) draws them. */
static void draw_portal_rim(const scene_tri_t* scene, int tx_start, int ut_first, int ut_count,
                            int tx_first, int tx_count, int holefill_on, int gi) {
    if (gi != 0) return;
    rhi_pipeline* ut = nr_cover_twin_of(holefill_on ? s_nr.gouraud_mark_pipe : s_nr.gouraud_pipe);
    if (ut && ut_count > 0) {
        bind_scene_pipe(ut);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count, (uint32_t)ut_first);
    }
    rhi_pipeline* tx = nr_cover_twin_of(holefill_on ? s_nr.scene_tex_mark_pipe : s_nr.scene_tex_pipe);
    if (!tx || tx_count <= 0 || !s_nr.vram_tex) return;
    rhi_pipeline* hd = nr_cover_twin_of(holefill_on ? s_nr.hd_cutout_mark_pipe : s_nr.hd_cutout_pipe);
    bind_scene_pipe(tx);
    rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
    draw_tex_runs(tx_first, tx_count, tx_start, scene, &tx, 1, hd, holefill_on ? 2 : 1, 0);
}

/** @brief Draw the WORLD_FILL faces carrying the portal inner-rim mark through the hole-fill pipeline's inner-rim twin.
 *
 * Same stencil TEST_ZERO role as the stock fill; the twin discards a
 * fragment inside a published opening outline. Without a twin the stock
 * fill pipeline draws them.
 * @param first First vertex of the faces.
 * @param count Their vertex count.
 * @param holefill_on Nonzero when hole-fill is armed; zero draws nothing, as
 *        for the stock fill.
 * @param gi The layered group being drawn; the fill draws after the ACTOR
 *        group (2). */
static void draw_portal_rim_fill(int first, int count, int holefill_on, int gi) {
    rhi_pipeline* p = nr_cover_twin_of(s_nr.gouraud_fill_pipe);
    if (gi != 2 || !holefill_on || !p || count <= 0) return;
    bind_scene_pipe(p);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
    rhi_draw(s_nr.dev, (uint32_t)count, (uint32_t)first);
}

/* Untextured single-category buckets of draw_native_scene, in vertex-buffer
 * order; k_utc_bucket maps each to its ut_first/ut_count slot. */
enum { UTC_OVERLAY, UTC_HUD_PANEL, UTC_HUD3D, UTC_PORTAL_GUARD, UTC_WORLD_FILL,
       UTC_ACTOR_FRONT, UTC_WORLD_RIM, UTC_FILL_RIM, UTC_N };
static const int k_utc_bucket[UTC_N] = { 7, 8, 9, 10, 12, 14, 16, 17 };
/* Untextured bucket slots of draw_native_scene (ut_first / ut_count). */
#define UT_BUCKETS 18

/** @brief The single-category untextured bucket a scene triangle packs into, or -1.
 *
 * Untextured OVERLAY, HUD_PANEL, HUD3D, PORTAL_GUARD and WORLD_FILL
 * triangles of any blend class, and untextured ACTOR_FRONT triangles of
 * blend class 0 (a class outside 0 to 4 counts as 0). An opaque WORLD or
 * WORLD_FILL triangle carrying the portal inner-rim mark takes the matching
 * inner-rim bucket instead.
 * @param st The triangle.
 * @return A UTC_* index, or -1. */
static int utc_class(const scene_tri_t *st)
{
    if (st->textured) return -1;
    const int opaque = st->blend <= 0 || st->blend > 4;
    if (st->portal_rim && opaque && st->category == OPENPETE_SCENE_WORLD) return UTC_WORLD_RIM;
    if (st->portal_rim && opaque && st->category == OPENPETE_SCENE_WORLD_FILL) return UTC_FILL_RIM;
    switch (st->category) {
    case OPENPETE_SCENE_OVERLAY:      return UTC_OVERLAY;
    case OPENPETE_SCENE_HUD_PANEL:    return UTC_HUD_PANEL;
    case OPENPETE_SCENE_HUD3D:        return UTC_HUD3D;
    case OPENPETE_SCENE_PORTAL_GUARD: return UTC_PORTAL_GUARD;
    case OPENPETE_SCENE_WORLD_FILL:   return UTC_WORLD_FILL;
    case OPENPETE_SCENE_ACTOR_FRONT:
        return opaque ? UTC_ACTOR_FRONT : -1;
    default:                          return -1;
    }
}

/** @brief Count the scene's triangles per single-category untextured bucket in one pass. */
static void utc_count(const scene_tri_t *scene, int scene_n, int cnt[UTC_N])
{
    for (int c = 0; c < UTC_N; c++) cnt[c] = 0;
    for (int t = 0; t < scene_n; t++) {
        const int c = utc_class(&scene[t]);
        if (c >= 0) cnt[c]++;
    }
}

/** @brief Give buckets lo..hi-1 consecutive vertex ranges from n, each truncated to what fits below cap.
 *
 * Sets each bucket's ut_first and ut_count and its scatter cursor and end.
 * @return The vertex count after the ranges. */
static int utc_reserve(int lo, int hi, const int cnt[UTC_N], int n, int cap,
                       int cur[UTC_N], int end[UTC_N], int *ut_first, int *ut_count)
{
    for (int c = lo; c < hi; c++) {
        const int room = cap > n ? (cap - n) / 3 : 0;
        const int take = cnt[c] < room ? cnt[c] : room;
        ut_first[k_utc_bucket[c]] = n;
        ut_count[k_utc_bucket[c]] = take * 3;
        cur[c] = n;
        n += take * 3;
        end[c] = n;
    }
    return n;
}

/** @brief Pack the triangles of buckets lo..hi-1 into their reserved ranges in one pass, in submission order. */
static void utc_scatter(int lo, int hi, const scene_tri_t *scene, int scene_n, rhi_vertex *buf,
                        int cur[UTC_N], const int end[UTC_N])
{
    for (int t = 0; t < scene_n; t++) {
        const int c = utc_class(&scene[t]);
        if (c < lo || c >= hi || cur[c] >= end[c]) continue;
        pack_scene_tri(&buf[cur[c]], &scene[t]);
        cur[c] += 3;
    }
}

/** @brief Print the screen bounding box of the composed scene's framebuffer-snapshot triangles under OPENPETE_FBSNAP_CENSUS.
 *
 * One line per compose that holds any: the VBL, the scene's tick and alpha
 * (0 inline), the triangle count and the bounding box in PSX screen units.
 * It reads the rect a window or a shrinking copy is drawn at on every
 * present, canonical and sub-tick alike.
 * @param scene Scene triangles.
 * @param n Triangle count. */
static void fbsnap_present_census(const scene_tri_t* scene, int n)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_FBSNAP_CENSUS"); on = (e && *e && *e != '0'); }
    if (!on) return;
    int k = 0; float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int t = 0; t < n; t++) {
        if (scene[t].bpp != OPENPETE_FB_SNAPSHOT_BPP) continue;
        k++;
        for (int j = 0; j < 3; j++) {
            x0 = fminf(x0, scene[t].v[j].sx); x1 = fmaxf(x1, scene[t].v[j].sx);
            y0 = fminf(y0, scene[t].v[j].sy); y1 = fmaxf(y1, scene[t].v[j].sy);
        }
    }
    if (!k) return;
    extern unsigned openpete_frame;
    fprintf(stderr, "[fbsnap] present vbl=%u tick=%u alpha=%.3f tris=%d rect=%.2f,%.2f..%.2f,%.2f\n",
            openpete_frame, ps_frame(), s_ps_slot ? s_ps_slot->alpha : 0.0, k, x0, y0, x1, y1);
}

/** @brief Composes the present's scene into the bound render target, one pass per layer and bucket.
 *
 * Packs the scene triangles into the vertex buffer and draws the world
 * groups, the HUD, overlay, panel, 3D-HUD, portal, sky, fill and
 * actor-front layers and the textured buckets in their fixed order.
 * @param full_extent 1 to map the full PSX extent (offscreen panel), 0 for the display mapping.
 * @return true when anything was drawn. */
static bool draw_native_scene(bool full_extent)
{
    int scene_n; uint32_t scene_bg, scene_fade, scene_freeze; int hud_under = 0, actor_front = 0;
    const scene_tri_t* scene = scene_present_buf(&scene_n, &scene_bg, &scene_fade, &scene_freeze, &hud_under, &actor_front);
    scene_n = compose_reserve(scene_n);
    scene_probes((scene_tri_t*)scene, scene_n);   /* per-present: sees intermediates too */
    depth_transport_census_scene(scene, scene_n);
    fbsnap_present_census(scene, scene_n);

    /* OPENPETE_TERRAIN_KEY_IDENT=1: prove the hoisted key pass and this draw
     * packed from the SAME scene snapshot + clip mapping - repack here and
     * byte-compare against the pass's recorded batch. A mismatch means the
     * snapshot or mapping moved between pass and draw: the one failure class
     * a key-image capture cannot attribute (the shared image is sequentially
     * overwritten per target, so post-frame readbacks only ever show the
     * LAST pass's content). */
    { static int s_ident = -1;
      if (s_ident < 0) { const char* e = getenv("OPENPETE_TERRAIN_KEY_IDENT");
          s_ident = (e && *e && *e != '0'); }
      if (s_ident && s_tk_last.m >= 0 && openpete_native_key_resolve_on()
          && s_tk_last.full_extent == (full_extent ? 1 : 0)
          && cz_instrument_run(&s_cz_ident, sizeof(rhi_vertex), "compose.key_ident",
                               SCENE_VB_WAS, s_cz_verts)) {
          rhi_vertex *ibuf = (rhi_vertex *)s_cz_ident.data;
          static keytex_span_t ispans[KEYTEX_MAX_SPANS];
          for (int cls = 0; cls < 3; cls++) {
              int lm = cls == 1 ? s_tk_last.mm : cls == 2 ? s_tk_last.am : s_tk_last.m;
              const rhi_vertex* lb = (const rhi_vertex *)(cls == 1 ? s_tk_last.mbuf
                                   : cls == 2 ? s_tk_last.abuf : s_tk_last.buf)->data;
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

    /* Publish this present's interned per-instance material uniforms
     * into the material UBO before any draw_tex_runs override binds them.
     * No-op (and cheap) when no per-instance selector is active. */
    fill_material_ubo();

    /* Load-window census (OPENPETE_LOAD_CENSUS=1): what THIS present actually
     * composes - tri counts per category/blend + the fade/freeze covers. One
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
     * from PutDrawEnv) - the colour the canonical render actually auto-clears the
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
     * But in ISOLATION (--render-paths) the VRAM is the UNFILTERED frame - falling
     * back to it would show every path and defeat the isolation. So in isolation
     * present just the background (= PsyCross's filtered clear) instead.
     * A retained-terrain frame with content is scene content too: a game
     * whose only feeder is the terrain, with every sector on the GPU cache,
     * has no CPU triangles at all.
     * Only the game's own triangles count: host-added geometry on a frame the
     * game drew nothing on would take the render branch and put the retained
     * framebuffer backdrop where the game left the frame untouched. */
    const bool cache_content = openpete_tercache_frame_open() && openpete_tercache_frame_content();
    if (!scene_has_game_tris(scene, scene_n) && !cache_content && (!s_isolation || bg == 0))
        return false;

    s_render_full_extent = full_extent ? 1 : 0;

    rhi_vertex *buf = (rhi_vertex *)s_cz_vb.data;
    int n = 0;
    /* Background fill (untextured, far plane), drawn first; depth-tested actors
     * sit in front of it. Covers the panel rect (window) or the full target. */
    if (bg != 0) {
        float l, r, t, b;
        cover_rect(full_extent, &l, &r, &t, &b);
        const float fz = OPENPETE_DEPTH_VIEWZ_FAR;   /* the backdrop view-Z */
        const rhi_vertex tl = { l, t, fz, 0, 0, bg, 0, .w = fz };
        const rhi_vertex tr = { r, t, fz, 0, 0, bg, 0, .w = fz };
        const rhi_vertex br = { r, b, fz, 0, 0, bg, 0, .w = fz };
        const rhi_vertex bl = { l, b, fz, 0, 0, bg, 0, .w = fz };
        buf[0] = tl; buf[1] = tr; buf[2] = br;
        buf[3] = tl; buf[4] = br; buf[5] = bl;
        n = 6;
    }
    /* The scene draws in 7 LAYERED groups, in this order, so a ground-decal
     * shadow lands OVER the terrain it falls on but UNDER the actors standing on
     * it (the PSX terrain -> shadow -> Spyro draw order). A single near-bias on
     * the shadow can't do both (it would also paint over Spyro's feet); the layer
     * order does. Each group's UNTEXTURED tris are packed into a contiguous VB
     * range (one batched draw); its TEXTURED tris are drawn per-tri (material
     * push) by filtering the appended textured block. gcat <0 = any (only OPAQUE
     * splits WORLD vs ACTOR vs PLAYER; transparent classes route by blend alone).
     *   0 WORLD-opaque  (blend 0, cat WORLD)   terrain / sky / props
     *   1 SUBTRACT      (blend 3)              shadows (depth_write_off)
     *   2 ACTOR-opaque  (blend 0, cat ACTOR)   mobys - cover the shadows
     *   --- HUD/overlay (depth-test off) drawn here: OVER mobys, UNDER the player ---
     *   3 PLAYER-opaque (blend 0, cat PLAYER)  Spyro - covers the near-OT HUD text
     *   4 ALPHA         (blend 1)              ABR0 semi-transparent
     *   5 ADDITIVE      (blend 2)              ABR1 flame / glows
     *   6 QUARTER-ADD   (blend 4)              ABR3 glass (last)
     * Groups 4 to 6 keep no plain bucket content: their triangles draw from
     * the emission-ordered blended-world block at gi 4, except those
     * deferred into a mod material span.
     * The HUD/overlay sits BETWEEN regular mobys and the player so the PSX's
     * near-OT-slot UI (HUD icons, "DEMO MODE", level-intro / "RESCUED <dragon>"
     * text) draws OVER mobys (e.g. the rescued dragon Nestor) yet UNDER the player
     * Spyro (who flies in front of the intro text) - exactly the PSX OT order
     * (text at the near slot, over other mobys; the player rendered on top). */
    /* utb/txb name the untextured and textured bucket a group packs into.
     * They are the group index for the six original groups; the quarter-add
     * group takes the free slots above the HUD and overlay buckets, so those
     * keep their numbers. */
    struct scene_group { int blend, gcat, utb, txb; rhi_pipeline *utex, *tex; };
    /* The WORLD-opaque group draws through the stencil-MARK variants
     * (identical raster + stencil=1 coverage write) so the FILL layer below can
     * paint only uncovered pixels. Falls back to the plain pipes (and no fill)
     * if the mark/fill pipes didn't come up. */
    const int holefill_on = s_nr.gouraud_mark_pipe && s_nr.scene_tex_mark_pipe
                            && s_nr.gouraud_fill_pipe;
    const struct scene_group groups[7] = {
        { 0, OPENPETE_SCENE_WORLD, 0, 0,
              holefill_on ? s_nr.gouraud_mark_pipe   : s_nr.gouraud_pipe,
              holefill_on ? s_nr.scene_tex_mark_pipe : s_nr.scene_tex_pipe    },
        { 3, -1, 1, 1,              s_nr.gouraud_sub_pipe,   s_nr.scene_tex_sub_pipe   },
        /* ACTOR opaques also MARK coverage (like WORLD): authored HP ground
         * holes under static decor (pedestal discs) must read as covered so
         * the LP hole-fill - drawn after this group - never paints over the
         * moby that hides them. */
        { 0, OPENPETE_SCENE_ACTOR, 2, 2,
              holefill_on ? s_nr.gouraud_mark_pipe   : s_nr.gouraud_pipe,
              holefill_on ? s_nr.scene_tex_mark_pipe : s_nr.scene_tex_pipe    },
        { 0, OPENPETE_SCENE_PLAYER, 3, 3, s_nr.gouraud_pipe,       s_nr.scene_tex_pipe       },
        { 1, -1, 4, 4,              s_nr.gouraud_blend_pipe, s_nr.scene_tex_blend_pipe },
        { 2, -1, 5, 5,              s_nr.gouraud_add_pipe,   s_nr.scene_tex_add_pipe   },
        /* 6 QUARTER-ADD (blend 4) ABR3 glass, after the additive group. */
        { 4, -1, 15, 10,            s_nr.gouraud_qadd_pipe,  s_nr.scene_tex_qadd_pipe  },
    };
    /* ut group 6 = HUD untextured overlay (after ACTOR); 7 = OVERLAY (dead last,
     * letterbox / HUD-OT 2D flat quads, over everything); 8 = HUD_PANEL (alpha,
     * under the HUD text); 9 = HUD3D (in-game 3D icon mobys, DEPTH-TESTED on top
     * after a depth clear). Groups 6/7/8 are depth-test off; 9 is depth-on.
     * 11 = SKY (cyclorama dome - WORLD-like, drawn FIRST with the PLAIN pipe so
     * it never stencil-marks; see OPENPETE_SCENE_SKY); 12 = WORLD_FILL (terrain
     * LP hole-fill, stencil TEST_ZERO, right after the WORLD group). */
    int ut_first[UT_BUCKETS], ut_count[UT_BUCKETS];
    /* CPU attribution (--profile-out): the compose phase splits into
     * build_ut (untextured bucketing incl. portal-run collection),
     * build_tex (textured bucketing + material sort), vb_tex (textured
     * vertex pack), upload (VB update) and record (draw recording). */


    NR_PROF_BEGIN("cmp.build_ut");
    s_utmat_n = 0; s_utmemo_n = 0;
    memset(s_bw_deferred, 0, (size_t)scene_n);
    for (int gi = 0; gi < 7; gi++) {
        const int utb = groups[gi].utb;
        ut_first[utb] = n;
        int gb = groups[gi].blend, gc = groups[gi].gcat;
        /* Defer only what this bucket will actually draw - see
         * ut_mat_spans_ok - and only while the span table can still record
         * it. Deferring more than that DELETES it: the tri leaves the plain
         * span and nothing else draws it. */
        const int defer_ok = tag_split_on() && ut_mat_spans_ok(gi, gb, gc) &&
                             s_utmat_n < UT_MAT_SPANS;
        int dn = 0;
        for (int t = 0; t < scene_n; t++) {
            if (scene[t].textured || n + 3 > s_cz_verts) continue;
            if (scene[t].category == OPENPETE_SCENE_HUD ||
                scene[t].category == OPENPETE_SCENE_HUD3D ||
                scene[t].category == OPENPETE_SCENE_HUD_PANEL) continue;  /* -> HUD passes */
            int b = scene[t].blend; if (b < 0 || b > 4) b = 0;   /* untextured class */
            if (b != gb) continue;
            if (ut_opaque_group_skips(gb, gc, &scene[t])) continue;
            /* SUBTRACT group = the drop-shadow DECALS only (SHADOW category).
             * Plain subtractive world content (blend 3, any other category -
             * e.g. hypothetical untextured smoke) goes to slot 13 below and
             * draws through the plain subtract pipe, not the decal. */
            if (gb == 3 && scene[t].category != OPENPETE_SCENE_SHADOW) continue;
            /* The SUBTRACT group (gb==3 = the depth-aware Spyro shadow decal)
             * carries its PSX screen coord in u,v so shadow_decal.frag gets the
             * exact per-pixel screen position via a NOPERSPECTIVE varying (no
             * mode-specific affine - identical full-extent and panel). */
            int is_shadow = (gb == 3);
            /* OPENPETE_PLY_VBDBG=1: print PLAYER-group verts in the upper band
             * (stamp forensics - sy<205 excludes Spyro's body). */
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
                s_bw_deferred[t] = 1;
                continue;
            }
            /* The blended classes (alpha, additive, quarter-additive here;
             * blend 3 outside the shadow decals is the same case) draw from the
             * emission-ordered blended-world block, not from this bucket. */
            if (gb == 1 || gb == 2 || gb == 4) continue;
            for (int k = 0; k < 3; k++)
                pack_scene_vert(&buf[n + k], &scene[t], k,
                                is_shadow ? scene[t].v[k].sx : 0.0f,
                                is_shadow ? scene[t].v[k].sy : 0.0f);
            n += 3;
        }
        ut_count[utb] = n - ut_first[utb];         /* the PLAIN (stock-drawn) span */
        /* Deferred tris: grouped by resolve-relevant tag into contiguous
         * spans after the plain span (ut_emit_deferred). */
        n = ut_emit_deferred(gi, scene, buf, n, dn);
    }
    /* HUD untextured: every HUD-category untextured tri (any blend). The pipe is
     * depth-test OFF by design (the layer must paint over the world uncondition-
     * ally yet stay under the player), so ordering INSIDE the layer is painter's.
     * Submission order is wrong for the near-OT mobys the game ROTATES - the
     * inventory screen's spinning letter/dragon-icon mobys painted their back
     * faces over their front faces. Sort the bucket stably BACK-TO-FRONT by
     * centroid z (the tris carry real view-Z-derived depth), the depth-off
     * analogue of the PSX OT bucket order - finer, per-tri - so a rotated glyph
     * self-occludes correctly at every phase. Stability (z ties break on
     * submission index) keeps every constant-z run - the 2D-UI carries at z=0,
     * flat per-glyph quads - in submission order, i.e. exactly the old draw. */
    ut_first[6] = n;
    {
        hud_paint_t *ho = (hud_paint_t *)s_cz_hud.data;
        int hn = 0;
        for (int t = 0; t < scene_n; t++) {
            if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD) continue;
            ho[hn].z = hud_paint_depth(&scene[t]);
            ho[hn].t = t; hn++;
        }
        static int s_h6sort = -1;   /* OPENPETE_HUD6_NOSORT=1: A/B - submission order */
        if (s_h6sort < 0) { const char* e = getenv("OPENPETE_HUD6_NOSORT"); s_h6sort = !(e && *e && *e != '0'); }
        if (s_h6sort)
            qsort(ho, (size_t)hn, sizeof ho[0], hud_paint_cmp);
        /* OPENPETE_HUD6_DBG=frameLo: paint-order forensics - every bucket-6 tri
         * whose v0 lands in a fixed probe box, in SORTED (drawn) order. */
        static int s_h6 = -2;
        if (s_h6 == -2) { const char* e = getenv("OPENPETE_HUD6_DBG"); s_h6 = (e && *e) ? atoi(e) : -1; }
        int h6_live = s_h6 >= 0 && (int)s_scene_frame >= s_h6 && (int)s_scene_frame < s_h6 + 2;
        for (int i = 0; i < hn && n + 3 <= s_cz_verts; i++) {
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
            pack_scene_tri(&buf[n], &scene[t]);
            n += 3;
        }
    }
    ut_count[6] = n - ut_first[6];
    /* The single-category untextured buckets, packed from one classifying
     * count pass and two scatter passes instead of one full scan each:
     *   7  OVERLAY: HUD-OT 2D flat quads (letterbox bars), drawn dead last
     *      over everything (incl. the player); opaque, painter's order.
     *   8  HUD_PANEL: the semi-transparent menu dark panel, drawn
     *      alpha-blended at the start of the HUD layer; depth-test off.
     *   9  HUD3D: the in-game 3D HUD icon mobys, depth-tested (opaque
     *      gouraud_pipe after a mid-pass depth clear) so their faces Z-sort;
     *      per-vertex z is the real view-Z depth.
     *  10  PORTAL_GUARD: near-band terrain protection redraw, depth-tested
     *      opaque right after the portal-sky sequence.
     *  12  WORLD_FILL: terrain LP hole-fill, drawn after the WORLD group with
     *      the TEST_ZERO pipe.
     *  14  ACTOR_FRONT (blend 0): the rescue actors' flat-shaded faces, split
     *      out so the dragon-rescue pass can front-pass only these.
     * Each bucket keeps submission order and the vertex range it had, SKY
     * (bucket 11) still packs between 10 and 12, and a full buffer truncates
     * the buckets in the same order. */
    int utc_cnt[UTC_N], utc_cur[UTC_N], utc_end[UTC_N];
    utc_count(scene, scene_n, utc_cnt);
    n = utc_reserve(UTC_OVERLAY, UTC_WORLD_FILL, utc_cnt, n, s_cz_verts, utc_cur, utc_end, ut_first, ut_count);
    utc_scatter(UTC_OVERLAY, UTC_WORLD_FILL, scene, scene_n, buf, utc_cur, utc_end);
    /* SKY (cyclorama dome) + WORLD_FILL (terrain LP hole-fill) untextured
     * buckets. SKY draws before the group loop with the PLAIN opaque
     * pipe (depth-tested backdrop, no stencil mark); FILL draws right after
     * the WORLD group with the TEST_ZERO pipe. Both untextured by
     * construction (gouraud dome / gouraud LP). */
    ut_first[UT_BUCKET_SKY] = n;
    {
        /* SKY takes the same material deferral as the numbered groups, and
         * through the same predicate - a dome-wide selector is one of the
         * plainest things a mod can ask for, and without this the sky is the
         * one opaque channel no material can reach. One tag, so at most one
         * deferred span. */
        const int sky_defer = tag_split_on() &&
            ut_mat_spans_ok(UT_BUCKET_SKY, 0, OPENPETE_SCENE_SKY) &&
            s_utmat_n < UT_MAT_SPANS;
        int sdn = 0;
        for (int t = 0; t < scene_n; t++) {
            if (scene[t].textured || scene[t].category != OPENPETE_SCENE_SKY ||
                n + 3 > s_cz_verts) continue;
            if (sky_defer && scene[t].tag && ut_tag_resolves(scene[t].tag)) {
                s_utdef_idx[sdn++] = t;
                continue;
            }
            pack_scene_tri(&buf[n], &scene[t]);
            n += 3;
        }
        ut_count[UT_BUCKET_SKY] = n - ut_first[UT_BUCKET_SKY];   /* PLAIN span */
        n = ut_emit_deferred(UT_BUCKET_SKY, scene, buf, n, sdn);
    }
    /* Slot 13 is retired: untextured blend-3 content outside the shadow decals
     * rides the emission-ordered blended-world block with the rest of its
     * class, so it no longer has a bucket of its own. Its empty range sits
     * between WORLD_FILL and ACTOR_FRONT. */
    n = utc_reserve(UTC_WORLD_FILL, UTC_WORLD_FILL + 1, utc_cnt, n, s_cz_verts, utc_cur, utc_end, ut_first, ut_count);
    ut_first[13] = n;
    ut_count[13] = 0;
    n = utc_reserve(UTC_ACTOR_FRONT, UTC_N, utc_cnt, n, s_cz_verts, utc_cur, utc_end, ut_first, ut_count);
    utc_scatter(UTC_WORLD_FILL, UTC_N, scene, scene_n, buf, utc_cur, utc_end);
    /* PORTAL buckets: sky colours (PORTAL_CYCLORAMA) + their stencil
     * aperture masks (PORTAL_MASK), collected per-portal via the feeder's group
     * id. Draw site (after the WORLD group): per group - clear stencil ->
     * rasterize the mask invisibly, depth-TESTED against the world (the arch
     * stone / a nearer wall punches it out) -> draw the colours stencil-gated
     * and ALSO depth-TESTED (write off - "noz" is no z WRITE, not no z test),
     * so a colour whose depth disagrees with the mask's silently drops out.
     * Groups draw far->near by plane depth so a nearer portal
     * paints over a farther one seen through it; within a group, submission
     * order = the feeder's painter order (flat base -> dome). */
    struct portal_run { int group; int first, count; float depth; };
    enum { MAX_PORTAL_RUNS = 32 };
    struct portal_run pmask[MAX_PORTAL_RUNS], psky[MAX_PORTAL_RUNS];
    int pmask_n = 0, psky_n = 0;
    /* Material segments inside a sky run. The run itself must stay
     * one-per-portal - masks pair by group and groups sort by depth - so an
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
            if (scene[t].textured || scene[t].category != want || n + 3 > s_cz_verts) continue;
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
            pack_scene_tri(&buf[n], &scene[t]);
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
     *   buckets 0..3 = layered groups WORLD, SUBTRACT, ACTOR, PLAYER (SAME
     *                  partition as the draw loop)
     *   buckets 4, 5, 8, 10 = empty; bucket 11 = the emission-ordered
     *                  blended-world block (draw_blended_world)
     *   bucket  6    = HUD 2D textured backdrops (draw_hud_flat_layer)
     *   bucket  7    = PORTAL_GUARD; bucket 9 = ACTOR_FRONT opaque faces
     *   bucket 12    = HUD3D opaque textured faces (draw_hud3d_layer)
     *   bucket 13    = opaque WORLD portal inner-rim faces (draw_portal_rim)
     * Textured tris no bucket matches (HUD3D with a blend class) are dropped
     * from the block. Opaque buckets (0,2,3,9,12) are material-sorted - depth resolves visibility regardless of draw order, so it
     * is exact. Bucket 11 keeps emission order (bw_rank_sort on a replay);
     * the other buckets (1, 6, 7) keep submission order and only coalesce
     * already-adjacent runs. */
    NR_PROF_END();   /* cmp.build_ut */
    NR_PROF_BEGIN("cmp.build_tex");
    int tx_start = n;
    int txg_first[TX_BUCKETS], txg_count[TX_BUCKETS], txtri = 0;
    for (int b = 0; b < TX_BUCKETS; b++) {
        txg_first[b] = tx_start + txtri * 3;
        int run_start = txtri;
        const int gblend = b < 4 ? groups[b].blend : 0;
        const int gcat   = b < 4 ? groups[b].gcat : -1;
        for (int t = 0; t < scene_n; t++) {
            if (tx_start + (txtri + 1) * 3 > s_cz_verts) break;
            if (tx_bucket_belongs(b, scene, t, gblend, gcat)) s_txscene[txtri++] = t;
        }
        tx_bucket_order(b, scene, &s_txscene[run_start], txtri - run_start);
        txg_count[b] = (tx_start + txtri * 3) - txg_first[b];
    }
    NR_PROF_END();   /* cmp.build_tex */
    NR_PROF_BEGIN("cmp.vb_tex");
    for (int i = 0; i < txtri; i++) {
        const scene_tri_t *st = &scene[s_txscene[i]];
        /* The blended-world block carries untextured triangles too; they
         * draw through the gouraud pipes, which read no UV. Location 3
         * carries the moby paint (rank, ordinal) pair for every textured tri:
         * psx_scene.vert forwards it to the same-moby painter select, and
         * unranked tris (rank 0) keep the select off. */
        for (int k = 0; k < 3; k++)
            pack_scene_vert(&buf[tx_start + i * 3 + k], st, k,
                            st->textured ? st->v[k].u : 0.0f,
                            st->textured ? st->v[k].v : 0.0f);
    }
    n = tx_start + txtri * 3;

    /* Full-screen fade overlay (func_800190D4): a single full-panel quad at the
     * near plane (z=0 passes LESS_OR_EQUAL against everything), drawn dead last so
     * it tints the WHOLE composited frame - world, actors, HUD and 2D UI - exactly
     * as the PSX's semi-transparent full-screen POLY_F4 does. The vertex colour is
     * the fade colour; the blend mode (set below) reproduces the PSX ABR. */
    NR_PROF_END();   /* cmp.vb_tex */
    int fade_first = -1, fade_mode = -1;
    if ((scene_fade & (1u << 31)) && n + 6 <= s_cz_verts) {
        fade_mode  = (int)((scene_fade >> 24) & 3u);
        int fr = (int)((scene_fade >> 16) & 0xFFu);
        int fg = (int)((scene_fade >> 8) & 0xFFu);
        int fb = (int)(scene_fade & 0xFFu);
        /* mode 0 = (B+F)/2 -> alpha 128; add/subtract ignore alpha. */
        uint32_t fa = (fade_mode == 0) ? 0x80u : 0xFFu;
        uint32_t frgba = (uint32_t)fr | ((uint32_t)fg << 8) | ((uint32_t)fb << 16) | (fa << 24);
        float l, r, t, b;
        cover_rect(full_extent, &l, &r, &t, &b);
        /* Screen-space quad: view-Z 1 (the near guard), W 1. */
        const rhi_vertex tl = { l, t, 1.0f, 0, 0, frgba, 0, .w = 1.0f };
        const rhi_vertex tr = { r, t, 1.0f, 0, 0, frgba, 0, .w = 1.0f };
        const rhi_vertex br = { r, b, 1.0f, 0, 0, frgba, 0, .w = 1.0f };
        const rhi_vertex bl = { l, b, 1.0f, 0, 0, frgba, 0, .w = 1.0f };
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
    rhi_pipeline *freeze_pipe = ps_freeze_retained() ? s_nr.freeze_plain_pipe
                                                 : s_nr.freeze_overlay_pipe;
    if ((scene_freeze & (1u << 31)) && frozen_ok && freeze_pipe
        && n + 6 <= s_cz_verts) {
        uint32_t trgb = scene_freeze & 0xFFFFFFu;     /* rgb in R<<16|G<<8|B */
        uint32_t tr8 = (trgb >> 16) & 0xFFu, tg8 = (trgb >> 8) & 0xFFu, tb8 = trgb & 0xFFu;
        /* Fade-to-black shift (bits 24-30, 0..127) rides the vertex alpha as 0..1; the
         * freeze_overlay shader subtracts it from the desaturated luma (GS_ExitLevel
         * case 0). 0 for the static pause/inventory backdrop -> alpha 0 -> no shift. */
        uint32_t fade7 = (scene_freeze >> 24) & 0x7Fu;
        uint32_t ta8 = (fade7 * 255u + 63u) / 127u;    /* 0..127 -> 0..255 (rounded) */
        uint32_t trgba = tr8 | (tg8 << 8) | (tb8 << 16) | (ta8 << 24);  /* byte order R,G,B,A */
        /* blit.frag multiplies the sample by the vertex colour, so retention
         * passes white: the frozen frame reaches the screen unaltered. */
        if (ps_freeze_retained()) trgba = 0xFFFFFFFFu;
        /* Fill the render rect (cover_rect = the --aspect display rect, fit into the
         * window). frozen_tex is captured at the MATCHING aspect - 4:3 in the default
         * /A-B path, or the Hor+ widescreen view in vk-widescreen (see compose_variant)
         * - so this fills with no stretch and no pillarbox. cover_rect is the --aspect
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
             * ~0.2695 (f8b) or ~0.2359 (f10) with y in the V band - dump the whole tri */
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
         * (left clear -> black/bg), so the render respects --aspect instead of
         * stretching to the window. ws_factor==1 -> exactly the 4:3 rect. */
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
     * (so letterbox borders stay black) and then composes the scene over it - so
     * without this draw the live window shows BLACK sky wherever the dome has a
     * gap, even though screenshots (which read native_rtt) look correct. z=1.0
     * (far) + depth-write: the dome (also z=1) overdraws it via LESS_OR_EQUAL,
     * nearer geometry over that. */
    /* Backdrop deferral: the bg fill and the cyclorama dome are far-plane
     * (z=1.0) full-coverage fills that nearer opaque terrain almost entirely
     * overdraws - drawn first (the PSX painter order) every one of their
     * pixels is shaded and then repainted. Deferring the pair until after
     * the WORLD-opaque group lets early-Z reject every covered pixel, so
     * they shade only the gaps - identical output (both write z=1.0 and
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
            bind_scene_pipe(freeze_pipe);
            rhi_bind_vram(s_nr.dev, frz);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, 6, (uint32_t)freeze_first);
        }
    }

    /* GPU attribution (OPENPETE_GPU_TIMERS=1): one nested zone per layered
     * group below, so the [gpu-pass]/trace "compose" cost splits by layer. */
    static const char* const k_scene_zone[7] =
        { "sc.world", "sc.shadow", "sc.actor", "sc.player", "sc.alpha", "sc.add",
          "sc.qadd" };

    /* Draw the 7 layered groups in order (WORLD-opaque, SUBTRACT shadows,
     * ACTOR-opaque, PLAYER-opaque, ALPHA, ADDITIVE, QUARTER-ADD). For each: the batched
     * untextured range, then its textured tris as material-coalesced runs
     * (draw_tex_runs over the pre-bucketed/sorted block - one draw per material
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
    for (int gi = 0; gi < 7; gi++) {
        /* Freeze backdrop active (pause / inventory / flight-results): the world is
         * frozen INTO the backdrop (drawn above), and the canonical game render emits
         * no world/actor/player/shadow/transparent geometry while a menu is up - only
         * the HUD menu (text mobys, panel, 3D icons) on top. The high-FPS spine, by
         * contrast, blindly re-extracts the still-in-RAM gameplay actors every present;
         * drawing them here would paint full-colour Spyro/mobys over the green-tinted
         * backdrop (a ghost double-image). Skip the gameplay groups; the HUD layers
         * below still draw, so the animated menu stays smooth at the present rate. */
        if (freeze_first >= 0) continue;
        rhi_gpu_zone_begin(s_nr.dev, k_scene_zone[gi]);
        const struct scene_group *g = &groups[gi];
        const int utb = g->utb, txb = g->txb;
        /* Dragon-rescue cutscene (actor_front): just BEFORE the PLAYER group,
         * depth-isolate the cutscene actors and draw them there so they win
         * over terrain they interpenetrate (Delvin's wing inside the tower).
         * BEFORE the player, so Spyro still sorts against the dragon by real Z.
         * The regular ACTOR group already drew normally at gi==2.
         *
         * Two passes over the SAME two buckets:
         *  1. PUNCH - the colour-write-off twins (depth compare ALWAYS) replace
         *     the stored depth on exactly the pixels the actors rasterize, so
         *     whatever terrain occluded them is evicted from those pixels and
         *     nowhere else. Texel-exact: same shaders, same discards.
         *  2. COLOUR - the ordinary opaque pipes. Every actor fragment nearer
         *     than the punched value passes, and the usual LESS_OR_EQUAL
         *     ordering picks the nearest of them, so the actors keep exact
         *     self-occlusion (the punched value is one of their own depths, so
         *     the nearest is never rejected).
         *
         * The depth left behind is min(true actor depth, world depth) - the
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
                                  &tpipe, 1, hpipe, pass, 0);
                }
            }
        }
        /* SUBTRACT (shadow decal) group: engage the hardware slope-scaled
         * depth bias for these draws (both sub pipes are .depth_bias). This is
         * the replacement for a moby-shadow ×0.96 multiplicative view-Z
         * pull, which is distance-PROPORTIONAL (a dimensional error for a
         * coplanar tie-break); the hardware offset is
         * distance-invariant, slope-aware at grazing angles, and its clamp
         * bounds the total pull far below anything that could reorder a
         * genuine occluder.
         *   Vulkan rule: dynamic state is INVALIDATED by binding any pipeline
         * that has the same state static (every non-decal pipeline here), so
         * the bias must be set AFTER each depth_bias pipeline bind, before its
         * draws - never rely on a value set earlier in the pass. */
        if (g->utex && ut_count[utb] > 0) {
            if (gi == 1) {
                /* Untextured SUBTRACT = the Spyro silhouette shadow, fed as
                 * its fan plus the screen hull of its footprint prism. Preferred path: the
                 * DEPTH-AWARE decal (shadow_decal.frag samples the shared
                 * WORLD-opaque pre-pass depth and decides every hull pixel
                 * from the fan's footprint block). Fallback (pre-pass
                 * unavailable this present / unsupported device): plain depth
                 * test + hardware depth bias: the fed fan triangles draw with
                 * their gouraud as before, and the hull's black vertices
                 * subtract nothing. The decal discards the fan triangles. */
                if (s_od_ready && s_nr.opaque_depth) {
                    bind_player_shadow_decal();
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
            rhi_draw(s_nr.dev, (uint32_t)ut_count[utb], (uint32_t)ut_first[utb]);
        }
        if (s_utmat_n && g->utex && ut_mat_spans_ok(gi, g->blend, g->gcat))
            draw_ut_mat_spans(gi, g->utex, holefill_on && (gi == 0 || gi == 2),
                              g->blend);
        if (txg_count[txb] > 0 && s_nr.vram_tex && g->tex) {
            /* MOBY SHADOWS (gi==1) prefer the DEPTH-AWARE textured decal when the
             * shared WORLD-opaque depth pre-pass is available this present - the
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
                pc.mode = shadow_decal_mode(ps_shadow()->valid ? 1.0f : 0.0f);
                pc.band = shadow_decal_band();
                pc.plat_eps = ps_shadow()->plat_eps;
                pc.plat_slack = ps_shadow()->plat_slack;
                pc.uax = s_od_uax; pc.ubx = s_od_ubx;
                pc.vay = s_od_vay; pc.vby = s_od_vby;
                pc.pvw = s_od_vw;  pc.pvh = s_od_vh;
                scene_fc_affine dfc; scene_fragcoord_affine(&dfc);
                pc.fax = dfc.fax; pc.fbx = dfc.fbx;
                pc.fay = dfc.fay; pc.fby = dfc.fby;
                pc.tpage = decal_pack_tpage(m0->tpage_x, m0->tpage_y);
                pc.clut  = decal_pack_clut(m0->clut_x, m0->clut_y, m0->bpp);
                /* Mod material on the DECAL PROFILE. The stock fragment
                 * stage here owns per-pixel occlusion, so a plain material
                 * would delete it (shadows would paint through walls) -
                 * hence a second contract, openpete_psx_decal.glsl, whose
                 * engine half hands the mod the visibility verdict to
                 * apply. Layout is `mod_pipeline`'s with set 1 holding the
                 * VRAM and terrain key images; set 3 the engine/mod UBO pair.
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
                        rhi_pipe_site_visit(RHI_PIPE_SITE_MOD_MATERIAL, 1);
                        if (*ds == NULL) {
                            rhi_pipeline_desc dd = {
                                .vert_spv = "shadow_decal_tex.vert.spv",
                                .frag_spv = "shadow_decal_tex.frag.spv",
                                .blend = RHI_BLEND_SUBTRACT,
                                .depth_test = false, .wire_scene = true,
                                .mod_pipeline = true, .set1_vram_depth = true,
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
                pc.key_contest = shadow_decal_bind_keys();              /* set 1 */
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
                 * (WORLD's stencil-MARK variant gets the MARK twin so
                 * hole-fill coverage stays intact); alpha/additive -> the
                 * ztest/no-zwrite HD twins. SUBTRACTIVE (gi==1) stays SD - its
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
                 * stage owns occlusion - the very stage a material replaces.
                 * Allowing the override only on the non-decal fallback would
                 * make a mod's reach depend on the host's depth-sampling
                 * support, so neither path takes it. */
                int mat_ok = (gi == 1) ? 0
                           : (g->tex == s_nr.scene_tex_mark_pipe ? 2 : 1);
                draw_tex_runs(txg_first[txb], txg_count[txb], tx_start, scene, &g->tex, 1, hd,
                              mat_ok, g->blend);
            }
        }
        /* ACTOR_FRONT (rescue actors) at the normal actor layer - only when NO
         * cutscene is active (a stale g_DragonCutscene pointer can still match a
         * live moby outside states 1..6). During a cutscene they are front-passed
         * above (gi==3), so skip them here then. Same pipelines as the ACTOR
         * group: outside a cutscene this is pixel-identical to leaving them ACTOR. */
        if (gi == 2 && !actor_front) {
            /* ACTOR opaques also stencil-MARK their coverage when hole-fill is
             * on: the level authors cut HP ground holes under static decor
             * (dragon pedestal discs) - og never draws ground there, the moby
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
                              &atex, 1, acut, holefill_on ? 2 : 1, 0);
            }
        }
        /* Terrain sector-mesh GPU cache: the retained WORLD terrain (unclaimed
         * sectors) draws in the same slot as the CPU-fed WORLD group - opaque
         * depth-write, stencil-MARK when hole-fill is armed (identical raster
         * roles; the vertex stage owns the per-face verdicts). */
        draw_portal_rim(scene, tx_start, ut_first[16], ut_count[16],
                        txg_first[13], txg_count[13], holefill_on, gi);
        if (gi == 0 && openpete_tercache_frame_open()) {
            openpete_tercache_clip aff; tercache_clip_params(&aff);
            openpete_tercache_draw(OPTC_DRAW_WORLD, &aff, s_nr.vram_tex, holefill_on,
                                   s_nr.terrain_key_depth, s_nr.moby_key_depth);
        }
        /* WORLD_FILL - terrain LP hole-fill: depth-tested at TRUE LP
         * depth, gated on stencil==0 so it can only paint pixels no opaque
         * coverage MARK claimed (LP<->HP T-junction seams - where the sky shows
         * through). Runs at gi==2 AFTER the ACTOR opaques so authored HP holes
         * hidden under static mobys (pedestal discs) stay covered by the moby,
         * not repainted with LP; still before the portal sequence below, which
         * clears stencil. (Under the dragon-rescue actor_front reorder the
         * actors draw at gi==3 and don't mark - cutscene draw lists carry no
         * fill-backed decor, so the seam fill stays correct there.) */
        if (gi == 2 && holefill_on && ut_count[12] > 0) {
            /* (The former sky-fan stencil pre-mark is retired with the
             * plane-cover rule: fills that land inside an aperture draw at
             * TRUE depth behind the opening plane, and the preview - testing
             * at plane − M - repaints over them like any other covered
             * opaque.) */
            bind_scene_pipe(s_nr.gouraud_fill_pipe);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, (uint32_t)ut_count[12], (uint32_t)ut_first[12]);
        }
        draw_portal_rim_fill(ut_first[17], ut_count[17], holefill_on, gi);
        /* Terrain-cache LP hole-fill twin: stencil TEST_ZERO at true LP depth,
         * same slot as the scene-fed fill (before the portal stencil clear). */
        if (gi == 2 && holefill_on && openpete_tercache_frame_open()) {
            openpete_tercache_clip aff; tercache_clip_params(&aff);
            openpete_tercache_draw(OPTC_DRAW_FILL, &aff, s_nr.vram_tex, 0,
                                   s_nr.terrain_key_depth, s_nr.moby_key_depth);
        }
        /* The four blended world classes draw here, in the scene's own emission
         * order, with the terrain cache's blended classes inside the terrain
         * feed's span of that order. The per-class buckets the groups at gi 4,
         * 5 and 6 used to draw are empty. */
        if (gi == 4)
            draw_blended_world(scene, tx_start, txg_first[11], txg_count[11],
                               openpete_tercache_frame_open());
        /* Portal-sky previews: drawn AFTER the ACTOR group (gi==2), BEFORE the
         * PLAYER group - the native realization of the PSX sky bucket, which
         * painted over everything OT-behind it:
         *   - the sky depth-TESTS (LEQUAL at the portal-plane z, write OFF)
         *     against world+actor depth, so it repaints over mobys BEHIND the
         *     plane (they drew in the ACTOR group and wrote deeper z) while a
         *     moby IN FRONT of the arch keeps its pixels;
         *   - Spyro (PLAYER, drawn next) is never cut by it - he always sorts
         *     on top, incl. flying/walking through the portal (the earlier
         *     depth-STAMP variant clipped him mid-entry and was replaced by
         *     this repaint ordering);
         *   - the stencil mask confines everything to the aperture footprint
         *     (side-poke impossible), and intra-portal compositing stays
         *     painter order (backstops fed before the dome). */
        if (gi == 2 && s_nr.gouraud_noz_pipe && s_nr.portal_mask_pipe && psky_n > 0) {
            /* Per-portal stencil sequence, far->near by plane depth (a nearer
             * portal seen in front of a farther one paints last): clear
             * stencil -> mask (invisible, depth-tested) -> sky colours
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
                 * at the shadow group). Default neutral - the tie-break margin
                 * is already baked into the mask z (the feeder's world-unit
                 * PORTAL_SKY_EPS_VZ pull); OPENPETE_PORTAL_MASK_BIAS adds the
                 * optional close-range pull (see portal_mask_bias). NB a pull
                 * here moves only the MASK - the sky colours depth-test
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
                            rhi_pipe_site_visit(RHI_PIPE_SITE_MOD_MATERIAL, 1);
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
                     * overflowed - draw the run stock. */
                    bind_scene_pipe(s_nr.gouraud_noz_pipe);
                    rhi_draw(s_nr.dev, (uint32_t)(drawn ? sk->count - drawn : sk->count),
                             (uint32_t)(drawn ? sk->first + drawn : sk->first));
                }
            }
            /* PORTAL_GUARD: near-band terrain protection redraw - the PSX
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
                draw_tex_runs(txg_first[7], txg_count[7], tx_start, scene, &s_nr.scene_tex_pipe, 1,
                              s_nr.hd_cutout_pipe, 0, 0);
            }
        }
        /* GS_LevelTransition flyby ONLY (hud_under): draw the flat HUD layer here -
         * after the ACTOR group (gi==2) but BEFORE the PLAYER group (gi==3) - so the
         * fly-in Spyro draws in front of the revealed "THE ADVENTURE BEGINS" text.
         * In every other state the flat HUD draws OVER the player (after the loop). */
        if (gi == 2 && hud_under)
            draw_hud_flat_layer(scene, scene_n, ut_first, ut_count, tx_start, txg_first[6], txg_count[6]);
        /* (Dragon-rescue front pass now runs at the TOP of gi==3, before PLAYER -
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

    hud3d_tex_census(scene, scene_n,
                     draw_hud3d_layer(scene, tx_start, ut_first[9], ut_count[9],
                                      txg_first[12], txg_count[12]));

    /* OVERLAY layer (HUD-OT 2D flat quads - the cutscene letterbox bars): drawn
     * after every scene group, over everything incl. the player, depth-test off
     * (the opaque untextured HUD pipe). Before the fade, matching the PSX HUD-OT order (the fade
     * F4 is linked after the border, so it draws on top of the bars). */
    if (s_nr.gouraud_hud_pipes[0] && ut_count[7] > 0) {
        bind_scene_pipe(s_nr.gouraud_hud_pipes[0]);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count[7], (uint32_t)ut_first[7]);
    }

    /* Draw the full-screen fade overlay last (over every layer above), in the PSX
     * ABR blend: mode 2 (B-F) subtract -> fade to black; mode 1 (B+F) add -> fade to
     * white; mode 0 ((B+F)/2) -> alpha. These pipes are depth_write_off + the quad
     * is at view-Z 1 (depth 0), so it test-passes against the whole frame and
     * writes no depth. */
    if (fade_first >= 0) {
        /* Mode 2 uses gouraud_sub_FADE_pipe (plain subtractive, depth-off) -
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
 * The FPS counter drawn with Spyro's OWN 3D HUD text mobys
 * (the gold extruded glyph models), built game-side by text_moby_native and
 * pushed via openpete_native_set_overlay_glyphs. Pure display: drawn only on the
 * LIVE vk present (not screenshots, ab, or diff, so the comparison images stay
 * clean). Toggle at runtime via the SHOW FPS row in the settings menu (O), the
 * Performance stats row in the M overlay, or --perf-stats at launch.
 *
 * The overlay has three levels (openpete_perf_stats.h): off, the FPS counter,
 * and the counter plus a pacing readout. Only the level lives here - the
 * renderer needs it to gate the draw and to size the supersample region - and
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
 * only renderer state (s_nr and this file's statics), consumed by the next
 * native present; never gameplay RAM. */
static float  s_fov_deg;          /* 0 = default */
static double s_fov_tan_pending;  /* tan(fov/2), 0 at the default; set live */
static double s_fov_tan;          /* latched at scene begin */
/// @brief Latch the live tan(fov/2) setting for the scene now beginning.
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
     * like --aspect does at launch - the render rect then fills the window
     * exactly, no bars. Skipped when fullscreen (can't resize - the fixed
     * aspect letterboxes/pillarboxes instead) and under AUTO (0 - the render
     * follows the window, which stays as the user left it). ref_w/ref_h is
     * the crop-corrected 4:3 display shape (synthesized in pure vk), so the
     * target window shape is rect = wf × ref shape - the same rect
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
    s_cur_presnap = on ? 1 : 0;   /* sticky, like set_tag - clear after the feed */
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

/* Checked per present at the scene postfx split; no pipeline rebuild. */
void openpete_native_set_dither(int on) {
    s_dither = on ? 1 : 0;
    OPL_INFO_F("native", "dither -> %s",
               s_dither ? "on (PS1 15-bit colour + 4x4 dither)" : "off");
}

int openpete_native_get_dither(void) { return s_dither; }

/* Read by the terrain feeder at the top of every sector walk. */
void openpete_native_set_terrain_visibility(int mode) {
    s_terrain_vis = (mode == OPENPETE_TERRAIN_VIS_GAME) ? OPENPETE_TERRAIN_VIS_GAME
                                                        : OPENPETE_TERRAIN_VIS_FRUSTUM;
    OPL_INFO_F("native", "terrain-visibility -> %s",
               s_terrain_vis == OPENPETE_TERRAIN_VIS_GAME
               ? "game (the game's visibility verdict)"
               : "frustum (every sector inside the displayed frame)");
}

int openpete_native_get_terrain_visibility(void) { return s_terrain_vis; }

/* Queued; texmap_apply_pending re-bakes the pipelines at the next present. */
void openpete_native_set_texture_mapping(int affine) {
    s_texmap_pending = affine ? 1 : 0;
}

int openpete_native_get_texture_mapping(void) { return s_texmap_affine; }

/** @brief Apply a queued texture-mapping mode change by rebuilding the pipelines.
 *
 * Runs between frames. No effect when the requested mode equals the current one
 * or there is no device. Sets the affine-UV mode in the rhi and rebuilds; a
 * rebuild that fails for some pipelines is logged and the mode is still
 * recorded as changed. */
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
 * terrain push) paints mobys through terrain genuinely in front of them -
 * visible from ordinary gameplay angles (chests through the ledge lip they
 * sit behind), not just off-rail - so true depth is the standing rule and
 * OPENPETE_MOBY_TRUESCOPE=0 restores og's painter for A/B. Cutscene actors
 * are unaffected either way: the ACTOR_FRONT class never emits keys, and
 * their authored inside-terrain look (Delvin's wing through the tower) is
 * carried by the ACTOR_FRONT depth-punch pass. Same re-bake shape as
 * texture-mapping. */
static int s_moby_truescope = 1;   /* currently-baked value (pipelines start at 1) */
/** @brief Re-bakes the pipelines when OPENPETE_MOBY_TRUESCOPE changed since the last bake; the knob is re-read when the registry env epoch changes. */
static void truescope_apply_pending(void) {
    if (!s_nr.dev) return;
    static int want = 1;
    static unsigned seen = 0;
    if (knob_env_epoch_stale(&seen)) {
        const char* e = getenv("OPENPETE_MOBY_TRUESCOPE");
        want = (e && *e) ? (*e != '0') : 1;
    }
    if (want == s_moby_truescope) return;
    rhi_set_moby_truescope(want);
    int fails = rhi_rebuild_pipelines(s_nr.dev);
    s_moby_truescope = want;
    OPL_INFO_F("native", "moby key contest -> %s (pipelines re-baked)",
               want ? "true-depth scoped" : "og compound painter (A/B)");
    if (fails)
        OPL_ERROR_F("native", "moby-truescope: %d pipeline(s) failed to rebuild (kept previous mode)", fails);
}

/* Untextured lighting view - same queued-rebuild shape as texture-mapping. */
void openpete_native_set_notex(int on) {
    s_notex_pending = on ? 1 : 0;
}

int openpete_native_get_notex(void) { return s_notex; }

/* Wireframe view - the same queued-rebuild shape as notex. */
void openpete_native_set_wireframe(int on) {
    s_wireframe_pending = on ? 1 : 0;
}

int openpete_native_get_wireframe(void) { return s_wireframe; }

/** @brief Apply a queued wireframe change by rebuilding the pipelines.
 *
 * Runs between frames. No effect when the requested mode equals the current one
 * or there is no device. Logs the change and the rhi wireframe report; a
 * rebuild that fails for some pipelines is logged and the mode is still
 * recorded as changed. */
static void wireframe_apply_pending(void) {
    if (s_wireframe_pending == s_wireframe || !s_nr.dev) return;
    rhi_set_wireframe(s_wireframe_pending);
    int fails = rhi_rebuild_pipelines(s_nr.dev);
    s_wireframe = s_wireframe_pending;
    OPL_INFO_F("native", "wireframe -> %s (pipelines re-baked)", s_wireframe ? "on" : "off");
    wireframe_report(s_nr.dev);
    if (fails)
        OPL_ERROR_F("native", "wireframe: %d pipeline(s) failed to rebuild (kept previous mode)", fails);
}

/** @brief Apply a queued untextured-lighting-view change by rebuilding the pipelines.
 *
 * Runs between frames. No effect when the requested mode equals the current
 * one or there is no device. A failed pipeline rebuild is logged. */
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
 * only before the font has ever loaded - there is no bitmap fallback). Carries
 * the performance-stats readout; reusable for other text overlays. */
/* Glyph list handoff. The stats readout, version line and settings menu
 * lists are built on the sim thread and drawn on the presenting thread. A
 * setter copies its list into the list's stage under s_glyph_mx and marks it
 * changed; glyph_lists_latch, at the start of each present's compose, copies
 * every changed stage into the arrays the draws read, under the same lock.
 * A present therefore draws one whole list, never a count from one push
 * with vertices from another. */
static void *s_glyph_mx;   /* SDL_Mutex, openpete_mutex_once */
typedef struct { openpete_hud_vert *stage; int stage_n; int changed; } glyph_stage_t;

/** @brief Copy a glyph list into its stage under s_glyph_mx and mark it changed.
 *
 * @param st The list's stage.
 * @param cap The list's capacity in vertices.
 * @param verts The vertices; NULL or nverts <= 0 publishes an empty list.
 * @param nverts The vertex count, clamped to 0..cap. */
static void glyph_stage_push(glyph_stage_t *st, int cap, const openpete_hud_vert *verts, int nverts)
{
    if (nverts < 0 || !verts) nverts = 0;
    if (nverts > cap) nverts = cap;
    SDL_Mutex *mx = openpete_mutex_once(&s_glyph_mx);
    SDL_LockMutex(mx);
    if (nverts > 0) memcpy(st->stage, verts, (size_t)nverts * sizeof(verts[0]));
    st->stage_n = nverts;
    st->changed = 1;
    SDL_UnlockMutex(mx);
}

/** @brief Copy a changed stage into the draw-side list; the caller holds s_glyph_mx. */
static void glyph_stage_take(glyph_stage_t *st, openpete_hud_vert *dst, int *dst_n)
{
    if (!st->changed) return;
    if (st->stage_n > 0) memcpy(dst, st->stage, (size_t)st->stage_n * sizeof(dst[0]));
    *dst_n = st->stage_n;
    st->changed = 0;
}

static openpete_hud_vert s_overlay_glyph_verts[OVL_MAX_VERTS];   /* draw side */
static int               s_overlay_glyph_n = 0;
static openpete_hud_vert s_overlay_glyph_stage_v[OVL_MAX_VERTS];
static glyph_stage_t     s_overlay_glyph_stage = { s_overlay_glyph_stage_v, 0, 0 };
void openpete_native_set_overlay_glyphs(const openpete_hud_vert *verts, int nverts) {
    glyph_stage_push(&s_overlay_glyph_stage, OVL_MAX_VERTS, verts, nverts);
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

/* ---- the build-version line ---------------------------------------------
 * Small text in a corner naming the build (core/include/openpete_build_id.h),
 * built from the same 3D glyph models as the stats readout and pushed once -
 * the string does not change. It is the cheapest way for a screenshot of a
 * build that is not a release to say which build it is.
 *
 * A tagged build shows it briefly and fades it out; an untagged one keeps it
 * up for the whole session, because that is the build whose identity nobody
 * can otherwise recover from an image.
 *
 * Not a toast: toasts are the game's bordered dialog chrome at a
 * user-chosen anchor, they expire (OPENPETE_OSD_TIMEOUT_MAX_MS), and a
 * session-long one would hold a ring slot the whole time. This is bare text
 * at a fixed anchor, drawn through the shared glyph path. */
static openpete_hud_vert s_ver_glyph_verts[VER_MAX_VERTS];   /* draw side */
static int               s_ver_glyph_n = 0;
static openpete_hud_vert s_ver_glyph_stage_v[VER_MAX_VERTS];
static glyph_stage_t     s_ver_glyph_stage = { s_ver_glyph_stage_v, 0, 0 };
/* Region extent as one packed 64-bit word, like the stats block's: the
 * builder writes it on the sim thread and the compose reads it on the present
 * thread, and a torn width/height pair would claim a different rect on the
 * anchor ledger than the glyphs fill. */
static uint64_t          s_ver_extent = 0;

void openpete_native_set_version_glyphs(const openpete_hud_vert *verts, int nverts,
                                        float region_w, float region_h) {
    glyph_stage_push(&s_ver_glyph_stage, VER_MAX_VERTS, verts, nverts);
    if (region_w > 0.0f && region_h > 0.0f) {
        uint32_t bw, bh;
        memcpy(&bw, &region_w, sizeof bw);
        memcpy(&bh, &region_h, sizeof bh);
        s_ver_extent = (uint64_t)bw | ((uint64_t)bh << 32);
    }
}

/** @brief Return the version line's published region size.
 *
 * Both halves come from one read of the packed extent, so they belong to
 * the same openpete_native_set_version_glyphs call. A size never published,
 * or not positive, reads as 0.
 * @param w Receives the width; may be NULL.
 * @param h Receives the height; may be NULL. */
static void version_region(float *w, float *h) {
    const uint64_t v = s_ver_extent;   /* one read: both halves from one publish */
    uint32_t bw = (uint32_t)v, bh = (uint32_t)(v >> 32);
    float fw = 0.0f, fh = 0.0f;
    memcpy(&fw, &bw, sizeof fw);
    memcpy(&fh, &bh, sizeof fh);
    if (w) *w = (fw > 0.0f) ? fw : 0.0f;
    if (h) *h = (fh > 0.0f) ? fh : 0.0f;
}

/* --version-osd: the knob's own value. It governs the release build's brief
 * line alone - see openpete_native_version_line_enabled. */
static int s_version_osd = 1;
void openpete_native_set_version_osd(int on) { s_version_osd = on ? 1 : 0; }
int  openpete_native_version_osd(void) { return s_version_osd; }

/* Version-line gate; documented at the declaration. */
int openpete_native_version_line_enabled(void) {
    if (!openpete_build_shared_recomp()) return 0;
    if (!openpete_build_tagged()) return 1;
    return s_version_osd;
}

/* One phrase naming the state above, for the shutdown census. */
const char *openpete_native_version_reason(void) {
    if (!openpete_build_shared_recomp())
        return "not a shared-recomp build, no line";
    if (!openpete_build_tagged())
        return "untagged shared build, whole session, not suppressible";
    return s_version_osd ? "release build, brief"
                         : "release build, suppressed by --version-osd off";
}

/* Lines actually drawn, cumulative. A capture that greened with zero here
 * proved nothing about the version line. */
static unsigned s_ver_drawn = 0;
unsigned openpete_native_version_drawn(void) { return s_ver_drawn; }

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
static openpete_hud_vert s_menu_glyph_verts[MENU_GLYPH_MAX];   /* draw side */
static int               s_menu_glyph_n = 0;
static openpete_hud_vert s_menu_glyph_stage_v[MENU_GLYPH_MAX];
static glyph_stage_t     s_menu_glyph_stage = { s_menu_glyph_stage_v, 0, 0 };
void openpete_native_set_menu_glyphs(const openpete_hud_vert *verts, int nverts) {
    glyph_stage_push(&s_menu_glyph_stage, MENU_GLYPH_MAX, verts, nverts);
}

/** @brief Take every changed glyph list (stats readout, version line, menu) for this present's draws. */
static void glyph_lists_latch(void)
{
    SDL_Mutex *mx = openpete_mutex_once(&s_glyph_mx);
    SDL_LockMutex(mx);
    glyph_stage_take(&s_overlay_glyph_stage, s_overlay_glyph_verts, &s_overlay_glyph_n);
    glyph_stage_take(&s_ver_glyph_stage, s_ver_glyph_verts, &s_ver_glyph_n);
    glyph_stage_take(&s_menu_glyph_stage, s_menu_glyph_verts, &s_menu_glyph_n);
    SDL_UnlockMutex(mx);
}

/* ---- OSD toast geometry channel ----------------------------------------
 * Synchronized like the glyph lists above, with its own lock: the OSD
 * re-emits on every sim tick a toast is live, and its table is what the
 * compositor resolves toast ids through - a half-copied table doesn't smear
 * a glyph, it points a quad at another toast's slot. Geometry and table
 * therefore move together under one lock. */
static void             *s_osd_mx;   /* SDL_Mutex, openpete_mutex_once */
static openpete_hud_vert s_osd_glyph_verts[OSD_GLYPH_MAX];
static int               s_osd_glyph_n = 0;
static openpete_osd_slot_t s_osd_slots[OPENPETE_OSD_MAX];
static int               s_osd_slot_n = 0;

/** @brief The OSD publish lock, created on first use without a creation race. */
static SDL_Mutex *osd_pub_mx(void) {
    return openpete_mutex_once(&s_osd_mx);
}

/* Documented at the declaration. */
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

/** @brief Read the OSD slot size in pixels that ensure_osd_ss published.
 *
 * Both halves come from one 64-bit read, so a concurrent republish cannot
 * pair a new width with a stale height. Zero in both until the grid has
 * been allocated.
 * @param w Receives the slot width in pixels; may be NULL.
 * @param h Receives the slot height in pixels; may be NULL. */
static void osd_slot_px(int *w, int *h) {
    const uint64_t v = s_nr.osd_slot_px;   /* one read: both halves from one publish */
    if (w) *w = (int)(uint32_t)v;
    if (h) *h = (int)(uint32_t)(v >> 32);
}

void openpete_native_osd_slot_extent(float *w, float *h) {
    /* A slot origin off the OSD_SS_FACTOR grid would make edge blocks
     * average in the neighbouring slot's texels. */
    const float ph = (s_nr.win_h > 0) ? (float)s_nr.win_h : 1.0f;
    int pw, phh;
    osd_slot_px(&pw, &phh);
    if (w) *w = (pw  > 0) ? (float)pw  / ph : OSD_SLOT_W;
    if (h) *h = (phh > 0) ? (float)phh / ph : OSD_SLOT_H;
}

/** @brief Draw a list of 3D text-moby glyph vertices over the current render pass.
 *
 * Serves the stats readout, the version line and the OSD toasts. Vertices
 * arrive in overlay space, fractions of the window height with the origin
 * top-left and +y down, with a depth in [0,1); each maps to NDC as
 * ((x + ox) * ax - 1, 1 - (y + oy) * ay). With @p pipe NULL the list draws
 * depth-tested through the opaque gouraud pipe after a local depth clear, so
 * the glyphs Z-sort among their own faces on top of the whole frame, as the
 * HUD3D pass does. A caller that fades the list passes a blended HUD pipe,
 * whose depth test is off, and no depth clear runs.
 * @param verts Glyph vertices, three per triangle.
 * @param nverts Vertex count.
 * @param vb Vertex buffer to upload into.
 * @param cap Vertex capacity of @p vb, clamped to the static staging array.
 * @param ax Overlay-to-NDC scale x.
 * @param ay Overlay-to-NDC scale y.
 * @param ox Overlay-space origin shift x.
 * @param oy Overlay-space origin shift y.
 * @param pipe Blended HUD pipe, or NULL for the depth-isolated opaque path.
 * @param alpha8 Scale for every vertex's alpha byte, 0 to 255, or negative
 *        to keep the alpha.
 * @return true when a triangle was drawn. */
static bool draw_glyph_list_3d(const openpete_hud_vert *verts, int nverts,
                               rhi_buffer *vb, int cap,
                               float ax, float ay, float ox, float oy,
                               rhi_pipeline *pipe, int alpha8) {
    /* `pipe` NULL selects the depth-isolated opaque path below; a caller that
     * wants the list to fade passes a blended HUD pipe (depth test already
     * off) and an alpha, which scales every vertex's alpha byte. */
    if (!pipe) pipe = s_nr.gouraud_pipe;
    if (nverts < 3 || !verts || !vb || !pipe) return false;
    static rhi_vertex buf[OSD_GLYPH_MAX > OVL_MAX_VERTS ? OSD_GLYPH_MAX : OVL_MAX_VERTS];
    if (cap > (int)(sizeof buf / sizeof buf[0])) cap = (int)(sizeof buf / sizeof buf[0]);
    int n = 0;
    for (int i = 0; i < nverts && n < cap; i++) {
        const openpete_hud_vert *v = &verts[i];
        /* Glyph coords are win_h-relative; ax/ay map them into the target's NDC.
         * Offscreen 1×: ax=2·win_h/win_w, ay=2. Overlay SS: ax=2/ovl_region_w(),
         * ay=2/ovl_region_h() - both keep glyphs square (ax/ay carry the same
         * win_h scale through the target's pixel dims). (ox,oy) shifts the origin
         * in overlay space - the 1× fallback path uses it to land on the block's
         * anchored rect; the SS prepass passes (0,0), since there the composite
         * quad is what moves and the glyphs fill their region. */
        buf[n].x = (v->x + ox) * ax - 1.0f;
        buf[n].y = 1.0f - (v->y + oy) * ay;
        buf[n].z = v->z; buf[n].u = 0.0f; buf[n].v = 0.0f;
        buf[n].rgba = v->rgba; buf[n].koff = 0.0f; buf[n].w = v->z;
        if (alpha8 >= 0) {
            const uint32_t a = (uint32_t)(((buf[n].rgba >> 24) * (uint32_t)alpha8) / 255u);
            buf[n].rgba = (buf[n].rgba & 0x00FFFFFFu) | (a << 24);
        }
        buf[n].sx = 0.0f; buf[n].sy = 0.0f;   /* unranked: rank select off */
        n++;
    }
    n -= n % 3;
    if (n <= 0) return false;
    rhi_update_buffer(vb, buf, (size_t)n * sizeof(rhi_vertex));
    rhi_reset_scissor(s_nr.dev);
    /* The opaque path clears depth so the glyphs z-sort among their own faces
     * on top of the frame. A blended HUD pipe has the depth test off already,
     * and clearing under it would throw away the scene depth the rest of the
     * pass still needs. */
    if (pipe == s_nr.gouraud_pipe) rhi_clear_depth(s_nr.dev);
    bind_scene_pipe(pipe);
    rhi_bind_vertex_buffer(s_nr.dev, vb);
    rhi_draw(s_nr.dev, (uint32_t)n, 0);
    return true;
}

/** @brief Draw the stats readout's glyph list through draw_glyph_list_3d on the opaque path.
 *
 * @param ax Overlay-to-NDC scale x.
 * @param ay Overlay-to-NDC scale y.
 * @param ox Overlay-space origin shift x.
 * @param oy Overlay-space origin shift y.
 * @return true when a triangle was drawn. */
static bool draw_overlay_glyphs_3d(float ax, float ay, float ox, float oy) {
    return draw_glyph_list_3d(s_overlay_glyph_verts, s_overlay_glyph_n,
                              s_nr.overlay_vb, OVL_MAX_VERTS, ax, ay, ox, oy,
                              NULL, -1);
}

/** @brief Draw the mod draw-service overlay (rects and text glyphs) into the currently open offscreen pass.
 *
 * Uses the overlay space, NDC mapping and depth isolation of
 * draw_glyph_list_3d, its own vertex buffer, and a direct 1x draw with no
 * supersample pre-pass. The vertices come from the composed scene slot when
 * one is set, else the live list. It runs in screenshot and record composes
 * too: a mod's HUD is content, not chrome. */
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
        buf[n].rgba = v->rgba; buf[n].koff = 0.0f; buf[n].w = v->z;
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

/** @brief Return whether chrome is drawn into a --record video.
 *
 * The answer is --record-fps-overlay when set, else OPENPETE_FPS_IN_RECORD
 * set to a value other than 0, else no. It is resolved on first use and
 * cached; openpete_native_set_fps_in_record clears the cache so a live
 * toggle re-resolves. The stats overlay, the version line and the OSD
 * toasts read it.
 * @return 1 when chrome is recorded, else 0. */
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

/** @brief Return whether the stats overlay draws on this present.
 *
 * Requires the stats level to be on, a known window size and at least one
 * glyph triangle. While --record is active the overlay is kept out of the
 * video unless fps_in_record_resolved allows chrome in it.
 * @return true when the overlay draws. */
static bool perf_overlay_should_draw(void) {
    if (s_perf_stats == OPENPETE_PERF_OFF || s_nr.win_w <= 0 || s_nr.win_h <= 0 || s_overlay_glyph_n < 3) return false;
    if (openpete_record_active() && !fps_in_record_resolved()) return false;
    return true;
}

/** @brief Push overlay_resolve.frag's constants: the supersample factor and the outline width.
 *
 * @param ssaa The layer's supersample factor.
 * @param outline_px The dark outline width in output pixels; 0 draws none. */
static void push_overlay_resolve(int ssaa, int outline_px) {
    const int pc[2] = { ssaa, outline_px };
    rhi_push_constants(s_nr.dev, pc, sizeof(pc));
}

/** @brief Return whether the stats overlay's supersample path is usable.
 *
 * The path needs the overlay resolve pipeline, the composite quad buffer and
 * the ovl_ss target at OVL_SS_FACTOR (4).
 * @return true when all three exist. */
static bool ovl_ss_ready(void) {
    return s_nr.ovl_resolve_pipe && s_nr.ovl_quad_vb && s_nr.ovl_ss;
}

/* Overlay-x position (win_h units) for the FPS counter's region: flush top-left
 * normally, but aligned to the settings-menu panel's right edge while it's open
 * so the two don't overlap. The offset is exactly panel_x1 (no extra gap): the
 * region's left edge then sits on the panel edge just as it sits on the window's
 * left edge when closed, so the FPS text's own left bearing reproduces the SAME
 * visual gap in both cases. Only the on-screen placement moves - the glyphs (and
 * the SS target they render into) are unchanged. */
/* Overlay region for the perf readout: the rect the supersample target, the
 * composite quad and the glyph projection must ALL agree on, or the text lands
 * in a shape other than the one it was rasterized into. Exactly one side owns
 * it - the game-side builder, which is the side that can measure a string -
 * and pushes it here; the renderer reads it back and never re-derives it. */
/* Published as ONE 64-bit word rather than two floats. The builder writes it
 * on the sim thread and the compose reads it on the present thread, and a torn
 * PAIR - new width, stale height - would build the composite quad for a
 * different rect than the supersample target holds, landing the readout in a
 * shape it was not rasterized into. An aligned 64-bit store is single-copy on
 * every target this builds for; two independent float stores are not one
 * event. (Same class as the glyph publish below, found by auditing the
 * sim->present statics rather than by seeing it go wrong.) */
static uint64_t s_ovl_extent =
    ((uint64_t)0) ;   /* 0 = never pushed; the OVL_REGION_* fallback applies */

/** @brief Extract one float half of the packed overlay extent word.
 *
 * @param v Packed word: width bits in the low 32, height bits in the high 32; 0 means never pushed.
 * @param hi Non-zero selects the height half, zero the width half.
 * @param fallback Returned when v is 0 or the half is not above 0.
 * @return The selected extent in window-height units, or @p fallback. */
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
 * everything downstream - the target size, the ledger claim, the composite
 * quad, the prepass projection - reads the latch. Reading the publish at each
 * of those would let a mid-present push change the rect between the target
 * that was allocated and the quad that samples it, which is the same torn-pair
 * failure one field down. */
static float s_perf_rw = OVL_REGION_W, s_perf_rh = OVL_REGION_H;
/** @brief Return the perf overlay region width latched for this present, in window-height units. */
static float ovl_region_w(void) { return s_perf_rw; }
/** @brief Return the perf overlay region height latched for this present, in window-height units. */
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

/** @brief Round an overlay-space length or offset to a whole output pixel.
 *
 * Overlay-space values are in units of the window height, so the value is
 * scaled by @p win_h, rounded half away from zero, and scaled back.
 * @param v Value in overlay space.
 * @param win_h Window height in pixels; a value not above 0 returns @p v
 *        unchanged.
 * @return The snapped value. */
static float snap_px(float v, float win_h) {
    if (win_h <= 0.0f) return v;
    return (float)(int)(v * win_h + (v >= 0.0f ? 0.5f : -0.5f)) / win_h;
}

/* How long a tagged build holds its version line at full opacity, and how
 * long the fade out takes. An untagged build never leaves the hold. */
#define VER_HOLD_NS  ((op_time_t)2000 * OP_NS_PER_MS)
#define VER_FADE_NS  ((op_time_t)300  * OP_NS_PER_MS)

/* The first present at which the line HAD geometry, which is where its dwell
 * starts. The glyph models come out of the game's own font, which is not in
 * RAM until the game has loaded it; a timer started at launch would run out
 * while there was still nothing to draw. */
static op_time_t s_ver_birth = 0;
/* The alpha and placement latched for this present, resolved once in the
 * layout pass so the ledger claim and the draw cannot disagree about whether
 * the line is up. */
static int   s_ver_alpha8 = 0;
static float s_ver_ox, s_ver_oy;

/** @brief Return the smoothstep 3x^2 - 2x^3 of x clamped to 0..1. */
static float ver_smoothstep(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

/** @brief Return the version line's opacity for this present, 0 to 255.
 *
 * 0 while the line is disabled, has fewer than 3 glyph vertices, the window has
 * no size, or a recording without the stats override is active. The dwell clock
 * starts at the first present with geometry (s_ver_birth) and is reset if the
 * present clock goes backwards. An untagged build (openpete_build_tagged) stays
 * at 255; a tagged build holds 255 for VER_HOLD_NS then fades to 0 over
 * VER_FADE_NS along a smoothstep.
 * @return Alpha, 0 to 255. */
static int version_overlay_alpha(void) {
    if (!openpete_native_version_line_enabled() || s_ver_glyph_n < 3) return 0;
    if (s_nr.win_w <= 0 || s_nr.win_h <= 0) return 0;
    /* Chrome stays out of a --record video under the same override the stats
     * block uses. */
    if (openpete_record_active() && !fps_in_record_resolved()) return 0;

    const op_time_t now = openpete_interp_present_now();
    if (s_ver_birth == 0 || now < s_ver_birth) s_ver_birth = now;
    if (!openpete_build_tagged()) return 255;   /* whole session */

    const op_time_t age = now - s_ver_birth;
    if (age >= VER_HOLD_NS + VER_FADE_NS) return 0;
    if (age <= VER_HOLD_NS) return 255;
    const float t = (float)((double)(age - VER_HOLD_NS) / (double)VER_FADE_NS);
    int a8 = (int)((1.0f - ver_smoothstep(t)) * 255.0f + 0.5f);
    if (a8 < 0) a8 = 0;
    if (a8 > 255) a8 = 255;
    return a8;
}

/** @brief Latch this present's overlay extents and place the stats block and the version line on the anchor ledger.
 *
 * Reads the published perf extent once into s_perf_rw and s_perf_rh, so the
 * supersample target, the composite quad and the projection agree for the
 * present, and snaps it to whole output pixels. Resets the anchor ledger to
 * the usable x range of the overlay aspect. When the stats overlay draws it
 * claims its anchor with margin 0 and snaps the placement to whole pixels.
 * It then resolves the version line's alpha (s_ver_alpha8) and, when
 * non-zero, claims the bottom-left anchor for it with margin 0 and snaps
 * that placement, before the toasts stack against what is left. Runs at the
 * top of the per-present resource pass. */
static void overlay_layout_update(void) {
    const float ph = (s_nr.win_h > 0) ? (float)s_nr.win_h : 0.0f;
    openpete_native_get_overlay_extent(&s_perf_rw, &s_perf_rh);
    /* Quantize the REGION to whole output pixels. The supersample target is
     * allocated at round(region * win_h) * N texels, so a region whose pixel
     * size is fractional makes the composite quad cover a slightly different
     * area than the target holds - the resolve's N×N blocks then straddle
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
         * inward, off the frame edge and onto the picture - which reads as a
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
         * pixel, which reads as a soft counter - the same 1:1
         * output-pixel-to-block rule the toast quads already follow. */
        s_perf_ox = snap_px(s_perf_ox, ph);
        s_perf_oy = snap_px(s_perf_oy, ph);
    }
    /* The version line claims next: persistent chrome, so it settles before
     * the transient toasts and they stack away from it. Margin 0 for the same
     * reason as the perf block - the region carries its own inset. */
    s_ver_alpha8 = version_overlay_alpha();
    if (s_ver_alpha8 > 0) {
        float vw, vh;
        version_region(&vw, &vh);
        openpete_anchor_place(&s_anchor_ledger, OPENPETE_ANCHOR_BOTTOM_LEFT,
                              snap_px(vw, ph), snap_px(vh, ph), 0.0f,
                              &s_ver_ox, &s_ver_oy);
        s_ver_ox = snap_px(s_ver_ox, ph);
        s_ver_oy = snap_px(s_ver_oy, ph);
    }
}

/** @brief Create or resize one text block's supersample target at OVL_SS_FACTOR times its region.
 *
 * The region is converted to pixels through the window height, and each
 * dimension is clamped to the device's largest image. The target is
 * destroyed and reallocated only when the pixel size changes, with a device
 * wait-idle, so the function runs outside a frame. A NULL target afterwards
 * leaves the caller on its 1x fallback. Shared by the stats block and the
 * version line, which differ only in their region and their name.
 * @param tex Target slot; replaced on a size change.
 * @param cur_w Recorded target width, updated on a size change.
 * @param cur_h Recorded target height, updated on a size change.
 * @param region_w Region width in window-height units.
 * @param region_h Region height in window-height units.
 * @param what Block name for the allocation log line. */
static void ensure_text_ss_target(rhi_texture **tex, int *cur_w, int *cur_h,
                                  float region_w, float region_h,
                                  const char *what) {
    int rw = (int)(region_w * (float)s_nr.win_h + 0.5f);
    int rh = (int)(region_h * (float)s_nr.win_h + 0.5f);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    int want_w = rw * OVL_SS_FACTOR, want_h = rh * OVL_SS_FACTOR;
    uint32_t maxdim = rhi_max_image_dimension(s_nr.dev);
    if ((uint32_t)want_w > maxdim) want_w = (int)maxdim;
    if ((uint32_t)want_h > maxdim) want_h = (int)maxdim;
    if (*tex && *cur_w == want_w && *cur_h == want_h) return;
    rhi_wait_idle(s_nr.dev);
    if (*tex) { rhi_destroy_texture(s_nr.dev, *tex); *tex = NULL; }
    *tex = rhi_create_render_target(s_nr.dev, (uint32_t)want_w, (uint32_t)want_h);
    *cur_w = want_w; *cur_h = want_h;
    if (*tex)
        OPL_INFO_F("native", "%s SS target %dx%d (%d×)", what, want_w, want_h, OVL_SS_FACTOR);
}

/** @brief Size the stats overlay supersample target and rebuild its composite quad.
 *
 * ovl_ss is (re)created at OVL_SS_FACTOR times the overlay region. The
 * composite quad, the overlay's screen rect in offscreen NDC, is rebuilt
 * when the window size, the overlay anchor or the region size changed, never
 * per present. Creation and the quad rewrite wait for the device to idle, so
 * the call belongs outside a frame, beside ensure_ss_rtt. No effect when the
 * resolve pipeline or the quad buffer failed at init (draw_perf_overlay then
 * takes the 1x path) or the window has no extent. */
static void ensure_ovl_ss(void) {
    if (!s_nr.ovl_resolve_pipe || !s_nr.ovl_quad_vb || s_nr.win_w <= 0 || s_nr.win_h <= 0) return;
    ensure_text_ss_target(&s_nr.ovl_ss, &s_nr.ovl_ss_w, &s_nr.ovl_ss_h,
                          ovl_region_w(), ovl_region_h(), "stats overlay");
    /* Composite quad - the overlay's screen rect in offscreen NDC. Left edge sits
     * at overlay-x = want_ox (0 = flush top-left; shifted right of the panel while
     * the menu is open, see perf_overlay_ox). Rect width is win_h-relative (glyph x
     * uses win_h), so its NDC width scales by win_h/win_w; UV 0..1 maps the whole
     * SS target (v0 top, top-down). Rebuilt on resize OR when the offset changes
     * (menu open/close) - never per-frame, so the wait_idle stays rare. */
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
         * in-flight frame may still read it - fully sync first (like panels_vb).
         * The anchor moves on a menu open/close, a stats-level switch or a
         * user setting, never per present, so the idle stays rare. */
        rhi_wait_idle(s_nr.dev);
        rhi_update_buffer(s_nr.ovl_quad_vb, q, sizeof(q));
        s_nr.ovl_quad_win_w = s_nr.win_w; s_nr.ovl_quad_win_h = s_nr.win_h;
        s_nr.ovl_quad_ox = want_ox; s_nr.ovl_quad_oy = want_oy;
        s_nr.ovl_quad_rw = want_rw; s_nr.ovl_quad_rh = want_rh;
    }
}

/** @brief Render the stats overlay glyphs into ovl_ss at OVL_SS_FACTOR, cleared transparent.
 *
 * The pass is its own render-target pass, so it runs before the offscreen
 * pass opens; render passes do not nest. draw_perf_overlay composites the
 * result. No-op unless the supersample path is ready and the overlay draws
 * on this present. */
static void prepare_perf_overlay_ss(void) {
    if (!ovl_ss_ready() || !perf_overlay_should_draw()) return;
    rhi_begin_render_target(s_nr.dev, s_nr.ovl_ss, 0.0f, 0.0f, 0.0f, 0.0f);  /* transparent */
    draw_overlay_glyphs_3d(2.0f / ovl_region_w(), 2.0f / ovl_region_h(), 0.0f, 0.0f);
    rhi_end_render_target(s_nr.dev, s_nr.ovl_ss);
}

/** @brief Composite the stats overlay into the currently open offscreen pass.
 *
 * On the supersample path the ovl_ss target the pre-pass built is
 * box-downsampled and straight-alpha-blended through ovl_resolve_pipe.
 * Without it the glyphs draw directly at 1x, aliased, at the stats block's
 * placement offset (s_perf_ox, s_perf_oy). */
static void draw_perf_overlay(void) {
    if (!perf_overlay_should_draw()) return;
    if (ovl_ss_ready()) {
        rhi_reset_scissor(s_nr.dev);
        bind_scene_pipe(s_nr.ovl_resolve_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.ovl_ss);
        push_overlay_resolve(OVL_SS_FACTOR, OVL_OUTLINE_PX);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.ovl_quad_vb);
        rhi_draw(s_nr.dev, 6, 0);
    } else {
        draw_overlay_glyphs_3d(2.0f * (float)s_nr.win_h / (float)s_nr.win_w, 2.0f,
                               s_perf_ox, s_perf_oy);
    }
}

/** @brief Create the version line's GPU resources on the first present that draws the line.
 *
 * Creates the glyph buffer and the composite quad, then sizes the
 * supersample target to the published region when the overlay resolve
 * pipeline exists (without it the line draws at 1x and needs no target).
 * Returns early while the line's alpha is zero, so a run that never shows
 * the line allocates none of them and bring-up has no extra resources to
 * unwind; the target is sized only once a region has been published (a
 * region of zero extent leaves it unsized). It creates resources and can
 * wait the device idle, so it runs in the per-present resource pass, outside
 * a render pass. */
static void ensure_version_ss(void) {
    if (!s_nr.dev || s_ver_alpha8 <= 0 || s_nr.win_w <= 0 || s_nr.win_h <= 0) return;
    if (!s_nr.version_vb)
        s_nr.version_vb = rhi_create_vertex_buffer_dynamic(s_nr.dev,
                                                           sizeof(rhi_vertex) * VER_MAX_VERTS);
    if (!s_nr.ver_quad_vb)
        s_nr.ver_quad_vb = rhi_create_vertex_buffer_dynamic(s_nr.dev, sizeof(rhi_vertex) * 6);
    if (!s_nr.ovl_resolve_pipe) return;   /* 1x fallback; no target to size */
    float vw, vh;
    version_region(&vw, &vh);
    if (vw <= 0.0f || vh <= 0.0f) return;
    ensure_text_ss_target(&s_nr.ver_ss, &s_nr.ver_ss_w, &s_nr.ver_ss_h,
                          vw, vh, "version line");
}

/** @brief Return whether the version line's supersample resources all exist. */
static bool ver_ss_ready(void) {
    return s_nr.ovl_resolve_pipe && s_nr.ver_quad_vb && s_nr.ver_ss && s_nr.version_vb;
}

/** @brief Render the version-line glyphs into ver_ss at OVL_SS_FACTOR, cleared transparent.
 *
 * The pass is its own render-target pass, so it runs before the offscreen
 * pass opens, beside the stats block's. The glyphs go in opaque and
 * region-local; the fade is applied to the composite quad, so the
 * supersampled coverage is resolved once and then scaled as a whole. No-op
 * while the supersample resources are not up, the line's alpha is zero, or
 * no region has been published. */
static void prepare_version_ss(void) {
    if (!ver_ss_ready() || s_ver_alpha8 <= 0) return;
    float vw, vh;
    version_region(&vw, &vh);
    if (vw <= 0.0f || vh <= 0.0f) return;
    rhi_begin_render_target(s_nr.dev, s_nr.ver_ss, 0.0f, 0.0f, 0.0f, 0.0f);
    draw_glyph_list_3d(s_ver_glyph_verts, s_ver_glyph_n,
                       s_nr.version_vb, VER_MAX_VERTS,
                       2.0f / vw, 2.0f / vh, 0.0f, 0.0f, NULL, -1);
    rhi_end_render_target(s_nr.dev, s_nr.ver_ss);
}

/** @brief Composite the version line into the currently open offscreen pass.
 *
 * The line lands at the placement and alpha the layout pass latched:
 * box-downsample the
 * supersampled target and straight-alpha-blend it, the same route the stats
 * readout takes, with the fade carried by the quad's tint. Fallback when the
 * supersample resources are not up: the glyphs at 1x through the
 * alpha-blended untextured HUD pipe, where the fade is the vertex alpha.
 * s_ver_drawn counts each draw. */
static void draw_version_overlay(void) {
    if (s_ver_alpha8 <= 0) return;
    const float ax = 2.0f * (float)s_nr.win_h / (float)s_nr.win_w;
    if (!ver_ss_ready()) {
        rhi_pipeline *pipe = s_nr.gouraud_hud_pipes[NR_BLEND_AVG];
        if (!pipe || !s_nr.version_vb) return;
        if (draw_glyph_list_3d(s_ver_glyph_verts, s_ver_glyph_n,
                               s_nr.version_vb, VER_MAX_VERTS, ax, 2.0f,
                               s_ver_ox, s_ver_oy, pipe, s_ver_alpha8))
            s_ver_drawn++;
        return;
    }
    float vw, vh;
    version_region(&vw, &vh);
    if (vw <= 0.0f || vh <= 0.0f) return;
    const float rw_ndc = vw * ax, rh_ndc = vh * 2.0f;
    const float nx_left = s_ver_ox * ax - 1.0f, ny_top = 1.0f - s_ver_oy * 2.0f;
    rhi_vertex q[6];
    build_panel_tinted(q, nx_left + rw_ndc * 0.5f, ny_top - rh_ndc * 0.5f,
                       rw_ndc * 0.5f, rh_ndc * 0.5f, 0.0f, 0.0f, 1.0f, 1.0f,
                       0x00FFFFFFu | ((uint32_t)s_ver_alpha8 << 24));
    rhi_update_buffer(s_nr.ver_quad_vb, q, sizeof(q));
    rhi_reset_scissor(s_nr.dev);
    bind_scene_pipe(s_nr.ovl_resolve_pipe);
    rhi_bind_vram(s_nr.dev, s_nr.ver_ss);
    push_overlay_resolve(OVL_SS_FACTOR, OVL_OUTLINE_PX);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.ver_quad_vb);
    rhi_draw(s_nr.dev, 6, 0);
    s_ver_drawn++;   /* engagement counter */
}

/* --- in-game settings menu overlay -----------------------------------------
 * Drawn on the live vk present's resolved 1× offscreen, same overlay space and
 * gating as the FPS overlay. Reproduces the PSX pause-menu frame: a subtractive
 * dark panel (matching the game's ABR-2 box), an opaque gold border, and an
 * alpha row highlight on the cursor. Glyph text lands in a later step. One
 * upload to menu_vb, drawn as blend-mode spans (single upload -> all spans read
 * consistent data from the per-frame ring slot). */

/** @brief Append an overlay-space rectangle to a vertex buffer as two triangles in the offscreen's NDC.
 *
 * Overlay space has x in window-height units and y in [0,1], origin top
 * left; it maps to NDC as (x * ax - 1, 1 - y * ay), the map of the FPS
 * overlay's 1x path (ax = 2 * win_h / win_w, ay = 2). The vertices carry
 * view-Z 1 (the near guard) and W 1. Colour byte order is R, G, B, A.
 * @param buf Vertex buffer of MENU_MAX_VERTS entries.
 * @param n Vertices already in @p buf.
 * @param ax Overlay-to-NDC scale x.
 * @param ay Overlay-to-NDC scale y.
 * @param x0 Left edge.
 * @param y0 Top edge.
 * @param x1 Right edge.
 * @param y1 Bottom edge.
 * @param rgba Vertex colour.
 * @return The new vertex count, @p n unchanged when six more do not fit. */
static int menu_push_quad(rhi_vertex* buf, int n, float ax, float ay,
                          float x0, float y0, float x1, float y1, uint32_t rgba) {
    if (n + 6 > MENU_MAX_VERTS) return n;
    const float nx0 = x0 * ax - 1.0f, nx1 = x1 * ax - 1.0f;
    const float ny0 = 1.0f - y0 * ay, ny1 = 1.0f - y1 * ay;
    /* Screen-space quad: view-Z 1 (the near guard), W 1. */
    const rhi_vertex tl = { nx0, ny0, 1.0f, 0,0, rgba, 0, 0,0, 1.0f };
    const rhi_vertex tr = { nx1, ny0, 1.0f, 0,0, rgba, 0, 0,0, 1.0f };
    const rhi_vertex bl = { nx0, ny1, 1.0f, 0,0, rgba, 0, 0,0, 1.0f };
    const rhi_vertex br = { nx1, ny1, 1.0f, 0,0, rgba, 0, 0,0, 1.0f };
    buf[n++] = tl; buf[n++] = tr; buf[n++] = bl;
    buf[n++] = tr; buf[n++] = br; buf[n++] = bl;
    return n;
}

/** @brief Draw the settings menu's glyph and border layer into the current render pass.
 *
 * The vertices (s_menu_glyph_verts) map from overlay space to NDC as
 * (x * ax - 1, 1 - y * ay) and draw through the opaque gouraud pipe after a
 * local depth clear, so the 3D glyph faces Z-sort among themselves on top of
 * what is already drawn. The panel is flush at the overlay origin, so one
 * map serves the 1x offscreen (ax = 2 * win_h / win_w, ay = 2) and the
 * menu_ss supersample pre-pass (ax = 2 / panel_w, ay = 2 / panel_h, where
 * the panel fills the target).
 * @param ax Overlay-to-NDC scale x.
 * @param ay Overlay-to-NDC scale y. */
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
        gbuf[gn].w = v->z;
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

/** @brief Return whether the settings-menu glyph supersample path is usable.
 *
 * @return true when ovl_resolve_pipe, menu_quad_vb and menu_ss all exist. */
static bool menu_ss_ready(void) {
    return s_nr.ovl_resolve_pipe && s_nr.menu_quad_vb && s_nr.menu_ss;
}

/** @brief Create or resize menu_ss at MENU_SS_FACTOR times the settings-menu panel and rebuild its composite quad on a resize.
 *
 * The target is allocated only while the menu is open, since it is large,
 * and is kept after the menu closes. Each dimension is clamped to the
 * device's largest image. The composite quad is the panel rectangle, flush
 * top left, in offscreen NDC, with UV 0..1 over the whole target; it is
 * rebuilt when the window size changes. Creates GPU resources and waits
 * idle, so it runs outside a frame beside ensure_ovl_ss. No-op when the
 * supersample pipe or quad failed at init or the window size is unknown. */
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
    /* Composite quad - the panel rect (flush top-left) in offscreen NDC; UV 0..1
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

/** @brief Render the settings menu's glyph and border layer into menu_ss at MENU_SS_FACTOR, cleared transparent.
 *
 * The pass is its own render-target pass, so it runs before the offscreen
 * pass opens. The layer is mapped over the menu panel's rect. No-op unless
 * the supersample path is ready, the menu is open and at least one glyph
 * triangle is built. */
static void prepare_menu_ss(void) {
    if (!menu_ss_ready() || !openpete_menu_is_open() || s_menu_glyph_n < 3) return;
    openpete_menu_layout_t L; openpete_menu_get_layout(&L);
    const float pw = L.panel_x1 - L.panel_x0, ph = L.panel_y1 - L.panel_y0;
    rhi_begin_render_target(s_nr.dev, s_nr.menu_ss, 0.0f, 0.0f, 0.0f, 0.0f);  /* transparent */
    draw_menu_glyphs_3d(2.0f / pw, 2.0f / ph);
    rhi_end_render_target(s_nr.dev, s_nr.menu_ss);
}

/** @brief Draw the settings menu panel and its glyph layer over the composed image.
 *
 * Returns when the menu is closed, the menu buffer is missing or the window
 * has no extent. The panel is one subtractive gray quad (RGB 112,112,112)
 * matching the game's own message box. The glyph and border layer then
 * composites from the supersampled menu_ss layer that prepare_menu_ss built
 * when that path is ready, with a MENU_OUTLINE_PX dark outline around the
 * glyphs (the border's vertices carry OPENPETE_HUD_ALPHA_NO_OUTLINE and take
 * none), else draws directly as depth-isolated 3D glyphs without one. */
static void draw_menu_overlay(void) {
    if (!openpete_menu_is_open() || !s_nr.menu_vb || s_nr.win_w <= 0 || s_nr.win_h <= 0)
        return;
    openpete_menu_layout_t L;
    openpete_menu_get_layout(&L);
    const float ax = 2.0f * (float)s_nr.win_h / (float)s_nr.win_w;
    const float ay = 2.0f;

    static rhi_vertex buf[MENU_MAX_VERTS];
    int n = 0;

    /* Panel - subtractive gray RGB(112,112,112), the exact colour Spyro's own
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

    if (sub_n > 0 && s_nr.gouraud_hud_pipes[NR_BLEND_SUB]) {
        bind_scene_pipe(s_nr.gouraud_hud_pipes[NR_BLEND_SUB]);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.menu_vb);
        rhi_draw(s_nr.dev, (uint32_t)sub_n, (uint32_t)sub0);
    }

    /* Menu glyph text (row labels/values/title) + the lit gold frame, both pushed
     * game-side (text_moby_native_menu). SS path: composite the supersampled layer
     * that prepare_menu_ss built - box-downsample + straight-alpha blend over the
     * panel via the FPS overlay's resolve pipe (anti-aliased 3D text). 1× fallback:
     * draw the glyphs directly (depth-isolated), the original crisp-but-aliased
     * path. Either way the glyphs sit on top of the panel. */
    if (menu_ss_ready() && s_menu_glyph_n >= 3) {
        rhi_reset_scissor(s_nr.dev);
        bind_scene_pipe(s_nr.ovl_resolve_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.menu_ss);
        push_overlay_resolve(MENU_SS_FACTOR, MENU_OUTLINE_PX);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.menu_quad_vb);
        rhi_draw(s_nr.dev, 6, 0);
    } else {
        draw_menu_glyphs_3d(2.0f * (float)s_nr.win_h / (float)s_nr.win_w, 2.0f);
    }
}

/* --- OSD toast overlay ---------------------------------------------------
 * Same machinery as the settings menu - 3D gold glyphs and a lit gold frame
 * supersampled into an offscreen RGBA layer, then box-downsampled and
 * alpha-composited over the frame - but with N independently-faded boxes
 * instead of one panel. The differences are the fixed slot grid (so nothing
 * reallocates while toasts come and go) and the per-toast alpha, which rides
 * the composite quad's VERTEX COLOUR rather than a push constant: the resolve
 * shader already had the input declared and unused, and the quads are rebuilt
 * every present anyway, so per-toast fade costs literally nothing extra. */

/* Toasts composited, cumulative; logged at shutdown. */
static unsigned s_osd_composited = 0;
unsigned openpete_native_osd_composited(void) { return s_osd_composited; }

/** @brief Return whether every resource the OSD toast composite needs exists.
 *
 * Requires the overlay resolve pipe, the quad and panel vertex buffers and
 * the osd_ss slot grid.
 * @return true when all four are present. */
static bool osd_ss_ready(void) {
    return s_nr.ovl_resolve_pipe && s_nr.osd_quad_vb && s_nr.osd_panel_vb &&
           s_nr.osd_ss;
}

/** @brief Return whether OSD toasts draw on this present.
 *
 * Toasts draw when the OSD is enabled, except while --record is active: the
 * video serves as ground truth for mod-shader A/Bs and carries no chrome
 * unless fps_in_record_resolved allows it, the rule and override the stats
 * overlay uses.
 * @return true when toasts draw. */
static bool osd_should_draw(void) {
    if (!openpete_osd_enabled()) return false;
    if (openpete_record_active() && !fps_in_record_resolved()) return false;
    return true;
}

/** @brief Create or resize osd_ss, the OSD toast slot grid, on a window resize.
 *
 * The grid holds OPENPETE_OSD_MAX slots stacked vertically, each OSD_SLOT_W
 * by OSD_SLOT_H window-height units at OSD_SS_FACTOR. It is sized from the
 * maximum toast box, never from the live ones: sizing to content would
 * rebuild the target, and wait the device idle, on every arrival, expiry
 * and coalescing repaint, which is once per keypress while a value is held
 * down. Each dimension is clamped to the device's largest image, shrinking
 * the slot. The slot size in pixels is stored in osd_slot_px. The grid is
 * first allocated when a toast is live and is kept afterwards. No-op when
 * the OSD is disabled, the resolve pipe failed at init, or the window size
 * is unknown. Runs outside a frame. */
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

/** @brief Render every live toast's border and text into the osd_ss slot grid at OSD_SS_FACTOR.
 *
 * The pass is its own render-target pass, so it runs before the offscreen
 * pass opens. The glyphs arrive in target-local overlay space (slot i at
 * y = i * slot_h), so one linear map covers the whole grid. The published
 * glyph list is read under the OSD publish mutex for the whole draw. No-op
 * unless the grid is ready, toasts draw on this present, at least one glyph
 * triangle is published and the slot extent is known. */
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
     * and a draw call - command-buffer recording, microseconds - and the
     * publish it can block happens once per sim tick. Nothing else is taken
     * under it, so there is no lock order to get wrong. */
    SDL_LockMutex(osd_pub_mx());
    draw_glyph_list_3d(s_osd_glyph_verts, s_osd_glyph_n,
                       s_nr.osd_glyph_vb, OSD_GLYPH_MAX,
                       2.0f / sw, 2.0f / (sh * (float)OPENPETE_OSD_MAX), 0.0f, 0.0f,
                       NULL, -1);
    SDL_UnlockMutex(osd_pub_mx());
    rhi_end_render_target(s_nr.dev, s_nr.osd_ss);
}

/** @brief Composite the OSD toasts into the currently open offscreen pass.
 *
 * Per toast a 1x subtractive panel draws, then the slot's supersampled
 * sub-rect at the toast's alpha. Placement goes through the anchor ledger,
 * so the stats block, which claims first, is never overprinted. The toast
 * state machine (openpete_osd_present_update) advances on the present clock
 * before the draw gates, so toasts expire on schedule while a --record
 * capture or a missing supersample grid suppresses their pixels. */
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
     * Returning early when the toasts are merely not being DRAWN - during a
     * --record capture, or before the supersample grid comes up - would stop
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
         * glyph edges - the fade carries the easing instead. */
        int bw = (int)(sl->box_w * ph + 0.5f);
        int bh = (int)(sl->box_h * ph + 0.5f);
        if (bw < 1 || bh < 1) continue;
        if (bw > slot_px_w) bw = slot_px_w;
        if (bh > slot_px_h) bh = slot_px_h;
        const float w = (float)bw / ph, h = (float)bh / ph;

        /* Where it sits. x hugs the anchor's edge; y stacks from whatever the
         * persistent chrome already claimed at this anchor. The slide-in runs
         * along x for the side anchors and along y for the centred ones - a
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

        /* Snap the placed rect to the pixel grid too - the same reason. */
        x = (float)(int)(x * ph + (x >= 0.0f ? 0.5f : -0.5f)) / ph;
        y = (float)(int)(y * ph + 0.5f) / ph;

        int a8 = (int)(view[i].alpha * 255.0f + 0.5f);
        if (a8 <= 0) continue;
        if (a8 > 255) a8 = 255;

        /* Panel - the game's own dialog-box subtractive grey. The sub pipe
         * ignores alpha, so the fade scales the SUBTRACTED RGB toward black:
         * at alpha 0 it subtracts nothing, which is the same thing as being
         * absent. */
        const int g = (112 * a8) / 255;
        const uint32_t sub = 0xFF000000u | (uint32_t)g | ((uint32_t)g << 8) | ((uint32_t)g << 16);
        pn = menu_push_quad(panels, pn, ax, ay, x, y, x + w, y + h, sub);

        /* Composite quad - the slot's used sub-rect, tinted by the fade. */
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
    s_osd_composited += (unsigned)drawn;   /* engagement counter: a green must prove it engaged */

    if (pn > 0 && s_nr.gouraud_hud_pipes[NR_BLEND_SUB]) {
        rhi_update_buffer(s_nr.osd_panel_vb, panels, (size_t)pn * sizeof(rhi_vertex));
        rhi_reset_scissor(s_nr.dev);
        bind_scene_pipe(s_nr.gouraud_hud_pipes[NR_BLEND_SUB]);
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
    rhi_reset_scissor(s_nr.dev);
    bind_scene_pipe(s_nr.ovl_resolve_pipe);
    rhi_bind_vram(s_nr.dev, s_nr.osd_ss);
    push_overlay_resolve(OSD_SS_FACTOR, OSD_OUTLINE_PX);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.osd_quad_vb);
    for (int i = 0; i < cn; i++)
        rhi_draw(s_nr.dev, 6, (uint32_t)(i * 6));
}

/** @brief Copy the just-ended offscreen's content region into frozen_tex as the freeze backdrop.
 *
 * The offscreen is projected and pillarboxed for the window, and under
 * --ssaa already resolved to window resolution. The region blitted is the
 * p0 panel rect with its half-width widened by ws_factor, in window pixels;
 * the pause, inventory and exit freeze presents sample it. Only a world
 * present is captured, so the backdrop is the gameplay image and not the
 * menu drawn over it; nothing happens without frozen_tex or with an empty
 * window or region. Marks frozen_valid on a capture. Must be called after
 * rhi_end_offscreen (off_image in TRANSFER_SRC). OPENPETE_LOAD_CENSUS
 * brackets the blit with the characters s and t on stderr.
 * @param world_frame True when this present composed the world. */
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

/** @brief Snapshot the freeze backdrop from the offscreen image before any chrome is drawn into it.
 *
 * capture_freeze_from_offscreen reads off_image, where the stats block, the
 * version line, the settings menu and the OSD toasts are drawn; a snapshot
 * taken after them keeps that chrome in frozen_tex behind the pause screen
 * after the live chrome has changed or expired. This ends the offscreen
 * pass, takes the snapshot, and reopens the pass loading its contents, so
 * chrome draws into the reopened pass; scene_postfx_split has the same
 * shape. It costs one extra pass boundary on the world presents that feed
 * the freeze. Mod content is drawn before the split and is part of the
 * backdrop: a mod's HUD is content, not chrome.
 * @param world_frame True when this present composes a world scene.
 * @return true when it captured, only during the live present's compose
 *         with frozen_tex allocated; the caller's tail capture then stands
 *         down. */
static bool chrome_split_capture(bool world_frame)
{
    if (!(world_frame && s_nr.frozen_tex && s_overlay_live)) return false;
    rhi_end_offscreen(s_nr.dev);
    capture_freeze_from_offscreen(true);
    rhi_begin_offscreen_load(s_nr.dev);
    return true;
}

/** @brief Run the SCENE-point postfx chain between the composed scene and the HUD.
 *
 * Called inside the live offscreen pass, after the scene is composed into
 * off_image and before the HUD draws. When a SCENE-point pass is enabled it
 * ends the pass, runs the chain over the scene at the swapchain extent
 * (depth available at set 1), and reopens the pass loading the processed
 * scene, so the HUD draws unfiltered on top. With no SCENE pass enabled the
 * pass is not split and the image is unchanged. */
static void scene_postfx_split(void)
{
    if (!native_postfx_point_active(1 /* OP_POSTFX_SCENE */)) return;
    int w = 0, h = 0;
    rhi_swapchain_extent(s_nr.dev, &w, &h);
    rhi_end_offscreen(s_nr.dev);                 /* scene -> off_image (TRANSFER_SRC) */
    native_postfx_run(1 /* SCENE */, NULL, w, h);/* chain over off_image, blit back */
    rhi_begin_offscreen_load(s_nr.dev);          /* resume for HUD, scene preserved */
}

/* ---- per-present accounting -----------------------------------------------
 * Every present that reaches the display is classified twice: by the SCENE it
 * composed (the canonical tick scene, a sub-tick intermediate, or the inline
 * path's live accumulator) and by what the compose actually put on screen
 * (the native scene, the PSX VRAM framebuffer that the empty-scene fallback
 * re-displays, the PsyCross reference frame, or a bare clear). A whole-scene
 * flicker is a present stream alternating between two of those; the counters
 * name which pair, and the flip counts say how often the stream crossed.
 * The composed target extent is recorded per scene kind, so a present framed
 * differently from its neighbours (a stale window size, a different full
 * extent) appears as a second extent under one kind. Composes that began no
 * frame, and frames that composed but never presented, are counted too, so
 * every call into the present path is accounted.
 *
 * Always counted (a handful of increments per present) and reported at exit;
 * OPENPETE_PRESENT_TRACE=1 adds one stderr line per present. */
enum { PC_SCENE = 0, PC_VRAM, PC_REF, PC_CLEAR, PC_OUTCOMES };
enum { PC_CANON = 0, PC_INTER, PC_INLINE, PC_KINDS };
static const char *const s_pc_outcome_name[PC_OUTCOMES] = { "scene", "vram-fb", "psycross-ref", "clear" };
static const char *const s_pc_kind_name[PC_KINDS] = { "canonical", "intermediate", "inline" };
static struct {
    unsigned n[PC_KINDS][PC_OUTCOMES];
    int      ext_w[PC_KINDS], ext_h[PC_KINDS];   /* first extent seen for this kind */
    unsigned ext_alt[PC_KINDS];                  /* presents at a DIFFERENT extent */
    unsigned full_extent[PC_KINDS];
    /* Scene content per kind: the tri count composed, and the retained
     * terrain the cache had for that scene's stamp. A whole-scene flicker
     * whose presents all show "scene" is content that comes and goes
     * between kinds - a terrain build the cache no longer holds for an
     * intermediate's stamp leaves the world missing for that present. */
    long long sum_n[PC_KINDS], sum_tc[PC_KINDS];
    int      min_n[PC_KINDS], max_n[PC_KINDS], min_tc[PC_KINDS], max_tc[PC_KINDS];
    unsigned no_tc[PC_KINDS];      /* composed with no retained terrain at all */
    unsigned seen[PC_KINDS];       /* frames tallied for this kind (seeds the ranges) */
    unsigned outcome_flips, kind_flips;
    int      last_outcome, last_kind;
    unsigned begin_refused;        /* render_frame_common could not begin a frame */
    unsigned total;
} s_pc = { .last_outcome = -1, .last_kind = -1 };
/* What the compose put on screen, set by compose_variant, read once the
 * present is known to have gone out. -1 = no compose ran. */
static int s_pc_cur = -1;

/** @brief Return whether OPENPETE_PRESENT_TRACE requests the per-present census.
 *
 * The variable is read on the first call and the result kept.
 * @return 1 when the variable is set to a value other than empty or 0. */
static int pc_trace_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OPENPETE_PRESENT_TRACE"); on = (e && *e && *e != '0'); }
    return on;
}

/* Ring audit (OPENPETE_PRESENT_TRACE): canonical presents compared, those
 * that drew a path short, and per family (16 = retained terrain) how often. */
static struct { unsigned compared, shortfall, fam[17]; } s_ra;

/** @brief Compare a canonical present's per-family triangle counts and retained terrain with its scene's publish-time counts.
 *
 * Under OPENPETE_PRESENT_TRACE, for a canonical present of a ring slot: the
 * slot's triangles are counted per provenance family (tag bits 31:28), and
 * a family the published scene held with more triangles than the present
 * composes is short; the retained terrain is short when its build held
 * drawn sectors and the present draws fewer. Each short present prints one
 * `[ring-audit]` line; openpete_native_present_census_report prints the
 * totals.
 * @param kind PC_CANON, PC_INTER or PC_INLINE.
 * @param tc_drawn Retained sectors the present draws (frame_stats `drawn`). */
static void pc_ring_audit(int kind, int tc_drawn)
{
    if (kind != PC_CANON || !s_ps_slot || !pc_trace_on()) return;
    const scene_slot_t *sl = s_ps_slot;
    uint32_t n[16] = {0};
    for (int i = 0; i < sl->n; i++) n[sl->tri[i].tag >> 28]++;
    uint32_t mask = 0;
    for (int f = 0; f < 16; f++) mask |= (uint32_t)(n[f] < sl->fam_n[f]) << f;
    mask |= (uint32_t)(sl->tc_built > 0 && tc_drawn < sl->tc_built) << 16;
    s_ra.compared++;
    if (!mask) return;
    s_ra.shortfall++;
    fprintf(stderr, "[ring-audit] #%u stamp=%08x short:", s_pc.total, sl->stamp);
    for (int f = 0; f < 16; f++) {
        if (!(mask & (1u << f))) continue;
        s_ra.fam[f]++;
        fprintf(stderr, " fam%d=%u/%u", f, n[f], sl->fam_n[f]);
    }
    if (mask & (1u << 16)) { s_ra.fam[16]++; fprintf(stderr, " tercache=%d/%d", tc_drawn, sl->tc_built); }
    fputc('\n', stderr);
}

/** @brief Tally one composed present into the per-present census.
 *
 * Called by render_frame_common after the frame is submitted. It consumes
 * the outcome compose_variant recorded (no compose leaves nothing to tally)
 * and counts it under the scene kind (inline, canonical, intermediate), with
 * the full-extent count, the triangle and retained-terrain ranges, the
 * target extent and the outcome and kind flip counts; it also pushes one
 * record to the glitch-mark ring and, under OPENPETE_PRESENT_TRACE, prints
 * one trace line. A frame rendered into a stage image is
 * counted here, when composed; the swapchain can still decline its later
 * submit, which the present thread's own census counts. */
static void pc_note(void)
{
    const int outcome = s_pc_cur;
    s_pc_cur = -1;
    if (outcome < 0) return;
    const int kind = !s_ps_slot ? PC_INLINE
                   : s_ps_slot->kind == OP_PACE_CANONICAL ? PC_CANON : PC_INTER;
    s_pc.total++;
    s_pc.n[kind][outcome]++;
    if (s_render_full_extent) s_pc.full_extent[kind]++;
    const int ntri = s_ps_slot ? s_ps_slot->n : s_scene_n;
    int tc_drawn = 0, tc_hp = 0, tc_lp = 0, tc_nv = 0, tc_gen[8];
    openpete_tercache_frame_stats(&tc_drawn, &tc_hp, &tc_lp, &tc_nv, tc_gen);
    if (!s_pc.seen[kind]++) { s_pc.min_n[kind] = s_pc.max_n[kind] = ntri;
                             s_pc.min_tc[kind] = s_pc.max_tc[kind] = tc_drawn; }
    else {
        if (ntri < s_pc.min_n[kind]) s_pc.min_n[kind] = ntri;
        if (ntri > s_pc.max_n[kind]) s_pc.max_n[kind] = ntri;
        if (tc_drawn < s_pc.min_tc[kind]) s_pc.min_tc[kind] = tc_drawn;
        if (tc_drawn > s_pc.max_tc[kind]) s_pc.max_tc[kind] = tc_drawn;
    }
    s_pc.sum_n[kind] += ntri; s_pc.sum_tc[kind] += tc_drawn;
    if (!tc_drawn) s_pc.no_tc[kind]++;
    pc_ring_audit(kind, tc_drawn);
    if (!s_pc.ext_w[kind]) { s_pc.ext_w[kind] = s_nr.win_w; s_pc.ext_h[kind] = s_nr.win_h; }
    else if (s_pc.ext_w[kind] != s_nr.win_w || s_pc.ext_h[kind] != s_nr.win_h) s_pc.ext_alt[kind]++;
    if (s_pc.last_outcome >= 0 && s_pc.last_outcome != outcome) s_pc.outcome_flips++;
    if (s_pc.last_kind >= 0 && s_pc.last_kind != kind) s_pc.kind_flips++;
    s_pc.last_outcome = outcome; s_pc.last_kind = kind;
    {   /* The glitch-mark ring. Every field here is one the census above
         * already read, so the ring costs one struct store per present. */
        openpete_glitch_present_t g;
        memset(&g, 0, sizeof g);
        g.serial = s_pc.total;
        g.tick = s_ps_slot ? s_ps_slot->frame : s_scene_frame;
        g.stamp = scene_present_stamp();
        g.alpha = (float)(s_ps_slot ? s_ps_slot->alpha : 0.0);
        g.kind = kind;
        g.paired = s_ps_slot ? s_ps_slot->paired : 0;
        g.shows = outcome;
        g.grid_age = openpete_native_occl_grid_age();
        g.tris = ntri;
        g.tercache = tc_drawn;
        const openpete_occl_camera *gc = s_ps_slot ? &s_ps_slot->occl_cam : &s_occl_cam_live;
        g.cam_valid = gc->valid;
        if (gc->valid) {
            memcpy(g.cam_pos, gc->pos, sizeof g.cam_pos);
            memcpy(g.cam_M, gc->M, sizeof g.cam_M);
        }
        if (s_ps_slot) g.walk = s_ps_slot->gwalk;
        else           openpete_glitch_mark_last_walk(&g.walk);
        openpete_glitch_mark_note_present(&g);
    }
    if (pc_trace_on())
        fprintf(stderr, "[present] #%u kind=%s shows=%s frame=%u stamp=%08x n=%d tc=%d alpha=%.4f paired=%d %dx%d fe=%d\n",
                s_pc.total, s_pc_kind_name[kind], s_pc_outcome_name[outcome],
                s_ps_slot ? s_ps_slot->frame : s_scene_frame, scene_present_stamp(), ntri, tc_drawn,
                s_ps_slot ? s_ps_slot->alpha : 0.0, s_ps_slot ? s_ps_slot->paired : 0,
                s_nr.win_w, s_nr.win_h, s_render_full_extent);
}

void openpete_native_present_census_report(void)
{
    nr_log_window_state(s_nr.window, "exit");
    if (!s_pc.total && !s_pc.begin_refused) return;
    for (int k = 0; k < PC_KINDS; k++) {
        unsigned tot = 0;
        for (int o = 0; o < PC_OUTCOMES; o++) tot += s_pc.n[k][o];
        if (!tot) continue;
        OPL_INFO_F("present-census",
                   "%s: %u frame(s): %u scene, %u vram-fb, %u psycross-ref, %u clear; "
                   "%dx%d (%u at another extent), %u at full extent; "
                   "tris %d..%d (mean %lld), retained sectors %d..%d (mean %lld), %u with none",
                   s_pc_kind_name[k], tot, s_pc.n[k][PC_SCENE], s_pc.n[k][PC_VRAM],
                   s_pc.n[k][PC_REF], s_pc.n[k][PC_CLEAR],
                   s_pc.ext_w[k], s_pc.ext_h[k], s_pc.ext_alt[k], s_pc.full_extent[k],
                   s_pc.min_n[k], s_pc.max_n[k], s_pc.sum_n[k] / tot,
                   s_pc.min_tc[k], s_pc.max_tc[k], s_pc.sum_tc[k] / tot, s_pc.no_tc[k]);
    }
    OPL_INFO_F("present-census",
               "%u composed frame(s) total, %u showed something other than their predecessor, "
               "%u changed scene kind; %u frame(s) never began",
               s_pc.total, s_pc.outcome_flips, s_pc.kind_flips, s_pc.begin_refused);
    if (s_ra.compared) {
        char per[256] = "";
        int at = 0;
        for (int f = 0; f < 16 && at < (int)sizeof per - 24; f++)
            if (s_ra.fam[f]) at += snprintf(per + at, sizeof per - (size_t)at, " fam%d=%u", f, s_ra.fam[f]);
        if (s_ra.fam[16]) snprintf(per + at, sizeof per - (size_t)at, " tercache=%u", s_ra.fam[16]);
        OPL_INFO_F("ring-audit", "%u canonical present(s) compared, %u drew a path short of its scene%s",
                   s_ra.compared, s_ra.shortfall, per);
    }
}

/** @brief Compose one present's display image into the window-sized offscreen pass.
 *
 * With no live native scene (PsyCross display mode) the reference texture is
 * blitted through panel 1, or the clear stands when it does not exist yet.
 * Otherwise the native scene is drawn, after the key pass, either through
 * the supersampled target and its box resolve (--ssaa above 1) or directly;
 * an empty scene falls back to the PSX VRAM framebuffer. Every path then
 * applies the SCENE postfx point, the mod HUD and, when s_overlay_live is
 * set, the stats, version, toast and menu overlays, with the dev overlay
 * unless a COMPOSITE chain is live. The freeze backdrop is captured from the
 * finished offscreen unless the chrome split already took it, and s_pc_cur
 * records what the image shows for the present census. */
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
        scene_postfx_split();                     /* SCENE point */
        draw_mod_overlay();                       /* mod HUD (content) */
        if (s_overlay_live) {
            draw_perf_overlay();
            draw_version_overlay();
            draw_osd_overlay();
            draw_menu_overlay();
        }
        if (s_overlay_live && !native_postfx_point_active(0))
            openpete_devui_render(s_nr.dev);
        rhi_end_offscreen(s_nr.dev);
        s_pc_cur = s_nr.ref_tex && s_nr.layout_ref_w > 0 ? PC_REF : PC_CLEAR;
        return;
    }

    int sn; uint32_t sbg, sfade, sfreeze;
    const scene_tri_t* scene = scene_present_buf(&sn, &sbg, &sfade, &sfreeze, NULL, NULL);
    uint32_t bg = sbg;   /* gamestate clear (== the PSX draw-env clear) */
    bool want_scene = scene_has_game_tris(scene, sn) || (s_isolation && bg != 0);

    /* native_rtt is the OFF-DISPLAY reference-resolution render that the
     * freeze-backdrop snapshot copies. The LIVE panel does NOT blit from it - it
     * draws the scene DIRECTLY into the window-sized offscreen below, so the
     * native display keeps its FULL window resolution (rendering into the
     * ref-sized native_rtt and upscaling would soften textures and drop fine UI).
     * So native_rtt is rendered only when actually consumed: on WORLD frames, to
     * refresh the freeze snapshot (skip menu/freeze frames so the snapshot is the
     * gameplay frame to freeze, never the menu drawn over it). */
    /* scene_is_freeze_source reads the selected retained state, so it runs here,
     * on the presenting thread, after frame_select. */
    bool world_frame = scene_is_freeze_source(scene, sn, sfreeze);
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
     * offscreen - a true box filter at every factor and gamma-correct, unlike the
     * present blit's single bilinear tap. */
    if (s_nr.ss_rtt && s_nr.ss_factor > 1 && s_nr.layout_ref_w > 0) {
        /* NB the black clear is deliberate: clearing to the scene bg colour
         * and skipping the bg fill quad measured as a WASH on NVIDIA - an
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
        s_pc_cur = drew ? PC_SCENE : PC_VRAM;
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
            /* Empty scene -> PSX VRAM framebuffer fallback (1× direct, no SSAA). */
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
        scene_postfx_split();                     /* SCENE point: scene->chain->HUD */
        draw_mod_overlay();                       /* mod HUD (content - records too) */
        /* Freeze snapshot first, chrome after - see chrome_split_capture. */
        const bool froze = chrome_split_capture(world_frame);
        if (s_overlay_live) {
            draw_perf_overlay();                  /* on the resolved 1× offscreen, crisp */
            draw_version_overlay();               /* build-version line           */
            draw_osd_overlay();                   /* toasts above the readout    */
            draw_menu_overlay();                  /* settings menu on top        */
        }
        /* Dev overlay: under a live COMPOSITE chain, drawn post-chain
         * instead so screen filters never render over the panel. */
        if (s_overlay_live && !native_postfx_point_active(0))
            openpete_devui_render(s_nr.dev);      /* dev overlay topmost */
        rhi_end_offscreen(s_nr.dev);
        /* The freeze snapshot lives in the fall-through path below, which this SSAA
         * branch skips - so capture it here too, from the box-resolved 1× offscreen
         * (window resolution, matching the non-SSAA snapshot). Without this the
         * pause/inventory freeze had no captured frame under --ssaa and showed the
         * clear/cyclorama colour instead of the frozen world. Skipped when the
         * chrome split already took it. */
        if (!froze) capture_freeze_from_offscreen(world_frame);
        return;
    }

    /* vk / ab (and the diff fallback on an empty scene): draw the scene DIRECTLY
     * into the window-sized offscreen at full resolution (the original display path,
     * unchanged by the freeze work). The freeze overlay - drawn inside
     * draw_native_scene sampling frozen_tex - composes here too on menu frames. An
     * empty scene falls back to re-displaying the PSX VRAM framebuffer (loading
     * screens / transitions / pre-scene bootstrap). */
    native_terrain_key_pass(0, s_nr.win_w, s_nr.win_h);
    rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);
    s_pc_cur = PC_CLEAR;                 /* no layout yet: the clear is the picture */
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
        s_pc_cur = scene_drew ? PC_SCENE : PC_VRAM;
        if (!scene_drew) {
            const psx_material nd_mat = { tx, ty, 0, 0, 16 };
            bind_scene_pipe(s_nr.vram_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            rhi_push_constants(s_nr.dev, &nd_mat, sizeof(nd_mat));
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
            rhi_draw(s_nr.dev, 6, 0);
        }

    }

    /* SCENE point then mod HUD - vk variant, live + record composes. */
    scene_postfx_split();
    draw_mod_overlay();
    bool froze = chrome_split_capture(world_frame);   /* backdrop before chrome */
    /* Chrome - the live present only, never screenshots or dumps, where
     * s_overlay_live is 0. */
    if (s_overlay_live) {
        draw_perf_overlay();
        draw_version_overlay();
        draw_osd_overlay();
        draw_menu_overlay();
    }
    /* Dev overlay: when a COMPOSITE postfx chain is live it would filter
     * the panel into illegibility (CRT masks) - skip here and re-draw on
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
 * present - acceptable for a diagnostic. */
#define PT_WINDOW 4096
static uint32_t s_pt_cur[OPPT_COUNT];
static uint32_t s_pt_ring[OPPT_COUNT][PT_WINDOW];
static uint32_t s_pt_cnt_ring[OPPC_COUNT][PT_WINDOW];
static int      s_pt_cnt_cur[OPPC_COUNT];
static int      s_pt_n = 0;
static uint64_t s_pt_last_report = 0;

/** @brief Return 1 when OPENPETE_PHASE_TIMERS requests the once-per-second `[phase ...]` stderr line.
 *
 * Separate from openpete_phase_timers_on() so a --profile-out run measures
 * the phases and forwards them to the trace without printing the line. The
 * variable is re-read when the knob registry's env epoch changes (a live
 * edit from the dev overlay), never per call, since every phase-scope site on
 * the extracting and presenting threads calls this. */
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
    /* True when EITHER the env report is requested OR a perf trace is open -
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

/** @brief qsort comparator ordering two uint32_t values ascending.
 * @param a Pointer to the first value.
 * @param b Pointer to the second value.
 * @return Negative, zero or positive as *a is below, equal to or above *b. */
static int pt_cmp_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

/** @brief Roll the current present's phase totals and counts into the ring and print the phase line about once per second.
 *
 * No-op unless openpete_phase_timers_on(). The ring flushes when it holds
 * PT_WINDOW presents or one second has passed since the last flush; the
 * flush prints p50/p99/max microseconds per phase and avg/max per count only
 * when pt_report_on(), and resets the ring either way. */
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

/* ── Mod postfx executor ─────────────────────────────────────────────
 * Runs the enabled COMPOSITE-point chain over the composed frame:
 * input -> ping/pong RTs -> 1:1 blit back into the compose target, so
 * every consumer (present blit, --record, live screenshot, AND the
 * screenshot-all/headless native_rtt dump) sees the post-processed
 * frame - the native_rtt compose bypasses off_image, so the chain is
 * wired into both paths explicitly. Engine UBO rides block 0, pass i's
 * mod block 1+i. */
#define PFX_MAX_PASSES 15
typedef struct {   /* C mirror of op_engine_ubo - std140, offsets load-bearing */
    float    time, alpha;
    uint32_t tick, present, canonical;
    float    aspect;
    float    res[2], win[2];
    float    pad_rect[2];               /* std140: vec4 aligns to 16 */
    float    scene_rect[4], scene_grid[4];
} pfx_engine_ubo_t;

/** @brief Fill the engine uniform block (op_engine, set 3 binding 0) for one pass.
 *
 * One fill serves every consumer (the postfx chain, mod meshes, the
 * material UBO), so a field added here reaches all of them. The block
 * carries the monotonic phase-clock time in seconds, the VBL stamp of the
 * composed scene (not a per-present count: sub-tick presents of one scene
 * share it), the tick, sub-tick alpha and
 * canonical flag of the present slot (or of the mod present context when no
 * slot is active), the pass and window resolutions and aspect, and
 * scene_rect and scene_grid: where the canonical PSX screen maps in the
 * pass image, mirroring scene_to_clip, and its display width, display
 * height, visible rows and widescreen factor.
 * @param eng Block to fill; zeroed first.
 * @param w Pass width in pixels; 0 uses the swapchain width.
 * @param h Pass height in pixels; 0 uses the swapchain height. */
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
/** @brief Write the engine uniform block into block blk of a UBO.
 *
 * Fills a pfx_engine_ubo_t with fill_engine_block and writes it whole.
 * @param u Destination UBO.
 * @param blk Block index.
 * @param w Pass width, 0 for the swapchain width.
 * @param h Pass height, 0 for the swapchain height. */
static void write_engine_block(rhi_ubo* u, int blk, int w, int h)
{
    pfx_engine_ubo_t eng;
    fill_engine_block(&eng, w, h);
    rhi_ubo_write(u, blk, &eng, sizeof eng);
}

#define MAT_UBO_BLOCKS 129   /* 1 engine + 128 slots (MOD_MATERIAL_SLOTS) */
/** @brief Refill the material UBO: the engine block and each interned slot's uniform bytes.
 *
 * Block 0 takes the engine block and block 1 + s the uniform bytes of
 * interned material slot s, read from the present slot's material view or,
 * with no slot active, from the mod registry. The UBO is created on first
 * use. Returns at once unless a material selector or dev split is armed
 * (tag_split_on). Slots past the block count are ignored. With
 * OPENPETE_MATERIAL_STATS set it prints each new peak of interned slots. */
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
     * them - and the honest budget signal (each slot is an extra draw).
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
 * reference for screenshot dumps) - LRU-evict beyond that (rare; under
 * wait-idle). */
static struct { int w, h; rhi_texture* t[2]; uint32_t used; } s_pfx_pool[2];
static uint32_t s_pfx_stamp;
/* Previous present's final composite (set 2, temporal effects) -
 * snapshotted from off_image AFTER the live chain each present, so
 * during pass N of present P it holds present P-1's output. Cleared
 * at creation (first present would otherwise sample garbage). */
static rhi_texture* s_pfx_prev;
static int s_pfx_prev_w, s_pfx_prev_h;

/** @brief Return the pair of ping-pong render targets of one size, creating it when absent.
 *
 * The pool holds two sizes, in practice the window and the reference
 * dump; a third size evicts the least recently used pair after a device
 * wait-idle. A partly created pair is destroyed.
 * @param w Target width in pixels.
 * @param h Target height in pixels.
 * @return The two targets, owned by the pool, or NULL when one cannot be created. */
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

/** @brief Destroy the postfx and material pipelines the mod registry retired.
 *
 * Pipelines are retired when a mod is unloaded and at shutdown. Takes up to
 * PFX_MAX_PASSES per call; later presents drain the rest. Waits the device
 * idle before destroying, only when the list is non-empty. Runs on every
 * present before the COMPOSITE-point chain. */
static void pfx_drain_retired(void) {
    void* dead[PFX_MAX_PASSES];
    int n = openpete_mods_postfx_take_retired(dead, PFX_MAX_PASSES);
    if (n <= 0) return;
    rhi_wait_idle(s_nr.dev);
    for (int i = 0; i < n; i++)
        rhi_destroy_pipeline(s_nr.dev, (rhi_pipeline*)dead[i]);
}

/* Run the chain. target == NULL -> over off_image (live present);
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

/** @brief Write the engine's raster-quantize pass into a postfx chain.
 *
 * Emits nothing unless @p point is the SCENE point and --raster-quantize is
 * above 0. The pipeline is built on first use; a failed build is logged
 * and stored as the failure marker, which makes the executor skip the
 * pass. The uniforms carry the multiplier and the affine that maps the
 * PSX screen into the opaque depth pre-pass allocation, with the valid
 * flag clear when that pre-pass did not render this present.
 * @param point Chain point; 1 is the SCENE point.
 * @param pass Receives the pass at index 0.
 * @return Number of passes written, 0 or 1. */
static int raster_quantize_prepend(int point, openpete_postfx_pass_t* pass)
{
    if (point != 1 /* OP_POSTFX_SCENE */ || s_raster_mult <= 0) return 0;
    rhi_pipe_site_visit(RHI_PIPE_SITE_BUILTIN_LAZY, 1);
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
                            "- quantize disabled\n");
    }
    s_rq_params.scale = (float)s_raster_mult;
    /* Depth-majority vote input: the WORLD-opaque pre-pass depth, addressed
     * through its PSX-screen->uv affine scaled from the raster extent to the
     * grow-only allocation (the shadow-decal consumers' mapping - plain
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

/* Engine-owned SCENE pass (--dither): PS1 15-bit colour - the 4x4 hardware
 * dither matrix on the PSX framebuffer lattice + RGB555 truncation
 * (psx_dither.frag). Runs AFTER the raster quantize (the console dithered
 * each framebuffer write, i.e. downstream of rasterisation) and BEFORE any
 * mod chain, so a CRT filter processes the dithered image - the authentic
 * dither -> composite -> CRT order that made the pattern invisible on
 * period hardware. Needs no custom uniforms: the grid mapping rides the
 * shared engine UBO (op_scene_rect/op_scene_grid). */
static void* s_dither_pipe;                    /* rhi_pipeline* / PFX_PIPE_FAILED */

/** @brief Write the engine's PS1 dither pass into a postfx chain.
 *
 * Emits nothing unless @p point is the SCENE point and --dither is on. The
 * pipeline is built on first use; a failed build is logged and stored as
 * the failure marker, which makes the executor skip the pass. The pass has
 * no custom uniforms.
 * @param point Chain point; 1 is the SCENE point.
 * @param pass Receives the pass at index 0.
 * @return Number of passes written, 0 or 1. */
static int psx_dither_append(int point, openpete_postfx_pass_t* pass)
{
    if (point != 1 /* OP_POSTFX_SCENE */ || !s_dither) return 0;
    rhi_pipe_site_visit(RHI_PIPE_SITE_BUILTIN_LAZY, 1);
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
                            "- dither disabled\n");
    }
    memset(&pass[0], 0, sizeof pass[0]);
    pass[0].pipeline_slot = &s_dither_pipe;
    pass[0].owner         = "psx-dither";
    return 1;
}

/** @brief Return whether any postfx pass would run at a chain point.
 *
 * The SCENE point (1) is active when the raster-quantize multiplier or the
 * dither is on; every point is active when the mods enable a pass there.
 * @param point Chain point; 1 is the SCENE point, 0 the COMPOSITE point.
 * @return 1 when a pass would run, else 0. */
static int native_postfx_point_active(int point)
{
    openpete_postfx_pass_t pass[PFX_MAX_PASSES];
    if (point == 1 /* OP_POSTFX_SCENE */ && (s_raster_mult > 0 || s_dither)) return 1;
    return openpete_mods_postfx_collect(point, pass, PFX_MAX_PASSES) > 0;
}

/** @brief Run the postfx chain of one point over a composed image and write the result back.
 *
 * The chain is the raster-quantize pass and the PSX dither pass when they
 * apply to @p point, followed by every enabled mod pass collected for it.
 * Pass pipelines are built on first use; a pass whose build failed is
 * skipped. Passes ping-pong between two render targets sized @p w x @p h;
 * each pass samples the previous pass (set 0), the opaque pre-pass depth or
 * the input as a fallback (set 1), and the previous present's composite on
 * the live path or the input on a dump (set 2). The engine block goes to UBO
 * block 0 and pass i's uniforms to block 1+i.
 *
 * Live path (@p target NULL): the input is the offscreen image, each pass is
 * a GPU timer zone named by its owner, the result is blitted back into the
 * offscreen image, and the post-chain composite is kept as the next
 * present's set-2 input. Dump path: the input and destination are
 * @p target, and the engine block carries alpha 0 and canonical 1.
 * @param point  0 for the COMPOSITE point, 1 for the SCENE point.
 * @param target Render target to process in place, or NULL for the live
 *               offscreen image.
 * @param w      Width of the processed image in pixels.
 * @param h      Height of the processed image in pixels.
 * @return Number of passes that ran; 0 leaves the image unchanged. */
static int native_postfx_run(int point, rhi_texture* target, int w, int h)
{
    openpete_postfx_pass_t pass[PFX_MAX_PASSES];
    int n = raster_quantize_prepend(point, pass);
    n += psx_dither_append(point, pass + n);
    const int n_mod = openpete_mods_postfx_collect(point, pass + n, PFX_MAX_PASSES - n);
    n += n_mod;
    if (n <= 0 || w <= 0 || h <= 0) return 0;
    rhi_pipe_site_visit(RHI_PIPE_SITE_MOD_POSTFX, n_mod);
    if (!s_pfx_ubo)
        s_pfx_ubo = rhi_create_ubo(s_nr.dev, OPENPETE_POSTFX_UNIFORMS_MAX,
                                   1 + PFX_MAX_PASSES);
    rhi_texture** ping = pfx_pings(w, h);
    if (!s_pfx_ubo || !ping) return 0;

    /* Same tick/alpha source as the present-hook ctx (mod_draw.c) -
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
                                "%s - pass disabled\n", pass[i].owner);
        }
        if (*pass[i].pipeline_slot == PFX_PIPE_FAILED) continue;
        rhi_texture* out = ping[out_idx];
        /* Per-mod GPU attribution: one zone per pass, named by the
         * owning mod id (loader-stable string). Live path only - the dump
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
         * again - snapshot the post-chain composite. */
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
 * lavapipe numbers measure the CPU rasterizer, not a GPU - real numbers come
 * from the Windows build. */
#define GPUZ_MAX 40   /* pass skeleton + nested sc.* (verbose) + per-mod postfx */
static struct { const char *name; double ema; } s_gpuz[GPUZ_MAX];
static int      s_gpuz_n = 0;
static uint64_t s_gpuz_last = 0;

static struct { const char *name; int id; } s_gpuz_pid[GPUZ_MAX];
static int s_gpuz_pid_n = 0;

/** @brief Return the interned perf-trace id of a GPU zone name, cached by name.
 *
 * An entry matches by name pointer (the zone brackets pass string literals)
 * with strcmp as the fallback. op_prof_intern walks the whole name table
 * with strcmp under the writer mutex and this runs once per zone per
 * present, so the cache makes the steady state a pointer compare. A name
 * past GPUZ_MAX cached entries is interned on every call.
 * @param name Zone name; the pointer is cached, so it must outlive the
 *        process (a string literal).
 * @return The op_prof_intern id. */
static int gpuz_prof_id(const char *name)
{
    const int n = s_gpuz_pid_n;
    for (int i = 0; i < n; i++) {
        if (!s_gpuz_pid[i].name) continue;
        if (s_gpuz_pid[i].name == name || strcmp(s_gpuz_pid[i].name, name) == 0)
            return s_gpuz_pid[i].id;
    }
    int id = op_prof_intern(name);
    if (n < GPUZ_MAX) {
        s_gpuz_pid[n].name = name;
        s_gpuz_pid[n].id   = id;
        s_gpuz_pid_n = n + 1;
    }
    return id;
}

/** @brief Fold the harvested GPU zone results into the perf trace and the once-per-second line.
 *
 * No effect unless GPU timers are on. Every harvested zone goes to the perf
 * trace, keyed by the RHI frame serial it was recorded under. Depth-0 zones
 * (all depths under OPENPETE_GPU_TIMERS_VERBOSE, re-read per call) update an
 * exponential moving average per zone name, weight 0.05, seeded by the
 * first sample, with at most GPUZ_MAX names. About once per second the
 * averages print as a `[gpu-pass]` line on stderr. */
static void gpu_zone_report(void)
{
    if (!rhi_gpu_timers_on(s_nr.dev)) return;
    rhi_gpu_zone_result res[RHI_GPU_ZONES_MAX];
    int n = rhi_gpu_zone_results(s_nr.dev, res, RHI_GPU_ZONES_MAX);

    /* Forward the harvested (frame-lagged) zones to the perf trace, keyed by the
     * RHI frame serial stored at that frame's begin. The serial identifies a
     * frame that actually rendered, which a present index does not: under eager
     * production scenes are published and superseded without ever rendering. */
    if (op_prof_on() && n > 0) {
        uint64_t serial = rhi_gpu_results_token(s_nr.dev);
        for (int i = 0; i < n; i++) {
            int zid = gpuz_prof_id(res[i].name);
            op_prof_gpu_zone(zid, res[i].depth, serial,
                             (uint32_t)(res[i].start_us + 0.5),
                             (uint32_t)(res[i].us + 0.5));
        }
    }

    /* The once-per-second stderr line folds DEPTH-0 zones only (the pass
     * skeleton + per-mod postfx attribution) - nested sub-zones (sc.*) would
     * double its length and their audience is the --profile-out trace, which
     * got every depth above. OPENPETE_GPU_TIMERS_VERBOSE=1 shows all depths. */
    /* Re-read per report (~1/s - cheap): setenv is the live channel. */
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
 * interpolated gamestate view. NULL -> no intermediates (present stays at sim
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
/** @brief Return the --record host buffer, grown to at least a size.
 *
 * The buffer only grows, and a growth failure leaves the old buffer in
 * place. s_reccap holds the capacity after the call.
 * @param need Required capacity in bytes.
 * @return The buffer, or NULL when need is 0 or the growth failed. */
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

/** @brief Queue this present for --record readback and hand a completed earlier one to the encoder.
 *
 * The readback rides the frame's own fence through a ring, so the GPU is
 * not stalled and the recording runs at real speed, unlike the screenshot
 * path's blocking rhi_read_offscreen; a readback queued a couple of
 * presents earlier that is ready is submitted top-down with its tag. Called
 * between the blit and end_frame, while the frame slot is still current.
 * No-op when the window-sized buffer cannot be allocated.
 * @param pts_us Presentation timestamp, microseconds, the readback's tag. */
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
static void pt_dump_maybe(int rendered, unsigned frame, uint32_t stamp, uint32_t freeze, int kind,
                          int64_t target_ns, double alpha, int paired, int ntri);
static int  s_last_frame_presented;
/** @brief The scene ring lock, created on first use without a creation race. */
static SDL_Mutex *ring_mx(void) {
    return openpete_mutex_once(&s_ring_mx);
}
/** @brief Load the display and clip rectangles of the scene being composed into s_nr.
 *
 * Takes the ring slot's snapshot when a slot is being composed, else the
 * live guest-side rectangles. */
static void ps_apply_geometry(void) {
    const int *g = s_ps_slot ? s_ps_slot->geom : s_geom_in;
    s_nr.disp_x = g[0]; s_nr.disp_y = g[1]; s_nr.disp_w = g[2]; s_nr.disp_h = g[3];
    s_nr.clip_x = g[4]; s_nr.clip_y = g[5]; s_nr.clip_w = g[6]; s_nr.clip_h = g[7];

}
/** @brief The draw-environment clear colour of the composed scene: the slot's copy, or the live value inline. */
static uint32_t ps_drawenv_clear(void)  { return s_ps_slot ? s_ps_slot->drawenv_clear : s_drawenv_clear; }
/** @brief The shadow decal parameters of the composed scene: the slot's snapshot, or the live block inline. */
static const openpete_shadow_decal_params *ps_shadow(void) { return s_ps_slot ? &s_ps_slot->shadow : &s_shadow_decal; }
/** @brief The player shadow fan footprint of the composed scene: the slot's snapshot, or the live block inline. */
static const openpete_shadow_footprint *ps_shadow_foot(void) { return s_ps_slot ? &s_ps_slot->shadow_foot : &s_shadow_foot; }
/** @brief Return the portal openings of the scene the present draws, chosen as scene_present_view chooses the scene.
 *
 * The openings travel with their scene: a compose of a kept or history
 * scene (a framebuffer-copy backdrop, a delayed present) reads the planes
 * that scene's terrain feed published, not the accumulator's, which a later
 * scene wipe has cleared.
 * @return The selected scene's cover block. */
static const nr_cover_block *ps_cover(void) {
    if (s_ps_slot) return &s_ps_slot->cover;
    if (s_fbsnap_game_sel) return &s_scene_game.cover;
    const int slot = scene_hist_slot_at(scene_delay_depth());
    if (slot >= 0) return &s_scene_hist_cover[slot];
    const scene_slot_t *ms = scene_moved_slot();
    return ms ? &ms->cover : &s_cover_live;
}
/** @brief The camera the composed scene was projected from: the slot's snapshot, or the live occlusion camera inline. */
static const openpete_occl_camera *ps_occl_cam(void) { return s_ps_slot ? &s_ps_slot->occl_cam : &s_occl_cam_live; }
/** @brief The blended-world order the composed scene recorded: the slot's copy, or the live record inline. */
static const bw_order_t *ps_bw(void) { return s_ps_slot ? &s_ps_slot->bw : &s_bw; }
/** @brief The freeze-backdrop production mode of the composed scene: the slot's copy, or the live mode inline. */
static int ps_freeze_retained(void) { return s_ps_slot ? s_ps_slot->freeze_retained : s_freeze_retained; }
/** @brief The VBL at which the composed scene was extracted: the slot's, or the live scene's inline. */
static unsigned ps_frame(void)          { return s_ps_slot ? s_ps_slot->frame : s_scene_frame; }

/* Stamp the terrain builder tags its build with: the scene id while an eager
 * scene is being extracted, else the VBL counter as before. Scene ids carry
 * the top bit so they never collide with a VBL number. */
uint32_t openpete_native_scene_build_stamp(void) {
    extern unsigned openpete_frame;
    return s_eager_stamp ? s_eager_stamp : (uint32_t)openpete_frame;
}
/** @brief Return the stamp the accumulator's retained terrain is kept under, for a present of the accumulator.
 *
 * The scene id of an eager scene the ring refused while the accumulator
 * still holds it (same epoch), else the VBL the scene was opened on.
 * @return The stamp. */
static unsigned scene_live_stamp(void) {
    return (s_scene_refused_stamp && s_scene_refused_epoch == s_scene_epoch)
           ? s_scene_refused_stamp : s_scene_frame;
}
/** @brief Return the stamp the terrain build of the scene accumulating in s_scene carries.
 *
 * The scene id while an eager scene is being extracted, else the VBL the
 * scene was opened on.
 * @return The build stamp. */
static uint32_t scene_accum_stamp(void) {
    return s_eager_stamp ? s_eager_stamp : (uint32_t)s_scene_frame;
}
void openpete_native_scene_eager_begin(void) {
    s_eager_stamp = OPTC_STAMP_SCENE_BIT | (s_scene_id_next & ~OPTC_STAMP_SCENE_BIT);
}
/** @brief Make a ring slot's triangle run hold n triangles and refresh the slot's pointer.
 *
 * Initialises the run on first use; publish calls it with 0 to guarantee an
 * initialised run before the storage swap.
 * @param sl The ring slot.
 * @param n Triangles wanted.
 * @return 1 when the run holds n triangles, else 0. */
static int slot_grow(scene_slot_t *sl, int n) {
    const int held = scene_run_fit(&sl->tri_v, "scene.ring", n);
    sl->tri = (scene_tri_t*)sl->tri_v.data;
    return held >= n;
}
/** @brief Keep a refused eager scene's build stamp on the accumulator and end the eager extraction.
 *
 * The refused scene stays in the accumulator while its terrain build sits
 * under the scene id, so the accumulator's present stamp is that id until
 * the next scene reset (a new epoch). */
static void scene_keep_refused_stamp(void) {
    s_scene_refused_stamp = s_eager_stamp;
    s_scene_refused_epoch = s_scene_epoch;
    s_eager_stamp = 0;
}
/** @brief Record a publishing slot's per-family triangle counts and its retained-terrain build size.
 *
 * Under OPENPETE_PRESENT_TRACE only: the per-present ring audit
 * (pc_ring_audit) compares the canonical present of the slot against these.
 * The counts are taken from the slot's own triangle run after the storage
 * exchange, keyed by provenance family (tag bits 31:28).
 * @param sl The slot being published, its run and stamp already set. */
static void slot_note_census(scene_slot_t *sl) {
    memset(sl->fam_n, 0, sizeof sl->fam_n);
    sl->tc_built = 0;
    if (!pc_trace_on()) return;
    for (int i = 0; i < sl->n; i++) sl->fam_n[sl->tri[i].tag >> 28]++;
    sl->tc_built = openpete_tercache_build_drawn(sl->stamp);
}
/* Publish into any free ring slot; see the header. */
int openpete_native_scene_publish(int64_t target_ns, int kind, double alpha, int paired) {
    SDL_LockMutex(ring_mx());
    int idx = -1;
    for (int i = 1; i <= SCENE_RING; i++) {
        const int cand = (s_ring_head + i) % SCENE_RING;
        if (s_ring[cand].state == SS_FREE) { idx = cand; break; }
    }
    if (idx < 0) {
        s_ring_census.refused_full++; SDL_UnlockMutex(ring_mx());
        scene_keep_refused_stamp();
        return -1;
    }
    scene_slot_t *sl = &s_ring[idx];
    SDL_UnlockMutex(ring_mx());   /* the slot is FREE: only this thread publishes */
    /* The finished scene moves into the slot by exchanging storage: the
     * accumulator takes the slot's former run and holds no scene until the
     * next one begins (every eager re-extract begins its scene). */
    if (!slot_grow(sl, 0)) { scene_keep_refused_stamp(); return -1; }
    scene_run_swap(&sl->tri_v, &s_scene_v);
    sl->tri = (scene_tri_t*)sl->tri_v.data;
    sl->n = s_scene_n; sl->bg = s_scene_bg; sl->fade = s_scene_fade; sl->freeze = s_scene_freeze;
    sl->replay = s_scene_replay;
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
    sl->shadow_foot = s_shadow_foot;
    sl->cover = s_cover_live;
    sl->occl_cam = s_occl_cam_live;
    sl->bw = s_bw;
    sl->freeze_retained = s_freeze_retained;
    slot_note_census(sl);
    /* The walk ran on this thread just now; the compose may run on another,
     * a tick later, so the census travels with the scene rather than being
     * read live at compose time. */
    openpete_glitch_mark_last_walk(&sl->gwalk);
    openpete_mods_present_ctx(&sl->pctx_tick, &sl->pctx_alpha, &sl->pctx_canonical);
    openpete_mods_material_view_capture(&sl->matview);
    sl->scene_id = s_scene_id_next++;
    sl->target_ns = target_ns; sl->alpha = alpha; sl->paired = paired; sl->kind = kind;
    sl->presented_serial = 0;
    s_eager_stamp = 0;
    s_scene_n = 0;          /* the accumulator's storage went to the slot */
    s_scene_moved = 1;
    s_scene_moved_slot = idx; s_scene_moved_id = sl->scene_id;
    SDL_LockMutex(ring_mx());
    sl->state = SS_PENDING;
    s_ring_head = idx;
    s_ring_census.published++;
    SDL_UnlockMutex(ring_mx());
    return idx;
}
/* Earliest pending target (0 = none pending). */
int64_t openpete_native_scene_next_target(void) {
    op_time_t t = 0;
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING; i++)
        if (s_ring[i].state == SS_PENDING && (t == 0 || s_ring[i].target_ns < t))
            t = s_ring[i].target_ns;
    SDL_UnlockMutex(ring_mx());
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
    SDL_UnlockMutex(ring_mx());
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
    SDL_UnlockMutex(ring_mx());
}
/* True while any unretired scene carries this build stamp. */
int openpete_native_scene_stamp_live(uint32_t stamp) {
    int live = 0;
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING && !live; i++)
        live = s_ring[i].state != SS_FREE && s_ring[i].stamp == stamp;
    SDL_UnlockMutex(ring_mx());
    return live;
}
/* Drop every pending scene (guest discontinuity: savestate, rewind, level
 * load). A scene mid-present finishes; its pixels are already committed. */
void openpete_native_scene_ring_flush(void) {
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING; i++)
        if (s_ring[i].state == SS_PENDING) { s_ring[i].state = SS_FREE; s_ring_census.superseded++; }
    SDL_UnlockMutex(ring_mx());
}
int openpete_native_scene_ring_pending(void) {
    int n = 0;
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING; i++) n += s_ring[i].state == SS_PENDING;
    SDL_UnlockMutex(ring_mx());
    return n;
}
int openpete_native_scene_ring_busy(void) {
    int n = 0;
    SDL_LockMutex(ring_mx());
    for (int i = 0; i < SCENE_RING; i++) n += s_ring[i].state != SS_FREE;
    SDL_UnlockMutex(ring_mx());
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

    /* Headless: no window / swapchain, so there is no live present. The
     * scene is composed on demand by openpete_native_render_screenshot_all. */
    if (s_nr.headless)
        return;

    /* Queued --texture-mapping change: re-bake the pipelines OUTSIDE the
     * frame, before any command buffer records against them. No-op normally. */
    texmap_apply_pending();
    notex_apply_pending();
    wireframe_apply_pending();
    truescope_apply_pending();


    /* Mirror VRAM + read the reference back BEFORE opening our frame: these do
     * their own one-shot GPU submits, kept clear of our render pass. Called
     * UNCONDITIONALLY: sync_reference_texture self-no-ops when PsyCross hasn't
     * produced a scene (pure vk - the OT isn't rasterized - so GR_GetPresentSceneSize
     * returns 0 -> early false), so it's free there; whenever PsyCross IS rendering
     * (screenshot) it must run, both to upload the reference AND because its
     * GL readback orders the GPU work the screenshot's rhi_read_offscreen then reads
     * (gating it off for vk blanked the captured native-intermediate frames). */
    sync_vram_texture();
    bool have_ref = sync_reference_texture();
    { const uint64_t b0 = rhi_frame_serial(s_nr.dev); render_frame_common(record_pts_us, have_ref); pt_dump_maybe(rhi_frame_serial(s_nr.dev) != b0, s_scene_frame, s_scene_frame, s_scene_freeze, -1, 0, 0.0, 0, s_scene_n); }
}

/* Compose and present one ring slot. The present thread's entry: no VRAM or
 * reference sync here (the guest side did the sync while the guest was idle,
 * and the only mode that displays the reference - psycross - never runs
 * eager, since it starts no present thread), the mod
 * registry is held for reading across the compose, and the slot moves to
 * PRESENTED with the serial of the frame that carries it - or back to FREE
 * when the swapchain declined, which is a refused present, not a shown one.
 * Returns 1 if a present went out. */
/* The frame in progress ends in stage image `s_stage_target` instead of a
 * present (rhi_end_frame_to_stage) when >= 0. */
static int s_stage_target = -1;

/** @brief Trace a rendered present and write its offscreen image under OPENPETE_PT_DUMP.
 *
 * OPENPETE_PT_DUMP=N (diagnostic) selects rendered scenes extracted at VBL N
 * or later and writes each straight from the offscreen target: the output
 * of either present path at one geometry, which no screenshot flag reaches.
 * Under eager production this runs on the present thread, inside the render
 * it dumps, so the eager present stream is captured without disengaging
 * eager. Each selected render prints one pt-dump trace line.
 *
 * OPENPETE_PT_DUMP_COUNT=N: how many renders to take (default 1).
 * OPENPETE_PT_DUMP_PATH: a directory, or a printf template taking the stamp.
 *   Unset, no image is written and only the trace line is printed. As a
 *   directory the name carries the sequence, the scene kind (canon, inter,
 *   inline), the VBL and the stamp, so the present order is the filename
 *   order. Alpha is forced opaque.
 * OPENPETE_PT_DUMP_FROZEN=1: also write frozen_tex beside each dump.
 * OPENPETE_PT_DUMP_MAXN=N: the ring render path passes only scenes with at
 *   most N triangles.
 * @param rendered Non-zero when the render produced a frame.
 * @param frame VBL the scene was extracted at.
 * @param stamp Scene stamp.
 * @param freeze Scene freeze word.
 * @param kind Scene kind (OP_PACE_CANONICAL, OP_PACE_INTERMEDIATE, else inline).
 * @param target_ns Present instant the alpha was computed for, nanoseconds.
 * @param alpha Sub-tick alpha of the scene.
 * @param paired The paired flag published with the scene.
 * @param ntri Triangle count. */
static void pt_dump_maybe(int rendered, unsigned frame, uint32_t stamp, uint32_t freeze, int kind,
                          int64_t target_ns, double alpha, int paired, int ntri)
{
    static int s_dump_at = -2;
    if (s_dump_at == -2) { const char *e = getenv("OPENPETE_PT_DUMP"); s_dump_at = (e && *e) ? atoi(e) : -1; }
    static int s_dump_left = -1;
    if (s_dump_left < 0) { const char *e = getenv("OPENPETE_PT_DUMP_COUNT"); s_dump_left = (e && *e) ? atoi(e) : 1; }
    static int s_dump_frozen = -1;
    if (s_dump_frozen < 0) { const char *e = getenv("OPENPETE_PT_DUMP_FROZEN"); s_dump_frozen = (e && *e && *e != '0'); }
    static unsigned s_dump_seq;
    if (s_dump_at >= 0 && rendered && (int)frame >= s_dump_at && s_dump_left > 0) {
        const char *tmpl = getenv("OPENPETE_PT_DUMP_PATH");
        const char *kname = kind == OP_PACE_CANONICAL ? "canon"
                          : kind == OP_PACE_INTERMEDIATE ? "inter" : "inline";
        char path[1024] = {0};
        if (tmpl && *tmpl) {
            if (strchr(tmpl, '%')) snprintf(path, sizeof path, tmpl, (unsigned)stamp);
            else snprintf(path, sizeof path, "%s/p%05u-%s-f%u-s%08x.png",
                          tmpl, s_dump_seq, kname, frame, (unsigned)stamp);
        }
        fprintf(stderr, "[pt-dump] seq=%u kind=%s stamp=%08x frame=%u freeze=%08x tri=%d target=%.3fms alpha=%.4f paired=%d%s%s\n",
                s_dump_seq, kname, stamp, frame, freeze, ntri,
                (double)target_ns / 1e6, alpha, paired, path[0] ? " -> " : "", path);
        if (path[0]) {
            size_t cap = (size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u;
            unsigned char *buf = cap ? malloc(cap) : NULL;
            int w = 0, h = 0;
            if (buf && rhi_read_offscreen(s_nr.dev, buf, cap, &w, &h)) {
                for (size_t i = 0; i < (size_t)w * (size_t)h; i++) buf[i * 4 + 3] = 0xFF;
                openpete_write_png_rgba(path, buf, w, h);
            }
            free(buf);
            if (s_dump_frozen && s_nr.frozen_tex) {
                int fw = 0, fh = 0; rhi_texture_extent(s_nr.frozen_tex, &fw, &fh);
                size_t fcap = (size_t)fw * (size_t)fh * 4u;
                unsigned char *fb = fcap ? malloc(fcap) : NULL;
                if (fb && rhi_read_render_target(s_nr.dev, s_nr.frozen_tex, fb, fcap, &fw, &fh)) {
                    for (size_t i = 0; i < (size_t)fw * (size_t)fh; i++) fb[i * 4 + 3] = 0xFF;
                    char fpath[1100]; snprintf(fpath, sizeof fpath, "%s.frozen.png", path);
                    openpete_write_png_rgba(fpath, fb, fw, fh);
                    fprintf(stderr, "[pt-dump] frozen_tex %dx%d valid=%d -> %s\n", fw, fh, (int)s_nr.frozen_valid, fpath);
                }
                free(fb);
            }
        }
        s_dump_seq++;
        if (--s_dump_left <= 0) s_dump_at = -1;
    }
}

/** @brief Compose one ring slot and present it directly or into a stage image.
 *
 * Declines without touching the slot when native rendering is off or
 * headless or @p slot is outside the ring. Otherwise the slot ends PRESENTED
 * when the render went out and FREE when it did not.
 * @param slot Ring slot; a value outside 0..SCENE_RING-1 is declined.
 * @param stage Stage image index, or -1 to present directly.
 * @param record_pts_us Capture instant for --record.
 * @return 1 when the render went out, 0 when it was declined. */
static int render_scene_impl(int slot, int stage, uint64_t record_pts_us)
{
    if (!s_nr.enabled || s_nr.headless || slot < 0 || slot >= SCENE_RING) return 0;
    scene_slot_t *sl = &s_ring[slot];
    rhi_set_present_blocking(s_nr.dev, true);
    texmap_apply_pending();
    notex_apply_pending();
    wireframe_apply_pending();
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
      if (sl->n <= maxn) pt_dump_maybe(after != before, sl->frame, sl->stamp, sl->freeze, sl->kind,
                                      sl->target_ns, sl->alpha, sl->paired, sl->n); }
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
    SDL_UnlockMutex(ring_mx());
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

/** @brief Record one present: apply the composed scene's geometry, take the glyph lists, adopt a settled window size, and run the passes and compose.
 *
 * @param record_pts_us Capture instant for --record.
 * @param have_ref Whether PsyCross produced a reference image this present; without one the estimated reference extent is used only for the native-only layout. */
static void render_frame_common(uint64_t record_pts_us, bool have_ref)
{
    s_last_frame_presented = 0;
    ps_apply_geometry();
    glyph_lists_latch();
    /* Detect window-size changes WITHOUT polling SDL events (the host loop
     * owns the event queue, and stealing events would starve PsyCross input). */
    int pw = 0, ph = 0;
    if (SDL_GetWindowSizeInPixels(s_nr.window, &pw, &ph) && pw > 0 && ph > 0 &&
        (pw != s_nr.win_w || ph != s_nr.win_h)) {
        /* Wait for the size to SETTLE before adopting it. A live resize walks
         * the width one pixel at a time, and adopting each step rebuilds the
         * SSAA scene target and the depth pre-pass every frame - dozens of
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


    /* Rolling 0.5s FPS window for the overlay. Counts every entry into this
     * function on whichever thread makes it (inline, or the present thread
     * under eager production), including a present begin_frame refuses below
     * and a render into a stage image. */
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
     * that returns nothing and ref_w/ref_h would stay 0 -> update_layout bails ->
     * compose draws nothing -> black. Derive it from GAMESTATE instead: the
     * bars-free PSX display is 4:3 corrected for the vertical overscan crop
     * (aspect = 4/3 * disp_h/clip_h), at a ~2x-internal reference so the
     * off-display RT stays crisp. Only the aspect matters to the layout (fit_cell
     * cancels the absolute scale); the size feeds native_rtt.
     *
     * Gated on the SCENE predicate, and it has to be. In PsyCross display mode
     * the reference IS the picture, and its extent comes from PsyCross's
     * present FBO, which is sized from the window and the game's overscan crop
     * (4/3 * disp_h/clip_h). The estimate below lands on the same numbers only
     * when clip_h is 224 - Spyro 1. For a 216-row clip it derives 640x432
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
    ensure_version_ss();       /* version-line buffers + SS target (lazy, once it is up) */
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
    if (!bf_ok) {
        s_pc.begin_refused++;
        return; /* swapchain recreating or window minimized - skip this present */
    }
    openpete_native_scene_retire(rhi_frame_serial(s_nr.dev));
    /* The mod registry is read for the rest of the frame's recording. Taken
     * AFTER begin_frame and released BEFORE end_frame: those two are where a
     * present blocks on the display, and a tick-boundary mutator on the
     * other thread must not wait behind a vblank. */
    openpete_mods_registry_read_lock();
    /* Retained terrain: select against the stamp of the scene this present
     * composes (freeze/lag included), so retained content follows the same
     * timeline and stale states never repaint. */
    /* Terrain-cache device service. This thread owns the device for the
     * whole present (the present thread while eager production is active,
     * the host loop otherwise), which is what makes buffer creation and
     * destruction legal here and illegal on the builder's thread. */
    openpete_tercache_present_service();
    openpete_tercache_frame_select(scene_present_stamp());
    /* Tag this frame's GPU work with its RHI frame serial, so the frame-lagged
     * harvest (next few frames) binds each zone set back to the frame that
     * submitted it. */
    if (op_prof_on()) rhi_gpu_frame_token(s_nr.dev, rhi_frame_serial(s_nr.dev));
    gpu_zone_report();  /* fold in the zone results begin_frame just harvested */
    /* Pending VRAM/reference uploads ride THIS frame's command buffer
     * - must precede the first render pass (the depth pre-pass below). */
    rhi_gpu_zone_begin(s_nr.dev, "upload");
    { OP_PROF_ACCUM_SCOPE("upl.synced");   upload_synced_textures(); }
    { OP_PROF_ACCUM_SCOPE("upl.hd");       hd_flush_pending(); }
    { OP_PROF_ACCUM_SCOPE("upl.hdfold");   openpete_native_hdfold_flush(); }
    rhi_gpu_zone_end(s_nr.dev);
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_GPU_SYNC, pt_t1 - pt_t0); pt_t0 = pt_t1; }
    if (s_ptrace) { fprintf(stderr, "[ptrace f=%u] B", s_scene_frame); fflush(stderr); }
    /* Shared WORLD-opaque depth pre-pass (its own render pass) - must record
     * before every consumer pass; one raster serves rtt/SSAA/offscreen alike. */
    rhi_gpu_zone_begin(s_nr.dev, "prepass");
    native_depth_prepass();
    rhi_gpu_zone_end(s_nr.dev);
    /* Moby occlusion grid: reduce + ring-readback (live presents only). */
    rhi_gpu_zone_begin(s_nr.dev, "occl");
    native_occl_downsample();
    rhi_gpu_zone_end(s_nr.dev);
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_PREPASS, pt_t1 - pt_t0); pt_t0 = pt_t1; }

    s_overlay_live = 1;                       /* this is the live present -> allow the stats overlay */
    /* Render the overlay glyphs at OVL_SS_FACTOR× into ovl_ss now - its own
     * render pass, so it must precede compose_variant's offscreen pass.
     * compose's draw_perf_overlay then composites the downsampled result. */
    rhi_gpu_zone_begin(s_nr.dev, "ovl_ss");
    prepare_perf_overlay_ss();
    rhi_gpu_zone_end(s_nr.dev);
    rhi_gpu_zone_begin(s_nr.dev, "menu_ss");
    prepare_menu_ss();         /* menu glyphs at MENU_SS_FACTOR× -> menu_ss (own pass) */
    rhi_gpu_zone_end(s_nr.dev);
    rhi_gpu_zone_begin(s_nr.dev, "osd_ss");
    prepare_osd_ss();          /* toast slot grid at OSD_SS_FACTOR× (own pass) */
    rhi_gpu_zone_end(s_nr.dev);
    rhi_gpu_zone_begin(s_nr.dev, "ver_ss");
    prepare_version_ss();      /* version line at OVL_SS_FACTOR× (own pass) */
    rhi_gpu_zone_end(s_nr.dev);
    rhi_gpu_zone_begin(s_nr.dev, "compose");
    compose_variant();
    rhi_gpu_zone_end(s_nr.dev);
    const int overlay_was_live = s_overlay_live;
    s_overlay_live = 0;                       /* screenshot/dump composes must not draw it */

    /* Mod postfx chain (COMPOSITE point). Runs on EVERY present (canonical +
     * intermediate). No outer GPU zone: the executor emits ONE ZONE PER
     * PASS NAMED BY THE OWNING MOD (zones don't nest), so a slow shader
     * shows up BY MOD ID in the [gpu-pass] report - the per-mod budget
     * attribution. */
    {
        pfx_drain_retired();
        int pw = 0, ph = 0;
        rhi_swapchain_extent(s_nr.dev, &pw, &ph);
        (void)native_postfx_run(0 /* COMPOSITE */, NULL, pw, ph);
        /* Dev overlay over the FILTERED composite: compose_variant skips
         * it whenever this chain is active (same predicate), so screen
         * filters (CRT masks) never render over the config panel - draw
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
     * slot is still current - the copy is appended to the frame's command
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
    pc_note();
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_GPU_SYNC, pt_t1 - pt_t0);
                 openpete_phase_set_count(OPPC_DRAWS, (int)rhi_draw_calls(s_nr.dev));
                 phase_present_done(); }
    if (s_ptrace) { fprintf(stderr, "E\n"); fflush(stderr); }

    /* Marker for the rendering thread: under eager production it never closes a
     * present, so this is where its accumulator buckets become one span per
     * rendered frame and its record chunk reaches the stream. */
    if (op_prof_on()) op_prof_thread_flush();
}

/** @brief Insert a suffix before a path's extension, or append it when there is none.
 *
 * Inserting -vk into shot-42.png gives shot-42-vk.png. The extension is the
 * text from the last dot, wherever that dot lies. The result is truncated
 * to the buffer.
 * @param base Source path.
 * @param suffix Text to insert.
 * @param out Output buffer.
 * @param cap Size of @p out in bytes. */
static void path_with_suffix(const char *base, const char *suffix,
                             char *out, size_t cap)
{
    const char *dot = strrchr(base, '.');
    if (dot) snprintf(out, cap, "%.*s%s%s", (int)(dot - base), base, suffix, dot);
    else     snprintf(out, cap, "%s%s", base, suffix);
}

/** @brief Write the terrain key image the capture's key pass left beside a screenshot, under OPENPETE_TERRAIN_KEY_READBACK.
 *
 * Two files are written next to @p base with -key inserted before the
 * extension, which is replaced. The .f32 file holds the depth value per
 * texel, rows top-down; a key's view-Z is 4096 z / (1 - z) and 1.0 means no
 * terrain key. The .txt file holds the image and viewport extents, the
 * pass's screen-to-clip mapping recorded by terrain_key_pass_record, and
 * openpete_frame, so a PSX screen point maps to its texel. No-op for an
 * in-memory capture, with the readback off, without the image, or before
 * any key pass ran.
 * @param base Screenshot path the file names derive from.
 * @param in_memory Nonzero for an in-memory capture. */
static void terrain_key_dump(const char *base, int in_memory)
{
    extern unsigned openpete_frame;
    if (in_memory || !terrain_key_readback_on() || !s_nr.terrain_key_depth ||
        s_tk_last.m < 0)
        return;
    int w = 0, h = 0;
    rhi_texture_extent(s_nr.terrain_key_depth, &w, &h);
    if (w <= 0 || h <= 0) return;
    float *z = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    if (!z) return;
    if (rhi_read_render_target_depth(s_nr.dev, s_nr.terrain_key_depth, z,
                                     (size_t)w * (size_t)h, &w, &h)) {
        char stem[1100], path[1110];
        path_with_suffix(base, "-key", stem, sizeof(stem));
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        snprintf(path, sizeof(path), "%s.f32", stem);
        FILE *fp = fopen(path, "wb");
        if (fp) { fwrite(z, sizeof(float), (size_t)w * (size_t)h, fp); fclose(fp); }
        snprintf(path, sizeof(path), "%s.txt", stem);
        fp = fopen(path, "w");
        if (fp) {
            const openpete_tercache_clip *c = &s_tk_last.clip;
            fprintf(fp, "w=%d h=%d vw=%d vh=%d x_base=%.9g x_span=%.9g y_base=%.9g "
                        "y_span=%.9g disp_w=%.9g clip_h=%.9g band=%.9g freeze_wf=%.9g "
                        "full_extent=%d frame=%u\n",
                    w, h, s_tk_last.vw, s_tk_last.vh, (double)c->x_base, (double)c->x_span,
                    (double)c->y_base, (double)c->y_span, (double)c->disp_w,
                    (double)c->clip_h, (double)c->band, (double)c->freeze_wf,
                    c->full_extent, openpete_frame);
            fclose(fp);
        }
    }
    free(z);
}

/* cap_out/cap_w/cap_h: when cap_out is non-NULL the composed native RGBA frame
 * is handed to the caller (malloc'd, top-down, alpha forced opaque) and no PNG
 * is written - the in-memory form of the same compose the -vk dump writes. */
static int native_render_screenshot_all_impl(const char *base, unsigned char **cap_out,
                                             int *cap_w, int *cap_h);

/** @brief Count a scene's 2D-layer triangles: HUD, HUD_PANEL, OVERLAY and carry copies.
 *
 * The freeze-leg probe compares what a composite holds against the
 * canonical scene it should match.
 * @param sc Scene triangles.
 * @param n Triangle count.
 * @return The 2D triangle count. */
static int scene_2d_count(const scene_tri_t* sc, int n) { return scene_2d_count_fwd(sc, n); }
int scene_2d_count_fwd(const scene_tri_t* sc, int n);
/** @brief Return whether OPENPETE_FREEZE_LEG_PROBE is set to a non-zero value.
 *
 * Enables the `[freezeleg]` line that openpete_native_render_snapshot_frozen
 * prints per snapshot request. Read once and cached.
 * @return 1 when the probe is on, else 0. */
static int freeze_leg_probe(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_FREEZE_LEG_PROBE"); v = (e && *e && *e != '0'); }
    return v;
}

void openpete_native_render_snapshot_frozen(float cap_wf)
{
    if (!s_nr.enabled) return;
    ps_apply_geometry();
    if (cap_wf < 1.0f) cap_wf = 1.0f;
    /* Only freeze-source frames: a freeze frame, a menu frame or a frame whose
     * terrain feed cut out would overwrite the good backdrop with an empty or
     * ground-less one. The retained terrain must land in the frozen backdrop
     * too, and the verdict reads the selected state, so select first. */
    int scene_n; uint32_t sbg, sfade, sfreeze;
    const scene_tri_t* sbuf = scene_present_buf(&scene_n, &sbg, &sfade, &sfreeze, NULL, NULL);
    openpete_tercache_frame_select(scene_present_stamp());
    const bool src = scene_is_freeze_source(sbuf, scene_n, sfreeze);
    if (freeze_leg_probe())
        fprintf(stderr, "[freezeleg f=%u] copies_stamp=%u scene_n=%d twod=%d freeze=%d skip=%d\n",
                s_scene_frame, scene_present_stamp(), scene_n, scene_2d_count(sbuf, scene_n),
                (sfreeze & (1u << 31)) ? 1 : 0, src ? 0 : 1);
    if (!src) return;

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

void openpete_native_occl_service_headless(void)
{
    if (!s_nr.enabled || !s_nr.headless || !s_nr.dev) return;
    ps_apply_geometry();
    if (s_nr.ref_w <= 0 || s_nr.ref_h <= 0) {
        int dh = s_nr.disp_h > 0 ? s_nr.disp_h : 240;
        int ch = s_nr.clip_h > 0 ? s_nr.clip_h : 224;
        int rh = ch * 2;
        int rw = (int)((double)rh * (4.0 / 3.0) * (double)dh / (double)ch + 0.5);
        if (rw > 0 && rh > 0) { s_nr.ref_w = rw; s_nr.ref_h = rh; }
    }
    sync_vram_texture();
    ensure_opaque_depth();
    if (!rhi_begin_frame(s_nr.dev)) return;
    openpete_tercache_frame_select(scene_present_stamp());
    upload_synced_textures();
    native_depth_prepass();
    native_occl_downsample();
    rhi_end_frame(s_nr.dev);
}

/** @brief Render the widescreen (Hor+) native-only screenshot into shot_rtt and write it as the -vk PNG or hand it back in memory.
 *
 * The scene is composed at the widened extent, ref_w * cap_wf by @p H,
 * matching the extraction-time widen of openpete_native_widescreen_factor,
 * which honours --aspect headless. s_render_freeze_wf is set to @p cap_wf
 * around the compose so the depth pre-pass affine and the scene draw
 * agree. The compose repeats, up to nine times, while HD texture uploads
 * are pending. The image is content-sized with no window letterbox, alpha
 * forced opaque; no ab or diff variant is written, since native-only mode
 * has no PsyCross reference.
 * @param base Screenshot path stem; the PNG path takes the -vk suffix.
 * @param H Capture height in pixels.
 * @param cap_wf Widescreen factor of the capture.
 * @param cap_out When not NULL, receives the malloc'd RGBA buffer, owned by
 *        the caller, and no PNG is written.
 * @param cap_w Receives the width when @p cap_out is used; may be NULL.
 * @param cap_h Receives the height when @p cap_out is used; may be NULL.
 * @return 0 on success, 1 on a target, frame, allocation, readback or
 *         write failure. */
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
     * content lands centred and the Hor+ reveal fills the widened sides - the
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
    terrain_key_dump(base, cap_out != NULL);
    for (size_t i = 0; i < px; i++) nat[i * 4 + 3] = 0xFF;   /* GPU blend alpha -> opaque */

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
    depth_transport_census_shot(path);
    free(nat);
    return rc;
}

/* The compose flags are set and cleared here so the impl's early returns
 * cannot leak them into the live present path. */
int openpete_native_render_screenshot_all(const char *base)
{
    /* Headless never runs openpete_native_render_frame, so the truescope
     * re-bake must happen here, outside the compose's command recording. */
    truescope_apply_pending();
    s_scene_shot_compose = 1;
    s_scene_shot_lag = s_nr.headless ? 1 : 0;
    /* Retained terrain rides the same scene selection: the state whose stamp
     * equals the composed scene's stamp (live, else staged). */
    openpete_tercache_frame_select(scene_present_stamp());
    if (scene_pos_sig_on()) scene_pos_sig_report(1);   /* the scene this dump composes */
    { static int pz = -1;
      if (pz < 0) { const char* e = getenv("OPENPETE_FREEZE_LEG_PROBE"); pz = (e && *e && *e != '0'); }
      if (pz) { int n; uint32_t bg, fd, fz;
        const scene_tri_t* b = scene_present_buf(&n, &bg, &fd, &fz, NULL, NULL);
        fprintf(stderr, "[dumpscene f=%u] lag=%d composes_stamp=%u scene_n=%d twod=%d\n",
                s_scene_frame, s_scene_shot_lag, scene_present_stamp(), n,
                scene_2d_count_fwd(b, n));
        for (int t = 0; t < n; t++) {
            const scene_tri_t* q = &b[t];
            if (!(q->category == OPENPETE_SCENE_HUD || q->category == OPENPETE_SCENE_HUD_PANEL ||
                  q->category == OPENPETE_SCENE_OVERLAY || q->carry)) continue;
            fprintf(stderr, "[2dtri] seq=%u cat=%d bl=%d tex=%d bpp=%d v=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) rgba=%08X\n",
                    q->seq, q->category, q->blend, q->textured, q->bpp,
                    q->v[0].sx, q->v[0].sy, q->v[1].sx, q->v[1].sy, q->v[2].sx, q->v[2].sy,
                    q->v[0].rgba);
        }
      } }
    int r = native_render_screenshot_all_impl(base, NULL, NULL, NULL);
    s_scene_shot_compose = 0;
    s_scene_shot_lag = 0;
    return r;
}

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

/** @brief Map a PSX framebuffer screen point to a pixel position of a full-extent capture.
 *
 * The point goes through scene_to_clip under the full-extent mapping at
 * the capture's widen, so the crop cannot drift from where the scene itself
 * lands; the previous mapping state is restored.
 * @param sx Screen x.
 * @param sy Screen y.
 * @param capw Capture width in pixels.
 * @param caph Capture height in pixels.
 * @param wf Widescreen factor the capture was composed at.
 * @param px Receives the pixel x.
 * @param py Receives the pixel y. */
static void fbsnap_px(float sx, float sy, int capw, int caph, float wf,
                      float *px, float *py)
{
    const int   prev_fe = s_render_full_extent;
    const float prev_wf = s_render_freeze_wf;
    s_render_full_extent = 1; s_render_freeze_wf = wf;
    float cx, cy; scene_to_clip(sx, sy, &cx, &cy);
    s_render_full_extent = prev_fe; s_render_freeze_wf = prev_wf;
    *px = (cx + 1.0f) * 0.5f * (float)capw;
    *py = (1.0f - cy) * 0.5f * (float)caph;
}

/** @brief Crop a full-extent capture to a framebuffer screen rectangle.
 *
 * The rectangle's corners map through fbsnap_px, round to the nearest
 * pixel and clamp to the capture.
 * @param cap Capture, RGBA.
 * @param cw Capture width in pixels.
 * @param ch Capture height in pixels.
 * @param wf Widescreen factor the capture was composed at.
 * @param sx Rectangle left in screen coordinates.
 * @param sy Rectangle top in screen coordinates.
 * @param w Rectangle width.
 * @param h Rectangle height.
 * @param out_w Receives the crop width.
 * @param out_h Receives the crop height.
 * @return A malloc'd RGBA crop owned by the caller, or NULL when the
 *         rectangle maps to no pixel or the allocation fails. */
static unsigned char *fbsnap_crop(const unsigned char *cap, int cw, int ch, float wf,
                                  int sx, int sy, int w, int h, int *out_w, int *out_h)
{
    float fx0, fy0, fx1, fy1;
    fbsnap_px((float)sx, (float)sy, cw, ch, wf, &fx0, &fy0);
    fbsnap_px((float)(sx + w), (float)(sy + h), cw, ch, wf, &fx1, &fy1);
    int x0 = (int)(fx0 + 0.5f), y0 = (int)(fy0 + 0.5f);
    int x1 = (int)(fx1 + 0.5f), y1 = (int)(fy1 + 0.5f);
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > cw) x1 = cw; if (y1 > ch) y1 = ch;
    const int pw = x1 - x0, ph = y1 - y0;
    if (pw <= 0 || ph <= 0) return NULL;
    unsigned char *px = (unsigned char *)malloc((size_t)pw * (size_t)ph * 4u);
    if (!px) return NULL;
    for (int row = 0; row < ph; row++)
        memcpy(px + (size_t)row * pw * 4u,
               cap + ((size_t)(y0 + row) * cw + x0) * 4u, (size_t)pw * 4u);
    *out_w = pw; *out_h = ph;
    return px;
}

/** @brief Return whether a copy naming a frame composes the kept game-render scene rather than the scene ring.
 *
 * The kept scene is the game's own render, so it serves its own frame first.
 * It also serves a later frame whose scene the ring holds empty or not at
 * all: a re-extract rebuilt that frame from records a paused tick does not
 * hold, and the buffer then still holds the last frame the game drew.
 * @param frame Stamp the copy names.
 * @return 1 to compose s_scene_game. */
static int fbsnap_use_game_scene(unsigned frame) {
    if (s_scene_game.n <= 0 || s_scene_game.frame > frame) return 0;
    if (s_scene_game.frame == frame) return 1;
    for (int d = 0; d <= scene_hist_depth_max(); d++) {
        if (scene_frame_at_depth(d) != frame) continue;
        const int slot = scene_hist_slot_at(d);
        if ((d == 0 ? s_scene_n : (slot >= 0 ? s_scene_hist_n[slot] : 0)) > 0) return 0;
    }
    return 1;
}

/** @brief Census the scene a framebuffer-window resolve composes, on stderr, under OPENPETE_FBSNAP_CENSUS.
 *
 * Prints the stamp the copy names, the stamp of the scene selected for the
 * compose, its triangle count, and its 2D triangles (HUD, HUD panel,
 * overlay) whose screen box overlaps the copied rectangle: a window copy
 * holds the buffer before the 2D drawn after the copy, so a scene holding
 * that 2D shows here as a nonzero count. Read once.
 * @param frame Stamp the copy names.
 * @param sx Rectangle left in screen coordinates.
 * @param sy Rectangle top.
 * @param w Rectangle width.
 * @param h Rectangle height. */
static void fbsnap_resolve_census(unsigned frame, int sx, int sy, int w, int h)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_FBSNAP_CENSUS"); on = (e && *e && *e != '0'); }
    if (!on) return;
    const scene_view_t v = scene_present_view();
    int n2d = 0;
    for (int t = 0; t < v.n; t++) {
        const scene_tri_t* q = &v.tri[t];
        if (q->category != OPENPETE_SCENE_HUD && q->category != OPENPETE_SCENE_HUD_PANEL &&
            q->category != OPENPETE_SCENE_OVERLAY) continue;
        float x0 = q->v[0].sx, x1 = x0, y0 = q->v[0].sy, y1 = y0;
        for (int j = 1; j < 3; j++) {
            x0 = fminf(x0, q->v[j].sx); x1 = fmaxf(x1, q->v[j].sx);
            y0 = fminf(y0, q->v[j].sy); y1 = fmaxf(y1, q->v[j].sy);
        }
        n2d += x0 < (float)(sx + w) && (float)sx < x1 && y0 < (float)(sy + h) && (float)sy < y1;
    }
    fprintf(stderr, "[fbsnap] resolve want=%u composed=%u game_sel=%d tris=%d 2d_in_rect=%d cover=%d\n",
            frame, scene_present_stamp(), s_fbsnap_game_sel, v.n, n2d, ps_cover()->cnt[0]);
}

/** @brief Compose a completed scene in memory and crop it to a framebuffer screen rectangle.
 *
 * s_fbsnap_want_frame selects the scene from the completed-scene ring for
 * the compose, which is the one the -vk dump and the live present produce;
 * the kept game-render scene wins where fbsnap_use_game_scene says so.
 * The crop uses the widen the compose used, capture width over ref_w, not
 * the one requested. A NULL result leaves the caller's slot unresolved, so
 * its quads keep drawing the previous snapshot.
 * A window slot's compose samples the VRAM texels its copy overwrote
 * (s_vram_under), since the scene was drawn before the copy.
 * @param frame openpete_frame stamp of the scene the copy names.
 * @param win Window slot index, or -1 for a draw area.
 * @param sx Rectangle left in screen coordinates.
 * @param sy Rectangle top in screen coordinates.
 * @param w Rectangle width.
 * @param h Rectangle height.
 * @param out_w Receives the crop width.
 * @param out_h Receives the crop height.
 * @return A malloc'd RGBA crop owned by the caller, or NULL when the
 *         compose or the crop fails. */
static unsigned char *fbsnap_capture_crop(unsigned frame, int win, int sx, int sy, int w, int h,
                                          int *out_w, int *out_h)
{
    unsigned char *cap = NULL; int cw = 0, ch = 0;
    s_fbsnap_want_frame = frame;
    s_fbsnap_game_sel = fbsnap_use_game_scene(frame);
    fbsnap_resolve_census(frame, sx, sy, w, h);
    {   rhi_rect ur = { 0, 0, 0, 0 };
        const uint16_t *under = win >= 0
            ? openpete_fb_snapshot_under(win, &ur.x, &ur.y, &ur.w, &ur.h) : NULL;
        SDL_LockMutex(vram_mx());
        s_vram_under.pix = under; s_vram_under.r = ur;
        SDL_UnlockMutex(vram_mx()); }
    const int r = openpete_native_render_capture_rgba(&cap, &cw, &ch);
    SDL_LockMutex(vram_mx());
    s_vram_under.pix = NULL;
    SDL_UnlockMutex(vram_mx());
    s_fbsnap_want_frame = 0;
    s_fbsnap_game_sel = 0;
    if (r != 0) return NULL;
    /* The widen the compose actually used, not the one it was asked for. */
    const float wf = (s_nr.ref_w > 0) ? (float)cw / (float)s_nr.ref_w : 1.0f;
    unsigned char *px = fbsnap_crop(cap, cw, ch, wf, sx, sy, w, h, out_w, out_h);
    free(cap);
    return px;
}

/** @brief Queue the upload of the image a snapshot index holds, unless it is cached or queued already.
 *
 * The present's HD flush then uploads it before the scene draws, so the
 * first present that samples a fresh window image draws it instead of
 * queueing it and drawing nothing.
 * @param idx Window slot index, or OPENPETE_FB_SNAPSHOT_AREA_BASE plus a draw area index. */
static void fbsnap_queue_upload(int idx)
{
    uint64_t key = 0; int sw = 0, sh = 0;
    if (!openpete_fb_snapshot_image(idx, &key, &sw, &sh)) return;
    if (hd_cache_lookup(key) || hd_pending_has(key)) return;
    unsigned char *copy = openpete_fb_snapshot_copy(idx, key, &sw, &sh);
    if (copy) hd_pending_add(key, NULL, copy, sw, sh, 0, 0);
}

void openpete_native_render_snapshot_fb_window(void)
{
    if (!s_nr.enabled) return;
    if (!openpete_fb_snapshot_pending()) return;
    int idx, sx, sy, w, h, cw = 0, ch = 0; unsigned frame = 0;
    while (openpete_fb_snapshot_take_pending(&idx, &sx, &sy, &w, &h, &frame)) {
        int pw = 0, ph = 0;
        unsigned char *px = fbsnap_capture_crop(frame, idx, sx, sy, w, h, &pw, &ph);
        if (px) { openpete_fb_snapshot_store(idx, px, pw, ph); fbsnap_queue_upload(idx); }
    }
    while (openpete_fb_snapshot_take_pending_area(&idx, &sx, &sy, &w, &h, &frame)) {
        int pw = 0, ph = 0;
        unsigned char *px = fbsnap_capture_crop(frame, -1, sx, sy, w, h, &pw, &ph);
        if (px) { openpete_fb_snapshot_store_area(idx, px, pw, ph); fbsnap_queue_upload(OPENPETE_FB_SNAPSHOT_AREA_BASE + idx); }
    }
    { static int s_census = -1;
      if (s_census < 0) { const char *e = getenv("OPENPETE_FBSNAP_CENSUS"); s_census = (e && *e && *e != '0'); }
      if (s_census) {
          extern unsigned openpete_frame;
          unsigned taken = 0, resolved = 0, quads = 0;
          openpete_fb_snapshot_counts(&taken, &resolved, &quads);
          fprintf(stderr, "[fbsnap] f=%u taken=%u resolved=%u quads=%u\n",
                  openpete_frame, taken, resolved, quads);
      } }
}

/** @brief Read the offscreen colour target back and write it as a PNG.
 *
 * The readback blocks on the GPU and returns the image the last rendered
 * frame left in off_image, into a buffer sized for the window. Blend ops
 * write real alpha into that image (opaque on llvmpipe, fractional on
 * discrete GPUs); alpha is forced to 255 so the file shows what the window
 * showed. Callers are the windowed live-present `-vk` capture and
 * openpete_native_render_screenshot_ext.
 * @param path Output PNG path.
 * @param what Label for the log line.
 * @param out_w Receives the width on success, else 0; may be NULL.
 * @param out_h Receives the height on success, else 0; may be NULL.
 * @return 0 on success, nonzero on failure. */
static int shot_write_offscreen_ext(const char *path, const char *what,
                                    int *out_w, int *out_h)
{
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    size_t cap = (size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u;
    if (cap == 0) return 1;
    unsigned char *buf = malloc(cap);
    if (!buf) return 1;
    int w = 0, h = 0, rc = 1;
    if (rhi_read_offscreen(s_nr.dev, buf, cap, &w, &h)) {
        /* Blend ops write real alpha into off_image (driver-dependent; opaque
         * on llvmpipe, fractional on discrete GPUs). The display ignores it,
         * but a PNG keeps it and viewers then show transparency checkerboard -
         * force opaque so the file shows exactly what the window showed. */
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
            buf[i * 4 + 3] = 0xFF;
        rc = openpete_write_png_rgba(path, buf, w, h);
    }
    free(buf);
    if (rc == 0) {
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        OPL_INFO_F("native", "%s screenshot %dx%d -> %s", what, w, h, path);
    }
    return rc;
}

/** @brief Write the offscreen image to a PNG with alpha forced opaque, discarding the dimensions.
 *
 * Wrapper over shot_write_offscreen_ext with no dimension outputs.
 * @param path Output PNG path.
 * @param what Label for the log line.
 * @return 0 on success, nonzero on failure. */
static int shot_write_offscreen(const char *path, const char *what)
{
    return shot_write_offscreen_ext(path, what, NULL, NULL);
}

/** @brief Write the native `-vk` capture for one screenshot base path, or hand the composed image to the caller.
 *
 * In a windowed run that is not an intermediate capture, `-vk` is first
 * written as a readback of the image the live present just showed. The
 * native scene is then recomposed into the reference-sized render target
 * (or the widened target for a Hor+ native-only capture) and read back with
 * alpha forced opaque. With @p cap_out the buffer goes to the caller and no
 * PNG is written; otherwise the recompose is written as `-vk` when the live
 * readback was not taken, and an empty native scene is replaced by the
 * PsyCross reference readback when one exists.
 * @param base    Screenshot path without its suffix.
 * @param cap_out NULL to write PNGs, else receives a malloc'd top-down RGBA
 *                buffer the caller frees.
 * @param cap_w   Receives the captured width when @p cap_out is set.
 * @param cap_h   Receives the captured height when @p cap_out is set.
 * @return 0 on success, 1 on failure. */
static int native_render_screenshot_all_impl(const char *base, unsigned char **cap_out,
                                             int *cap_w, int *cap_h)
{
    ps_apply_geometry();
    if (!s_nr.enabled || !base) return 1;

    /* Live-present -vk (pure-vk window runs): off_image still holds the frame
     * the user is seeing - the present just blitted it to the swapchain - so
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
     * always under --headless where openpete_native_render_frame - which would
     * otherwise derive this - early-returns), ref_w/ref_h stay 0 and the dump
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
     * without one (pure vk with PsyCross in null-GPU library mode - no FBO
     * exists at all) the dump writes the native scene as composed. ref_w/ref_h
     * are still valid: the present path derives them from gamestate when there
     * is no reference (see the !have_ref branch in
     * openpete_native_render_frame). */
    bool with_ref = have_ref && s_nr.readback != NULL;
    if (!s_nr.native_rtt || s_nr.ref_w <= 0 || s_nr.ref_h <= 0)
        return 1;

    /* Live -vk written and no reference -> nothing further to build; the
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
        upload_synced_textures();  /* pending VRAM/ref uploads ride this frame */
        hd_flush_pending();        /* HD replacement textures queued at draw time */
        openpete_native_hdfold_flush();
        native_depth_prepass();    /* shared WORLD-opaque depth for the shadow decal */
        /* BLACK clear - live-offscreen parity. The live present clears black and
         * relies on the scene's own bg-fill quad; clearing to the bg colour here
         * painted over exactly the class of bug where that quad is built but
         * never drawn, making the dump unable to reproduce a live-visible black
         * region. Where the quad exists (every normal frame) the output is
         * identical. */
        { int kw, kh; rhi_texture_extent(s_nr.native_rtt, &kw, &kh);
          native_terrain_key_pass(1, kw, kh); }
        rhi_begin_render_target(s_nr.dev, s_nr.native_rtt, 0.0f, 0.0f, 0.0f, 1.0f);
        draw_native_scene(true);
        /* Mod draw-service content (3D meshes under the mod HUD) - the live
         * compose draws these into the offscreen after the scene; without them
         * the dump showed a mod-free frame while the window showed mod content.
         * Committed per tick and re-read here, so drawing after the live present
         * is idempotent. NB the live path runs the SCENE postfx point BEFORE
         * these; the RT pass can't re-open with a load-op, so the dump runs the
         * SCENE chain after them - mod content gets chained into SCENE passes
         * here (visible only when a SCENE-point pass AND a mod overlay are
         * both active). */
        draw_mod_overlay();
        rhi_end_render_target(s_nr.dev, s_nr.native_rtt);
        /* Postfx over the dump compose too: this path bypasses off_image, so
         * without these calls a headless or screenshot-all capture would
         * carry no postfx. SCENE point first (--raster-quantize, dither, mod
         * scene shaders), then COMPOSITE. */
        native_postfx_run(1 /* SCENE */, s_nr.native_rtt, W, H);
        native_postfx_run(0 /* COMPOSITE */, s_nr.native_rtt, W, H);
        rhi_begin_offscreen(s_nr.dev, br, bgc, bb, 1.0f);
        rhi_end_offscreen(s_nr.dev);
        rhi_blit_to_swapchain(s_nr.dev);
        rhi_end_frame(s_nr.dev);
        /* Self-heal a first sighting: an HD/mod texture seen for the first
         * time mid-draw only QUEUES its upload and draws nothing for those
         * faces (hd_pending_add). Live presents absorb that as one SD frame,
         * but in a headless run the only frames ARE these dumps - a single
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
    terrain_key_dump(base, cap_out != NULL);
    /* RT alpha is whatever the blend ops left (fractional on discrete GPUs) -
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
        depth_transport_census_shot(path);
    }
    free(nat); free(ref);
    return rc;
}

int openpete_native_render_screenshot_ext(const char *path, const char *what,
                                          int *out_w, int *out_h)
{
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    ps_apply_geometry();
    if (!s_nr.enabled || !path) return 1;
    return shot_write_offscreen_ext(path, what ? what : "offscreen", out_w, out_h);
}

int openpete_native_render_screenshot(const char *path, const char *what)
{
    return openpete_native_render_screenshot_ext(path, what, NULL, NULL);
}

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
    scene_box_summary();
    rhi_depth_report(s_nr.dev);   /* every pipeline compiled over the run */
    /* Sub-tick view engagement. Reported unconditionally because a run that
     * never opened it cannot support any claim about the sub-tick path, and
     * that fact is invisible in the pixels. */
    {
        uint32_t opens = openpete_subtick_view_opens();
        uint32_t spans = 0, served = 0, writes = 0;
        openpete_subtick_view_stats(&spans, &served, &writes);
        OPL_INFO_F("subtick-view",
                   "presents=%u staged_spans=%u served_reads=%u (last present); "
                   "cpu_writes_while_open=%u writes_into_staging=%u poke_dropped=%u",
                   opens, spans, served, writes,
                   openpete_subtick_view_staged_writes(),
                   openpete_subtick_view_poke_drops());
        openpete_subtick_view_report_writes();
    }

    if (!s_nr.enabled)
        return;
    rhi_wait_idle(s_nr.dev);
    openpete_devui_shutdown();   /* imgui backend objects die before the device */
    openpete_tercache_shutdown(s_nr.dev);
    /* Mod postfx chain objects: ping pool, UBO, cached pipelines
     * (both live registrations and the retirement list). */
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            if (s_pfx_pool[i].t[j]) {
                rhi_destroy_texture(s_nr.dev, s_pfx_pool[i].t[j]);
                s_pfx_pool[i].t[j] = NULL;
            }
    if (s_pfx_ubo) { rhi_destroy_ubo(s_nr.dev, s_pfx_ubo); s_pfx_ubo = NULL; }
    if (s_shfoot_ubo) { rhi_destroy_ubo(s_nr.dev, s_shfoot_ubo); s_shfoot_ubo = NULL; }
    if (s_cover_ubo) { rhi_destroy_ubo(s_nr.dev, s_cover_ubo); s_cover_ubo = NULL; }
    if (s_mat_ubo) { rhi_destroy_ubo(s_nr.dev, s_mat_ubo); s_mat_ubo = NULL; }
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
    rhi_destroy_texture(s_nr.dev, s_nr.ref_tex);
    rhi_destroy_texture(s_nr.dev, s_nr.native_rtt);
    rhi_destroy_texture(s_nr.dev, s_nr.shot_rtt);
    rhi_destroy_texture(s_nr.dev, s_nr.shot_frozen);
    rhi_destroy_texture(s_nr.dev, s_nr.frozen_tex);
    rhi_destroy_texture(s_nr.dev, s_nr.ss_rtt);
    rhi_destroy_texture(s_nr.dev, s_nr.opaque_depth);
    rhi_destroy_texture(s_nr.dev, s_nr.occl_grid);
    rhi_destroy_pipeline(s_nr.dev, s_nr.occl_pipe);
    rhi_destroy_buffer(s_nr.dev, s_nr.prepass_vb);
    rhi_destroy_pipeline(s_nr.dev, s_nr.prepass_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.prepass_depth_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.prepass_tex_pipe);
    rhi_destroy_texture(s_nr.dev, s_nr.terrain_key_depth);
    rhi_destroy_texture(s_nr.dev, s_nr.moby_key_depth);
    rhi_destroy_texture(s_nr.dev, s_nr.actor_key_depth);
    rhi_destroy_texture(s_nr.dev, s_nr.moby_rank_rt);
    rhi_destroy_buffer(s_nr.dev, s_nr.terrain_key_vb);
    rhi_destroy_pipeline(s_nr.dev, s_nr.terrain_key_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.terrain_key_tex_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.moby_rank_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.moby_rank_cutout_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_sub_ztest_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_sub_fade_pipe);
    rhi_destroy_texture(s_nr.dev, s_nr.ovl_ss);
    rhi_destroy_buffer(s_nr.dev, s_nr.ovl_quad_vb);
    rhi_destroy_texture(s_nr.dev, s_nr.menu_ss);
    rhi_destroy_texture(s_nr.dev, s_nr.osd_ss);
    rhi_destroy_buffer(s_nr.dev, s_nr.menu_quad_vb);
    rhi_destroy_pipeline(s_nr.dev, s_nr.ovl_resolve_pipe);
    rhi_destroy_texture(s_nr.dev, s_nr.vram_tex);
    if (s_nr.drain_tex) rhi_destroy_texture(s_nr.dev, s_nr.drain_tex);
    rhi_destroy_buffer(s_nr.dev, s_nr.fs_quad_vb);
    rhi_destroy_buffer(s_nr.dev, s_nr.overlay_vb);
    if (s_nr.version_vb) rhi_destroy_buffer(s_nr.dev, s_nr.version_vb);
    if (s_nr.ver_quad_vb) rhi_destroy_buffer(s_nr.dev, s_nr.ver_quad_vb);
    if (s_nr.ver_ss) rhi_destroy_texture(s_nr.dev, s_nr.ver_ss);
    rhi_destroy_buffer(s_nr.dev, s_nr.mod_ovl_vb);
    rhi_destroy_buffer(s_nr.dev, s_nr.menu_vb);
    rhi_destroy_buffer(s_nr.dev, s_nr.menu_glyph_vb);
    rhi_destroy_buffer(s_nr.dev, s_nr.osd_glyph_vb);
    rhi_destroy_buffer(s_nr.dev, s_nr.osd_quad_vb);
    rhi_destroy_buffer(s_nr.dev, s_nr.osd_panel_vb);
    rhi_destroy_buffer(s_nr.dev, s_nr.scene_vb);
    rhi_destroy_buffer(s_nr.dev, s_nr.panels_vb);
    rhi_destroy_pipeline(s_nr.dev, s_nr.freeze_overlay_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.freeze_plain_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.resolve_pipe);
    blend_family_destroy(s_nr.dev, s_nr.scene_tex_hud_pipes, OPENPETE_STP_SELS);
    rhi_destroy_pipeline(s_nr.dev, s_nr.hd_tex_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.hd_cutout_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.hd_cutout_mark_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_punch_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_punch_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.hd_cutout_punch_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.hd_scene_blend_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.hd_scene_add_pipe);
    hd_cache_shutdown();
    if (s_nr.mat_pack_dummy) { rhi_destroy_texture(s_nr.dev, s_nr.mat_pack_dummy); s_nr.mat_pack_dummy = NULL; }
    texture_pack_shutdown();
    blend_family_destroy(s_nr.dev, s_nr.gouraud_hud_pipes, 1);
    rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_sub_decal_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_sub_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_qadd_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_qadd_pipe);
    cover_family_destroy(s_nr.dev, s_nr.cover_twins, &s_nr.cover_twin_n);
    rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_add_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_blend_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_noz_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.portal_mask_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_mark_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_mark_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_fill_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_add_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_sub_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_blend_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.vram_pipe);
    rhi_destroy_pipeline(s_nr.dev, s_nr.blit_pipe);
    rhi_destroy(s_nr.dev);
    SDL_DestroyWindow(s_nr.window);
    free(s_nr.readback);
    /* s_nr.vram_mirror is the static native VRAM model (native_vram.c) - not freed. */
    free(s_recbuf); s_recbuf = NULL; s_reccap = 0;   /* --record async ring host buffer */

    memset(&s_nr, 0, sizeof(s_nr));
    if (rhi_seam_violations())
        OPL_ERROR_F("native", "display-seam fence: %u call(s) reached the device "
                    "from a thread that did not own the display (see SEAM lines; "
                    "OPENPETE_SEAM_GUARD_FATAL=1 aborts at the first)",
                    rhi_seam_violations());
    OPL_INFO_F("native", "Vulkan native renderer window shut down");
}
