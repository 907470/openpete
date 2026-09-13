// games/spyro-3/features/freecam.c — Spyro 3 descriptor for the debug free
// camera (F). The drive, path and teleport bodies are shared
// (games/common/freecam_game.c); this file holds the addresses.

#include "freecam_game.h"
#include "openpete_freecam.h"         /* OPENPETE_FREECAM_SPEED_STEPS */

#include <stddef.h>
#include <stdint.h>

/* g_Camera @ 0x8006DFF8 (camera.h). Fields we touch:
 *   nextCameraPosCartesian @ +0x28 : int32 x, y, z (world; Z is up); Draw
 *                                     copies it to +0x34 each tick and the
 *                                     draw utilities read it
 *   +0x46 : pitch, +0x48 : yaw       : 12-bit angles (0..0xFFF); the
 *                                     rotation builder (func_8004F6C4, from
 *                                     Draw with a0 = +0x44) masks each with
 *                                     0xFFF, and the follow's own writer
 *                                     (func_80012B34) sets +0x44 = 0, +0x46
 *                                     from the polar elevation and +0x48
 *                                     from the azimuth + 0x800. Raising
 *                                     +0x46 looks down, raising +0x48 turns
 *                                     left, and yaw 0x400 looks along +Y:
 *                                     the Spyro 1 convention with no offset. */
#define G_CAMERA     0x8006DFF8u

/* g_Spyro @ 0x80070328 (spyro.h): position at +0x00, three int32. No
 * previous-position field is identified (+0x124 is a copy made inside the
 * movement routine func_80041C20, after the move), so a teleport writes the
 * position alone, and no momentum-reset routine is identified. */
#define G_SPYRO      0x80070328u

/* Spyro 1's tables: Spyro 3 uses the same world scale (a level spans about
 * 0x20000 units). */
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
    /* The rotation builder composes rotX(pitch)·rotY(yaw)·rotZ(roll); at
     * 53° of pitch (0x260) the frame visibly rolls, at 31° (0x160) it does
     * not. */
    .pitch_limit = 0x01C0,
    /* The follow entry is also reached from the loading-state updates and
     * clears state before its follow work, so it and the player update are
     * frozen only while gameplay is live (loadStage -1, game.state 0). */
    .freeze_only_when_live = 1,
    /* func_8001204C: the camera update, called with UPDATE_CAMERA from
     * func_80055294 every gameplay tick; its callee func_80012168 is the only
     * writer of the five pose fields between the follow and the draw.
     * func_8003E83C: the player update dispatcher (UPDATE_SPYRO). */
    .cam_follow    = 0x8001204Cu,
    .player_update = 0x8003E83Cu,
    .extra_freeze  = NULL,
    .player_pos      = G_SPYRO + 0x00u,
    .player_prev_pos = 0u,
    .level_live         = NULL,   /* game_is_gameplay_live */
    .draw_all           = NULL,   /* no camera-region PVS identified */
    .reset_momentum     = NULL,
    .resolve_named_pose = NULL,
    .move_speed = s_move_speed,
    .turn_speed = s_turn_speed,
};
