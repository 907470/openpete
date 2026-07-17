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
#include "openpete_log.h"
#include "psx_runtime.h"        /* CPUState + its read/write callbacks */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>   /* getenv (scripted auto-engage) */

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

/* g_Spyro @ 0x80078A58 (spyro.h). Fields the teleport writes; both offsets are
 * confirmed against the original code, which forms them as g_Spyro + literal
 * (m_Position at 0x8003DF70, m_previousPosition at 0x8003DF7C).
 *   m_Position         @ +0x00 : Vector3D int32 x, y, z   (camera's world space)
 *   m_previousPosition @ +0x8C : where the physics thinks he was last tick */
#define G_SPYRO             0x80078A58u
#define SPYRO_POS_X         (G_SPYRO + 0x00u)
#define SPYRO_PREV_POS_X    (G_SPYRO + 0x8Cu)

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

#define ADDR_LEVEL_LOAD_STATE     0x80075864u  /* g_LoadStage: < 0 while a level is live */
#define ADDR_OCCLUSION_GROUP      0x80076E24u  /* g_Camera.m_OcclusionGroup (+0x54) */
#define ADDR_CAMERA_UNK_POS_VEC_X 0x80076EBCu  /* _cameraUnknownPositionVectorX */

/* Draw every resident sector instead of following the camera's region.
 *
 * Spyro's "regions" are baked occlusion groups (a per-region PVS). Normally the
 * world renderer (func_800258F0, via func_8002B9CC) only frustum-tests the
 * sectors in the CURRENT group's sublist — so flying into a new area shows
 * nothing there until the camera's group switches (the pop-in). But the whole
 * level is already resident in RAM, and func_800258F0 has a "no group" arm:
 * pass m_OcclusionGroup = -1 and it frustum-tests EVERY sector. That's a real
 * game path (the title screen uses func_800258F0(-1)), and the native terrain
 * feed already handles the -1 "walk-all" case, so this needs no renderer
 * change. Result: everything in the frustum draws regardless of camera region —
 * no pop-in — while off-screen sectors are still frustum-culled.
 *
 * C0mposer's raycast-to-follow existed only for the PS1 fill-rate limit we
 * don't have; -1 supersedes it. We can safely own m_OcclusionGroup because the
 * game's own writer (the floor raycast) lives inside CameraFollowSpyro, which
 * we suppress. Runs only while engaged, so the canonical path is untouched. */
static void freecam_force_draw_all_sectors(CPUState* cpu) {
    /* Only meaningful once a level is live. */
    if ((int32_t)cpu->read_word(ADDR_LEVEL_LOAD_STATE) >= 0) return;
    cpu->write_word(ADDR_OCCLUSION_GROUP, 0xFFFFFFFFu);  /* m_OcclusionGroup = -1 */
    cpu->write_word(ADDR_CAMERA_UNK_POS_VEC_X, 0u);
}

/* Drive the camera from input + force all-sector drawing. Runs IN PLACE OF
 * CameraFollowSpyro (see freecam_native_should_freeze), i.e. at the exact frame
 * point the follow logic normally runs, so the camera we write is what the
 * downstream matrix setup and world renderer (func_8002B9CC/func_800258F0)
 * consume that same frame — and suppressing CameraFollowSpyro is also what lets
 * us own m_OcclusionGroup (the game's raycast that sets it lives there). */
static void freecam_drive(CPUState* cpu) {
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
    yaw   -= (int32_t)lround((double)ax.look_yaw   * turn);
    pitch -= (int32_t)lround((double)ax.look_pitch * turn);
    /* OPENPETE_FREECAM_SPIN=<yaw-units-per-tick>: scripted continuous yaw
     * sweep (4096 = full turn), on top of input — lets one run profile every
     * view direction (perf-vs-direction hunts, e.g. gnorc_cove 2026-07-10). */
    {
        static int s_have_spin = -1; static int s_spin;
        if (s_have_spin < 0) {
            const char* e = getenv("OPENPETE_FREECAM_SPIN");
            s_spin = e ? atoi(e) : 0;
            s_have_spin = 1;
        }
        yaw += s_spin;
    }
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

    /* OPENPETE_FREECAM_DRIFT="dx,dy,dz": scripted per-tick camera drift
     * (world units), applied on top of input. Deterministic boundary-crossing
     * repro for cull/handoff work (Phase 8.11) — headless runs can sweep the
     * camera across an admission edge without a human at the stick. */
    {
        static int32_t s_drift[3]; static int s_have_drift = -1;
        if (s_have_drift < 0) {
            const char* e = getenv("OPENPETE_FREECAM_DRIFT");
            s_have_drift = 0;
            if (e && *e) {
                char* end;
                s_drift[0] = (int32_t)strtol(e, &end, 10);
                if (*end) s_drift[1] = (int32_t)strtol(end + 1, &end, 10);
                if (*end) s_drift[2] = (int32_t)strtol(end + 1, &end, 10);
                s_have_drift = 1;
            }
        }
        if (s_have_drift) { px += s_drift[0]; py += s_drift[1]; pz += s_drift[2]; }
    }

    cpu->write_word(CAM_POS_X, (uint32_t)px);
    cpu->write_word(CAM_POS_Y, (uint32_t)py);
    cpu->write_word(CAM_POS_Z, (uint32_t)pz);

    /* Log the live camera while engaged, but only when it changed since the
     * last log (no console spam while parked). */
    {
        int32_t cyaw = (int16_t)yaw, cpitch = (int16_t)pitch;
        static int      s_have = 0;
        static int32_t  l_x, l_y, l_z, l_yaw, l_pitch;
        if (!s_have || px != l_x || py != l_y || pz != l_z ||
            cyaw != l_yaw || cpitch != l_pitch) {
            s_have = 1;
            l_x = px; l_y = py; l_z = pz; l_yaw = cyaw; l_pitch = cpitch;
            OPL_INFO_F("freecam", "pos=(%d, %d, %d) yaw=%d pitch=%d",
                       px, py, pz, (int)cyaw, (int)cpitch);
        }
    }

    /* Draw all sectors in view, not just the camera's region (no pop-in). */
    freecam_force_draw_all_sectors(cpu);
}

/* Move Spyro to wherever the free camera is floating. Camera and player share
 * one world space, so the position copies straight across.
 *
 * m_previousPosition is written to match: the physics derives this tick's
 * movement from it, so leaving it at the pre-teleport spot would present the
 * whole jump as one tick of travel — a sweep across the level that collides
 * with whatever lies between the two points.
 *
 * Momentum is cancelled through the game's own routine rather than by zeroing
 * fields here: whatever Spyro carried when the camera was engaged belongs to
 * the place he left, not the one he lands in. func_8003DFA4 is what the game
 * itself calls to "reset all velocity/momentum" (pete.c, on charge damage) —
 * it nulls m_TrueVelocity, m_TrueSpeed, m_Acceleration and, crucially,
 * m_Physics.m_SpeedAngle.m_Speed. That last one is the root of the momentum
 * chain (m_Speed drives m_Acceleration, which drives m_Velocity), so clearing
 * only the vectors would let the old speed regenerate on the next tick.
 *
 * Everything else — state, animation, health, facing — is deliberately
 * untouched: Spyro carries on falling / gliding / standing as he was, from
 * the new spot. UpdateSpyro resumes next tick, once the camera is
 * disengaged, and re-derives the floor and wall queries from the new
 * position. */
extern void func_8003DFA4(CPUState* cpu);   /* "Reset all velocity/momentum" */

static void freecam_teleport_player_to_camera(CPUState* cpu) {
    const int32_t x = (int32_t)cpu->read_word(CAM_POS_X);
    const int32_t y = (int32_t)cpu->read_word(CAM_POS_Y);
    const int32_t z = (int32_t)cpu->read_word(CAM_POS_Z);

    cpu->write_word(SPYRO_POS_X + 0, (uint32_t)x);
    cpu->write_word(SPYRO_POS_X + 4, (uint32_t)y);
    cpu->write_word(SPYRO_POS_X + 8, (uint32_t)z);

    cpu->write_word(SPYRO_PREV_POS_X + 0, (uint32_t)x);
    cpu->write_word(SPYRO_PREV_POS_X + 4, (uint32_t)y);
    cpu->write_word(SPYRO_PREV_POS_X + 8, (uint32_t)z);

    func_8003DFA4(cpu);

    OPL_INFO_F("freecam", "teleported Spyro to (%d, %d, %d)", x, y, z);
}

/* Pull up to `max` signed integers out of arbitrary text, skipping any
 * non-numeric runs. Lenient on purpose so a whole freecam log line can be
 * pasted verbatim — "pos=(84992, 45582, 9948) yaw=1024 pitch=160" yields all
 * five. Returns the count parsed. */
static int freecam_parse_ints(const char* s, int32_t* out, int max) {
    int n = 0;
    while (*s && n < max) {
        int is_num = (*s >= '0' && *s <= '9') ||
                     ((*s == '-' || *s == '+') && s[1] >= '0' && s[1] <= '9');
        if (!is_num) { s++; continue; }
        char* end;
        long v = strtol(s, &end, 10);
        out[n++] = (int32_t)v;
        s = end;
    }
    return n;
}

/* Apply an OPENPETE_FREECAM_POSE string ("x,y,z[,yaw,pitch]", or any text with
 * ≥3 integers in it) to g_Camera. Position is required; yaw/pitch optional. */
static void freecam_apply_pose(CPUState* cpu, const char* s) {
    int32_t v[5] = {0, 0, 0, 0, 0};
    int n = freecam_parse_ints(s, v, 5);
    if (n < 3) {
        OPL_WARN_F("freecam",
                   "OPENPETE_FREECAM_POSE needs at least x,y,z (got %d numbers)", n);
        return;
    }
    cpu->write_word(CAM_POS_X, (uint32_t)v[0]);
    cpu->write_word(CAM_POS_Y, (uint32_t)v[1]);
    cpu->write_word(CAM_POS_Z, (uint32_t)v[2]);
    if (n >= 4) cpu->write_half(CAM_YAW,   (uint16_t)(int16_t)v[3]);
    if (n >= 5) cpu->write_half(CAM_PITCH, (uint16_t)(int16_t)v[4]);
    OPL_INFO_F("freecam", "pose set to (%d, %d, %d) yaw=%d pitch=%d",
               v[0], v[1], v[2], v[3], v[4]);
}

/* Optional scripted / headless engage. Set OPENPETE_FREECAM=1 to engage the
 * free camera, or OPENPETE_FREECAM_POSE="x,y,z,yaw,pitch" to engage AND jump to
 * a specific pose (for reproducing a test view — the coords come straight from
 * the per-tick freecam log). Fires once the level is live — deliberately NOT
 * from boot: engaging during a level load / demo transition would freeze
 * UpdateSpyro while the level is still setting up and leave it un-rendered.
 * Interactive use (F / stick-click) is independent of this. */
void freecam_native_maybe_autoengage(CPUState* cpu) {
    static int s_done = 0;
    if (s_done) return;
    const char* pose = getenv("OPENPETE_FREECAM_POSE");
    const char* eng  = getenv("OPENPETE_FREECAM");
    int want = (pose && *pose) || (eng && *eng && *eng != '0');
    if (!want || openpete_freecam_active()) { s_done = 1; return; }
    /* Gate on "a level is live" (load state negative) — unambiguous, unlike
     * g_Gamestate whose Playing==0 aliases the zero-initialised boot value. */
    if ((int32_t)cpu->read_word(ADDR_LEVEL_LOAD_STATE) < 0) {
        s_done = 1;
        openpete_freecam_toggle();
        if (pose && *pose) freecam_apply_pose(cpu, pose);
    }
}

int freecam_native_should_freeze(CPUState* cpu, uint32_t addr) {
    if (!openpete_freecam_active()) return 0;

    /* Spyro's own update: suppress it entirely so the scene holds still. */
    if (addr == ADDR_UPDATE_SPYRO) return 1;

    /* The camera follow: replace it with our drive (same fiber, same frame
     * point), then suppress the original so it can't overwrite the camera. */
    if (addr == ADDR_CAMERA_FOLLOW_SPYRO) {
        /* A pending T: move Spyro up to the camera and hand the camera back.
         * Disengaging here rather than on the host loop keeps the two halves
         * atomic — the same tick that moves the player is the one that stops
         * freezing UpdateSpyro. Returning 0 then lets the real
         * CameraFollowSpyro run immediately, so it re-derives the follow
         * camera from Spyro's new position on this very tick and there's no
         * frame of stale free-camera view before it snaps back. */
        if (openpete_freecam_take_teleport_request()) {
            freecam_teleport_player_to_camera(cpu);
            openpete_freecam_toggle();
            return 0;
        }
        freecam_drive(cpu);
        return 1;
    }
    return 0;
}
