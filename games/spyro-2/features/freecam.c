// games/spyro-2/features/freecam.c — Spyro 2 descriptor for the debug free
// camera (F). The drive, path and teleport bodies are shared
// (games/common/freecam_game.c); this file holds the addresses.

#include "freecam_game.h"
#include "openpete_freecam.h"         /* OPENPETE_FREECAM_SPEED_STEPS */

#include <stddef.h>
#include <stdint.h>

/* The camera struct @ 0x80067E84 has Spyro 3's layout: position at +0x28
 * (D_80067EAC, three int32, Z up), then the three 12-bit angle shorts at
 * +0x44 (roll), +0x46 (pitch) and +0x48 (yaw), which func_8001E270 sets to
 * 0, (elevation + yaw) & 0xFFF and (azimuth + 0x800 + pitch) & 0xFFF from
 * the polar follow position (D_80067EC8/CA/CC). */
#define G_CAMERA     0x80067E84u

/* The player struct @ 0x80069FF0 has Spyro 3's layout: position at +0x00.
 * No previous-position field or momentum-reset routine is identified, so a
 * teleport writes the position alone. */
#define G_SPYRO      0x80069FF0u

/* Spyro 1's tables: the same world scale. */
static const int s_move_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 25, 100, 300, 500, 800 };
static const int s_turn_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 10,  25,  35,  50,  75 };

const freecam_game_desc freecam_desc = {
    .cam_pos_x = G_CAMERA + 0x28u,
    .cam_pos_y = G_CAMERA + 0x2Cu,
    .cam_pos_z = G_CAMERA + 0x30u,
    .cam_yaw   = G_CAMERA + 0x48u,
    .cam_pitch = G_CAMERA + 0x46u,
    .angle_bits = 12,
    .yaw_offset = 0, .yaw_sign = 1, .pitch_sign = 1,
    .pitch_limit = 0x01C0,
    .freeze_only_when_live = 1,
    /* func_8001D87C: the camera update (func_8001D8A4, whose angle store is
     * func_8001E270, then func_80021DC0), called from the update routines.
     * func_8002A07C: the player update dispatcher (13 unconditional calls,
     * Spyro 3's func_8003E83C shape). */
    .cam_follow    = 0x8001D87Cu,
    .player_update = 0x8002A07Cu,
    .extra_freeze  = NULL,
    .player_pos      = G_SPYRO + 0x00u,
    .player_prev_pos = 0u,
    .level_live         = NULL,   /* game_is_gameplay_live */
    .draw_all           = NULL,
    .reset_momentum     = NULL,
    .resolve_named_pose = NULL,
    .move_speed = s_move_speed,
    .turn_speed = s_turn_speed,
};
