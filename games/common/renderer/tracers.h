/* tracers.h : the tracer feeder's export. */
#pragma once
#include "psx_runtime_internal.h"

/* Replay the tick's captured ribbon and mesh primitives on a sub-tick present,
 * at the screen positions and depths the canonical tick captured. */
void tracers_reextract(CPUState* cpu);
