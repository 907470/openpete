/* core/runtime/thread_roles.c - the run-time half of thread-role ownership:
 * the per-thread role mask, the boot handoff of `sim`, check-site records,
 * seals and the exit report. See openpete_thread_roles.h.
 *
 * SDL-free and log-free (failures go to stderr, which the log-file mirror
 * captures), so the leaf modules that seal state (openpete_time.c) and their
 * unit tests link it alone. */

#include "openpete_thread_roles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP_ROLE_DEFINE_CAP_(name, bit) const op_role_cap_t op_role_##name = { 0 };
OP_ROLE_LIST(OP_ROLE_DEFINE_CAP_)
#undef OP_ROLE_DEFINE_CAP_
const op_role_cap_t op_role_display = { 0 };

/* The calling thread's roles. Private to this file: a _Thread_local in a
 * header fails under g++ and mingw g++, and mingw TLS is emutls, so every
 * read is a call either way. */
static _Thread_local unsigned t_mask;

/* OPENPETE_THREAD_ROLE_FATAL, read once on the first failure: 1 aborts. */
static OP_ONCE_INT s_fatal = -1;

/* Head of the reached sites, pushed by compare-and-swap on a site's first
 * evaluation and read by the exit report. */
static _Atomic(op_role_site_t*) s_sites;

/* Failures over every site, for op_role_failures. */
static _Atomic unsigned long long s_fail_total;

/* Set once the exit report is registered. */
static atomic_int s_report_registered;

/** @brief Push a site onto the reached list on its first evaluation.
 *
 * @param site The site record. */
static void site_link(op_role_site_t* site) {
    if (atomic_load_explicit(&site->linked, memory_order_relaxed)) return;
    int expect = 0;
    if (!atomic_compare_exchange_strong(&site->linked, &expect, 1)) return;
    op_role_site_t* head = atomic_load(&s_sites);
    do {
        site->next = head;
    } while (!atomic_compare_exchange_weak(&s_sites, &head, site));
}

/** @brief Count one evaluation of a site.
 *
 * @param site The site record. */
static void site_eval(op_role_site_t* site) {
    site_link(site);
    atomic_fetch_add_explicit(&site->evals, 1, memory_order_relaxed);
}

/** @brief Return whether OPENPETE_THREAD_ROLE_FATAL=1, reading the variable on the first call.
 *
 * @return 1 when a failed check aborts the process. */
static int fatal_enabled(void) {
    int v = op_once_int_get(&s_fatal);
    if (v >= 0) return v;
    const char* e = getenv("OPENPETE_THREAD_ROLE_FATAL");
    return op_once_int_fill(&s_fatal, (e && e[0] == '1' && e[1] == '\0') ? 1 : 0);
}

/** @brief Count a failed check, log it on the site's first failure, and abort under OPENPETE_THREAD_ROLE_FATAL=1.
 *
 * @param site The site record.
 * @param what "failed" for a role check; the seal's name for a seal check
 *        (a site with no role bits). */
static void site_fail(op_role_site_t* site, const char* what) {
    const unsigned long long prior =
        atomic_fetch_add_explicit(&site->fails, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_fail_total, 1, memory_order_relaxed);
    const int fatal = fatal_enabled();
    if (prior == 0 || fatal) {
        if (site->bits)
            fprintf(stderr, "[thread-roles] %s %s at %s:%d (%s): role bits 0x%x, thread holds 0x%x%s\n",
                    site->kind, what, site->file, site->line, site->func, site->bits, t_mask,
                    fatal ? "; aborting (OPENPETE_THREAD_ROLE_FATAL=1)" : "");
        else
            fprintf(stderr, "[thread-roles] seal %s written after the seal at %s:%d (%s)%s\n",
                    what, site->file, site->line, site->func,
                    fatal ? "; aborting (OPENPETE_THREAD_ROLE_FATAL=1)" : "");
        fflush(stderr);
    }
    if (fatal) abort();
}

void op_role_bind_bits(unsigned bits, op_role_site_t* site) {
    site_eval(site);
    t_mask |= bits;
}

unsigned op_role_enter_bits(unsigned bits, op_role_site_t* site) {
    site_eval(site);
    const unsigned prior = t_mask;
    t_mask |= bits;
    return prior;
}

void op_role_leave(unsigned prior) { t_mask = prior; }

int op_role_check_at(op_role_site_t* site) {
    site_eval(site);
    if ((t_mask & site->bits) == site->bits) return 1;
    site_fail(site, "failed");
    return 0;
}

unsigned op_role_mask(void) { return t_mask; }

unsigned long long op_role_failures(void) {
    return atomic_load_explicit(&s_fail_total, memory_order_relaxed);
}

/* The per-role bind and assert bodies. The assert attribute on their
 * declarations tells the analysis the role is held after the call; the
 * body is the run-time half. */
#define OP_ROLE_DEFINE_FNS_(name, bit)                                         \
    void op_role__bind_##name(op_role_site_t* site) {                          \
        op_role_bind_bits(OP_ROLE_BIT_##name, site);                           \
    }                                                                          \
    void op_role__assert_##name(op_role_site_t* site) { (void)op_role_check_at(site); } \
    void op_role__assert_shared_##name(op_role_site_t* site) { (void)op_role_check_at(site); }
OP_ROLE_LIST(OP_ROLE_DEFINE_FNS_)
#undef OP_ROLE_DEFINE_FNS_

/** @brief Write every reached site's counts to OPENPETE_THREAD_ROLE_REPORT at exit (atexit handler).
 *
 * One tab-separated row per site: kind, file, line, function, role bits,
 * evaluations, failures. The fence's `join` mode adds the sites no run
 * reached. The variable is read here, once. */
static void report_at_exit(void) {
    const char* path = getenv("OPENPETE_THREAD_ROLE_REPORT");
    if (!path || !*path) return;
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[thread-roles] cannot write the report %s\n", path);
        return;
    }
    fprintf(f, "kind\tfile\tline\tfunction\tbits\tevals\tfails\n");
    for (op_role_site_t* s = atomic_load(&s_sites); s; s = s->next)
        fprintf(f, "%s\t%s\t%d\t%s\t0x%x\t%llu\t%llu\n", s->kind, s->file, s->line, s->func,
                s->bits, (unsigned long long)atomic_load(&s->evals),
                (unsigned long long)atomic_load(&s->fails));
    fclose(f);
}

/* The boot functions set the mask the analysis cannot see them set; their
 * acquire/release attributes describe the effect to callers. */
void op_role_boot_acquire(void) OP_ACQUIRE_CAP(op_role_sim) OP_NO_THREAD_SAFETY_ANALYSIS {
    int expect = 0;
    if (atomic_compare_exchange_strong(&s_report_registered, &expect, 1))
        atexit(report_at_exit);
    t_mask |= OP_ROLE_BIT_sim;
}

/* Static only: main keeps sim in its mask, since exit() and the return from
 * main run the atexit handlers on this thread after openpete_run returns,
 * and each handler asserts sim. */
void op_role_boot_release(void) OP_RELEASE_CAP(op_role_sim) OP_NO_THREAD_SAFETY_ANALYSIS {
}

void op_role_boot_handoff(void) OP_RELEASE_CAP(op_role_sim) OP_NO_THREAD_SAFETY_ANALYSIS {
    t_mask = (t_mask & ~(unsigned)OP_ROLE_BIT_sim) | OP_ROLE_BIT_main;
}

void op_role_boot_reacquire(void) OP_ACQUIRE_CAP(op_role_sim) OP_NO_THREAD_SAFETY_ANALYSIS {
    t_mask |= OP_ROLE_BIT_sim;
}

void op_seal_apply(op_seal_t* seal) {
    atomic_store_explicit(&seal->sealed, 1, memory_order_release);
}

void op_seal_check_at(op_seal_t* seal, op_role_site_t* site) {
    site_eval(site);
    if (atomic_load_explicit(&seal->sealed, memory_order_acquire))
        site_fail(site, seal->name);
}

int op_seal_is_sealed(const op_seal_t* seal) {
    return atomic_load_explicit(&seal->sealed, memory_order_acquire);
}
