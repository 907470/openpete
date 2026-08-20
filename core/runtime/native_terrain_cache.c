/* native_terrain_cache.c — terrain sector-mesh GPU cache, core side.
 * See openpete_terrain_cache.h for the contract and
 * notes/plans/openpete-terrain-gpu-cache.md for the design. */

#include "rhi.h"
#include "openpete_terrain_cache.h"

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
    double dH[2];      /* H, unused       */
    int32_t fog[4];    /* fogRefBase, gflags, cullRefS2, 0 */
} tc_globals_ubo;

typedef struct { float o[4]; } tc_sector_ubo;   /* xyz origin, w flag bits */

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
    int           ubo_written;        /* live blocks flushed this rhi frame   */
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

    int ok = s_tc.pipe_world && s_tc.pipe_world_mark && s_tc.pipe_alpha &&
             s_tc.pipe_add && s_tc.pipe_sub && s_tc.pipe_fill && s_tc.pipe_prepass &&
             s_tc.pipe_key;
    for (int a = 0; ok && a < OPTC_ARENA_COUNT; a++) {
        s_tc.arena[a].cap = k_arena_cap[a];
        s_tc.arena[a].vb = rhi_create_vertex_buffer_device(
            dev, (size_t)k_arena_cap[a] * sizeof(rhi_terrain_vertex));
        if (!s_tc.arena[a].vb) ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "[ter-gpu] init failed (pipeline or arena alloc) — "
                        "terrain cache disabled, CPU path stays\n");
        openpete_tercache_shutdown(dev);
        return 0;
    }
    s_tc.ready = 1;
    return 1;
}

void openpete_tercache_shutdown(rhi_device *dev)
{
    if (!dev) return;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        if (s_tc.arena[a].vb) { rhi_destroy_buffer(dev, s_tc.arena[a].vb); s_tc.arena[a].vb = NULL; }
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
        uint32_t block = sizeof(tc_sector_ubo) * OPTC_SLOTS;   /* 8 KB > globals */
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
    {   static int dbg = -1; static int n;
        if (dbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_GPU_DBG2");
                       dbg = (e && *e && *e != '0'); }
        if (dbg && ++n <= 4000)
            fprintf(stderr, "[tc-beg] stamp=%u imm=%d cam=%.4f,%.4f,%.4f M00=%.0f M02=%.0f "
                    "fogref=%d fogscale=%.6f gflags=0x%x\n",
                    stamp, immediate, g->camx, g->camy, g->camz,
                    (double)g->mrow[0][0], (double)g->mrow[0][2],
                    g->fog_ref_base, g->fogscale, g->gflags);
    }
    for (int k = 0; k < 3; k++) {
        gu->mrow0[k] = g->mrow[0][k];
        gu->mrow1[k] = g->mrow[1][k];
        gu->mrow2[k] = g->mrow[2][k];
    }
    gu->dcam_xy[0] = g->camx; gu->dcam_xy[1] = g->camy;
    gu->dcam_zf[0] = g->camz; gu->dcam_zf[1] = g->fogscale;
    gu->dof_xy[0]  = g->ofx;  gu->dof_xy[1]  = g->ofy;
    gu->dH[0]      = g->H;
    gu->fog[0] = g->fog_ref_base;
    gu->fog[1] = (int32_t)g->gflags;
    gu->fog[2] = g->cull_ref_s2;

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
    s_tc.ubo_written = 0;
    {   static int dbg = -1; static int n;
        if (dbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_GPU_DBG2");
                       dbg = (e && *e && *e != '0'); }
        if (dbg && ++n <= 40)
            fprintf(stderr, "[tc-sel] present=%u live=%u valid=%d staged=%u svalid=%d open=%d "
                    "livecam=%.4f,%.4f,%.4f liveM=%.0f,%.0f\n",
                    present_stamp, s_tc.live.stamp, s_tc.live.valid,
                    s_tc.staged.stamp, s_tc.staged.valid, s_tc.draw_open,
                    s_tc.live.g.dcam_xy[0], s_tc.live.g.dcam_xy[1], s_tc.live.g.dcam_zf[0],
                    (double)s_tc.live.g.mrow0[0], (double)s_tc.live.g.mrow0[2]);
    }
}

void openpete_tercache_sector_set(int slot, float ox, float oy, float oz,
                                  uint32_t sflags)
{
    if (!s_tc.build_target || slot < 0 || slot >= OPTC_SLOTS || slot == (int)OPTC_SLOT_DEAD)
        return;
    union { float f; uint32_t u; } w;
    w.u = sflags;
    s_tc.build_target[slot].o[0] = ox;
    s_tc.build_target[slot].o[1] = oy;
    s_tc.build_target[slot].o[2] = oz;
    s_tc.build_target[slot].o[3] = w.f;
}

void openpete_tercache_frame_commit(void)
{
    /* Build staging is complete; promotion happens at the next frame_begin
     * (hook builds) and the UBO flush at the present's first draw. */
    s_tc.build_target = NULL;
}

/* ---- draw --------------------------------------------------------------- */

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
    rhi_bind_vertex_buffer(s_tc.dev, a->vb);
    rhi_draw(s_tc.dev, a->high, 0);
    s_tc.draws_last++;
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
    if (!s_tc.ubo_written) {   /* first draw of the present: flush the state
                                * frame_select snapshotted for it */
        rhi_ubo_write(s_tc.ubo, 0, &s_tc.sel.g, sizeof(s_tc.sel.g));
        rhi_ubo_write(s_tc.ubo, 1, s_tc.sel.sec, sizeof(s_tc.sel.sec));
        s_tc.ubo_written = 1;
    }
    rhi_bind_pipeline(s_tc.dev, p);
    if (vram) rhi_bind_vram(s_tc.dev, vram);
    /* og key contest inputs (scene slots only; same bind-at-pipe discipline
     * as bind_scene_pipe — absent images leave vKoff==0 fragments alone). */
    if (key_hp)   rhi_bind_depth_sampler(s_tc.dev, key_hp, 1);
    if (key_prop) rhi_bind_depth_sampler(s_tc.dev, key_prop, 2);
    rhi_ubo_bind(s_tc.dev, s_tc.ubo, 0, 1);
    switch (which) {
        case OPTC_DRAW_WORLD:
            tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     0, clip);
            break;
        case OPTC_DRAW_ALPHA:
            {   static int nb = -1;
                if (nb < 0) { const char* e = getenv("OPENPETE_TERRAIN_GPU_NOBLEND");
                              nb = (e && *e && *e != '0'); }
                if (nb) break;
            }
            tc_draw_arena(OPTC_ARENA_ALPHA, 0, clip); break;
        case OPTC_DRAW_ADD:
            {   static int nb = -1;
                if (nb < 0) { const char* e = getenv("OPENPETE_TERRAIN_GPU_NOBLEND");
                              nb = (e && *e && *e != '0'); }
                if (nb) break;
            }
            tc_draw_arena(OPTC_ARENA_ADD,   0, clip); break;
        case OPTC_DRAW_SUB:   tc_draw_arena(OPTC_ARENA_SUB,   0, clip); break;
        case OPTC_DRAW_FILL:  tc_draw_arena(OPTC_ARENA_LP,    1, clip); break;
        case OPTC_DRAW_PREPASS:
            tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     1, clip);
            break;
        case OPTC_DRAW_KEY:
            /* keyed opaque faces only (blend arenas contest but never emit;
             * the vertex stage kills koff<=0.25 and fill-pass faces). */
            tc_draw_arena(OPTC_ARENA_OPAQUE, 2, clip);
            tc_draw_arena(OPTC_ARENA_LP,     2, clip);
            break;
    }
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
