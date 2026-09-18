/* games/spyro-2/renderer/interp_regions.c — Spyro 2's render-side
 * interpolation region table, captured per tick into the openpete_interp ring
 * so intermediate presents can lerp the camera transform.
 *
 * The rows are registered through the shared generator
 * (renderer/interp_regions_gen.h), which assigns each one its ring region
 * index. Region 0 is the camera's draw-time transform [cam.base,
 * cam.xform_end): the sky matrix at +0x00, the world matrix at +0x14, the
 * position at +0x28 and the Draw-time position copy at +0x30, all rewritten
 * once per tick before the draws read them.
 *
 * The terrain feeder is the only one this game reaches, and it re-derives its
 * geometry from the camera alone, so the camera is the whole plain-global
 * table. The avatar record, the game-state word and any captured pool belong
 * to the feeders that read them and are added with those feeders.
 *
 * The per-object capture is the shared moby-array walk
 * (game_interp_capture_moby_array), installed by the same registration call;
 * it walks nothing until game_layout.moby carries an array head. The
 * scrolling-texture descriptors the same call captures are located
 * (game_layout.env_anim.scroll), so the UV-scroll mirror has its pairs. */
#include "renderer/interp_regions_gen.h"
#include "renderer/orchestrator.h"
#include "game_layout.h"
#include <stddef.h>

void game_interp_register(void) {
    /* The camera's span is the draw-time transform [cam.base, cam.xform_end),
     * computed from the layout descriptor rather than stated. */
    game_interp_register_common(NULL, 0u,
                                game_layout.cam.xform_end - game_layout.cam.base,
                                NULL);
}
