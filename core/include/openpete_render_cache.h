/* openpete_render_cache.h — shared world-space cache for the renderer
 * foundation. See notes/plans/openpete-renderer-foundation.md.
 *
 * Producer: psxrecomp's gte_rtps_internal writes one entry per projected
 *   vertex, immediately after push_sxy(). Key is the packed int16-saturated
 *   SXY that the game will subsequently mfc2 into prim packets.
 *
 * Consumer: PsyCross's OT walker (ParsePrimitive → MakeVertexTriangle /
 *   MakeVertexQuad) looks up per-vertex by SXY hash to recover world-space
 *   position, model-local vertex, and the RT/TR/OFX/OFY/H projection state.
 *
 * Determinism: renderer-side state only. Never reads back into PSX RAM.
 * Cannot perturb gameplay-readable state by construction.
 *
 * Phase 0 goal: pgxp_hit ≈ 1,945/1,945 prims per frame (and ≈ 5,400/5,400
 * verts per frame) on level_10 active gameplay, no behavior change.
 */

#ifndef OPENPETE_RENDER_CACHE_H
#define OPENPETE_RENDER_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Schema per renderer-foundation.md "Cache schema". sxy_key is packed as
 * ((u16)(int16)sy << 16) | (u16)(int16)sx — matches both the GTE SXY[]
 * register layout AND the (x,y) shorts the game writes into prim packets. */
typedef struct openpete_render_cache_entry {
    uint32_t sxy_key;
    uint32_t seq;            /* matches frame->seq when entry is current */
    uint32_t matrix_span_id; /* bumped on RT/TR change */
    int32_t  mac[3];         /* pre-projection world-space (q12) */
    int16_t  v_local[3];     /* model-local vertex pre-transform */
    int16_t  _pad0;
    int16_t  rt[3][3];       /* rotation matrix snapshot */
    int32_t  tr[3];          /* translation vector snapshot */
    int32_t  ofx;            /* canonical projection state (pre-Hor+) */
    int32_t  ofy;
    uint16_t h;
    /* transform-interp §7.5-R3 per-vertex world key for WORLD-tagged
     * geometry (obj_tag has the world bit), from the world-geo side-
     * channel. Two families, distinguished by bit31:
     *   - PHASE-D (bit31 set): the vertex's SOURCE ADDRESS in strip
     *     data, pushed by the recompiler's vertex-load hooks in the
     *     stream-variant terrain loops. Stable under emission-count
     *     variation (a dropped vertex just misses; neighbors keep
     *     their own keys).
     *   - PHASE-B (bit31 clear): per-strip emission ordinal, the
     *     fallback for the subdivided/LOD variants whose midpoint
     *     vertices are synthesized (no source pointer). Exact only
     *     while per-strip emission counts hold.
     * Meaningless (0) for moby / untagged geometry. */
    uint32_t world_vkey;
    /* transform-interp §4: PSX vaddr of the moby that produced this
     * vertex (from the recompiler's per-moby ui-moby capture), or 0
     * for world / non-moby geometry. Real cross-frame identity,
     * replacing the matcher's centroid-hash heuristic. NOTE: until
     * moby-scope bracketing lands, this is best-effort (last ui-moby
     * ptr); consumers must gate on moby-pool validity. */
    uint32_t obj_tag;
    /* Day 4 Phase 1.1: PC of the MIPS instruction that issued the GTE
     * op for this vertex. Recompiled code writes a per-call PC into
     * openpete_gte_caller_pc immediately before each gte_execute(), and
     * gte.cpp snapshots that here at cache emit. 0 = unknown caller
     * (pre-regen code, hand-written thunks, interpreter). Used by
     * downstream per-prim-class discrimination (cyclorama vs world-moby
     * vs Spyro mesh vs HUD). */
    uint32_t gte_pc;
} openpete_render_cache_entry_t;

/* 8192 slots × ~64 B ≈ 512 KB per frame × 2 frames = 1 MB. Comfortably
 * fits in L2 on any host CPU. Load factor at 1945 verts ≈ 24 %. */
#define OPENPETE_RENDER_CACHE_SLOTS     8192u
#define OPENPETE_RENDER_CACHE_PROBE_MAX 16u

/* Producer (called from gte_rtps_internal). Fills sxy_key/seq/matrix_span_id
 * from current state; caller fills the rest (mac/v_local/rt/tr/ofx/ofy/h).
 * Bumps matrix_span_id internally when RT/TR observed-change since prior
 * emit. */
void openpete_render_cache_emit_vertex(const openpete_render_cache_entry_t* src);

/* Day 4 Phase 1.3 / Phase 2.5: per-game PC→perspective-eligibility
 * classifier. Set by games/<game>/ at boot. Called by the cache shim
 * on every lookup; returns nonzero if the vertex should use the
 * perspective shader branch (real w for hyperbolic UV interp), zero
 * for the ortho fallback. Default (NULL) behavior = canonical context
 * alone decides. Games override to filter out functions that author
 * affine vertex shading (cyclorama gradient, etc.). */
extern int (*openpete_pgxp_classify_fn)(uint32_t gte_pc, int is_canonical);

/* Consumer (called from PsyCross OT walker per vertex). Returns 1 on
 * hit (out filled), 0 on miss. */
int openpete_render_cache_lookup_curr(uint32_t sxy_key,
                                      openpete_render_cache_entry_t* out);
int openpete_render_cache_lookup_prev(uint32_t sxy_key,
                                      openpete_render_cache_entry_t* out);

/* PGXP consumer convenience — look up by SXY in the generation the
 * walked OT was emitted from (gen_prev2 when frame-gen displays the
 * lagged OT, gen_prev when the 'I' kill switch walks the current one;
 * see openpete-pgxp.md + transform-interp plan §11 F5) and convert the
 * cached entry into the float values PsyCross's ApplyVertexPGXP
 * expects. Returns 1 on hit (out filled), 0 on miss. The conversion
 * mirrors what PsyX_GTE.cpp::PGXP_EmitCacheData would have written if
 * it ran (it doesn't, because Spyro uses inline COP2 not the libgte
 * function API — see openpete-pgxp.md "the two findings"). */
typedef struct openpete_pgxp_lookup {
    float px, py, pz;   /* mac[3] / 128 * g_pgxpZScale + g_pgxpZOffset */
    float ofx, ofy;     /* gte->OFX/OFY are Q16 — divide by 65536 */
    float scr_h;        /* gte->H, direct cast to float */
} openpete_pgxp_lookup_t;

int openpete_render_cache_lookup_pgxp(uint32_t sxy_key,
                                      openpete_pgxp_lookup_t* out);
/* Interpolation "to" endpoint (keyed in gen_prev = G(N)) — used by the
 * matcher to build PGXP-consistent float-to-float substitute deltas. */
int openpete_render_cache_lookup_pgxp_to(uint32_t sxy_key,
                                         openpete_pgxp_lookup_t* out);
/* Projection core shared by both keyed lookups: float-project an
 * already-resolved cache entry. */
int openpete_render_cache_pgxp_project(const openpete_render_cache_entry_t* ep,
                                       openpete_pgxp_lookup_t* out);
/* Interpolation "from" endpoint (gen two game frames back = the GTE
 * that built the lagged OT we display). See openpete-interpolation-
 * frame-gen.md. */
int openpete_render_cache_lookup_prev2(uint32_t sxy_key,
                                       openpete_render_cache_entry_t* out);

/* Legacy immediate rotate — advances the 3-generation ring one step
 * synchronously. Retained for tests; production callers use the
 * deferred arm_rotate() instead. */
void openpete_render_cache_begin_frame(void);

/* Arm a deferred rotate. The next emit_vertex consumes the arm and
 * advances the ring. PadVSync-only yields produce no emits, so the
 * arm carries over harmlessly — this is the mechanism that keeps the
 * cache aligned with the 30 Hz GAME FRAME cadence on a 60 Hz VBL game
 * fiber. See openpete_render_cache.c header comment for rationale. */
void openpete_render_cache_arm_rotate(void);

/* Phase 2.1 — sim-tick boundary, split into two calls:
 *
 *   advance_time: update the "this sim tick started at T" timestamp +
 *     sim_period so substitute can compute alpha. Call BEFORE the
 *     game-fiber yield that runs DrawOTag, so substitute (inside that
 *     yield) reads alpha ≈ 0. Called once per sim-due iter (cheap).
 *
 *   rotate_cache: rotate s_curr ↔ s_prev. Call ONCE per Spyro game
 *     frame (after sim-due EndScene fires) so the OT walked next sim
 *     tick sees fresh cache state. */
#include <stdint.h>
typedef int64_t openpete_op_time_t;
void openpete_render_sim_tick_advance_time(openpete_op_time_t sim_tick_ns,
                                           openpete_op_time_t sim_period_ns);
void openpete_render_sim_tick_rotate_cache(void);

/* Phase 2.1 hotfix — re-stamp s_last_sim_tick_ns to op_now() at the
 * moment DrawOTag actually fires. advance_time is called from run.c
 * BEFORE the game-fiber yield, but the game runs a full VBL of logic
 * before DrawOTag → substitute would observe alpha = (~16 ms / 33 ms)
 * ≈ 0.5 of a sim period, smearing static geometry by alpha·velocity.
 * Calling this from psyq_DrawOTag immediately before prepare_substitute
 * pins the sim-tick origin to the actual DrawOTag moment, so:
 *   sim-due render:  alpha ≈ 0 (microseconds of substitute setup)
 *   intermediate:    alpha grows naturally from that origin */
void openpete_render_sim_tick_mark_now(void);

/* Phase 2.1 — return clamped alpha in [0, 1.0] for the time elapsed
 * since the last sim_tick_advanced call. Used by the substitute
 * pipeline at intermediate-render time. Returns 0.0 if no sim tick
 * has been recorded yet. */
float openpete_render_compute_alpha(openpete_op_time_t now_ns);

/* Phase 2.1 hotfix — snapshot alpha. The OT walk reads alpha once
 * per vertex via openpete_get_lerp_alpha(); calling op_now() per
 * vertex during a multi-ms walk lets alpha drift mid-frame and the
 * resulting per-vertex deltas alias as jitter on mispaired batches.
 * Snapshotted at two points (and ONLY those):
 *   mark_now()          → snapshot = 0  (sim-due render: ground truth)
 *   snapshot_intermediate() → snapshot = compute_alpha(op_now())
 * PsyX_GPU's get_lerp_alpha reads this global; env override still wins. */
extern float g_openpete_substitute_alpha_snapshot;
void openpete_render_substitute_alpha_snapshot_intermediate(void);

/* §5 / #24 camera-cut snap. The game-side per-tick discontinuity
 * detector (game_on_frame — owns the game-specific g_Camera layout)
 * arms/clears this every sim tick; while armed,
 * snapshot_intermediate pins alpha to 0 so the display cycle that
 * spans the cut presents the lagged OT unchanged (clean snap, no
 * full-screen lerp smear toward the post-cut viewpoint). */
void openpete_render_interp_snap_set(int snap);

/* Iterate every populated slot in s_curr (the "next-visible" buffer just
 * emitted by the game fiber's GTE pass). Used by Phase 2's
 * future-anchor scan to build FRAME-N batches from the in-flight cache
 * before the rotate moves it to s_prev. The callback is called once
 * per valid entry; iteration order is implementation-defined. */
typedef void (*openpete_render_cache_iter_fn)(
    const openpete_render_cache_entry_t* entry, void* userdata);

void openpete_render_cache_for_each_curr_entry(
    openpete_render_cache_iter_fn cb, void* userdata);

/* Iterate every populated slot in s_prev (the "currently visible"
 * buffer; consumer of OT walker lookups). Used by Phase 2's dual-scan
 * to build FRAME-N-1 anchors alongside FRAME-N future anchors. */
void openpete_render_cache_for_each_prev_entry(
    openpete_render_cache_iter_fn cb, void* userdata);

/* Iterate every populated slot in gen_prev2 (= G(N-1), the "from"
 * endpoint of the interpolation span; gen_prev = G(N) is "to"). */
void openpete_render_cache_for_each_prev2_entry(
    openpete_render_cache_iter_fn cb, void* userdata);

/* Diagnostic counters (per render frame; reset by PsyX_EndScene summary). */
extern unsigned g_openpete_render_cache_emit_count;
extern unsigned g_openpete_render_cache_lookup_count;
extern unsigned g_openpete_render_cache_hit_count;
extern unsigned g_openpete_render_cache_probe_overflow_count;
extern unsigned g_openpete_render_cache_matrix_span_id;

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_RENDER_CACHE_H */
