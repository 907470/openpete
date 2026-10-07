/* tracers.h : the tracer feeder's export. */
#pragma once
#include <stdint.h>
#include "psx_runtime_internal.h"

/** @brief Re-feeds the tick's captured ribbon and mesh primitives on a sub-tick present, rebased onto the pose it shows.
 *
 * The primitives are the guest's own output and are gone once the polybuf
 * is reused, so the replay re-feeds the held set at the depths the
 * canonical tick paired, each corner moved by the screen displacement of
 * its source point between the tick's camera and records and the staged
 * camera and interpolated records. At alpha 1, and on a present outside a
 * bracket, it emits the captured corners verbatim and reproduces the
 * canonical present. A held camera cut shows tick N-1's set
 * (cut_held_sets_enter) verbatim, since the staged camera is then tick
 * N-1's. A no-op until hook_reextract_ready_held(RP_TRACER, ...).
 * @param cpu The guest CPU state.
 * @param bracketed Nonzero on a present inside a reextract bracket.
 * @param alpha The present's position in the tick interval, 0..1.
 * @param hold Nonzero on a held camera cut. */
void tracers_reextract(CPUState* cpu, int bracketed, double alpha, int hold);

/** @brief Copies out the mesh vertex lists the most recent VBL with a mesh draw read.
 *
 * The per-object capture keys each vertex by its address, so the sub-tick
 * replay has a previous position to rebase every mesh corner onto.
 * @param addr Receives each list's guest address.
 * @param nverts Receives each list's vertex count.
 * @param max The capacity of addr and nverts.
 * @return The count of lists written. */
int tracers_mesh_lists(uint32_t* addr, uint32_t* nverts, int max);
