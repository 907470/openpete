/* renderer/moby.h - the shared moby (3D actor) feeder's entries.
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
 * precision - shortest-arc interpolating the byte euler (256/circle) between
 * the moby's prev/curr interp-objs snapshots at its per-object alpha, with the
 * sin/cos sampled by linear interpolation of the in-RAM table - instead of the
 * byte-quantized angle. Removes the 1.4°/step stair-stepping on slowly-rotating
 * mobys (gems, idle dragons, HUD icons). Set by the orchestrator around the
 * feeder re-extract; reset (off) so the canonical inline render stays
 * byte-exact. Turning it off after a sub-tick present emits that present's
 * OPENPETE_MOBY_CENSUS `pose_snap` line.
 * @param on 1 to open a sub-tick present's feeder pass, 0 to close it */
void moby_native_set_rot_interp(int on);

/** @brief Set the interpolation alpha of the present being re-extracted and whether the camera bracket ran.
 *
 * Alpha 0 shows the previous canonical tick, 1 the latest; an unbracketed
 * present shows the latest and reads as alpha 1. The sub-tick carry of the
 * same-moby paint verdict applies the previous tick's verdict below 0.5 and
 * the latest tick's from 0.5. Set by the orchestrator before moby_reextract
 * and reset to (1, 1) after.
 * @param alpha The present's interpolation alpha.
 * @param bracketed 1 when the camera bracket ran, else 0. */
void moby_native_set_verdict_alpha(double alpha, int bracketed);

/** @brief Install the camera the cull composed this tick's records against, or clear it with NULL.
 *
 * For a game whose cull stores view-space records (moby_game.host_reveal ==
 * 0): the interp ring's curr snapshot, nine int16 matrix entries in GTE row
 * order (4096 = 1.0) and the three camera position words. With it the
 * sub-tick record replay re-bases each record onto the camera the present
 * stages, so an animated moby follows the interpolated view like every other
 * feeder instead of holding its tick pose in screen space. A NULL argument
 * clears it and the replay leaves every record as the cull wrote it. The
 * orchestrator sets it around the feeder re-extract; a game that rebuilds
 * records host-side does not read it.
 * @param mat The matrix, or NULL.
 * @param pos The camera position, or NULL. */
void moby_native_set_tick_camera(const int16_t mat[9], const int32_t pos[3]);

/** @brief The installed tick camera position, for a cull whose records bake the camera position into a stored per-record delta.
 *
 * The delta is re-based onto the staged camera by the difference between the
 * staged position and this one.
 * @param out Receives the three camera position words when a tick camera is
 *        installed; untouched otherwise.
 * @return 1 when a tick camera is installed; 0 otherwise, which is every
 *         canonical inline render. */
int moby_native_tick_camera_pos(int32_t out[3]);

/** @brief The horizontal widening of the pass view against the game's own 4:3 frame.
 *
 * The value is the view's F / s: Hor+ multiplies the frame's half-width by F, and a --fov scale s divides
 * the projection H by s, which widens it by the same ratio. 1.0 at 4:3 with
 * the game's FOV, so a test scaled by it is bit-identical there.
 *
 * A decode that repeats one of the draw's lateral cone tests scales that
 * test's slope by this, since the draw's slope is the 4:3 half-angle and the
 * frame on screen is wider. The vertical arm takes no factor: Hor+ keeps the
 * vertical FOV. Valid from the pass view's build at the pass entry.
 * @return The widening factor. */
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
 * motion. Returns 1 with a zero out[] for a moby static in the world. */
/** @brief The moby's world-position offset at the present's phase t, (prev - curr) * (1 - t) in world units.
 *
 * Its axis order is the moby's own position words; a caller in another
 * convention maps the components itself.
 * @param moby The Moby address.
 * @param out Receives the offset; zeroed when there is no usable pair.
 * @return 0 when there is no usable prev/curr pair, including a stale one,
 *         where a multi-tick gap is a discontinuity rather than motion; 1
 *         otherwise, with a zero out[] for a moby static in the world. */
int moby_record_world_offset(uint32_t moby, double out[3]);
/** @brief The (prev - curr) * (1 - t) offset of one captured unsigned 16-bit moby field at the present's phase t.
 *
 * For a caller whose own convention is not the position words. The field is
 * read at byte offset off inside the object's captured record.
 * @param moby The Moby address.
 * @param off The field's byte offset inside the captured record.
 * @param out Receives the offset; zeroed when 0 is returned.
 * @return 0 when there is no usable prev/curr pair (as for
 *         moby_record_world_offset), when off lies past the captured blob, or
 *         when either state reads 0, which every such field uses as its
 *         absent value and which would otherwise sweep the object toward an
 *         origin it never held; 1 otherwise. */
int moby_record_halfword_offset(uint32_t moby, uint32_t off, double* out);
/** @brief The moby's three euler axes at the present's phase, as GTE sin/cos pairs.
 *
 * Each axis is interpolated by the shortest arc between the object's prev and
 * curr byte angles and sampled out of the in-RAM sine table by linear
 * interpolation, so a rotation that steps one byte per tick sweeps instead.
 * Z is byte +0 of the angle word, X +1 and Y +2; its high byte is an overlay
 * flag and no axis. A facing reversal (moby_mirror_hold) holds the previous
 * tick's angles until the hold expires, then takes this tick's.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param e Receives the sin/cos pairs; left untouched when 0 is returned.
 * @return 0 when the caller must use the byte-exact table lookup: rotation
 *         interp is off (every canonical render), there is no pair, the pair
 *         has no distinct prev, or the two states hold different classes,
 *         where a reused slot would lerp across two objects; 1 otherwise. */
int moby_record_euler_at_present(struct CPUState* cpu, uint32_t moby,
                                 ngte_euler_sc* e);

/** @brief Set the occlusion cull's pan allowance: the camera's rotation this tick as screen-pixel motion.
 *
 * The value is rad/tick times an overestimated pixels-per-radian. The feeder
 * inflates a cached screen rect by it, scaled by the view's H scale, per tick
 * of cache age, so a pan cannot cull a moby whose stale rect has swept toward
 * a wall edge.
 * @param px_per_tick The allowance in pixels per tick at the canonical H. */
void moby_native_set_occl_pan(float px_per_tick);

/** @brief The mobys the canonical render passes drew this tick, taken from their cull records and deduplicated across the material passes.
 *
 * Most are also reachable by walking the moby array, but not all: a game may
 * relocate a moby into render scratch and draw it from there, and a moby
 * drawn from outside the array has no prev/curr snapshot and no fractional
 * position, so it would step at the tick rate while everything around it
 * interpolates. The interp capture and the sub-tick lerp walk consult this
 * list so that class is covered generally. A set older than two VBLs dates
 * from before a stall (a CD load or level swap) and reports no entries, since
 * its addresses may name other objects.
 * @param out Receives the borrowed array.
 * @return The entry count. */
int moby_native_canon_drawn_mobys(const uint32_t** out);

/** @brief Sub-tick re-extract entry: rebuild this present's moby contribution from the captured cull records and the interpolated camera.
 *
 * Inert while the native scene is off or the canonical present did not draw
 * the moby render path. Each material pass (regular, specular, second model
 * kind, shaded) replays only when its own producer ran in the committed
 * canonical epoch, so a state that ran a subset of the draws replays that
 * subset. The paint key is not computed while the rank carry applies the
 * tick's verdict.
 * @param cpu The guest CPU state. */
void moby_reextract(struct CPUState* cpu);
/** @brief Whether the second model kind has a queue this present may replay.
 *
 * True when the game provides the kind's replay (moby_game.simple_replay),
 * the canonical present ran its draw, and that draw's epoch is the committed
 * one. The queue is a host copy rather than a staged view of
 * the tick's draw scratch, so it replays independently of the animated queue's
 * staging: a state whose tick ran one of the two draws replays that one, and a
 * state that ran neither still replays nothing.
 * @return 1 when the queue may replay, else 0. */
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
/** @brief Name the moby the following second-kind triangles belong to.
 *
 * Sets the current record's moby, class, HUD tag (static or UI address
 * range) and cutscene-actor front flag, and clears the screen-space origin
 * and the per-face emit keys.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address, or 0. */
void moby_simple_record(struct CPUState* cpu, uint32_t moby);
/** @brief Set the projection origin of a record whose model the game places in screen space rather than in the world.
 *
 * The following triangles of this record project through this origin, in PSX
 * pixels, under the canonical view, which keeps the game's projection
 * distance, in place of the view's own origin. The record is tagged HUD3D by
 * the call. Called after moby_simple_record, which clears it, so a
 * world-placed record projects through its view's origin.
 * @param ofx The origin X, PSX pixels.
 * @param ofy The origin Y, PSX pixels. */
void moby_simple_screen_origin(double ofx, double ofy);
/** @brief The screen position of one view-space point under the current record's view.
 *
 * A provider runs its own winding test on the same coordinates the emit
 * projects with: the raw float projection with the GTE near-plane mirror.
 * @param p The view-space point, full world units.
 * @param sx Receives the screen X.
 * @param sy Receives the screen Y. */
void moby_simple_project(const double p[3], float* sx, float* sy);
/** @brief Project and emit one second-kind triangle given in view space, full world units.
 *
 * Each vertex takes the raw float projection and the GTE near-plane mirror
 * under the current record's view, then the triangle goes through the shared
 * emit with the animated walk's tagging.
 * @param p The three view-space vertices.
 * @param rgb The three vertex colours.
 * @param u The three texel U coordinates.
 * @param v The three texel V coordinates.
 * @param textured 1 when the triangle is textured.
 * @param tpage_x The texture page X, VRAM pixels.
 * @param tpage_y The texture page Y, VRAM pixels.
 * @param clut_x The CLUT X, VRAM pixels.
 * @param clut_y The CLUT Y, VRAM pixels.
 * @param bpp The texture depth.
 * @param blend 0 = opaque, else the blend id moby_abr_blend maps.
 * @param fkey The face identity key.
 * @param otk The triangle's bucket word in the record's paint order
 *        (moby_rank.h: the face's moby_otk_face_bucket, MOBY_OTK_Q2 set on the
 *        second triangle of one prim), -1 for none.
 * @param iv The draw's integer RTPS of the three vertices
 *        (moby_simple_otk_vertex), or NULL. */
void moby_simple_tri(const double p[3][3], const uint32_t rgb[3],
                     const int u[3], const int v[3],
                     int textured, int tpage_x, int tpage_y,
                     int clut_x, int clut_y, int bpp, int blend,
                     uint32_t fkey, int32_t otk, const moby_otk_vert_t* const iv[3]);
/** @brief The paint key of the current record under a row, with the TRZ the draw loaded.
 *
 * The row becomes MOBY_OTK_NONE while the rank is off or the present carries
 * the tick's verdict.
 * @param row The record's row.
 * @param trz The GTE TRZ the draw loaded.
 * @param out Receives the paint key. */
void moby_simple_otk_record(moby_otk_row_t row, int trz, moby_otk_rec_t* out);
/** @brief One model-space vertex's integer RTPS under rotation R and translation tr, through the current record's view.
 *
 * Nothing is written when r carries no key. The projection distance is the
 * view's canonical H.
 * @param r The record's paint key.
 * @param tr The integer translation.
 * @param R The rotation, 4096 = 1.0.
 * @param vx The model-space vertex X.
 * @param vy The model-space vertex Y.
 * @param vz The model-space vertex Z.
 * @param out Receives SZ3 and SXY2. */
void moby_simple_otk_vertex(const moby_otk_rec_t* r, const int tr[3], const int R[3][3],
                            double vx, double vy, double vz, moby_otk_vert_t* out);

/** @brief Offer the mobys the canonical cull rejected to moby_game.reveal_record, which appends records in the game's own encoding.
 *
 * Walks the moby array, skipping the sentinel, dead states and run-once
 * instances, and applies two clauses. The widened-view clause (animated kind,
 * moby_game.cull_view_admits supplied, view widening above 1) offers a moby
 * the cull left undrawn that the cull's admission admits with the lateral
 * slope scaled to the view. The extended-LOD clause (--no-lod) offers a moby
 * outside the cull's distance box (moby_game.cull_box_threshold) that stands
 * in front of the camera plane. Both clauses skip a moby the occlusion test
 * culls. A moby the canonical box still admits is left to the cull, which
 * either queued it or rejected it for a reason the box does not express.
 *
 * Reads gamestate only. Call from inside the pass whose queue the game
 * appends to, once per model kind, after that pass has captured the cull's
 * own records and before it decodes them. Inert unless the game supplies
 * cull_box_threshold and reveal_record and one of the two clauses applies.
 * @param cpu The guest CPU state, read only.
 * @param kind The model kind whose queue the records join.
 * @return The number of records appended. */
int moby_reveal_candidates(struct CPUState* cpu, moby_reveal_kind_t kind);

/* ---- decode pieces shared with the player feeder -----------------------
 * The avatar draw and the moby draw are different routines, but they encode a
 * face the same way and run the same emit tree, so these four live here
 * once. */
/** @brief The signed doubled area of a screen-space triangle, the GTE NCLIP analog on float coordinates rather than the GTE's floored int16.
 *
 * (b - a) x (c - a), the sign convention NCLIP uses; the caller reads the
 * sign as the facing.
 * @param ax Vertex a's screen X.
 * @param ay Vertex a's screen Y.
 * @param bx Vertex b's screen X.
 * @param by Vertex b's screen Y.
 * @param cx Vertex c's screen X.
 * @param cy Vertex c's screen Y.
 * @return The signed doubled area. */
double moby_tri_nclip(float ax, float ay, float bx, float by, float cx, float cy);
/** @brief Map a texture page's semi-transparency mode to the renderer's blend id.
 *
 * ABR 0 is average (id 1), 1 additive (2), 2 subtractive (3) and 3
 * quarter-additive (4), the hardware's own table.
 * @param abr The mode; only its low two bits count.
 * @return The blend id, 1..4. Opaque (0) is the caller's, from its own
 *         semi-transparency bit. */
int    moby_abr_blend(int abr);
/** @brief Fill one face record's texture fields from its three texture words.
 *
 * The words are {u0|v0<<8|clut<<16}, {u1|v1<<8|tpage<<16} and
 * {u2|v2<<8|u3<<16|v3<<24}. A tpage colour-depth field of 3 is reserved on the
 * hardware and marks a replaced texture instead: the record takes depth 32 and
 * carries the texture id, the CLUT half read as a signed 16-bit value, where
 * the page x would go.
 * @param td0 The first texture word.
 * @param td1 The second.
 * @param td2 The third.
 * @param F Receives the u/v pairs, blend mode, page, depth and CLUT fields. */
void   moby_tiledef_decode(uint32_t td0, uint32_t td1, uint32_t td2, moby_wface_t* F);
/** @brief Decide from two NCLIP areas which triangles of a face draw, and in which vertex order.
 *
 * A fresh (quad-capable) face tests NCLIP1 on (v0,v1,v2) and NCLIP2 on
 * (v3,v1,v2) and emits, by the sign pair, the triangle (v3,v1,v2), nothing,
 * the quad (v0,v1,v2)+(v1,v3,v2), or the triangle (v0,v1,v2). Without
 * have_v3 the quad and the (v3,v1,v2) triangle degrade to nothing and the
 * triangle (v0,v1,v2). A strip face is one triangle (v0,v1,v2) kept on
 * NCLIP1's sign.
 *
 * The face's own bit0 changes both rules, and the two draws differ in how.
 * With bit0_unconditional the bit skips the test outright: a pre-marked quad
 * is emitted whole (a triangle without have_v3) and a strip triangle is kept
 * of either winding. Without it the bit replaces NCLIP1 by -|NCLIP1| and NCLIP2
 * by |NCLIP2| before the same tests, which differs from the first only on a
 * zero-area face; a strip triangle is then kept unless NCLIP1 is 0.
 * @param fresh Nonzero for a quad-capable face, 0 for a strip face.
 * @param bit0 The face's bit0.
 * @param bit0_unconditional Nonzero when bit0 skips the test.
 * @param ncl1 NCLIP of (v0,v1,v2).
 * @param ncl2 NCLIP of (v3,v1,v2); unused for a strip face.
 * @param have_v3 Nonzero when the face has a fourth vertex; unused for a strip
 *        face.
 * @param order Receives the three face-vertex slots (0..3) of triangle e in
 *        order[e].
 * @return The triangle count, 0..2. */
int    moby_face_emit(int fresh, int bit0, int bit0_unconditional,
                      double ncl1, double ncl2, int have_v3, int order[2][3]);

/** @brief The guest base of the cull-record queue the last canonical moby pass read, 0 before any pass.
 *
 * A game whose queue lives in memory the rest of the tick reuses stages the
 * queue's bytes for each sub-tick present, so the replay reads the queue the
 * tick drew rather than whatever overwrote it. The base is captured at the
 * pass rather than recomputed, since a queue carved out of the polybuf rides
 * the draw buffer that tick allocated.
 * @return The base, or 0. */
uint32_t moby_canon_rec_base(void);
/** @brief The length in bytes of the queue moby_canon_rec_base names, game_layout.moby.rec_base_polybuf_off.
 *
 * A queue carved out of the polybuf runs from its base to the polybuf end.
 * 0 for a game that keeps its queue at a fixed address nothing else touches.
 * @return The length, or 0. */
uint32_t moby_canon_rec_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_MOBY_H */
