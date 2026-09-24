/* tracers.h : the tracer feeder's export. */
#pragma once
#include <stdint.h>
#include "psx_runtime_internal.h"

/* Replay the tick's captured ribbon and mesh primitives on a sub-tick present,
 * at the depths the canonical tick captured and rebased onto the screen
 * positions the staged camera and the records' own motion put their points at. */
void tracers_reextract(CPUState* cpu, int bracketed, double alpha);

/* The mesh vertex lists the most recent frame with a mesh draw read, as PSX
 * address and vertex count, up to `max`. The per-object capture keys each
 * vertex by its address, so the sub-tick replay has a previous position to
 * rebase every mesh corner onto. */
int tracers_mesh_lists(uint32_t* addr, uint32_t* nverts, int max);
