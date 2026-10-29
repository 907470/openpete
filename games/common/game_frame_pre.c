// games/common/game_frame_pre.c - the shared per-tick pre-hook; see
// game_frame_pre.h.

#include "game_frame_pre.h"

#include "game_host_desc.h"

#include "freecam_game.h"
#include "features/state_watch.h"
#include "write_census.h"
#include "renderer/text_moby.h"

void game_frame_pre_on_tick(struct CPUState* cpu, uint32_t frame_counter) {
    static const game_frame_pre_t k_none = { 0 };
    const game_host_desc_t* host = openpete_game_runtime()->host;
    const game_frame_pre_t* g = host && host->frame_pre ? host->frame_pre : &k_none;
    if (g->lead) g->lead(cpu, frame_counter);
    if (g->skip_on_tick) g->skip_on_tick(cpu, frame_counter);
    if (g->levelwarp_on_tick) g->levelwarp_on_tick(cpu);
    if (g->trail) g->trail(cpu, frame_counter);
    state_watch_on_tick(cpu, frame_counter);
    /* Scripted player placement (--player-pose) on the first placeable
     * tick, then scripted engage of the debug free camera (OPENPETE_FREECAM=1
     * / OPENPETE_FREECAM_POSE / OPENPETE_FREECAM_PATH) once gameplay is live;
     * placement first so one run can view the placed player. A game holds
     * the engage while a transition it has queued would re-seed the camera
     * and discard the pose. */
    freecam_player_pose_maybe_apply(cpu);
    if (!g->freecam_engage_held || !g->freecam_engage_held())
        freecam_maybe_autoengage(cpu);
    /* Write census: every fiber resume in a tick happens inside the draw's
     * VSync waits, after the view matrix has been built, so a store seen from
     * here until the next phase entry precedes it for that tick. */
    if (write_census_active()) write_census_set_phase(0);
    text_moby_native_layers_tick(cpu);
}
