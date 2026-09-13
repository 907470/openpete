#pragma once
/* openpete_freecam.h — host-side state for the debug free camera (F key).
 *
 * This is the GENERIC half: it owns the on/off toggle, the per-axis input
 * intent latched from the keyboard each host iteration, and the discrete
 * speed step. It carries NO Spyro addresses or camera-struct knowledge, so
 * it lives in core alongside the other host input handling (run.c). The
 * game-specific half — reading g_Camera, applying the motion, and freezing
 * the game's camera-follow / player-update functions while active — lives in
 * games/common/freecam_game.c behind a per-game descriptor
 * (games/<game>/features/freecam.c) and reads this state each tick.
 *
 * Ported from C0mposer's Spyro-1-Practice-Rom (src/freecam.c): freeze the
 * follow logic, then drive the camera position/angle directly. Debug tool —
 * OFF by default (zero effect, oracle-clean); ON is an intentional,
 * opt-in gameplay-state divergence for inspecting a level.
 */

#include <stdbool.h>

/* Discrete speed steps (slowest → fastest). The per-step move/turn magnitudes
 * live game-side (the freecam descriptor), keyed by this index. */
#define OPENPETE_FREECAM_SPEED_STEPS 5

/* Per-axis movement/look intent, each component a signed magnitude in
 * [-1, +1], level-held (set from the live keyboard + pad every host
 * iteration, consumed once per game tick). Keyboard drives full ±1; an
 * analog stick drives proportionally, so the two sum (then clamp). Positive
 * senses are the intuitive ones; the game-side drive maps them onto the
 * camera's angle convention.
 *
 *   axis         keyboard          controller
 *   move_fwd     W / S             left stick Y
 *   move_strafe  D / A             left stick X
 *   move_up      Space / LShift    R1 / L1
 *   look_yaw     → / ←             right stick X
 *   look_pitch   ↑ / ↓             right stick Y
 *
 * T (teleport the player to the camera) is not an axis — it's a one-shot
 * request; see openpete_freecam_request_teleport below.
 */
typedef struct openpete_freecam_axes {
    float move_fwd;     /* + forward,   - back   */
    float move_strafe;  /* + right,     - left   */
    float move_up;      /* + up,        - down   */
    float look_yaw;     /* + turn right, - turn left */
    float look_pitch;   /* + look up,    - look down */
} openpete_freecam_axes;

/* True while the free camera is engaged. */
bool openpete_freecam_active(void);

/* Toggle engaged/disengaged (the F-key edge). Clears the latched axes so a
 * key held across the toggle doesn't carry stale intent into the new state.
 * Returns false when the engage is refused: while a bk2 movie is replaying
 * or recording, a free camera would freeze the player update and desync the
 * movie from its inputs, so the off→on edge is refused with a WARN; the
 * on→off edge always passes. */
bool openpete_freecam_toggle(void);

/* True while a bk2 movie is replaying or recording (set by the run loop). */
bool openpete_bk2_active(void);
void openpete_bk2_set_active(bool active);

/* Latch this iteration's input intent (run.c → module). */
void openpete_freecam_set_axes(const openpete_freecam_axes* ax);

/* Read the latched input intent (module → game drive). */
void openpete_freecam_get_axes(openpete_freecam_axes* out);

/* Mouse look: running totals of the look axes' mouse displacement, in
 * counts, accumulated by the host loop every iteration the freecam or
 * camlock is engaged (run.c → module). Unlike the axes they are never
 * rewritten: each consumer keeps the total it last applied and takes the
 * difference, so a slow drag below one unit per tick still adds up and a
 * speculated or replayed tick applies nothing twice. */
void openpete_freecam_add_look_counts(long long dx, long long dy);
void openpete_freecam_look_counts(long long* x, long long* y);

/* Mouse look scale: angle units (a full turn is 4096) per count at
 * sensitivity 1.0. 0.5 is one full turn per 8192 counts, about 26 cm at
 * 800 counts per inch. A consumer applies
 * round(OPENPETE_FREECAM_MOUSE_K * sensitivity * (total - base)). */
#define OPENPETE_FREECAM_MOUSE_K 0.5

/* --mouse-sensitivity / mouse_sensitivity, live. Consumers read it at every
 * apply and rebase when it changed, so a change never rotates
 * retroactively. */
void  openpete_freecam_set_mouse_sensitivity(float s);
float openpete_freecam_mouse_sensitivity(void);

/* Engage generation: bumped by every toggle. A consumer compares it at its
 * own apply site and rebases its mouse markers when it differs, so an
 * engage from any thread (key binding, O menu row, M overlay row, env pose)
 * starts the mouse term from the totals as they stand then. */
unsigned openpete_freecam_engage_generation(void);

/* Request that the player be moved to the free camera's position (the T-key
 * edge). Only meaningful while engaged. The host loop latches the request;
 * the game side consumes it on the next tick — it needs the game fiber to
 * read the camera and write the player, and it disengages the free camera as
 * part of applying it. Ignored (and dropped) if the camera is disengaged
 * before the request is taken. */
void openpete_freecam_request_teleport(void);

/* Consume a pending teleport request: true once per request (game → drive). */
bool openpete_freecam_take_teleport_request(void);

/* Current speed step, clamped to [0, OPENPETE_FREECAM_SPEED_STEPS). */
int openpete_freecam_speed_index(void);

/* Step the speed up (dir > 0) or down (dir < 0), clamped. */
void openpete_freecam_adjust_speed(int dir);
