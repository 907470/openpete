// games/common/game_extras_overlay_resync.c — the boot and savestate-restore
// entries for a game whose resident overlay is identified by the id word at
// the head of the overlay region rather than by the disc read that fetched it.
//
// A game's CMakeLists lists this file in place of defining the two entries in
// its own games/<game>/game_extras_impl.c; a game whose boot or restore needs
// more than this defines them there instead and does not list the file.

#include "game_extras.h"
#include "feature_registry.h"
#include "spyro_overlay_table.h"
#include "psyq/psyq_game_hooks.h"
#include "write_census.h"

/** @brief Return the resident overlay's id, re-derived from the id word at the head of the overlay region.
 *
 * It is the feature registry's overlay probe: a feature hook on an address
 * inside the overlay window is qualified by this value. A tracker updated
 * only on a load event would be stale here, since an overlay staged into a
 * scratch buffer and copied into place arrives with no disc read naming it.
 * @return The overlay id, or 0 while no registered overlay id has been read. */
static uint32_t resident_overlay_id(void) {
    psyq_overlay_resync_from_ram();
    return spyro_get_active_overlay_magic();
}

void game_on_boot(CPUState* cpu) {
    /* The atlas and the cutscene overlays are staged into a scratch buffer and
     * copied into the overlay region, so the disc read that fetched them never
     * named them; the dispatcher re-derives the resident one from the id word
     * instead. */
    spyro_overlay_set_resync(psyq_overlay_resync_from_ram);
    feature_registry_set_overlay_probe(resident_overlay_id);
    /* The write census installs over the store hooks before any guest
     * instruction runs; inert without OPENPETE_WRITE_CENSUS. */
    write_census_install(cpu);
}

/* The id of the resident overlay is the first word of the overlay region. */
void game_on_state_loaded(CPUState* cpu) {
    psyq_overlay_resync_from_ram();
    (void)cpu;
}
