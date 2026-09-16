// games/spyro-2/features/freecam.c — Spyro 2 descriptor for the debug free
// camera (F). The drive, path and teleport bodies are shared
// (games/common/freecam_game.c); this file holds the addresses.

#include "freecam_game.h"
#include "game_extras.h"            /* game_is_gameplay_live */
#include "openpete_freecam.h"         /* OPENPETE_FREECAM_SPEED_STEPS */
#include "psx_runtime_internal.h"     /* CPUState + its read callbacks */

#include <stddef.h>
#include <stdint.h>

/* The camera pose is read through game_layout.cam (renderer/layout.c). */

/* The player struct @ 0x80069FF0 has Spyro 3's layout: position at +0x00.
 * No previous-position field or momentum-reset routine is identified, so a
 * teleport writes the position alone. */
#define G_SPYRO      0x80069FF0u
#define ADDR_DEMO_MODE  0x80067024u  /* g_DemoMode (skip_to_level.c): 1 while the attract demo drives the pad */
#define ADDR_LEVEL_ID   0x80066F90u  /* g_NextLevelNum: the level the loader loaded (10..66) */

/* Placeable while gameplay is live and the pad drives the player. No
 * fly-in camera state is identified for this game. */
static int player_placeable(CPUState* cpu) {
    return game_is_gameplay_live(cpu) && cpu->read_word(ADDR_DEMO_MODE) == 0u;
}

/* Spyro 1's tables: the same world scale. */
static const int s_move_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 25, 100, 300, 500, 800 };
static const int s_turn_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 10,  25,  35,  50,  75 };

const freecam_game_desc freecam_desc = {
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
    .player_facing = 0u, .player_facing_byte = 0u,   /* facing field not located */
    .level_id      = ADDR_LEVEL_ID,
    .level_live         = NULL,   /* game_is_gameplay_live */
    .player_placeable   = player_placeable,
    .place_camera       = NULL,   /* placement camera routine not located */
    .draw_all           = NULL,
    .reset_momentum     = NULL,
    .resolve_named_pose = NULL,
    .move_speed = s_move_speed,
    .turn_speed = s_turn_speed,
};
