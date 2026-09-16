/* terrain_game_none.c — the terrain provider of a game with no portal-sky
 * feeder, no fill-tag list and no art corrections. See renderer/terrain_game.h. */
#include "renderer/terrain_game.h"
#include <stdio.h>

static int   none_recent(void) { return 0; }
static int   none_planes(CPUState* cpu, const ngte_view* v, openpete_portal_skyplane* out, int max) {
    (void)cpu; (void)v; (void)out; (void)max; return 0;
}
static float none_margin(void) { return 256.0f; }
static int   none_fill_tag(int level, int sec, int face) { (void)level; (void)sec; (void)face; return 0; }
static void  none_level_name(int level, char* out, size_t cap) { snprintf(out, cap, "level%d", level); }
static int   none_artfix(int level, int sec, int face, uint32_t* rgb) {
    (void)level; (void)sec; (void)face; (void)rgb; return 0;
}

const terrain_game_t terrain_game = {
    .portal_sky_recent   = none_recent,
    .portal_sky_planes   = none_planes,
    .portal_plane_margin = none_margin,
    .fill_tag            = none_fill_tag,
    .level_name          = none_level_name,
    .lp_artfix           = none_artfix,
};
