/* tsan_noise.c - the scheduler's race-detector runtime over the real
 * ThreadSanitizer runtime (libtsan), for the noise build.
 *
 * The code under test is compiled with -fsanitize=thread and calls libtsan
 * directly. The scheduler runs one thread at a time under a baton; the
 * baton is hidden from the detector (sched_tsan_ignore), so the only
 * happens-before edges the detector sees are the ones the program's own
 * synchronisation makes: the model mutexes, thread start and join, and the
 * SDL atomics publish theirs through sched_tsan_acquire and
 * sched_tsan_release, and C11 atomics in instrumented code reach libtsan
 * themselves. ThreadSanitizer's detector is happens-before based, so two
 * accesses no edge orders are reported whether or not they ran at the same
 * moment; the scheduler's seeded schedules vary which lock-order edges a
 * run makes.
 *
 * Each report is classified in the runtime's report hook: when the
 * innermost frame of one of its accesses lies on a source line carrying
 * `sched: once` or `sched: unordered` (a first-use cache, a statistic no
 * protocol decision reads), it is counted and suppressed; any other report
 * is printed with both stacks and fails the schedule at the next yield.
 * The watch API is inert here: no access is a yield. */
#define _GNU_SOURCE
#include "op_sched_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* libtsan's interface (compiler-rt tsan_interface.h, tsan_debugging.cpp, sanitizer_common). */
void __tsan_acquire(void *addr);
void __tsan_release(void *addr);
void AnnotateIgnoreReadsBegin(const char *file, int line);
void AnnotateIgnoreReadsEnd(const char *file, int line);
void AnnotateIgnoreWritesBegin(const char *file, int line);
void AnnotateIgnoreWritesEnd(const char *file, int line);
void AnnotateIgnoreSyncBegin(const char *file, int line);
void AnnotateIgnoreSyncEnd(const char *file, int line);
void *__tsan_get_current_report(void);
int __tsan_get_report_data(void *report, const char **description, int *count, int *stack_count, int *mop_count,
                           int *loc_count, int *mutex_count, int *thread_count, int *unique_tid_count,
                           void **sleep_trace, uintptr_t trace_size);
int __tsan_get_report_mop(void *report, uintptr_t idx, int *tid, void **addr, int *size, int *write, int *atomic,
                          void **trace, uintptr_t trace_size);
void __sanitizer_symbolize_pc(void *pc, const char *fmt, char *out_buf, uintptr_t out_buf_size);

enum { MAX_MARKS = 256, MAX_SITES = 64, LOC_LEN = 256 };

/** A source line carrying a classifying marker. */
typedef struct mark { char file[LOC_LEN]; int line; char cls[16]; } mark;

/** A classified site: the marked access's location and how many reports it absorbed. */
typedef struct site { char loc[LOC_LEN]; char cls[16]; unsigned long n; } site;

/** Counters and tables shared by every process of a run (MAP_SHARED, made before the first fork). */
typedef struct shared {
    int nmarks;
    mark marks[MAX_MARKS];
    unsigned long reports, classified, unclassified;
    int site_lock;
    int nsites;
    site sites[MAX_SITES];
} shared;

static shared *s_sh;
/* An unclassified report of this process, made inside the runtime's report path and raised at the next yield. */
static volatile int s_pending;
static char s_pending_msg[1024];

int sched_noise(void) { return 1; }

void sched_tsan_release(const void *obj) { __tsan_release((void *)(uintptr_t)obj); }
void sched_tsan_acquire(const void *obj) { __tsan_acquire((void *)(uintptr_t)obj); }

/* The calling thread's open sched_tsan_ignore brackets. */
static __thread int tl_ignore;

void sched_tsan_ignore(int on)
{
    if (on > 0) {
        tl_ignore++;
        AnnotateIgnoreReadsBegin(__FILE__, __LINE__);
        AnnotateIgnoreWritesBegin(__FILE__, __LINE__);
        AnnotateIgnoreSyncBegin(__FILE__, __LINE__);
        return;
    }
    for (int k = on < 0 ? tl_ignore : tl_ignore > 0 ? 1 : 0; k > 0; k--) {
        tl_ignore--;
        AnnotateIgnoreSyncEnd(__FILE__, __LINE__);
        AnnotateIgnoreWritesEnd(__FILE__, __LINE__);
        AnnotateIgnoreReadsEnd(__FILE__, __LINE__);
    }
}

void sched_tsan_poll(void)
{
    if (!s_pending) return;
    s_pending = 0;
    sched_fail("TSAN: %s", s_pending_msg);
}

/** @brief Return the class a `sched:` marker on a source line names, or NULL; only once and unordered classify a race.
 * @param text The line.
 * @return "once", "unordered" or NULL. */
static const char *marker_class(const char *text)
{
    const char *m = strstr(text, "sched:");
    if (!m) return NULL;
    m += 6;
    while (*m == ' ') m++;
    if (!strncmp(m, "once", 4)) return "once";
    if (!strncmp(m, "unordered", 9)) return "unordered";
    return NULL;
}

/** @brief Open a debug-information path, through the first reversed prefix map whose recorded prefix it starts with.
 * @param path The recorded path.
 * @param maps "recorded=directory" entries.
 * @param nmaps Their count.
 * @param real Receives the path opened.
 * @param cap Its capacity.
 * @return The stream, or NULL. */
static FILE *open_recorded(const char *path, const char *const *maps, int nmaps, char *real, size_t cap)
{
    for (int k = 0; k < nmaps; k++) {
        const char *eq = strchr(maps[k], '=');
        if (!eq) continue;
        const size_t pl = (size_t)(eq - maps[k]);
        if (pl == 0 && path[0] != '/') {   /* "=directory": the base of a relative path (the compile's directory) */
            snprintf(real, cap, "%s/%s", eq + 1, path);
            return fopen(real, "r");
        }
        if (pl > 0 && !strncmp(path, maps[k], pl) && path[pl] == '/') {
            snprintf(real, cap, "%s%s", eq + 1, path + pl);
            return fopen(real, "r");
        }
    }
    snprintf(real, cap, "%s", path);
    return fopen(real, "r");
}

int sched_tsan_markers(const void *const *code, int n, const char *const *maps, int nmaps)
{
    if (!s_sh) {
        void *p = mmap(NULL, sizeof(shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return -1;
        s_sh = p;
    }
    for (int i = 0; i < n; i++) {
        char path[LOC_LEN] = "", real[2 * LOC_LEN];
        /* A few bytes into the function: the symbolizer may step back one byte from the address it is given. */
        __sanitizer_symbolize_pc((char *)(uintptr_t)code[i] + 4, "%s", path, sizeof path);
        FILE *f = path[0] ? open_recorded(path, maps, nmaps, real, sizeof real) : NULL;
        if (!f) { fprintf(stderr, "sched: race detector: no source file for code %p (\"%s\")\n", code[i], path); return -1; }
        char text[4096];
        /* Markers are keyed by the recorded path, which is what a report's frames symbolise to. */
        for (int line = 1; fgets(text, sizeof text, f); line++) {
            const char *cls = marker_class(text);
            if (!cls || s_sh->nmarks >= MAX_MARKS) continue;
            mark *mk = &s_sh->marks[s_sh->nmarks++];
            snprintf(mk->file, sizeof mk->file, "%s", path);
            mk->line = line;
            snprintf(mk->cls, sizeof mk->cls, "%s", cls);
        }
        fclose(f);
    }
    return s_sh->nmarks;
}

/** @brief Return the marker covering a symbolised location ("file:line"), matched by the file's path suffix, or NULL.
 * @param loc The location.
 * @return The marker. */
static const mark *mark_at(const char *loc)
{
    const char *colon = strrchr(loc, ':');
    if (!colon || !s_sh) return NULL;
    const int line = atoi(colon + 1);
    const size_t flen = (size_t)(colon - loc);
    for (int i = 0; i < s_sh->nmarks; i++) {
        const mark *mk = &s_sh->marks[i];
        const size_t ml = strlen(mk->file);
        if (mk->line != line) continue;
        /* The same path, or one path a suffix of the other at a '/' (a relative symbolised path). */
        const size_t k = ml < flen ? ml : flen;
        if (!strncmp(mk->file + ml - k, loc + flen - k, k) &&
            (ml == flen || (ml > flen ? mk->file[ml - k - 1] == '/' : loc[flen - k - 1] == '/'))) return mk;
    }
    return NULL;
}

/** @brief Count a classified report against its site, adding the site on first sight.
 * @param loc The marked access's location.
 * @param cls Its class. */
static void site_count(const char *loc, const char *cls)
{
    while (__atomic_exchange_n(&s_sh->site_lock, 1, __ATOMIC_ACQUIRE)) {}
    int k = 0;
    while (k < s_sh->nsites && strcmp(s_sh->sites[k].loc, loc)) k++;
    if (k == s_sh->nsites && k < MAX_SITES) {
        snprintf(s_sh->sites[k].loc, sizeof s_sh->sites[k].loc, "%s", loc);
        snprintf(s_sh->sites[k].cls, sizeof s_sh->sites[k].cls, "%s", cls);
        s_sh->nsites++;
    }
    if (k < s_sh->nsites) s_sh->sites[k].n++;
    __atomic_store_n(&s_sh->site_lock, 0, __ATOMIC_RELEASE);
}

/* ThreadSanitizer's report hook, a weak C++ function in libtsan (__tsan::OnReport):
 * returning true suppresses the report. */
bool sched_tsan_on_report(const void *rep, bool suppressed) __asm__("_ZN6__tsan8OnReportEPKNS_10ReportDescEb");
/** @brief Classify a report by the `sched:` markers on its accesses' lines: count and suppress, or print and raise.
 * @param rep The report (the runtime's current report).
 * @param suppressed The runtime's own suppression verdict.
 * @return true to suppress. */
bool sched_tsan_on_report(const void *rep, bool suppressed)
{
    if (suppressed) return true;
    void *r = rep ? (void *)(uintptr_t)rep : __tsan_get_current_report();
    const char *desc = "?";
    int count = 0, stacks = 0, mops = 0, locs = 0, mutexes = 0, threads = 0, utids = 0;
    void *sleep[1];
    if (r) __tsan_get_report_data(r, &desc, &count, &stacks, &mops, &locs, &mutexes, &threads, &utids, sleep, 1);
    char loc[2][LOC_LEN] = { "?", "?" };
    int wr[2] = { 0, 0 }, tid[2] = { -1, -1 };
    const mark *hit = NULL;
    int hit_mop = -1;
    for (int i = 0; i < mops && i < 2; i++) {
        void *addr = NULL, *trace[1] = { NULL };
        int size = 0, atomic = 0;
        __tsan_get_report_mop(r, (uintptr_t)i, &tid[i], &addr, &size, &wr[i], &atomic, trace, 1);
        if (trace[0]) __sanitizer_symbolize_pc((char *)trace[0] - 1, "%s:%l", loc[i], sizeof loc[i]);
        const mark *mk = mark_at(loc[i]);
        if (mk && !hit) { hit = mk; hit_mop = i; }
    }
    if (s_sh) __atomic_add_fetch(&s_sh->reports, 1, __ATOMIC_RELAXED);
    if (hit && !strcmp(desc, "data-race")) {
        if (s_sh) {
            __atomic_add_fetch(&s_sh->classified, 1, __ATOMIC_RELAXED);
            site_count(loc[hit_mop], hit->cls);
        }
        return true;
    }
    if (s_sh) __atomic_add_fetch(&s_sh->unclassified, 1, __ATOMIC_RELAXED);
    snprintf(s_pending_msg, sizeof s_pending_msg, "%s: %s %s (T%d) vs previous %s %s (T%d), no sched: once|unordered marker",
             desc, wr[0] ? "write" : "read", loc[0], tid[0], wr[1] ? "write" : "read", loc[1], tid[1]);
    s_pending = 1;
    return false;
}

unsigned long sched_tsan_summary(void *out, unsigned long *unclassified)
{
    FILE *f = out;
    if (!s_sh) { if (unclassified) *unclassified = 0; return 0; }
    fprintf(f, "sched: race detector: %lu report(s), %lu classified, %lu unclassified; %d marker(s) loaded\n",
            s_sh->reports, s_sh->classified, s_sh->unclassified, s_sh->nmarks);
    for (int k = 0; k < s_sh->nsites; k++)
        fprintf(f, "  classified %-9s %8lu  %s\n", s_sh->sites[k].cls, s_sh->sites[k].n, s_sh->sites[k].loc);
    if (unclassified) *unclassified = s_sh->unclassified;
    return s_sh->classified;
}

/* The watch API: inert under the real detector (no access is a yield). */
void sched_watch(const void *addr, size_t len, const char *name) { (void)addr; (void)len; (void)name; }
void sched_watch_guarded(const void *addr, size_t len, const char *name, void *const *guard_slot)
{
    (void)addr; (void)len; (void)name; (void)guard_slot;
}
void sched_watch_owned(const void *addr, size_t len, const char *name, int (*owned)(const void *addr, int thread))
{
    (void)addr; (void)len; (void)name; (void)owned;
}
void sched_watch_exclude(const void *addr, size_t len) { (void)addr; (void)len; }
const void *sched_watch_probe(int on) { (void)on; return NULL; }
unsigned long sched_watch_guarded_count(void) { return 0; }
unsigned long sched_watch_counts(unsigned long *hits)
{
    if (hits) *hits = 0;
    return 0;
}

const char *__tsan_default_options(void);
/** @brief The detector's defaults: keep running after a report (the hook raises it), the longest access history, fork-per-schedule allowed.
 * @return The option string. */
const char *__tsan_default_options(void)
{
    return "halt_on_error=0:history_size=7:die_after_fork=0:report_signal_unsafe=0";
}
