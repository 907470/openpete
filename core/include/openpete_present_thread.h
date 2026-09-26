/* The present thread: composes and presents the scenes the guest-side
 * producer publishes into the scene ring, on their own target instants.
 * See core/runtime/present_thread.c. */
#ifndef OPENPETE_PRESENT_THREAD_H
#define OPENPETE_PRESENT_THREAD_H

#include "openpete_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Report whether eager production and the present thread are enabled.
 *
 * On by default; OPENPETE_EAGER set to a value starting with '0' disables
 * it, which returns presents to the sim thread and removes the scene ring,
 * the lag and the present thread together. Read once per process.
 * @return 1 when enabled, else 0. */
int  openpete_present_thread_enabled(void);
/** @brief Return 1 when OPENPETE_EAGER_LATTICE=1 makes the present thread target and compose on the display's fitted vblank lattice.
 *
 * The lattice implies present timing. Off by default: the thread then
 * composes each scene at its own target and observes flips only under
 * OPENPETE_PACE_FLIP_LOG. The variable is read once per run.
 * @return 1 when the lattice is enabled, else 0. */
int  openpete_present_thread_lattice_enabled(void);
int  openpete_present_thread_start(void);
void openpete_present_thread_stop(void);

/** @brief Hand the display to the present thread (1) or take it back (0).
 *
 * Handing over wakes the thread, which claims the display before its first
 * present. Taking it back flushes the scene ring, waits out a compose or
 * flip wait in flight, discards the staged scene, and claims the display
 * for the caller, which then releases the stage's acquired swapchain image
 * by presenting it once more; on return the present thread issues nothing
 * further. Idempotent; a no-op before the thread has started.
 * @param on 1 to hand the display to the thread, 0 to take it back. */
void openpete_present_thread_set_active(int on);
int  openpete_present_thread_active(void);

/** @brief Wake the present thread after a scene was published so it re-reads the earliest target.
 *
 * Signals the thread's condition variable under its mutex; a no-op before
 * the thread has started. */
void openpete_present_thread_kick(void);

/* Per-present callback on the present thread (frame-time log, profiler). */
typedef void (*openpete_present_thread_cb)(void *ctx, int kind, double alpha, int paired);
void openpete_present_thread_set_callback(openpete_present_thread_cb cb, void *ctx);

/** @brief Read the display's vblank lattice v(n) = base + n * period fitted from observed flips.
 *
 * The fit locks once the window holds 48 flips and the last 24 fell within
 * an eighth of a slot of it; that needs a vsynced swapchain, present timing
 * and OPENPETE_EAGER_LATTICE. The eager planner puts its targets on the
 * lattice so each target is a vblank. The outputs are written only when
 * locked.
 * @param base_ns Receives the time of lattice instant 0 on the op_now()
 *        clock; may be NULL.
 * @param period_ns Receives the fitted period; may be NULL.
 * @return 1 when the lattice is locked, 0 when not or before the thread
 *         has started. */
int      openpete_present_thread_lattice(int64_t *base_ns, double *period_ns);

void     openpete_present_thread_report(void);
unsigned openpete_present_thread_presents(void);
int64_t  openpete_present_thread_cost_ns(void);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_PRESENT_THREAD_H */
