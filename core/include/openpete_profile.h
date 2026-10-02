/* openpete_profile.h - empirical CPU+GPU performance trace (--profile-out PATH)
 *
 * A performance trace that attributes CPU scope time to presents, keyed by
 * (present_index, canon_tick, kind), and GPU zone time to rendered frames,
 * keyed by the RHI frame serial. Records are
 * streamed to a compact binary `.opp` file; post-run Python converters turn it
 * into a Chrome/Perfetto trace (human flame charts) and a plain-text
 * summary (scripts/profile-to-trace.py, scripts/profile-summary.py).
 *
 * Relationship to the OPPT phase timers (openpete_native_render.h): the profiler
 * REUSES the existing OPPT/OPPC call sites - openpete_phase_add()/set_count()
 * forward to op_prof automatically when a trace is open - so the 9 phase scopes
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

/** @brief Open the trace stream at path, write the 32-byte header, and turn the profiler on.
 *
 * The header carries the magic "OPPR", the format version, a clock rate of
 * 1000000 (timestamps are microseconds) and a zero creation time. The name
 * table is cleared and every registered thread block is reset in place, so
 * a producer's cached block pointer stays valid across a reopen. The writer
 * mutex is created on first use and never destroyed.
 * @param path Output file path; NULL or empty leaves the profiler off,
 *        op_prof_on returns 0, and every other entry point is a no-op.
 * @return 0 when a trace is open or path is empty, -1 when the mutex or the
 *         file cannot be created. */
int  op_prof_open(const char* path);
/** @brief Flush every thread's pending buckets and chunk, close the stream, and free the name table.
 *
 * The profiler is turned off first, so producers entered afterwards return
 * before touching any state. Every registered thread is flushed, not only
 * the caller, since a render thread under eager production never reaches a
 * present boundary. The mutex is kept because op_prof_intern and the
 * emitters may still be entered from another thread. No-op when no trace is
 * open. */
void op_prof_close(void);

/** @brief Return nonzero while a trace is open.
 *
 * A plain load of one flag, the fast gate every emitter and the scope macros
 * test first.
 * @return 1 between op_prof_open and op_prof_close, else 0. */
int  op_prof_on(void);

/** @brief Return the SDL performance counter converted to microseconds.
 *
 * The clock shared with the OPPT phase timers. The conversion factor is
 * cached by op_prof_open; before that it is computed on each call.
 * @return Microseconds since the performance counter's origin. */
uint64_t op_prof_now_us(void);

/** @brief Intern a scope, zone or counter name and return its id, stable for the run.
 *
 * Repeated calls with the same string return the same id. A new name gets
 * the next id and a NAME record (type, u16 id, u16 length, bytes, length
 * capped at 65535) written straight to the stream rather than into a chunk,
 * so it precedes every record citing the id. Thread-safe; the lookup is a
 * strcmp walk under the writer mutex, so hot-path callers cache the id. At
 * most 8192 names.
 * @param name Name to intern.
 * @return The id, or -1 when the profiler is off, name is NULL, or the
 *         table is full. */
int  op_prof_intern(const char* name);

/** @brief Push a scope with the current time onto the calling thread's span stack.
 *
 * The stack holds 32 scopes; a push past that is dropped with one warning
 * per thread, and the matching op_prof_end then closes the enclosing scope.
 * No-op while the profiler is off.
 * @param name_id Id from op_prof_intern. */
void op_prof_begin(int name_id);
/** @brief Pop the calling thread's innermost scope and emit its SPAN record.
 *
 * The record carries the scope's name, start time, duration and nesting
 * depth. An end with an empty stack is ignored with one warning per
 * thread. */
void op_prof_end(void);

/** @brief Emit an already-measured span ending now on the calling thread.
 *
 * Used by the OPPT phase-add shim. The start is inferred as now - dur_us
 * (now when dur_us exceeds the clock), and the depth is the thread's current
 * span-stack depth, so a span forwarded inside an open scope nests under it.
 * @param name_id Id from op_prof_intern; negative is ignored.
 * @param dur_us Measured duration in microseconds. */
void op_prof_add_span(int name_id, uint64_t dur_us);

/** @brief Emit a COUNT record carrying a census value, such as a moby, triangle or draw count.
 *
 * The record carries the current present index for information only; the
 * reader bins counts by thread and marker interval.
 * @param name_id Id from op_prof_intern; negative is ignored.
 * @param value The census value. */
void op_prof_count(int name_id, uint64_t value);

/* ---- accumulating scopes ---------------------------------------------------
 * For regions entered many times per tick (dispatch hooks fire once per hooked
 * guest call - the 2D addPrim chokepoint runs hundreds of times a tick), one
 * SPAN record per entry would bloat the trace and put an fwrite in the hot path.
 * op_prof_accum() instead sums into a per-name bucket; the present boundary
 * emits ONE span per non-empty bucket, carrying the per-present call count in
 * the span's aux field.
 *
 * Emitted spans are depth 0 and anchored at the bucket's FIRST entry this
 * present (t0 = first entry, dur = summed time), so the interval stays inside
 * whatever real scope contained the entries and present_busy_us() - a union of
 * span intervals - is not inflated by the synthetic timestamp.
 *
 * Buckets are FLAT, not nested: a bucket that measures a region inside another
 * bucket's region (e.g. "terrain.srend", the guest renderer call inside
 * "hook.terrain_native") is a subset, and the consumer subtracts. */
/** @brief Add us to the calling thread's bucket for name_id and count one call.
 *
 * A bucket created by this call is anchored at now - us, the first entry of
 * the flush interval. Buckets are emitted as depth-0 spans by
 * op_prof_present, op_prof_thread_flush and op_prof_close. A thread holds at
 * most 64 buckets; a new name past that is dropped with one warning per
 * thread.
 * @param name_id Id from op_prof_intern; negative is ignored.
 * @param us Time measured for this entry, in microseconds. */
void op_prof_accum(int name_id, uint64_t us);

/** @brief Close the current present: flush the calling thread's buckets, emit a PRESENT record, and flush its chunk.
 *
 * Spans raised on the closing thread since its previous marker bind to this
 * present by stream order; spans from other threads bind by interval
 * containment in the consumer. The record carries the kind, the present
 * index, canon_tick, the thread's span count since its last marker, and the
 * time. The present index then advances.
 * @param canon_tick openpete_canon_tick.
 * @param kind OP_PROF_CANONICAL or OP_PROF_INTERMEDIATE.
 * @return This present's index; while the profiler is off, the current
 *         index, which does not advance. */
uint32_t op_prof_present(uint32_t canon_tick, int kind);

/** @brief Emit a GPUZ record for one GPU zone result harvested from a rendered frame.
 *
 * The native renderer harvests the results from rhi_gpu_zone_*. Consumers
 * group zones by the RHI frame serial that submitted them.
 * @param name_id Id from op_prof_intern; negative is ignored.
 * @param depth Zone nesting depth.
 * @param frame_serial RHI frame serial that submitted the zone.
 * @param t0_rel_us Start relative to that frame's first GPU timestamp.
 * @param dur_us Duration in microseconds. */
void op_prof_gpu_zone(int name_id, int depth, uint64_t frame_serial,
                      uint32_t t0_rel_us, uint32_t dur_us);

/** @brief Emit the calling thread's pending buckets and write its chunk to the stream.
 *
 * The marker for a producer that never closes a present. The native
 * renderer calls it at the end of each rendered frame, so the render
 * thread's accumulating scopes land one span per frame instead of one per
 * run. No-op while the profiler is off. */
void op_prof_thread_flush(void);

/** @brief Emit a FUNC record carrying one traced function's aggregate for the run.
 *
 * func_trace.c emits one per traced function at shutdown.
 * @param name_id Id from op_prof_intern; negative is ignored.
 * @param addr Function address.
 * @param calls Total calls, saturated to 32 bits in the record.
 * @param self_us Time excluding callees, in microseconds.
 * @param total_us Time including callees, in microseconds. */
void op_prof_func(int name_id, uint32_t addr, uint64_t calls,
                  uint64_t self_us, uint64_t total_us);

/* ---- RAII-ish scope macro (GCC/Clang cleanup attribute) --------------------
 * Usage: OP_PROF_SCOPE("compose.qsort"); at the top of a block; the span ends
 * when the block exits. The name is interned once (function-static cache). */
#if defined(__GNUC__) || defined(__clang__)
/** @brief Cleanup handler of OP_PROF_SCOPE: close the span when it was opened.
 *
 * Runs when the scope's block exits, through the cleanup attribute.
 * @param began Points to the scope's flag, nonzero once op_prof_begin ran. */
static inline void op_prof__scope_cleanup(int* began) { if (*began) op_prof_end(); }
typedef struct { int name_id; uint64_t t0_us; } op_prof_accum_guard;
/** @brief Cleanup handler of OP_PROF_ACCUM_SCOPE: add the elapsed time to the scope's bucket.
 *
 * Runs when the scope's block exits, through the cleanup attribute. Adds
 * the microseconds since g->t0_us to bucket g->name_id with op_prof_accum;
 * a negative name_id, the value while the profiler is off, adds nothing.
 * @param g Points to the scope's guard. */
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
