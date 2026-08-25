// games/spyro-1/features/crash_demo_woah.h — title-screen Crash demo cheat.

#ifndef SPYRO1_CRASH_DEMO_WOAH_H
#define SPYRO1_CRASH_DEMO_WOAH_H

#include "psx_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-tick: restart the title music the cheat branch stopped, once the
 * combo is released. Inert unless the cheat has fired. */
void spyro1_crash_woah_on_tick(CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO1_CRASH_DEMO_WOAH_H */
