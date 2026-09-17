// games/spyro-2/renderer/layout.c — the Spyro 2 engine layout. Only the
// camera pose and the player's position are located; every other field is 0,
// so each feeder that reads one refuses to run rather than read through
// address 0 (core/include/game_layout.h).

#include "game_layout.h"
#include "renderer/ui_game.h"

/* The camera struct @ 0x80067E84 has Spyro 3's layout: position at +0x28
 * (D_80067EAC, three int32, Z up), then the three 12-bit angle shorts at
 * +0x44 (roll), +0x46 (pitch) and +0x48 (yaw), which func_8001E270 sets to
 * 0, (elevation + yaw) & 0xFFF and (azimuth + 0x800 + pitch) & 0xFFF from
 * the polar follow position (D_80067EC8/CA/CC). */
#define G_CAMERA 0x80067E84u

/* The player state block @ 0x80069FF0 opens with the world position, three
 * int32. Only `base` and `pos_off` are located: the player feeder needs the
 * part records and the draw hook as well, so it still refuses to run, and the
 * position probe (game_get_player_position) reads these two alone. */
#define G_SPYRO 0x80069FF0u

const game_layout_t game_layout = {
    .cam = {
        .base     = G_CAMERA,
        .position = G_CAMERA + 0x28u,
        .rotation = G_CAMERA + 0x44u,
    },
    .player = {
        .base    = G_SPYRO,
        .pos_off = 0x00u,
    },
};

/* No 2D UI feeder: the ordering-table chokepoint and the fade emitter are not
 * located, so games/common/renderer/ui_sprite.c registers no hook. */
const ui_game_t ui_game = { 0 };
