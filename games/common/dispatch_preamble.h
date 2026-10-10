// games/common/dispatch_preamble.h - the shared front of every game's
// override_dispatch (the runtime descriptor's), and the resident-overlay probe.

#ifndef OPENPETE_DISPATCH_PREAMBLE_H
#define OPENPETE_DISPATCH_PREAMBLE_H

#include <stdint.h>
#include "psx_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A game's verdict hook: reads guest state at a function entry and logs;
 * returns nothing, since it never ends the call. */
typedef void (*game_dispatch_verdict_fn)(CPUState* cpu, uint32_t addr);

/** @brief Run the intercepts every game's dispatch performs first, in one fixed order.
 *
 * In order: the debug free camera's freeze test (ends the call), the
 * game's verdict hook, the write census's entry note, then the mod
 * override chains behind openpete_mods_active (end the call when a chain
 * ran). A mod override therefore cannot skip the verdict hook or the
 * census note. base() re-enters through the same preamble; the census note
 * is skipped on that re-entry (openpete_mods_bypass_pending), and the
 * verdict hooks are edge-triggered, so their second run changes nothing.
 * The game's own arms and the feature registry follow in the caller.
 * @param cpu The guest CPU state.
 * @param addr The entered function's guest vaddr.
 * @param verdict The game's verdict hook, or NULL.
 * @return 1 when the recompiled body must return at once, else 0. */
int game_dispatch_preamble(CPUState* cpu, uint32_t addr,
                           game_dispatch_verdict_fn verdict);

/** @brief Return the resident overlay's id, re-derived from the id word at the head of the overlay region.
 *
 * The feature registry's overlay probe, which the mods dispatcher also
 * reads: a feature hook or a mod override inside the overlay window is
 * qualified by this value. A tracker updated only on a load event would be
 * stale here, since an overlay staged into a scratch buffer and copied
 * into place arrives with no disc read naming it.
 * @return The overlay id, or 0 while no registered overlay id has been
 *         read. */
uint32_t game_resident_overlay_probe(void);

#ifdef __cplusplus
}
#endif

#endif
