/* openpete_profile.h — empirical CPU+GPU performance trace (--profile-out PATH)
 *
 * A performance trace that attributes CPU scope time to presents, keyed by
 * (present_index, canon_tick, kind), and GPU zone time to rendered frames,
 * keyed by the RHI frame serial. Records are
 * streamed to a compact binary `.opp` file; post-run Python converters turn it
 * into a Chrome/Perfetto trace (human flame charts) and a compact text
 * summary (scripts/profile-to-trace.py, scripts/profile-summary.py).
 *
 * Relationship to the OPPT phase timers (openpete_native_render.h): the profiler
 * REUSES the existing OPPT/OPPC call sites — openpete_phase_add()/set_count()
 * forward to op_prof automatically when a trace is open — so the 9 phase scopes
 * and 4 census counters appear with zero new call sites. New finer scopes use
 * OP_PROF_SCOPE()/op_prof_begin()/op_prof_end() directly, or
 * OP_PROF_ACCUM_SCOPE() when the region is entered many times per tick.
 *
 * Everything is inert (one predictable branch) unless --profile-out is set.
 * Diagnostic-only; never touches gameplay state. Every entry point is callable
 * from any thread: span stacks and accumulator buckets are per-thread, records
 * carry the raising thread's index, and each thread buffers its own records.
 */
#ifndef OPENPETE_PROFILE_H
#define OPENPETE_PROFILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* frame_kind of a present */
enum { OP_PROF_CANONICAL = 0, OP_PROF_INTERMEDIATE = 1 };

/* Open/close the trace stream. path==NULL or "" ⇒ profiler stays off (op_prof_on
 * returns 0 and every other entry point is a no-op). Returns 0 on success. */
int  op_prof_open(const char* path);
void op_prof_close(void);

/* Fast gate: nonzero when a trace is open. Cache-friendly (a single load). */
int  op_prof_on(void);

/* Microsecond clock shared with the OPPT timers (SDL perf counter). */
uint64_t op_prof_now_us(void);

/* Intern a scope/zone/counter name → small integer id (stable for the run).
 * Repeated calls with the same string return the same id. Thread-safe; the
 * lookup is a strcmp walk under the writer mutex, so callers on a hot path
 * cache the id at the call site. */
int  op_prof_intern(const char* name);

/* CPU scope stack. begin pushes, end pops and emits a completed SPAN with the
 * measured duration and its nesting depth. Unbalanced end ⇒ ignored + warned. */
void op_prof_begin(int name_id);
void op_prof_end(void);

/* Emit an already-measured completed span (used by the OPPT phase-add shim:
 * t0 is inferred as now-us). depth is recorded as 0 (sibling phase). */
void op_prof_add_span(int name_id, uint64_t dur_us);

/* Census value for the current/next present (e.g. mobys/tris/draws/culled). */
void op_prof_count(int name_id, uint64_t value);

/* ---- accumulating scopes ---------------------------------------------------
 * For regions entered many times per tick (dispatch hooks fire once per hooked
 * guest call — the 2D addPrim chokepoint runs hundreds of times a tick), one
 * SPAN record per entry would bloat the trace and put an fwrite in the hot path.
 * op_prof_accum() instead sums into a per-name bucket; the present boundary
 * emits ONE span per non-empty bucket, carrying the per-present call count in
 * the span's aux field.
 *
 * Emitted spans are depth 0 and anchored at the bucket's FIRST entry this
 * present (t0 = first entry, dur = summed time), so the interval stays inside
 * whatever real scope contained the entries and present_busy_us() — a union of
 * span intervals — is not inflated by the synthetic timestamp.
 *
 * Buckets are FLAT, not nested: a bucket that measures a region inside another
 * bucket's region (e.g. "terrain.srend", the guest renderer call inside
 * "hook.terrain_native") is a subset, and the consumer subtracts. */
void op_prof_accum(int name_id, uint64_t us);

/* Close the current present. Spans raised on the closing thread since its
 * previous marker bind to this present by stream order; spans from other
 * threads bind by interval containment in the consumer. canon_tick is
 * openpete_canon_tick; kind is OP_PROF_CANONICAL or OP_PROF_INTERMEDIATE.
 * Returns this present's index. */
uint32_t op_prof_present(uint32_t canon_tick, int kind);

/* GPU zone result harvested for one rendered frame (from rhi_gpu_zone_*),
 * keyed by the RHI frame serial that submitted it. t0_rel_us is relative to
 * that frame's first GPU timestamp. Consumers group zones by frame_serial. */
void op_prof_gpu_zone(int name_id, int depth, uint64_t frame_serial,
                      uint32_t t0_rel_us, uint32_t dur_us);

/* Marker for a producer that never closes a present: emits this thread's
 * pending accumulator buckets and writes its chunk to the stream. Called at
 * the end of each rendered frame, so the render thread's accum scopes land one
 * span per frame instead of one span per run. */
void op_prof_thread_flush(void);

/* Per-function aggregate record (Tier B, from func_trace.c). Emitted once per
 * traced function at shutdown: total calls, self µs (excl. children), total µs
 * (incl. children) across the run. */
void op_prof_func(int name_id, uint32_t addr, uint64_t calls,
                  uint64_t self_us, uint64_t total_us);

/* ---- RAII-ish scope macro (GCC/Clang cleanup attribute) --------------------
 * Usage: OP_PROF_SCOPE("compose.qsort"); at the top of a block; the span ends
 * when the block exits. The name is interned once (function-static cache). */
#if defined(__GNUC__) || defined(__clang__)
static inline void op_prof__scope_cleanup(int* began) { if (*began) op_prof_end(); }
typedef struct { int name_id; uint64_t t0_us; } op_prof_accum_guard;
static inline void op_prof__accum_cleanup(op_prof_accum_guard* g) {
    if (g->name_id >= 0) op_prof_accum(g->name_id, op_prof_now_us() - g->t0_us);
}
#define OP_PROF__CAT2(a, b) a##b
#define OP_PROF__CAT(a, b)  OP_PROF__CAT2(a, b)
#define OP_PROF_SCOPE(lit) \
    static int OP_PROF__CAT(op_prof_id_, __LINE__) = -1; \
    int OP_PROF__CAT(op_prof_began_, __LINE__) \
        __attribute__((cleanup(op_prof__scope_cleanup))) = 0; \
    if (op_prof_on()) { \
        if (OP_PROF__CAT(op_prof_id_, __LINE__) < 0) \
            OP_PROF__CAT(op_prof_id_, __LINE__) = op_prof_intern(lit); \
        op_prof_begin(OP_PROF__CAT(op_prof_id_, __LINE__)); \
        OP_PROF__CAT(op_prof_began_, __LINE__) = 1; \
    }
/* Accumulating variant: same block-scoped usage, but the measured time is
 * summed into a per-name bucket and emitted once per present (see
 * op_prof_accum). Use for hooks that fire many times per tick. */
#define OP_PROF_ACCUM_SCOPE(lit) \
    static int OP_PROF__CAT(op_prof_aid_, __LINE__) = -1; \
    op_prof_accum_guard OP_PROF__CAT(op_prof_ag_, __LINE__) \
        __attribute__((cleanup(op_prof__accum_cleanup))) = { -1, 0 }; \
    if (op_prof_on()) { \
        if (OP_PROF__CAT(op_prof_aid_, __LINE__) < 0) \
            OP_PROF__CAT(op_prof_aid_, __LINE__) = op_prof_intern(lit); \
        OP_PROF__CAT(op_prof_ag_, __LINE__).name_id = \
            OP_PROF__CAT(op_prof_aid_, __LINE__); \
        OP_PROF__CAT(op_prof_ag_, __LINE__).t0_us = op_prof_now_us(); \
    }
#else
#define OP_PROF_SCOPE(lit) ((void)0)
#define OP_PROF_ACCUM_SCOPE(lit) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PROFILE_H */
