// games/spyro-1/features/freecam_native.c — Spyro-1 debug free camera (F).
//
// Ported from C0mposer's Spyro-1-Practice-Rom (src/freecam.c,
// src/custom_menu*.c). The practice ROM's technique, in three parts:
//
//   1. Freeze the camera-follow and player-update functions so nothing
//      overwrites the camera each frame. The practice ROM byte-patches
//      their prologues to `jr ra; nop`; openpete has a cleaner built-in
//      equivalent — psx_override_dispatch_game returning 1 for a function
//      entry makes the generated body early-return (see core/runtime/
//      dispatch.c). freecam_native_should_freeze() drives that.
//
//   2. Drive g_Camera's position + Euler angles directly from input.
//      freecam_native_drive() read-modify-writes those fields each tick.
//
//   3. The game's own per-frame matrix setup (which still runs — freezing
//      CameraFollowSpyro does NOT skip it; the practice ROM proves this)
//      rebuilds m_ViewMatrix / m_ProjectionMatrix from the fields we wrote.
//      So both the PsyCross reference renderer and the native Vulkan
//      renderer — which read those built matrices out of g_Camera — follow
//      the free camera for free, with no matrix math duplicated here.
//
// Debug tool: OFF by default (zero writes → oracle-clean). ON is an
// intentional, opt-in gameplay-state divergence for inspecting a level.
//
// Not yet ported: render-region resync (the practice ROM raycasts the floor
// under the camera to pick the terrain sector to draw, so flying far from
// Spyro keeps geometry visible). Without it the drawn set stays keyed to
// Spyro's frozen sector — fine for looking around the current area. See the
// TODO at the bottom.

#include "features/freecam_native.h"
#include "openpete_freecam.h"
#include "psx_runtime.h"        /* CPUState + its read/write callbacks */

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#  define M_PI    3.14159265358979323846
#endif
#ifndef M_PI_2
#  define M_PI_2  1.57079632679489661923
#endif

/* g_Camera @ 0x80076DD0 (camera.h). Fields we touch:
 *   m_Position @ +0x28 : Vector3D int32 x, y, z   (world; Z is up)
 *   m_Rotation @ +0x4C : int16 roll, pitch, yaw   (yaw/heading @ +0x50)
 * These are exactly the practice ROM's _cameraPosition (0x80076DF8 = C+0x28)
 * and _cameraAngle (0x80076E1C = C+0x4C). */
#define G_CAMERA     0x80076DD0u
#define CAM_POS_X    (G_CAMERA + 0x28u)
#define CAM_POS_Y    (G_CAMERA + 0x2Cu)
#define CAM_POS_Z    (G_CAMERA + 0x30u)
#define CAM_PITCH    (G_CAMERA + 0x4Eu)
#define CAM_YAW      (G_CAMERA + 0x50u)

/* Functions frozen while engaged (== the practice ROM's StopCameraFollowSpyro
 * / StopUpdateSpyro byte-patch targets). CameraFollowSpyro stops re-deriving
 * the camera from Spyro; UpdateSpyro freezes Spyro so the scene holds still. */
#define ADDR_CAMERA_FOLLOW_SPYRO  0x80037BD4u
#define ADDR_UPDATE_SPYRO         0x8004A200u

/* Spyro angle units: a full turn is 0x1000 (4096). */
#define ANGLE_FULL   4096.0
/* Clamp pitch to ~87° up/down so the camera can't flip (practice ROM value). */
#define PITCH_LIMIT  0x03E0

/* Per-tick move (world units) and turn (angle units) magnitudes, indexed by
 * the shared speed step. Straight from the practice ROM's freecam.c tables. */
static const int s_move_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 25, 100, 300, 500, 800 };
static const int s_turn_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 10,  25,  35,  50,  75 };

int freecam_native_should_freeze(uint32_t addr) {
    if (!openpete_freecam_active()) return 0;
    return addr == ADDR_CAMERA_FOLLOW_SPYRO || addr == ADDR_UPDATE_SPYRO;
}

void freecam_native_drive(CPUState* cpu) {
    if (!openpete_freecam_active()) return;

    openpete_freecam_axes ax;
    openpete_freecam_get_axes(&ax);

    const int si    = openpete_freecam_speed_index();  /* pre-clamped */
    const int speed = s_move_speed[si];
    const int turn  = s_turn_speed[si];

    /* --- Look: read/modify the Euler yaw + pitch (int16). Signs mirror the
     * practice ROM: turning right lowers yaw, looking up lowers pitch
     * ("positive pitch = look down"). --- */
    int32_t yaw   = (int16_t)cpu->read_half(CAM_YAW);
    int32_t pitch = (int16_t)cpu->read_half(CAM_PITCH);
    yaw   -= ax.look_yaw   * turn;
    pitch -= ax.look_pitch * turn;
    if (pitch >  PITCH_LIMIT) pitch =  PITCH_LIMIT;
    if (pitch < -PITCH_LIMIT) pitch = -PITCH_LIMIT;
    cpu->write_half(CAM_YAW,   (uint16_t)(int16_t)yaw);
    cpu->write_half(CAM_PITCH, (uint16_t)(int16_t)pitch);

    /* --- Move: read/modify the int32 position. Forward/right basis is the
     * practice ROM's: forward from (yaw, pitch), right = yaw − 90° on the
     * horizontal plane, and Space/Shift + forward-pitch move along world Z.
     * Freecam is already non-canonical, so host float trig is fine here (no
     * need to route through the game's fixed-point SinScaled). --- */
    int32_t px = (int32_t)cpu->read_word(CAM_POS_X);
    int32_t py = (int32_t)cpu->read_word(CAM_POS_Y);
    int32_t pz = (int32_t)cpu->read_word(CAM_POS_Z);

    const double ry = (double)yaw   * (2.0 * M_PI / ANGLE_FULL);
    const double rp = (double)pitch * (2.0 * M_PI / ANGLE_FULL);
    const double cp = cos(rp);
    const double fwdx =  cos(ry) * cp;
    const double fwdy =  sin(ry) * cp;
    const double fwdz = -sin(rp);
    const double rgtx =  cos(ry - M_PI_2);
    const double rgty =  sin(ry - M_PI_2);

    const double mvx = (ax.move_fwd * fwdx + ax.move_strafe * rgtx) * speed;
    const double mvy = (ax.move_fwd * fwdy + ax.move_strafe * rgty) * speed;
    const double mvz = (ax.move_fwd * fwdz) * speed + (double)ax.move_up * speed;

    px += (int32_t)lround(mvx);
    py += (int32_t)lround(mvy);
    pz += (int32_t)lround(mvz);

    cpu->write_word(CAM_POS_X, (uint32_t)px);
    cpu->write_word(CAM_POS_Y, (uint32_t)py);
    cpu->write_word(CAM_POS_Z, (uint32_t)pz);

    /* TODO: port FreeCamSyncRenderRegion — raycast the floor under the camera
     * (MCP_RaycastFloorHeightPrecise @ 0x8004DF24) and set the render-region
     * index (g_Camera+0x54 / 0x80076E24) so distant sectors keep drawing.
     * Needs an out-of-band recompiled-function call (save/restore caller-saved
     * GPRs); deferred until the basic fly-cam is validated in the window. */
}
