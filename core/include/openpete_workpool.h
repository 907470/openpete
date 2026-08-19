// core/include/openpete_workpool.h — data-parallel extraction worker pool.
//
// A persistent pool of worker threads for fan-out over independent items
// (terrain sectors, compose buckets). Contracts (extraction-rearchitecture
// plan, C1-C5):
//  - Workers run ONLY inside openpete_workpool_run(); between runs they are
//    parked. They must never touch the game fiber or any thread-affine state.
//  - Callers may only hand work that reads guest RAM while the game fiber is
//    parked (the extraction window) and writes exclusively to per-item or
//    per-worker state.
//  - openpete_workpool_run() is itself single-threaded: it is called from the
//    host-loop thread only, and returns only after every item completed, so
//    the caller resumes with all results visible (semaphores order the
//    memory).
//  - Item scheduling is dynamic (atomic pull); callers needing deterministic
//    OUTPUT order must tag results by item index and merge in item order.

#ifndef OPENPETE_WORKPOOL_H
#define OPENPETE_WORKPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*openpete_work_fn)(int item, int worker, void* arg);

/* Configure the pool size before its first use (boot-latched; from
 * --extract-threads). 0 = auto (logical cores / 2, clamped to [1, 8]);
 * 1 = serial (no threads created, run() degrades to a plain loop). */
void openpete_workpool_configure(int threads);

/* Resolved worker count (>= 1). Worker ids passed to fn are 0..count-1;
 * id 0 is the calling thread (it participates in the run). */
int openpete_workpool_threads(void);

/* Run fn(item, worker, arg) for every item in [0, nitems). Returns after all
 * items completed. Serial (in caller order) when the pool is size 1 or
 * nitems <= 1. */
void openpete_workpool_run(int nitems, openpete_work_fn fn, void* arg);

#ifdef __cplusplus
}
#endif

#endif
