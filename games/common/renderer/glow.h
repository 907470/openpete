/* glow.h : the glow-ring feeder's export. */
#pragma once
#include "psx_runtime_internal.h"

/* Replay the tick's captured glow fans on a sub-tick present, at the depths
 * the canonical tick captured and rebased onto the screen position the staged
 * camera and the record's own motion put them at. */
void glow_reextract(CPUState* cpu, int bracketed, double alpha);
