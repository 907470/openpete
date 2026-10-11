// games/spyro-1/features/freecam.c - Spyro 1 descriptor for the debug free
// camera (F). The drive, path and teleport bodies are shared
// (games/common/freecam_game.c); this file holds the addresses and the game
// hooks.

#include "freecam_game.h"
#include "spyro-1/renderer/reextract.h"   /* terrain_native_topdown_pose / _sweep_pose */
#include "openpete_freecam.h"         /* OPENPETE_FREECAM_SPEED_STEPS */
#include "psx_runtime_internal.h"     /* CPUState + its read/write callbacks */

#include <stdint.h>
#include <stdlib.h>   /* atoi (sweepN) */
#include <string.h>

/* The camera pose (g_Camera m_Position @ +0x28, m_Rotation @ +0x4C: int16
 * roll, pitch, yaw) is read through game_layout.cam (renderer/layout.c);
 * these are the practice ROM's _cameraPosition and _cameraAngle. */

/* g_Spyro @ 0x80078A58 (spyro.h). Fields the teleport writes; the offsets are
 * confirmed against the original code, which forms them as g_Spyro + literal
 * (m_Position at 0x8003DF70, m_previousPosition at 0x8003DF7C).
 *   m_Position         @ +0x00  : Vector3D int32 x, y, z   (camera's world space)
 *   m_bodyRotation.z   @ +0x0E  : u_char facing, rewritten from m_RotZ >> 4 each tick
 *   m_previousPosition @ +0x8C  : where the physics thinks he was last tick
 *   m_SpeedAngle.m_RotZ @ +0x11C: the 12-bit facing the physics turns from */
#define G_SPYRO      0x80078A58u

#define ADDR_LEVEL_LOAD_STATE     0x80075864u  /* g_LoadStage: < 0 while a level is live */
#define ADDR_GAMESTATE            0x800757D8u  /* g_Gamestate: 0 = GS_Playing */
#define ADDR_DEMO_MODE            0x80075714u  /* g_DemoMode: nonzero while the attract demo drives the pad */
#define ADDR_LEVEL_ID             0x8007596Cu  /* g_LevelId */
#define G_CAMERA                  0x80076DD0u  /* g_Camera (camera.h): m_State +0x58, unk_0xC0 +0xC0,
                                                  m_Focus +0xD0, m_FocusRotation +0xD4, m_SphericalPreset +0xD8 */
#define ADDR_CAM_PRESET_PLACED    0x80078668u  /* D_80078668: the loader's spawn preset (SphericalCoordsOffset, 0x18) */
#define ADDR_CAM_PRESET_DEFAULT   0x8006C934u  /* D_8006C934: its source values */
#define ADDR_OCCLUSION_GROUP      0x80076E24u  /* g_Camera.m_OcclusionGroup (+0x54) */
#define ADDR_CAMERA_UNK_POS_VEC_X 0x80076EBCu  /* _cameraUnknownPositionVectorX */

/* Per-tick move (world units) and turn (angle units) magnitudes, indexed by
 * the shared speed step. Straight from the practice ROM's freecam.c tables. */
static const int s_move_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 25, 100, 300, 500, 800 };
static const int s_turn_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 10,  25,  35,  50,  75 };

/** @brief Reports whether a level is live, read as g_LoadStage negative.
 *
 * g_LoadStage is used instead of g_Gamestate because GS_Playing (0) aliases
 * the zero-initialised boot value.
 * @param cpu The guest CPU state.
 * @return 1 while g_LoadStage is negative, else 0. */
static int spyro1_level_live(CPUState* cpu) {
    return (int32_t)cpu->read_word(ADDR_LEVEL_LOAD_STATE) < 0;
}

/** @brief Makes the world renderer frustum-test every resident sector instead of the camera's occlusion group.
 *
 * Spyro's regions are baked occlusion groups (a per-region PVS). The world
 * renderer (func_800258F0, via func_8002B9CC) frustum-tests only the sectors
 * in the current group's sublist, so a camera flown into a new area shows
 * nothing there until the group switches. func_800258F0 has a no-group arm:
 * with m_OcclusionGroup -1 it frustum-tests every sector. The title screen
 * takes that arm (func_800258F0(-1)), and the native terrain feed handles the
 * -1 walk-all case. Off-screen sectors stay frustum-culled.
 *
 * Stores m_OcclusionGroup -1 and _cameraUnknownPositionVectorX 0 while a
 * level is live. The game's own m_OcclusionGroup writer (the floor raycast)
 * is inside CameraFollowSpyro, which the free camera suppresses. The free
 * camera drive calls this each tick only while engaged, so the unengaged
 * path makes no store.
 * @param cpu The guest CPU state. */
static void spyro1_draw_all(CPUState* cpu) {
    /* Only meaningful once a level is live. */
    if (!spyro1_level_live(cpu)) return;
    cpu->write_word(ADDR_OCCLUSION_GROUP, 0xFFFFFFFFu);  /* m_OcclusionGroup = -1 */
    cpu->write_word(ADDR_CAMERA_UNK_POS_VEC_X, 0u);
}

extern void func_8003DFA4(CPUState* cpu);
/** @brief Zeroes the player's velocity, acceleration and speed through the game's own reset.
 *
 * func_8003DFA4 is the routine pete.c calls to reset all momentum: it nulls
 * m_TrueVelocity, m_TrueSpeed, m_Acceleration and
 * m_Physics.m_SpeedAngle.m_Speed. The last is the root of the momentum chain
 * (m_Speed drives m_Acceleration, which drives the velocity), so clearing
 * only the vectors would let the old speed regenerate on the next tick.
 * @param cpu The guest CPU state. */
static void spyro1_reset_momentum(CPUState* cpu) { func_8003DFA4(cpu); }

/** @brief Reports whether the player can be placed and the pose read back.
 *
 * Requires a live level, g_Gamestate GS_Playing, the pad driving the player
 * (g_DemoMode 0), and the camera in its ordinary follow (g_Camera.m_State 0).
 * g_Gamestate stays GS_Playing through the level fly-in; m_State separates
 * it (0x80000010/11 until the follow takes over), and a placement inside the
 * fly-in would be flown away from.
 * @param cpu The guest CPU state.
 * @return 1 when every condition holds, else 0. */
static int spyro1_player_placeable(CPUState* cpu) {
    return spyro1_level_live(cpu) &&
           cpu->read_word(ADDR_GAMESTATE) == 0u &&
           cpu->read_word(ADDR_DEMO_MODE) == 0u &&
           cpu->read_word(G_CAMERA + 0x58u) == 0u;
}

extern void func_80034358(CPUState* cpu);
/** @brief Places the camera behind the player as the loader does for a spawn without a fly-in.
 *
 * Reproduces LoadLevelScene's camera setup (loaders.c): copies the 0x18-byte
 * preset D_8006C934 to D_80078668 with radius 1536, stores g_Camera m_Focus
 * = &g_Spyro.m_Position, m_State 0, unk_0xC0 0, m_SphericalPreset = the
 * preset and m_FocusRotation = g_Spyro.m_SpeedAngle.m_RotZ, then calls
 * func_80034358, which applies the preset and snaps m_Position to the
 * preset's offset from the focus. Without it the follow converges from
 * wherever the camera stood, sweeping across the level for a hundred-plus
 * ticks.
 * @param cpu The guest CPU state. */
static void spyro1_place_camera(CPUState* cpu) {
    for (uint32_t o = 0; o < 0x18u; o += 4u)
        cpu->write_word(ADDR_CAM_PRESET_PLACED + o, cpu->read_word(ADDR_CAM_PRESET_DEFAULT + o));
    cpu->write_word(G_CAMERA + 0xD0u, G_SPYRO);              /* m_Focus = &g_Spyro.m_Position */
    cpu->write_word(G_CAMERA + 0x58u, 0u);                   /* m_State */
    cpu->write_word(G_CAMERA + 0xC0u, 0u);                   /* unk_0xC0 (state source) */
    cpu->write_word(ADDR_CAM_PRESET_PLACED + 8u, 1536u);     /* m_Coords.radius */
    cpu->write_word(G_CAMERA + 0xD8u, ADDR_CAM_PRESET_PLACED); /* m_SphericalPreset */
    cpu->write_word(G_CAMERA + 0xD4u, cpu->read_word(G_SPYRO + 0x11Cu)); /* m_FocusRotation */
    func_80034358(cpu);
}

/** @brief Resolves a named free-camera pose against the live sector table.
 *
 * `topdown` is the whole-level overhead pose; `sweepN` is corner N of a
 * low-altitude circuit (N parsed by atoi, taken modulo 4).
 * @param cpu The guest CPU state.
 * @param name The pose name.
 * @param v Receives {x, y, z, yaw, pitch}.
 * @return 1 on success, 0 for an unknown name or when no sector table is
 *     loaded. */
static int spyro1_resolve_named_pose(CPUState* cpu, const char* name, int32_t v[5]) {
    if (strcmp(name, "topdown") == 0) return terrain_native_topdown_pose(cpu, v);
    if (strncmp(name, "sweep", 5) == 0) return terrain_native_sweep_pose(cpu, atoi(name + 5), v);
    return 0;
}

const freecam_game_desc spyro1_freecam_desc = {
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
    /* m_Physics.m_SpeedAngle.m_RotZ (+0xC8 +0x50 +0x4) and the byte
     * m_bodyRotation.z (+0x0C +2) the player update derives from it. */
    .player_facing      = G_SPYRO + 0x11Cu,
    .player_facing_byte = G_SPYRO + 0x0Eu,
    .level_id           = ADDR_LEVEL_ID,
    .level_live         = spyro1_level_live,
    .draw_all           = spyro1_draw_all,
    .reset_momentum     = spyro1_reset_momentum,
    .player_placeable   = spyro1_player_placeable,
    .place_camera       = spyro1_place_camera,
    .resolve_named_pose = spyro1_resolve_named_pose,
    .move_speed = s_move_speed,
    .turn_speed = s_turn_speed,
};
