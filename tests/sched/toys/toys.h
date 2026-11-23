/* toys.h - calibration programs for the scheduler (sched_calibrate.c). */
#ifndef OPENPETE_SCHED_TOYS_H
#define OPENPETE_SCHED_TOYS_H

/** Counter whose address the scheduler does not watch. */
extern int g_toy_counter_unseamed;
/** Counter whose address the scheduler watches. */
extern int g_toy_counter_seamed;

/** @brief Increment a counter as a separate load and store, both instrumented.
 * @param c The counter. */
void toy_counter_inc(int *c);

#endif
