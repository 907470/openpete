// games/common/game_host_desc.h - the per-game tables the shared host TUs
// consult, carried by the runtime descriptor (openpete_game_runtime.h).
//
// The shared feeders and the free camera read one global per table (ui_game,
// terrain_game, moby_game, player_game, the three Spyro 2/3 constant blocks,
// freecam_desc) and call the orchestrator entries of renderer/orchestrator.h.
// Each game defines its tables under game-unique names and points its
// descriptor's host field at one game_host_desc_t; game_runtime_install
// copies the tables into those globals and binds the orchestrator, so the
// shared code reads the running game's tables without naming the game.

#ifndef OPENPETE_GAME_HOST_DESC_H
#define OPENPETE_GAME_HOST_DESC_H

#include "openpete_game_runtime.h"

#include "freecam_game.h"
#include "game_frame_pre.h"
#include "renderer/moby_game.h"
#include "renderer/moby_game_spyro23.h"
#include "renderer/orchestrator.h"
#include "renderer/player_game.h"
#include "renderer/player_game_spyro23.h"
#include "renderer/portal_spyro23.h"
#include "renderer/terrain_game.h"
#include "renderer/ui_game.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A NULL table binds the empty one: every slot 0, and for the terrain table
 * the inert provider (no portal sky, no fill tags, no art fixes). */
typedef struct game_host_desc {
    const ui_game_t*                ui;
    const terrain_game_t*           terrain;
    const moby_game_t*              moby;
    const player_game_t*            player;
    const moby_game_s23_consts_t*   moby_s23;
    const player_game_s23_consts_t* player_s23;
    const portal_game_s23_consts_t* portal_s23;
    const freecam_game_desc*        freecam;
    const game_orchestrator_t*      orchestrator;  /* NULL: no sub-tick spine */
    void (*interp_register)(void);                 /* NULL: no interp regions */
    const game_frame_pre_t*         frame_pre;     /* game_frame_pre_on_tick's steps; NULL: none */
} game_host_desc_t;

/** @brief Register a game's runtime descriptor with core and bind its host tables into the shared globals.
 *
 * The entry point calls it first, before any other code reads a game table.
 * @param rt The game's descriptor; it must outlive the process. */
void game_runtime_install(const openpete_game_runtime_t* rt);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_HOST_DESC_H */
