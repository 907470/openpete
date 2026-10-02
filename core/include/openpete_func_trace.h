/* openpete_func_trace.h — opt-in per-function CPU tracing of the recompiled
 * game code (function-level drill-down; see openpete_profile.h for the
 * profiler's named scopes).
 *
 * Built with -DOPENPETE_FUNC_TRACE=ON, which compiles the main generated EXE
 * (PSX.EXE_full.c) with -finstrument-functions. GCC/Clang then call the
 * __cyg_profile_func_enter/exit hooks in func_trace.c around every recompiled
 * func_80XXXXXX. A build-generated symtab (tools/gen_func_trace_symtab.py)
 * resolves each function POINTER to its manifest name; a shadow stack tracks
 * self vs child time. At shutdown the per-function aggregate (calls / self µs /
 * total µs) is emitted into the active --profile-out trace and printed to
 * stderr.
 *
 * Overhead is ~1-8 ms/tick (a hash probe + two clock reads per call), so this
 * is a drill-down build, NEVER the regression-baseline harness. When the build
 * option is OFF the generated TU carries no instrumentation, the hooks are
 * never called, and everything here is inert (the symtab weak-refs to empty).
 *
 * Timing perturbs no gameplay state — the hooks only read the clock — so an
 * OPENPETE_FUNC_TRACE build must still pass the oracle gate.
 */
#ifndef OPENPETE_FUNC_TRACE_H
#define OPENPETE_FUNC_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One recompiled function: its address-of (compared against the hook's this_fn),
 * PSX vaddr, and manifest name. The generated symtab TU defines the array; it
 * is weak so a non-FUNC_TRACE build links with an empty table. */
typedef struct {
    const void *fn;      /* &func_80XXXXXX                                  */
    uint32_t    addr;    /* PSX virtual address                             */
    const char *name;    /* manifest name (or "func_80XXXXXX" fallback)     */
} op_func_sym_t;

/** @brief Register the build-generated function symtab the tracer resolves hook addresses against.
 *
 * The generated symtab TU (tools/gen_func_trace_symtab.py) calls this from a
 * startup constructor, which works on ELF and PE without weak symbols. Without
 * a FUNC_TRACE build no one calls it and the tracer stays inert. The table is
 * stored by pointer, not copied.
 * @param tab The symtab array; must outlive the process.
 * @param count Number of entries in tab. */
void op_functrace_register_symtab(const op_func_sym_t *tab, int count);

/** @brief Return nonzero once the tracer is active.
 *
 * The tracer activates on the first entry hook when a symtab is registered
 * and its per-function tables allocate; a build without instrumentation or
 * without a symtab returns 0.
 * @return 1 while tracing is active, else 0. */
int  op_functrace_on(void);

/** @brief Stop the func-trace clock at a game-fiber to host yield.
 *
 * The host's render and pacing-sleep time is then not charged to the game
 * functions left on the shadow stack across the yield; without it the
 * functions that wrap VSync absorb all wall-clock time.
 * openpete_yield_to_host and openpete_yield_to_host_no_tick call it before
 * co_switch and op_functrace_resume after. Inert unless tracing is active and
 * the caller is the thread that initialized the tracer. Calls must pair with
 * op_functrace_resume. */
void op_functrace_pause(void);
/** @brief Restart the func-trace clock after a yield, adding the paused span to the excluded time.
 *
 * Inert unless tracing is active, a pause is pending, and the caller is the
 * thread that initialized the tracer. */
void op_functrace_resume(void);

/** @brief Emit the per-function aggregate into the active perf trace and print the top 20 by self time to stderr.
 *
 * Each function with at least one call is emitted with its call count, self
 * microseconds and total microseconds, in descending self-time order. Call
 * once at shutdown while the --profile-out stream is still open. No-op when
 * the tracer never activated or has no symtab. */
void op_functrace_dump_aggregate(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FUNC_TRACE_H */
