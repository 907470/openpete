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

/* ---- second model kind ------------------------------------------------
 * A game whose moby set includes a model kind its own draw pass owns
 * (game_layout.moby.hook_draw_simple) decodes that kind inside
 * moby_game.simple_pass and hands the result back through these two: one call
 * per record to name the moby the following triangles belong to, then one per
 * triangle with its three vertices already in VIEW SPACE and FULL WORLD
 * UNITS. The projection, the GTE near-plane mirror, the per-face identity
 * tagging and the scene push stay with the shared walk, so the second kind
 * lands in the same scene category and the same depth space as the animated
 * one. Valid only from inside simple_pass. */
void moby_simple_record(struct CPUState* cpu, uint32_t moby);
/* The screen position of one view-space point under the pass's view, so a
 * provider can run its own winding test on the same coordinates the emit
 * projects with. */
void moby_simple_project(const double p[3], float* sx, float* sy);
void moby_simple_tri(const double p[3][3], const uint32_t rgb[3],
                     const int u[3], const int v[3],
                     int textured, int tpage_x, int tpage_y,
                     int clut_x, int clut_y, int bpp, int blend,
                     uint32_t fkey);

/* The cull-record queue the last canonical moby pass read: its guest base and
 * its length in bytes. A game whose queue lives in memory the rest of the tick
 * reuses stages these bytes for each sub-tick present, so the replay reads the
 * queue the tick drew rather than whatever overwrote it. 0 = no queue read yet,
 * or the game keeps its queue at a fixed address that nothing else touches. */
uint32_t moby_canon_rec_base(void);
uint32_t moby_canon_rec_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_MOBY_H */
