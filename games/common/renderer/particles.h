/* particles.h — the particle feeder's sub-tick entry.
 *
 * The feeder's geometry is the primitives the guest routine wrote, which exist
 * only during the canonical tick's draw, so the sub-tick re-extract replays the
 * set captured then rather than re-deriving it. */
#ifndef OPENPETE_RENDERER_PARTICLES_H
#define OPENPETE_RENDERER_PARTICLES_H

#include "psx_runtime_internal.h"

void particles_reextract(CPUState* cpu);

#endif /* OPENPETE_RENDERER_PARTICLES_H */
