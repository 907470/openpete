// core/runtime/work_pool.c — persistent data-parallel worker pool.
//
// See openpete_workpool.h for the contracts. Shape: N-1 persistent SDL
// threads park on a start semaphore; a run publishes the job, wakes them,
// and the calling thread works the same item stream as worker 0. Items are
// pulled from one atomic counter (dynamic load balance — sector costs vary
// by 100x, a static split would leave workers idle behind one heavy chunk).
// The done semaphore both joins the run and publishes the workers' writes
// to the caller.

#include "openpete_workpool.h"

#include <SDL3/SDL.h>

#define WP_MAX_THREADS 8

typedef struct {
    openpete_work_fn fn;
    void*            arg;
    int              nitems;
    SDL_AtomicInt    next;      /* next item to pull */
} wp_job;

static SDL_Semaphore* s_start[WP_MAX_THREADS];  /* one per helper: wake for a run */
static SDL_Semaphore* s_done;                    /* helpers post once per run */
static SDL_Thread*    s_thread[WP_MAX_THREADS];
static wp_job         s_job;
static int            s_threads   = 0;   /* resolved count, 0 = not yet resolved */
static int            s_requested = 0;   /* configure() value, 0 = auto */
static int            s_spawned   = 0;   /* helper threads actually created */

static void wp_work(int worker) {
    for (;;) {
        int item = SDL_AddAtomicInt(&s_job.next, 1);
        if (item >= s_job.nitems) return;
        s_job.fn(item, worker, s_job.arg);
    }
}

static int SDLCALL wp_thread_main(void* p) {
    int worker = (int)(intptr_t)p;          /* 1..threads-1; 0 is the caller */
    for (;;) {
        SDL_WaitSemaphore(s_start[worker]);
        wp_work(worker);
        SDL_SignalSemaphore(s_done);
    }
    return 0;
}

void openpete_workpool_configure(int threads) {
    s_requested = threads;
}

int openpete_workpool_threads(void) {
    if (s_threads > 0) return s_threads;
    int n = s_requested;
    if (n <= 0) {
        n = SDL_GetNumLogicalCPUCores() / 2;
        if (n < 1) n = 1;
    }
    if (n > WP_MAX_THREADS) n = WP_MAX_THREADS;
    s_threads = n;
    return s_threads;
}

static void wp_spawn(void) {
    int n = openpete_workpool_threads();
    if (s_spawned || n <= 1) return;
    s_done = SDL_CreateSemaphore(0);
    for (int w = 1; w < n; w++) {
        s_start[w] = SDL_CreateSemaphore(0);
        char name[32];
        SDL_snprintf(name, sizeof name, "openpete-extract%d", w);
        s_thread[w] = SDL_CreateThread(wp_thread_main, name, (void*)(intptr_t)w);
        if (!s_thread[w]) {                /* thread creation failed: run without it */
            SDL_DestroySemaphore(s_start[w]);
            s_start[w] = NULL;
            s_threads = w;
            break;
        }
    }
    s_spawned = 1;
}

/* OPENPETE_WORKPOOL_SELFTEST=1: what a run costs with NO work in it — the
 * floor every caller pays to fan out at all. Separates the two halves of that
 * floor by sweeping item count at a fixed worker count: the constant part is
 * the semaphore wake + join (n-1 signals, n-1 waits, kernel transitions), and
 * the slope in item count is contention on the single pull counter, which
 * every worker RMWs once per item on one cache line. A caller deciding whether
 * a scene is big enough to fan out needs this number, and it is the half of
 * the decision that travels worst between machines. */
static void wp_noop(int item, int worker, void* arg) {
    (void)item; (void)worker; (void)arg;
}

static void wp_selftest(void) {
    static const int ITEMS[] = { 1, 2, 8, 32, 128, 512 };
    const int reps = 200;
    int n = openpete_workpool_threads();
    SDL_Log("[workpool] selftest: %d worker(s), %d reps, empty items", n, reps);
    for (size_t k = 0; k < sizeof ITEMS / sizeof ITEMS[0]; k++) {
        int items = ITEMS[k];
        double best = 1e30, sum = 0.0;
        Uint64 freq = SDL_GetPerformanceFrequency();
        for (int r = 0; r < reps; r++) {
            Uint64 t0 = SDL_GetPerformanceCounter();
            s_job.fn = wp_noop; s_job.arg = NULL; s_job.nitems = items;
            SDL_SetAtomicInt(&s_job.next, 0);
            for (int w = 1; w < n; w++) SDL_SignalSemaphore(s_start[w]);
            wp_work(0);
            for (int w = 1; w < n; w++) SDL_WaitSemaphore(s_done);
            double us = (double)(SDL_GetPerformanceCounter() - t0) * 1e6 / (double)freq;
            sum += us;
            if (us < best) best = us;
        }
        SDL_Log("[workpool]   items=%-4d mean %7.2f us   best %7.2f us", 
                items, sum / reps, best);
    }
}

void openpete_workpool_run(int nitems, openpete_work_fn fn, void* arg) {
    if (nitems <= 0) return;
    wp_spawn();
    {
        static int st = -1;
        if (st < 0) {
            const char* e = SDL_getenv("OPENPETE_WORKPOOL_SELFTEST");
            st = (e && *e && *e != '0');
            if (st && openpete_workpool_threads() > 1) wp_selftest();
        }
    }
    int n = openpete_workpool_threads();
    if (n <= 1 || nitems <= 1) {
        for (int i = 0; i < nitems; i++) fn(i, 0, arg);
        return;
    }
    /* Wake at most one helper per item beyond the caller's own. The join has
     * to wait for every helper it signalled to come back, and a helper with no
     * item left still costs its full wake latency before it can post done — so
     * signalling more helpers than there is work for buys nothing and sets the
     * run's floor at the slowest of them. A 2-item job used to wake seven
     * threads to do one item between them. */
    int wake = nitems < n ? nitems : n;
    s_job.fn = fn; s_job.arg = arg; s_job.nitems = nitems;
    SDL_SetAtomicInt(&s_job.next, 0);
    for (int w = 1; w < wake; w++) SDL_SignalSemaphore(s_start[w]);
    wp_work(0);
    for (int w = 1; w < wake; w++) SDL_WaitSemaphore(s_done);
}
