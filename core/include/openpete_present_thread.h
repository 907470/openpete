/* Re-presenting the last composed frame while the guest executes.
 * See core/runtime/present_thread.c for what this measures and why. */
#ifndef OPENPETE_PRESENT_THREAD_H
#define OPENPETE_PRESENT_THREAD_H

#include "openpete_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OPENPETE_PRESENT_THREAD=1. Off by default: this is a mechanism test. */
int  openpete_present_thread_enabled(void);
int  openpete_present_thread_start(void);
void openpete_present_thread_stop(void);

/* Open the window immediately before switching to the game fiber: presenting
 * is permitted until the matching close. `first_at` is the next slot on the
 * sim's own render grid, `period` that grid's spacing. */
void openpete_present_thread_open(op_time_t first_at, op_time_t period);

/* Close the window immediately after the fiber returns, waiting out a present
 * still in flight. Returns the nanoseconds the sim spent waiting (0 if none)
 * and, via out_slots, how many grid slots the thread stepped -- including any
 * it stepped over without presenting, which the sim's deadline must also skip
 * or its grid comes back lagging. */
op_time_t openpete_present_thread_close(unsigned *out_slots);

void     openpete_present_thread_report(void);
unsigned openpete_present_thread_presents(void);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_PRESENT_THREAD_H */
