/* particles.h — the particle feeder's sub-tick entry.
 *
 * The feeder's geometry is the primitives the guest routine wrote, which exist
 * only during the canonical tick's draw, so the sub-tick re-extract replays the
 * set captured then rather than re-deriving it. */
#ifndef OPENPETE_RENDERER_PARTICLES_H
#define OPENPETE_RENDERER_PARTICLES_H

#include "psx_runtime_internal.h"

/* `bracketed` is the pass's camera-pair verdict and `alpha` its phase between
 * the two ticks it presents between: the replay rebases the captured
 * primitives onto that pose only while the pass has a pair, and replays them
 * unchanged across a cut or an unpaired present, where the rest of the scene
 * snaps to the tick as well. `alpha` is passed as the pass holds it and is
 * read only under that verdict, so a caller never conditions it. The pass's
 * own alpha is what the staged camera was built at, so the pool positions the
 * rebase projects under it are phased onto the same instant. */
void particles_reextract(CPUState* cpu, int bracketed, double alpha);

#endif /* OPENPETE_RENDERER_PARTICLES_H */
