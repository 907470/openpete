/* openpete_func_trace.h — opt-in per-function CPU tracing of the recompiled
 * game code (Tier B drill-down; see openpete_profile.h for Tier A scopes).
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

/* The generated symtab TU registers its table here via a startup constructor
 * (portable across ELF/PE — no weak symbols). Without a FUNC_TRACE build no one
 * calls this, so the tracer stays inert. */
void op_functrace_register_symtab(const op_func_sym_t *tab, int count);

/* True once at least one instrumented function has been seen (i.e. this is a
 * FUNC_TRACE build actually running instrumented code). */
int  op_functrace_on(void);

/* Pause/resume the func-trace clock around a game-fiber → host yield, so the
 * host's render + pacing-sleep time is NOT charged to the game functions left
 * on the shadow stack across the yield (otherwise the frame-loop functions that
 * wrap VSync absorb all wall-clock time). Called from openpete_yield_to_host;
 * inert unless tracing is active. Balanced pause/resume only. */
void op_functrace_pause(void);
void op_functrace_resume(void);

/* Emit the per-function aggregate (calls / self µs / total µs) into the active
 * perf trace and print the top entries to stderr. Call once at shutdown, while
 * the --profile-out stream is still open. No-op when no data was collected. */
void op_functrace_dump_aggregate(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FUNC_TRACE_H */
