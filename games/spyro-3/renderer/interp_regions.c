/* games/spyro-3/renderer/interp_regions.c — Spyro 3's render-side
 * interpolation region table, captured per tick into the openpete_interp ring
 * so intermediate presents can lerp the camera transform.
 *
 * Region 0 is the camera's draw-time transform [cam.base, cam.xform_end): the
 * sky matrix at +0x00, the world matrix at +0x14 and the position at +0x28,
 * all rewritten once per tick before the draws read them. The per-object
 * capture is empty until the moby walk runs on this game. */
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "renderer/orchestrator.h"
#include "game_layout.h"

static openpete_interp_region_t s_regions[1];

static void capture_objs_none(void) {}

void game_interp_register(void) {
    /* ra_commit = 1: the camera presents from the committed timeline under
     * runahead, so a misprediction never shows as a pan that un-shows. */
    s_regions[0].name     = "camera";
    s_regions[0].psx_addr = game_layout.cam.base;
    s_regions[0].length   = game_layout.cam.xform_end - game_layout.cam.base;
    s_regions[0].ra_commit = 1;
    openpete_interp_register_regions(s_regions, 1);
    openpete_interp_register_obj_capture(capture_objs_none);
}
