/* particles.h — the particle feeder's sub-tick entry.
 *
 * The feeder's geometry is the primitives the guest routine wrote, which exist
 * only during the canonical tick's draw, so the sub-tick re-extract replays the
 * set captured then rather than re-deriving it. */
#ifndef OPENPETE_RENDERER_PARTICLES_H
#define OPENPETE_RENDERER_PARTICLES_H

#include "psx_runtime_internal.h"

/** @brief Re-feeds the particle primitives the canonical tick captured, rebased onto the pose this present shows.
 *
 * The primitives are the guest's own output and are gone once the polybuf is
 * reused, so the replay re-feeds the captured set: each record point's screen
 * displacement carries its primitive's corners and its depth. The replay
 * recomputes the rebase only while the pass has a camera pair; across a cut
 * or an unpaired present it skips the rebase and feeds the set with the
 * displacements the last bracketed call since the capture left in place,
 * or none when no bracketed call ran since the capture. At the end of the
 * tick's interval every term of the rebase
 * is zero and the replay reproduces the canonical present exactly. Does
 * nothing until the particle path is ready for re-extract.
 * @param cpu Reads the staged camera, the pool snapshots and the sin/cos table.
 * @param bracketed The pass's camera-pair verdict.
 * @param alpha The pass's phase between the two ticks it presents between,
 *        passed as the pass holds it and read only when `bracketed` is set.
 *        The staged camera was built at this alpha, so the pool positions the
 *        rebase projects under it are phased onto the same instant. */
void particles_reextract(CPUState* cpu, int bracketed, double alpha);

#endif /* OPENPETE_RENDERER_PARTICLES_H */
