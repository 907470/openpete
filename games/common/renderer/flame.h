/* flame.h — the player's fire-breath ribbon producer (renderer/flame.c).
 *
 * The producer hooks the game's flame emitter (game_layout.flame.hook), runs
 * it once, and re-derives the same ribbon from gamestate in float for the
 * native scene. The sub-tick entries below let a game's re-extract replay one
 * ribbon per flame record at a present between two ticks. */
#ifndef OPENPETE_RENDERER_FLAME_H
#define OPENPETE_RENDERER_FLAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* The fractional part of the keyframe-counter advance the next walk slides
 * each cross-section by, 0..1. 0 restores exact keyframes, which is what the
 * canonical inline render uses. */
void flame_native_set_subframe(double frac);

/* Re-derive one flame record's ribbon into the native scene under `slot`, the
 * model record it renders with. Self-gated on the staged active flag and on
 * the canonical tick having drawn the path. */
void flame_reextract(struct CPUState* cpu, int slot);

/* Bracket one prev/curr pair of flame-state bytes into `fl` (already holding
 * curr): origin and attach matrix lerped at `alpha`, per-strand keyframe
 * counters advanced fractionally with the shared sub-keyframe slide returned
 * in *subframe. Returns 0 when the origin jumped and the bracket snapped, and
 * `fl` is then untouched. */
int flame_bracket(uint8_t* fl, const uint8_t* pfl, const uint8_t* cfl,
                  double alpha, double* subframe);

/* Run every flame record of this present at the producer's place in the scene
 * order, after the particles. `fl` is the caller's ambient staged copy of the
 * flame span, or NULL to let the loop stage the span itself. */
void flame_records_present(struct CPUState* cpu, int paired, int cam_cut,
                           double alpha, uint8_t* fl);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_RENDERER_FLAME_H */
