/* glow.h : the glow-ring feeder's export. */
#pragma once
#include "psx_runtime_internal.h"

/** @brief Replay the tick's captured glow fans on a sub-tick present.
 *
 * The primitives are the guest's own output and are gone once the polybuf is
 * reused, so the replay re-feeds the set the canonical tick captured, at the
 * depths paired then, rebased onto the screen position the staged camera and
 * the record's own motion put them at. At alpha 1 the rebase is zero and the
 * replay reproduces the canonical present exactly. Does nothing unless
 * hook_reextract_ready(RP_GLOW) holds.
 * @param cpu The guest CPU state, read only.
 * @param bracketed Nonzero when the present lies inside a paired tick
 *        interval; zero skips the rebase and re-feeds at the displacements
 *        last computed.
 * @param alpha The present's position in the tick interval, 1 at its end. */
void glow_reextract(CPUState* cpu, int bracketed, double alpha);
