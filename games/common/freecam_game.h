#pragma once
/* freecam_game.h - game-side half of the debug free camera (F key), shared
 * by every game target.
 *
 * The host-side half (openpete_freecam.h) owns the toggle, the latched input
 * axes, the speed step, mouse counts and the teleport request; it carries no
 * addresses. This half drives the camera bytes and freezes the game's camera
 * follow and player update while engaged. Everything address- or
 * game-specific comes through `freecam_desc`, the running game's
 * descriptor; the body (freecam_game.c) reads only CPUState and the
 * descriptor.
 *
 * Called from the game's game_extras_impl.c: its on_frame_pre ->
 * freecam_maybe_autoengage; its override_dispatch (through
 * game_dispatch_preamble) -> freecam_should_freeze. */

#include <stdint.h>

typedef struct CPUState CPUState;

/* How a game composes its view matrix from the rotation record (roll at +0,
 * pitch at +2, yaw at +4), each factor in the game's own form: Rx(t) rows
 * (1,0,0) (0,c,-s) (0,s,c); Ry(t) rows (c,0,s) (0,1,0) (-s,0,c); Rz(t) rows
 * (c,s,0) (-s,c,0) (0,0,1). Rx(pitch)·Ry(yaw) is the roll-free view of
 * (yaw, pitch) in both known orders. A pose's roll R is the view
 * Rz(R)·Rx(pitch)·Ry(yaw), the roll applied about the view axis. */
typedef enum freecam_builder_order {
    /* Unknown: the stamp writes yaw and pitch only and leaves +0 to the
     * game; a pose's roll is ignored. */
    FREECAM_BUILDER_UNKNOWN = 0,
    /* M = Rz(+0)·Rx(+2)·Ry(+4): the stored triple is the pose's (roll,
     * pitch, yaw) as it is. */
    FREECAM_BUILDER_RZ_RX_RY,
    /* M = Rx(+2)·Ry(+4)·Rz(+0): +0 turns about the vector's third axis
     * (world X) before yaw, not about the view axis; the stamp writes the
     * triple of this order whose product equals the pose's view. */
    FREECAM_BUILDER_RX_RY_RZ,
} freecam_builder_order;

typedef struct freecam_game_desc {
    /* The camera pose bytes come from game_layout.cam: the position (three
     * int32, Z up) and the rotation record (roll, pitch, yaw halfwords, so
     * pitch at +2 and yaw at +4). */
    /* Bits of a stored angle: 16 (int16) or 12 (0..0xFFF). A seed read
     * sign-extends from bit angle_bits-1; a stamp masks to angle_bits. */
    int angle_bits;
    /* Stored angle = logical angle * sign + offset, where the logical
     * convention is the drive's: yaw 0 looks along +X and rises turning
     * left, pitch 0 is level and rises looking down. */
    int yaw_offset, yaw_sign, pitch_sign;
    /* Pitch clamp in angle units, applied to the logical pitch: the gimbal
     * bound, short of straight up or down where yaw degenerates. */
    int pitch_limit;
    /* The product order in which the game builds its view matrix
     * (game_layout.cam.view_matrix) from the rotation record, over the GTE
     * vector (camY - Y, camZ - Z, X - camX); see freecam_builder_order. */
    int builder_order;
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
    /* Player facing: `player_facing` is the 12-bit angle the physics turns
     * from (4096 per turn, 0 = +X); `player_facing_byte` the 8-bit copy
     * the game re-derives from it each tick (facing >> 4). 0 = the game
     * has none located; --player-pose then places without a facing. */
    uint32_t player_facing, player_facing_byte;
    /* Current level id word, printed by the exit pose line; 0 = none. */
    uint32_t level_id;
    /* Hooks; each may be NULL. NULL level_live means game_is_gameplay_live. */
    int  (*level_live)(CPUState* cpu);
    void (*draw_all)(CPUState* cpu);        /* force every sector in view */
    void (*reset_momentum)(CPUState* cpu);  /* after a teleport */
    /* 1 while the player can be placed and the pose read back: gameplay
     * live, the player under pad control (no attract demo), the camera in
     * its ordinary follow (no fly-in or exit sequence). NULL means
     * level_live. */
    int  (*player_placeable)(CPUState* cpu);
    /* The game's own camera setup for a player placed without a fly-in:
     * re-targets the follow at the player, facing the player's facing, and
     * snaps the camera to its preset offset. NULL = leave the follow to
     * converge on its own. */
    void (*place_camera)(CPUState* cpu);
    /* Named poses ("topdown", "sweepN"): fills v = {x, y, z, yaw, pitch}
     * in stored units, returns 0 when the name is unknown or no level is
     * live. */
    int  (*resolve_named_pose)(CPUState* cpu, const char* name, int32_t v[5]);
    /* Per speed step move (world units) and turn (angle units) per tick,
     * OPENPETE_FREECAM_SPEED_STEPS entries each. */
    const int* move_speed;
    const int* turn_speed;
} freecam_game_desc;

/* The running game's table: game_runtime_install (game_host_desc.h) copies
 * the registered descriptor's table here before any feeder runs. */
extern freecam_game_desc freecam_desc;

/** @brief Resolve a named free-camera pose against the live level's sector table.
 *
 * `topdown` is the whole-level overhead pose (terrain_native_topdown_pose);
 * `sweepN` is corner N of a low-altitude circuit (terrain_native_sweep_pose,
 * N parsed by atoi, taken modulo 4). Every game's sector table carries the
 * fields both read through game_layout, so each game's
 * freecam_game_desc.resolve_named_pose names this function.
 * @param cpu The guest CPU state.
 * @param name The pose name.
 * @param v Receives {x, y, z, yaw, pitch}.
 * @return 1 on success, 0 for an unknown name or when no sector table is
 *     loaded. */
int freecam_terrain_named_pose(CPUState* cpu, const char* name, int32_t v[5]);

/** @brief Decide at a guest function entry whether the free camera short-circuits it.
 *
 * Called from the game's override_dispatch. While disengaged nothing is frozen.
 * player_update and the extra_freeze entries are frozen, only while
 * level_live when freeze_only_when_live is set. At cam_follow outside a live
 * level under freeze_only_when_live, the follow runs and the host pose is
 * re-seeded at the next frozen entry. At cam_follow with a pending teleport
 * request, the player moves to the camera, the free camera disengages, and
 * the follow runs on the same tick. Otherwise cam_follow is replaced by the
 * free-camera drive at the same tick point.
 * @param cpu The guest CPU state.
 * @param addr The entry address being dispatched.
 * @return 1 when the recompiled body must return at once, else 0. */
int freecam_should_freeze(CPUState* cpu, uint32_t addr);

/** @brief The pose a present shows on a fractional flight path, from the snapshot pair it brackets.
 *
 * A path with a fraction in any numeric waypoint stamps each tick's pose
 * rounded to the game's integers and records the stamp's path parameter. A
 * present between the snapshots of two such stamps shows the path curve
 * (Catmull-Rom through the waypoints as written, linear across a "cut") at
 * the parameter the bracket's alpha reaches between them. Each snapshot is
 * matched to its stamp by VBL and stamped position. Writes no guest memory.
 * @param prev_tick The earlier snapshot's VBL.
 * @param prev_pos The earlier snapshot's camera position.
 * @param curr_tick The later snapshot's VBL.
 * @param curr_pos The later snapshot's camera position.
 * @param alpha The bracket's blend factor, 0 at the earlier snapshot.
 * @param pose Receives x, y, z, yaw, pitch, roll in stored units, unrounded.
 * @param prev_ang Receives the yaw, pitch and roll stamped for the earlier
 *        snapshot, the angles its matrices were built from.
 * @return 1 when @p pose is set; 0 with no fractional path engaged, a
 *         snapshot no stamp matches, or a pair more than two waypoints apart. */
int freecam_present_pose(uint32_t prev_tick, const int32_t prev_pos[3],
                         uint32_t curr_tick, const int32_t curr_pos[3],
                         double alpha, double pose[6], int32_t prev_ang[3]);

/** @brief Parse the numbers of --freecam-pose text: x, y, z, then yaw and pitch when present.
 *
 * The text reads as --freecam-pose's does ("pos=(x, y, z) yaw=Y pitch=P"
 * or "x,y,z,yaw,pitch"); a named pose resolves to nothing here.
 * @param text The pose text, or NULL.
 * @param v Receives {x, y, z, yaw, pitch}, 0 where absent.
 * @return The numbers parsed. */
int freecam_pose_numbers(const char* text, int32_t v[5]);

/** @brief The unit rotation Rz(roll)·Rx(pitch)·Ry(yaw) of a pose, in the game's matrix forms.
 *
 * The view the free camera's stamp composes under every known builder
 * order; angles in stored units, 4096 per turn, fractions allowed.
 * @param yaw The yaw.
 * @param pitch The pitch.
 * @param roll The roll about the view axis.
 * @param R Receives the matrix, unit scale. */
void freecam_view_rotation(double yaw, double pitch, double roll, double R[3][3]);

/** @brief Engage the free camera once from OPENPETE_FREECAM, OPENPETE_FREECAM_POSE or OPENPETE_FREECAM_PATH.
 *
 * Called once per tick from the game's on_frame_pre; acts at most once per
 * process. Waits while a scripted player placement is pending, then for the
 * VBL OPENPETE_FREECAM_AT_VBL names when set, then for a live level; engaging
 * during a load would freeze the player update while the level sets up. A
 * timed engage passes a replaying bk2's refusal. A path, when given, is parsed
 * and supersedes the pose; a pose is queued for the drive to stamp at the
 * follow's tick point. Nothing happens when no variable is set or the free
 * camera is already engaged. Interactive toggling is independent of this.
 * @param cpu The guest CPU state. */
void freecam_maybe_autoengage(CPUState* cpu);

/** @brief Place the player once from OPENPETE_PLAYER_POSE ("x,y,z[,yaw]", set by --player-pose).
 *
 * Fires on the first tick player_placeable holds, then never again. An "@N"
 * suffix defers it to the first such tick whose feed-key value
 * (openpete_feed_key_now) is at least N, which serves a level whose intro
 * re-seats the player after an early placeable gap; a suffix that is not a
 * non-negative integer is logged and nothing is placed. A spec
 * with other than three or four integers is logged and not applied. The
 * placement writes the position and previous position, the facing when a yaw
 * is given, cancels momentum, and re-seats the follow camera. Called once
 * per tick from the game's on_frame_pre before freecam_maybe_autoengage, which
 * waits for it, so a free-camera view of the placed player needs no second
 * run.
 * @param cpu The guest CPU state. */
void freecam_player_pose_maybe_apply(CPUState* cpu);
