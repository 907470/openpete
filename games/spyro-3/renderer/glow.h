/* glow.h : the Spyro 3 glow-ring feeder's export. */
#pragma once
#include "psx_runtime_internal.h"

/* Replay the tick's captured glow fans on a sub-tick present, at the screen
 * positions and depths the canonical tick captured. */
void glow_reextract(CPUState* cpu);
