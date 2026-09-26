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
#include "renderer/moby_game.h"   /* moby_wface_t: the shared face record */
#include "renderer/moby_rank.h"   /* paint-key records and vertices */
#include "renderer/native_gte.h"  /* ngte_euler_sc: the composed rotation's sin/cos */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/** @brief Sub-tick moby rotation interpolation toggle.
 *
 * When on, the moby euler composer renders each moby's rotation at float
 * precision — shortest-arc interpolating the byte euler (256/circle) between
 * the moby's prev/curr interp-objs snapshots at its per-object alpha, with the
 * sin/cos sampled by linear interpolation of the in-RAM table — instead of the
 * byte-quantized angle. Removes the 1.4°/step stair-stepping on slowly-rotating
 * mobys (gems, idle dragons, HUD icons). Set by the orchestrator around the
 * feeder re-extract; reset (off) so the canonical inline render stays
 * byte-exact. Turning it off after a sub-tick present emits that present's
 * OPENPETE_MOBY_CENSUS `pose_snap` line.
 * @param on 1 to open a sub-tick present's feeder pass, 0 to close it */
void moby_native_set_rot_interp(int on);

/* The interpolation alpha of the present being re-extracted and whether the
 * camera bracket ran: alpha 0 shows the previous canonical tick, 1 the latest;
 * an unbracketed present shows the latest and reads as alpha 1. The sub-tick
 * carry of the same-moby paint verdict applies the previous tick's verdict
 * below 0.5 and the latest tick's from 0.5. Set by the orchestrator before
 * moby_reextract and reset to (1, 1) after. */
void moby_native_set_verdict_alpha(double alpha, int bracketed);

/* The camera the cull composed this tick's records against, for a game whose
 * cull stores view-space records (moby_game.host_reveal == 0): the interp
 * ring's curr snapshot, nine int16 matrix entries in GTE row order (1.0 =
 * 4096) and the three camera position words. With it the sub-tick record
 * replay re-bases each record onto the camera the present stages, so an
 * animated moby follows the interpolated view like every other feeder instead
 * of holding its tick pose in screen space. NULL/NULL clears it and the replay
 * leaves every record exactly as the cull wrote it. Set around the feeder
 * re-extract by the orchestrator; ignored by a game that rebuilds records
 * host-side. */
void moby_native_set_tick_camera(const int16_t mat[9], const int32_t pos[3]);

/* The installed tick camera position, for a cull whose records bake the camera
 * position into a stored per-record delta: the delta is re-based onto the
 * staged camera by the difference between the staged position and this one.
 * Returns 0 and leaves out[] untouched when no tick camera is installed, which
 * is every canonical inline render. */
int moby_native_tick_camera_pos(int32_t out[3]);

/* The horizontal widening of the pass view against the game's own 4:3 frame:
 * Hor+ multiplies the frame's half-width by F, and a --fov scale s divides
 * the projection H by s, which widens it by the same ratio. 1.0 at 4:3 with
 * the game's FOV, so a test scaled by it is bit-identical there.
 *
 * A decode that repeats one of the draw's lateral cone tests scales that
 * test's slope by this, since the draw's slope is the 4:3 half-angle and the
 * frame on screen is wider. The vertical arm takes no factor: Hor+ keeps the
 * vertical FOV. Valid from the pass view's build at the pass entry. */
double moby_native_view_widen(void);

/* ---- the present's own-motion terms, shared by both model kinds ---------
 * A cull record encodes the pose the moby held at the tick; the present shows
 * the moby part-way to it. These two carry the object's own movement over that
 * fraction, so a record placed from the tick's values steps and one that adds
 * them sweeps. Both read the per-object prev/curr snapshot pair and both return
 * the identity when there is none, so a record without a pair, a slot that
 * changed class between the two states and every canonical render stay exactly
 * as the cull wrote them.
 *
 * moby_record_world_offset fills out[] with the moby's world-position offset in
 * world units, (prev - curr) * (1 - t) at the present's phase t. Its axis order
 * is the moby's own position words; a caller in another convention maps the
 * components itself. Returns 0 with out[] zeroed when there is no usable pair,
 * including a stale one, where a multi-tick gap is a discontinuity rather than
 * motion. Returns 1 with a zero out[] for a moby static in the world.
 *
 * moby_record_halfword_offset does the same for the unsigned 16-bit field at
 * byte offset `off` inside the captured record, for a caller whose own
 * convention is not the position words: *out is (prev - curr) * (1 - t).
 * Returns 0 with *out zeroed when there is no usable pair, when `off` lies
 * past the captured blob, or when either state reads 0, which every such
 * field uses as its absent value and which would otherwise sweep the object
 * toward an origin it never held.
 *
 * moby_record_euler_at_present fills *e with the moby's three euler axes at the
 * present's phase. Returns 0 and leaves *e untouched when the caller must use
 * the byte-exact table lookup instead. */
int moby_record_world_offset(uint32_t moby, double out[3]);
int moby_record_halfword_offset(uint32_t moby, uint32_t off, double* out);
int moby_record_euler_at_present(struct CPUState* cpu, uint32_t moby,
                                 ngte_euler_sc* e);

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
/* Whether the second model kind has a queue this present may replay: the game
 * provides the kind, the canonical present ran its draw, and that draw's epoch
 * is the committed one. The queue is a host copy rather than a staged view of
 * the tick's draw scratch, so it replays independently of the animated queue's
 * staging: a state whose tick ran one of the two draws replays that one, and a
 * state that ran neither still replays nothing. */
int moby_native_simple_fresh(void);

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
/* A record whose model the game places in screen space rather than in the
 * world: the projection origin, in PSX pixels, that the following triangles of
 * this record project through, in place of the view's own. The record is
 * tagged HUD3D by the call. Called after moby_simple_record, which clears it,
 * so a world-placed record projects exactly as it did. */
void moby_simple_screen_origin(double ofx, double ofy);
/* The screen position of one view-space point under the pass's view, so a
 * provider can run its own winding test on the same coordinates the emit
 * projects with. */
void moby_simple_project(const double p[3], float* sx, float* sy);
/* `otk` is the triangle's bucket word in the record's paint order
 * (moby_rank.h: the face's moby_otk_face_bucket, MOBY_OTK_Q2 set on the
 * second triangle of one prim), -1 for none; `iv` the draw's integer RTPS of
 * the three vertices (moby_simple_otk_vertex), or NULL. */
void moby_simple_tri(const double p[3][3], const uint32_t rgb[3],
                     const int u[3], const int v[3],
                     int textured, int tpage_x, int tpage_y,
                     int clut_x, int clut_y, int bpp, int blend,
                     uint32_t fkey, int32_t otk, const moby_otk_vert_t* const iv[3]);
/* The paint key of the current record under `row`, with the TRZ the draw
 * loaded: row MOBY_OTK_NONE while the rank is off or the present carries the
 * tick's verdict. */
void moby_simple_otk_record(moby_otk_row_t row, int trz, moby_otk_rec_t* out);
/* One model-space vertex's integer RTPS under rotation R and translation tr,
 * through the current record's view; nothing when `r` carries no key. */
void moby_simple_otk_vertex(const moby_otk_rec_t* r, const int tr[3], const int R[3][3],
                            double vx, double vy, double vz, moby_otk_vert_t* out);

/* ---- extended-LOD reveal for a game with its own record encoding -------
 * Walks the moby array and offers every moby the canonical cull's distance
 * box rejected to moby_game.reveal_record, which appends a record for it in
 * the game's own encoding. The admission the walk applies before offering a
 * moby is the cull's own, widened only where extended LOD says so: the array
 * sentinel and dead states, the run-once classification, the cull's distance
 * box through moby_game.cull_box_threshold (which the extended-LOD clause
 * lifts), the camera plane, the view window and the occlusion test. A moby
 * the canonical box still admits is left to the cull, which either queued it
 * or rejected it for a reason the box does not express.
 *
 * Returns the number of records appended. Reads gamestate only. Call from
 * inside the pass whose queue the game appends to, once per model kind, after
 * that pass has captured the cull's own records and before it decodes them.
 * Inert unless extended LOD is on and the game supplies both slots. */
int moby_reveal_candidates(struct CPUState* cpu, moby_reveal_kind_t kind);

/* ---- decode pieces shared with the player feeder -----------------------
 * The avatar draw and the moby draw are different routines, but they encode a
 * face the same way and run the same emit tree, so these four live here once.
 *
 * moby_tri_nclip is the signed 2*area of a screen-space triangle, the NCLIP
 * analog, on float coordinates rather than the GTE's floored int16.
 *
 * moby_abr_blend maps a texture page's semi-transparency mode to the
 * renderer's blend id: ABR 0 average, 1 additive, 2 subtractive, 3
 * quarter-additive.
 *
 * moby_tiledef_decode fills one face record's texture fields from the three
 * texture words {u0|v0<<8|clut<<16}, {u1|v1<<8|tpage<<16},
 * {u2|v2<<8|u3<<16|v3<<24}. A tpage colour-depth field of 3 is reserved on the
 * hardware and marks a replaced texture instead: the record takes depth 32 and
 * carries the texture id where the page x would go.
 *
 * moby_face_emit is the dual-NCLIP tree. A FRESH (quad-capable) face runs
 * NCLIP1 on (v0,v1,v2) and NCLIP2 on (v3,v1,v2) and emits, by the sign pair,
 * the triangle (v3,v1,v2), nothing, the quad (v0,v1,v2)+(v1,v3,v2), or the
 * triangle (v0,v1,v2); a STRIP face is one triangle kept on NCLIP1's sign. The
 * face's own bit0 changes both rules, and the two draws differ in how: with
 * `bit0_unconditional` the bit skips the test outright (a pre-marked quad, a
 * strip of either winding), and without it the bit negates the two NCLIP signs
 * before the same tests, which differs from the first only on a zero-area
 * face. Returns the triangle count and fills order[e] with the three
 * face-vertex slots (0..3) of triangle e. */
double moby_tri_nclip(float ax, float ay, float bx, float by, float cx, float cy);
int    moby_abr_blend(int abr);
void   moby_tiledef_decode(uint32_t td0, uint32_t td1, uint32_t td2, moby_wface_t* F);
int    moby_face_emit(int fresh, int bit0, int bit0_unconditional,
                      double ncl1, double ncl2, int have_v3, int order[2][3]);

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
