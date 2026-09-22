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
 * games/spyro-1/renderer/terrain.c.
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
struct rhi_terrain_nearctx;

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
    /* Near-variant twins: same draw classes, for sectors whose geometry can
     * reach or cross the near plane. Every emitted triangle is baked TWICE
     * (a primary and a spare fan triangle, 6 vertices per source triangle),
     * each vertex carrying a PARALLEL rhi_terrain_nearctx record with the
     * other two members' UVs and colours; drawn with the terrain_near
     * pipelines, which re-run the walk's near-plane clip fan in the vertex
     * stage. A slot holds both its plain and its near chunks and the
     * per-present sector flags pick one (OPTC_SF_NEARVAR). */
    OPTC_ARENA_N_OPAQUE,
    OPTC_ARENA_N_ALPHA,
    OPTC_ARENA_N_ADD,
    OPTC_ARENA_N_SUB,
    OPTC_ARENA_N_LP,
    OPTC_ARENA_COUNT
};
#define OPTC_ARENA_ANIM_BASE OPTC_ARENA_A_OPAQUE   /* base + class = twin */
#define OPTC_ARENA_NEAR_BASE OPTC_ARENA_N_OPAQUE
#define OPTC_ARENA_IS_ANIM(a) ((a) >= OPTC_ARENA_A_OPAQUE && (a) < OPTC_ARENA_N_OPAQUE)
#define OPTC_ARENA_IS_NEAR(a) ((a) >= OPTC_ARENA_N_OPAQUE)

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
#define OPTC_SF_LPGRADE    256u   /* apply the per-sector pack LP grade (gp/gq) */
#define OPTC_SF_NEARVAR    512u   /* draw this sector's NEAR-VARIANT chunks (the
                                   * clip-fan vertex stage) instead of its plain
                                   * ones: exactly one of the two sets survives,
                                   * so a sector never draws twice */

/* Global flag bits (globals UBO; mirrored in terrain.vert). */
#define OPTC_GF_NEARHQ     1u
#define OPTC_GF_CLUTFOG    2u
#define OPTC_GF_CULLHP     4u
#define OPTC_GF_HANDOFF    8u
#define OPTC_GF_GAPFILL    16u
#define OPTC_GF_NOLOD      32u
#define OPTC_GF_CULLLP     64u    /* LP per-half NCLIP cull (s_cull bit 1)    */
#define OPTC_GF_KEYON      128u   /* og OT-key emit/contest active this walk  */
#define OPTC_GF_HQPIN      256u   /* extended LOD pins the HQ subdivide on every
                                   * eligible HP quad at every distance: the
                                   * walk's face_is_near extended branch      */
#define OPTC_GF_LADNEAR    512u   /* game_layout.terrain.ladder_nearest_corner:
                                   * the CLUT-fog row and near-tile select key
                                   * on the nearest corner (row = 15 - (max
                                   * ir0 >> 8)), not the four-corner mean */
#define OPTC_GF_NBKEYS     1024u  /* game_layout.terrain.nearband_ot_keys: the
                                   * near/HQ-classified faces key through the
                                   * ordinary OT insert (authored slots kept,
                                   * key emitted); clear = contest-only      */

/* face_flags bit layout lives with rhi_terrain_vertex in rhi.h. */
#define OPTC_SLOT_DEAD     511u
#define OPTC_SLOTS         512
/* Build stamps: a VBL counter, or a scene id from the eager scene ring. Scene
 * ids carry the top bit so the two spaces never collide. */
#define OPTC_STAMP_SCENE_BIT  (1u << 31)

/* Camera/projection/fog inputs are DOUBLE on purpose: terrain.vert re-runs
 * the CPU walk's double-precision GTE chain (fp64, `precise`) so the emitted
 * float screen coordinates round identically — float32 inputs alone would
 * already break the pixel-identical gate at the last-ulp level. */
typedef struct {
    float   mrow[3][3];   /* g_Camera+0x00 projection matrix, raw int16 values */
    double  ofx, ofy, H;  /* GTE screen offset + projection distance           */
    double  H_canon;      /* the GTE's H: the hardware-limit mirrors read it    */
    double  camx, camy, camz;  /* fractional camera, world/16                  */
    double  fogscale;     /* s_fogscale (SZ = fogscale * m3)                   */
    double  vanim_alpha;  /* poly-anim sub-tick lerp alpha (1.0 = canonical /
                           * unpaired build: the anim chunks draw cur exactly) */
    double  nearz;        /* near-plane camera-space view-Z (s_nearz): the
                           * near-variant vertex stage clips against it        */
    int32_t fog_ref_base; /* ENV_LODDIST >> 2                                  */
    int32_t cull_ref_s2;  /* ENV_CULLDIST >> 7 (.s LP far-cull threshold)      */
    uint32_t gflags;      /* OPTC_GF_*                                         */
    /* Per-tile fractional UV-scroll V offsets for THIS build (env texture
     * anim, sub-tick lerp of the 7-bit accumulator): [0] = LQ/flat-face dv,
     * [1]/[2] = HQ record entries 1-2 / 3-4 (the two ring halves), [3] pad.
     * All zero on canonical builds (whole-texel committed decode). Faces
     * carry their tile id in flags16; the vertex stage adds the offset. */
    float   scroll_dv[128][4];
    /* Portal inner-rim cull mirror (portal_rim_classify in the vertex
     * stage): the per-plane frames rim_frames_derive builds once per feed,
     * plus the classifier's knob values, all fp64. rim_n = 0 disables the
     * test (no active portal, or the rim-claim lever routes rim-candidate
     * sectors to the CPU walk instead). Plane pi's outline lives at
     * rim_pv[pi*16 .. pi*16+vn-1]; vn rides rim_nrm[pi][3], the frame's
     * validity rides rim_abc[pi][3]. */
    int32_t rim_n;
    double  rim_ofx, rim_ofy, rim_H;  /* rim_frames_derive's GTE constants  */
    double  rim_knobs[4];             /* eps, near, reach, cos(angle)       */
    double  rim_abc[6][4];            /* screen inv-vz plane fit a,b,c; w=valid */
    double  rim_O[6][4];              /* in-plane basis origin              */
    double  rim_U[6][4];              /* in-plane basis U                   */
    double  rim_W[6][4];              /* in-plane basis W                   */
    double  rim_nrm[6][4];            /* unit view-space normal; w = vn     */
    double  rim_bbox[6][4];           /* outline UV bbox pulo,puhi,pvlo,pvhi */
    double  rim_pv[96][2];            /* outlines in (U,W) coords, 16/plane */
    /* Blended-face portal-cover drop mirror (portal_sky_covers in the
     * vertex stage): the active opening planes in SCREEN space. The feed
     * drops any blended tri whose screen centroid falls inside an opening's
     * outline polygon with the tri's mean view-Z at or behind the plane
     * minus the cover margin; baked faces carry a blend bit (flags16 bit
     * 14) and the vertex stage kills them by the same float-exact test.
     * cov_n = 0 disables (no active portal, or the transparent-cover drop
     * lever is off — the feed then keeps such tris too). A plane whose
     * screen outline projection failed rides with cov_pn = 0 and never
     * matches, exactly like the CPU test's pn < 3 skip. */
    int32_t cov_n;
    double  cov_margin;               /* plane-cover near margin M (world)  */
    double  cov_abc[6][3];            /* screen inv-vz plane fit a, b, c    */
    int32_t cov_pn[6];                /* outline point count (0 = no poly)  */
    float   cov_bbox[6][4];           /* outline screen bbox x0, y0, x1, y1 */
    float   cov_pxy[6][16][2];        /* outline polygon, screen px/py      */
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
/* grade6: the pack-aware LP colour grade for this sector (p.rgb then q.rgb,
 * lp_grade_apply's affine — own fit or the level mean), or NULL when no pack
 * is active. Pair a non-NULL grade with OPTC_SF_LPGRADE in sflags. */
void openpete_tercache_sector_set(int slot, float ox, float oy, float oz,
                                  uint32_t sflags, const float *grade6);
void openpete_tercache_frame_commit(void);
void openpete_tercache_frame_skip(uint32_t stamp, int immediate);
/* Renderer side, once per rendered present: gate the retained draws on the
 * LIVE state's freshness (a stale state — menus, level transitions, a
 * stopped feeder — must not repaint old terrain). frame_open() reports the
 * verdict to the draw sites. */
void openpete_tercache_frame_select(uint32_t present_stamp);
int  openpete_tercache_frame_open(void);
/* Whether the selected state would paint anything: 1 = live sectors, 0 =
 * open but empty (the feeder skipped this scene's build), -1 = cache not
 * in use. The freeze-backdrop snapshot's "is this a world frame" test. */
int  openpete_tercache_frame_content(void);
int  openpete_tercache_frame_source(void);   /* 0 closed, 1 scene ring, 2 live, 3 staged, 4 live-by-age */
void openpete_tercache_frame_stats(int *drawn, int *hp, int *lp, int *nearvar, int gens[8]);
void openpete_tercache_frame_globals(char *out, int cap);
void openpete_tercache_frame_slots(char *out, int cap);

/* Provided by native_render.c: the renderer-side conditions ANDed (cache up,
 * no reference-compare delay, no dev tag instruments / PSX wobble, hardware
 * GPU). HD packs and material selectors are per-sector CLAIMS in the feeder,
 * not exclusions here. The feeder ANDs its own (flag, debug envs, portal
 * recency). */
int  openpete_native_tercache_usable(void);
/* Host loop, once per VBL: run the terrain cache's device service on the
 * headless path. Self-no-ops with a window, where the present entry runs it
 * on the thread that owns the device for that present. */
void openpete_native_tercache_service_headless(void);

/* ---- HD texture-replacement fold (pack art in the retained path) --------
 * Defined in native_render.c (owner of the pack-registry and pack-fog
 * helpers plus the frame upload point). The feeder's bake interns one entry
 * per emitted textured tri; the retained fragment stage samples the fold
 * atlas instead of the VRAM model when a face carries an entry id (upper
 * halves of the mat_far1/mat_near1 clut words). */
#define OPENPETE_HDFOLD_MAX 416   /* entry-table cap (globals-UBO budget) */

/* Master gate: OPENPETE_TERRAIN_HDFOLD=1, a pack active, and the pack-fog
 * models at their shipped defaults (the fold bakes those). Per-run latch. */
int  openpete_native_hdfold_on(void);

/* Intern the replacement verdict of one emitted tri (page texel corners).
 * Returns 0 = no replacement (draw-time resolve misses / no match too),
 * >0 = 1-based entry id, -1 = unbakeable (table/atlas full) — the caller
 * claims the sector back to the CPU walk. */
int  openpete_native_hdfold_intern(int tpx, int tpy, int clx, int cly, int bpp,
                                   const float us[3], const float vs[3]);

/* Entry-table snapshot for the globals fill: payload of
 * OPENPETE_HDFOLD_MAX x 4 vec4 (rect / atlas rect / fogA / fogB), the live
 * count, and the continuous-fog ramp (lad[0..1]; lad valid = ret). */
const void *openpete_native_hdfold_table(int *count, float lad[2]);

/* The fold atlas (NULL until the first intern) — the set-4 sampler. */
struct rhi_texture *openpete_native_hdfold_atlas(void);

/* The material UBO (native_render), whose block 0 is the engine block and
 * whose blocks 1+ are the interned per-material uniform blocks. A retained
 * material pass binds it at set 3, the same pair a walked material draw
 * binds — one shading contract, one uniform source, either residency.
 * NULL until the first override arms the batcher split. */
struct rhi_ubo *openpete_native_material_ubo(void);

/* Atlas uploads still queued (drained ~96 per rendered frame). */
int openpete_native_hdfold_pending(void);

/* Bumped on every intern and on a flush that drains the upload queue —
 * fold-overflow-claimed sectors re-bake when it moves. */
uint32_t openpete_native_hdfold_epoch(void);

/* Flush pending atlas uploads into the current rhi frame (after begin_frame,
 * before the first render pass — the hd_flush_pending site). */
void openpete_native_hdfold_flush(void);
/* Create + make resident the replacement atlas ahead of first use, so the
 * driver's allocation work does not land on a pacer-timed frame. */
void openpete_native_hdfold_prime(struct rhi_device *dev);

/* Drop table + atlas content (pack registry reset / level change); baked
 * entry ids die with it, so callers must also rebake hd-baked sectors. */
void openpete_native_hdfold_reset(void);

/* Chunk management (builder side). alloc returns 0 on success, the first
 * vertex index in *out_first and the arena buffer that index addresses in
 * *out_buf; every later call for that chunk repeats the buffer index, since
 * each buffer has its own vertex numbering from 0. write uploads packed
 * vertices; free defers the range's reuse by the frames-in-flight window and
 * stamps its face_flags to the dead slot so in-flight frames drop it.
 *
 * alloc returns -1 when every buffer of the arena is full. It then posts an
 * overflow request for that arena (see below), so the caller's claim-back to
 * the CPU walk lasts the presents the device thread needs to allocate. */
int  openpete_tercache_chunk_alloc(int arena, int vert_count, uint32_t *out_first,
                                   int *out_buf);
void openpete_tercache_chunk_write(int arena, int buf, uint32_t first,
                                   const struct rhi_terrain_vertex *v, int count);
/* Anim arenas only: upload the parallel previous-tick records for a chunk
 * written at the same [first, first+count) range. */
void openpete_tercache_chunk_write_prev(int arena, int buf, uint32_t first,
                                        const struct rhi_terrain_prev *p, int count);
/* Near arenas only: upload the parallel emitted-tri member context for a chunk
 * written at the same [first, first+count) range. */
void openpete_tercache_chunk_write_ctx(int arena, int buf, uint32_t first,
                                       const struct rhi_terrain_nearctx *c, int count);
void openpete_tercache_chunk_free(int arena, int buf, uint32_t first, int count);

/* ---- overflow buffers ---------------------------------------------------
 * An arena holds a primary buffer and, once it has run out, one overflow
 * buffer sized from the level's own measured capacity, or from the refused
 * chunk where that is larger, so one grant covers the level. The builder
 * posts the request from chunk_alloc, the device-owning thread allocates it
 * in present_service, and the builder installs it at its next frame_begin;
 * from then on chunk_alloc serves from the overflow once the primary is full
 * and the draw issues one rhi_draw per buffer. The vertex stage's only index
 * use is gl_VertexIndex % 3, which restarts at every draw, so a second buffer
 * needs no index translation and nothing is copied.
 *
 * One overflow per arena is outstanding at a time: a request raised while one
 * is pending or already installed is dropped, and an allocation that fails
 * logs once for that arena and stops the requests until the next level edge,
 * where the replacement collapses both buffers into one fresh primary sized
 * for the incoming level. */

/* ---- level-edge arena replacement ---------------------------------------
 * `high` never falls and a freed range returns to the free list only after
 * the defer window, so a session accumulates toward every arena cap across
 * level loads: the previous level's chunks are never released (their decode
 * slots are rebuilt only when another sector takes them) and the surviving
 * free spans re-fit the next level's chunk sizes imperfectly. The cure is
 * replacement, not reset in place: the defer window counts BUILDS and no
 * build runs during a load, so a range freed at the level edge can never
 * age out before the new level's first walk allocates over it.
 *
 * Three steps on two threads. The builder posts a request at the level
 * latch (request_resize) after giving up every slot's chunks. The
 * device-owning thread allocates one fresh {vb, pv} pair per arena
 * (present_service) — device objects are illegal on the builder's thread.
 * The builder adopts the pairs at its next frame_begin: the arena's
 * bookkeeping resets to high = 0 with an empty free list, and the outgoing
 * pairs go on a retire list that present_service destroys once both drain
 * floors have passed. Adopting resets every arena's counters, so a caller
 * holding chunk records frees them BEFORE the adopting frame_begin;
 * resize_ready reports that moment. */
void openpete_tercache_request_resize(int level_id);
int  openpete_tercache_resize_ready(void);
/* The level the arenas are being filled for, which is what sizes every buffer
 * the cache allocates from here on. Set at every level latch, including the
 * first of a session, where no replacement is owed. */
void openpete_tercache_set_level(int level_id);
/* Steps whenever an arena gains buffer room: a level-edge replacement or an
 * overflow buffer. A sector claimed back because an arena was full is baked
 * again when this moves, which is what keeps that claim a transient. */
uint32_t openpete_tercache_alloc_epoch(void);
/* Device-owning thread only, once per present windowed and once per VBL
 * headless: fulfil a pending request and destroy drained retire entries.
 * The only caller of rhi_destroy_buffer for arena buffers outside
 * shutdown. */
void openpete_tercache_present_service(void);

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
    /* Per-arena, in OPTC_ARENA_* order. `high` is the arena's high-water
     * vertex count summed over its buffers (the draw range, and what a cap
     * binds against); `cap` is the summed capacity of its live buffers; `alloc_fail` counts chunk_alloc's -1
     * returns, which are the sectors the builder claimed back to the CPU
     * walk because the arena was full. */
    uint32_t high[OPTC_ARENA_COUNT];
    uint32_t cap[OPTC_ARENA_COUNT];
    uint32_t alloc_fail[OPTC_ARENA_COUNT];
    /* Overflow buffers installed on each arena over the session. A non-zero
     * count beside a settled `alloc_fail` is the arena that ran out and then
     * grew; `high` and `cap` above sum the arena's buffers. */
    uint32_t overflow_adopted[OPTC_ARENA_COUNT];
    /* Device bytes the live buffers hold: every arena's capacity times its
     * vertex stride plus its twin's. */
    uint64_t cap_bytes;
    /* Level-edge replacement engagement: present_service calls and requests
     * fulfilled. Both zero means the service never ran, which makes every
     * arena figure below a pre-replacement figure. */
    uint32_t svc_calls, svc_fulfilled;
    /* Draws that bound a buffer pair already on the retire list. A
     * destroyed-while-drawn detector: the invariant is 0. */
    uint32_t draws_retired;
} openpete_tercache_stats_t;
void openpete_tercache_get_stats(openpete_tercache_stats_t *out);

#endif /* OPENPETE_TERRAIN_CACHE_H */
