/* renderer/moby.h — the shared moby (3D actor) feeder's entries.
 *
 * The walk lives in games/common/renderer/moby.c and reads every engine
 * address from game_layout.moby; the per-game stream formats and the data a
 * game keeps outside those addresses come through moby_game
 * (renderer/moby_game.h). A game reaches the feeder by listing moby_module in
 * its feature list; a game whose layout names no draw hook registers nothing
 * and the walk never runs. */
#ifndef OPENPETE_COMMON_RENDERER_MOBY_H
#define OPENPETE_COMMON_RENDERER_MOBY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Sub-tick moby ROTATION interpolation toggle. When on, the moby euler
 * composer renders each moby's rotation at FLOAT precision — shortest-arc
 * interpolating the byte euler (256/circle) between the moby's prev/curr
 * interp-objs snapshots at its per-object alpha, with the sin/cos sampled by
 * linear interpolation of the in-RAM table — instead of the byte-quantized
 * angle. Removes the 1.4°/step stair-stepping on slowly-rotating mobys (gems,
 * idle dragons, HUD icons). Set by the orchestrator around the feeder
 * re-extract; reset (off) so the canonical inline render stays byte-exact. */
void moby_native_set_rot_interp(int on);

/* Occlusion-cull pan allowance: the camera's rotation this tick expressed as
 * screen-pixel motion (rad/tick × ~px-per-radian, overestimated). The feeder
 * inflates cached screen rects by it per tick of cache age, so a pan cannot
 * cull a moby whose stale rect has swept toward a wall edge. */
void moby_native_set_occl_pan(float px_per_tick);

/* The mobys the canonical render passes drew this tick, taken from their cull
 * records (deduped across the material passes). Most are also reachable by
 * walking the moby array, but not all: a game may relocate a moby into render
 * scratch and draw it from there, and anything drawn from outside the array
 * has no prev/curr snapshot and no fractional position — it steps at the sim
 * rate while everything around it interpolates. Both the interp capture and
 * the sub-tick lerp walk consult this list so that class is covered
 * generally. Returns the entry count and points *out at the (borrowed)
 * array. */
int moby_native_canon_drawn_mobys(const uint32_t** out);

/* Sub-tick re-extract entry: rebuild this present's moby contribution from
 * the captured cull records and the interpolated camera. */
void moby_reextract(struct CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_MOBY_H */
