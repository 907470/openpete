/* func_trace.c - see openpete_func_trace.h.
 *
 * The __cyg_profile_func_enter/exit hooks fire around every function in a TU
 * compiled with -finstrument-functions (only PSX.EXE_full.c, under
 * -DOPENPETE_FUNC_TRACE). Each enter pushes a shadow-stack frame; each exit
 * accumulates self time (own, excluding children) and total time into a
 * per-function bin resolved from the build-generated symtab. At shutdown the
 * aggregate is emitted to the perf trace and printed.
 *
 * The hooks themselves must never be instrumented (they'd recurse infinitely) -
 * this whole TU is compiled WITHOUT the flag, and the hooks carry
 * no_instrument_function as belt-and-suspenders.
 */
#include "openpete_func_trace.h"
#include "openpete_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

/* The generated symtab (tools/gen_func_trace_symtab.py) registers itself here at
 * startup via a constructor - portable across ELF/PE, and no weak symbols (which
 * mingw won't pull from an archive to satisfy an undefined ref). NULL until a
 * FUNC_TRACE build registers a table; the tracer stays inert otherwise. */
static const op_func_sym_t *s_symtab = NULL;
static int s_symtab_n = 0;

void op_functrace_register_symtab(const op_func_sym_t *tab, int count) {
    s_symtab = tab;
    s_symtab_n = count;
}

#define NO_INSTR __attribute__((no_instrument_function))

/* ---- per-function accumulators (indexed like op_func_symtab) --------------- */
static uint64_t *s_total_ticks = NULL;
static uint64_t *s_self_ticks  = NULL;
static uint64_t *s_calls       = NULL;
static int       s_nfuncs      = 0;
static int       s_active      = 0;   /* a hook has fired at least once */

/* ---- pointer -> index open-addressed hash ----------------------------------- */
static int     *s_hash = NULL;        /* -1 empty, else symtab index */
static uint32_t s_hash_mask = 0;

/* ---- shadow stack ---------------------------------------------------------- */
#define FT_STACK_MAX 512
struct ft_frame { int idx; uint64_t t_enter; uint64_t child_ticks; };
static struct ft_frame s_stack[FT_STACK_MAX];
static int s_sp = 0;
static int s_over = 0;                 /* frames beyond FT_STACK_MAX (kept balanced) */
static SDL_ThreadID s_tid = 0;

/* Virtual clock: real perf-counter minus time spent yielded to the host, so
 * host render + pacing sleep don't count against on-stack game functions. */
static uint64_t s_excluded = 0;
static uint64_t s_pause_start = 0;

/** @brief Return the virtual clock: the performance counter minus the time spent paused. */
static NO_INSTR uint64_t ft_now(void) { return SDL_GetPerformanceCounter() - s_excluded; }

/** @brief Hash a pointer to 32 bits with a 64-bit multiply-xorshift mix. */
static NO_INSTR uint32_t ptr_hash(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x;
}

/** @brief Allocate the per-function accumulators and the pointer hash from the registered symtab.
 *
 * Sizes three counter arrays to the symtab, builds an open-addressed hash of
 * function pointers to symtab indices at a power-of-two capacity of at least
 * twice the entry count, and records the calling thread as the only thread
 * the hooks accept. Sets s_active to 1 on success and to -1 (inert) when no
 * symtab is registered or an allocation fails. */
static NO_INSTR void ft_init(void) {
    s_nfuncs = s_symtab_n;
    if (s_nfuncs <= 0 || !s_symtab) { s_active = -1; return; }   /* no symtab: inert */
    s_total_ticks = calloc((size_t)s_nfuncs, sizeof(uint64_t));
    s_self_ticks  = calloc((size_t)s_nfuncs, sizeof(uint64_t));
    s_calls       = calloc((size_t)s_nfuncs, sizeof(uint64_t));

    uint32_t cap = 1;
    while (cap < (uint32_t)s_nfuncs * 2u) cap <<= 1;
    s_hash_mask = cap - 1;
    s_hash = malloc(cap * sizeof(int));
    for (uint32_t i = 0; i < cap; i++) s_hash[i] = -1;
    for (int i = 0; i < s_nfuncs; i++) {
        uint32_t h = ptr_hash(s_symtab[i].fn) & s_hash_mask;
        while (s_hash[h] != -1) h = (h + 1) & s_hash_mask;
        s_hash[h] = i;
    }
    if (!s_total_ticks || !s_self_ticks || !s_calls || !s_hash) { s_active = -1; return; }
    s_tid = SDL_GetCurrentThreadID();
    s_active = 1;
}

/** @brief Look fn up in the pointer hash by linear probing.
 *
 * @param fn Function address as passed to the enter hook.
 * @return The symtab index whose fn equals the argument, -1 when the symtab
 *         does not list it. */
static NO_INSTR int resolve(const void *fn) {
    uint32_t h = ptr_hash(fn) & s_hash_mask;
    for (;;) {
        int i = s_hash[h];
        if (i < 0) return -1;
        if (s_symtab[i].fn == fn) return i;
        h = (h + 1) & s_hash_mask;
    }
}

/* ---- the instrumentation hooks --------------------------------------------- */
/** @brief -finstrument-functions entry hook: push a shadow-stack frame for this_fn.
 *
 * The first call initializes the tracer on the calling thread; without a
 * registered symtab the tracer stays inert. Calls from any other thread are
 * ignored so the single shared stack stays consistent. Past FT_STACK_MAX
 * frames the call is only counted, so the exit hook stays balanced. A
 * function the symtab does not list gets index -1 and accrues no bin.
 * @param this_fn Address of the entered function.
 * @param call_site Unused. */
NO_INSTR void __cyg_profile_func_enter(void *this_fn, void *call_site) {
    (void)call_site;
    if (s_active == 0) ft_init();
    if (s_active != 1) return;
    /* Only the main (game/host) thread runs instrumented code; ignore any stray
     * call from another thread so the single shared stack stays consistent. */
    if (SDL_GetCurrentThreadID() != s_tid) return;
    if (s_sp >= FT_STACK_MAX) { s_over++; return; }
    s_stack[s_sp].idx = resolve(this_fn);
    s_stack[s_sp].t_enter = ft_now();
    s_stack[s_sp].child_ticks = 0;
    s_sp++;
}

/** @brief -finstrument-functions exit hook: pop the shadow-stack frame and accrue its times.
 *
 * Ignored while the tracer is not active and on any thread other than the
 * one that initialized it. An exit that pairs with an enter dropped for
 * exceeding FT_STACK_MAX only decrements the overflow count. Otherwise the
 * elapsed virtual time is added to the popped function's total, its total
 * minus the time spent in callees to its self time, and its call count is
 * incremented; the popped total is then charged to the parent frame as
 * child time. A frame whose function was not in the symtab accrues nothing
 * but still charges its parent.
 * @param this_fn Unused.
 * @param call_site Unused. */
NO_INSTR void __cyg_profile_func_exit(void *this_fn, void *call_site) {
    (void)this_fn; (void)call_site;
    if (s_active != 1) return;
    if (SDL_GetCurrentThreadID() != s_tid) return;
    if (s_over > 0) { s_over--; return; }
    if (s_sp <= 0) return;
    s_sp--;
    uint64_t total = ft_now() - s_stack[s_sp].t_enter;
    int idx = s_stack[s_sp].idx;
    if (idx >= 0) {
        s_total_ticks[idx] += total;
        s_self_ticks[idx]  += total - s_stack[s_sp].child_ticks;
        s_calls[idx]++;
    }
    if (s_sp > 0) s_stack[s_sp - 1].child_ticks += total;  /* charge parent */
}

int op_functrace_on(void) { return s_active == 1; }

void op_functrace_pause(void) {
    if (s_active == 1 && SDL_GetCurrentThreadID() == s_tid)
        s_pause_start = SDL_GetPerformanceCounter();
}
void op_functrace_resume(void) {
    if (s_active == 1 && s_pause_start && SDL_GetCurrentThreadID() == s_tid) {
        s_excluded += SDL_GetPerformanceCounter() - s_pause_start;
        s_pause_start = 0;
    }
}

/* ---- shutdown dump --------------------------------------------------------- */
struct ft_row { int idx; uint64_t self; uint64_t total; uint64_t calls; };

/** @brief qsort comparator ordering ft_row entries by descending self time. */
static NO_INSTR int ft_cmp_self(const void *a, const void *b) {
    uint64_t x = ((const struct ft_row *)a)->self, y = ((const struct ft_row *)b)->self;
    return (x < y) - (x > y);   /* descending */
}

void op_functrace_dump_aggregate(void) {
    if (s_active != 1 || s_nfuncs <= 0) return;
    double us_per_tick = 1e6 / (double)SDL_GetPerformanceFrequency();

    struct ft_row *rows = malloc((size_t)s_nfuncs * sizeof(*rows));
    if (!rows) return;
    int n = 0;
    for (int i = 0; i < s_nfuncs; i++) {
        if (s_calls[i] == 0) continue;
        rows[n].idx = i; rows[n].self = s_self_ticks[i];
        rows[n].total = s_total_ticks[i]; rows[n].calls = s_calls[i];
        n++;
    }
    qsort(rows, (size_t)n, sizeof(*rows), ft_cmp_self);

    /* Emit every called function into the trace (self-time order). */
    for (int i = 0; i < n; i++) {
        const op_func_sym_t *s = &s_symtab[rows[i].idx];
        int nid = op_prof_intern(s->name);
        op_prof_func(nid, s->addr, rows[i].calls,
                     (uint64_t)(rows[i].self * us_per_tick),
                     (uint64_t)(rows[i].total * us_per_tick));
    }

    /* Human-facing top-20 to stderr. */
    fprintf(stderr, "[func-trace] %d functions called; top by self-time (us total):\n", n);
    for (int i = 0; i < n && i < 20; i++) {
        const op_func_sym_t *s = &s_symtab[rows[i].idx];
        fprintf(stderr, "  0x%08x %-28s calls=%llu self=%llu total=%llu\n",
                s->addr, s->name, (unsigned long long)rows[i].calls,
                (unsigned long long)(rows[i].self * us_per_tick),
                (unsigned long long)(rows[i].total * us_per_tick));
    }
    if (s_over) fprintf(stderr, "[func-trace] note: stack exceeded %d frames %d times\n",
                        FT_STACK_MAX, s_over);
    free(rows);
}
