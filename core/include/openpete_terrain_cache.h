/* openpete_terrain_cache.h - the terrain sector-mesh GPU cache (core side).
 *
 * Renderer-generic half of the retained-terrain path: per-class vertex ARENAS in
 * device-local memory, the terrain pipelines (terrain.vert/.frag), the
 * globals + sector-params UBO, chunk alloc/free with in-flight-safe deferred
 * reuse, and the per-present draw entry points native_render.c calls at the
 * WORLD / blend / fill / prepass slots.
 *
 * The game-specific half (decoding Spyro sector data into
 * rhi_terrain_vertex chunks, deciding per-sector claims and flags) lives in
 * games/common/renderer/terrain.c.
 *
 * Ownership contract: a sector is rendered by the GPU path IFF its slot was
 * openpete_tercache_sector_set() this present with OPTC_SF_DRAWN - the CPU
 * walk must then skip its face loop entirely. Sector slot 511 is RESERVED as
 * the dead-chunk stamp target and is never drawn (the builder claims it). */
#ifndef OPENPETE_TERRAIN_CACHE_H
#define OPENPETE_TERRAIN_CACHE_H

#include <stddef.h>
#include <stdint.h>

/* Forward decls only (full definitions in rhi.h) - this header is included
 * by targets that don't carry the RHI include path (knob registry test), so
 * every RHI type appears here as a plain struct-tag pointer. */
struct rhi_device;
struct rhi_texture;
struct rhi_terrain_vertex;
struct rhi_terrain_prev;
struct rhi_terrain_nearctx;
struct openpete_mod_cdata;

/* Vertex arenas, one per draw class. OPAQUE carries every HP face (textured
 * or flat, incl. HQ-subdivide variants and blend-4 "B+F/4" faces - the scene
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
     * NEVER in FILL - poly-anim sectors are excluded from the hole-fill
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
    /* 4x4 near-cell records (rhi_terrain_cell, one per half of a tier-4-
     * eligible HQ quad or triangle, drawn instanced by the terrain_cells
     * pipelines). The CN family belongs to near-variant sectors and draws
     * with their chunks (OPTC_SF_NEARVAR); the CA family
     * (rhi_terrain_cell_anim, previous corners appended) draws with the
     * plain chunk set: poly-anim sectors, and under OPTC_GF_HQPIN the
     * faces the pin sends to the cells in every sector (a non-anim sector's
     * previous corners equal its current ones). One arena per HP draw
     * class; no LP class, as cells are HP faces. These arenas count RECORDS,
     * not vertices. */
    OPTC_ARENA_CN_OPAQUE,
    OPTC_ARENA_CN_ALPHA,
    OPTC_ARENA_CN_ADD,
    OPTC_ARENA_CN_SUB,
    OPTC_ARENA_CA_OPAQUE,
    OPTC_ARENA_CA_ALPHA,
    OPTC_ARENA_CA_ADD,
    OPTC_ARENA_CA_SUB,
    OPTC_ARENA_COUNT
};
#define OPTC_ARENA_ANIM_BASE OPTC_ARENA_A_OPAQUE   /* base + class = twin */
#define OPTC_ARENA_NEAR_BASE OPTC_ARENA_N_OPAQUE
#define OPTC_ARENA_CELL_NEAR_BASE OPTC_ARENA_CN_OPAQUE   /* base + class */
#define OPTC_ARENA_CELL_ANIM_BASE OPTC_ARENA_CA_OPAQUE
#define OPTC_ARENA_IS_ANIM(a) ((a) >= OPTC_ARENA_A_OPAQUE && (a) < OPTC_ARENA_N_OPAQUE)
#define OPTC_ARENA_IS_NEAR(a) ((a) >= OPTC_ARENA_N_OPAQUE && (a) < OPTC_ARENA_CN_OPAQUE)
#define OPTC_ARENA_IS_CELL(a) ((a) >= OPTC_ARENA_CN_OPAQUE && (a) < OPTC_ARENA_COUNT)
#define OPTC_ARENA_IS_CELL_ANIM(a) ((a) >= OPTC_ARENA_CA_OPAQUE && (a) < OPTC_ARENA_COUNT)

/* Draw slots - native_render.c calls these at the matching point of its
 * draw order. WORLD = opaque arena + LP arena (pass WORLD); FILL = LP arena
 * (pass FILL, stencil TEST_ZERO); ALPHA/ADD/SUB = the blend arenas;
 * PREPASS = the WORLD faces (opaque + LP world pass) into the depth
 * pre-pass target, depth and packed-depth colour where terrain.frag paints;
 * PREPASS_DEPTH = the WORLD faces over their whole footprint and the
 * hole-fill faces (LP fill pass), depth only. */
enum {
    OPTC_DRAW_WORLD = 0,
    OPTC_DRAW_ALPHA,
    OPTC_DRAW_ADD,
    OPTC_DRAW_SUB,
    OPTC_DRAW_FILL,
    OPTC_DRAW_PREPASS,       /* depth pre-pass, WORLD faces: depth + packed depth in colour */
    OPTC_DRAW_PREPASS_DEPTH, /* depth pre-pass, WORLD footprints + hole-fill: depth only */
    OPTC_DRAW_KEY        /* og OT-key emit into the terrain key-depth image */
};

/* Sector flag bits (sector-params UBO; mirrored in terrain.vert).
 * Bits 12-14 carry the slot's chunk GENERATION (the vertex stage draws a
 * chunk only when its baked face_flags generation matches - that is what
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
#define OPTC_SF_LPUNCOV    1024u  /* the LP world pass draws only the faces
                                   * baked with the LP-uncovered face bit */
/* Bits 16-27: the sector's LP near-outcode SZ3 bound this walk (0 = no test). */
#define OPTC_SF_LPNEAR_SHIFT 16u
#define OPTC_SF_LPNEAR_MASK  0xFFFu

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
                                   * walk's face_is_near extended branch; with
                                   * OPTC_GF_CELLS4 / OPTC_GF_TRI4 a face baked
                                   * with face_flags bit 19 (tface_pin_cells)
                                   * and word 3 near_flag_tier2 clear takes the
                                   * 4x4 cells / 16 sub-triangles at every
                                   * distance (face_near_tier4 pinned branch) */
#define OPTC_GF_LADNEAR    512u   /* game_layout.terrain.ladder_nearest_corner:
                                   * the CLUT-fog row and near-tile select key
                                   * on the nearest corner (row = 15 - (max
                                   * ir0 >> 8)), not the four-corner mean */
#define OPTC_GF_NBKEYS     1024u  /* game_layout.terrain.nearband_ot_keys: the
                                   * near/HQ-classified faces key through the
                                   * ordinary OT insert (authored slots kept,
                                   * key emitted); clear = contest-only      */
#define OPTC_GF_CELLS4     2048u  /* game_layout.terrain.hq_cells4 != 0: a tier-4
                                   * face (a corner SZ3 under near_corner, word
                                   * 3 near_flag_tier2 clear) draws its 4x4
                                   * cell variant instead of the 2x2          */
#define OPTC_GF_TRI4       4096u  /* game_layout.terrain.hq_tri4_cells != 0: a
                                   * tier-4 triangle (the same verdict over
                                   * corners A, B, C, C) draws its 16
                                   * sub-triangle records instead of the flat
                                   * GT3                                      */
#define OPTC_GF_TRI2       8192u  /* game_layout.terrain.hq_tri2_tiles != 0: a
                                   * near triangle outside tier 4 draws its 4
                                   * tier-2 sub-triangle records instead of
                                   * the flat GT3                             */
#define OPTC_GF_LPNEAROUT  16384u /* game_layout.terrain.lp_near_sz != 0: the LP
                                   * near outcode (the sector flags' bound)
                                   * replaces the authored LP admission       */
#define OPTC_GF_HQFOG      32768u /* the HQ pin's far faces take the game's
                                   * depth cue: DPCS-fogged node colours and
                                   * the ladder fade of their HQ texels
                                   * (terrain.c tn_hqfog_face)                 */
#define OPTC_GF_HPRAW      65536u /* extended LOD (terrain.c s_hp_raw): an HP
                                   * face draws its base CLUT row and its near
                                   * colours at every distance, never the far
                                   * palette or a deeper ladder row           */
#define OPTC_GF_HPFOG      131072u /* with OPTC_GF_HPRAW, a heavy-fog level
                                   * (terrain.c s_hp_fog): HP corner colours
                                   * fade toward their own far colours on the
                                   * vertex depth-cue curve, texels toward
                                   * modulation-neutral grey                  */
#define OPTC_GF_FOGOFF     262144u /* fog mode off (terrain.c s_fog_mode): no
                                    * vertex fade, no ladder row, no water
                                    * fade                                     */
#define OPTC_GF_FOGPP      524288u /* per-pixel fog mode: laddered faces draw
                                    * the base row and pick the row per pixel;
                                    * heavy-fog texels fade on the ramp. With
                                    * neither bit the original form: the row
                                    * per face, the heavy-fog fade stepped per
                                    * face                                     */

/* face_flags bit layout lives with rhi_terrain_vertex in rhi.h. */
#define OPTC_SLOT_DEAD     511u
#define OPTC_SLOTS         512
/* Build stamps: a VBL counter, or a scene id from the eager scene ring. Scene
 * ids carry the top bit so the two spaces never collide. */
#define OPTC_STAMP_SCENE_BIT  (1u << 31)

/* Camera/projection/fog inputs are DOUBLE on purpose: terrain.vert re-runs
 * the CPU walk's double-precision GTE chain (fp64, `precise`) so the emitted
 * float screen coordinates round identically - float32 inputs alone would
 * already break the pixel-identical gate at the last-ulp level. */
typedef struct {
    float   mrow[3][3];   /* g_Camera+0x00 projection matrix, raw int16 values */
    double  ofx, ofy, H;  /* GTE screen offset + projection distance           */
    double  H_canon;      /* the GTE's H: the hardware-limit mirrors read it    */
    double  camx, camy, camz;  /* fractional camera, world/16                  */
    double  fogscale;     /* s_fogscale (SZ = fogscale * m3)                   */
    double  vanim_alpha;  /* poly-anim sub-tick lerp alpha (1.0 = canonical /
                           * unpaired build: the anim chunks draw cur exactly) */
    double  canim_alpha;  /* env-anim colour lerp alpha: the anim chunks'
                           * previous -> current colour words, 1.0 selects the
                           * current words (canonical and unpaired builds)     */
    double  nearz;        /* near-plane camera-space view-Z (s_nearz): the
                           * near-variant vertex stage clips against it        */
    int32_t fog_ref_base; /* ENV_LODDIST >> 2                                  */
    int32_t cull_ref_s2;  /* ENV_CULLDIST >> 7 (.s LP far-cull threshold)      */
    int32_t hp_drop_sum_far;    /* game_layout.terrain.hp_drop_sum_far        */
    int32_t hp_drop_corner_min; /* game_layout.terrain.hp_drop_corner_min     */
    uint32_t gflags;      /* OPTC_GF_*                                         */
    uint32_t ot_terms;    /* the LP insert's ordering-table terms, packed:
                           * bits 0-7 lp_base_slots, 8-11 lp_bias_slots,
                           * 12-15 hp_bias_slots, 16 lp_farcull_keyed
                           * (game_layout.terrain)                            */
    /* Per-tile fractional UV-scroll V offsets for THIS build (env texture
     * anim, sub-tick lerp of the 7-bit accumulator): [0] = LQ/flat-face dv,
     * [1]/[2] = HQ record entries 1-2 / 3-4 (the two ring halves), [3] pad.
     * All zero on canonical builds (whole-texel committed decode). Faces
     * carry their tile id in flags16; the vertex stage adds the offset. */
    float   scroll_dv[128][4];
    /* [r] = the 4x4 near-cell row r's V offset: that row's tiles carry
     * ((scroll >> 1) + 16*r) & 0x3F, wrapped like [1]/[2]. */
    float   scroll_dvc[128][4];
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
    double  rim_O[6][4];              /* in-plane basis origin; w = 1: rim per fragment */
    double  rim_U[6][4];              /* in-plane basis U                   */
    double  rim_W[6][4];              /* in-plane basis W                   */
    double  rim_nrm[6][4];            /* unit view-space normal; w = vn     */
    double  rim_bbox[6][4];           /* outline UV bbox pulo,puhi,pvlo,pvhi */
    double  rim_pv[96][2];            /* outlines in (U,W) coords, 16/plane */
    /* Portal cover planes for the blended-face discard in the
     * fragment stage: the active opening planes in SCREEN space. The
     * blended passes' fragment stage (terrain.frag, portal_cover.glsl)
     * discards a fragment inside an opening's outline polygon at or behind
     * the plane minus the cover margin while cov_blend is set (the
     * transparent-cover lever); every pass discards a face the vertex stage
     * flags as per-fragment inner rim inside an outline. cov_n = 0 disables
     * both (no active portal). A plane whose
     * screen outline projection failed rides with cov_pn = 0 and never
     * matches, exactly like the CPU test's pn < 3 skip. */
    int32_t cov_n;
    int32_t cov_blend;                /* 1: the blended cover discard is on */
    double  cov_margin;               /* plane-cover near margin M (world)  */
    double  cov_abc[6][3];            /* screen inv-vz plane fit a, b, c    */
    int32_t cov_pn[6];                /* outline point count (0 = no poly)  */
    float   cov_bbox[6][4];           /* outline screen bbox x0, y0, x1, y1 */
    float   cov_pxy[6][16][2];        /* outline polygon, screen px/py      */
} openpete_tercache_globals;

/** @brief Create the terrain cache's pipelines, arena buffers and uniform buffer on a device.
 *
 * native_render.c owns the device lifetime and calls this at bring-up. The
 * world, anim, near and cell-record pipeline families are built in each
 * draw variant, every arena gets a primary buffer sized from the union
 * capacity row (no level has loaded yet), and the 1x1 stand-in for the
 * HD-fold atlas and the globals and sectors UBO are made here on the
 * device-owning thread. Any failure leaves the cache disabled and terrain
 * on the CPU walk. The pipeline creates are bracketed by @p batch: called
 * with 1 before the first create and with 0 after the last, before any
 * pipeline is read, so the caller can run them in a pipeline batch.
 * @param dev Device; NULL leaves the cache disabled.
 * @param batch Opens (1) and closes (0) the creates' bracket; NULL for none.
 * @return 1 when the cache is ready, 0 otherwise. */
int  openpete_tercache_init(struct rhi_device *dev, void (*batch)(int open));
/** @brief Describe the terrain cache's pipelines without creating them: run the builder openpete_tercache_init runs inside the caller's rhi_pipeline_describe_begin.
 *
 * Touches no cache state, so it runs before bring-up (the launcher's
 * precompile). Display owner only.
 * @param dev Device in description mode; NULL does nothing. */
void openpete_tercache_describe(struct rhi_device *dev);
/** @brief Return whether the terrain cache's vertex stages are the TERRAIN_FP32 build.
 *
 * OPENPETE_TERRAIN_FP32 set and not "0" selects the fp32 stages (no
 * Float64 capability); "2" selects them with TERRAIN_FP32_SECDELTA, the
 * hole census's per-sector rounding control; unset, the fp64 stages. Read
 * through openpete_config_device_value at the first call and latched, so
 * the launcher's describe, bring-up's init and every material pipeline
 * take the same build.
 * @return 1 for an fp32 build, 0 for the fp64 build. */
int  openpete_tercache_fp32(void);
/** @brief Return whether OPENPETE_TERRAIN_FACEID selects the face-identity colour pipelines.
 *
 * Set and not "0": every retained colour pipeline paints the identity of
 * the face covering each pixel (terrain_faceid.frag) in place of its
 * shading, and the renderer keeps only terrain in the scene. Read with
 * getenv at the first call and latched. Diagnostic; unregistered.
 * @return 1 when on, else 0. */
int  openpete_tercache_faceid(void);
/** @brief Return whether a device can run the selected build of the terrain vertex stages.
 *
 * The fp32 build needs no device feature; the fp64 build needs shaderFloat64.
 * @param dev The device.
 * @return 1 when the selected stages may be created on @p dev. */
int  openpete_tercache_device_precision_ok(const struct rhi_device *dev);
/** @brief Destroy the terrain cache's device objects and zero its state, keeping the final stats.
 *
 * The stats are snapshotted first so a census printed after teardown
 * reads them. With OPENPETE_TC_SUBMIT_CENSUS=1 it prints the submission
 * census: vertices submitted per selected present beside the count
 * whole-range draws would have submitted, and draw calls per present.
 * @param dev The device the cache was created on; NULL is ignored. */
void openpete_tercache_shutdown(struct rhi_device *dev);
/** @brief Report whether the terrain cache came up and is usable.
 *
 * Set by openpete_tercache_init(), cleared by openpete_tercache_shutdown()
 * and by a frame_begin that finds no uniform buffer. The flag is a plain
 * int read without synchronisation.
 * @return 1 when the cache is ready, 0 otherwise. */
int  openpete_tercache_ready(void);

/* Kinds of lazy pipeline slot (openpete_lazy_ref). */
enum {
    OPENPETE_LAZY_BUILTIN = 0,  /* the engine's raster quantize (index 0) or dither (index 1) pass */
    OPENPETE_LAZY_POSTFX,       /* a mod postfx pass                                             */
    OPENPETE_LAZY_MATERIAL,     /* a mod material's scene, sky, portal or decal slot             */
    OPENPETE_LAZY_TERRAIN,      /* a mod material's retained-terrain slot                        */
    OPENPETE_LAZY_CHAIN,        /* a combined module's scene or portal slot (index: module id)   */
    OPENPETE_LAZY_CHAIN_TERRAIN /* a combined module's retained-terrain slot (index: module id)  */
};

/* Where a lazy pipeline slot lives in the registry: the late compile path
 * finds the slot again from this when the compile returns, never from a
 * pointer kept since the request. */
typedef struct {
    int         kind;        /* OPENPETE_LAZY_*                                            */
    int         index;       /* built-in pass id, postfx pass index or material index      */
    int         sub;         /* MATERIAL: the packed draw-site request; TERRAIN: pipe index */
    unsigned    generation;  /* the slot's generation at the request                       */
    int         point;       /* POSTFX: the insertion point; otherwise -1                  */
    const char *owner;       /* owning mod id, or the built-in pass name                   */
} openpete_lazy_ref;

struct rhi_pipeline_desc;

/** @brief Fill a lazy pipeline slot for a draw site or the precompile: build now, queue on the batch, or queue a late compile.
 *
 * Display owner only. A failed slot stays failed (-1). A synchronous
 * context (a headless or deterministic run, any compose other than the
 * live present's, the precompile) builds an empty slot now through
 * rhi_create_pipeline_slot, which queues it instead inside an open
 * pipeline batch, and first waits for and installs the compile of a slot
 * left pending by an earlier live present. The live present of a windowed
 * run instead sets an empty slot to OPENPETE_POSTFX_PIPE_PENDING and
 * queues its compile on the compile pool; the result is installed by a
 * later present's compose, and until then the caller draws the stock path.
 * @param desc Pipeline description.
 * @param vert_words Vertex SPIR-V words, or NULL to use desc->vert_spv.
 * @param vert_n Their count.
 * @param frag_words Fragment SPIR-V words, or NULL to use desc->frag_spv.
 * @param frag_n Their count.
 * @param slot The slot.
 * @param ref Where the slot lives.
 * @return 0 when the slot holds a pipeline, 1 when its build is queued or
 *         pending, -1 when it failed. */
int openpete_native_lazy_pipe(const struct rhi_pipeline_desc *desc,
                              const uint32_t *vert_words, size_t vert_n,
                              const uint32_t *frag_words, size_t frag_n,
                              void **slot, const openpete_lazy_ref *ref);

/** @brief Return a mod material's retained-terrain pipeline for a vertex variant and colour slot, building it into its slot when the slot is empty.
 *
 * The one description builder of the terrain material site, shared by the
 * draw and the pipeline precompile: the variant's terrain vertex stage, the
 * colour slot's stencil, blend and depth-write rule, and the material's
 * terrain-profile fragment stage. The slot is filled through
 * openpete_native_lazy_pipe: a failed build is logged and cached as
 * OPENPETE_POSTFX_PIPE_FAILED, never retried; a build queued on the open
 * pipeline batch or as a late compile leaves the draw on the stock pipeline
 * meanwhile. Display owner only.
 * @param pslot The material's pipeline slot for (variant, slot).
 * @param variant Vertex variant: 0 plain, 1 anim, 2 near, 3 anim cells, 4 near cells.
 * @param slot Colour slot: 0 world, 1 world with stencil mark, 2 fill,
 *             3 alpha, 4 add, 5 subtract.
 * @param frag Terrain-profile fragment SPIR-V words.
 * @param frag_words Their count.
 * @param owner Owning mod id for the failure log, or NULL.
 * @param mat_index The material's registry index.
 * @param chain_module The combined module whose slot @p pslot is, or -1 for a material's.
 * @param generation The material's or module's slot generation.
 * @return The pipeline (an rhi_pipeline*), or NULL when the slot failed,
 *         the build was queued or is pending, or the cache has no device. */
void *openpete_tercache_material_pipe(void **pslot, int variant, int slot,
                                      const uint32_t *frag, size_t frag_words,
                                      const char *owner, int mat_index, int chain_module,
                                      unsigned generation);

/* Per-build protocol (driven by the terrain feeder). The sub-tick spine
 * presents the PREVIOUS canonical extraction (the canonical-lag leg), so
 * the retained state is double-buffered to match: a hook (canonical) build
 * STAGES its state (immediate=0) and the previous staged state promotes to
 * LIVE; a re-extracted present's build (immediate=1) writes LIVE directly -
 * its build is the displayed state. stamp = openpete_frame of the build.
 * frame_begin fills the target state's globals + zeroes its sector flags;
 * sector_set publishes one walked sector's verdicts into it; frame_commit
 * closes the build. frame_skip stages an all-claimed (empty) state for a
 * pure-CPU fallback build, so the present that renders that build's scene
 * draws no retained content. */
/** @brief Open a terrain build: fill the target state's globals and clear its sector flags.
 *
 * Builder thread, once per terrain feed. The call first adopts any
 * fulfilled level-edge replacement and overflow buffers, then releases the
 * deferred chunk ranges that are older than the in-flight window and that
 * no live eager ring state can still read, stamping them dead before they
 * become reusable. The target state (tc_begin_state) is a ring entry for an
 * immediate build of a scene stamp (OPTC_STAMP_SCENE_BIT), the live state
 * for any other immediate build, or the staged state, whose previous build
 * is promoted to live. Its globals are copied from @p g together with the
 * HD-fold entry table and continuous-fog ramp; the UBO itself is written at
 * the present's first draw. No-op while the cache is not ready or @p g is
 * NULL.
 * @param g Build globals.
 * @param stamp Stamp of the build (openpete_native_scene_build_stamp).
 * @param immediate 1 to write the live state or a ring entry directly (a
 *        re-extracted present's build), 0 to stage it (a canonical build). */
void openpete_tercache_frame_begin(const openpete_tercache_globals *g,
                                   uint32_t stamp, int immediate);
/** @brief Publish one sector's origin, flags and LP colour grade into the open build.
 *
 * Builder thread, between frame_begin (or frame_skip) and frame_commit; a
 * call outside that window, an out-of-range slot and OPTC_SLOT_DEAD are
 * ignored. A slot set with OPTC_SF_DRAWN is drawn by the retained path this
 * build and skipped by the CPU walk. frame_begin clears every slot's flags,
 * so a slot not set in a build stays undrawn; the grade words are written
 * only when @p grade6 is non-NULL.
 * @param slot Sector slot (the sector's index in the walk's sector table).
 * @param ox Sector origin X.
 * @param oy Sector origin Y.
 * @param oz Sector origin Z.
 * @param sflags OPTC_SF_* bits, with the slot's chunk generation in bits
 *        12-14.
 * @param grade6 The pack-aware LP colour grade (p.rgb then q.rgb,
 *        lp_grade_apply's affine, from the sector's own fit or the level
 *        mean), or NULL when no pack grade applies; a non-NULL grade is
 *        paired with OPTC_SF_LPGRADE in @p sflags.
 * @param coltab The first unit of the sector's colour table in the
 *        previous-tick store, or OPTC_PREV_REC_NONE. */
void openpete_tercache_sector_set(int slot, float ox, float oy, float oz,
                                  uint32_t sflags, const float *grade6, uint32_t coltab);
/** @brief Close the open build: its sector flags are final, so its draw-range list is built from the chunk registry.
 *
 * Promotion to the live state happens at the next frame_begin; the UBO
 * flush at the present's first draw. */
void openpete_tercache_frame_commit(void);
/** @brief Open a build whose state draws no retained terrain, keeping its place in the promote chain.
 *
 * Used for a pure-CPU fallback build: the state is stamped with zeroed
 * globals and sector flags and no draw-range list.
 * @param stamp Stamp of the build.
 * @param immediate Nonzero for an immediate build. */
void openpete_tercache_frame_skip(uint32_t stamp, int immediate);
/** @brief Select the retained terrain state built by the scene being composed, or close the retained draws.
 *
 * Called by the renderer once per rendered present. The state whose stamp
 * equals @p present_stamp is selected, searched in order: the eager scene
 * ring (only for a stamp carrying OPTC_STAMP_SCENE_BIT), then the live
 * state, then the staged state (a depth-0 compose renders the current
 * scene, whose build sits in staged until the next build's promote). No
 * other state stands in: the retained terrain must be the same extraction
 * as the CPU tris it composes with, and a scene that ran no terrain feed
 * (fades, level loads, menus) has no retained terrain, so an older state
 * never repaints over VRAM a load is overwriting. The selection's UBO is
 * flushed again at the present's first draw.
 * @param present_stamp Stamp of the scene being composed. */
void openpete_tercache_frame_select(uint32_t present_stamp);
/** @brief Report whether the last openpete_tercache_frame_select() opened the retained draws.
 *
 * @return 1 when a state is selected, 0 when the retained draws are closed. */
int  openpete_tercache_frame_open(void);
/** @brief Report whether the state selected for this present would paint any terrain.
 *
 * The freeze-backdrop source test (scene_is_freeze_source) and the native
 * compose's empty-scene test read it: a scene whose only terrain is on the
 * cache has no CPU triangles.
 * @return 1 when the selected state holds a live sector; 0 when the draws
 *         are closed or the state is empty (a frame_skip state, the feeder
 *         having cut out for that scene); -1 when the cache is not ready. */
int  openpete_tercache_frame_content(void);
/** @brief Report whether the retained state built for a scene stamp would paint any terrain.
 *
 * Reads the state the build of @p stamp wrote (eager ring, live or staged,
 * searched as openpete_tercache_frame_select does) without changing the
 * selection, so it answers for a scene before that scene is composed.
 * @param stamp Stamp of the scene (openpete_native_scene_build_stamp at its build).
 * @return 1 when that state holds a live sector; 0 when no state carries
 *         @p stamp or it is empty (a frame_skip state); -1 when the cache
 *         is not ready. */
int  openpete_tercache_build_content(uint32_t stamp);
/** @brief Count the drawn sectors of the state the build of a stamp wrote.
 *
 * Searches as openpete_tercache_build_content does and counts the slots
 * with OPTC_SF_DRAWN, the count openpete_tercache_frame_stats reports as
 * `drawn` once that state is selected.
 * @param stamp Stamp of the scene (openpete_native_scene_build_stamp at its build).
 * @return The drawn-sector count; 0 when no state carries @p stamp; -1 when
 *         the cache is not ready. */
int  openpete_tercache_build_drawn(uint32_t stamp);
/* Retained states kept for held scenes: one per hold-store entry of the
 * native compose (OPENPETE_NATIVE_HOLD_POOL). */
#define OPENPETE_TERCACHE_HOLDS 6
/** @brief Keep a copy of the retained state a stamp's build wrote, so a scene held past its build still draws its terrain.
 *
 * Copies the state openpete_tercache_build_content would read for @p stamp
 * (eager ring, live or staged) into hold @p idx. A held state answers
 * openpete_tercache_frame_select for its stamp after the state it was copied
 * from is reused, and its build serial joins the release floor, so the arena
 * ranges it draws are not reused while it is held. A level-edge arena
 * replacement drops every hold, since their ranges address the outgoing
 * buffers. Builder thread. With no state for @p stamp the hold is left empty
 * and the miss is counted.
 * @param idx Hold index, 0 to OPENPETE_TERCACHE_HOLDS - 1.
 * @param stamp Stamp of the held scene. */
void openpete_tercache_hold_take(int idx, uint32_t stamp);
/** @brief Release a hold taken by openpete_tercache_hold_take.
 *
 * @param idx Hold index; out of range is ignored. */
void openpete_tercache_hold_drop(int idx);
/** @brief Read the hold census since boot.
 *
 * Any output pointer may be NULL.
 * @param takes Receives the holds that copied a state.
 * @param misses Receives the takes that found no state for their stamp.
 * @param drops Receives the holds a level-edge replacement dropped. */
void openpete_tercache_hold_counts(unsigned *takes, unsigned *misses, unsigned *drops);
/** @brief Report which retained state the last openpete_tercache_frame_select() chose.
 *
 * @return 0 when the draws are closed, 1 for the scene-ring state, 2 for the
 *         live state, 3 for the staged state. */
int  openpete_tercache_frame_source(void);
/** @brief Count the selected state's drawn sectors by flag and by generation.
 *
 * Diagnostic. Walks the OPTC_SLOTS slots of the selected state; a slot with
 * OPTC_SF_DRAWN set counts once in `drawn` and once more in each of `hp`,
 * `lp` and `nearvar` for its OPTC_SF_DRAWHP, OPTC_SF_DRAWLP and
 * OPTC_SF_NEARVAR bits. Bits 12..14 of the slot flags index `gens`. All
 * outputs are zeroed first and stay zero while the draws are closed.
 * @param drawn Out: sectors with OPTC_SF_DRAWN.
 * @param hp Out: drawn sectors with OPTC_SF_DRAWHP.
 * @param lp Out: drawn sectors with OPTC_SF_DRAWLP.
 * @param nearvar Out: drawn sectors with OPTC_SF_NEARVAR.
 * @param gens Out: 8-entry histogram of the 3-bit generation over the drawn
 *        sectors. */
void openpete_tercache_frame_stats(int *drawn, int *hp, int *lp, int *nearvar, int gens[8]);
/** @brief Format the selected state's globals block as one text line.
 *
 * Diagnostic. Prints camera xy and z, the ofx/ofy offset, H, the near
 * plane, the fog and flag words, the cull reference, the first and third
 * matrix rows, and the rim and coverage plane counts. Reads the selected
 * state whether or not the draws are open, so a closed present reports the
 * last selection. The text is truncated to fit.
 * @param out Destination buffer.
 * @param cap Size of `out` in bytes; must be at least 1. */
void openpete_tercache_frame_globals(char *out, int cap);
/** @brief Format the selected state's drawn sector slots as text.
 *
 * Diagnostic. Writes from index 0 of `out` (a caller appending to a line
 * passes the tail of its buffer) one " slot:flags" pair per slot with
 * OPTC_SF_DRAWN set, the flags as the low 12 bits in hex, and stops when
 * fewer than 25 bytes remain. Leaves an empty string when no slot is
 * drawn. Reads the selected state whether or not the draws are open.
 * @param out Destination buffer.
 * @param cap Size of `out` in bytes; must be at least 1. */
void openpete_tercache_frame_slots(char *out, int cap);

/** @brief Return 1 when the terrain feeder may route unclaimed sectors through the GPU cache.
 *
 * Defined in native_render.c. ANDs the renderer-side conditions the feeder
 * cannot see: the native renderer is enabled, the cache is ready, no
 * reference-compare scene delay is selected (the cache draws the live walk,
 * not the scene history ring), neither the tag-split nor the tag-tint
 * instrument nor the OPENPETE_SCENE_BOXDBG box instrument is armed (their
 * semantics live in the CPU batcher), the device can run the selected
 * terrain vertex stages (openpete_tercache_device_precision_ok: shaderFloat64
 * for the fp64 stages, which match the CPU walk's double math bit for bit;
 * nothing for the TERRAIN_FP32 stages), and the device
 * is a hardware GPU (terrain.vert runs slowly on llvmpipe) unless
 * OPENPETE_TERRAIN_CACHE_FORCE=1 waives that.
 *
 * The PSX vertex wobble is not an exclusion: terrain.vert applies the same
 * floor to its emitted screen point. HD texture packs and mod material
 * selectors are not exclusions either: the feeder claims affected sectors
 * back per sector. The feeder ANDs its own conditions (flag, debug envs,
 * portal recency).
 * @return 1 when the retained path is usable for this present, else 0. */
int  openpete_native_tercache_usable(void);
/** @brief Run the terrain cache's device service on the headless path.
 *
 * The host loop calls it once per VBL. Headless never reaches the present
 * entry, the service's windowed site, so this entry drives it instead. It
 * is a no-op with a window, so exactly one of the two sites is live in any
 * mode and the device is never serviced from two threads, and a no-op when
 * the retained path is not usable this present
 * (openpete_native_tercache_usable). */
void openpete_native_tercache_service_headless(void);

/* ---- HD texture-replacement fold (pack art in the retained path) --------
 * Defined in native_render.c (owner of the pack-registry and pack-fog
 * helpers plus the frame upload point). The feeder's bake interns one entry
 * per emitted textured tri; the retained fragment stage samples the fold
 * atlas instead of the VRAM model when a face carries an entry id (upper
 * halves of the mat_far1/mat_near1 clut words). */
#define OPENPETE_HDFOLD_MAX 416   /* entry-table cap (globals-UBO budget) */

/** @brief Return 1 when the HD texture-replacement fold is active.
 *
 * Default on; OPENPETE_TERRAIN_HDFOLD=0 disables it, and so does
 * OPENPETE_TEXPACK_FOG_CONT=0 or OPENPETE_TEXPACK_FOG_FIT=0, since the fold
 * bakes the default pack-fog models. The environment is read once per run;
 * the result also requires a texture pack with active replacements, which is
 * tested on every call.
 * @return 1 when the fold is on, else 0. */
int  openpete_native_hdfold_on(void);

/** @brief Intern the pack replacement for one emitted tri and return its fold entry id.
 *
 * Resolves and matches the tri's texel bounding box the way the draw-time
 * path does, so a tri that returns 0 here also has no replacement at draw
 * time. Entries are memoized per (replacement file, material); a new entry
 * shelf-packs the file image into the fold atlas with a 1-texel gutter,
 * queues its upload, fills its 4-vec4 payload (page-anchored rect, atlas
 * rect, fog affine, filter class) and bumps the fold epoch.
 * @param tpx Texture page X.
 * @param tpy Texture page Y.
 * @param clx CLUT X.
 * @param cly CLUT Y.
 * @param bpp Texel depth (4, 8 or 16).
 * @param us The tri's three U texel coordinates in page space.
 * @param vs The tri's three V texel coordinates in page space.
 * @return 0 when the fold is off or the tri has no replacement, a 1-based
 *         entry id, or -1 when the entry table, the atlas or the upload
 *         queue is full; on -1 the caller claims the sector back to the CPU
 *         walk. */
int  openpete_native_hdfold_intern(int tpx, int tpy, int clx, int cly, int bpp,
                                   const float us[3], const float vs[3]);

/** @brief Return the fold entry table for the retained terrain globals fill.
 *
 * The table holds OPENPETE_HDFOLD_MAX entries of 4 vec4 each: the
 * page-anchored texel rect, the atlas rect, fog affine A with the filter
 * class in w (0 nearest, 1 linear), and fog affine B. The first count
 * entries are live.
 * @param count Receives the live entry count; may be NULL.
 * @param lad Receives the continuous-fog ramp, or zeros when the pack has
 *        none; may be NULL.
 * @return The table base; never NULL. */
const void *openpete_native_hdfold_table(int *count, float lad[2]);

/** @brief Return the fold atlas texture that retained terrain samples at set 4.
 *
 * Created by openpete_native_hdfold_prime at bring-up, or by the first
 * openpete_native_hdfold_flush with queued uploads.
 * @return The atlas, or NULL before either creates it. */
struct rhi_texture *openpete_native_hdfold_atlas(void);

/** @brief Return the material UBO that walked and retained material draws bind at set 3.
 *
 * Block 0 is the engine block; block 1 + slot is the uniform block of an
 * interned material slot. A retained terrain material pass binds the same
 * set-3 pair a walked material draw binds, so both residencies read one
 * uniform source. The UBO is created and filled once per present while the
 * material tag split is armed.
 * @return The UBO, or NULL before the first present with the split armed. */
struct rhi_ubo *openpete_native_material_ubo(void);

/** @brief Bind the material contract set: the engine block, a material's uniform block and its fragment profile's table block.
 *
 * Binds set 3 of the bound pipeline: binding 0 the material UBO's engine
 * block, binding 1 block 1 + @p uniform_slot (the engine block for slot 0),
 * binding 2 the device copy of @p cd's table block, or the RHI's zero
 * buffer when @p cd carries none. The device copy is made on the first
 * bind of a block, shared by every profile with identical bytes, cached in
 * the profile's slot and freed at renderer shutdown; a failed upload binds
 * the zero buffer. Does nothing before the material UBO exists.
 * @param dev Device whose current frame records the bind.
 * @param uniform_slot The material's uniform slot (0 = no params).
 * @param cd The profile's moved tables; NULL or len 0 for none. */
void openpete_native_material_bind(struct rhi_device *dev, int uniform_slot,
                                   const struct openpete_mod_cdata *cd);

/** @brief Return whether the chain uniform set exists, which it does once a combined module installed and a present filled it.
 *
 * @return 1 or 0. */
int openpete_native_material_chain_ready(void);

/** @brief Bind the material contract set of a retained-terrain chain pass: the chain uniform set's engine block, the chain entry's terrain block and the terrain module's table block.
 *
 * Binds set 3 of the bound pipeline as openpete_native_material_bind
 * does, from the chain uniform set
 * (openpete_mods_material_chain_ubo_block with the terrain profile).
 * Does nothing before the chain uniform set exists.
 * @param dev Device whose current frame records the bind.
 * @param entry Chain entry id of a terrain chain.
 * @param cd The terrain module's moved tables; NULL or len 0 for none. */
void openpete_native_material_chain_bind(struct rhi_device *dev, int entry,
                                         const struct openpete_mod_cdata *cd);

/** @brief Return the number of fold atlas uploads still queued.
 *
 * Each openpete_native_hdfold_flush drains at most 96. The screenshot
 * compose repeats, up to 8 retries, until this reaches 0, so a capture
 * does not sample a partly filled atlas.
 * @return The queued upload count. */
int openpete_native_hdfold_pending(void);

/** @brief Return the fold epoch.
 *
 * Bumped on every new entry and on a flush that drains the upload queue. A
 * sector claimed back to the CPU walk for fold overflow rebakes when it
 * moves, since a new entry or a drained queue can clear the overflow.
 * @return The epoch. */
uint32_t openpete_native_hdfold_epoch(void);

/** @brief Upload queued fold atlas rects into the current rhi frame.
 *
 * Called after begin_frame and before the first render pass. Uploads at
 * most 96 rects per call and keeps the rest queued; draining the queue bumps
 * the epoch. Creates the atlas when it does not exist; a failed creation
 * drops the queue. No-op when nothing is queued or native render is off.
 * Serialized with openpete_native_hdfold_intern by the fold mutex. */
void openpete_native_hdfold_flush(void);
/** @brief Create the fold atlas and make it resident ahead of first use.
 *
 * A 1x1 upload transitions the image, so the driver's allocation and
 * residency work lands at bring-up and not on a present the sim pacer is
 * timing. No-op when the atlas exists or @p dev is NULL.
 * @param dev Device that owns the atlas. */
void openpete_native_hdfold_prime(struct rhi_device *dev);

/** @brief Drop every fold entry and queued upload and rewind the atlas packer.
 *
 * Called on a level change. The atlas texture is kept and later entries
 * overwrite its content. Entry ids baked into sectors no longer name
 * entries, so the caller rebakes those sectors; the epoch is not bumped. */
void openpete_native_hdfold_reset(void);

/* ---- chunk management (builder side) ------------------------------------ */
/** @brief Allocate a chunk of vertices, or of records on a cell arena, in one of an arena's buffers.
 *
 * Each buffer of the arena is tried in order, first fit in its free list,
 * else growth of its high-water mark. Every later call for the chunk
 * (write, free) repeats the returned buffer index, since each buffer has
 * its own vertex numbering from 0. When every buffer is full the call
 * counts the failure and posts an overflow request for the arena (see the
 * overflow buffers below), so the caller's claim-back to the CPU walk lasts
 * only the presents the device thread needs to allocate.
 * @param arena Arena index.
 * @param vert_count Vertices, or records on a cell arena, to allocate.
 * @param out_first Receives the first index of the chunk.
 * @param out_buf Receives the buffer the index addresses.
 * @return 0 on success; -1 when the arena is full, the cache is not ready,
 *         or an argument is out of range. */
int  openpete_tercache_chunk_alloc(int arena, int vert_count, uint32_t *out_first,
                                   int *out_buf);
/** @brief Upload packed vertices at [first, first+count) of an arena buffer and record the chunk in the registry.
 *
 * The registry entry carries the sector slot and generation of the
 * vertices' face_flags, which the draw-range lists select by. No-op while
 * the cache is not ready.
 * @param arena Arena index.
 * @param buf Buffer index from openpete_tercache_chunk_alloc().
 * @param first First vertex index of the chunk.
 * @param v Vertices.
 * @param count Vertex count. */
void openpete_tercache_chunk_write(int arena, int buf, uint32_t first,
                                   const struct rhi_terrain_vertex *v, int count);
/** @brief Upload a chunk's previous-tick records on an anim arena.
 *
 * The records are parallel to the chunk's vertices at the same
 * [first, first+count) range, in the buffer's prev twin. No-op on a
 * non-anim arena or while the cache is not ready.
 * @param arena Arena index.
 * @param buf Buffer index from openpete_tercache_chunk_alloc().
 * @param first First vertex index of the chunk.
 * @param p Previous-tick records.
 * @param count Record count. */
void openpete_tercache_chunk_write_prev(int arena, int buf, uint32_t first,
                                        const struct rhi_terrain_prev *p, int count);
/** @brief Upload a chunk's emitted-triangle member context on a near arena.
 *
 * The records are parallel to the chunk's vertices at the same
 * [first, first+count) range, in the buffer's context twin. No-op on a
 * non-near arena or while the cache is not ready.
 * @param arena Arena index.
 * @param buf Buffer index from openpete_tercache_chunk_alloc().
 * @param first First vertex index of the chunk.
 * @param c Member-context records.
 * @param count Record count. */
void openpete_tercache_chunk_write_ctx(int arena, int buf, uint32_t first,
                                       const struct rhi_terrain_nearctx *c, int count);
/** @brief Upload cell records to a cell arena.
 *
 * The records are rhi_terrain_cell, or rhi_terrain_cell_anim on the CA
 * family, written at record index @p first. Alloc and free count records
 * on these arenas. No-op on a non-cell arena or while the cache is not
 * ready.
 * @param arena Arena index.
 * @param buf Buffer index from openpete_tercache_chunk_alloc().
 * @param first First record index.
 * @param recs Records.
 * @param count Record count. */
void openpete_tercache_chunk_write_cells(int arena, int buf, uint32_t first,
                                         const void *recs, int count);
/** @brief Release a chunk and drop it from the registry.
 *
 * Its bytes stay untouched while in-flight frames may read them (the
 * per-slot generation hides a replaced chunk); the range becomes reusable
 * after the defer window, when it is stamped dead.
 * @param arena Arena index.
 * @param buf Buffer index from openpete_tercache_chunk_alloc().
 * @param first First element of the chunk.
 * @param count Element count. */
void openpete_tercache_chunk_free(int arena, int buf, uint32_t first, int count);

/* ---- previous-tick store -------------------------------------------------
 * A storage buffer the near, near-cell and anim vertex stages read (binding
 * 1 of set 2, beside the prop key image), allocated in units of
 * OPTC_PREV_REC_WORDS words, holding two kinds of run:
 *
 * Previous-corner records. A poly-anim sector drawn with its near-variant
 * chunks lerps each face's corners from the previous tick's positions. The
 * near arenas' vertices and cell records carry the current corners only, so
 * each such face's previous corners live in one record of one unit: the
 * twelve int16 corners c0x..c3z, two to a word, low half first. A near
 * vertex names its face's record in px | py << 16, a near cell record in
 * fog_a; OPTC_PREV_REC_NONE names none, and the stage takes the current
 * corners.
 *
 * Colour tables. An env-anim colour sector baked under
 * OPENPETE_TERRAIN_CANIMNEAR=1 reads its rolled colours from one table per
 * sector, so a colour roll rewrites the table and leaves the chunks as
 * baked. The table is a header word (LP colour count | HP colour count <<
 * 16), then the current LP, HP near and HP far colour words (RGB, top byte
 * 0), then the previous tick's three arrays in the same layout. A sector
 * holds a ring of OPTC_COLTAB_RING such entries, allocated once per
 * residency; a roll writes the next entry once no state still drawn names
 * it (openpete_tercache_serial_released), and the entry's first unit rides
 * the sector's per-present state (openpete_tercache_sector_set),
 * OPTC_PREV_REC_NONE for none. The vertex stages lerp previous to current
 * at the build's colour alpha, as terrain.c canim_lerp does.
 *
 * Each face's flags (OPTC_FACE_F_*) ride a near vertex's pz, a cell record's
 * fog_b and an anim vertex's rhi_terrain_prev.pad. A face with
 * OPTC_FACE_F_COLTAB takes the colours of its corners' colour indices: a
 * near vertex carries the four (i0..i3, one byte each, low first) in
 * rgba_far, a cell record in near_rgb[0], and an anim vertex its own in the
 * low byte of pad.
 *
 * Runs are allocated, written and freed by the builder, and a freed run is
 * reused only after the chunks' defer window, so a run is unchanged while
 * any state that can draw a chunk naming it lives. */
#define OPTC_PREV_REC_WORDS 6u
#define OPTC_PREV_REC_NONE  0xFFFFFFFFu
#define OPTC_FACE_F_PREV    1u   /* a near face names a previous-corner record */
#define OPTC_FACE_F_COLTAB  2u   /* the face's colours come from the colour table */
#define OPTC_PREV_PAD_COLTAB 0x4000u   /* rhi_terrain_prev.pad: colour-table index in bits 0-7 */
#define OPTC_COLTAB_RING    6u         /* colour-table entries per sector: a
                                        * roll writes the next one, so the
                                        * TC_DEFER_PRESENTS-build window plus
                                        * the entry being named fit before reuse */
#define OPTC_COLTAB_BAKED   0xFFu      /* a slot index naming no table word: a
                                        * triangle's slot 3 past the array keeps
                                        * the record's baked word */
/** @brief Allocate a run of previous-tick store units.
 * @param count Unit count, at least 1.
 * @param out_first Receives the first unit index.
 * @return 0, or -1 when the store has no room (the caller claims the sector
 *         back to the CPU walk, as on a full arena). */
int  openpete_tercache_prev_alloc(int count, uint32_t *out_first);
/** @brief Write previous-tick store units at an allocated index.
 * @param first First unit index from openpete_tercache_prev_alloc.
 * @param words count * OPTC_PREV_REC_WORDS words.
 * @param count Unit count. */
void openpete_tercache_prev_write(uint32_t first, const uint32_t *words, int count);
/** @brief Return the serial of the build being made, the stamp a colour-table ring entry takes when the ring moves past it.
 * @return The build serial. */
uint32_t openpete_tercache_build_serial(void);
/** @brief Test whether no state still drawn can read store data that builds before a serial named.
 *
 * The rule of the deferred releases: the serial is older than the defer
 * window and at or below the oldest build serial a ring or held state
 * carries (a state built at or after it does not name the data).
 * @param serial The serial of the first build that no longer names the data.
 * @return 1 when released, else 0. */
int openpete_tercache_serial_released(uint32_t serial);
/** @brief Release a run of previous-tick store units after the chunk defer window.
 * @param first First unit index.
 * @param count Unit count. */
void openpete_tercache_prev_free(uint32_t first, int count);

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

/* ---- level-edge arena replacement --------------------------------------- */
/** @brief Record the incoming level and post a request to replace every arena's buffers with fresh ones.
 *
 * Builder thread, at a level latch that has an outgoing level, after every
 * slot has given up its chunks. `high` never falls and a freed range returns
 * to the free list only after the defer window, so without replacement the
 * arenas accumulate toward every cap across level loads: the previous
 * level's chunks are never released (their decode slots are rebuilt only
 * when another sector takes them) and the surviving free spans re-fit the
 * next level's chunk sizes imperfectly. A reset in place does not work
 * because the defer window counts builds and no build runs during a load,
 * so a range freed at the level edge cannot age out before the new level's
 * first walk allocates over it.
 *
 * Three steps on two threads. This call stores the level and the request.
 * The device-owning thread allocates one fresh {vb, pv} pair per arena,
 * sized for the stored level (present_service), since device objects are
 * illegal on the builder's thread. The builder adopts the pairs at its next
 * frame_begin: each arena's bookkeeping resets to high = 0 with an empty
 * free list, and the outgoing pairs go on a retire list that
 * present_service destroys once both drain floors have passed. Adopting
 * resets every arena's counters, so a caller holding chunk records frees
 * them before the adopting frame_begin; openpete_tercache_resize_ready()
 * reports that moment. A second request before the device-owning thread
 * takes the first only replaces the stored level; one made after it starts
 * a second replacement, which runs once the first has retired. No-op while
 * the cache is not ready.
 * @param level_id Level the replacement buffers are sized for. */
void openpete_tercache_request_resize(int level_id);
/** @brief Report whether a fulfilled replacement pair awaits adoption.
 *
 * Builder thread. The adoption performed by the next frame_begin zeroes
 * every arena's bookkeeping, so a caller holding chunk records frees them
 * while this reads 1 and before that frame_begin.
 * @return 1 when the cache is ready and a fulfilled pair is waiting, else 0. */
int  openpete_tercache_resize_ready(void);
/** @brief Store the level the arenas are being filled for.
 *
 * The stored level sizes every buffer the cache allocates afterwards: the
 * level-edge replacement pairs and the overflow buffers. Called directly at
 * the first level latch after boot, where init has just made the buffers
 * and no replacement is owed, and through
 * openpete_tercache_request_resize() at every later latch.
 * @param level_id Level id of the registered game (game_get_id); the
 *                 capacities are that game's row for the id, or the game's
 *                 union row when it has none. */
void openpete_tercache_set_level(int level_id);
/** @brief Return the allocation epoch, which steps whenever an arena gains buffer room.
 *
 * The epoch steps when a level-edge replacement or an overflow buffer is
 * adopted. A sector claimed back because an arena was full records the
 * epoch and is baked again when it moves, which keeps that claim a
 * transient.
 * @return The current epoch. */
uint32_t openpete_tercache_alloc_epoch(void);
/** @brief Run the device-owning thread's half of the arena buffer handshakes.
 *
 * Called once per present windowed (native_render.c's present entry) and
 * once per VBL headless (openpete_native_tercache_service_headless); only
 * one of the two sites is live in a mode. Each call counts one service
 * call, then does one of: destroys the pairs a level-edge replacement
 * retired once their drain floors have passed; nothing while a fulfilled
 * replacement awaits adoption; allocates the replacement pairs for a
 * pending level-edge request; or answers the pending overflow-buffer
 * requests. A replacement request takes precedence over overflow requests.
 * This is the only caller of rhi_destroy_buffer for arena buffers outside
 * shutdown. No-op while the cache is not ready. */
void openpete_tercache_present_service(void);

/* Screen->clip mapping for one draw target: the RAW scene_to_clip inputs,
 * so terrain.vert replicates the CPU's exact float expression term for term
 * (a fitted affine reconstruction loses ~5 subpixel LSBs at the screen
 * edges - enough to flip rasterized edge pixels vs the CPU-fed draws). */
typedef struct {
    float x_base, x_span;   /* cx = x_base + nx*x_span, nx = sx/disp_w      */
    float y_base, y_span;   /* cy = y_base - ny*y_span, ny = (sy-band)/clip_h */
    float disp_w, clip_h, band, freeze_wf;
    int   full_extent;      /* cx = (-1 + nx*2)/freeze_wf, cy = 1 - ny*2    */
    int   cover;            /* 1: the fragment stage discards what a portal
                             * preview covers (the blended passes)         */
    float fc[4];            /* gl_FragCoord -> PSX screen affine of the
                             * target (fax, fbx, fay, fby); read with cover */
} openpete_tercache_clip;

/* The scene's key-contest images a terrain colour pass reads, bound at
 * the sets terrain.frag declares. A NULL member leaves its set unbound;
 * fragments that carry no key (vKoff == 0) never sample it. */
typedef struct openpete_tercache_keys {
    struct rhi_texture *hp;     /* terrain key image, set 1                  */
    struct rhi_texture *prop;   /* moby key image (prop true depth), set 2   */
    struct rhi_texture *actor;  /* actor key image with its owner colour,
                                 * set 7 (the key-order contest)             */
} openpete_tercache_keys;

/** @brief Draw one pass of the selected retained terrain state.
 *
 * Called from native_render.c. @p which picks the pass (OPTC_DRAW_WORLD,
 * ALPHA, ADD, SUB, FILL, PREPASS or KEY); each draws the plain, anim, near
 * and cell arenas that pass covers. The first draw of each rhi frame writes
 * the selected state into that frame's UBO slot and latches the
 * vertex-snap mode. The colour passes draw the stock pass and then one pass
 * per live mod material bucket; PREPASS and KEY draw every face. No-op
 * while the cache is not ready or no state is selected.
 * @param which Pass, an OPTC_DRAW_* value.
 * @param clip The target's screen-to-clip mapping (tercache_clip_params).
 * @param vram VRAM texture, or NULL to leave the binding.
 * @param use_mark Nonzero for the stencil-MARK world variant (hole-fill on).
 * @param keys The scene's key-contest images for the colour passes, or
 *        NULL for a pass that contests nothing (prepass, key emit). */
void openpete_tercache_draw(int which, const openpete_tercache_clip *clip,
                            struct rhi_texture *vram, int use_mark,
                            const openpete_tercache_keys *keys);

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
    /* Cell-arena buffer draws issued (one per buffer per pass, however many
     * spans it splits into) and the records in those buffers' high-water
     * ranges, summed over the session. */
    uint32_t cell_draws;
    uint64_t cell_instances;
    /* The previous-tick store: its high-water mark and capacity in units,
     * the allocations it refused (each a sector claimed back, counted with
     * the arena-full claims), and the releases its deferred list could not
     * hold (returned at once, so a held state may read a reused run). */
    uint32_t prev_high, prev_cap, prev_fail, prev_defer_ovf;
    /* Growth census (the `tercache:` exit row). `bakes` counts sector
     * uploads that landed; `window_bakes` those between a level-edge resize
     * request and its adoption. A claim is a chunk the arenas refused: one
     * made while a level-edge request is open counts in `edge_claims`, one
     * made while the arena's growth or consolidation request is open in
     * `grow_claims`, any other in `claims` of its arena; `retire_claims`
     * counts, across all three, the claims made while a replacement awaited
     * its retire drain. `prev_fail` above counts store refusals outside a
     * store-growth window and `prev_grow_claims` those inside one. */
    uint32_t bakes, window_bakes;
    uint32_t claims[OPTC_ARENA_COUNT];
    uint32_t edge_claims, grow_claims, retire_claims, prev_grow_claims;
    /* Buffers adopted by growth, consolidation buffers adopted, level-edge
     * replacements the device refused, store replacements adopted. */
    uint32_t grow, consolidations, failed_edges, prev_grow;
    /* Deferred releases that found the arena or store deferred list full
     * and returned at once. */
    uint32_t defer_overflow;
    /* openpete_tercache_hold_counts, copied so the exit row survives shutdown. */
    uint32_t hold_takes, hold_misses, hold_drops;
    /* Held draws of a sealed range, allocations made in a sealed buffer
     * (the invariant is 0), bytes of consolidation sources still waiting on
     * their floors, buffers allocated on a memory type without
     * DEVICE_LOCAL, free-list spans dropped for want of list room (the
     * invariant is 0), canonical ticks a consolidation took to migrate its
     * sources, and bytes held by slots whose sector left the table. */
    uint32_t sealed_draws, sealed_allocs;
    uint64_t pinned_bytes;
    uint32_t nonlocal_bufs, dropped_spans, migrate_ticks;
    uint64_t stale_bytes;
    /* Draw-range census: the most buffers any arena held at once, the
     * largest range list a committed state carried (against TC_RANGE_MAX),
     * and the draws of a state with no range list. */
    uint32_t bufs_max[OPTC_ARENA_COUNT];
    uint32_t ranges_max, cen_nolist;
} openpete_tercache_stats_t;
/** @brief Fill a snapshot of the cache's arena, draw and engagement counters.
 *
 * Sums each arena's buffers. After openpete_tercache_shutdown() it returns
 * the snapshot taken there, so a census printed at exit still reads the
 * final values; on a cache that never came up and was never shut down it
 * returns zeros. The counters are read without a lock, so a call from
 * another thread than the builder can be one update stale.
 * @param out Destination; NULL is ignored. */
void openpete_tercache_get_stats(openpete_tercache_stats_t *out);
/** @brief Count one sector upload that landed on the cache.
 *
 * Builder thread, after a sector's chunks and records were all allocated
 * and written. Feeds `bakes` and, while a level-edge resize request awaits
 * adoption, `window_bakes` of the stats. */
void openpete_tercache_note_bake(void);
/** @brief Print the `tercache:` exit row of the growth census to the log.
 *
 * Reads openpete_tercache_get_stats, so after shutdown it reports the final
 * snapshot, and on a run whose cache never came up it reports zeros. One
 * row: bakes, grow, consolidations, failed_edges, prev_grow, prev_fail,
 * claims per arena and their sum, edge_claims, grow_claims, retire_claims,
 * prev_grow_claims, window_bakes, defer_overflow, the hold counters,
 * sealed_draws, sealed_allocs, svc_fulfilled, draws_retired, pinned_bytes,
 * nonlocal_bufs, dropped_spans, migrate_ticks, stale_bytes, buffers per
 * arena, ranges_max and nolist. */
void openpete_tercache_print_exit_row(void);

#endif /* OPENPETE_TERRAIN_CACHE_H */
