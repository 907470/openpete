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

void     openpete_present_thread_report(void);
unsigned openpete_present_thread_presents(void);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_PRESENT_THREAD_H */
