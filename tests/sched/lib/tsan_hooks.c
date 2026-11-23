/* tsan_hooks.c - the __tsan_* entry points that -fsanitize=thread code
 * calls, defined over the scheduler instead of the ThreadSanitizer runtime.
 *
 * Code under test compiled with -fsanitize=thread calls one of these before
 * every memory access and in place of every C11 atomic operation. An access
 * whose address lies in a range registered with sched_watch yields before it
 * (SCHED_OP_READ, SCHED_OP_WRITE or SCHED_OP_ATOMIC); every other access runs
 * through. Atomic operations execute sequentially consistent whatever
 * memory order was asked for. libtsan is never linked: a __tsan_* entry
 * this file does not define is a link error. */
#include "op_sched_internal.h"

#include <stdint.h>
#include <stdio.h>

enum { MAX_WATCH = 64, MAX_EXCLUDE = 64 };

/** A watched range, with the slot of the mutex that guards it and its ownership predicate (either may be NULL). */
static struct { uintptr_t lo, hi; const char *name; void *const *guard; int (*owned)(const void *, int); } s_watch[MAX_WATCH];
static int s_nwatch;
static struct { uintptr_t lo, hi; } s_excl[MAX_EXCLUDE];
static int s_nexcl;
static unsigned long s_seen, s_hits, s_guarded;
static int s_probe_on;
static const void *s_probe_addr;

void sched_watch_guarded(const void *addr, size_t len, const char *name, void *const *guard_slot)
{
    if (s_nwatch >= MAX_WATCH) return;
    s_watch[s_nwatch].lo = (uintptr_t)addr;
    s_watch[s_nwatch].hi = (uintptr_t)addr + len;
    s_watch[s_nwatch].name = name;
    s_watch[s_nwatch].guard = guard_slot;
    s_watch[s_nwatch].owned = NULL;
    s_nwatch++;
    sched_name(addr, name);
}

void sched_watch_owned(const void *addr, size_t len, const char *name, int (*owned)(const void *addr, int thread))
{
    sched_watch_guarded(addr, len, name, NULL);
    if (s_nwatch > 0 && s_watch[s_nwatch - 1].lo == (uintptr_t)addr) s_watch[s_nwatch - 1].owned = owned;
}

void sched_watch(const void *addr, size_t len, const char *name) { sched_watch_guarded(addr, len, name, NULL); }

void sched_watch_exclude(const void *addr, size_t len)
{
    if (s_nexcl >= MAX_EXCLUDE) return;
    s_excl[s_nexcl].lo = (uintptr_t)addr;
    s_excl[s_nexcl].hi = (uintptr_t)addr + len;
    s_nexcl++;
}

const void *sched_watch_probe(int on)
{
    const void *a = s_probe_addr;
    s_probe_on = on;
    if (on) s_probe_addr = NULL;
    return a;
}

unsigned long sched_watch_guarded_count(void) { return s_guarded; }

/** @brief Report whether the calling thread holds the mutex a guard slot names.
 * @param slot The slot, or NULL.
 * @return 1 when it does. */
static int guard_held(void *const *slot)
{
    if (!slot || !*slot) return 0;
    return ((const sched_mutex *)*slot)->owner == sched_self();
}

unsigned long sched_watch_counts(unsigned long *hits)
{
    if (hits) *hits = s_hits;
    return s_seen;
}

/** @brief Yield before an access when its address is watched and the caller is scheduled.
 * @param kind SCHED_OP_READ, SCHED_OP_WRITE or SCHED_OP_ATOMIC.
 * @param a The address.
 * @param ra The instrumented code's address. */
static void hook(sched_op_kind kind, const volatile void *a, const void *ra)
{
    s_seen++;
    if (!sched_active()) return;
    const uintptr_t p = (uintptr_t)a;
    for (int i = 0; i < s_nexcl; i++)
        if (p >= s_excl[i].lo && p < s_excl[i].hi) return;
    for (int i = 0; i < s_nwatch; i++) {
        if (p < s_watch[i].lo || p >= s_watch[i].hi) continue;
        if (guard_held(s_watch[i].guard) ||
            (s_watch[i].owned && s_watch[i].owned((const void *)(uintptr_t)a, sched_self()))) { s_guarded++; return; }
        s_hits++;
        sched_point_ra(kind, (const void *)(uintptr_t)a, ra);
        return;
    }
}

/** @brief Record an atomic store's address while the probe is on and nothing is recorded yet.
 * @param a The address. */
static void probe(const volatile void *a)
{
    if (s_probe_on && !s_probe_addr) s_probe_addr = (const void *)a;
}

/* Plain accesses. */
#define RA __builtin_return_address(0)
#define PLAIN(n) \
    void __tsan_read##n(void *a); void __tsan_write##n(void *a); \
    void __tsan_unaligned_read##n(void *a); void __tsan_unaligned_write##n(void *a); \
    void __tsan_read##n(void *a) { hook(SCHED_OP_READ, a, RA); } \
    void __tsan_write##n(void *a) { hook(SCHED_OP_WRITE, a, RA); } \
    void __tsan_unaligned_read##n(void *a) { hook(SCHED_OP_READ, a, RA); } \
    void __tsan_unaligned_write##n(void *a) { hook(SCHED_OP_WRITE, a, RA); }
PLAIN(1) PLAIN(2) PLAIN(4) PLAIN(8) PLAIN(16)

void __tsan_read_range(void *a, unsigned long n);
void __tsan_write_range(void *a, unsigned long n);
/** @brief Instrumented read of a byte range (struct copies, memcpy-like code).
 * @param a Start.
 * @param n Length. */
void __tsan_read_range(void *a, unsigned long n) { (void)n; hook(SCHED_OP_READ, a, RA); }
/** @brief Instrumented write of a byte range.
 * @param a Start.
 * @param n Length. */
void __tsan_write_range(void *a, unsigned long n) { (void)n; hook(SCHED_OP_WRITE, a, RA); }

void __tsan_init(void);
void __tsan_func_entry(void *pc);
void __tsan_func_exit(void);
/** @brief Module constructor entry: nothing to set up. */
void __tsan_init(void) {}
/** @brief Function entry: unused.
 * @param pc The caller's address. */
void __tsan_func_entry(void *pc) { (void)pc; }
/** @brief Function exit: unused. */
void __tsan_func_exit(void) {}

void __tsan_atomic_thread_fence(int mo);
void __tsan_atomic_signal_fence(int mo);
/** @brief Thread fence: every hooked atomic is sequentially consistent already.
 * @param mo Memory order. */
void __tsan_atomic_thread_fence(int mo) { (void)mo; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
/** @brief Signal fence: nothing to order.
 * @param mo Memory order. */
void __tsan_atomic_signal_fence(int mo) { (void)mo; }

/* Atomic operations, every size and operation of the tsan interface. */
#define SC __ATOMIC_SEQ_CST
#define ATOMICS(n, T) \
    T __tsan_atomic##n##_load(const volatile T *a, int mo); \
    void __tsan_atomic##n##_store(volatile T *a, T v, int mo); \
    T __tsan_atomic##n##_exchange(volatile T *a, T v, int mo); \
    T __tsan_atomic##n##_fetch_add(volatile T *a, T v, int mo); \
    T __tsan_atomic##n##_fetch_sub(volatile T *a, T v, int mo); \
    T __tsan_atomic##n##_fetch_and(volatile T *a, T v, int mo); \
    T __tsan_atomic##n##_fetch_or(volatile T *a, T v, int mo); \
    T __tsan_atomic##n##_fetch_xor(volatile T *a, T v, int mo); \
    T __tsan_atomic##n##_fetch_nand(volatile T *a, T v, int mo); \
    int __tsan_atomic##n##_compare_exchange_strong(volatile T *a, T *c, T v, int mo, int fmo); \
    int __tsan_atomic##n##_compare_exchange_weak(volatile T *a, T *c, T v, int mo, int fmo); \
    T __tsan_atomic##n##_compare_exchange_val(volatile T *a, T c, T v, int mo, int fmo); \
    T __tsan_atomic##n##_load(const volatile T *a, int mo) \
        { (void)mo; hook(SCHED_OP_ATOMIC, a, RA); return __atomic_load_n(a, SC); } \
    void __tsan_atomic##n##_store(volatile T *a, T v, int mo) \
        { (void)mo; probe(a); hook(SCHED_OP_ATOMIC, a, RA); __atomic_store_n(a, v, SC); } \
    T __tsan_atomic##n##_exchange(volatile T *a, T v, int mo) \
        { (void)mo; hook(SCHED_OP_ATOMIC, a, RA); return __atomic_exchange_n(a, v, SC); } \
    T __tsan_atomic##n##_fetch_add(volatile T *a, T v, int mo) \
        { (void)mo; hook(SCHED_OP_ATOMIC, a, RA); return __atomic_fetch_add(a, v, SC); } \
    T __tsan_atomic##n##_fetch_sub(volatile T *a, T v, int mo) \
        { (void)mo; hook(SCHED_OP_ATOMIC, a, RA); return __atomic_fetch_sub(a, v, SC); } \
    T __tsan_atomic##n##_fetch_and(volatile T *a, T v, int mo) \
        { (void)mo; hook(SCHED_OP_ATOMIC, a, RA); return __atomic_fetch_and(a, v, SC); } \
    T __tsan_atomic##n##_fetch_or(volatile T *a, T v, int mo) \
        { (void)mo; hook(SCHED_OP_ATOMIC, a, RA); return __atomic_fetch_or(a, v, SC); } \
    T __tsan_atomic##n##_fetch_xor(volatile T *a, T v, int mo) \
        { (void)mo; hook(SCHED_OP_ATOMIC, a, RA); return __atomic_fetch_xor(a, v, SC); } \
    T __tsan_atomic##n##_fetch_nand(volatile T *a, T v, int mo) \
        { (void)mo; hook(SCHED_OP_ATOMIC, a, RA); return __atomic_fetch_nand(a, v, SC); } \
    int __tsan_atomic##n##_compare_exchange_strong(volatile T *a, T *c, T v, int mo, int fmo) \
        { (void)mo; (void)fmo; hook(SCHED_OP_ATOMIC, a, RA); \
          return __atomic_compare_exchange_n(a, c, v, 0, SC, SC); } \
    int __tsan_atomic##n##_compare_exchange_weak(volatile T *a, T *c, T v, int mo, int fmo) \
        { (void)mo; (void)fmo; hook(SCHED_OP_ATOMIC, a, RA); \
          return __atomic_compare_exchange_n(a, c, v, 0, SC, SC); } \
    T __tsan_atomic##n##_compare_exchange_val(volatile T *a, T c, T v, int mo, int fmo) \
        { (void)mo; (void)fmo; hook(SCHED_OP_ATOMIC, a, RA); \
          __atomic_compare_exchange_n(a, &c, v, 0, SC, SC); return c; }
ATOMICS(8, int8_t)
ATOMICS(16, int16_t)
ATOMICS(32, int32_t)
ATOMICS(64, int64_t)

/* The race-detector runtime interface: no detector runs in this build. */
int sched_noise(void) { return 0; }
void sched_tsan_release(const void *obj) { (void)obj; }
void sched_tsan_acquire(const void *obj) { (void)obj; }
void sched_tsan_ignore(int on) { (void)on; }
void sched_tsan_poll(void) {}
int sched_tsan_markers(const void *const *code, int n, const char *const *maps, int nmaps)
{
    (void)code; (void)n; (void)maps; (void)nmaps;
    return 0;
}
unsigned long sched_tsan_summary(void *out, unsigned long *unclassified)
{
    (void)out;
    if (unclassified) *unclassified = 0;
    return 0;
}
