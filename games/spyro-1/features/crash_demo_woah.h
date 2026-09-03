// games/spyro-1/features/crash_demo_woah.h — title-screen Crash demo cheat.

#ifndef SPYRO1_CRASH_DEMO_WOAH_H
#define SPYRO1_CRASH_DEMO_WOAH_H

#include "psx_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-tick: start the background decode of the woah once the title
 * screen is resident, and reset the once-per-press latches when the
 * combo comes up. Inert away from the title screen. */
void spyro1_crash_woah_on_tick(CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO1_CRASH_DEMO_WOAH_H */
