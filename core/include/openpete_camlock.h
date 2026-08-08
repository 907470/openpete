#pragma once
/* openpete_camlock.h — host-side state for the render camera lock (G key).
 *
 * The lock is the PRESENTATION-side sibling of the gameplay freecam (F key).
 * The freecam freezes the game's camera-follow / player-update functions and
 * writes g_Camera on the game fiber — an intentional gameplay-state
 * divergence, and inert during cutscenes (whose cameras never run the
 * follow function it intercepts). The lock instead overrides the camera
 * INSIDE the sub-tick re-extract bracket: the bytes are saved, overwritten
 * for extraction only, and restored before the sim resumes, so gameplay
 * state is byte-identical whether the lock is on or off (the bracket-
 * integrity self-check covers the writes). That makes it the tool for
 * render-only inspection: a cutscene or flyby keeps playing underneath
 * while the view holds still or flies free — e.g. parking a close-up on an
 * animated moby to judge sub-pixel flicker that is invisible from the
 * authored camera distance.
 *
 * This is the GENERIC half: the on/off toggle, the flown pose (world
 * position + game angle units, 4096 per turn), and pose integration from
 * the freecam's shared input axes (WASD/sticks — one set of fly controls
 * for both cameras; run.c keeps the two mutually exclusive). The
 * game-specific half — latching the initial pose from the live camera and
 * writing the bracketed g_Camera bytes/matrices — lives in the game's
 * re-extract orchestrator.
 *
 * Scripted use: OPENPETE_CAMLOCK=1 engages at the first present;
 * OPENPETE_CAMLOCK_POSE="x,y,z,yaw,pitch" engages AND flies to that pose
 * (the numbers round-trip through the change-gated "camlock:" log line),
 * so a headless capture reproduces an eyeballed view exactly.
 */

#include <stdbool.h>

/* True while the render camera lock is engaged. */
bool openpete_camlock_active(void);

/* Toggle engaged/disengaged (the G-key edge). Engaging without a scripted
 * pose defers the pose latch to the game side's first apply (it needs the
 * live camera); disengaging simply stops overriding — the bracket restore
 * already returns every present's bytes. */
void openpete_camlock_toggle(void);

/* True once the flown pose holds real values (scripted pose, or latched by
 * the game side). While false the game side must derive one before flying. */
bool openpete_camlock_pose_valid(void);

/* Pose accessors. Position is world units (game ints, double for sub-unit
 * fly precision); yaw/pitch are game angle units (4096 = full turn). */
void openpete_camlock_set_pose(double x, double y, double z,
                               double yaw, double pitch);
void openpete_camlock_get_pose(double* x, double* y, double* z,
                               double* yaw, double* pitch);

/* True if any fly input (or a scripted pose) has moved the pose since the
 * latch — the game side holds the latched camera bytes verbatim until then,
 * so engage-and-don't-touch is an EXACT hold of the on-screen view. */
bool openpete_camlock_moved(void);

/* Advance the pose from the latched freecam axes. dt_ticks is the elapsed
 * time in sim ticks (29.97 Hz) since the previous call, so fly speed matches
 * the freecam's per-tick steps at any present rate. */
void openpete_camlock_integrate(double dt_ticks);
