// games/common/game_extras_overlay_resync.c - the boot and savestate-restore
// entries for a game whose resident overlay is identified by the id word at
// the head of the overlay region rather than by the disc read that fetched it.
//
// A game's CMakeLists lists this file and its runtime descriptor names the two
// hooks for on_boot and on_state_loaded; a game whose boot or restore needs
// more than this defines its own hooks in games/<game>/game_extras_impl.c and
// does not list the file.

#include "game_extras.h"
#include "dispatch_preamble.h"
#include "feature_registry.h"
#include "spyro_overlay_table.h"
#include "psyq/psyq_game_hooks.h"
#include "write_census.h"

/** @brief Install the resident-overlay resync and probe and the write census before any guest instruction runs.
 *
 * @param cpu Guest CPU state. */
void game_overlay_resync_on_boot(CPUState* cpu) {
    /* The atlas and the cutscene overlays are staged into a scratch buffer and
     * copied into the overlay region, so the disc read that fetched them never
     * named them; the dispatcher re-derives the resident one from the id word
     * instead. */
    spyro_overlay_set_resync(psyq_overlay_resync_from_ram);
    feature_registry_set_overlay_probe(game_resident_overlay_probe);
    /* The write census installs over the store hooks before any guest
     * instruction runs; inert without OPENPETE_WRITE_CENSUS. */
    write_census_install(cpu);
}

/** @brief Re-point the active-overlay dispatcher after a savestate restore at the overlay whose id word heads the restored overlay region.
 *
 * @param cpu Guest CPU state. */
void game_overlay_resync_on_state_loaded(CPUState* cpu) {
    psyq_overlay_resync_from_ram();
    (void)cpu;
}
