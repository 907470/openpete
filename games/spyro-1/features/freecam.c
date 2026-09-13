// games/spyro-1/features/freecam.c — Spyro 1 descriptor for the debug free
// camera (F). The drive, path and teleport bodies are shared
// (games/common/freecam_game.c); this file holds the addresses and the three
// game hooks.

#include "freecam_game.h"
#include "spyro_native_reextract.h"   /* terrain_native_topdown_pose / _sweep_pose */
#include "openpete_freecam.h"         /* OPENPETE_FREECAM_SPEED_STEPS */
#include "psx_runtime_internal.h"     /* CPUState + its read/write callbacks */

#include <stdint.h>
#include <stdlib.h>   /* atoi (sweepN) */
#include <string.h>

/* g_Camera @ 0x80076DD0 (camera.h). Fields we touch:
 *   m_Position @ +0x28 : Vector3D int32 x, y, z   (world; Z is up)
 *   m_Rotation @ +0x4C : int16 roll, pitch, yaw   (yaw/heading @ +0x50)
 * These are exactly the practice ROM's _cameraPosition (0x80076DF8 = C+0x28)
 * and _cameraAngle (0x80076E1C = C+0x4C). */
#define G_CAMERA     0x80076DD0u

/* g_Spyro @ 0x80078A58 (spyro.h). Fields the teleport writes; both offsets are
 * confirmed against the original code, which forms them as g_Spyro + literal
 * (m_Position at 0x8003DF70, m_previousPosition at 0x8003DF7C).
 *   m_Position         @ +0x00 : Vector3D int32 x, y, z   (camera's world space)
 *   m_previousPosition @ +0x8C : where the physics thinks he was last tick */
#define G_SPYRO      0x80078A58u

#define ADDR_LEVEL_LOAD_STATE     0x80075864u  /* g_LoadStage: < 0 while a level is live */
#define ADDR_OCCLUSION_GROUP      0x80076E24u  /* g_Camera.m_OcclusionGroup (+0x54) */
#define ADDR_CAMERA_UNK_POS_VEC_X 0x80076EBCu  /* _cameraUnknownPositionVectorX */

/* Per-tick move (world units) and turn (angle units) magnitudes, indexed by
 * the shared speed step. Straight from the practice ROM's freecam.c tables. */
static const int s_move_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 25, 100, 300, 500, 800 };
static const int s_turn_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 10,  25,  35,  50,  75 };

/* A level is live while g_LoadStage is negative — unambiguous, unlike
 * g_Gamestate whose Playing==0 aliases the zero-initialised boot value. */
static int spyro1_level_live(CPUState* cpu) {
    return (int32_t)cpu->read_word(ADDR_LEVEL_LOAD_STATE) < 0;
}

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
static void spyro1_draw_all(CPUState* cpu) {
    /* Only meaningful once a level is live. */
    if (!spyro1_level_live(cpu)) return;
    cpu->write_word(ADDR_OCCLUSION_GROUP, 0xFFFFFFFFu);  /* m_OcclusionGroup = -1 */
    cpu->write_word(ADDR_CAMERA_UNK_POS_VEC_X, 0u);
}

/* func_8003DFA4 is what the game itself calls to "reset all velocity/momentum"
 * (pete.c, on charge damage) — it nulls m_TrueVelocity, m_TrueSpeed,
 * m_Acceleration and, crucially, m_Physics.m_SpeedAngle.m_Speed. That last one
 * is the root of the momentum chain (m_Speed drives m_Acceleration, which
 * drives m_Velocity), so clearing only the vectors would let the old speed
 * regenerate on the next tick. */
extern void func_8003DFA4(CPUState* cpu);
static void spyro1_reset_momentum(CPUState* cpu) { func_8003DFA4(cpu); }

/* "topdown": the whole-level overhead pose; "sweepN": corner N of a
 * low-altitude circuit. Both resolve against the live sector table. */
static int spyro1_resolve_named_pose(CPUState* cpu, const char* name, int32_t v[5]) {
    if (strcmp(name, "topdown") == 0) return terrain_native_topdown_pose(cpu, v);
    if (strncmp(name, "sweep", 5) == 0) return terrain_native_sweep_pose(cpu, atoi(name + 5), v);
    return 0;
}

const freecam_game_desc freecam_desc = {
    .cam_pos_x = G_CAMERA + 0x28u,
    .cam_pos_y = G_CAMERA + 0x2Cu,
    .cam_pos_z = G_CAMERA + 0x30u,
    .cam_yaw   = G_CAMERA + 0x50u,
    .cam_pitch = G_CAMERA + 0x4Eu,
    .angle_bits = 16,
    .yaw_offset = 0, .yaw_sign = 1, .pitch_sign = 1,
    /* ~87° up/down so the camera can't flip (practice ROM value). */
    .pitch_limit = 0x03E0,
    .freeze_only_when_live = 0,
    /* The practice ROM's StopCameraFollowSpyro / StopUpdateSpyro byte-patch
     * targets. CameraFollowSpyro stops re-deriving the camera from Spyro;
     * UpdateSpyro freezes Spyro so the scene holds still. */
    .cam_follow    = 0x80037BD4u,   /* CameraFollowSpyro */
    .player_update = 0x8004A200u,   /* UpdateSpyro */
    .extra_freeze  = NULL,
    .player_pos      = G_SPYRO + 0x00u,
    .player_prev_pos = G_SPYRO + 0x8Cu,
    .level_live         = spyro1_level_live,
    .draw_all           = spyro1_draw_all,
    .reset_momentum     = spyro1_reset_momentum,
    .resolve_named_pose = spyro1_resolve_named_pose,
    .move_speed = s_move_speed,
    .turn_speed = s_turn_speed,
};
