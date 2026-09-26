/* core/renderer/native_terrain_cache.c — terrain sector-mesh GPU cache,
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
       TC_CP_PREPASS, TC_CP_KEY, TC_CP_N };

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
    double dspare[2];  /* x: H_canon (limit-regime mirrors); y: env-anim colour alpha */
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
    uint32_t       stamp;             /* openpete_frame of the build, or a scene id */
    int            valid;
    uint32_t       build_serial;      /* present_ctr at the build: ranges freed at a
                                       * later serial may still be drawn by this state */
} tc_state;

/* Eager scene ring: one state per published scene, keyed by the scene id the
 * builder stamped it with. Sized past the scene ring so a state outlives its
 * scene's retirement. */
#define TC_RING 48

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
    /* terrain_anim variants (binding-1 prev twin + vertex-stage lerp). */
    rhi_pipeline *pipe_a_world, *pipe_a_world_mark;
    rhi_pipeline *pipe_a_alpha, *pipe_a_add, *pipe_a_sub;
    rhi_pipeline *pipe_a_prepass, *pipe_a_key;
    rhi_pipeline *pipe_a_fill;        /* anim LP hole-fill: env-anim colour
                                       * sectors fill, poly-anim sectors do
                                       * not (their SF_FILLLP stays clear)   */
    /* terrain_near variants (binding-1 member-context twin + the vertex-stage
     * near-plane clip fan). Full set: near sectors carry every draw class. */
    rhi_pipeline *pipe_n_world, *pipe_n_world_mark;
    rhi_pipeline *pipe_n_alpha, *pipe_n_add, *pipe_n_sub;
    rhi_pipeline *pipe_n_fill, *pipe_n_prepass, *pipe_n_key;
    /* terrain_cells variants (instanced cell records): the near-variant
     * sectors' set (clip-fan stage) and the poly-anim sectors' set (lerp
     * stage), indexed by TC_CP_*. */
    rhi_pipeline *pipe_cn[TC_CP_N], *pipe_ca[TC_CP_N];
    uint32_t      cell_draws;         /* instanced cell draws (census)        */
    uint64_t      cell_instances;     /* record instances they covered        */
    rhi_texture  *hd_dummy;           /* 1x1 stand-in for the set-4 atlas     */
    tc_state      staged;             /* canonical build awaiting its present */
    tc_state      live;               /* what presents render                 */
    tc_state      ring[TC_RING];      /* per-scene states (eager); stamp = scene id */
    int           ring_next;
    tc_state      sel;                /* frame_select's snapshot of live: the
                                       * present's draws may execute VBLs after
                                       * selection (deferred screenshot
                                       * readback), by which time live has been
                                       * re-promoted — flushing live at draw
                                       * time would pair old chunks with a
                                       * fresher camera/fog/colour-generation
                                       * set (rung flips, sub-pixel skew) */
    tc_sector_ubo *build_target;      /* where sector_set writes this build   */
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
    uint32_t      draws_last;   /* accumulating (this present) */
    uint32_t      draws_prev;   /* completed previous present (census) */
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


/* One buffer of an arena, for the builder and the chunk writes. The draw
 * path loads the pointer itself so one present's draw reads one pair. */
static const tc_pair *tc_live_pair(int arena, int buf)
{
    return atomic_load_explicit(&s_tc.arena[arena].buf[buf].pair, memory_order_acquire);
}

/* The measured vertex capacity of one arena for one level. The generated
 * table carries a row per measured level id and a union row over every level;
 * an id with no row — and the init before any level has loaded, which passes
 * -1 — takes the union row, and an empty table falls back to k_arena_cap. */
static uint32_t tc_level_cap(int level_id, int arena)
{
    for (int i = 0; i < k_optc_caps_level_n; i++)
        if (k_optc_caps_level[i].level_id == level_id)
            return k_optc_caps_level[i].cap[arena];
    return k_optc_caps_union[arena] ? k_optc_caps_union[arena] : k_arena_cap[arena];
}

/* The capacity an arena's PRIMARY buffer is allocated at.
 * OPENPETE_TC_INITIAL_CAP replaces it with one vertex count for every vertex
 * arena and a sixteenth of it for every cell-record arena (a record replaces
 * 48 or 96 baked vertices, so the sixteenth still binds after the vertex
 * arenas do), so a measuring run can size the primaries past any content and
 * read an uncensored high-water mark, or squeeze them so the overflow path
 * runs. It does not size the overflow buffer, which is what the arena grows
 * to. */
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

/* Bytes of one element of an arena's main buffer: a terrain vertex, or a cell
 * record on the cell arenas. */
static size_t tc_elem_size(int arena)
{
    if (OPTC_ARENA_IS_CELL_ANIM(arena)) return sizeof(rhi_terrain_cell_anim);
    if (OPTC_ARENA_IS_CELL(arena))      return sizeof(rhi_terrain_cell);
    return sizeof(rhi_terrain_vertex);
}

/* Device bytes one buffer of this arena holds: the element stride plus the
 * twin's, at the buffer's capacity. */
static uint64_t tc_pair_bytes(int arena, uint32_t cap)
{
    size_t stride = tc_elem_size(arena);
    if (OPTC_ARENA_IS_ANIM(arena))      stride += sizeof(rhi_terrain_prev);
    else if (OPTC_ARENA_IS_NEAR(arena)) stride += sizeof(rhi_terrain_nearctx);
    return (uint64_t)cap * stride;
}

static void tc_pair_free(rhi_device *dev, tc_pair *pr)
{
    if (!pr) return;
    if (pr->vb) rhi_destroy_buffer(dev, pr->vb);
    if (pr->pv) rhi_destroy_buffer(dev, pr->pv);
    free(pr);
}

/* Device-owning thread only. The twin is the anim arenas' previous-tick
 * buffer or the near arenas' emitted-tri context buffer, at the same vertex
 * count and its own stride; a base arena and a cell arena have none. */
static tc_pair *tc_pair_make(rhi_device *dev, int arena, uint32_t cap)
{
    tc_pair *pr = (tc_pair *)calloc(1, sizeof(*pr));
    if (!pr) return NULL;
    pr->cap = cap;
    pr->vb = rhi_create_vertex_buffer_device(dev, (size_t)cap * tc_elem_size(arena));
    if (OPTC_ARENA_IS_ANIM(arena))
        pr->pv = rhi_create_vertex_buffer_device(dev, (size_t)cap * sizeof(rhi_terrain_prev));
    else if (OPTC_ARENA_IS_NEAR(arena))
        pr->pv = rhi_create_vertex_buffer_device(dev, (size_t)cap * sizeof(rhi_terrain_nearctx));
    const int twin_owed = OPTC_ARENA_IS_ANIM(arena) || OPTC_ARENA_IS_NEAR(arena);
    if (!pr->vb || (twin_owed && !pr->pv)) { tc_pair_free(dev, pr); return NULL; }
    return pr;
}

/* Every pair one arena owns, released: its live buffers, and the pairs its two
 * handshakes may still be holding — the replacement's fulfilled-but-unadopted
 * pair, its retire list, and an overflow buffer the builder never installed.
 * Teardown is the only caller: while the cache runs, each handshake frees its
 * own pairs at the point that proves no frame reads them (tc_retire_drain for
 * the replacement, tc_pair_free for an allocation that failed half-made). */
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
    int32_t mat_pass;      /* -1 = every face, 0 = stock, >0 = that bucket */
    int32_t snap;          /* 1 = integer vertex precision (screen-point floor) */
} tc_pc;

/* --vertex-precision integer, latched once per RHI frame (below) so every
 * arena draw of one present agrees. The renderer's snap state is live: a
 * mid-present flip would otherwise split a frame between a snapped and an
 * unsnapped half of the same terrain, along the sector-residency boundary. */
static int s_tc_snap;

int openpete_tercache_ready(void) { return s_tc.ready; }
int openpete_tercache_frame_open(void) { return s_tc.draw_open; }
int openpete_tercache_frame_source(void) { return s_tc.sel_src; }
/* Diagnostic: flag census of the selected state (drawn / HP / LP / near-variant)
 * and a 3-bit generation histogram over its drawn sectors. */
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

/* Scans the selected state's sector flags, skipping the dead slot. */
int openpete_tercache_frame_content(void)
{
    if (!s_tc.ready) return -1;
    if (!s_tc.draw_open) return 0;
    for (int i = 0; i < OPTC_SLOTS; i++)
        if (i != (int)OPTC_SLOT_DEAD && s_tc.sel.sec[i].o[3] != 0.0f) return 1;
    return 0;
}

/* One cell-record pipeline set: `vert` is the near or anim cell stage, `anim`
 * selects the anim record layout. Returns 1 when every slot built. */
static int tc_make_cell_pipes(rhi_device *dev, const char *vert, int anim,
                              rhi_pipeline *out[TC_CP_N])
{
    const rhi_pipeline_desc base = {
        .vert_spv = vert, .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .terrain_cells = true,
        .terrain_anim = anim != 0, .terrain_near = anim == 0,
        .push_const_size = sizeof(tc_pc),
    };
    rhi_pipeline_desc d[TC_CP_N];
    for (int i = 0; i < TC_CP_N; i++) d[i] = base;
    d[TC_CP_MARK].stencil = RHI_STENCIL_MARK;
    d[TC_CP_ALPHA].blend = RHI_BLEND_ALPHA;    d[TC_CP_ALPHA].depth_write_off = true;
    d[TC_CP_ADD].blend   = RHI_BLEND_ADD;      d[TC_CP_ADD].depth_write_off = true;
    d[TC_CP_SUB].blend   = RHI_BLEND_SUBTRACT; d[TC_CP_SUB].depth_write_off = true;
    d[TC_CP_PREPASS].frag_spv = "terrain_prepass.frag.spv";
    d[TC_CP_PREPASS].color_write_off = true;
    d[TC_CP_PREPASS].depth_class = RHI_DEPTH_FORWARD;
    d[TC_CP_KEY].frag_spv = "terrain_key_cache.frag.spv";
    d[TC_CP_KEY].depth_class = RHI_DEPTH_FORWARD;
    int ok = 1;
    for (int i = 0; i < TC_CP_N; i++) {
        out[i] = rhi_create_pipeline(dev, &d[i]);
        ok &= out[i] != NULL;
    }
    return ok;
}

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
    d.depth_class = RHI_DEPTH_FORWARD;
    s_tc.pipe_prepass = rhi_create_pipeline(dev, &d);
    d = world_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    s_tc.pipe_key = rhi_create_pipeline(dev, &d);
    /* Anim variants: identical state, the anim vertex stage + the
     * binding-1 prev twin. The FILL variant serves the env-anim colour
     * sectors' LP hole-fill backing; a poly-anim sector never publishes
     * SF_FILLLP, so the vertex stage kills its LP in that pass. */
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
    d.depth_class = RHI_DEPTH_FORWARD;
    s_tc.pipe_a_prepass = rhi_create_pipeline(dev, &d);
    d = anim_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    s_tc.pipe_a_key = rhi_create_pipeline(dev, &d);
    d = anim_desc; d.stencil = RHI_STENCIL_TEST_ZERO;
    s_tc.pipe_a_fill = rhi_create_pipeline(dev, &d);
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
    d.depth_class = RHI_DEPTH_FORWARD;
    s_tc.pipe_n_prepass = rhi_create_pipeline(dev, &d);
    d = near_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    s_tc.pipe_n_key = rhi_create_pipeline(dev, &d);

    /* Cell-record variants: the same states over the instanced cell stages. */
    const int cells_ok = tc_make_cell_pipes(dev, "terrain_cells_near.vert.spv", 0, s_tc.pipe_cn)
                       & tc_make_cell_pipes(dev, "terrain_cells_anim.vert.spv", 1, s_tc.pipe_ca);

    int ok = cells_ok && s_tc.pipe_world && s_tc.pipe_world_mark && s_tc.pipe_alpha &&
             s_tc.pipe_add && s_tc.pipe_sub && s_tc.pipe_fill && s_tc.pipe_prepass &&
             s_tc.pipe_key &&
             s_tc.pipe_a_world && s_tc.pipe_a_world_mark && s_tc.pipe_a_alpha &&
             s_tc.pipe_a_add && s_tc.pipe_a_sub && s_tc.pipe_a_prepass &&
             s_tc.pipe_a_key && s_tc.pipe_a_fill &&
             s_tc.pipe_n_world && s_tc.pipe_n_world_mark && s_tc.pipe_n_alpha &&
             s_tc.pipe_n_add && s_tc.pipe_n_sub && s_tc.pipe_n_fill &&
             s_tc.pipe_n_prepass && s_tc.pipe_n_key;
    /* No level has loaded yet, so every arena starts on the union row. */
    for (int a = 0; ok && a < OPTC_ARENA_COUNT; a++) {
        tc_pair *pr = tc_pair_make(dev, a, tc_initial_cap(-1, a));
        if (!pr) { ok = 0; break; }
        s_tc.arena[a].buf[0].cap = pr->cap;
        atomic_store_explicit(&s_tc.arena[a].buf[0].pair, pr, memory_order_relaxed);
        atomic_store_explicit(&s_tc.arena[a].n, 1, memory_order_relaxed);
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
    /* The UBO (block 0 globals, block 1 sectors) is a device object, so it
     * is made here on the device-owning thread rather than lazily by the
     * first build, which may run on the thread that runs the guest. */
    s_tc.ubo = rhi_create_ubo(dev, (uint32_t)TC_UBO_BLOCK, 2);
    if (!s_tc.ubo) return 0;
    s_tc.ready = 1;
    return 1;
}

void openpete_tercache_shutdown(rhi_device *dev)
{
    if (!dev) return;
    openpete_tercache_get_stats(&s_tc_final);
    s_tc_final_valid = 1;
    if (s_tc.hd_dummy) { rhi_destroy_texture(dev, s_tc.hd_dummy); s_tc.hd_dummy = NULL; }
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) tc_arena_free_all(dev, a);
    rhi_pipeline **const named[] = {
        &s_tc.pipe_world, &s_tc.pipe_world_mark, &s_tc.pipe_alpha, &s_tc.pipe_add,
        &s_tc.pipe_sub, &s_tc.pipe_fill, &s_tc.pipe_prepass, &s_tc.pipe_key,
        &s_tc.pipe_a_world, &s_tc.pipe_a_world_mark, &s_tc.pipe_a_alpha,
        &s_tc.pipe_a_add, &s_tc.pipe_a_sub, &s_tc.pipe_a_prepass, &s_tc.pipe_a_key,
        &s_tc.pipe_a_fill,
        &s_tc.pipe_n_world, &s_tc.pipe_n_world_mark, &s_tc.pipe_n_alpha,
        &s_tc.pipe_n_add, &s_tc.pipe_n_sub, &s_tc.pipe_n_fill, &s_tc.pipe_n_prepass,
        &s_tc.pipe_n_key,
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (*named[i]) rhi_destroy_pipeline(dev, *named[i]);
    for (int i = 0; i < TC_CP_N; i++) {
        if (s_tc.pipe_cn[i]) rhi_destroy_pipeline(dev, s_tc.pipe_cn[i]);
        if (s_tc.pipe_ca[i]) rhi_destroy_pipeline(dev, s_tc.pipe_ca[i]);
    }
    if (s_tc.ubo)             rhi_destroy_ubo(dev, s_tc.ubo);
    memset(&s_tc, 0, sizeof(s_tc));
}

/* ---- allocator ---------------------------------------------------------- */

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

/* First fit in one buffer's free list, else a grow of its high-water mark.
 * -1 = this buffer has no room for the chunk. */
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

/* Builder thread, from a refused chunk. Ask the device-owning thread for an
 * overflow buffer of the level's own generated capacity, or the refused chunk
 * where that is larger, so one grant covers the rest of the level rather than
 * one sector. Dropped while this arena already has a request outstanding, an
 * overflow installed, or a refusal on record. */
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
}

void openpete_tercache_chunk_free(int arena, int buf, uint32_t first, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT || count <= 0) return;
    tc_buf *a = &s_tc.arena[arena].buf[buf];
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

/* Builder thread. 1 while a fulfilled pair is waiting for the adoption that
 * the next frame_begin performs. The adoption zeroes every arena's
 * bookkeeping, so a caller holding chunk records must free them while this
 * reads 1 and before that frame_begin. */
int openpete_tercache_resize_ready(void)
{
    return s_tc.ready &&
        atomic_load_explicit(&s_tc.rz_state, memory_order_acquire) == TC_RZ_READY;
}

/* Builder thread, at the replacement adoption. An overflow buffer the device
 * answered but no frame_begin installed has never been bound by a draw; it
 * still retires through the drain, the only path that destroys a pair. The
 * device stops answering overflow requests once a replacement is posted, so
 * every answer raised before this point is visible here. */
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

/* Builder thread, from frame_begin. Install every answered overflow buffer as
 * the arena's next buffer. A refused request leaves the arena at one buffer
 * and stops further requests until the next level edge, so the sectors it
 * could not hold stay on the CPU walk instead of re-asking every build. */
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

/* Builder thread, from frame_begin. Publishes the fresh pairs, resets the
 * allocator, collapses the arena back to one buffer, and hands the outgoing
 * pairs to the retire drain. */
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
    s_tc.rz_old_build = s_tc.present_ctr;
    s_tc.rz_old_svc   = atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed);
    atomic_store_explicit(&s_tc.rz_state, TC_RZ_RETIRE, memory_order_release);
}

/* Device-owning thread. Destroy the outgoing pairs once both floors counted
 * from the adoption have passed: TC_DEFER_PRESENTS builds (frame_begin's
 * counter — the window a freed range already waits out, which covers the
 * one-build presentation lag) and RHI_FRAMES_IN_FLIGHT presents (this
 * function's counter — the frames whose recorded command buffers may still
 * bind the outgoing buffers). */
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

/* Device-owning thread. Allocate one fresh pair per arena; on any failure
 * free what was made and leave the live pairs in place, so the cache keeps
 * drawing at the old (accumulating) high-water marks instead of losing the
 * retained path. */
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

/* Device-owning thread. Allocate one overflow buffer per requested arena and
 * publish the answer, NULL included: a refusal is what stops the builder from
 * asking again. Only reached with no replacement in flight, so an answer the
 * builder has not taken yet cannot outlive the arena it belongs to. */
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

/* Shared prologue for frame_begin/frame_skip: pick the build target and run
 * the promote rule. A hook (canonical) build stages; the PREVIOUS staged
 * state promotes to live (that is the extraction the next presents show).
 * An immediate (re-extract) build writes live directly. A repeated stamp
 * (the serial-retry path re-runs one build) restages in place. */
static int tc_dbg(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OPENPETE_TC_DBG"); on = (e && *e && *e != '0'); }
    return on;
}
static tc_state *tc_begin_state(uint32_t stamp, int immediate)
{
    if (immediate && (stamp & OPTC_STAMP_SCENE_BIT)) {
        /* A scene-id build: its own ring entry, so every pending scene of a
         * burst composes against the terrain state extracted WITH it. Reuse
         * an entry whose scene has retired; a state for a scene still
         * pending is never overwritten (the oldest retired one is taken). */
        int pick = -1;
        for (int k = 0; k < TC_RING; k++) {
            int i = (s_tc.ring_next + k) % TC_RING;
            if (!s_tc.ring[i].valid || !openpete_native_scene_stamp_live(s_tc.ring[i].stamp)) { pick = i; break; }
        }
        if (pick < 0) pick = s_tc.ring_next;   /* ring exhausted: the oldest (should not happen) */
        s_tc.ring_next = (pick + 1) % TC_RING;
        tc_state *T = &s_tc.ring[pick];
        if (tc_dbg()) fprintf(stderr, "[tc] state pick=%d for stamp=%08x (was %08x valid=%d) ctr=%u\n",
                              pick, stamp, T->stamp, T->valid, s_tc.present_ctr);
        T->stamp = stamp; T->valid = 1; T->build_serial = s_tc.present_ctr;
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
        s_tc.live = s_tc.staged;           /* promote the previous build */
    s_tc.staged.stamp = stamp; s_tc.staged.valid = 1; s_tc.staged.build_serial = s_tc.present_ctr;
    return &s_tc.staged;
}

/* Dead-stamp a released range BEFORE it becomes reusable. Every draw walks
 * the whole arena and selects per element by 3-bit generation, so a
 * released range that lingers unreused (release != reuse; the allocator can
 * go elsewhere for many builds) still rasterises whenever a later state's
 * generation aliases its own mod 8 — a sector that rebuilds every build
 * (animated vertex lighting) then z-fights its current chunk with a
 * pulse-phase-old twin. The defer window just guaranteed no in-flight frame
 * reads these bytes, so overwriting here is the one race-free moment
 * (stamping at free time races the frames still reading them). Slot DEAD is
 * never published, so SF_DRAWN kills the element forever. A vertex and a
 * cell record both carry face_flags, at their own offsets. */
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
    s_tc.draws_prev = s_tc.draws_last;
    s_tc.draws_last = 0;
    /* release deferred ranges older than the in-flight window */
    int w = 0;
    /* Eager: a range freed at build serial F was drawn by ring states built
     * before F. Find the oldest build serial among ring states whose scene
     * is still unretired; nothing freed at or after it may be reused yet.
     * With no eager scenes in flight this is "no floor" and the release
     * condition is the build-count window alone, exactly as before. */
    uint32_t floor_serial = 0; int have_floor = 0;
    for (int i = 0; i < TC_RING; i++) {
        const tc_state *R = &s_tc.ring[i];
        if (!R->valid || !openpete_native_scene_stamp_live(R->stamp)) continue;
        if (!have_floor || (int32_t)(R->build_serial - floor_serial) < 0) { floor_serial = R->build_serial; have_floor = 1; }
    }
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
    gu->dspare[0]  = g->H_canon;       /* canonical H (limit-regime mirrors) */
    gu->dspare[1]  = g->canim_alpha;   /* env-anim colour lerp alpha (anim stage) */
    gu->fog[0] = g->fog_ref_base;
    gu->fog[1] = (int32_t)g->gflags;
    gu->fog[2] = g->cull_ref_s2;
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
    tc_adopt_pending();
    tc_adopt_overflow();
    tc_state *T = tc_begin_state(stamp, immediate);
    memset(&T->g, 0, sizeof(T->g));
    for (int i = 0; i < OPTC_SLOTS; i++) T->sec[i].o[3] = 0.0f;
    s_tc.build_target = T->sec;
}

/* Selects the retained state whose stamp matches the composed scene. */
void openpete_tercache_frame_select(uint32_t present_stamp)
{
    /* present_stamp = the stamp of the SCENE being composed. The retained
     * terrain must be the same extraction as the CPU tris it composes with:
     * the eager ring first, then live (the lagged screenshot compose and the
     * sub-tick spine's immediate publishes both land here), then staged (a
     * depth-0 compose — the capture freeze-backdrop snapshot, a spine-off
     * windowed present — renders the CURRENT scene, whose terrain build sits
     * in staged until the next build's promote; drawing live there pairs
     * scene N with terrain N-1, and the camera moves between extractions, so
     * the skew bakes a whole-scene rim into the frozen backdrop). No other
     * state may stand in: a scene with no build of its own ran no feed. */
    const tc_state *R = NULL;
    if (s_tc.ready && (present_stamp & OPTC_STAMP_SCENE_BIT))
        for (int i = 0; i < TC_RING; i++)
            if (s_tc.ring[i].valid && s_tc.ring[i].stamp == present_stamp) { R = &s_tc.ring[i]; break; }
    if (R) {
        s_tc.draw_open = 1; s_tc.sel = *R; s_tc.sel_src = 1;
    } else if (s_tc.ready && s_tc.live.valid && present_stamp == s_tc.live.stamp) {
        s_tc.draw_open = 1; s_tc.sel = s_tc.live; s_tc.sel_src = 2;
    } else if (s_tc.ready && s_tc.staged.valid && present_stamp == s_tc.staged.stamp) {
        s_tc.draw_open = 1; s_tc.sel = s_tc.staged; s_tc.sel_src = 3;
    } else {
        s_tc.draw_open = 0; s_tc.sel_src = 0;
        if (tc_dbg() && s_tc.ready && s_tc.live.valid)
            fprintf(stderr, "[tc] select stamp=%08x no state (live=%08x staged=%08x%s)\n",
                    present_stamp, s_tc.live.stamp, s_tc.staged.valid ? s_tc.staged.stamp : 0u,
                    s_tc.staged.valid ? "" : " invalid");
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

/* Which bucket the arena draws are currently filtering for: -1 = every face
 * (no terrain material installed, and every depth/key pass), 0 = the stock
 * pass, >0 = one material's pass. Set around each group of arena draws. */
static int s_tc_mat_pass = -1;
/* Bucket passes that fell through to the stock pipeline, counted per draw
 * pass. A live bucket binds its own pipeline; a bucket whose material is
 * dead, disabled or has no terrain profile redraws the whole arena stock,
 * which is what iterating the live-bucket list avoids. */
static unsigned s_tc_mat_stock;
/* Bucket passes run in this draw pass. With the live-bucket list this is
 * the number of live terrain materials; iterating 1..bucket_max instead
 * would make it the highest material index, however few are live. */
static unsigned s_tc_mat_passes;

/* Is this pair one the level-edge replacement has already retired? The
 * invariant is that no draw ever binds one. */
static int tc_pair_retired(int arena, const tc_pair *pr)
{
    if (atomic_load_explicit(&s_tc.rz_state, memory_order_acquire) != TC_RZ_RETIRE)
        return 0;
    for (int b = 0; b <= TC_BUF_MAX; b++)
        if (s_tc.rz_old[arena][b] == pr) return 1;
    return 0;
}

/* One buffer of an arena, whole. Each buffer numbers its vertices from 0 and
 * the vertex stage's only index use is gl_VertexIndex % 3, which restarts at
 * every draw, so the buffers need no index translation between them. A cell
 * arena's buffer is an instance-rate record array: one instance per record,
 * the record's cell vertices per instance. */
static void tc_draw_buf(int arena, const tc_buf *b, const tc_pc *pc)
{
    /* One load of the pair for the whole draw, and the range clamped to its
     * capacity: the builder may swap the pair and reset `high` between this
     * present's draws, and a stale `high` against a smaller fresh buffer
     * would read past its end. */
    const tc_pair *pr = atomic_load_explicit(&b->pair, memory_order_acquire);
    if (!pr) return;
    uint32_t nverts = b->high;            /* records, on a cell arena */
    if (nverts > pr->cap) nverts = pr->cap;
    if (nverts == 0) return;
    if (tc_pair_retired(arena, pr))
        atomic_fetch_add_explicit(&s_tc.draws_retired, 1u, memory_order_relaxed);
    rhi_push_constants(s_tc.dev, pc, sizeof(*pc));
    if (pr->pv) rhi_bind_vertex_buffer2(s_tc.dev, pr->vb, pr->pv);
    else        rhi_bind_vertex_buffer(s_tc.dev, pr->vb);
    const int cells = OPTC_ARENA_IS_CELL(arena);
    const uint32_t per = OPTC_ARENA_IS_CELL_ANIM(arena) ? RHI_TERRAIN_CELL_VERTS_ANIM
                                                        : RHI_TERRAIN_CELL_VERTS_NEAR;
    for (int r = 0, rn = tc_dup_draw(); r < rn; r++) {
        if (cells) rhi_draw_instanced(s_tc.dev, per, nverts, 0, 0);
        else       rhi_draw(s_tc.dev, nverts, 0);
    }
    if (cells) { s_tc.cell_draws++; s_tc.cell_instances += nverts; }
    s_tc.draws_last++;
}

/* The arena's high-water vertex count over its buffers: the census figure and
 * the "is there anything to draw" test. */
static uint32_t tc_arena_high(int arena)
{
    const tc_arena *a = &s_tc.arena[arena];
    uint32_t h = 0;
    for (int b = 0, n = atomic_load_explicit(&a->n, memory_order_acquire); b < n; b++)
        h += a->buf[b].high;
    return h;
}

static void tc_pc_fill(tc_pc *pc, int pass_sel, const openpete_tercache_clip *c)
{
    memset(pc, 0, sizeof(*pc));
    pc->x_base = c->x_base; pc->x_span = c->x_span;
    pc->y_base = c->y_base; pc->y_span = c->y_span;
    pc->disp_w = c->disp_w; pc->clip_h = c->clip_h;
    pc->band = c->band; pc->freeze_wf = c->freeze_wf;
    pc->pass_sel = pass_sel; pc->full_extent = c->full_extent;
    pc->mat_pass = s_tc_mat_pass;
    pc->snap = s_tc_snap;
}

static void tc_draw_arena(int arena, int pass_sel, const openpete_tercache_clip *c)
{
    const tc_arena *a = &s_tc.arena[arena];
    tc_pc pc;
    tc_pc_fill(&pc, pass_sel, c);
    for (int b = 0, n = atomic_load_explicit(&a->n, memory_order_acquire); b < n; b++)
        tc_draw_buf(arena, &a->buf[b], &pc);
}

/* Draw a twin arena (poly-anim or near variant): rebind that variant of the
 * slot's pipeline (same state, a different vertex stage + binding-1 twin),
 * then restore nothing — the caller sequences all draws for one slot and
 * every arena draw re-pushes its own constants and buffers. */
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
 * once per participating bucket, and the vertex stage keeps only that
 * bucket's faces (pc.mat_pass). The stock pass runs first with mat_pass 0,
 * dropping every claimed face.
 *
 * Only the OPAQUE colour slots take part. Blended terrain would have its
 * painter order changed by the stock/material split, which is unresolved —
 * so a sector with a blended material face claims back to the CPU walk
 * instead, and the blend arenas draw unfiltered. */

/* Build (lazily, once per material) the terrain pipeline for one bucket at
 * one arena vertex variant and colour slot, then bind it and the material
 * UBO pair at set 3.
 *
 * `stock` is the pipeline this bucket's faces would have been drawn with
 * had they not been claimed, and it is bound instead whenever the material
 * cannot serve them — a refine callback that declined this present, a
 * pipeline that failed to build, a shader with no terrain profile. The pass
 * ALWAYS runs: the stock pass has already dropped these faces, so skipping
 * it would leave a hole in the world rather than an unreshaded patch. */
static int tc_bind_material(int bucket, int variant, int slot,
                            rhi_pipeline *stock)
{
    openpete_mod_material_terrain_t mg;
    int idx = variant * OPENPETE_MATERIAL_TER_SLOTS + slot;
    if (openpete_mods_material_terrain_bucket_gpu(bucket, idx, &mg) != 0)
        goto draw_stock;
    if (*mg.pipeline_slot == OPENPETE_POSTFX_PIPE_FAILED) goto draw_stock;
    if (!*mg.pipeline_slot) {
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
             * stage — same equation, same depth-write rule. */
            case 3: d.blend = RHI_BLEND_ALPHA;    d.depth_write_off = true; break;
            case 4: d.blend = RHI_BLEND_ADD;      d.depth_write_off = true; break;
            case 5: d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true; break;
            default: break;
        }
        rhi_pipeline *p = rhi_create_pipeline_spv(s_tc.dev, &d, NULL, 0,
                                                  mg.frag_spv, mg.frag_words);
        if (!p) {
            fprintf(stderr, "[tercache] material: terrain pipeline build "
                            "FAILED for %s (variant %d slot %d) — its faces "
                            "draw stock\n", mg.owner ? mg.owner : "?",
                    variant, slot);
            *mg.pipeline_slot = OPENPETE_POSTFX_PIPE_FAILED;
            goto draw_stock;
        }
        *mg.pipeline_slot = p;
    }
    {
        rhi_ubo *mu = openpete_native_material_ubo();
        if (!mu) goto draw_stock;   /* no uniform source yet */
        rhi_bind_pipeline(s_tc.dev, (rhi_pipeline *)*mg.pipeline_slot);
        rhi_ubo_bind(s_tc.dev, mu, 0,
                     mg.uniform_slot > 0 ? (uint32_t)(1 + mg.uniform_slot) : 0u);
        return 1;
    }
draw_stock:
    if (!stock) return 0;       /* no arena of this flavour in this slot */
    s_tc_mat_stock++;
    rhi_bind_pipeline(s_tc.dev, stock);
    return 1;
}

/* The five arena flavours of one colour slot, for one bucket. Each carries
 * the stock pipeline it would have used, so a material that cannot serve
 * its faces this present degrades to stock shading rather than to a hole. */
static void tc_draw_material_world(int bucket, int slot, int use_mark,
                                   const openpete_tercache_clip *clip)
{
    if (tc_bind_material(bucket, 0, slot,
                         use_mark ? s_tc.pipe_world_mark : s_tc.pipe_world)) {
        tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
        tc_draw_arena(OPTC_ARENA_LP,     0, clip);
    }
    if (tc_bind_material(bucket, 1, slot,
                         use_mark ? s_tc.pipe_a_world_mark : s_tc.pipe_a_world)) {
        tc_draw_arena(OPTC_ARENA_A_OPAQUE, 0, clip);
        tc_draw_arena(OPTC_ARENA_A_LP,     0, clip);
    }
    if (tc_bind_material(bucket, 2, slot,
                         use_mark ? s_tc.pipe_n_world_mark : s_tc.pipe_n_world)) {
        tc_draw_arena(OPTC_ARENA_N_OPAQUE, 0, clip);
        tc_draw_arena(OPTC_ARENA_N_LP,     0, clip);
    }
    const int cp = use_mark ? TC_CP_MARK : TC_CP_WORLD;
    if (tc_bind_material(bucket, 3, slot, s_tc.pipe_ca[cp]))
        tc_draw_arena(OPTC_ARENA_CA_OPAQUE, 0, clip);
    if (tc_bind_material(bucket, 4, slot, s_tc.pipe_cn[cp]))
        tc_draw_arena(OPTC_ARENA_CN_OPAQUE, 0, clip);
}

/* One blend class (cls = OPTC_ARENA_ALPHA/ADD/SUB), all five arena flavours,
 * for one bucket. */
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
    if (tc_bind_material(bucket, 3, slot, s_tc.pipe_ca[cp]))
        tc_draw_arena(OPTC_ARENA_CELL_ANIM_BASE + cls, 0, clip);
    if (tc_bind_material(bucket, 4, slot, s_tc.pipe_cn[cp]))
        tc_draw_arena(OPTC_ARENA_CELL_NEAR_BASE + cls, 0, clip);
}

/* The material passes of one colour slot, run after its stock passes.
 *
 * One pass per live terrain bucket. Bucket ids are a material index + 1
 * and so are sparse: a material registered at index 200 leaves 199 ids
 * with no live material behind it. Iterating the live list rather than
 * 1..highest keeps the pass count proportional to the live terrain
 * materials at any table size, because an id with no live material still
 * binds a pipeline and redraws the whole arena, every face of which the
 * bucket filter then discards. */
static void tc_draw_material_passes(int which, const int *buckets, int bn,
                                    int use_mark,
                                    const openpete_tercache_clip *clip)
{
    for (int k = 0; k < bn; k++) {
        const int b = buckets[k];
        s_tc_mat_pass = b;
        s_tc_mat_passes++;
        switch (which) {
        case OPTC_DRAW_WORLD:
            tc_draw_material_world(b, use_mark ? 1 : 0, use_mark, clip);
            break;
        case OPTC_DRAW_ALPHA:
            tc_draw_material_blend(b, 3, OPTC_ARENA_ALPHA,
                                   s_tc.pipe_alpha, s_tc.pipe_a_alpha,
                                   s_tc.pipe_n_alpha, clip);
            break;
        case OPTC_DRAW_ADD:
            tc_draw_material_blend(b, 4, OPTC_ARENA_ADD,
                                   s_tc.pipe_add, s_tc.pipe_a_add,
                                   s_tc.pipe_n_add, clip);
            break;
        case OPTC_DRAW_SUB:
            tc_draw_material_blend(b, 5, OPTC_ARENA_SUB,
                                   s_tc.pipe_sub, s_tc.pipe_a_sub,
                                   s_tc.pipe_n_sub, clip);
            break;
        case OPTC_DRAW_FILL:
            if (tc_bind_material(b, 0, 2, s_tc.pipe_fill))
                tc_draw_arena(OPTC_ARENA_LP,   1, clip);
            if (tc_bind_material(b, 1, 2, s_tc.pipe_a_fill))
                tc_draw_arena(OPTC_ARENA_A_LP, 1, clip);
            if (tc_bind_material(b, 2, 2, s_tc.pipe_n_fill))
                tc_draw_arena(OPTC_ARENA_N_LP, 1, clip);
            break;
        default: break;
        }
    }
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
    int snap_now = openpete_native_vertex_snap_active();   /* 0 or 1 */
    {   /* First draw of each RHI FRAME: flush the selected state into this
         * frame's UBO ring slot (see ubo_flush_serial — one selection can
         * render across several rhi frames), and latch the vertex-snap mode
         * the frame's draws all push. */
        uint64_t fs = rhi_frame_serial(s_tc.dev);
        if (s_tc.ubo_flush_serial != fs) {
            rhi_ubo_write(s_tc.ubo, 0, &s_tc.sel.g, sizeof(s_tc.sel.g));
            rhi_ubo_write(s_tc.ubo, 1, s_tc.sel.sec, sizeof(s_tc.sel.sec));
            s_tc.ubo_flush_serial = fs;
            s_tc_snap = snap_now;
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
    /* Every colour slot runs its stock pass filtered (mat_pass 0) and then
     * one pass per live bucket. Bucket ids are a material index + 1 and so
     * are sparse: the list is iterated rather than the 1..max range,
     * because an id with no live material still binds the stock pipeline
     * and redraws the whole arena. The depth and key passes shade nothing,
     * so they draw every face regardless of bucket. */
    int buckets[OPENPETE_MATERIAL_TER_BUCKET_MAX];
    const int bn = (which == OPTC_DRAW_PREPASS || which == OPTC_DRAW_KEY)
                       ? 0
                       : openpete_mods_material_terrain_bucket_list(
                             buckets, OPENPETE_MATERIAL_TER_BUCKET_MAX);
    s_tc_mat_pass = bn > 0 ? 0 : -1;
    s_tc_mat_stock = 0;
    s_tc_mat_passes = 0;
    const int cworld = use_mark ? TC_CP_MARK : TC_CP_WORLD;
    switch (which) {
        case OPTC_DRAW_WORLD:
            tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     0, clip);
            tc_draw_anim(use_mark ? s_tc.pipe_a_world_mark : s_tc.pipe_a_world,
                         OPTC_ARENA_A_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.pipe_ca[cworld], OPTC_ARENA_CA_OPAQUE, 0, clip);
            tc_draw_anim(use_mark ? s_tc.pipe_a_world_mark : s_tc.pipe_a_world,
                         OPTC_ARENA_A_LP,     0, clip);
            tc_draw_anim(use_mark ? s_tc.pipe_n_world_mark : s_tc.pipe_n_world,
                         OPTC_ARENA_N_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.pipe_cn[cworld], OPTC_ARENA_CN_OPAQUE, 0, clip);
            tc_draw_anim(use_mark ? s_tc.pipe_n_world_mark : s_tc.pipe_n_world,
                         OPTC_ARENA_N_LP,     0, clip);
            break;
        case OPTC_DRAW_ALPHA:
            tc_draw_arena(OPTC_ARENA_ALPHA, 0, clip);
            tc_draw_anim(s_tc.pipe_a_alpha, OPTC_ARENA_A_ALPHA, 0, clip);
            tc_draw_anim(s_tc.pipe_ca[TC_CP_ALPHA], OPTC_ARENA_CA_ALPHA, 0, clip);
            tc_draw_anim(s_tc.pipe_n_alpha, OPTC_ARENA_N_ALPHA, 0, clip);
            tc_draw_anim(s_tc.pipe_cn[TC_CP_ALPHA], OPTC_ARENA_CN_ALPHA, 0, clip);
            break;
        case OPTC_DRAW_ADD:
            tc_draw_arena(OPTC_ARENA_ADD,   0, clip);
            tc_draw_anim(s_tc.pipe_a_add, OPTC_ARENA_A_ADD, 0, clip);
            tc_draw_anim(s_tc.pipe_ca[TC_CP_ADD], OPTC_ARENA_CA_ADD, 0, clip);
            tc_draw_anim(s_tc.pipe_n_add, OPTC_ARENA_N_ADD, 0, clip);
            tc_draw_anim(s_tc.pipe_cn[TC_CP_ADD], OPTC_ARENA_CN_ADD, 0, clip);
            break;
        case OPTC_DRAW_SUB:
            tc_draw_arena(OPTC_ARENA_SUB,   0, clip);
            tc_draw_anim(s_tc.pipe_a_sub, OPTC_ARENA_A_SUB, 0, clip);
            tc_draw_anim(s_tc.pipe_ca[TC_CP_SUB], OPTC_ARENA_CA_SUB, 0, clip);
            tc_draw_anim(s_tc.pipe_n_sub, OPTC_ARENA_N_SUB, 0, clip);
            tc_draw_anim(s_tc.pipe_cn[TC_CP_SUB], OPTC_ARENA_CN_SUB, 0, clip);
            break;
        case OPTC_DRAW_FILL:
            /* LP only: poly-anim sectors never backstop-fill (their
             * SF_FILLLP is clear), env-anim colour and near ones do. */
            tc_draw_arena(OPTC_ARENA_LP,    1, clip);
            tc_draw_anim(s_tc.pipe_a_fill, OPTC_ARENA_A_LP, 1, clip);
            tc_draw_anim(s_tc.pipe_n_fill, OPTC_ARENA_N_LP, 1, clip);
            break;
        case OPTC_DRAW_PREPASS:
            tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     1, clip);
            tc_draw_anim(s_tc.pipe_a_prepass, OPTC_ARENA_A_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.pipe_ca[TC_CP_PREPASS], OPTC_ARENA_CA_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.pipe_a_prepass, OPTC_ARENA_A_LP,     0, clip);
            tc_draw_anim(s_tc.pipe_n_prepass, OPTC_ARENA_N_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.pipe_cn[TC_CP_PREPASS], OPTC_ARENA_CN_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.pipe_n_prepass, OPTC_ARENA_N_LP,     0, clip);
            tc_draw_anim(s_tc.pipe_n_prepass, OPTC_ARENA_N_LP,     1, clip);
            break;
        case OPTC_DRAW_KEY:
            /* keyed opaque faces only (blend arenas contest but never emit;
             * the vertex stage kills koff<=0.25 and fill-pass faces). */
            tc_draw_arena(OPTC_ARENA_OPAQUE, 2, clip);
            tc_draw_arena(OPTC_ARENA_LP,     2, clip);
            tc_draw_anim(s_tc.pipe_a_key, OPTC_ARENA_A_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.pipe_ca[TC_CP_KEY], OPTC_ARENA_CA_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.pipe_a_key, OPTC_ARENA_A_LP,     2, clip);
            tc_draw_anim(s_tc.pipe_n_key, OPTC_ARENA_N_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.pipe_cn[TC_CP_KEY], OPTC_ARENA_CN_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.pipe_n_key, OPTC_ARENA_N_LP,     2, clip);
            break;
    }
    tc_draw_material_passes(which, buckets, bn, use_mark ? 1 : 0, clip);
    if (tc_dbg())
        fprintf(stderr, "[tc] %s mat_passes=%u stock_fallback=%u\n",
                (which >= 0 && which <= OPTC_DRAW_KEY) ? k_zone[which]
                                                       : "ter.gpu",
                s_tc_mat_passes, s_tc_mat_stock);
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
