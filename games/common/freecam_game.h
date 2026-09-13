#pragma once
/* freecam_game.h — game-side half of the debug free camera (F key), shared
 * by every game target.
 *
 * The host-side half (openpete_freecam.h) owns the toggle, the latched input
 * axes, the speed step, mouse counts and the teleport request; it carries no
 * addresses. This half drives the camera bytes and freezes the game's camera
 * follow and player update while engaged. Everything address- or
 * game-specific comes through `freecam_desc`, one per game target; the body
 * (freecam_game.c) reads only CPUState and the descriptor.
 *
 * Called from the game's game_extras_impl.c: game_on_frame_pre ->
 * freecam_maybe_autoengage; psx_override_dispatch_game ->
 * freecam_should_freeze. */

#include <stdint.h>

typedef struct CPUState CPUState;

typedef struct freecam_game_desc {
    /* Camera bytes the game's view-matrix setup reads after the follow logic
     * has run: three int32 world coordinates (Z up) and two Euler angles in
     * 1/4096-turn units. */
    uint32_t cam_pos_x, cam_pos_y, cam_pos_z;
    uint32_t cam_yaw, cam_pitch;
    /* Bits of a stored angle: 16 (int16) or 12 (0..0xFFF). A seed read
     * sign-extends from bit angle_bits-1; a stamp masks to angle_bits. */
    int angle_bits;
    /* Stored angle = logical angle * sign + offset, where the logical
     * convention is the drive's: yaw 0 looks along +X and rises turning
     * left, pitch 0 is level and rises looking down. */
    int yaw_offset, yaw_sign, pitch_sign;
    /* Pitch clamp in angle units, applied to the logical pitch. */
    int pitch_limit;
    /* 1: cam_follow / player_update are short-circuited only while
     * level_live; 0: whenever engaged. */
    int freeze_only_when_live;
    /* Function entries short-circuited while engaged. `cam_follow` is also
     * the tick point at which the drive runs, in its place on the game
     * fiber. `extra_freeze` lists further entries, 0-terminated; NULL for
     * none. */
    uint32_t cam_follow;
    uint32_t player_update;
    const uint32_t* extra_freeze;
    /* Player position and previous position (three int32 each) written by
     * the teleport; player_prev_pos 0 = none. */
    uint32_t player_pos, player_prev_pos;
    /* Hooks; each may be NULL. NULL level_live means game_is_gameplay_live. */
    int  (*level_live)(CPUState* cpu);
    void (*draw_all)(CPUState* cpu);        /* force every sector in view */
    void (*reset_momentum)(CPUState* cpu);  /* after a teleport */
    /* Named poses ("topdown", "sweepN"): fills v = {x, y, z, yaw, pitch}
     * in stored units, returns 0 when the name is unknown or no level is
     * live. */
    int  (*resolve_named_pose)(CPUState* cpu, const char* name, int32_t v[5]);
    /* Per speed step move (world units) and turn (angle units) per tick,
     * OPENPETE_FREECAM_SPEED_STEPS entries each. */
    const int* move_speed;
    const int* turn_speed;
} freecam_game_desc;

extern const freecam_game_desc freecam_desc;

/* Per function-entry hook. Returns 1 (the recompiled body short-circuits)
 * when the free camera is engaged and `addr` is a frozen entry. For
 * `cam_follow` it first drives the camera in place of the follow logic. Call
 * from psx_override_dispatch_game. */
int freecam_should_freeze(CPUState* cpu, uint32_t addr);

/* Scripted engage (OPENPETE_FREECAM / OPENPETE_FREECAM_POSE /
 * OPENPETE_FREECAM_PATH) once the level is live. Call once per tick from
 * game_on_frame_pre. */
void freecam_maybe_autoengage(CPUState* cpu);
