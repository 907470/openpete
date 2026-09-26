/* The present thread: composes and presents the scenes the guest-side
 * producer publishes into the scene ring, on their own target instants.
 * See core/runtime/present_thread.c. */
#ifndef OPENPETE_PRESENT_THREAD_H
#define OPENPETE_PRESENT_THREAD_H

#include "openpete_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OPENPETE_EAGER=1 enables eager production and the present thread.
 * Off by default until the A/B has priced it. */
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

/* Hand the display to the thread (1) or take it back (0). Taking it back
 * drops every pending scene, waits out a compose in flight, and returns only
 * when the thread will issue nothing further: from then on the calling thread
 * owns the device again. Idempotent. */
void openpete_present_thread_set_active(int on);
int  openpete_present_thread_active(void);

/* A scene was published: wake the thread so it re-reads the earliest target. */
void openpete_present_thread_kick(void);

/* Per-present callback on the present thread (frame-time log, profiler). */
typedef void (*openpete_present_thread_cb)(void *ctx, int kind, double alpha, int paired);
void openpete_present_thread_set_callback(openpete_present_thread_cb cb, void *ctx);

/* The display's vblank lattice, v(n) = base + n * period, fitted from
 * observed flips; 1 when locked (vsynced swapchain, present timing on, enough
 * flips seen). The planner puts eager targets on it so a target is a vblank. */
int      openpete_present_thread_lattice(int64_t *base_ns, double *period_ns);

void     openpete_present_thread_report(void);
unsigned openpete_present_thread_presents(void);
int64_t  openpete_present_thread_cost_ns(void);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_PRESENT_THREAD_H */
