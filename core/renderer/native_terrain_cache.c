/* core/renderer/native_terrain_cache.c - terrain sector-mesh GPU cache,
 * core side. See openpete_terrain_cache.h for the contract. */

#include "rhi.h"
#include "openpete_terrain_cache.h"
#include "openpete_mods.h"   /* terrain material buckets + their pipelines */
#include "openpete_native_render.h"   /* scene ring: is a build stamp still referenced */

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Measured per-level arena capacities, generated at configure time from the
 * committed high-water baseline (k_optc_caps_level keyed by the level id the
 * game reports, k_optc_caps_union over every level for an id with no row).
 * The level ids are the ones baselines/terrain-arena was measured against;
 * another game's ids land on the union row or, matching by accident, on a row
 * sized for different content, which costs memory or an overflow buffer and
 * never a claim that outlives one. */
#include "terrain_arena_caps.gen.h"

/* Last-resort arena capacities (vertices; records on the cell arenas), used
 * only when the generated table is empty (no baseline rows at all). Sized
 * from worst-case level content: a big level carries ~50k HP faces x (6 flat
 * + 24 HQ-variant) verts and a few thousand LP/blend faces x 6. 64 B/vert =>
 * OPAQUE 96 MB, 192 MiB over the fifteen vertex arenas with their twins; the
 * eight cell-record arenas add 4.8 MiB. */
static const uint32_t k_arena_cap[OPTC_ARENA_COUNT] = {
    [OPTC_ARENA_OPAQUE] = 1500000,
    [OPTC_ARENA_ALPHA]  =  131072,
    [OPTC_ARENA_ADD]    =  131072,
    [OPTC_ARENA_SUB]    =  131072,
    [OPTC_ARENA_LP]     =  262144,
    /* Poly-anim twins: a level names a handful of anim sectors (<= 256
     * faces each), so these stay small; overflow claims back to the CPU
     * walk like any arena-full. Each also carries a parallel prev buffer
     * (rhi_terrain_prev, 40 B/vert). */
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
    /* Cell records (one per half of a tier-4-eligible face in a near-variant
     * or poly-anim sector), counted in RECORDS: a record stands in for the
     * 96 near-variant or 48 anim vertices its eight cells would bake. */
    [OPTC_ARENA_CN_OPAQUE] = 16384,
    [OPTC_ARENA_CN_ALPHA]  =  1024,
    [OPTC_ARENA_CN_ADD]    =  1024,
    [OPTC_ARENA_CN_SUB]    =  1024,
    [OPTC_ARENA_CA_OPAQUE] =  4096,
    [OPTC_ARENA_CA_ALPHA]  =  1024,
    [OPTC_ARENA_CA_ADD]    =  1024,
    [OPTC_ARENA_CA_SUB]    =  1024,
};

#define TC_FREE_MAX   1024   /* free-list spans per arena */
#define TC_DEFER_MAX  1024   /* deferred releases across arenas */
/* Freed ranges stay unreused for this many BUILDS: covers both the RHI
 * frames-in-flight window and the one-build presentation lag (the LIVE
 * state may still generation-reference a chunk the newest build replaced). */
#define TC_DEFER_PRESENTS 4

#define TC_BUF_MAX 2   /* an arena's primary buffer plus one overflow */

/* Colour/depth slots of a cell-record pipeline set. No FILL: cells are HP
 * faces, and the hole-fill backing is LP only. */
enum { TC_CP_WORLD, TC_CP_MARK, TC_CP_ALPHA, TC_CP_ADD, TC_CP_SUB,
       TC_CP_PREPASS, TC_CP_PREPASS_DEPTH, TC_CP_KEY, TC_CP_N };

typedef struct { uint32_t first, count; } tc_span;

/* One arena's device buffers. Immutable once published: the level-edge
 * replacement swaps the whole struct through the arena's atomic pointer, so
 * a present can never bind a new `vb` beside the outgoing `pv` (two separate
 * stores would leave that window open). The retire entry is the pair. */
typedef struct {
    rhi_buffer *vb;
    rhi_buffer *pv;                   /* anim arenas: parallel prev-tick buffer
                                       * near arenas: parallel ctx buffer      */
    uint32_t    cap;
    uint32_t    serial;               /* nonzero, distinct per pair made: the
                                       * identity a state's marks are checked
                                       * against (tc_state.mark_pair) */
} tc_pair;

/* One buffer of an arena and its allocator. Each buffer numbers its vertices
 * from 0 and is drawn by its own rhi_draw, so a chunk is addressed by the
 * (buffer, first) pair everywhere. */
typedef struct {
    _Atomic(tc_pair *) pair;          /* live buffers; swapped at a level edge */
    uint32_t    cap;                  /* builder's mirror of pair->cap         */
    uint32_t    high;                 /* high-water vertex count (draw range) */
    tc_span     free_list[TC_FREE_MAX];
    int         free_n;
    uint32_t    used;                 /* live verts (census) */
    uint32_t    chunks;               /* live chunk count (census) */
} tc_buf;

typedef struct {
    tc_buf      buf[TC_BUF_MAX];
    _Atomic int n;                    /* buffers in use: 1, or 2 once an
                                       * overflow buffer is installed         */
    int         ovf_state;            /* builder-owned; TC_OVF_*              */
} tc_arena;

typedef struct { uint8_t arena, buf; uint32_t first, count, stamp; } tc_defer;

/* std140 globals block (mirrors terrain.vert TerrainGlobals). Camera /
 * projection / fog scalars are DOUBLE - the vertex stage re-runs the CPU
 * walk's double GTE chain bit-for-bit. C natural layout == std140 here
 * (vec4 x3 = 48 B, then six 16 B-aligned dvec2, then ivec4 at 144). */
typedef struct {
    float mrow0[4];    /* M[0][0..2], pad */
    float mrow1[4];    /* M[1][0..2], pad */
    float mrow2[4];    /* M[2][0..2], pad */
    double dcam_xy[2]; /* camx, camy      */
    double dcam_zf[2]; /* camz, fogscale  */
    double dof_xy[2];  /* ofx, ofy        */
    double dH[2];      /* H, poly-anim alpha */
    double dnear[2];   /* x: near-plane view-Z (near-stage clip); y unused */
    double dspare[2];  /* x: H_canon (limit-regime mirrors); y: env-anim colour alpha */
    int32_t fog[4];    /* fogRefBase, gflags, cullRefS2, ot_terms */
    float sdv[128][4]; /* per-tile scroll V offsets (lq, hqA, hqB, 0) */
    /* Portal inner-rim cull frames (portal_rim_classify mirror; all fp64).
     * Offset here is 2208 = 32-aligned, so the dvec4 run below keeps
     * C natural layout == std140 (dvec4 base alignment 32, dvec2 array
     * stride 16 == double[2]). rim_cnt.x = plane count (0 = test off). */
    int32_t rim_cnt[4];
    int32_t rim_pad[4];
    double  rim_oxyH[4];   /* rim ofx, ofy, H, pad                       */
    double  rim_knobs[4];  /* eps, near, reach, cos(angle)               */
    double  rim_abc[6][4]; /* screen inv-vz plane fit a,b,c; w = valid   */
    double  rim_O[6][4];   /* in-plane basis origin; w = 1: rim per fragment */
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
    /* Portal cover planes for the blended passes' fragment discard
     * (terrain.frag, portal_cover.glsl): active opening planes in screen
     * space. hd_entry ends 16- but not 32-aligned,
     * so the ivec4 first keeps the dvec4 run at std140's 32-byte base
     * alignment (C natural layout == std140 continues to hold). cov_poly
     * holds one screen point per vec4 slot, 16 slots per plane. */
    int32_t cov_cnt[4];    /* x = plane count (0 = test off); y = blended cover on */
    double  cov_margin[4]; /* x = cover margin M (world units)            */
    double  cov_abc[6][4]; /* screen inv-vz plane fit a, b, c; w = pn     */
    float   cov_bbox[6][4];/* outline screen bbox x0, y0, x1, y1          */
    float   cov_poly[96][4];/* outline points (px, py, 0, 0), 16 per plane */
    float   sdvc[128][4];  /* per-tile 4x4 near-cell row V offsets (rows 0-3) */
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
 * layout to keep matching the block the vertex stage reads - otherwise GLSL
 * pads where C does not and EVERY field past the first dvec4 shifts (garbage
 * fog/gflags, all-killed vertices). Any field added above them must therefore
 * be a multiple of 32 bytes wide, not merely 16. */
_Static_assert(offsetof(tc_globals_ubo, rim_oxyH) % 32 == 0 &&
               offsetof(tc_globals_ubo, cov_margin) % 32 == 0,
               "terrain-cache globals: a dvec4 run lost its 32-byte alignment");

/* One build's complete retained frame state. The feeder STAGES the canonical
 * build's state and it PROMOTES to live one build later - the sub-tick
 * spine presents the previous canonical extraction (the canonical-lag leg),
 * and the retained draws must ride the same timeline or animated tiles and
 * moving cameras render one tick ahead of the CPU-fed scene around them.
 * Re-extracted presents (windowed intermediates + canon-lag re-extract)
 * publish immediately: their build IS the displayed state. */
/* Draw ranges of one state: per (arena, buffer), the element spans holding
 * a chunk the state's sector flags select, in ascending order, adjacent
 * spans closer than the arena's merge gap joined. Built at frame_commit
 * from the builder's chunk registry. n < 0 means no list (a build still
 * open, or more spans than TC_RANGE_MAX): the state draws every buffer's
 * whole high-water range. Must stay the last member of tc_state, and `r`
 * the last member here: tc_state_copy copies only the n spans in use. */
#define TC_RANGE_MAX 4096
typedef struct {
    int      n;
    uint16_t off[OPTC_ARENA_COUNT * TC_BUF_MAX + 1]; /* first span of (arena, buf) */
    tc_span  r[TC_RANGE_MAX];
} tc_ranges;

typedef struct {
    tc_globals_ubo g;
    tc_sector_ubo  sec[OPTC_SLOTS];
    uint32_t       stamp;             /* openpete_frame of the build, or a scene id */
    int            valid;
    uint32_t       build_serial;      /* present_ctr at the build: ranges freed at a
                                       * later serial may still be drawn by this state */
    /* Each buffer's high-water mark as the builder left it for this build
     * (tc_state_mark), and the serial of the pair that mark counts in (0 =
     * no buffer). The draw ranges are bounded by these, never by the live
     * marks the builder grows for later builds while a present draws this
     * one, and a buffer whose live pair is not the marked one holds none of
     * this build's chunks. */
    uint32_t       mark_high[OPTC_ARENA_COUNT][TC_BUF_MAX];
    uint32_t       mark_pair[OPTC_ARENA_COUNT][TC_BUF_MAX];
    tc_ranges      rl;                /* last: see tc_ranges */
} tc_state;

/* One chunk in the builder's registry: its element span in one arena
 * buffer and the sector slot and generation its elements carry in
 * face_flags. `uniform` is 0 when the elements disagree on either, and such
 * a chunk is drawn by every state. */
typedef struct {
    uint32_t first, count;
    uint16_t slot;
    uint8_t  gen, uniform;
} tc_chunk;

/* Builder-owned chunk registry of one arena buffer, sorted by `first`. */
typedef struct {
    tc_chunk *c;
    int       n, cap;
} tc_chunk_list;

/* Eager scene ring: one state per scene id, keyed by the id the builder
 * stamped it with. A build of an id that already holds a state reuses that
 * state (the rebuild of a scene the ring refused), so the live states are at
 * most one per unretired scene. Sized at the scene ring plus the build in
 * progress plus the refused scene the accumulator holds, so a build always
 * finds a state no unretired scene carries. */
#define TC_RING (OPENPETE_NATIVE_SCENE_RING + 2)

/* The terrain cache's pipelines (tc_make_pipes); a member is NULL when its
 * create failed. */
typedef struct {
    rhi_pipeline *pipe_world;         /* opaque, plain                        */
    rhi_pipeline *pipe_world_mark;    /* opaque + stencil MARK (hole-fill on) */
    rhi_pipeline *pipe_alpha;
    rhi_pipeline *pipe_add;
    rhi_pipeline *pipe_sub;
    rhi_pipeline *pipe_fill;          /* stencil TEST_ZERO                    */
    rhi_pipeline *pipe_prepass;       /* pre-pass WORLD: depth + packed depth in colour */
    rhi_pipeline *pipe_prepass_depth; /* pre-pass full footprint: depth only (colour off) */
    rhi_pipeline *pipe_key;           /* og OT-key emit (pass_sel 2)          */
    /* terrain_anim variants (binding-1 prev twin + vertex-stage lerp). */
    rhi_pipeline *pipe_a_world, *pipe_a_world_mark;
    rhi_pipeline *pipe_a_alpha, *pipe_a_add, *pipe_a_sub;
    rhi_pipeline *pipe_a_prepass, *pipe_a_prepass_depth, *pipe_a_key;
    rhi_pipeline *pipe_a_fill;        /* anim LP hole-fill: env-anim colour
                                       * sectors fill, poly-anim sectors do
                                       * not (their SF_FILLLP stays clear)   */
    /* terrain_near variants (binding-1 member-context twin + the vertex-stage
     * near-plane clip fan). Full set: near sectors carry every draw class. */
    rhi_pipeline *pipe_n_world, *pipe_n_world_mark;
    rhi_pipeline *pipe_n_alpha, *pipe_n_add, *pipe_n_sub;
    rhi_pipeline *pipe_n_fill, *pipe_n_prepass, *pipe_n_prepass_depth, *pipe_n_key;
    /* terrain_cells variants (instanced cell records): the near-variant
     * sectors' set (clip-fan stage) and the poly-anim sectors' set (lerp
     * stage), indexed by TC_CP_*. */
    rhi_pipeline *pipe_cn[TC_CP_N], *pipe_ca[TC_CP_N];
} tc_pipe_set;

static struct {
    rhi_device   *dev;
    int           ready;
    tc_arena      arena[OPTC_ARENA_COUNT];
    rhi_ubo      *ubo;                /* block 0 = globals, block 1 = sectors */
    tc_pipe_set   ps;                 /* the pipelines (tc_make_pipes)        */
    uint32_t      cell_draws;         /* cell-arena buffer draws (census)     */
    uint64_t      cell_instances;     /* records in those buffers' high-water
                                       * ranges                               */
    rhi_texture  *hd_dummy;           /* 1x1 stand-in for the set-4 atlas     */
    tc_state      staged;             /* canonical build awaiting its present */
    tc_state      live;               /* what presents render                 */
    tc_state      ring[TC_RING];      /* per-scene states (eager); stamp = scene id */
    /* ring[i]'s identity for the compose-side lookups (tc_state_for_stamp,
     * tc_dup_probe): build_serial << 32 | stamp, 0 while ring[i] holds no
     * state. The builder rewrites ring states while another thread composes
     * against a different one, so the compose side reads only these words;
     * the content of the state a scene's stamp names reaches it through
     * that scene's publish. */
    _Atomic uint64_t ring_key[TC_RING];
    tc_state      hold[OPENPETE_TERCACHE_HOLDS]; /* copies kept for held scenes (openpete_tercache_hold_take) */
    unsigned      hold_takes, hold_misses, hold_drops; /* hold census */
    int           ring_next;
    unsigned      ring_builds;        /* scene-id builds that took a ring state */
    unsigned      ring_reuses;        /* of those, rebuilds of an id holding a state */
    unsigned      ring_overwrites;    /* builds that overwrote a live scene's state */
    unsigned      ring_dup_selects;   /* selects of a stamp held by several states
                                       * that took an older build than the newest */
    tc_state      sel;                /* frame_select's snapshot of live: the
                                       * present's draws may execute VBLs after
                                       * selection (deferred screenshot
                                       * readback), by which time live has been
                                       * re-promoted - flushing live at draw
                                       * time would pair old chunks with a
                                       * fresher camera/fog/colour-generation
                                       * set (rung flips, sub-pixel skew) */
    tc_sector_ubo *build_target;      /* where sector_set writes this build   */
    tc_state     *build_state;        /* the state build_target belongs to    */
    tc_chunk_list reg[OPTC_ARENA_COUNT][TC_BUF_MAX]; /* builder's chunk registry */
    int           reg_broken;         /* a registry insert failed: no range lists */
    /* Submission census: per selected present, the elements the draws
     * submitted and the elements drawing each buffer's whole high-water
     * range would have submitted (vertices; cell records times their
     * per-instance vertices), and the draw calls issued. */
    uint64_t      cen_presents, cen_verts, cen_verts_whole, cen_draws;
    uint32_t      cen_nolist;         /* draws of a state with no range list  */
    int           draw_open;
    int  sel_src;      /* 0 closed, 1 scene ring, 2 live, 3 staged */          /* frame_select verdict for this present */
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
    _Atomic uint32_t draws_last; /* accumulating: buffer draws since the last
                                  * build began (present thread adds, the
                                  * builder takes it at frame_begin)     */
    uint32_t      draws_prev;   /* the draws taken at the last frame_begin (census) */
    /* ---- level-edge arena replacement (see the header's contract) ----
     * One replacement is in flight at a time, as a three-state handshake
     * between the builder and the device-owning thread. IDLE: the builder
     * may post a request. READY: present_service has allocated rz_new and
     * the builder has not adopted yet. RETIRE: the builder has adopted and
     * rz_old awaits its drain floors. Each transition is a release store
     * paired with the other thread's acquire load, which is what publishes
     * the rz_new / rz_old arrays across the two threads. */
    _Atomic int      rz_req;      /* builder -> device: a level edge happened */
    _Atomic int      rz_state;
    _Atomic int      level_id;    /* the level the arenas are filled for      */
    uint32_t         alloc_epoch; /* steps at every buffer the arenas gain    */
    int              rz_alloc_logged;
    tc_pair         *rz_new[OPTC_ARENA_COUNT];
    /* The pairs one replacement retires: the arena's live buffers, plus the
     * slot at TC_BUF_MAX for an overflow buffer the device answered that no
     * frame_begin got to install. */
    tc_pair         *rz_old[OPTC_ARENA_COUNT][TC_BUF_MAX + 1];
    uint32_t         rz_old_build, rz_old_svc;  /* drain floors, at adoption  */
    /* ---- overflow buffers (see the header's contract) ----
     * Two bitmasks over the arenas carry one request each way. The builder
     * writes ovf_want under its request bit and reads ovf_new under the
     * answer bit; each bit is set with a release store and taken with an
     * acquiring exchange, which is what publishes the two arrays across the
     * threads. */
    _Atomic uint32_t ovf_req;     /* builder -> device: arenas wanting a buffer */
    _Atomic uint32_t ovf_done;    /* device -> builder: requests answered       */
    uint32_t         ovf_want[OPTC_ARENA_COUNT];   /* vertices asked for        */
    uint32_t         ovf_req_svc[OPTC_ARENA_COUNT];   /* service call at request */
    uint32_t         ovf_req_build[OPTC_ARENA_COUNT]; /* build at request        */
    tc_pair         *ovf_new[OPTC_ARENA_COUNT];    /* answer (NULL = refused)   */
    int              ovf_fail_logged[OPTC_ARENA_COUNT];
    uint32_t         overflow_adopted[OPTC_ARENA_COUNT];
    _Atomic uint32_t build_ctr;   /* present_ctr, published for the device    */
    _Atomic uint32_t svc_calls, svc_fulfilled;
    _Atomic uint32_t draws_retired;
    uint32_t         alloc_fail[OPTC_ARENA_COUNT];
} s_tc;

enum { TC_RZ_IDLE = 0, TC_RZ_READY = 1, TC_RZ_RETIRE = 2 };
/* Builder-side overflow state per arena. REQ and FAILED both stop further
 * requests; FAILED until the next level edge, REQ until the answer lands. */
enum { TC_OVF_IDLE = 0, TC_OVF_REQ = 1, TC_OVF_LIVE = 2, TC_OVF_FAILED = 3 };

/* The last stats before teardown. shutdown zeroes the module's state, so a
 * census printed at process exit would otherwise read every counter as 0
 * whichever teardown ran first. */
static openpete_tercache_stats_t s_tc_final;
static int s_tc_final_valid;


/** @brief Load the current buffer pair of one arena buffer.
 *
 * Used by the chunk writes and the dead-stamp release. The draw path loads
 * the pointer itself (tc_draw_buf) so one draw reads one pair.
 * @param arena Arena index.
 * @param buf Buffer index within the arena.
 * @return The pair, or NULL when that buffer holds none. */
static const tc_pair *tc_live_pair(int arena, int buf)
{
    return atomic_load_explicit(&s_tc.arena[arena].buf[buf].pair, memory_order_acquire);
}

/** @brief Return the measured capacity of one arena for one level.
 *
 * The generated table carries a row per measured level id and a union row
 * over every level; an id with no row, including the -1 init passes before
 * any level has loaded, takes the union row, and an arena with a zero union
 * entry falls back to k_arena_cap.
 * @param level_id Level id, or -1.
 * @param arena Arena index.
 * @return Capacity in vertices, or in records on a cell arena. */
static uint32_t tc_level_cap(int level_id, int arena)
{
    for (int i = 0; i < k_optc_caps_level_n; i++)
        if (k_optc_caps_level[i].level_id == level_id)
            return k_optc_caps_level[i].cap[arena];
    return k_optc_caps_union[arena] ? k_optc_caps_union[arena] : k_arena_cap[arena];
}

/** @brief Return the capacity an arena's primary buffer is allocated at.
 *
 * Without OPENPETE_TC_INITIAL_CAP this is tc_level_cap. The variable, read
 * once, replaces it with one vertex count for every vertex arena and a
 * sixteenth of it (at least 1) for every cell-record arena; a record
 * replaces 48 or 96 baked vertices, so the sixteenth still binds after the
 * vertex arenas do. A measuring run uses it to size the primaries past any
 * content and read an uncensored high-water mark, or to squeeze them so the
 * overflow path runs. The overflow buffer is sized by tc_level_cap, not by
 * this.
 * @param level_id Level id, or -1 before any level has loaded.
 * @param arena Arena index.
 * @return Capacity in vertices, or in records on a cell arena. */
static uint32_t tc_initial_cap(int level_id, int arena)
{
    static long ov = -1;
    if (ov < 0) {
        const char *e = getenv("OPENPETE_TC_INITIAL_CAP");
        ov = (e && *e) ? strtol(e, NULL, 10) : 0;
        if (ov < 0) ov = 0;
    }
    if (ov <= 0) return tc_level_cap(level_id, arena);
    if (OPTC_ARENA_IS_CELL(arena)) return ov >= 16 ? (uint32_t)(ov / 16) : 1u;
    return (uint32_t)ov;
}

/** @brief Return the byte size of one element of an arena's main buffer.
 *
 * @param arena Arena index.
 * @return sizeof a cell record on the cell arenas (anim or near layout),
 *         else sizeof(rhi_terrain_vertex). */
static size_t tc_elem_size(int arena)
{
    if (OPTC_ARENA_IS_CELL_ANIM(arena)) return sizeof(rhi_terrain_cell_anim);
    if (OPTC_ARENA_IS_CELL(arena))      return sizeof(rhi_terrain_cell);
    return sizeof(rhi_terrain_vertex);
}

/** @brief Return the device bytes one buffer pair of an arena holds at a capacity.
 *
 * The stride is the element size plus the twin's (the prev-tick record on
 * the anim arenas, the near-context record on the near arenas). Feeds the
 * cap_bytes census of openpete_tercache_get_stats.
 * @param arena Arena index.
 * @param cap Buffer capacity in elements.
 * @return Bytes of the main buffer and its twin together. */
static uint64_t tc_pair_bytes(int arena, uint32_t cap)
{
    size_t stride = tc_elem_size(arena);
    if (OPTC_ARENA_IS_ANIM(arena))      stride += sizeof(rhi_terrain_prev);
    else if (OPTC_ARENA_IS_NEAR(arena)) stride += sizeof(rhi_terrain_nearctx);
    return (uint64_t)cap * stride;
}

/** @brief Destroy a buffer pair's device buffers and free the pair.
 *
 * Device-owning thread only. The caller must know no in-flight draw still
 * reads the buffers (see tc_arena_free_all).
 * @param dev Device the buffers were created on.
 * @param pr Pair to free; NULL is ignored. */
static void tc_pair_free(rhi_device *dev, tc_pair *pr)
{
    if (!pr) return;
    if (pr->vb) rhi_destroy_buffer(dev, pr->vb);
    if (pr->pv) rhi_destroy_buffer(dev, pr->pv);
    free(pr);
}

/** @brief Allocate one arena buffer pair on the device.
 *
 * Device-owning thread only. The twin is the anim arenas' previous-tick
 * buffer or the near arenas' emitted-tri context buffer, at the same element
 * count and its own stride; a base arena and a cell arena have none. A pair
 * missing any buffer it owes is freed. Each pair takes the next serial of a
 * process-wide counter, skipping 0.
 * @param dev Device.
 * @param arena Arena index.
 * @param cap Capacity in elements.
 * @return The pair, or NULL on an allocation failure. */
static tc_pair *tc_pair_make(rhi_device *dev, int arena, uint32_t cap)
{
    static uint32_t s_serial;
    tc_pair *pr = (tc_pair *)calloc(1, sizeof(*pr));
    if (!pr) return NULL;
    pr->cap = cap;
    if (++s_serial == 0) s_serial = 1;
    pr->serial = s_serial;
    pr->vb = rhi_create_vertex_buffer_device(dev, (size_t)cap * tc_elem_size(arena));
    if (OPTC_ARENA_IS_ANIM(arena))
        pr->pv = rhi_create_vertex_buffer_device(dev, (size_t)cap * sizeof(rhi_terrain_prev));
    else if (OPTC_ARENA_IS_NEAR(arena))
        pr->pv = rhi_create_vertex_buffer_device(dev, (size_t)cap * sizeof(rhi_terrain_nearctx));
    const int twin_owed = OPTC_ARENA_IS_ANIM(arena) || OPTC_ARENA_IS_NEAR(arena);
    if (!pr->vb || (twin_owed && !pr->pv)) { tc_pair_free(dev, pr); return NULL; }
    return pr;
}

/** @brief Free every buffer pair one arena owns and clear the pointers.
 *
 * The pairs are its live buffers and the ones its two handshakes may still
 * hold: the replacement's fulfilled-but-unadopted pair, its retire list, and
 * an overflow buffer the builder never installed. Teardown
 * (openpete_tercache_shutdown) is the only caller; while the cache runs,
 * each handshake frees its own pairs at the point that proves no frame reads
 * them (tc_retire_drain for the replacement, tc_pair_make's own cleanup for
 * an allocation that failed half-made).
 * @param dev Device.
 * @param arena Arena index. */
static void tc_arena_free_all(rhi_device *dev, int arena)
{
    for (int b = 0; b < TC_BUF_MAX; b++) {
        tc_buf *bf = &s_tc.arena[arena].buf[b];
        tc_pair_free(dev, atomic_load_explicit(&bf->pair, memory_order_relaxed));
        atomic_store_explicit(&bf->pair, NULL, memory_order_relaxed);
    }
    for (int b = 0; b <= TC_BUF_MAX; b++) {
        tc_pair_free(dev, s_tc.rz_old[arena][b]);
        s_tc.rz_old[arena][b] = NULL;
    }
    tc_pair_free(dev, s_tc.rz_new[arena]);  s_tc.rz_new[arena]  = NULL;
    tc_pair_free(dev, s_tc.ovf_new[arena]); s_tc.ovf_new[arena] = NULL;
}

/* push-constant block (mirrors terrain.vert TerrainPC) */
typedef struct {
    float   x_base, x_span, y_base, y_span;
    float   disp_w, clip_h, band, freeze_wf;
    int32_t pass_sel, full_extent;
    int32_t mat_pass;      /* -1 = every face, 0 = stock, >0 = that material's pass */
    int32_t snap;          /* 1 = integer vertex precision (screen-point floor) */
    int32_t cover;         /* 1 = portal cover discard (terrain.frag) */
    int32_t pad_[3];
    float   fc[4];         /* gl_FragCoord -> PSX screen affine, read by the portal discards */
    uint32_t cls_mask[8];  /* mat_pass >= 0: bit c keeps the faces of resolution class c */
} tc_pc;
_Static_assert(sizeof(tc_pc) == 112, "tc_pc mirrors terrain.vert TerrainPC");

/* --vertex-precision integer, latched once per RHI frame (below) so every
 * arena draw of one present agrees. The renderer's snap state is live: a
 * mid-present flip would otherwise split a frame between a snapped and an
 * unsnapped half of the same terrain, along the sector-residency boundary. */
static int s_tc_snap;

int openpete_tercache_ready(void) { return s_tc.ready; }
int openpete_tercache_frame_open(void) { return s_tc.draw_open; }
int openpete_tercache_frame_source(void) { return s_tc.sel_src; }
void openpete_tercache_frame_slots(char *out, int cap)
{
    int off = 0;
    for (int i = 0; i < OPTC_SLOTS && off < cap - 24; i++) {
        union { float f; uint32_t u; } w; w.f = s_tc.sel.sec[i].o[3];
        if (!(w.u & OPTC_SF_DRAWN)) continue;
        off += snprintf(out + off, (size_t)(cap - off), " %d:%03x", i, (unsigned)(w.u & 0xFFF));
    }
    if (off == 0) out[0] = 0;
}
void openpete_tercache_frame_globals(char *out, int cap)
{
    const tc_globals_ubo *g = &s_tc.sel.g;
    snprintf(out, (size_t)cap, "cam=%.0f,%.0f,%.0f of=%.1f,%.1f H=%.1f near=%.1f fog=%d gfl=%08x cull=%d m0=%.3f,%.3f,%.3f m2=%.3f,%.3f,%.3f rim=%d cov=%d",
             g->dcam_xy[0], g->dcam_xy[1], g->dcam_zf[0], g->dof_xy[0], g->dof_xy[1], g->dH[0], g->dnear[0],
             g->fog[0], (unsigned)g->fog[1], g->fog[2], g->mrow0[0], g->mrow0[1], g->mrow0[2], g->mrow2[0], g->mrow2[1], g->mrow2[2],
             g->rim_cnt[0], g->cov_cnt[0]);
}
void openpete_tercache_frame_stats(int *drawn, int *hp, int *lp, int *nearvar, int gens[8])
{
    *drawn = *hp = *lp = *nearvar = 0;
    for (int g = 0; g < 8; g++) gens[g] = 0;
    if (!s_tc.draw_open) return;
    for (int i = 0; i < OPTC_SLOTS; i++) {
        union { float f; uint32_t u; } w; w.f = s_tc.sel.sec[i].o[3];
        if (!(w.u & OPTC_SF_DRAWN)) continue;
        (*drawn)++;
        if (w.u & OPTC_SF_DRAWHP) (*hp)++;
        if (w.u & OPTC_SF_DRAWLP) (*lp)++;
        if (w.u & OPTC_SF_NEARVAR) (*nearvar)++;
        gens[(w.u >> 12) & 7u]++;
    }
}

static const tc_state *tc_state_for_stamp(uint32_t stamp, int *src);
static void tc_state_copy(tc_state *dst, const tc_state *src);

/** @brief Test whether a retained state holds a live sector: a nonzero flag word on any slot but the dead one.
 * @param T The state.
 * @return 1 when a sector is published, else 0. */
static int tc_state_content(const tc_state *T)
{
    for (int i = 0; i < OPTC_SLOTS; i++)
        if (i != (int)OPTC_SLOT_DEAD && T->sec[i].o[3] != 0.0f) return 1;
    return 0;
}

int openpete_tercache_frame_content(void)
{
    if (!s_tc.ready) return -1;
    if (!s_tc.draw_open) return 0;
    return tc_state_content(&s_tc.sel);
}

int openpete_tercache_build_content(uint32_t stamp)
{
    if (!s_tc.ready) return -1;
    int src = 0;
    const tc_state *T = tc_state_for_stamp(stamp, &src);
    return T ? tc_state_content(T) : 0;
}

int openpete_tercache_build_drawn(uint32_t stamp)
{
    if (!s_tc.ready) return -1;
    int src = 0;
    const tc_state *T = tc_state_for_stamp(stamp, &src);
    if (!T) return 0;
    int n = 0;
    for (int i = 0; i < OPTC_SLOTS; i++) {
        union { float f; uint32_t u; } w; w.f = T->sec[i].o[3];
        n += (w.u & OPTC_SF_DRAWN) != 0;
    }
    return n;
}

void openpete_tercache_hold_take(int idx, uint32_t stamp)
{
    if (idx < 0 || idx >= OPENPETE_TERCACHE_HOLDS) return;
    s_tc.hold[idx].valid = 0;
    if (!s_tc.ready) return;
    int src = 0;
    const tc_state *T = tc_state_for_stamp(stamp, &src);
    if (!T || src == 4) { s_tc.hold_misses++; return; }
    tc_state_copy(&s_tc.hold[idx], T);
    s_tc.hold_takes++;
}

void openpete_tercache_hold_drop(int idx)
{
    if (idx >= 0 && idx < OPENPETE_TERCACHE_HOLDS) s_tc.hold[idx].valid = 0;
}

void openpete_tercache_hold_counts(unsigned *takes, unsigned *misses, unsigned *drops)
{
    if (takes) *takes = s_tc.hold_takes;
    if (misses) *misses = s_tc.hold_misses;
    if (drops) *drops = s_tc.hold_drops;
}

/** @brief Create one cache pipeline into its slot: now, or at the end of the caller's open pipeline batch.
 *
 * The create is the one rhi_create_pipeline makes; the slot is NULL until
 * it installs, and stays NULL when it fails.
 * @param dev Device.
 * @param d Pipeline description, copied.
 * @param out The slot. */
static void tc_pipe(rhi_device *dev, const rhi_pipeline_desc *d, rhi_pipeline **out)
{
    *out = NULL;
    (void)rhi_create_pipeline_slot(dev, d, NULL, 0, NULL, 0, (void **)out, NULL);
}

/** @brief Build one cell-record pipeline set, one pipeline per TC_CP_* slot.
 *
 * The slots share terrain.frag and depth test and differ as the world
 * pipelines do: stencil MARK, the three blend equations with depth writes
 * off, the colour-off depth prepass, and the OT-key emit. Each create is
 * made now, or at the end of the open pipeline batch (tc_cell_pipes_ok
 * then reads the set).
 * @param dev Device.
 * @param vert Vertex stage SPIR-V name (the near or anim cell stage).
 * @param anim Nonzero for the anim record layout, 0 for the near layout.
 * @param out Receives the pipelines; a failed slot is NULL. */
static void tc_make_cell_pipes(rhi_device *dev, const char *vert, int anim,
                               rhi_pipeline *out[TC_CP_N])
{
    const rhi_pipeline_desc base = {
        .vert_spv = vert, .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .terrain_cells = true,
        .terrain_anim = anim != 0, .terrain_near = anim == 0,
        .pair_set7 = true, .push_const_size = sizeof(tc_pc),
    };
    rhi_pipeline_desc d[TC_CP_N];
    for (int i = 0; i < TC_CP_N; i++) d[i] = base;
    d[TC_CP_MARK].stencil = RHI_STENCIL_MARK;
    d[TC_CP_ALPHA].blend = RHI_BLEND_ALPHA;    d[TC_CP_ALPHA].depth_write_off = true;
    d[TC_CP_ADD].blend   = RHI_BLEND_ADD;      d[TC_CP_ADD].depth_write_off = true;
    d[TC_CP_SUB].blend   = RHI_BLEND_SUBTRACT; d[TC_CP_SUB].depth_write_off = true;
    d[TC_CP_PREPASS].frag_spv = "terrain_prepass_cutout.frag.spv";   /* colour = packed depth */
    d[TC_CP_PREPASS].depth_class = RHI_DEPTH_FORWARD;
    d[TC_CP_PREPASS_DEPTH].frag_spv = "terrain_prepass.frag.spv";
    d[TC_CP_PREPASS_DEPTH].color_write_off = true;
    d[TC_CP_PREPASS_DEPTH].depth_class = RHI_DEPTH_FORWARD;
    d[TC_CP_KEY].frag_spv = "terrain_key_cache.frag.spv";
    d[TC_CP_KEY].depth_class = RHI_DEPTH_FORWARD;
    for (int i = 0; i < TC_CP_N; i++) tc_pipe(dev, &d[i], &out[i]);
}

/** @brief Report whether every slot of a cell-record pipeline set built.
 *
 * @param set The set tc_make_cell_pipes filled.
 * @return 1 when every slot holds a pipeline, else 0. */
static int tc_cell_pipes_ok(rhi_pipeline *const set[TC_CP_N])
{
    int ok = 1;
    for (int i = 0; i < TC_CP_N; i++) ok &= set[i] != NULL;
    return ok;
}

/** @brief Create every terrain cache pipeline into a set: now, or at the end of the caller's open pipeline batch.
 *
 * The world, anim, near and cell-record families, each in every draw
 * class. Inside rhi_pipeline_describe_begin each create is recorded
 * instead (openpete_tercache_describe).
 * @param dev Device.
 * @param ps Receives the pipelines; a failed slot is NULL. */
static void tc_make_pipes(rhi_device *dev, tc_pipe_set *ps)
{
    const rhi_pipeline_desc world_desc = {
        .vert_spv = "terrain.vert.spv", .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .pair_set7 = true,
        .push_const_size = sizeof(tc_pc),
    };
    rhi_pipeline_desc d;
    d = world_desc;
    tc_pipe(dev, &d, &ps->pipe_world);
    d = world_desc; d.stencil = RHI_STENCIL_MARK;
    tc_pipe(dev, &d, &ps->pipe_world_mark);
    d = world_desc; d.blend = RHI_BLEND_ALPHA; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_alpha);
    d = world_desc; d.blend = RHI_BLEND_ADD; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_add);
    d = world_desc; d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_sub);
    d = world_desc; d.stencil = RHI_STENCIL_TEST_ZERO;
    tc_pipe(dev, &d, &ps->pipe_fill);
    /* Pre-pass pair: the WORLD run writes its packed depth to colour (the
     * occlusion grid's source) where terrain.frag would paint, its
     * transparent texels cut out (terrain_prepass_cutout.frag); the depth
     * run writes depth only, over whole footprints. */
    d = world_desc; d.frag_spv = "terrain_prepass_cutout.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_prepass);
    d.frag_spv = "terrain_prepass.frag.spv"; d.color_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_prepass_depth);
    d = world_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_key);
    /* Anim variants: identical state, the anim vertex stage + the
     * binding-1 prev twin. The FILL variant serves the env-anim colour
     * sectors' LP hole-fill backing; a poly-anim sector never publishes
     * SF_FILLLP, so the vertex stage kills its LP in that pass. */
    const rhi_pipeline_desc anim_desc = {
        .vert_spv = "terrain_anim.vert.spv", .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .terrain_anim = true, .pair_set7 = true,
        .push_const_size = sizeof(tc_pc),
    };
    d = anim_desc;
    tc_pipe(dev, &d, &ps->pipe_a_world);
    d = anim_desc; d.stencil = RHI_STENCIL_MARK;
    tc_pipe(dev, &d, &ps->pipe_a_world_mark);
    d = anim_desc; d.blend = RHI_BLEND_ALPHA; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_a_alpha);
    d = anim_desc; d.blend = RHI_BLEND_ADD; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_a_add);
    d = anim_desc; d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_a_sub);
    d = anim_desc; d.frag_spv = "terrain_prepass_cutout.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_a_prepass);
    d.frag_spv = "terrain_prepass.frag.spv"; d.color_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_a_prepass_depth);
    d = anim_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_a_key);
    d = anim_desc; d.stencil = RHI_STENCIL_TEST_ZERO;
    tc_pipe(dev, &d, &ps->pipe_a_fill);
    /* Near variants: identical state, the clip-fan vertex stage + the
     * binding-1 member-context twin. Includes FILL - near sectors keep the
     * LP hole-fill backing (unlike poly-anim, which is excluded from it). */
    const rhi_pipeline_desc near_desc = {
        .vert_spv = "terrain_near.vert.spv", .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .terrain_near = true, .pair_set7 = true,
        .push_const_size = sizeof(tc_pc),
    };
    d = near_desc;
    tc_pipe(dev, &d, &ps->pipe_n_world);
    d = near_desc; d.stencil = RHI_STENCIL_MARK;
    tc_pipe(dev, &d, &ps->pipe_n_world_mark);
    d = near_desc; d.blend = RHI_BLEND_ALPHA; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_n_alpha);
    d = near_desc; d.blend = RHI_BLEND_ADD; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_n_add);
    d = near_desc; d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_n_sub);
    d = near_desc; d.stencil = RHI_STENCIL_TEST_ZERO;
    tc_pipe(dev, &d, &ps->pipe_n_fill);
    d = near_desc; d.frag_spv = "terrain_prepass_cutout.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_n_prepass);
    d.frag_spv = "terrain_prepass.frag.spv"; d.color_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_n_prepass_depth);
    d = near_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_n_key);

    /* Cell-record variants: the same states over the instanced cell stages. */
    tc_make_cell_pipes(dev, "terrain_cells_near.vert.spv", 0, ps->pipe_cn);
    tc_make_cell_pipes(dev, "terrain_cells_anim.vert.spv", 1, ps->pipe_ca);
}

void openpete_tercache_describe(rhi_device *dev)
{
    tc_pipe_set ps;
    if (dev) tc_make_pipes(dev, &ps);
}

int openpete_tercache_init(rhi_device *dev, void (*batch)(int open))
{
    memset(&s_tc, 0, sizeof(s_tc));
    s_tc.dev = dev;
    if (!dev) return 0;
    if (batch) batch(1);

    tc_make_pipes(dev, &s_tc.ps);
    if (batch) batch(0);
    const int cells_ok = tc_cell_pipes_ok(s_tc.ps.pipe_cn) & tc_cell_pipes_ok(s_tc.ps.pipe_ca);

    int ok = cells_ok && s_tc.ps.pipe_world && s_tc.ps.pipe_world_mark && s_tc.ps.pipe_alpha &&
             s_tc.ps.pipe_add && s_tc.ps.pipe_sub && s_tc.ps.pipe_fill && s_tc.ps.pipe_prepass &&
             s_tc.ps.pipe_prepass_depth && s_tc.ps.pipe_key &&
             s_tc.ps.pipe_a_world && s_tc.ps.pipe_a_world_mark && s_tc.ps.pipe_a_alpha &&
             s_tc.ps.pipe_a_add && s_tc.ps.pipe_a_sub && s_tc.ps.pipe_a_prepass &&
             s_tc.ps.pipe_a_prepass_depth && s_tc.ps.pipe_a_key && s_tc.ps.pipe_a_fill &&
             s_tc.ps.pipe_n_world && s_tc.ps.pipe_n_world_mark && s_tc.ps.pipe_n_alpha &&
             s_tc.ps.pipe_n_add && s_tc.ps.pipe_n_sub && s_tc.ps.pipe_n_fill &&
             s_tc.ps.pipe_n_prepass && s_tc.ps.pipe_n_prepass_depth && s_tc.ps.pipe_n_key;
    /* No level has loaded yet, so every arena starts on the union row. */
    for (int a = 0; ok && a < OPTC_ARENA_COUNT; a++) {
        tc_pair *pr = tc_pair_make(dev, a, tc_initial_cap(-1, a));
        if (!pr) { ok = 0; break; }
        s_tc.arena[a].buf[0].cap = pr->cap;
        atomic_store_explicit(&s_tc.arena[a].buf[0].pair, pr, memory_order_relaxed);
        atomic_store_explicit(&s_tc.arena[a].n, 1, memory_order_relaxed);
    }
    if (!ok) {
        fprintf(stderr, "[ter-gpu] init failed (pipeline or arena alloc) - "
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
    /* The UBO (block 0 globals, block 1 sectors) is a device object, so it
     * is made here on the device-owning thread rather than lazily by the
     * first build, which may run on the thread that runs the guest. */
    s_tc.ubo = rhi_create_ubo(dev, (uint32_t)TC_UBO_BLOCK, 2);
    if (!s_tc.ubo) return 0;
    s_tc.ready = 1;
    return 1;
}

/** @brief Return 0 when OPENPETE_TC_RANGE_DRAW=0 forces whole-range arena draws, else 1.
 *
 * Read once. The whole-range leg is the reference an A/B of the range
 * draws compares against: both must produce the same image. */
static int tc_range_draw_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("OPENPETE_TC_RANGE_DRAW");
        on = !(e && *e == '0');
    }
    return on;
}

void openpete_tercache_shutdown(rhi_device *dev)
{
    if (!dev) return;
    openpete_tercache_get_stats(&s_tc_final);
    s_tc_final_valid = 1;
    if (s_tc.ring_builds)
        fprintf(stderr, "[tercache] ring: %u scene build(s), %u rebuild(s) of a held id, "
                "%u overwrite(s) of a live state, %u duplicate-stamp select(s) of an older build\n",
                s_tc.ring_builds, s_tc.ring_reuses, s_tc.ring_overwrites, s_tc.ring_dup_selects);
    {
        const char *e = getenv("OPENPETE_TC_SUBMIT_CENSUS");
        if (e && *e && *e != '0' && s_tc.cen_presents)
            fprintf(stderr, "[tercache] submit census: presents=%llu "
                    "verts_per_present=%.0f whole_range_verts_per_present=%.0f "
                    "draws_per_present=%.1f nolist_draws=%u range_draw=%d\n",
                    (unsigned long long)s_tc.cen_presents,
                    (double)s_tc.cen_verts / (double)s_tc.cen_presents,
                    (double)s_tc.cen_verts_whole / (double)s_tc.cen_presents,
                    (double)s_tc.cen_draws / (double)s_tc.cen_presents,
                    s_tc.cen_nolist, tc_range_draw_on());
    }
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        for (int b = 0; b < TC_BUF_MAX; b++) free(s_tc.reg[a][b].c);
    if (s_tc.hd_dummy) { rhi_destroy_texture(dev, s_tc.hd_dummy); s_tc.hd_dummy = NULL; }
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) tc_arena_free_all(dev, a);
    rhi_pipeline **const named[] = {
        &s_tc.ps.pipe_world, &s_tc.ps.pipe_world_mark, &s_tc.ps.pipe_alpha, &s_tc.ps.pipe_add,
        &s_tc.ps.pipe_sub, &s_tc.ps.pipe_fill, &s_tc.ps.pipe_prepass, &s_tc.ps.pipe_key,
        &s_tc.ps.pipe_a_world, &s_tc.ps.pipe_a_world_mark, &s_tc.ps.pipe_a_alpha,
        &s_tc.ps.pipe_a_add, &s_tc.ps.pipe_a_sub, &s_tc.ps.pipe_a_prepass, &s_tc.ps.pipe_a_key,
        &s_tc.ps.pipe_a_fill,
        &s_tc.ps.pipe_n_world, &s_tc.ps.pipe_n_world_mark, &s_tc.ps.pipe_n_alpha,
        &s_tc.ps.pipe_n_add, &s_tc.ps.pipe_n_sub, &s_tc.ps.pipe_n_fill, &s_tc.ps.pipe_n_prepass,
        &s_tc.ps.pipe_n_key,
        &s_tc.ps.pipe_prepass_depth, &s_tc.ps.pipe_a_prepass_depth, &s_tc.ps.pipe_n_prepass_depth,
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (*named[i]) rhi_destroy_pipeline(dev, *named[i]);
    for (int i = 0; i < TC_CP_N; i++) {
        if (s_tc.ps.pipe_cn[i]) rhi_destroy_pipeline(dev, s_tc.ps.pipe_cn[i]);
        if (s_tc.ps.pipe_ca[i]) rhi_destroy_pipeline(dev, s_tc.ps.pipe_ca[i]);
    }
    if (s_tc.ubo)             rhi_destroy_ubo(dev, s_tc.ubo);
    memset(&s_tc, 0, sizeof(s_tc));
}

/* ---- allocator ---------------------------------------------------------- */

/** @brief Return a vertex range to one buffer's free list.
 *
 * The list stays sorted by first index, and the range coalesces with an
 * adjacent span on either side. A range that neither coalesces nor fits
 * (the list holds TC_FREE_MAX spans) is dropped and stays unusable until
 * the level-edge replacement resets the buffer.
 * @param a Buffer.
 * @param first First element of the range.
 * @param count Element count of the range. */
static void tc_free_insert(tc_buf *a, uint32_t first, uint32_t count)
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

/** @brief Allocate a range in one buffer: first fit in its free list, else a grow of its high-water mark.
 *
 * Updates the buffer's live-element and chunk census on success.
 * @param a Buffer.
 * @param n Element count.
 * @param out_first Receives the range's first element.
 * @return 0 on success, -1 when this buffer has no room for the range. */
static int tc_buf_alloc(tc_buf *a, uint32_t n, uint32_t *out_first)
{
    for (int i = 0; i < a->free_n; i++) {           /* first fit */
        if (a->free_list[i].count < n) continue;
        *out_first = a->free_list[i].first;
        a->free_list[i].first += n;
        a->free_list[i].count -= n;
        if (a->free_list[i].count == 0) {
            memmove(&a->free_list[i], &a->free_list[i+1],
                    (size_t)(a->free_n - i - 1) * sizeof(tc_span));
            a->free_n--;
        }
        a->used += n; a->chunks++;
        return 0;
    }
    if (a->high + n > a->cap) return -1;            /* grow high-water */
    *out_first = a->high;
    a->high += n;
    a->used += n; a->chunks++;
    return 0;
}

/** @brief Ask the device-owning thread for an overflow buffer for one arena.
 *
 * Builder thread, from a chunk every buffer of the arena refused. The size
 * asked for is the level's generated capacity, or the refused chunk where
 * that is larger, so one grant covers the rest of the level rather than one
 * sector. Dropped while this arena already has a request outstanding, an
 * overflow installed, or a refusal on record. Records the service call and
 * build counts the adoption log measures against.
 * @param arena Arena index.
 * @param verts Element count of the refused chunk. */
static void tc_request_overflow(int arena, uint32_t verts)
{
    tc_arena *a = &s_tc.arena[arena];
    if (a->ovf_state != TC_OVF_IDLE) return;
    if (atomic_load_explicit(&a->n, memory_order_relaxed) >= TC_BUF_MAX) return;
    uint32_t want = tc_level_cap(atomic_load_explicit(&s_tc.level_id,
                                                      memory_order_relaxed), arena);
    if (verts > want) want = verts;
    s_tc.ovf_want[arena] = want;
    s_tc.ovf_req_svc[arena] = atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed);
    s_tc.ovf_req_build[arena] = s_tc.present_ctr;
    a->ovf_state = TC_OVF_REQ;
    atomic_fetch_or_explicit(&s_tc.ovf_req, 1u << arena, memory_order_release);
}

/* ---- chunk registry and draw ranges -------------------------------------
 * The builder records every chunk it writes with the sector slot and
 * generation its elements carry, so that each committed state can list the
 * element spans its sector flags select and the draw can skip the rest of
 * the arena. A skipped span holds only elements the vertex stage would kill
 * for that state: a chunk whose slot is not drawn, whose generation is not
 * the published one, or whose plain/near family is not the one the slot's
 * NEARVAR flag picks, or a range no chunk occupies. */

/** @brief Return the index of the first registry entry at or after an element index.
 * @param l The registry list.
 * @param first Element index.
 * @return Insertion index in [0, l->n]. */
static int tc_reg_lower(const tc_chunk_list *l, uint32_t first)
{
    int lo = 0, hi = l->n;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (l->c[mid].first < first) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/** @brief Record a written chunk in its buffer's registry, replacing an entry at the same first element.
 *
 * On an allocation failure the registry is left unchanged and every state
 * built afterwards draws its whole ranges (tc_ranges_build finds the
 * registry marked incomplete).
 * @param arena Arena index.
 * @param buf Buffer index.
 * @param c The chunk. */
static void tc_reg_add(int arena, int buf, tc_chunk c)
{
    tc_chunk_list *l = &s_tc.reg[arena][buf];
    const int i = tc_reg_lower(l, c.first);
    if (i < l->n && l->c[i].first == c.first) { l->c[i] = c; return; }
    if (l->n == l->cap) {
        const int ncap = l->cap ? l->cap * 2 : 256;
        tc_chunk *nc = (tc_chunk *)realloc(l->c, (size_t)ncap * sizeof(*nc));
        if (!nc) { s_tc.reg_broken = 1; return; }
        l->c = nc; l->cap = ncap;
    }
    memmove(&l->c[i + 1], &l->c[i], (size_t)(l->n - i) * sizeof(*l->c));
    l->c[i] = c;
    l->n++;
}

/** @brief Drop the registry entry of a freed chunk.
 * @param arena Arena index.
 * @param buf Buffer index.
 * @param first The chunk's first element. */
static void tc_reg_remove(int arena, int buf, uint32_t first)
{
    tc_chunk_list *l = &s_tc.reg[arena][buf];
    const int i = tc_reg_lower(l, first);
    if (i >= l->n || l->c[i].first != first) return;
    memmove(&l->c[i], &l->c[i + 1], (size_t)(l->n - i - 1) * sizeof(*l->c));
    l->n--;
}

/** @brief Empty every buffer's registry, keeping the storage. */
static void tc_reg_clear(void)
{
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        for (int b = 0; b < TC_BUF_MAX; b++) s_tc.reg[a][b].n = 0;
}

/** @brief Build a registry entry from a chunk's face_flags words.
 * @param first The chunk's first element.
 * @param count Element count.
 * @param ff First face_flags word.
 * @param stride Bytes between consecutive face_flags words.
 * @return The entry; uniform when every element's slot and generation equal the first's. */
static tc_chunk tc_chunk_of(uint32_t first, int count, const uint8_t *ff, size_t stride)
{
    uint32_t f0;
    memcpy(&f0, ff, sizeof f0);
    tc_chunk c = { first, (uint32_t)count, (uint16_t)(f0 & OPTC_SLOT_DEAD),
                   (uint8_t)((f0 >> 29) & 7u), 1 };
    const uint32_t key = f0 & (OPTC_SLOT_DEAD | 0xE0000000u);
    for (int i = 1; i < count && c.uniform; i++) {
        uint32_t f;
        memcpy(&f, ff + (size_t)i * stride, sizeof f);
        if ((f & (OPTC_SLOT_DEAD | 0xE0000000u)) != key) c.uniform = 0;
    }
    return c;
}

/** @brief Return 1 when a state may draw any element of a chunk.
 *
 * Mirrors the vertex stage's sector-level kills: OPTC_SF_DRAWN clear, a
 * generation other than the slot's published one, or a near-family arena
 * (the near and near-cell arenas) under a slot without OPTC_SF_NEARVAR, or
 * a plain-family arena under one with it.
 * @param T The state.
 * @param arena Arena index.
 * @param c The chunk. */
static int tc_chunk_selected(const tc_state *T, int arena, const tc_chunk *c)
{
    if (!c->uniform) return 1;
    union { float f; uint32_t u; } w;
    w.f = T->sec[c->slot].o[3];
    const uint32_t sf = w.u;
    if (!(sf & OPTC_SF_DRAWN) || ((sf >> 12) & 7u) != c->gen) return 0;
    const int near_family = OPTC_ARENA_IS_NEAR(arena) ||
                            (OPTC_ARENA_IS_CELL(arena) && !OPTC_ARENA_IS_CELL_ANIM(arena));
    return near_family == ((sf & OPTC_SF_NEARVAR) != 0);
}

/** @brief Return the gap below which two selected spans of an arena are drawn as one.
 *
 * Joining spans draws the elements between them, which the vertex stage
 * kills as it would in a whole-range draw; the gap trades those elements
 * against a draw call.
 * @param arena Arena index.
 * @return Elements: 192 vertices, or 2 cell records. */
static uint32_t tc_range_gap(int arena)
{
    return OPTC_ARENA_IS_CELL(arena) ? 2u : 192u;
}

/** @brief Append one arena buffer's selected spans to a state's range list.
 * @param T The state.
 * @param arena Arena index.
 * @param buf Buffer index.
 * @return 0, or -1 when the list is full. */
static int tc_ranges_add_buf(tc_state *T, int arena, int buf)
{
    const tc_chunk_list *l = &s_tc.reg[arena][buf];
    const uint32_t gap = tc_range_gap(arena);
    int open = 0;
    uint32_t lo = 0, hi = 0;
    for (int i = 0; i < l->n; i++) {
        const tc_chunk *c = &l->c[i];
        if (!tc_chunk_selected(T, arena, c)) continue;
        if (open && c->first <= hi + gap) {
            if (c->first + c->count > hi) hi = c->first + c->count;
            continue;
        }
        if (open) {
            if (T->rl.n >= TC_RANGE_MAX) return -1;
            T->rl.r[T->rl.n++] = (tc_span){ lo, hi - lo };
        }
        open = 1; lo = c->first; hi = c->first + c->count;
    }
    if (open) {
        if (T->rl.n >= TC_RANGE_MAX) return -1;
        T->rl.r[T->rl.n++] = (tc_span){ lo, hi - lo };
    }
    return 0;
}

/** @brief Build a committed state's range list from the chunk registry and its sector flags.
 *
 * Leaves the list absent (n = -1) when the registry is incomplete or the
 * spans overflow TC_RANGE_MAX, so the state draws whole ranges.
 * @param T The state. */
static void tc_ranges_build(tc_state *T)
{
    T->rl.n = 0;
    if (s_tc.reg_broken) { T->rl.n = -1; return; }
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        for (int b = 0; b < TC_BUF_MAX; b++) {
            T->rl.off[a * TC_BUF_MAX + b] = (uint16_t)T->rl.n;
            if (tc_ranges_add_buf(T, a, b) != 0) { T->rl.n = -1; return; }
        }
    T->rl.off[OPTC_ARENA_COUNT * TC_BUF_MAX] = (uint16_t)T->rl.n;
}

/** @brief Copy a state, including only the range-list spans in use.
 * @param dst Destination.
 * @param src Source. */
static void tc_state_copy(tc_state *dst, const tc_state *src)
{
    const int n = src->rl.n > 0 ? src->rl.n : 0;
    memcpy(dst, src, offsetof(tc_state, rl.r) + (size_t)n * sizeof(tc_span));
}

/** @brief Record every arena buffer's high-water mark and pair serial into a state.
 *
 * Builder thread, the only writer of the marks and pairs it reads. Called
 * when a build opens its state and again at its commit, so a state carries
 * the marks covering every chunk its build allocated; buffers past the
 * arena's count record 0 for both.
 * @param T The state the build writes. */
static void tc_state_mark(tc_state *T)
{
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        const int n = atomic_load_explicit(&s_tc.arena[a].n, memory_order_relaxed);
        for (int b = 0; b < TC_BUF_MAX; b++) {
            const tc_pair *pr = b < n ? tc_live_pair(a, b) : NULL;
            T->mark_high[a][b] = pr ? s_tc.arena[a].buf[b].high : 0u;
            T->mark_pair[a][b] = pr ? pr->serial : 0u;
        }
    }
}

int openpete_tercache_chunk_alloc(int arena, int vert_count, uint32_t *out_first,
                                  int *out_buf)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT || vert_count <= 0)
        return -1;
    tc_arena *a = &s_tc.arena[arena];
    const int n = atomic_load_explicit(&a->n, memory_order_relaxed);
    for (int b = 0; b < n; b++) {
        if (tc_buf_alloc(&a->buf[b], (uint32_t)vert_count, out_first) != 0) continue;
        *out_buf = b;
        return 0;
    }
    s_tc.alloc_fail[arena]++;
    tc_request_overflow(arena, (uint32_t)vert_count);
    return -1;   /* full: caller claims the sector back to the CPU walk */
}

void openpete_tercache_chunk_write(int arena, int buf, uint32_t first,
                                   const rhi_terrain_vertex *v, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT) return;
    const tc_pair *pr = tc_live_pair(arena, buf);
    if (!pr) return;
    rhi_update_buffer_range(pr->vb, v,
                            (size_t)first * sizeof(rhi_terrain_vertex),
                            (size_t)count * sizeof(rhi_terrain_vertex));
    if (count > 0)
        tc_reg_add(arena, buf, tc_chunk_of(first, count, (const uint8_t *)&v[0].face_flags,
                                           sizeof(rhi_terrain_vertex)));
}

void openpete_tercache_chunk_write_prev(int arena, int buf, uint32_t first,
                                        const rhi_terrain_prev *p, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT) return;
    const tc_pair *pr = tc_live_pair(arena, buf);
    if (!OPTC_ARENA_IS_ANIM(arena) || !pr || !pr->pv) return;
    rhi_update_buffer_range(pr->pv, p,
                            (size_t)first * sizeof(rhi_terrain_prev),
                            (size_t)count * sizeof(rhi_terrain_prev));
}

void openpete_tercache_chunk_write_ctx(int arena, int buf, uint32_t first,
                                       const rhi_terrain_nearctx *c, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT) return;
    const tc_pair *pr = tc_live_pair(arena, buf);
    if (!OPTC_ARENA_IS_NEAR(arena) || !pr || !pr->pv) return;
    rhi_update_buffer_range(pr->pv, c,
                            (size_t)first * sizeof(rhi_terrain_nearctx),
                            (size_t)count * sizeof(rhi_terrain_nearctx));
}

void openpete_tercache_chunk_write_cells(int arena, int buf, uint32_t first,
                                         const void *recs, int count)
{
    if (!s_tc.ready || !OPTC_ARENA_IS_CELL(arena)) return;
    const tc_pair *pr = tc_live_pair(arena, buf);
    if (!pr) return;
    const size_t sz = tc_elem_size(arena);
    rhi_update_buffer_range(pr->vb, recs, (size_t)first * sz, (size_t)count * sz);
    if (count > 0)
        tc_reg_add(arena, buf, tc_chunk_of(first, count, (const uint8_t *)recs +
                                           offsetof(rhi_terrain_cell, face_flags), sz));
}

void openpete_tercache_chunk_free(int arena, int buf, uint32_t first, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT || count <= 0) return;
    tc_buf *a = &s_tc.arena[arena].buf[buf];
    tc_reg_remove(arena, buf, first);
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
            (uint8_t)arena, (uint8_t)buf, first, (uint32_t)count, s_tc.present_ctr };
    } else {
        tc_free_insert(a, first, (uint32_t)count);   /* overflow: reuse now (rare) */
    }
}

/* ---- level-edge arena replacement --------------------------------------- */

/* Builder thread: stores the level, then raises the replacement request. */
void openpete_tercache_request_resize(int level_id)
{
    if (!s_tc.ready) return;
    openpete_tercache_set_level(level_id);
    atomic_store_explicit(&s_tc.rz_req, 1, memory_order_release);
}

void openpete_tercache_set_level(int level_id)
{
    atomic_store_explicit(&s_tc.level_id, level_id, memory_order_release);
}

uint32_t openpete_tercache_alloc_epoch(void) { return s_tc.alloc_epoch; }

int openpete_tercache_resize_ready(void)
{
    return s_tc.ready &&
        atomic_load_explicit(&s_tc.rz_state, memory_order_acquire) == TC_RZ_READY;
}

/** @brief Move answered but uninstalled overflow buffers onto the replacement's retire list.
 *
 * Builder thread, from tc_adopt_pending. An overflow buffer the device
 * answered but no frame_begin installed has never been bound by a draw; it
 * still retires through the drain, the only path that destroys a pair. The
 * device answers no overflow request while a replacement is requested or
 * awaiting adoption, so every answer raised before this point is visible
 * here. Outstanding unanswered requests are dropped. */
static void tc_collect_overflow_pending(void)
{
    const uint32_t mask = atomic_exchange_explicit(&s_tc.ovf_done, 0u,
                                                   memory_order_acquire);
    atomic_store_explicit(&s_tc.ovf_req, 0u, memory_order_relaxed);
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        if (!(mask & (1u << a))) continue;
        s_tc.rz_old[a][TC_BUF_MAX] = s_tc.ovf_new[a];
        s_tc.ovf_new[a] = NULL;
    }
}

/** @brief Install every answered overflow buffer as its arena's next buffer.
 *
 * Builder thread, from frame_begin and frame_skip. A refused request (a NULL
 * answer) leaves the arena at one buffer and stops further requests until
 * the next level edge, so the sectors it could not hold stay on the CPU walk
 * instead of re-asking every build. Each adoption steps the allocation epoch
 * and logs the request-to-adoption delay in service calls and builds. */
static void tc_adopt_overflow(void)
{
    const uint32_t mask = atomic_exchange_explicit(&s_tc.ovf_done, 0u,
                                                   memory_order_acquire);
    if (!mask) return;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        if (!(mask & (1u << a))) continue;
        tc_arena *ar = &s_tc.arena[a];
        tc_pair *pr = s_tc.ovf_new[a];
        s_tc.ovf_new[a] = NULL;
        if (!pr) { ar->ovf_state = TC_OVF_FAILED; continue; }
        const int n = atomic_load_explicit(&ar->n, memory_order_relaxed);
        tc_buf *b = &ar->buf[n];
        b->cap = pr->cap; b->high = 0; b->free_n = 0; b->used = 0; b->chunks = 0;
        atomic_store_explicit(&b->pair, pr, memory_order_release);
        atomic_store_explicit(&ar->n, n + 1, memory_order_release);
        ar->ovf_state = TC_OVF_LIVE;
        s_tc.overflow_adopted[a]++;
        s_tc.alloc_epoch++;
        /* The window the claimed sectors spend on the CPU walk, in the two
         * clocks that bound it: the device thread's service calls and the
         * builder's builds. */
        fprintf(stderr, "[ter-gpu] arena %d overflow buffer of %u vertices adopted "
                        "%u service calls / %u builds after the request\n",
                a, pr->cap,
                atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed)
                    - s_tc.ovf_req_svc[a],
                s_tc.present_ctr - s_tc.ovf_req_build[a]);
    }
}

/** @brief Find the oldest build serial a state still drawn may read ranges of: the release floor.
 *
 * A range freed at build serial F was drawn by states built before F. The
 * states still drawn are the ring states whose scene is unretired (eager)
 * and the held states (openpete_tercache_hold_take); nothing freed at or
 * after the oldest of their build serials may be reused yet. With neither
 * there is no floor, and the release condition is the build-count window
 * alone.
 * @param floor_serial Receives the floor when there is one.
 * @return 1 when a floor exists, else 0. */
static int tc_release_floor(uint32_t *floor_serial)
{
    int have = 0;
    for (int i = 0; i < TC_RING + OPENPETE_TERCACHE_HOLDS; i++) {
        const int held = i >= TC_RING;
        const tc_state *R = held ? &s_tc.hold[i - TC_RING] : &s_tc.ring[i];
        if (!R->valid || (!held && !openpete_native_scene_stamp_live(R->stamp))) continue;
        if (!have || (int32_t)(R->build_serial - *floor_serial) < 0) { *floor_serial = R->build_serial; have = 1; }
    }
    return have;
}

/** @brief Adopt a fulfilled level-edge replacement.
 *
 * Builder thread, from frame_begin and frame_skip; no-op unless the
 * replacement is in the READY state. Publishes the fresh pairs, resets each
 * arena's allocator, collapses each arena back to one buffer, drops the
 * deferred-release list (its ranges address the outgoing pairs), steps the
 * allocation epoch, records the drain floors, and hands the outgoing pairs
 * to the retire drain. */
static void tc_adopt_pending(void)
{
    if (atomic_load_explicit(&s_tc.rz_state, memory_order_acquire) != TC_RZ_READY) return;
    tc_collect_overflow_pending();
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        tc_arena *ar = &s_tc.arena[a];
        for (int b = 0; b < TC_BUF_MAX; b++)
            s_tc.rz_old[a][b] = atomic_load_explicit(&ar->buf[b].pair,
                                                     memory_order_relaxed);
        tc_buf *p = &ar->buf[0];
        p->cap = s_tc.rz_new[a]->cap;
        p->high = 0; p->free_n = 0; p->used = 0; p->chunks = 0;
        atomic_store_explicit(&p->pair, s_tc.rz_new[a], memory_order_release);
        /* The overflow slot is emptied before the count falls, and the draw
         * skips a NULL pair, so a present that read the old count draws
         * nothing extra rather than binding a pair now on the retire list. */
        atomic_store_explicit(&ar->buf[1].pair, NULL, memory_order_release);
        atomic_store_explicit(&ar->n, 1, memory_order_release);
        ar->ovf_state = TC_OVF_IDLE;
        s_tc.ovf_fail_logged[a] = 0;
        s_tc.rz_new[a] = NULL;
    }
    s_tc.alloc_epoch++;
    /* Every deferred range addresses the outgoing pair's bytes. The fresh
     * pair has none of them allocated, so the list is dropped whole rather
     * than drained: re-inserting those spans into an empty free list would
     * hand the allocator ranges it already owns. */
    s_tc.defer_n = 0;
    /* A held state's ranges address the outgoing pairs too. */
    for (int i = 0; i < OPENPETE_TERCACHE_HOLDS; i++)
        if (s_tc.hold[i].valid) { s_tc.hold[i].valid = 0; s_tc.hold_drops++; }
    /* The registry described the outgoing buffers. */
    tc_reg_clear();
    s_tc.reg_broken = 0;
    s_tc.rz_old_build = s_tc.present_ctr;
    s_tc.rz_old_svc   = atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed);
    atomic_store_explicit(&s_tc.rz_state, TC_RZ_RETIRE, memory_order_release);
}

/** @brief Destroy the pairs a replacement retired once both drain floors have passed.
 *
 * Device-owning thread, from openpete_tercache_present_service. The floors
 * count from the adoption: more than TC_DEFER_PRESENTS builds (frame_begin's
 * counter, the window a freed range already waits out, which covers the
 * one-build presentation lag) and more than RHI_FRAMES_IN_FLIGHT service
 * calls (the frames whose recorded command buffers may still bind the
 * outgoing buffers). On destruction the handshake returns to IDLE.
 * @param svc Service call count including the current call. */
static void tc_retire_drain(uint32_t svc)
{
    const uint32_t builds = atomic_load_explicit(&s_tc.build_ctr, memory_order_acquire);
    if (builds - s_tc.rz_old_build <= TC_DEFER_PRESENTS) return;
    if (svc - s_tc.rz_old_svc <= RHI_FRAMES_IN_FLIGHT) return;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        for (int b = 0; b <= TC_BUF_MAX; b++) {
            tc_pair_free(s_tc.dev, s_tc.rz_old[a][b]);
            s_tc.rz_old[a][b] = NULL;
        }
    }
    atomic_store_explicit(&s_tc.rz_state, TC_RZ_IDLE, memory_order_release);
}

/** @brief Allocate one fresh pair per arena for the stored level and mark the replacement READY.
 *
 * Device-owning thread. Each pair is sized by tc_initial_cap for the level
 * stored by openpete_tercache_set_level. On any failure what was made is
 * freed, the live pairs stay in place and the state stays IDLE, so the cache
 * keeps drawing at the old (accumulating) high-water marks instead of losing
 * the retained path; the consumed request is not retried until the next
 * level edge. The failure is logged once. */
static void tc_resize_fulfil(void)
{
    tc_pair *made[OPTC_ARENA_COUNT];
    const int level = atomic_load_explicit(&s_tc.level_id, memory_order_acquire);
    int ok = 1;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        made[a] = ok ? tc_pair_make(s_tc.dev, a, tc_initial_cap(level, a)) : NULL;
        if (!made[a]) ok = 0;
    }
    if (!ok) {
        for (int a = 0; a < OPTC_ARENA_COUNT; a++) tc_pair_free(s_tc.dev, made[a]);
        if (!s_tc.rz_alloc_logged) {
            s_tc.rz_alloc_logged = 1;
            fprintf(stderr, "[ter-gpu] arena replacement at level %d: allocation failed, "
                            "the live buffers stay\n", level);
        }
        return;
    }
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) s_tc.rz_new[a] = made[a];
    atomic_fetch_add_explicit(&s_tc.svc_fulfilled, 1u, memory_order_relaxed);
    atomic_store_explicit(&s_tc.rz_state, TC_RZ_READY, memory_order_release);
}

/** @brief Allocate one overflow buffer per requested arena and publish the answers.
 *
 * Device-owning thread. The answer is published NULL included: a refusal is
 * what stops the builder from asking again. Only reached with no replacement
 * requested or in flight, so an answer the builder has not taken yet cannot
 * outlive the arena it belongs to. A failed allocation is logged once per
 * arena. */
static void tc_overflow_fulfil(void)
{
    const uint32_t mask = atomic_exchange_explicit(&s_tc.ovf_req, 0u,
                                                   memory_order_acquire);
    if (!mask) return;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        if (!(mask & (1u << a))) continue;
        tc_pair *pr = tc_pair_make(s_tc.dev, a, s_tc.ovf_want[a]);
        if (!pr && !s_tc.ovf_fail_logged[a]) {
            s_tc.ovf_fail_logged[a] = 1;
            fprintf(stderr, "[ter-gpu] arena %d overflow buffer of %u vertices: "
                            "allocation failed, its sectors stay on the CPU walk\n",
                    a, s_tc.ovf_want[a]);
        }
        s_tc.ovf_new[a] = pr;
        atomic_fetch_or_explicit(&s_tc.ovf_done, 1u << a, memory_order_release);
    }
}

void openpete_tercache_present_service(void)
{
    if (!s_tc.ready || !s_tc.dev) return;
    const uint32_t svc = atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed) + 1u;
    atomic_store_explicit(&s_tc.svc_calls, svc, memory_order_relaxed);
    const int st = atomic_load_explicit(&s_tc.rz_state, memory_order_acquire);
    if (st == TC_RZ_RETIRE) { tc_retire_drain(svc); return; }
    if (st != TC_RZ_IDLE) return;   /* READY: the builder has not adopted yet */
    /* A replacement resizes every arena and collapses the overflow buffers
     * into it, so it takes precedence over an outstanding overflow request. */
    if (atomic_exchange_explicit(&s_tc.rz_req, 0, memory_order_acquire)) {
        tc_resize_fulfil();
        return;
    }
    tc_overflow_fulfil();
}

/* ---- per-present protocol ---------------------------------------------- */

/** @brief Return whether OPENPETE_TC_DBG enables the terrain-cache debug log.
 *
 * Read once; any non-empty value other than one starting with `0` enables
 * it. */
static int tc_dbg(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OPENPETE_TC_DBG"); on = (e && *e && *e != '0'); }
    return on;
}
/** @brief Pick the eager ring state a scene-id build writes.
 *
 * A state already carrying @p stamp is reused: the id was built before and
 * its scene was refused by the full scene ring, so the id was not consumed
 * and the rebuild supersedes that build (one state per id, so selection
 * cannot find the refused build). Otherwise the first state from ring_next
 * on whose scene has retired is taken. When every state belongs to an
 * unretired scene, ring_next's state is overwritten and counted in
 * ring_overwrites; TC_RING's sizing keeps that branch unreachable.
 * @param stamp Scene-id stamp of the build (OPTC_STAMP_SCENE_BIT set).
 * @return Ring index of the state. */
static int tc_ring_pick(uint32_t stamp)
{
    s_tc.ring_builds++;
    for (int i = 0; i < TC_RING; i++)
        if (s_tc.ring[i].valid && s_tc.ring[i].stamp == stamp) { s_tc.ring_reuses++; return i; }
    int pick = -1;
    for (int k = 0; k < TC_RING && pick < 0; k++) {
        const int i = (s_tc.ring_next + k) % TC_RING;
        if (!s_tc.ring[i].valid || !openpete_native_scene_stamp_live(s_tc.ring[i].stamp)) pick = i;
    }
    if (pick < 0) { pick = s_tc.ring_next; s_tc.ring_overwrites++; }
    s_tc.ring_next = (pick + 1) % TC_RING;
    return pick;
}

/** @brief Pick a build's target state and run the promote rule.
 *
 * Shared prologue of frame_begin and frame_skip. An immediate build with a
 * scene stamp (OPTC_STAMP_SCENE_BIT) takes the ring entry tc_ring_pick
 * chooses. Any other immediate (re-extract) build writes
 * live directly and leaves staged in place. A canonical build stages, and
 * the previous staged state promotes to live (the extraction the next
 * presents show); a repeated stamp (the serial-retry path re-runs one build)
 * restages in place without a promote.
 * @param stamp Stamp of the build.
 * @param immediate Nonzero for an immediate build.
 * @return The state the build writes, stamped and marked valid. */
static tc_state *tc_begin_state(uint32_t stamp, int immediate)
{
    if (immediate && (stamp & OPTC_STAMP_SCENE_BIT)) {
        /* A scene-id build: its own ring entry, so every pending scene of a
         * burst composes against the terrain state extracted WITH it. */
        const int pick = tc_ring_pick(stamp);
        tc_state *T = &s_tc.ring[pick];
        if (tc_dbg()) fprintf(stderr, "[tc] state pick=%d for stamp=%08x (was %08x valid=%d) ctr=%u\n",
                              pick, stamp, T->stamp, T->valid, s_tc.present_ctr);
        T->stamp = stamp; T->valid = 1; T->build_serial = s_tc.present_ctr;
        atomic_store_explicit(&s_tc.ring_key[pick],
                              (uint64_t)T->build_serial << 32 | stamp, memory_order_release);
        return T;
    }
    if (immediate) {
        /* The staged canonical state SURVIVES: an intermediate re-extract
         * borrows live for its own present, and the next hook build's
         * promote restores the canonical timeline. */
        s_tc.live.stamp = stamp; s_tc.live.valid = 1; s_tc.live.build_serial = s_tc.present_ctr;
        return &s_tc.live;
    }
    if (s_tc.staged.valid && s_tc.staged.stamp != stamp)
        tc_state_copy(&s_tc.live, &s_tc.staged);   /* promote the previous build */
    s_tc.staged.stamp = stamp; s_tc.staged.valid = 1; s_tc.staged.build_serial = s_tc.present_ctr;
    return &s_tc.staged;
}

/** @brief Overwrite a released range's elements with the dead slot before the range becomes reusable.
 *
 * Every draw covers a whole buffer or spans joined across small gaps, and
 * selects per element by 3-bit generation, so a released range that lingers
 * unreused (the allocator can go elsewhere for many builds) still
 * rasterises whenever a later state's generation aliases its own mod 8; a
 * sector that rebuilds every build (animated vertex lighting) then z-fights
 * its current chunk with an older copy. frame_begin calls this after the
 * defer window guarantees no in-flight frame reads these bytes, the one
 * race-free moment (stamping at free time races the frames still reading
 * them). Each element is zeroed
 * with OPTC_SLOT_DEAD in face_flags; that slot is never published with
 * OPTC_SF_DRAWN, so the element is never drawn again. A vertex and a cell
 * record both carry face_flags, at their own offsets. Only the main buffer
 * is written.
 * @param d The deferred release. */
static void tc_dead_stamp(const tc_defer *d)
{
    enum { DEAD_N = 512 };
    static uint8_t s_dead[DEAD_N * sizeof(rhi_terrain_cell_anim)];
    static size_t s_dead_sz;          /* element size s_dead is laid out for */
    const size_t sz = tc_elem_size(d->arena);
    if (s_dead_sz != sz) {
        const size_t ffo = OPTC_ARENA_IS_CELL(d->arena)
                               ? offsetof(rhi_terrain_cell, face_flags)
                               : offsetof(rhi_terrain_vertex, face_flags);
        const uint32_t dead = OPTC_SLOT_DEAD;
        memset(s_dead, 0, sizeof(s_dead));
        for (int k = 0; k < DEAD_N; k++) memcpy(s_dead + (size_t)k * sz + ffo, &dead, sizeof dead);
        s_dead_sz = sz;
    }
    uint32_t left = d->count, at = d->first;
    /* Non-NULL while the cache is ready: init makes every pair or fails, and
     * the level-edge adoption swaps one in. */
    const tc_pair *dp = tc_live_pair(d->arena, d->buf);
    while (left) {
        uint32_t n = left > DEAD_N ? DEAD_N : left;
        rhi_update_buffer_range(dp->vb, s_dead, (size_t)at * sz, (size_t)n * sz);
        at += n; left -= n;
    }
}

void openpete_tercache_frame_begin(const openpete_tercache_globals *g,
                                   uint32_t stamp, int immediate)
{
    if (!s_tc.ready || !g) return;
    s_tc.present_ctr++;
    atomic_store_explicit(&s_tc.build_ctr, s_tc.present_ctr, memory_order_release);
    tc_adopt_pending();
    tc_adopt_overflow();
    s_tc.draws_prev = atomic_exchange_explicit(&s_tc.draws_last, 0u, memory_order_relaxed);
    /* release deferred ranges older than the in-flight window */
    int w = 0;
    uint32_t floor_serial = 0;
    const int have_floor = tc_release_floor(&floor_serial);
    for (int i = 0; i < s_tc.defer_n; i++) {
        const int aged   = s_tc.present_ctr - s_tc.defer[i].stamp > TC_DEFER_PRESENTS;
        const int unread = !have_floor || (int32_t)(s_tc.defer[i].stamp - floor_serial) <= 0;
        if (aged && unread) {
            tc_dead_stamp(&s_tc.defer[i]);
            if (tc_dbg()) fprintf(stderr, "[tc] release arena=%d first=%u count=%u freed_at=%u ctr=%u floor=%u\n",
                                  s_tc.defer[i].arena, s_tc.defer[i].first, s_tc.defer[i].count, s_tc.defer[i].stamp, s_tc.present_ctr, floor_serial);
            tc_free_insert(&s_tc.arena[s_tc.defer[i].arena].buf[s_tc.defer[i].buf],
                           s_tc.defer[i].first, s_tc.defer[i].count);
        } else
            s_tc.defer[w++] = s_tc.defer[i];
    }
    if (tc_dbg()) {
        int live = 0;
        for (int i = 0; i < TC_RING; i++) live += s_tc.ring[i].valid && openpete_native_scene_stamp_live(s_tc.ring[i].stamp);
        fprintf(stderr, "[tc] begin stamp=%08x ctr=%u floor=%u have=%d live_states=%d released=%d held=%d\n",
                stamp, s_tc.present_ctr, floor_serial, have_floor, live, s_tc.defer_n - w, w);
    }
    s_tc.defer_n = w;

    if (!s_tc.ubo) { s_tc.ready = 0; return; }   /* created at init: no device
                                                   * object is made from the
                                                   * builder's thread */
    /* STAGE into host structs only: the feeder runs outside the RHI frame,
     * and an outside-frame rhi_ubo_write fills every ring slot - racing an
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
    gu->dspare[0]  = g->H_canon;       /* canonical H (limit-regime mirrors) */
    gu->dspare[1]  = g->canim_alpha;   /* env-anim colour lerp alpha (anim stage) */
    gu->fog[0] = g->fog_ref_base;
    gu->fog[1] = (int32_t)g->gflags;
    gu->fog[2] = g->cull_ref_s2;
    gu->fog[3] = (int32_t)g->ot_terms;
    memcpy(gu->sdv, g->scroll_dv, sizeof(gu->sdv));
    memcpy(gu->sdvc, g->scroll_dvc, sizeof(gu->sdvc));
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
    gu->cov_cnt[1]   = g->cov_blend;
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
    T->rl.n = -1;                         /* no range list until the commit */
    tc_state_mark(T);
    s_tc.build_target = T->sec;
    s_tc.build_state = T;
}

void openpete_tercache_frame_skip(uint32_t stamp, int immediate)
{
    /* A pure-CPU fallback build still occupies its slot in the promote
     * chain with an all-claimed (empty) state, so the present that renders
     * this build's scene draws no retained content. */
    if (!s_tc.ready) return;
    tc_adopt_pending();
    tc_adopt_overflow();
    tc_state *T = tc_begin_state(stamp, immediate);
    memset(&T->g, 0, sizeof(T->g));
    for (int i = 0; i < OPTC_SLOTS; i++) T->sec[i].o[3] = 0.0f;
    T->rl.n = -1;                         /* no range list until the commit */
    tc_state_mark(T);
    s_tc.build_target = T->sec;
    s_tc.build_state = T;
}

/** @brief Find the retained state built for a stamp: the eager scene ring, then live, then staged.
 *
 * The ring is searched only for a stamp carrying OPTC_STAMP_SCENE_BIT.
 * @param stamp Stamp of the scene.
 * @param src Receives 1 for a ring state, 2 for live, 3 for staged; left
 *        unchanged when no state matches.
 * @return The matching state, or NULL when the cache is not ready or no
 *         state carries @p stamp. */
static const tc_state *tc_state_for_stamp(uint32_t stamp, int *src)
{
    if (!s_tc.ready) return NULL;
    if (stamp & OPTC_STAMP_SCENE_BIT)
        for (int i = 0; i < TC_RING; i++)
            if ((uint32_t)atomic_load_explicit(&s_tc.ring_key[i], memory_order_acquire) == stamp) {
                *src = 1;
                return &s_tc.ring[i];
            }
    if (s_tc.live.valid && stamp == s_tc.live.stamp) { *src = 2; return &s_tc.live; }
    if (s_tc.staged.valid && stamp == s_tc.staged.stamp) { *src = 3; return &s_tc.staged; }
    for (int i = 0; i < OPENPETE_TERCACHE_HOLDS; i++)
        if (s_tc.hold[i].valid && stamp == s_tc.hold[i].stamp) { *src = 4; return &s_tc.hold[i]; }
    return NULL;
}

/** @brief Count a selection that found an older build of a stamp several ring states carry.
 *
 * Diagnostic. tc_state_for_stamp returns the lowest-index ring match; when
 * more than one valid ring state carries @p stamp and @p R is not the one
 * with the newest build serial, ring_dup_selects is incremented.
 * @param stamp Stamp of the composed scene.
 * @param R The state the selection returned. */
static void tc_dup_probe(uint32_t stamp, const tc_state *R)
{
    int n = 0, newer = 0;
    const uint32_t mine = (uint32_t)(atomic_load_explicit(&s_tc.ring_key[R - s_tc.ring],
                                                          memory_order_acquire) >> 32);
    for (int i = 0; i < TC_RING; i++) {
        const uint64_t k = atomic_load_explicit(&s_tc.ring_key[i], memory_order_acquire);
        if ((uint32_t)k != stamp) continue;
        n++;
        newer |= (int32_t)((uint32_t)(k >> 32) - mine) > 0;
    }
    if (n > 1 && newer) {
        s_tc.ring_dup_selects++;
        if (tc_dbg()) fprintf(stderr, "[tc] select stamp=%08x took an older build of %d\n", stamp, n);
    }
}

/* Selects the retained state whose stamp matches the composed scene. */
void openpete_tercache_frame_select(uint32_t present_stamp)
{
    /* present_stamp = the stamp of the SCENE being composed. The retained
     * terrain must be the same extraction as the CPU tris it composes with:
     * the eager ring first, then live (the lagged screenshot compose and the
     * sub-tick spine's immediate publishes both land here), then staged (a
     * depth-0 compose - the capture freeze-backdrop snapshot, a spine-off
     * windowed present - renders the CURRENT scene, whose terrain build sits
     * in staged until the next build's promote; drawing live there pairs
     * scene N with terrain N-1, and the camera moves between extractions, so
     * the skew bakes a whole-scene rim into the frozen backdrop). No other
     * state may stand in: a scene with no build of its own ran no feed. */
    int src = 0;
    const tc_state *R = tc_state_for_stamp(present_stamp, &src);
    if (R) {
        if (src == 1) tc_dup_probe(present_stamp, R);
        s_tc.draw_open = 1; tc_state_copy(&s_tc.sel, R); s_tc.sel_src = src;
    } else {
        s_tc.draw_open = 0; s_tc.sel_src = 0;
        if (tc_dbg() && s_tc.ready && s_tc.live.valid)
            fprintf(stderr, "[tc] select stamp=%08x no state (live=%08x staged=%08x%s)\n",
                    present_stamp, s_tc.live.stamp, s_tc.staged.valid ? s_tc.staged.stamp : 0u,
                    s_tc.staged.valid ? "" : " invalid");
    }
    if (s_tc.draw_open) s_tc.cen_presents++;
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
     * (hook builds) and the UBO flush at the present's first draw. The
     * sector flags are final, so the state's draw ranges are listed now,
     * and its marks cover every chunk the build allocated. */
    if (s_tc.build_state) {
        tc_state_mark(s_tc.build_state);
        tc_ranges_build(s_tc.build_state);
    }
    s_tc.build_target = NULL;
    s_tc.build_state = NULL;
}

/* ---- draw --------------------------------------------------------------- */

/** @brief Return how many times each arena draw is issued (OPENPETE_TERRAIN_DUP_DRAW).
 *
 * Read once, clamped to 1..8, default 1. The measurement it serves: a
 * partial material claim costs one extra full-arena pass per material, and
 * N=2 prices that pass as an upper bound, since the duplicate also re-runs
 * the fragment stage, which a material pass mostly does not. */
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

/* Which pass the arena draws are currently filtering for: -1 = every face
 * (no terrain material pass, and every depth/key pass), 0 = the stock
 * pass, >0 = one material's pass (its index + 1). Set around each group of
 * arena draws, with the resolution classes the pass keeps in
 * s_tc_mat_mask. */
static int s_tc_mat_pass = -1;
static uint32_t s_tc_mat_mask[8];

/** @brief Select the material pass the following arena draws filter for, with its resolution-class mask.
 *
 * @param pass -1 for every face, 0 for the stock pass, else a material
 *        pass id; a pass >= 0 loads its class mask from the material
 *        view (openpete_mods_material_terrain_pass_mask). */
static void tc_mat_pass_set(int pass)
{
    s_tc_mat_pass = pass;
    if (pass >= 0) (void)openpete_mods_material_terrain_pass_mask(pass, s_tc_mat_mask);
}
/* Material passes that fell through to the stock pipeline, counted per
 * draw pass: a pass whose material is dead, disabled, held, has no terrain
 * profile or no pipeline draws its faces stock. */
static unsigned s_tc_mat_stock;
/* Material passes run in this draw pass: the distinct members the baked
 * resolution classes resolved to this present. */
static unsigned s_tc_mat_passes;

/** @brief Return whether a pair is on the level-edge replacement's retire list.
 *
 * No draw may bind a retired pair; tc_draw_buf counts every draw that does
 * in draws_retired, which stays 0 while the invariant holds.
 * @param arena Arena index.
 * @param pr Pair about to be drawn.
 * @return 1 when a replacement is in the RETIRE state and lists @p pr, else 0. */
static int tc_pair_retired(int arena, const tc_pair *pr)
{
    if (atomic_load_explicit(&s_tc.rz_state, memory_order_acquire) != TC_RZ_RETIRE)
        return 0;
    for (int b = 0; b <= TC_BUF_MAX; b++)
        if (s_tc.rz_old[arena][b] == pr) return 1;
    return 0;
}

/** @brief Issue one arena draw over elements [first, first + n) of the bound buffer, tc_dup_draw times.
 * @param cells Nonzero for a cell-record arena (instanced).
 * @param per Vertices per cell record.
 * @param first First element (vertex, or record instance).
 * @param n Element count. */
static void tc_issue(int cells, uint32_t per, uint32_t first, uint32_t n)
{
    for (int r = 0, rn = tc_dup_draw(); r < rn; r++) {
        if (cells) rhi_draw_instanced(s_tc.dev, per, n, 0, first);
        else       rhi_draw(s_tc.dev, n, first);
    }
    s_tc.cen_verts += (uint64_t)n * (cells ? per : 1u);
    s_tc.cen_draws++;
}

/** @brief Draw one buffer of an arena: the selected state's spans in it, or the whole high-water range.
 *
 * Each buffer numbers its vertices from 0, and gl_VertexIndex includes the
 * draw's first vertex, so the vertex stage sees the same index in a span
 * draw as in a whole-range draw. Every vertex chunk holds whole triangles
 * and starts on a triangle boundary (allocations and frees are multiples
 * of 3), so a span that starts at a chunk assembles the same triangles. A
 * cell arena's buffer is an instance-rate record array: one instance per
 * record, the record's cell vertices per instance, a span selected by the
 * first instance. With a range list the draw covers the state's spans in
 * ascending order, clamped to the buffer's range, and so emits the
 * primitives a whole-range draw would rasterise, in the same order;
 * without one, or with OPENPETE_TC_RANGE_DRAW=0, it covers [0, high).
 * `high` is the selected state's mark for the buffer (tc_state_mark), not
 * the live mark the builder grows meanwhile; a buffer whose live pair is not
 * the one the state marked holds none of the state's chunks and is skipped.
 * Pushes @p pc, binds the pair (with its twin when it has one), and counts
 * the submission for the census.
 * @param arena Arena index.
 * @param bi Buffer index within the arena.
 * @param b Buffer.
 * @param pc Push constants for the draw. */
static void tc_draw_buf(int arena, int bi, const tc_buf *b, const tc_pc *pc)
{
    /* One load of the pair for the whole draw, and the range clamped to its
     * capacity. */
    const tc_pair *pr = atomic_load_explicit(&b->pair, memory_order_acquire);
    if (!pr || pr->serial != s_tc.sel.mark_pair[arena][bi]) return;
    uint32_t nverts = s_tc.sel.mark_high[arena][bi];   /* records, on a cell arena */
    if (nverts > pr->cap) nverts = pr->cap;
    if (nverts == 0) return;
    const int cells = OPTC_ARENA_IS_CELL(arena);
    const uint32_t per = OPTC_ARENA_IS_CELL_ANIM(arena) ? RHI_TERRAIN_CELL_VERTS_ANIM
                                                        : RHI_TERRAIN_CELL_VERTS_NEAR;
    const tc_ranges *rl = &s_tc.sel.rl;
    const int listed = rl->n >= 0 && tc_range_draw_on();
    const int k = arena * TC_BUF_MAX + bi;
    if (listed && rl->off[k] == rl->off[k + 1]) {
        s_tc.cen_verts_whole += (uint64_t)nverts * (cells ? per : 1u);
        return;                           /* nothing selected in this buffer */
    }
    if (tc_pair_retired(arena, pr))
        atomic_fetch_add_explicit(&s_tc.draws_retired, 1u, memory_order_relaxed);
    rhi_push_constants(s_tc.dev, pc, sizeof(*pc));
    if (pr->pv) rhi_bind_vertex_buffer2(s_tc.dev, pr->vb, pr->pv);
    else        rhi_bind_vertex_buffer(s_tc.dev, pr->vb);
    s_tc.cen_verts_whole += (uint64_t)nverts * (cells ? per : 1u);
    if (!listed) {
        s_tc.cen_nolist++;
        tc_issue(cells, per, 0, nverts);
    } else {
        for (int i = rl->off[k]; i < rl->off[k + 1]; i++) {
            const uint32_t lo = rl->r[i].first;
            if (lo >= nverts) break;
            const uint32_t hi = lo + rl->r[i].count < nverts ? lo + rl->r[i].count : nverts;
            tc_issue(cells, per, lo, hi - lo);
        }
    }
    if (cells) { s_tc.cell_draws++; s_tc.cell_instances += nverts; }
    atomic_fetch_add_explicit(&s_tc.draws_last, 1u, memory_order_relaxed);
}

/** @brief Return an arena's high-water element count summed over its buffers, as the selected state marked them.
 *
 * tc_draw_anim's test for whether the arena has anything to draw. Reads the
 * selected state's marks (tc_state_mark), which the present thread owns,
 * not the live marks the builder writes.
 * @param arena Arena index.
 * @return The summed high-water count; 0 for an empty arena. */
static uint32_t tc_arena_high(int arena)
{
    uint32_t h = 0;
    for (int b = 0; b < TC_BUF_MAX; b++)
        h += s_tc.sel.mark_high[arena][b];
    return h;
}

/** @brief Fill a terrain push-constant block from a clip mapping.
 *
 * Zeroes the block, copies the clip mapping's extents, band and freeze
 * factor, the portal cover flag and fragment affine, and stamps the material
 * pass (s_tc_mat_pass) with its class mask (s_tc_mat_mask) and the vertex
 * precision snap latched for this present (s_tc_snap).
 * @param pc Out: the block to fill.
 * @param pass_sel Pass selector.
 * @param c Clip mapping; must not be NULL. */
static void tc_pc_fill(tc_pc *pc, int pass_sel, const openpete_tercache_clip *c)
{
    memset(pc, 0, sizeof(*pc));
    pc->x_base = c->x_base; pc->x_span = c->x_span;
    pc->y_base = c->y_base; pc->y_span = c->y_span;
    pc->disp_w = c->disp_w; pc->clip_h = c->clip_h;
    pc->band = c->band; pc->freeze_wf = c->freeze_wf;
    pc->pass_sel = pass_sel; pc->full_extent = c->full_extent;
    pc->mat_pass = s_tc_mat_pass;
    memcpy(pc->cls_mask, s_tc_mat_mask, sizeof pc->cls_mask);
    pc->snap = s_tc_snap;
    pc->cover = c->cover;
    memcpy(pc->fc, c->fc, sizeof pc->fc);
}

/** @brief Draw every buffer of one arena with the bound pipeline.
 * @param arena Arena index.
 * @param pass_sel Pass selector pushed to the vertex stage.
 * @param c Clip mapping for the draw target. */
static void tc_draw_arena(int arena, int pass_sel, const openpete_tercache_clip *c)
{
    const tc_arena *a = &s_tc.arena[arena];
    tc_pc pc;
    tc_pc_fill(&pc, pass_sel, c);
    for (int b = 0, n = atomic_load_explicit(&a->n, memory_order_acquire); b < n; b++)
        tc_draw_buf(arena, b, &a->buf[b], &pc);
}

/** @brief Bind a variant pipeline and draw one arena with it.
 *
 * Serves the poly-anim, near-variant and cell-record arenas: the pipeline is
 * that variant of the slot's pipeline (same state, a different vertex stage
 * and binding-1 twin). Nothing is restored afterwards: the caller sequences
 * all draws for one slot and every arena draw re-pushes its own constants
 * and buffers. Skipped when @p p is NULL or the arena is empty.
 * @param p Pipeline to bind.
 * @param arena Arena index.
 * @param pass_sel Pass selector pushed to the vertex stage.
 * @param c Clip mapping for the draw target. */
static void tc_draw_anim(rhi_pipeline *p, int arena, int pass_sel,
                         const openpete_tercache_clip *c)
{
    if (!p || tc_arena_high(arena) == 0) return;
    rhi_bind_pipeline(s_tc.dev, p);
    tc_draw_arena(arena, pass_sel, c);
}

/* ---- material passes ----------------------------------------------------
 * A material is a draw-state change, so faces it claims cannot be drawn in
 * the same pass as the faces it does not. The arena stays whole: it is drawn
 * once per pass, and the vertex stage keeps only the faces whose resolution
 * class maps to that pass (pc.cls_mask). The stock pass runs first with
 * mat_pass 0, keeping the faces of class 0 and of every class that resolved
 * to stock.
 *
 * The world, fill, alpha, add and subtract colour slots take part, the blend
 * arenas included: each runs its stock pass, then one pass per resolved
 * member. The depth prepass and key slots draw every face. */

void *openpete_tercache_material_pipe(void **pslot, int variant, int slot,
                                      const uint32_t *frag, size_t frag_words,
                                      const char *owner, int mat_index, int chain_module,
                                      unsigned generation)
{
    if (*pslot == OPENPETE_POSTFX_PIPE_FAILED) return NULL;
    if (*pslot && *pslot != OPENPETE_POSTFX_PIPE_PENDING) return *pslot;
    if (!s_tc.dev || variant < 0 || variant >= OPENPETE_MATERIAL_TER_VARIANTS) return NULL;
    static const char *k_vert[OPENPETE_MATERIAL_TER_VARIANTS] = {
        "terrain.vert.spv", "terrain_anim.vert.spv", "terrain_near.vert.spv",
        "terrain_cells_anim.vert.spv", "terrain_cells_near.vert.spv"
    };
    rhi_pipeline_desc d;
    memset(&d, 0, sizeof(d));
    d.vert_spv = k_vert[variant];
    d.blend = RHI_BLEND_NONE;
    d.depth_test = true;
    d.terrain_pipeline = true;
    d.terrain_anim = (variant == 1 || variant == 3);
    d.terrain_near = (variant == 2 || variant == 4);
    d.terrain_cells = (variant >= 3);
    d.push_const_size = sizeof(tc_pc);
    switch (slot) {
        case 1: d.stencil = RHI_STENCIL_MARK; break;
        case 2: d.stencil = RHI_STENCIL_TEST_ZERO; break;
        /* A material inherits its group's compositing, so a blend slot's
         * pipeline differs from the stock one only in the fragment
         * stage - same equation, same depth-write rule. */
        case 3: d.blend = RHI_BLEND_ALPHA;    d.depth_write_off = true; break;
        case 4: d.blend = RHI_BLEND_ADD;      d.depth_write_off = true; break;
        case 5: d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true; break;
        default: break;
    }
    const openpete_lazy_ref ref = {
        .kind = chain_module >= 0 ? OPENPETE_LAZY_CHAIN_TERRAIN : OPENPETE_LAZY_TERRAIN,
        .index = chain_module >= 0 ? chain_module : mat_index,
        .sub = variant * OPENPETE_MATERIAL_TER_SLOTS + slot, .generation = generation,
        .point = -1, .owner = owner,
    };
    const int rc = openpete_native_lazy_pipe(&d, NULL, 0, frag, frag_words, pslot, &ref);
    if (rc < 0)
        fprintf(stderr, "[tercache] material: terrain pipeline build "
                        "FAILED for %s (variant %d slot %d) - its faces "
                        "draw stock\n", owner ? owner : "?", variant, slot);
    return rc == 0 ? *pslot : NULL;
}

/** @brief Bind one pass's terrain material pipeline for a vertex variant and colour slot, or the stock pipeline.
 *
 * The material pipeline is built lazily, once per material (or combined
 * module, for a chain pass), variant and slot, and bound with the material
 * UBO pair at set 3, or for a chain pass the chain entry's terrain block
 * of the chain uniform set (openpete_native_material_chain_bind). @p stock is the
 * pipeline this pass's faces would have been drawn with had they not been
 * claimed; it is bound instead whenever the material cannot serve them (a
 * held or disabled owner, a pipeline that failed to build, a shader with
 * no terrain profile, no material UBO yet), and each
 * such fallback counts in s_tc_mat_stock. The pass always runs: the stock
 * pass has already dropped these faces, so skipping it would leave a hole in
 * the world rather than an unreshaded patch.
 * @param bucket Terrain material pass id: a material index + 1, or
 *        OPENPETE_MATERIAL_TER_PASS_CHAIN + terrain chain entry id.
 * @param variant Vertex variant: 0 plain, 1 anim, 2 near, 3 anim cells,
 *        4 near cells.
 * @param slot Colour slot: 0 world, 1 world with stencil mark, 2 fill,
 *        3 alpha, 4 add, 5 subtract.
 * @param stock Stock pipeline for the fallback, or NULL when no arena of
 *        this flavour draws in this slot.
 * @return 1 when a pipeline is bound, 0 when nothing is to be drawn. */
static int tc_bind_material(int bucket, int variant, int slot,
                            rhi_pipeline *stock)
{
    openpete_mod_material_terrain_t mg;
    int idx = variant * OPENPETE_MATERIAL_TER_SLOTS + slot;
    if (openpete_mods_material_terrain_bucket_gpu(bucket, idx, &mg) != 0)
        goto draw_stock;
    rhi_pipe_site_visit(RHI_PIPE_SITE_TERRAIN_MATERIAL, 1);
    if (!openpete_tercache_material_pipe(mg.pipeline_slot, variant, slot, mg.frag_spv,
                                         mg.frag_words, mg.owner, mg.index, mg.chain_module,
                                         mg.generation))
        goto draw_stock;
    {
        if (!openpete_native_material_ubo()) goto draw_stock;   /* no uniform source yet */
        if (mg.chain >= 0 && !openpete_native_material_chain_ready()) goto draw_stock;
        rhi_bind_pipeline(s_tc.dev, (rhi_pipeline *)*mg.pipeline_slot);
        if (mg.chain >= 0)
            openpete_native_material_chain_bind(s_tc.dev, mg.chain, &mg.frag_cdata);
        else
            openpete_native_material_bind(s_tc.dev, mg.uniform_slot, &mg.frag_cdata);
        return 1;
    }
draw_stock:
    if (!stock) return 0;       /* no arena of this flavour in this slot */
    s_tc_mat_stock++;
    rhi_bind_pipeline(s_tc.dev, stock);
    return 1;
}

/** @brief Draw the opaque world arenas of all five vertex variants for one material pass.
 *
 * Each variant carries the stock pipeline it would have used, so a material
 * that cannot serve its faces this present degrades to stock shading rather
 * than to a hole. The plain, anim and near variants draw their opaque and LP
 * arenas; the cell variants draw their opaque arena.
 * @param bucket Terrain material pass id.
 * @param slot Colour slot (0 world, 1 world with stencil mark).
 * @param use_mark Nonzero to fall back to the stencil-mark pipelines.
 * @param clip Clip mapping for the draw target. */
static void tc_draw_material_world(int bucket, int slot, int use_mark,
                                   const openpete_tercache_clip *clip)
{
    if (tc_bind_material(bucket, 0, slot,
                         use_mark ? s_tc.ps.pipe_world_mark : s_tc.ps.pipe_world)) {
        tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
        tc_draw_arena(OPTC_ARENA_LP,     0, clip);
    }
    if (tc_bind_material(bucket, 1, slot,
                         use_mark ? s_tc.ps.pipe_a_world_mark : s_tc.ps.pipe_a_world)) {
        tc_draw_arena(OPTC_ARENA_A_OPAQUE, 0, clip);
        tc_draw_arena(OPTC_ARENA_A_LP,     0, clip);
    }
    if (tc_bind_material(bucket, 2, slot,
                         use_mark ? s_tc.ps.pipe_n_world_mark : s_tc.ps.pipe_n_world)) {
        tc_draw_arena(OPTC_ARENA_N_OPAQUE, 0, clip);
        tc_draw_arena(OPTC_ARENA_N_LP,     0, clip);
    }
    const int cp = use_mark ? TC_CP_MARK : TC_CP_WORLD;
    if (tc_bind_material(bucket, 3, slot, s_tc.ps.pipe_ca[cp]))
        tc_draw_arena(OPTC_ARENA_CA_OPAQUE, 0, clip);
    if (tc_bind_material(bucket, 4, slot, s_tc.ps.pipe_cn[cp]))
        tc_draw_arena(OPTC_ARENA_CN_OPAQUE, 0, clip);
}

/** @brief Draw one blend class's arenas of all five vertex variants for one material pass.
 *
 * @param bucket Terrain material pass id.
 * @param slot Colour slot (3 alpha, 4 add, 5 subtract).
 * @param cls Blend arena class: OPTC_ARENA_ALPHA, OPTC_ARENA_ADD or
 *        OPTC_ARENA_SUB.
 * @param s_plain Stock pipeline of the plain arena.
 * @param s_anim Stock pipeline of the anim arena.
 * @param s_near Stock pipeline of the near arena.
 * @param clip Clip mapping for the draw target. */
static void tc_draw_material_blend(int bucket, int slot, int cls,
                                   rhi_pipeline *s_plain, rhi_pipeline *s_anim,
                                   rhi_pipeline *s_near,
                                   const openpete_tercache_clip *clip)
{
    const int cp = TC_CP_ALPHA + (cls - OPTC_ARENA_ALPHA);
    if (tc_bind_material(bucket, 0, slot, s_plain))
        tc_draw_arena(cls, 0, clip);
    if (tc_bind_material(bucket, 1, slot, s_anim))
        tc_draw_arena(OPTC_ARENA_ANIM_BASE + cls, 0, clip);
    if (tc_bind_material(bucket, 2, slot, s_near))
        tc_draw_arena(OPTC_ARENA_NEAR_BASE + cls, 0, clip);
    if (tc_bind_material(bucket, 3, slot, s_tc.ps.pipe_ca[cp]))
        tc_draw_arena(OPTC_ARENA_CELL_ANIM_BASE + cls, 0, clip);
    if (tc_bind_material(bucket, 4, slot, s_tc.ps.pipe_cn[cp]))
        tc_draw_arena(OPTC_ARENA_CELL_NEAR_BASE + cls, 0, clip);
}

/** @brief Run the material passes of one colour slot, after its stock passes.
 *
 * One pass per listed pass id, each setting s_tc_mat_pass to the id and
 * s_tc_mat_mask to the resolution classes that resolved to it, so the
 * vertex stage keeps only those faces. The list holds only the members
 * the baked classes resolved to this present, so the pass count is at
 * most the live terrain materials with baked faces: a class that resolved
 * to stock draws in the stock pass, and a material no class resolved to
 * draws no pass. The prepass and key slots run no material pass.
 * @param which OPTC_DRAW_* slot.
 * @param buckets Pass ids.
 * @param bn Number of pass ids.
 * @param use_mark Nonzero when the world slot uses the stencil-mark pipelines.
 * @param clip Clip mapping for the draw target. */
static void tc_draw_material_passes(int which, const int *buckets, int bn,
                                    int use_mark,
                                    const openpete_tercache_clip *clip)
{
    for (int k = 0; k < bn; k++) {
        const int b = buckets[k];
        tc_mat_pass_set(b);
        s_tc_mat_passes++;
        switch (which) {
        case OPTC_DRAW_WORLD:
            tc_draw_material_world(b, use_mark ? 1 : 0, use_mark, clip);
            break;
        case OPTC_DRAW_ALPHA:
            tc_draw_material_blend(b, 3, OPTC_ARENA_ALPHA,
                                   s_tc.ps.pipe_alpha, s_tc.ps.pipe_a_alpha,
                                   s_tc.ps.pipe_n_alpha, clip);
            break;
        case OPTC_DRAW_ADD:
            tc_draw_material_blend(b, 4, OPTC_ARENA_ADD,
                                   s_tc.ps.pipe_add, s_tc.ps.pipe_a_add,
                                   s_tc.ps.pipe_n_add, clip);
            break;
        case OPTC_DRAW_SUB:
            tc_draw_material_blend(b, 5, OPTC_ARENA_SUB,
                                   s_tc.ps.pipe_sub, s_tc.ps.pipe_a_sub,
                                   s_tc.ps.pipe_n_sub, clip);
            break;
        case OPTC_DRAW_FILL:
            if (tc_bind_material(b, 0, 2, s_tc.ps.pipe_fill))
                tc_draw_arena(OPTC_ARENA_LP,   1, clip);
            if (tc_bind_material(b, 1, 2, s_tc.ps.pipe_a_fill))
                tc_draw_arena(OPTC_ARENA_A_LP, 1, clip);
            if (tc_bind_material(b, 2, 2, s_tc.ps.pipe_n_fill))
                tc_draw_arena(OPTC_ARENA_N_LP, 1, clip);
            break;
        default: break;
        }
    }
}

/** @brief Write the selected state into this rhi frame's UBO ring slot on the frame's first terrain draw.
 *
 * One selection can render across several rhi frames (ubo_flush_serial), so
 * the flush runs once per frame serial; it also latches the vertex-snap
 * mode every draw of the frame pushes. */
static void tc_ubo_flush_frame(void)
{
    const int snap_now = openpete_native_vertex_snap_active();   /* 0 or 1 */
    const uint64_t fs = rhi_frame_serial(s_tc.dev);
    if (s_tc.ubo_flush_serial == fs) return;
    rhi_ubo_write(s_tc.ubo, 0, &s_tc.sel.g, sizeof(s_tc.sel.g));
    rhi_ubo_write(s_tc.ubo, 1, s_tc.sel.sec, sizeof(s_tc.sel.sec));
    s_tc.ubo_flush_serial = fs;
    s_tc_snap = snap_now;
}

/** @brief Return the GPU zone name of a terrain draw pass.
 * @param which Pass, an OPTC_DRAW_* value.
 * @return The zone name; "ter.gpu" for a value outside the passes. */
static const char *tc_zone_name(int which)
{
    static const char *k_zone[] = { "ter.world", "ter.alpha", "ter.add",
                                    "ter.sub", "ter.fill", "ter.prepass",
                                    "ter.prepass_depth", "ter.key" };
    return (which >= 0 && which <= OPTC_DRAW_KEY) ? k_zone[which] : "ter.gpu";
}

/** @brief Bind the scene's key-contest images at the sets terrain.frag declares.
 *
 * Same bind-at-pipe discipline as bind_scene_pipe: an absent image leaves
 * its set unbound, and fragments with vKoff == 0 never sample it.
 * @param keys The images; each NULL member is skipped. */
static void tc_bind_keys(const openpete_tercache_keys *keys)
{
    if (keys->hp)    rhi_bind_depth_sampler(s_tc.dev, keys->hp, 1);
    if (keys->prop)  rhi_bind_depth_sampler(s_tc.dev, keys->prop, 2);
    if (keys->actor) rhi_bind_depth_tex_pair(s_tc.dev, keys->actor, keys->actor, 7);
}

/** @brief Bind every input a terrain pass reads besides its pipeline.
 *
 * The VRAM page, the key-contest images, the HD-fold atlas at set 4 (the
 * init-time 1x1 dummy when no fold entry exists: terrain.frag references the
 * set statically and vHd == 0 fragments never sample it), and the two
 * terrain blocks at sets 5/6 - not the contract set 3, whose binding 1 is a
 * mod's own uniform block, and a descriptor set views one buffer per binding.
 * @param vram VRAM texture, or NULL to leave the binding.
 * @param keys Key-contest images, or NULL for a pass that contests nothing. */
static void tc_bind_draw_inputs(rhi_texture *vram, const openpete_tercache_keys *keys)
{
    if (vram) rhi_bind_vram(s_tc.dev, vram);
    if (keys) tc_bind_keys(keys);
    rhi_texture *at = openpete_native_hdfold_atlas();
    if (!at) at = s_tc.hd_dummy;
    if (at) rhi_bind_texture_set(s_tc.dev, at, 4);
    rhi_ubo_bind_at(s_tc.dev, s_tc.ubo, 5, 0);
    rhi_ubo_bind_at(s_tc.dev, s_tc.ubo, 6, 1);
}

void openpete_tercache_draw(int which, const openpete_tercache_clip *clip,
                            rhi_texture *vram, int use_mark,
                            const openpete_tercache_keys *keys)
{
    if (!s_tc.ready || !s_tc.draw_open || !s_tc.ubo) return;
    rhi_pipeline *p = NULL;
    switch (which) {
        case OPTC_DRAW_WORLD:   p = use_mark ? s_tc.ps.pipe_world_mark : s_tc.ps.pipe_world; break;
        case OPTC_DRAW_ALPHA:   p = s_tc.ps.pipe_alpha;   break;
        case OPTC_DRAW_ADD:     p = s_tc.ps.pipe_add;     break;
        case OPTC_DRAW_SUB:     p = s_tc.ps.pipe_sub;     break;
        case OPTC_DRAW_FILL:    p = s_tc.ps.pipe_fill;    break;
        case OPTC_DRAW_PREPASS: p = s_tc.ps.pipe_prepass; break;
        case OPTC_DRAW_PREPASS_DEPTH: p = s_tc.ps.pipe_prepass_depth; break;
        case OPTC_DRAW_KEY:     p = s_tc.ps.pipe_key;     break;
    }
    if (!p) return;
    tc_ubo_flush_frame();
    rhi_gpu_zone_begin(s_tc.dev, tc_zone_name(which));
    rhi_bind_pipeline(s_tc.dev, p);
    tc_bind_draw_inputs(vram, keys);
    /* Every colour slot runs its stock pass filtered (mat_pass 0, the
     * classes resolved to stock) and then one pass per listed member. The
     * depth and key passes shade nothing, so they draw every face
     * regardless of class. */
    int buckets[OPENPETE_MATERIAL_TER_BUCKET_MAX];
    const int bn = (which == OPTC_DRAW_PREPASS || which == OPTC_DRAW_PREPASS_DEPTH ||
                    which == OPTC_DRAW_KEY)
                       ? 0
                       : openpete_mods_material_terrain_bucket_list(
                             buckets, OPENPETE_MATERIAL_TER_BUCKET_MAX);
    tc_mat_pass_set(bn > 0 ? 0 : -1);
    s_tc_mat_stock = 0;
    s_tc_mat_passes = 0;
    const int cworld = use_mark ? TC_CP_MARK : TC_CP_WORLD;
    switch (which) {
        case OPTC_DRAW_WORLD:
            tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     0, clip);
            tc_draw_anim(use_mark ? s_tc.ps.pipe_a_world_mark : s_tc.ps.pipe_a_world,
                         OPTC_ARENA_A_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[cworld], OPTC_ARENA_CA_OPAQUE, 0, clip);
            tc_draw_anim(use_mark ? s_tc.ps.pipe_a_world_mark : s_tc.ps.pipe_a_world,
                         OPTC_ARENA_A_LP,     0, clip);
            tc_draw_anim(use_mark ? s_tc.ps.pipe_n_world_mark : s_tc.ps.pipe_n_world,
                         OPTC_ARENA_N_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[cworld], OPTC_ARENA_CN_OPAQUE, 0, clip);
            tc_draw_anim(use_mark ? s_tc.ps.pipe_n_world_mark : s_tc.ps.pipe_n_world,
                         OPTC_ARENA_N_LP,     0, clip);
            break;
        case OPTC_DRAW_ALPHA:
            tc_draw_arena(OPTC_ARENA_ALPHA, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_alpha, OPTC_ARENA_A_ALPHA, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[TC_CP_ALPHA], OPTC_ARENA_CA_ALPHA, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_alpha, OPTC_ARENA_N_ALPHA, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[TC_CP_ALPHA], OPTC_ARENA_CN_ALPHA, 0, clip);
            break;
        case OPTC_DRAW_ADD:
            tc_draw_arena(OPTC_ARENA_ADD,   0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_add, OPTC_ARENA_A_ADD, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[TC_CP_ADD], OPTC_ARENA_CA_ADD, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_add, OPTC_ARENA_N_ADD, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[TC_CP_ADD], OPTC_ARENA_CN_ADD, 0, clip);
            break;
        case OPTC_DRAW_SUB:
            tc_draw_arena(OPTC_ARENA_SUB,   0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_sub, OPTC_ARENA_A_SUB, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[TC_CP_SUB], OPTC_ARENA_CA_SUB, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_sub, OPTC_ARENA_N_SUB, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[TC_CP_SUB], OPTC_ARENA_CN_SUB, 0, clip);
            break;
        case OPTC_DRAW_FILL:
            /* LP only: poly-anim sectors never backstop-fill (their
             * SF_FILLLP is clear), env-anim colour and near ones do. */
            tc_draw_arena(OPTC_ARENA_LP,    1, clip);
            tc_draw_anim(s_tc.ps.pipe_a_fill, OPTC_ARENA_A_LP, 1, clip);
            tc_draw_anim(s_tc.ps.pipe_n_fill, OPTC_ARENA_N_LP, 1, clip);
            break;
        case OPTC_DRAW_PREPASS:
            /* The WORLD faces of the OPTC_DRAW_WORLD set. */
            tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_prepass, OPTC_ARENA_A_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[TC_CP_PREPASS], OPTC_ARENA_CA_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_prepass, OPTC_ARENA_A_LP,     0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_prepass, OPTC_ARENA_N_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[TC_CP_PREPASS], OPTC_ARENA_CN_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_prepass, OPTC_ARENA_N_LP,     0, clip);
            break;
        case OPTC_DRAW_PREPASS_DEPTH:
            /* The OPTC_DRAW_PREPASS set again over whole footprints, then
             * the hole-fill faces of the OPTC_DRAW_FILL set. */
            tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_prepass_depth, OPTC_ARENA_A_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[TC_CP_PREPASS_DEPTH], OPTC_ARENA_CA_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_prepass_depth, OPTC_ARENA_A_LP,     0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_prepass_depth, OPTC_ARENA_N_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[TC_CP_PREPASS_DEPTH], OPTC_ARENA_CN_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_prepass_depth, OPTC_ARENA_N_LP,     0, clip);
            /* The base arena takes the base pipeline back: a variant
             * pipeline bound above reads its binding-1 twin, which a base
             * arena does not bind. */
            rhi_bind_pipeline(s_tc.dev, p);
            tc_draw_arena(OPTC_ARENA_LP,     1, clip);
            tc_draw_anim(s_tc.ps.pipe_a_prepass_depth, OPTC_ARENA_A_LP, 1, clip);
            tc_draw_anim(s_tc.ps.pipe_n_prepass_depth, OPTC_ARENA_N_LP, 1, clip);
            break;
        case OPTC_DRAW_KEY:
            /* keyed opaque faces only (blend arenas contest but never emit;
             * the vertex stage kills koff<=0.25 and fill-pass faces). */
            tc_draw_arena(OPTC_ARENA_OPAQUE, 2, clip);
            tc_draw_arena(OPTC_ARENA_LP,     2, clip);
            tc_draw_anim(s_tc.ps.pipe_a_key, OPTC_ARENA_A_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[TC_CP_KEY], OPTC_ARENA_CA_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.ps.pipe_a_key, OPTC_ARENA_A_LP,     2, clip);
            tc_draw_anim(s_tc.ps.pipe_n_key, OPTC_ARENA_N_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[TC_CP_KEY], OPTC_ARENA_CN_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.ps.pipe_n_key, OPTC_ARENA_N_LP,     2, clip);
            break;
    }
    tc_draw_material_passes(which, buckets, bn, use_mark ? 1 : 0, clip);
    if (tc_dbg())
        fprintf(stderr, "[tc] %s mat_passes=%u stock_fallback=%u\n",
                tc_zone_name(which), s_tc_mat_passes, s_tc_mat_stock);
    s_tc_mat_pass = -1;
    rhi_gpu_zone_end(s_tc.dev);
}

void openpete_tercache_get_stats(openpete_tercache_stats_t *out)
{
    if (!out) return;
    if (!s_tc.ready && s_tc_final_valid) { *out = s_tc_final; return; }
    memset(out, 0, sizeof(*out));
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        const tc_arena *ar = &s_tc.arena[a];
        for (int b = 0, n = atomic_load_explicit(&ar->n, memory_order_relaxed); b < n; b++) {
            out->chunks     += ar->buf[b].chunks;
            out->verts_used += ar->buf[b].used;
            out->verts_cap  += ar->buf[b].cap;
            out->high[a]    += ar->buf[b].high;
            out->cap[a]     += ar->buf[b].cap;
            out->cap_bytes  += tc_pair_bytes(a, ar->buf[b].cap);
        }
        out->alloc_fail[a]       = s_tc.alloc_fail[a];
        out->overflow_adopted[a] = s_tc.overflow_adopted[a];
    }
    out->draws_last_present = s_tc.draws_prev;
    out->frees_pending = (uint32_t)s_tc.defer_n;
    out->svc_calls     = atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed);
    out->svc_fulfilled = atomic_load_explicit(&s_tc.svc_fulfilled, memory_order_relaxed);
    out->draws_retired = atomic_load_explicit(&s_tc.draws_retired, memory_order_relaxed);
    out->cell_draws     = s_tc.cell_draws;
    out->cell_instances = s_tc.cell_instances;
}
