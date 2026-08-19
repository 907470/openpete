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

void openpete_workpool_run(int nitems, openpete_work_fn fn, void* arg) {
    if (nitems <= 0) return;
    wp_spawn();
    int n = openpete_workpool_threads();
    if (n <= 1 || nitems <= 1) {
        for (int i = 0; i < nitems; i++) fn(i, 0, arg);
        return;
    }
    s_job.fn = fn; s_job.arg = arg; s_job.nitems = nitems;
    SDL_SetAtomicInt(&s_job.next, 0);
    for (int w = 1; w < n; w++) SDL_SignalSemaphore(s_start[w]);
    wp_work(0);
    for (int w = 1; w < n; w++) SDL_WaitSemaphore(s_done);
}
