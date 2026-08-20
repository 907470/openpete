/* openpete_terrain_cache.h — the terrain sector-mesh GPU cache (core side).
 *
 * Renderer-generic half of the retained-terrain path
 * (notes/plans/openpete-terrain-gpu-cache.md): per-class vertex ARENAS in
 * device-local memory, the terrain pipelines (terrain.vert/.frag), the
 * globals + sector-params UBO, chunk alloc/free with in-flight-safe deferred
 * reuse, and the per-present draw entry points native_render.c calls at the
 * WORLD / blend / fill / prepass slots.
 *
 * The game-specific half (decoding Spyro sector data into
 * rhi_terrain_vertex chunks, deciding per-sector claims and flags) lives in
 * games/spyro-1/features/terrain_native.c.
 *
 * Ownership contract: a sector is rendered by the GPU path IFF its slot was
 * openpete_tercache_sector_set() this present with OPTC_SF_DRAWN — the CPU
 * walk must then skip its face loop entirely. Sector slot 511 is RESERVED as
 * the dead-chunk stamp target and is never drawn (the builder claims it). */
#ifndef OPENPETE_TERRAIN_CACHE_H
#define OPENPETE_TERRAIN_CACHE_H

#include <stdint.h>

/* Forward decls only (full definitions in rhi.h) — this header is included
 * by targets that don't carry the RHI include path (knob registry test), so
 * every RHI type appears here as a plain struct-tag pointer. */
struct rhi_device;
struct rhi_texture;
struct rhi_terrain_vertex;
struct rhi_terrain_prev;

/* Vertex arenas, one per draw class. OPAQUE carries every HP face (textured
 * or flat, incl. HQ-subdivide variants and blend-4 "B+F/4" faces — the scene
 * clamps blend 4 to opaque); ALPHA/ADD/SUB the ABE blend classes 1..3; LP
 * the untextured low-poly tier (drawn in WORLD and re-drawn stencil-gated in
 * FILL). */
enum {
    OPTC_ARENA_OPAQUE = 0,
    OPTC_ARENA_ALPHA,
    OPTC_ARENA_ADD,
    OPTC_ARENA_SUB,
    OPTC_ARENA_LP,
    /* Poly-anim twins: same draw classes, small arenas whose vertices carry
     * a PARALLEL rhi_terrain_prev buffer (previous-tick position + corners);
     * drawn with the terrain_anim pipelines, which lerp prev->cur at the
     * build's sub-tick alpha. Anim LP is drawn in WORLD (and PREPASS) but
     * NEVER in FILL — poly-anim sectors are excluded from the hole-fill
     * backing (a stale LP bank must not backstop-fill). */
    OPTC_ARENA_A_OPAQUE,
    OPTC_ARENA_A_ALPHA,
    OPTC_ARENA_A_ADD,
    OPTC_ARENA_A_SUB,
    OPTC_ARENA_A_LP,
    OPTC_ARENA_COUNT
};
#define OPTC_ARENA_ANIM_BASE OPTC_ARENA_A_OPAQUE   /* base + class = twin */
#define OPTC_ARENA_IS_ANIM(a) ((a) >= OPTC_ARENA_A_OPAQUE)

/* Draw slots — native_render.c calls these at the matching point of its
 * draw order. WORLD = opaque arena + LP arena (pass WORLD); FILL = LP arena
 * (pass FILL, stencil TEST_ZERO); ALPHA/ADD/SUB = the blend arenas;
 * PREPASS = depth-only opaque + LP (world & fill) into the occlusion
 * pre-pass target. */
enum {
    OPTC_DRAW_WORLD = 0,
    OPTC_DRAW_ALPHA,
    OPTC_DRAW_ADD,
    OPTC_DRAW_SUB,
    OPTC_DRAW_FILL,
    OPTC_DRAW_PREPASS,
    OPTC_DRAW_KEY        /* og OT-key emit into the terrain key-depth image */
};

/* Sector flag bits (sector-params UBO; mirrored in terrain.vert).
 * Bits 12-14 carry the slot's chunk GENERATION (the vertex stage draws a
 * chunk only when its baked face_flags generation matches — that is what
 * lets a lagged present keep rendering the pre-rebuild chunk, and what
 * retires the old in-place dead-stamp). */
#define OPTC_SF_DRAWN      1u
#define OPTC_SF_DRAWHP     2u
#define OPTC_SF_DRAWLP     4u
#define OPTC_SF_LPFARONLY  8u
#define OPTC_SF_LPGAPONLY  16u
#define OPTC_SF_FILLLP     32u
#define OPTC_SF_FILLNEAR   64u
#define OPTC_SF_SECINLOD   128u

/* Global flag bits (globals UBO; mirrored in terrain.vert). */
#define OPTC_GF_NEARHQ     1u
#define OPTC_GF_CLUTFOG    2u
#define OPTC_GF_CULLHP     4u
#define OPTC_GF_HANDOFF    8u
#define OPTC_GF_GAPFILL    16u
#define OPTC_GF_NOLOD      32u
#define OPTC_GF_CULLLP     64u    /* LP per-half NCLIP cull (s_cull bit 1)    */
#define OPTC_GF_KEYON      128u   /* og OT-key emit/contest active this walk  */

/* face_flags bit layout lives with rhi_terrain_vertex in rhi.h. */
#define OPTC_SLOT_DEAD     511u
#define OPTC_SLOTS         512

/* Camera/projection/fog inputs are DOUBLE on purpose: terrain.vert re-runs
 * the CPU walk's double-precision GTE chain (fp64, `precise`) so the emitted
 * float screen coordinates round identically — float32 inputs alone would
 * already break the pixel-identical gate at the last-ulp level. */
typedef struct {
    float   mrow[3][3];   /* g_Camera+0x00 projection matrix, raw int16 values */
    double  ofx, ofy, H;  /* GTE screen offset + projection distance           */
    double  camx, camy, camz;  /* fractional camera, world/16                  */
    double  fogscale;     /* s_fogscale (SZ = fogscale * m3)                   */
    double  vanim_alpha;  /* poly-anim sub-tick lerp alpha (1.0 = canonical /
                           * unpaired build: the anim chunks draw cur exactly) */
    int32_t fog_ref_base; /* ENV_LODDIST >> 2                                  */
    int32_t cull_ref_s2;  /* ENV_CULLDIST >> 7 (.s LP far-cull threshold)      */
    uint32_t gflags;      /* OPTC_GF_*                                         */
} openpete_tercache_globals;

/* Init/teardown (native_render.c owns the device lifetime). init returns 0
 * and leaves the cache disabled on any pipeline/arena failure. */
int  openpete_tercache_init(struct rhi_device *dev);
void openpete_tercache_shutdown(struct rhi_device *dev);
int  openpete_tercache_ready(void);

/* Per-build protocol (driven by the terrain feeder). The sub-tick spine
 * presents the PREVIOUS canonical extraction (the canonical-lag leg), so
 * the retained state is double-buffered to match: a hook (canonical) build
 * STAGES its state (immediate=0) and the previous staged state promotes to
 * LIVE; a re-extracted present's build (immediate=1) writes LIVE directly —
 * its build is the displayed state. stamp = openpete_frame of the build.
 * frame_begin fills the target state's globals + zeroes its sector flags;
 * sector_set publishes one walked sector's verdicts into it; frame_commit
 * closes the build. frame_skip stages an all-claimed (empty) state for a
 * pure-CPU fallback build, so the present that renders that build's scene
 * draws no retained content. */
void openpete_tercache_frame_begin(const openpete_tercache_globals *g,
                                   uint32_t stamp, int immediate);
void openpete_tercache_sector_set(int slot, float ox, float oy, float oz,
                                  uint32_t sflags);
void openpete_tercache_frame_commit(void);
void openpete_tercache_frame_skip(uint32_t stamp, int immediate);
/* Renderer side, once per rendered present: gate the retained draws on the
 * LIVE state's freshness (a stale state — menus, level transitions, a
 * stopped feeder — must not repaint old terrain). frame_open() reports the
 * verdict to the draw sites. */
void openpete_tercache_frame_select(uint32_t present_stamp);
int  openpete_tercache_frame_open(void);

/* Provided by native_render.c: the renderer-side conditions ANDed (cache up,
 * no reference-compare delay, no dev tag instruments / PSX wobble, hardware
 * GPU). HD packs and material selectors are per-sector CLAIMS in the feeder,
 * not exclusions here. The feeder ANDs its own (flag, debug envs, portal
 * recency). */
int  openpete_native_tercache_usable(void);

/* Chunk management (builder side). alloc returns 0 on success and the first
 * vertex index in *out_first; write uploads packed vertices; free defers the
 * range's reuse by the frames-in-flight window and stamps its face_flags to
 * the dead slot so in-flight frames drop it. */
int  openpete_tercache_chunk_alloc(int arena, int vert_count, uint32_t *out_first);
void openpete_tercache_chunk_write(int arena, uint32_t first,
                                   const struct rhi_terrain_vertex *v, int count);
/* Anim arenas only: upload the parallel previous-tick records for a chunk
 * written at the same [first, first+count) range. */
void openpete_tercache_chunk_write_prev(int arena, uint32_t first,
                                        const struct rhi_terrain_prev *p, int count);
void openpete_tercache_chunk_free(int arena, uint32_t first, int count);

/* Screen->clip mapping for one draw target: the RAW scene_to_clip inputs,
 * so terrain.vert replicates the CPU's exact float expression term for term
 * (a fitted affine reconstruction loses ~5 subpixel LSBs at the screen
 * edges — enough to flip rasterized edge pixels vs the CPU-fed draws). */
typedef struct {
    float x_base, x_span;   /* cx = x_base + nx*x_span, nx = sx/disp_w      */
    float y_base, y_span;   /* cy = y_base - ny*y_span, ny = (sy-band)/clip_h */
    float disp_w, clip_h, band, freeze_wf;
    int   full_extent;      /* cx = (-1 + nx*2)/freeze_wf, cy = 1 - ny*2    */
} openpete_tercache_clip;

/* Draw entry (native_render.c). clip = the target's screen->clip mapping;
 * use_mark selects the stencil-MARK world variant (hole-fill enabled).
 * key_hp / key_prop: the terrain og-key depth image and the prop TRUE-depth
 * image for the fragment key contest (NULL = contest off — key pass down);
 * pass both for the scene slots (WORLD/blends), NULL for PREPASS/KEY. */
void openpete_tercache_draw(int which, const openpete_tercache_clip *clip,
                            struct rhi_texture *vram, int use_mark,
                            struct rhi_texture *key_hp, struct rhi_texture *key_prop);

/* Census (builder increments rebuild counts; the module tracks the rest). */
typedef struct {
    uint32_t chunks, verts_used, verts_cap;   /* summed over arenas */
    uint32_t draws_last_present;
    uint32_t frees_pending;
} openpete_tercache_stats_t;
void openpete_tercache_get_stats(openpete_tercache_stats_t *out);

#endif /* OPENPETE_TERRAIN_CACHE_H */
