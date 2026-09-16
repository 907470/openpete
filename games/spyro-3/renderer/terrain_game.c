/* games/spyro-3/renderer/terrain_game.c : Spyro 3's data for the shared
 * terrain feeder: the portal feeder's opening planes and the level names.
 * See renderer/terrain_game.h. */
#include "renderer/terrain_game.h"
#include "renderer/portal.h"         /* the opening-plane provider */
#include "renderer/portal_plane.h"   /* the shared plane-cover margin */
#include "skip_to_level.h"           /* level id -> name */
#include <stdio.h>

static void s3_level_name(int level, char* out, size_t cap) {
    size_t nlv = 0;
    const spyro3_level_t* lv = spyro3_level_table(&nlv);
    snprintf(out, cap, "level%d", level);
    for (size_t k = 0; k < nlv; k++)
        if (lv[k].number == level) { snprintf(out, cap, "%s", lv[k].name); break; }
}

/* No frozen LP fill-face list and no authored-data colour corrections: both
 * are per-face verdicts produced by a reviewed sweep, and no sweep has run on
 * this game's levels. Absence means "not tagged", the same answer a face the
 * sweep never measured gets on Spyro 1. */
static int s3_fill_tag(int level, int sec, int face) {
    (void)level; (void)sec; (void)face; return 0;
}
static int s3_lp_artfix(int level, int sec, int face, uint32_t* rgb) {
    (void)level; (void)sec; (void)face; (void)rgb; return 0;
}

const terrain_game_t terrain_game = {
    .portal_sky_recent   = portal3_sky_recent,
    .portal_sky_planes   = portal3_sky_planes,
    .portal_plane_margin = openpete_portal_plane_margin,
    .fill_tag            = s3_fill_tag,
    .level_name          = s3_level_name,
    .lp_artfix           = s3_lp_artfix,
};
