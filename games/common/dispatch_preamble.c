// games/common/dispatch_preamble.c - the shared front of every game's
// override_dispatch (the runtime descriptor's), and the resident-overlay probe.

#include "dispatch_preamble.h"
#include "freecam_game.h"
#include "openpete_mods.h"
#include "openpete_modsave.h"
#include "psyq/psyq_game_hooks.h"
#include "renderer/player_hide.h"
#include "spyro_overlay_table.h"
#include "write_census.h"

int game_dispatch_preamble(CPUState* cpu, uint32_t addr,
                           game_dispatch_verdict_fn verdict) {
    /* Debug free camera (F): while engaged it drives the camera in place of
     * the game's camera follow and suppresses it and the player update.
     * Inactive: one bool test. */
    if (freecam_should_freeze(cpu, addr)) return 1;
    /* --hide-player: the owner of the running call subtree, which the
     * particle allocator hook tags each slot with. Off: one call reading
     * two flags. */
    if (player_hide_tracking()) player_hide_note_entry(cpu, addr);
    if (verdict) verdict(cpu, addr);
    if (write_census_active() && !openpete_mods_bypass_pending(addr))
        write_census_note_entry(addr);
    /* Per-mod save records: the new-playthrough reset, load-binding and
     * delivery observers, above the mods arm so they act at a function's
     * first entry before any override. One flag test while unarmed. */
    if (openpete_modsave_armed) openpete_modsave_dispatch(cpu, addr);
    /* One global-flag branch when no mods are loaded. Not trace-scoped:
     * this arm runs on every translated-function entry. */
    if (openpete_mods_active && openpete_mods_dispatch(cpu, addr)) return 1;
    return 0;
}

uint32_t game_resident_overlay_probe(void) {
    psyq_overlay_resync_from_ram();
    return spyro_get_active_overlay_magic();
}
