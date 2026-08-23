/* native_terrain_cache.c — terrain sector-mesh GPU cache, core side.
 * See openpete_terrain_cache.h for the contract and
 * notes/plans/openpete-terrain-gpu-cache.md for the design. */

#include "rhi.h"
#include "openpete_terrain_cache.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Arena capacities (vertices). Sized from worst-case level content: a big
 * level carries ~50k HP faces x (6 flat + 24 HQ-variant) verts and a few
 * thousand LP/blend faces x 6. 64 B/vert => OPAQUE 96 MB, total ~128 MB in
 * device-local (ReBAR) memory. A sector whose chunk doesn't fit is simply
 * claimed back to the CPU walk — overflow degrades perf, never correctness.
 * (Dynamic growth is a follow-up; see the plan's open questions.) */
static const uint32_t k_arena_cap[OPTC_ARENA_COUNT] = {
    [OPTC_ARENA_OPAQUE] = 1500000,
    [OPTC_ARENA_ALPHA]  =  131072,
    [OPTC_ARENA_ADD]    =  131072,
    [OPTC_ARENA_SUB]    =  131072,
    [OPTC_ARENA_LP]     =  262144,
    /* Poly-anim twins: a level names a handful of anim sectors (<= 256
     * faces each), so these stay small; overflow claims back to the CPU
     * walk like any arena-full. Each also carries a parallel prev buffer
     * (rhi_terrain_prev, 32 B/vert). */
    [OPTC_ARENA_A_OPAQUE] = 65536,
    [OPTC_ARENA_A_ALPHA]  = 16384,
    [OPTC_ARENA_A_ADD]    = 16384,
    [OPTC_ARENA_A_SUB]    = 16384,
    [OPTC_ARENA_A_LP]     = 32768,
    /* Near-variant twins: only sectors within reach of the near plane bake
     * these, but each source triangle costs 6 vertices (primary + spare fan
     * triangle) plus a 24 B ctx record, so size them for the handful of
     * sectors around the camera rather than the whole visible set. Overflow
     * claims the sector back to the CPU walk like any arena-full. */
    [OPTC_ARENA_N_OPAQUE] = 393216,
    [OPTC_ARENA_N_ALPHA]  =  32768,
    [OPTC_ARENA_N_ADD]    =  32768,
    [OPTC_ARENA_N_SUB]    =  32768,
    [OPTC_ARENA_N_LP]     =  65536,
};

#define TC_FREE_MAX   1024   /* free-list spans per arena */
#define TC_DEFER_MAX  1024   /* deferred releases across arenas */
/* Freed ranges stay unreused for this many BUILDS: covers both the RHI
 * frames-in-flight window and the one-build presentation lag (the LIVE
 * state may still generation-reference a chunk the newest build replaced). */
#define TC_DEFER_PRESENTS 4

typedef struct { uint32_t first, count; } tc_span;

typedef struct {
    rhi_buffer *vb;
    rhi_buffer *pv;                   /* anim arenas: parallel prev-tick buffer
                                       * near arenas: parallel ctx buffer      */
    uint32_t    cap;
    uint32_t    high;                 /* high-water vertex count (draw range) */
    tc_span     free_list[TC_FREE_MAX];
    int         free_n;
    uint32_t    used;                 /* live verts (census) */
    uint32_t    chunks;               /* live chunk count (census) */
} tc_arena;

typedef struct { uint8_t arena; uint32_t first, count, stamp; } tc_defer;

/* std140 globals block (mirrors terrain.vert TerrainGlobals). Camera /
 * projection / fog scalars are DOUBLE — the vertex stage re-runs the CPU
 * walk's double GTE chain bit-for-bit. C natural layout == std140 here
 * (vec4 x3 = 48 B, then four 16 B-aligned dvec2, then ivec4 at 112). */
typedef struct {
    float mrow0[4];    /* M[0][0..2], pad */
    float mrow1[4];    /* M[1][0..2], pad */
    float mrow2[4];    /* M[2][0..2], pad */
    double dcam_xy[2]; /* camx, camy      */
    double dcam_zf[2]; /* camz, fogscale  */
    double dof_xy[2];  /* ofx, ofy        */
    double dH[2];      /* H, poly-anim alpha */
    double dnear[2];   /* near-plane view-Z (clip mirror), reserved */
    double dspare[2];  /* reserved — see the 32-byte stride note below */
    int32_t fog[4];    /* fogRefBase, gflags, cullRefS2, 0 */
    float sdv[128][4]; /* per-tile scroll V offsets (lq, hqA, hqB, 0) */
    /* Portal inner-rim cull frames (portal_rim_classify mirror; all fp64).
     * Offset here is 2176 = 32-aligned, so the dvec4 run below keeps
     * C natural layout == std140 (dvec4 base alignment 32, dvec2 array
     * stride 16 == double[2]). rim_cnt.x = plane count (0 = test off). */
    int32_t rim_cnt[4];
    int32_t rim_pad[4];
    double  rim_oxyH[4];   /* rim ofx, ofy, H, pad                       */
    double  rim_knobs[4];  /* eps, near, reach, cos(angle)               */
    double  rim_abc[6][4]; /* screen inv-vz plane fit a,b,c; w = valid   */
    double  rim_O[6][4];   /* in-plane basis origin                      */
    double  rim_U[6][4];   /* in-plane basis U                           */
    double  rim_W[6][4];   /* in-plane basis W                           */
    double  rim_nrm[6][4]; /* unit view normal; w = outline vn           */
    double  rim_bbox[6][4];/* outline UV bbox pulo, puhi, pvlo, pvhi     */
    double  rim_pv[96][2]; /* outlines in (U,W), 16 slots per plane      */
    /* HD texture-replacement fold: the continuous-fog ramp (ts, tb) +
     * entry count/enable, then the entry table (4 vec4 per entry: page
     * rect, atlas rect, fogA + filter class, fogB). Snapshotted from
     * openpete_native_hdfold_table at frame_begin. */
    float   hd_lad[4];
    float   hd_entry[OPENPETE_HDFOLD_MAX][16];
    /* Blended-face portal-cover drop (portal_sky_covers mirror): active
     * opening planes in screen space. hd_entry ends 16- but not 32-aligned,
     * so the ivec4 first keeps the dvec4 run at std140's 32-byte base
     * alignment (C natural layout == std140 continues to hold). cov_poly
     * holds one screen point per vec4 slot, 16 slots per plane. */
    int32_t cov_cnt[4];    /* x = plane count (0 = test off)              */
    double  cov_margin[4]; /* x = cover margin M (world units)            */
    double  cov_abc[6][4]; /* screen inv-vz plane fit a, b, c; w = pn     */
    float   cov_bbox[6][4];/* outline screen bbox x0, y0, x1, y1          */
    float   cov_poly[96][4];/* outline points (px, py, 0, 0), 16 per plane */
} tc_globals_ubo;

typedef struct {
    float o[4];             /* xyz origin, w flag bits                      */
    float gp[4], gq[4];     /* pack LP grade affine v' = p*v + q*255 (rgb);
                             * valid only with OPTC_SF_LPGRADE set in o.w   */
} tc_sector_ubo;

/* Both blocks ride one rhi_ubo with a uniform block size (the larger of the
 * two); everything must stay inside the 64 KB maxUniformBufferRange every
 * desktop implementation and llvmpipe provide. */
#define TC_UBO_BLOCK (sizeof(tc_globals_ubo) > sizeof(tc_sector_ubo) * OPTC_SLOTS \
                          ? sizeof(tc_globals_ubo)                                \
                          : sizeof(tc_sector_ubo) * OPTC_SLOTS)
_Static_assert(sizeof(tc_globals_ubo) <= 65536 &&
               sizeof(tc_sector_ubo) * OPTC_SLOTS <= 65536,
               "terrain-cache UBO block exceeds the 64 KB UBO-range floor");
/* The dvec4 runs must land on std140's 32-byte base alignment for C natural
 * layout to keep matching the block the vertex stage reads — otherwise GLSL
 * pads where C does not and EVERY field past the first dvec4 shifts (garbage
 * fog/gflags, all-killed vertices). Any field added above them must therefore
 * be a multiple of 32 bytes wide, not merely 16. */
_Static_assert(offsetof(tc_globals_ubo, rim_oxyH) % 32 == 0 &&
               offsetof(tc_globals_ubo, cov_margin) % 32 == 0,
               "terrain-cache globals: a dvec4 run lost its 32-byte alignment");

/* One build's complete retained frame state. The feeder STAGES the canonical
 * build's state and it PROMOTES to live one build later — the sub-tick
 * spine presents the previous canonical extraction (the canonical-lag leg),
 * and the retained draws must ride the same timeline or animated tiles and
 * moving cameras render one tick ahead of the CPU-fed scene around them.
 * Re-extracted presents (windowed intermediates + canon-lag re-extract)
 * publish immediately: their build IS the displayed state. */
typedef struct {
    tc_globals_ubo g;
    tc_sector_ubo  sec[OPTC_SLOTS];
    uint32_t       stamp;             /* openpete_frame of the build */
    int            valid;
} tc_state;

static struct {
    rhi_device   *dev;
    int           ready;
    tc_arena      arena[OPTC_ARENA_COUNT];
    rhi_ubo      *ubo;                /* block 0 = globals, block 1 = sectors */
    rhi_pipeline *pipe_world;         /* opaque, plain                        */
    rhi_pipeline *pipe_world_mark;    /* opaque + stencil MARK (hole-fill on) */
    rhi_pipeline *pipe_alpha;
    rhi_pipeline *pipe_add;
    rhi_pipeline *pipe_sub;
    rhi_pipeline *pipe_fill;          /* stencil TEST_ZERO                    */
    rhi_pipeline *pipe_prepass;       /* colour-write-off depth-only          */
    rhi_pipeline *pipe_key;           /* og OT-key emit (pass_sel 2)          */
    /* terrain_anim variants (binding-1 prev twin + vertex-stage lerp); no
     * anim FILL — poly-anim sectors never backstop-fill. */
    rhi_pipeline *pipe_a_world, *pipe_a_world_mark;
    rhi_pipeline *pipe_a_alpha, *pipe_a_add, *pipe_a_sub;
    rhi_pipeline *pipe_a_prepass, *pipe_a_key;
    /* terrain_near variants (binding-1 member-context twin + the vertex-stage
     * near-plane clip fan). Full set: near sectors carry every draw class. */
    rhi_pipeline *pipe_n_world, *pipe_n_world_mark;
    rhi_pipeline *pipe_n_alpha, *pipe_n_add, *pipe_n_sub;
    rhi_pipeline *pipe_n_fill, *pipe_n_prepass, *pipe_n_key;
    rhi_texture  *hd_dummy;           /* 1x1 stand-in for the set-4 atlas     */
    tc_state      staged;             /* canonical build awaiting its present */
    tc_state      live;               /* what presents render                 */
    tc_state      sel;                /* frame_select's snapshot of live: the
                                       * present's draws may execute VBLs after
                                       * selection (deferred screenshot
                                       * readback), by which time live has been
                                       * re-promoted — flushing live at draw
                                       * time would pair old chunks with a
                                       * fresher camera/fog/colour-generation
                                       * set (rung flips, sub-pixel skew) */
    tc_sector_ubo *build_target;      /* where sector_set writes this build   */
    int           draw_open;          /* frame_select verdict for this present */
    uint64_t      ubo_flush_serial;   /* rhi_frame_serial of the last UBO flush
                                       * for the CURRENT selection (0 = none).
                                       * Keyed on the frame serial, not a
                                       * boolean: a capture path can render one
                                       * selection across SEVERAL rhi frames
                                       * (the HD-pending screenshot retry), and
                                       * each frame's ring slot needs its own
                                       * flush or the retained draws bind a
                                       * never-written slot (all-zero sector
                                       * flags = every vertex killed). */
    uint32_t      present_ctr;
    tc_defer      defer[TC_DEFER_MAX];
    int           defer_n;
    uint32_t      draws_last;   /* accumulating (this present) */
    uint32_t      draws_prev;   /* completed previous present (census) */
} s_tc;

/* push-constant block (mirrors terrain.vert TerrainPC) */
typedef struct {
    float   x_base, x_span, y_base, y_span;
    float   disp_w, clip_h, band, freeze_wf;
    int32_t pass_sel, full_extent;
    int32_t pad[2];
} tc_pc;

int openpete_tercache_ready(void) { return s_tc.ready; }
int openpete_tercache_frame_open(void) { return s_tc.draw_open; }

int openpete_tercache_init(rhi_device *dev)
{
    memset(&s_tc, 0, sizeof(s_tc));
    s_tc.dev = dev;
    if (!dev) return 0;

    const rhi_pipeline_desc world_desc = {
        .vert_spv = "terrain.vert.spv", .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .push_const_size = sizeof(tc_pc),
    };
    rhi_pipeline_desc d;
    d = world_desc;
    s_tc.pipe_world = rhi_create_pipeline(dev, &d);
    d = world_desc; d.stencil = RHI_STENCIL_MARK;
    s_tc.pipe_world_mark = rhi_create_pipeline(dev, &d);
    d = world_desc; d.blend = RHI_BLEND_ALPHA; d.depth_write_off = true;
    s_tc.pipe_alpha = rhi_create_pipeline(dev, &d);
    d = world_desc; d.blend = RHI_BLEND_ADD; d.depth_write_off = true;
    s_tc.pipe_add = rhi_create_pipeline(dev, &d);
    d = world_desc; d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true;
    s_tc.pipe_sub = rhi_create_pipeline(dev, &d);
    d = world_desc; d.stencil = RHI_STENCIL_TEST_ZERO;
    s_tc.pipe_fill = rhi_create_pipeline(dev, &d);
    d = world_desc; d.frag_spv = "terrain_prepass.frag.spv"; d.color_write_off = true;
    s_tc.pipe_prepass = rhi_create_pipeline(dev, &d);
    d = world_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    s_tc.pipe_key = rhi_create_pipeline(dev, &d);
    /* Poly-anim variants: identical state, the anim vertex stage + the
     * binding-1 prev twin. No anim fill pipeline by design. */
    const rhi_pipeline_desc anim_desc = {
        .vert_spv = "terrain_anim.vert.spv", .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .terrain_anim = true,
        .push_const_size = sizeof(tc_pc),
    };
    d = anim_desc;
    s_tc.pipe_a_world = rhi_create_pipeline(dev, &d);
    d = anim_desc; d.stencil = RHI_STENCIL_MARK;
    s_tc.pipe_a_world_mark = rhi_create_pipeline(dev, &d);
    d = anim_desc; d.blend = RHI_BLEND_ALPHA; d.depth_write_off = true;
    s_tc.pipe_a_alpha = rhi_create_pipeline(dev, &d);
    d = anim_desc; d.blend = RHI_BLEND_ADD; d.depth_write_off = true;
    s_tc.pipe_a_add = rhi_create_pipeline(dev, &d);
    d = anim_desc; d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true;
    s_tc.pipe_a_sub = rhi_create_pipeline(dev, &d);
    d = anim_desc; d.frag_spv = "terrain_prepass.frag.spv"; d.color_write_off = true;
    s_tc.pipe_a_prepass = rhi_create_pipeline(dev, &d);
    d = anim_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    s_tc.pipe_a_key = rhi_create_pipeline(dev, &d);
    /* Near variants: identical state, the clip-fan vertex stage + the
     * binding-1 member-context twin. Includes FILL — near sectors keep the
     * LP hole-fill backing (unlike poly-anim, which is excluded from it). */
    const rhi_pipeline_desc near_desc = {
        .vert_spv = "terrain_near.vert.spv", .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .terrain_near = true,
        .push_const_size = sizeof(tc_pc),
    };
    d = near_desc;
    s_tc.pipe_n_world = rhi_create_pipeline(dev, &d);
    d = near_desc; d.stencil = RHI_STENCIL_MARK;
    s_tc.pipe_n_world_mark = rhi_create_pipeline(dev, &d);
    d = near_desc; d.blend = RHI_BLEND_ALPHA; d.depth_write_off = true;
    s_tc.pipe_n_alpha = rhi_create_pipeline(dev, &d);
    d = near_desc; d.blend = RHI_BLEND_ADD; d.depth_write_off = true;
    s_tc.pipe_n_add = rhi_create_pipeline(dev, &d);
    d = near_desc; d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true;
    s_tc.pipe_n_sub = rhi_create_pipeline(dev, &d);
    d = near_desc; d.stencil = RHI_STENCIL_TEST_ZERO;
    s_tc.pipe_n_fill = rhi_create_pipeline(dev, &d);
    d = near_desc; d.frag_spv = "terrain_prepass.frag.spv"; d.color_write_off = true;
    s_tc.pipe_n_prepass = rhi_create_pipeline(dev, &d);
    d = near_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    s_tc.pipe_n_key = rhi_create_pipeline(dev, &d);

    int ok = s_tc.pipe_world && s_tc.pipe_world_mark && s_tc.pipe_alpha &&
             s_tc.pipe_add && s_tc.pipe_sub && s_tc.pipe_fill && s_tc.pipe_prepass &&
             s_tc.pipe_key &&
             s_tc.pipe_a_world && s_tc.pipe_a_world_mark && s_tc.pipe_a_alpha &&
             s_tc.pipe_a_add && s_tc.pipe_a_sub && s_tc.pipe_a_prepass &&
             s_tc.pipe_a_key &&
             s_tc.pipe_n_world && s_tc.pipe_n_world_mark && s_tc.pipe_n_alpha &&
             s_tc.pipe_n_add && s_tc.pipe_n_sub && s_tc.pipe_n_fill &&
             s_tc.pipe_n_prepass && s_tc.pipe_n_key;
    for (int a = 0; ok && a < OPTC_ARENA_COUNT; a++) {
        s_tc.arena[a].cap = k_arena_cap[a];
        s_tc.arena[a].vb = rhi_create_vertex_buffer_device(
            dev, (size_t)k_arena_cap[a] * sizeof(rhi_terrain_vertex));
        if (!s_tc.arena[a].vb) ok = 0;
        if (ok && OPTC_ARENA_IS_ANIM(a)) {
            s_tc.arena[a].pv = rhi_create_vertex_buffer_device(
                dev, (size_t)k_arena_cap[a] * sizeof(rhi_terrain_prev));
            if (!s_tc.arena[a].pv) ok = 0;
        }
        if (ok && OPTC_ARENA_IS_NEAR(a)) {
            s_tc.arena[a].pv = rhi_create_vertex_buffer_device(
                dev, (size_t)k_arena_cap[a] * sizeof(rhi_terrain_nearctx));
            if (!s_tc.arena[a].pv) ok = 0;
        }
    }
    if (!ok) {
        fprintf(stderr, "[ter-gpu] init failed (pipeline or arena alloc) — "
                        "terrain cache disabled, CPU path stays\n");
        openpete_tercache_shutdown(dev);
        return 0;
    }
    /* Set-4 stand-in (init-time: uploads are illegal inside a render pass). */
    s_tc.hd_dummy = rhi_create_rgba8(dev, 1, 1);
    if (s_tc.hd_dummy) {
        const uint32_t px = 0;
        rhi_update_rgba8(dev, s_tc.hd_dummy, &px);
    }
    s_tc.ready = 1;
    return 1;
}

void openpete_tercache_shutdown(rhi_device *dev)
{
    if (!dev) return;
    if (s_tc.hd_dummy) { rhi_destroy_texture(dev, s_tc.hd_dummy); s_tc.hd_dummy = NULL; }
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        if (s_tc.arena[a].vb) { rhi_destroy_buffer(dev, s_tc.arena[a].vb); s_tc.arena[a].vb = NULL; }
        if (s_tc.arena[a].pv) { rhi_destroy_buffer(dev, s_tc.arena[a].pv); s_tc.arena[a].pv = NULL; }
    }
    if (s_tc.pipe_a_world)      rhi_destroy_pipeline(dev, s_tc.pipe_a_world);
    if (s_tc.pipe_a_world_mark) rhi_destroy_pipeline(dev, s_tc.pipe_a_world_mark);
    if (s_tc.pipe_a_alpha)      rhi_destroy_pipeline(dev, s_tc.pipe_a_alpha);
    if (s_tc.pipe_a_add)        rhi_destroy_pipeline(dev, s_tc.pipe_a_add);
    if (s_tc.pipe_a_sub)        rhi_destroy_pipeline(dev, s_tc.pipe_a_sub);
    if (s_tc.pipe_a_prepass)    rhi_destroy_pipeline(dev, s_tc.pipe_a_prepass);
    if (s_tc.pipe_a_key)        rhi_destroy_pipeline(dev, s_tc.pipe_a_key);
    if (s_tc.pipe_n_world)      rhi_destroy_pipeline(dev, s_tc.pipe_n_world);
    if (s_tc.pipe_n_world_mark) rhi_destroy_pipeline(dev, s_tc.pipe_n_world_mark);
    if (s_tc.pipe_n_alpha)      rhi_destroy_pipeline(dev, s_tc.pipe_n_alpha);
    if (s_tc.pipe_n_add)        rhi_destroy_pipeline(dev, s_tc.pipe_n_add);
    if (s_tc.pipe_n_sub)        rhi_destroy_pipeline(dev, s_tc.pipe_n_sub);
    if (s_tc.pipe_n_fill)       rhi_destroy_pipeline(dev, s_tc.pipe_n_fill);
    if (s_tc.pipe_n_prepass)    rhi_destroy_pipeline(dev, s_tc.pipe_n_prepass);
    if (s_tc.pipe_n_key)        rhi_destroy_pipeline(dev, s_tc.pipe_n_key);
    if (s_tc.pipe_world)      rhi_destroy_pipeline(dev, s_tc.pipe_world);
    if (s_tc.pipe_world_mark) rhi_destroy_pipeline(dev, s_tc.pipe_world_mark);
    if (s_tc.pipe_alpha)      rhi_destroy_pipeline(dev, s_tc.pipe_alpha);
    if (s_tc.pipe_add)        rhi_destroy_pipeline(dev, s_tc.pipe_add);
    if (s_tc.pipe_sub)        rhi_destroy_pipeline(dev, s_tc.pipe_sub);
    if (s_tc.pipe_fill)       rhi_destroy_pipeline(dev, s_tc.pipe_fill);
    if (s_tc.pipe_prepass)    rhi_destroy_pipeline(dev, s_tc.pipe_prepass);
    if (s_tc.pipe_key)        rhi_destroy_pipeline(dev, s_tc.pipe_key);
    if (s_tc.ubo)             rhi_destroy_ubo(dev, s_tc.ubo);
    memset(&s_tc, 0, sizeof(s_tc));
}

/* ---- allocator ---------------------------------------------------------- */

static void tc_free_insert(tc_arena *a, uint32_t first, uint32_t count)
{
    /* insert sorted + coalesce neighbours */
    int i = 0;
    while (i < a->free_n && a->free_list[i].first < first) i++;
    if (i > 0 && a->free_list[i-1].first + a->free_list[i-1].count == first) {
        a->free_list[i-1].count += count;
        if (i < a->free_n && a->free_list[i-1].first + a->free_list[i-1].count == a->free_list[i].first) {
            a->free_list[i-1].count += a->free_list[i].count;
            memmove(&a->free_list[i], &a->free_list[i+1],
                    (size_t)(a->free_n - i - 1) * sizeof(tc_span));
            a->free_n--;
        }
        return;
    }
    if (i < a->free_n && first + count == a->free_list[i].first) {
        a->free_list[i].first = first;
        a->free_list[i].count += count;
        return;
    }
    if (a->free_n >= TC_FREE_MAX) return;   /* span leaked until level reset */
    memmove(&a->free_list[i+1], &a->free_list[i],
            (size_t)(a->free_n - i) * sizeof(tc_span));
    a->free_list[i].first = first;
    a->free_list[i].count = count;
    a->free_n++;
}

int openpete_tercache_chunk_alloc(int arena, int vert_count, uint32_t *out_first)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT || vert_count <= 0)
        return -1;
    tc_arena *a = &s_tc.arena[arena];
    for (int i = 0; i < a->free_n; i++) {           /* first fit */
        if (a->free_list[i].count >= (uint32_t)vert_count) {
            *out_first = a->free_list[i].first;
            a->free_list[i].first += (uint32_t)vert_count;
            a->free_list[i].count -= (uint32_t)vert_count;
            if (a->free_list[i].count == 0) {
                memmove(&a->free_list[i], &a->free_list[i+1],
                        (size_t)(a->free_n - i - 1) * sizeof(tc_span));
                a->free_n--;
            }
            a->used += (uint32_t)vert_count; a->chunks++;
            return 0;
        }
    }
    if (a->high + (uint32_t)vert_count <= a->cap) {  /* grow high-water */
        *out_first = a->high;
        a->high += (uint32_t)vert_count;
        a->used += (uint32_t)vert_count; a->chunks++;
        return 0;
    }
    return -1;   /* full: caller claims the sector back to the CPU walk */
}

void openpete_tercache_chunk_write(int arena, uint32_t first,
                                   const rhi_terrain_vertex *v, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT) return;
    rhi_update_buffer_range(s_tc.arena[arena].vb, v,
                            (size_t)first * sizeof(rhi_terrain_vertex),
                            (size_t)count * sizeof(rhi_terrain_vertex));
}

void openpete_tercache_chunk_write_prev(int arena, uint32_t first,
                                        const rhi_terrain_prev *p, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT) return;
    if (!OPTC_ARENA_IS_ANIM(arena) || !s_tc.arena[arena].pv) return;
    rhi_update_buffer_range(s_tc.arena[arena].pv, p,
                            (size_t)first * sizeof(rhi_terrain_prev),
                            (size_t)count * sizeof(rhi_terrain_prev));
}

void openpete_tercache_chunk_write_ctx(int arena, uint32_t first,
                                       const rhi_terrain_nearctx *c, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT) return;
    if (!OPTC_ARENA_IS_NEAR(arena) || !s_tc.arena[arena].pv) return;
    rhi_update_buffer_range(s_tc.arena[arena].pv, c,
                            (size_t)first * sizeof(rhi_terrain_nearctx),
                            (size_t)count * sizeof(rhi_terrain_nearctx));
}

void openpete_tercache_chunk_free(int arena, uint32_t first, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT || count <= 0) return;
    tc_arena *a = &s_tc.arena[arena];
    /* No in-place dead-stamp: a replaced chunk keeps its bytes untouched (a
     * host write would race in-flight frames reading the arena). Presents
     * that still show the pre-rebuild state draw it via its baked chunk
     * GENERATION (face_flags bits 29-31 vs the published slot generation);
     * newer presents kill it the same way. The range itself is reused only
     * after the defer window below. */
    a->used -= (uint32_t)count; a->chunks--;
    /* Defer the RANGE's reuse past the frames-in-flight window. */
    if (s_tc.defer_n < TC_DEFER_MAX) {
        s_tc.defer[s_tc.defer_n++] = (tc_defer){
            (uint8_t)arena, first, (uint32_t)count, s_tc.present_ctr };
    } else {
        tc_free_insert(a, first, (uint32_t)count);   /* overflow: reuse now (rare) */
    }
}

/* ---- per-present protocol ---------------------------------------------- */

/* Shared prologue for frame_begin/frame_skip: pick the build target and run
 * the promote rule. A hook (canonical) build stages; the PREVIOUS staged
 * state promotes to live (that is the extraction the next presents show).
 * An immediate (re-extract) build writes live directly. A repeated stamp
 * (the serial-retry path re-runs one build) restages in place. */
static tc_state *tc_begin_state(uint32_t stamp, int immediate)
{
    if (immediate) {
        /* The staged canonical state SURVIVES: an intermediate re-extract
         * borrows live for its own present, and the next hook build's
         * promote restores the canonical timeline. */
        s_tc.live.stamp = stamp; s_tc.live.valid = 1;
        return &s_tc.live;
    }
    if (s_tc.staged.valid && s_tc.staged.stamp != stamp)
        s_tc.live = s_tc.staged;           /* promote the previous build */
    s_tc.staged.stamp = stamp; s_tc.staged.valid = 1;
    return &s_tc.staged;
}

void openpete_tercache_frame_begin(const openpete_tercache_globals *g,
                                   uint32_t stamp, int immediate)
{
    if (!s_tc.ready || !g) return;
    s_tc.present_ctr++;
    s_tc.draws_prev = s_tc.draws_last;
    s_tc.draws_last = 0;
    /* release deferred ranges older than the in-flight window */
    int w = 0;
    for (int i = 0; i < s_tc.defer_n; i++) {
        if (s_tc.present_ctr - s_tc.defer[i].stamp > TC_DEFER_PRESENTS) {
            /* Dead-stamp BEFORE the range becomes reusable. Every draw walks
             * the whole arena and selects per vertex by 3-bit generation, so
             * a released range that lingers unreused (release != reuse; the
             * allocator can go elsewhere for many builds) still rasterises
             * whenever a later state's generation aliases its own mod 8 — a
             * sector that rebuilds every build (animated vertex lighting)
             * then z-fights its current chunk with a pulse-phase-old twin.
             * The defer window just guaranteed no in-flight frame reads
             * these bytes, so overwriting here is the one race-free moment
             * (stamping at free time was the July hazard). Slot DEAD is
             * never published, so SF_DRAWN kills the vertex forever. */
            static const rhi_terrain_vertex k_dead = { .face_flags = OPTC_SLOT_DEAD };
            static rhi_terrain_vertex s_deadbuf[512];
            if (s_deadbuf[0].face_flags != OPTC_SLOT_DEAD)
                for (int d = 0; d < 512; d++) s_deadbuf[d] = k_dead;
            uint32_t left = s_tc.defer[i].count, at = s_tc.defer[i].first;
            while (left) {
                uint32_t n = left > 512 ? 512 : left;
                rhi_update_buffer_range(s_tc.arena[s_tc.defer[i].arena].vb,
                                        s_deadbuf,
                                        (size_t)at * sizeof(rhi_terrain_vertex),
                                        (size_t)n * sizeof(rhi_terrain_vertex));
                at += n; left -= n;
            }
            tc_free_insert(&s_tc.arena[s_tc.defer[i].arena],
                           s_tc.defer[i].first, s_tc.defer[i].count);
        } else
            s_tc.defer[w++] = s_tc.defer[i];
    }
    s_tc.defer_n = w;

    /* lazily create the UBO (needs the device from init) */
    if (!s_tc.ubo) {
        uint32_t block = (uint32_t)TC_UBO_BLOCK;
        s_tc.ubo = rhi_create_ubo(s_tc.dev, block, 2);
        if (!s_tc.ubo) { s_tc.ready = 0; return; }
    }
    /* STAGE into host structs only: the feeder runs outside the RHI frame,
     * and an outside-frame rhi_ubo_write fills every ring slot — racing an
     * in-flight frame's read. The live blocks flush on the present's first
     * draw (inside the frame, current slot only). */
    tc_state *T = tc_begin_state(stamp, immediate);
    tc_globals_ubo *gu = &T->g;
    memset(gu, 0, sizeof(*gu));
    for (int k = 0; k < 3; k++) {
        gu->mrow0[k] = g->mrow[0][k];
        gu->mrow1[k] = g->mrow[1][k];
        gu->mrow2[k] = g->mrow[2][k];
    }
    gu->dcam_xy[0] = g->camx; gu->dcam_xy[1] = g->camy;
    gu->dcam_zf[0] = g->camz; gu->dcam_zf[1] = g->fogscale;
    gu->dof_xy[0]  = g->ofx;  gu->dof_xy[1]  = g->ofy;
    gu->dH[0]      = g->H;
    gu->dH[1]      = g->vanim_alpha;   /* poly-anim lerp alpha (anim stage) */
    gu->dnear[0]   = g->nearz;         /* near-plane clip zc (near stage)   */
    gu->fog[0] = g->fog_ref_base;
    gu->fog[1] = (int32_t)g->gflags;
    gu->fog[2] = g->cull_ref_s2;
    memcpy(gu->sdv, g->scroll_dv, sizeof(gu->sdv));
    gu->rim_cnt[0]  = g->rim_n;
    gu->rim_oxyH[0] = g->rim_ofx;
    gu->rim_oxyH[1] = g->rim_ofy;
    gu->rim_oxyH[2] = g->rim_H;
    memcpy(gu->rim_knobs, g->rim_knobs, sizeof(gu->rim_knobs));
    memcpy(gu->rim_abc,   g->rim_abc,   sizeof(gu->rim_abc));
    memcpy(gu->rim_O,     g->rim_O,     sizeof(gu->rim_O));
    memcpy(gu->rim_U,     g->rim_U,     sizeof(gu->rim_U));
    memcpy(gu->rim_W,     g->rim_W,     sizeof(gu->rim_W));
    memcpy(gu->rim_nrm,   g->rim_nrm,   sizeof(gu->rim_nrm));
    memcpy(gu->rim_bbox,  g->rim_bbox,  sizeof(gu->rim_bbox));
    memcpy(gu->rim_pv,    g->rim_pv,    sizeof(gu->rim_pv));
    {   /* HD-fold entry table + continuous-fog ramp (level-lifetime state,
         * snapshotted per build like the rest of the globals). */
        int n = 0; float lad[2];
        const void *tab = openpete_native_hdfold_table(&n, lad);
        gu->hd_lad[0] = lad[0]; gu->hd_lad[1] = lad[1];
        gu->hd_lad[2] = (float)n;
        gu->hd_lad[3] = n > 0 ? 1.0f : 0.0f;
        if (n > 0)
            memcpy(gu->hd_entry, tab, (size_t)n * sizeof(gu->hd_entry[0]));
    }
    gu->cov_cnt[0]   = g->cov_n;
    gu->cov_margin[0] = g->cov_margin;
    for (int pi = 0; pi < 6; pi++) {
        gu->cov_abc[pi][0] = g->cov_abc[pi][0];
        gu->cov_abc[pi][1] = g->cov_abc[pi][1];
        gu->cov_abc[pi][2] = g->cov_abc[pi][2];
        gu->cov_abc[pi][3] = (double)g->cov_pn[pi];
        memcpy(gu->cov_bbox[pi], g->cov_bbox[pi], sizeof(gu->cov_bbox[pi]));
        for (int k = 0; k < 16; k++) {
            gu->cov_poly[pi * 16 + k][0] = g->cov_pxy[pi][k][0];
            gu->cov_poly[pi * 16 + k][1] = g->cov_pxy[pi][k][1];
        }
    }

    /* reset sector flags; origins are rewritten by sector_set */
    for (int i = 0; i < OPTC_SLOTS; i++) T->sec[i].o[3] = 0.0f;
    s_tc.build_target = T->sec;
}

void openpete_tercache_frame_skip(uint32_t stamp, int immediate)
{
    /* A pure-CPU fallback build still occupies its slot in the promote
     * chain with an all-claimed (empty) state, so the present that renders
     * this build's scene draws no retained content. */
    if (!s_tc.ready) return;
    tc_state *T = tc_begin_state(stamp, immediate);
    memset(&T->g, 0, sizeof(T->g));
    for (int i = 0; i < OPTC_SLOTS; i++) T->sec[i].o[3] = 0.0f;
    s_tc.build_target = T->sec;
}

/* Called once per rendered present: open the retained draws only when the
 * LIVE state is fresh relative to the present (a stale live state — menus,
 * level transitions, a stopped feeder — must not repaint old terrain). */
void openpete_tercache_frame_select(uint32_t present_stamp)
{
    /* present_stamp = the stamp of the SCENE being composed. Prefer an EXACT
     * stamp match so the retained terrain is the same extraction as the CPU
     * tris it composes with: live first (the lagged screenshot compose and
     * the sub-tick spine's immediate publishes both land here), then staged
     * (a depth-0 compose — the capture freeze-backdrop snapshot, a spine-off
     * windowed present — renders the CURRENT scene, whose terrain build sits
     * in staged until the next build's promote; drawing live there pairs
     * scene N with terrain N-1, and the camera moves between extractions, so
     * the skew bakes a whole-scene rim into the frozen backdrop). Only then
     * fall back to a small-tolerance live pick: modest skews are normal
     * (pause-entry tail feeds), while a large gap means the feeder stopped
     * (menus, transitions) and the retained state must not repaint. */
    if (s_tc.ready && s_tc.live.valid && present_stamp == s_tc.live.stamp) {
        s_tc.draw_open = 1; s_tc.sel = s_tc.live;
    } else if (s_tc.ready && s_tc.staged.valid && present_stamp == s_tc.staged.stamp) {
        s_tc.draw_open = 1; s_tc.sel = s_tc.staged;
    } else {
        int32_t age = (int32_t)(present_stamp - s_tc.live.stamp);
        s_tc.draw_open = s_tc.ready && s_tc.live.valid && age >= -8 && age <= 8;
        if (s_tc.draw_open) s_tc.sel = s_tc.live;
    }
    s_tc.ubo_flush_serial = 0;
}

void openpete_tercache_sector_set(int slot, float ox, float oy, float oz,
                                  uint32_t sflags, const float *grade6)
{
    if (!s_tc.build_target || slot < 0 || slot >= OPTC_SLOTS || slot == (int)OPTC_SLOT_DEAD)
        return;
    union { float f; uint32_t u; } w;
    w.u = sflags;
    s_tc.build_target[slot].o[0] = ox;
    s_tc.build_target[slot].o[1] = oy;
    s_tc.build_target[slot].o[2] = oz;
    s_tc.build_target[slot].o[3] = w.f;
    if (grade6) {
        for (int c = 0; c < 3; c++) {
            s_tc.build_target[slot].gp[c] = grade6[c];
            s_tc.build_target[slot].gq[c] = grade6[3 + c];
        }
        s_tc.build_target[slot].gp[3] = s_tc.build_target[slot].gq[3] = 0.0f;
    }
}

void openpete_tercache_frame_commit(void)
{
    /* Build staging is complete; promotion happens at the next frame_begin
     * (hook builds) and the UBO flush at the present's first draw. */
    s_tc.build_target = NULL;
}

/* ---- draw --------------------------------------------------------------- */

/* OPENPETE_TERRAIN_DUP_DRAW=N (dev probe): issue every arena draw N times.
 * Pricing the material design — a partial material claim costs one extra
 * full-arena pass per material, and the question is whether the retained
 * vertex stage is cheap enough for that to be free. N=2 is the upper bound
 * (the duplicate also re-runs the FRAGMENT stage, which a real material
 * pass mostly would not). */
static int tc_dup_draw(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("OPENPETE_TERRAIN_DUP_DRAW");
        n = (e && *e) ? atoi(e) : 1;
        if (n < 1) n = 1;
        if (n > 8) n = 8;
    }
    return n;
}

static void tc_draw_arena(int arena, int pass_sel, const openpete_tercache_clip *c)
{
    tc_arena *a = &s_tc.arena[arena];
    if (a->high == 0) return;
    tc_pc pc;
    memset(&pc, 0, sizeof(pc));
    pc.x_base = c->x_base; pc.x_span = c->x_span;
    pc.y_base = c->y_base; pc.y_span = c->y_span;
    pc.disp_w = c->disp_w; pc.clip_h = c->clip_h;
    pc.band = c->band; pc.freeze_wf = c->freeze_wf;
    pc.pass_sel = pass_sel; pc.full_extent = c->full_extent;
    rhi_push_constants(s_tc.dev, &pc, sizeof(pc));
    if (a->pv) rhi_bind_vertex_buffer2(s_tc.dev, a->vb, a->pv);
    else       rhi_bind_vertex_buffer(s_tc.dev, a->vb);
    for (int r = 0, rn = tc_dup_draw(); r < rn; r++)
        rhi_draw(s_tc.dev, a->high, 0);
    s_tc.draws_last++;
}

/* Draw a twin arena (poly-anim or near variant): rebind that variant of the
 * slot's pipeline (same state, a different vertex stage + binding-1 twin),
 * then restore nothing — the caller sequences all draws for one slot and
 * every arena draw re-pushes its own constants and buffers. */
static void tc_draw_anim(rhi_pipeline *p, int arena, int pass_sel,
                         const openpete_tercache_clip *c)
{
    if (!p || s_tc.arena[arena].high == 0) return;
    rhi_bind_pipeline(s_tc.dev, p);
    tc_draw_arena(arena, pass_sel, c);
}

void openpete_tercache_draw(int which, const openpete_tercache_clip *clip,
                            rhi_texture *vram, int use_mark,
                            rhi_texture *key_hp, rhi_texture *key_prop)
{
    if (!s_tc.ready || !s_tc.draw_open || !s_tc.ubo) return;
    rhi_pipeline *p = NULL;
    switch (which) {
        case OPTC_DRAW_WORLD:   p = use_mark ? s_tc.pipe_world_mark : s_tc.pipe_world; break;
        case OPTC_DRAW_ALPHA:   p = s_tc.pipe_alpha;   break;
        case OPTC_DRAW_ADD:     p = s_tc.pipe_add;     break;
        case OPTC_DRAW_SUB:     p = s_tc.pipe_sub;     break;
        case OPTC_DRAW_FILL:    p = s_tc.pipe_fill;    break;
        case OPTC_DRAW_PREPASS: p = s_tc.pipe_prepass; break;
        case OPTC_DRAW_KEY:     p = s_tc.pipe_key;     break;
    }
    if (!p) return;
    {   /* First draw of each RHI FRAME: flush the selected state into this
         * frame's UBO ring slot (see ubo_flush_serial — one selection can
         * render across several rhi frames). */
        uint64_t fs = rhi_frame_serial(s_tc.dev);
        if (s_tc.ubo_flush_serial != fs) {
            rhi_ubo_write(s_tc.ubo, 0, &s_tc.sel.g, sizeof(s_tc.sel.g));
            rhi_ubo_write(s_tc.ubo, 1, s_tc.sel.sec, sizeof(s_tc.sel.sec));
            s_tc.ubo_flush_serial = fs;
        }
    }
    static const char *k_zone[] = { "ter.world", "ter.alpha", "ter.add",
                                    "ter.sub", "ter.fill", "ter.prepass",
                                    "ter.key" };
    rhi_gpu_zone_begin(s_tc.dev,
                       (which >= 0 && which <= OPTC_DRAW_KEY) ? k_zone[which]
                                                             : "ter.gpu");
    rhi_bind_pipeline(s_tc.dev, p);
    if (vram) rhi_bind_vram(s_tc.dev, vram);
    /* og key contest inputs (scene slots only; same bind-at-pipe discipline
     * as bind_scene_pipe — absent images leave vKoff==0 fragments alone). */
    if (key_hp)   rhi_bind_depth_sampler(s_tc.dev, key_hp, 1);
    if (key_prop) rhi_bind_depth_sampler(s_tc.dev, key_prop, 2);
    {   /* set 4: the HD-fold atlas. terrain.frag references it statically,
         * so the init-time 1x1 dummy stands in whenever no fold entry
         * exists (vHd == 0 fragments never sample it). */
        rhi_texture *at = openpete_native_hdfold_atlas();
        if (!at) at = s_tc.hd_dummy;
        if (at) rhi_bind_texture_set(s_tc.dev, at, 4);
    }
    /* Sets 5/6, not the contract set: set 3 binding 1 belongs to a mod's own
     * uniform block, and a descriptor set views one buffer per binding — so
     * the terrain blocks cannot sit beside it. */
    rhi_ubo_bind_at(s_tc.dev, s_tc.ubo, 5, 0);
    rhi_ubo_bind_at(s_tc.dev, s_tc.ubo, 6, 1);
    switch (which) {
        case OPTC_DRAW_WORLD:
            tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     0, clip);
            tc_draw_anim(use_mark ? s_tc.pipe_a_world_mark : s_tc.pipe_a_world,
                         OPTC_ARENA_A_OPAQUE, 0, clip);
            tc_draw_anim(use_mark ? s_tc.pipe_a_world_mark : s_tc.pipe_a_world,
                         OPTC_ARENA_A_LP,     0, clip);
            tc_draw_anim(use_mark ? s_tc.pipe_n_world_mark : s_tc.pipe_n_world,
                         OPTC_ARENA_N_OPAQUE, 0, clip);
            tc_draw_anim(use_mark ? s_tc.pipe_n_world_mark : s_tc.pipe_n_world,
                         OPTC_ARENA_N_LP,     0, clip);
            break;
        case OPTC_DRAW_ALPHA:
            tc_draw_arena(OPTC_ARENA_ALPHA, 0, clip);
            tc_draw_anim(s_tc.pipe_a_alpha, OPTC_ARENA_A_ALPHA, 0, clip);
            tc_draw_anim(s_tc.pipe_n_alpha, OPTC_ARENA_N_ALPHA, 0, clip);
            break;
        case OPTC_DRAW_ADD:
            tc_draw_arena(OPTC_ARENA_ADD,   0, clip);
            tc_draw_anim(s_tc.pipe_a_add, OPTC_ARENA_A_ADD, 0, clip);
            tc_draw_anim(s_tc.pipe_n_add, OPTC_ARENA_N_ADD, 0, clip);
            break;
        case OPTC_DRAW_SUB:
            tc_draw_arena(OPTC_ARENA_SUB,   0, clip);
            tc_draw_anim(s_tc.pipe_a_sub, OPTC_ARENA_A_SUB, 0, clip);
            tc_draw_anim(s_tc.pipe_n_sub, OPTC_ARENA_N_SUB, 0, clip);
            break;
        case OPTC_DRAW_FILL:
            /* LP only: poly-anim sectors never backstop-fill, near ones do. */
            tc_draw_arena(OPTC_ARENA_LP,    1, clip);
            tc_draw_anim(s_tc.pipe_n_fill, OPTC_ARENA_N_LP, 1, clip);
            break;
        case OPTC_DRAW_PREPASS:
            tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     1, clip);
            tc_draw_anim(s_tc.pipe_a_prepass, OPTC_ARENA_A_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.pipe_a_prepass, OPTC_ARENA_A_LP,     0, clip);
            tc_draw_anim(s_tc.pipe_n_prepass, OPTC_ARENA_N_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.pipe_n_prepass, OPTC_ARENA_N_LP,     0, clip);
            tc_draw_anim(s_tc.pipe_n_prepass, OPTC_ARENA_N_LP,     1, clip);
            break;
        case OPTC_DRAW_KEY:
            /* keyed opaque faces only (blend arenas contest but never emit;
             * the vertex stage kills koff<=0.25 and fill-pass faces). */
            tc_draw_arena(OPTC_ARENA_OPAQUE, 2, clip);
            tc_draw_arena(OPTC_ARENA_LP,     2, clip);
            tc_draw_anim(s_tc.pipe_a_key, OPTC_ARENA_A_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.pipe_a_key, OPTC_ARENA_A_LP,     2, clip);
            tc_draw_anim(s_tc.pipe_n_key, OPTC_ARENA_N_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.pipe_n_key, OPTC_ARENA_N_LP,     2, clip);
            break;
    }
    rhi_gpu_zone_end(s_tc.dev);
}

void openpete_tercache_get_stats(openpete_tercache_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        out->chunks     += s_tc.arena[a].chunks;
        out->verts_used += s_tc.arena[a].used;
        out->verts_cap  += s_tc.arena[a].cap;
    }
    out->draws_last_present = s_tc.draws_prev;
    out->frees_pending = (uint32_t)s_tc.defer_n;
}
