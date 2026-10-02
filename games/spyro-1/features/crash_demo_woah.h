// games/spyro-1/features/crash_demo_woah.h - title-screen Crash demo cheat.

#ifndef SPYRO1_CRASH_DEMO_WOAH_H
#define SPYRO1_CRASH_DEMO_WOAH_H

#include "psx_runtime_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Runs the Crash demo cheat's per-tick housekeeping while the title screen overlay is resident.
 *
 * Starts the background decode of the woah on the first such tick, clears
 * the once-per-press latch when L1+Triangle is no longer held, and disarms a
 * pending one-shot substitution whose key-on has not arrived within
 * ARM_EXPIRY_TICKS ticks. Does nothing away from the title screen.
 * @param cpu The guest CPU state. */
void spyro1_crash_woah_on_tick(CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO1_CRASH_DEMO_WOAH_H */
