/* openpete_obj_id.h — side-channel "current render object" identity.
 *
 * Carries a stable cross-frame per-object PSX vaddr from each per-object
 * render-loop iteration to gte_rtps_internal, which stamps it into the
 * render-cache entry's obj_tag so the transform-interp matcher can pair
 * spans by real instance identity (instead of falling back to the
 * class-shared m_Props ptr or world-ordinal heuristics).
 *
 * This is DELIBERATELY separate from openpete_ui_moby's s_current.
 * They answer two different questions:
 *
 *   - openpete_ui_moby  "is the object being projected a HUD/UI element?"
 *                       — narrow; only ever meaningful on the shaded
 *                         renderer (the only path HUD/text mobys flow
 *                         through). Consumed by widescreen_eff_ofx to
 *                         choose centring vs Hor+ widening.
 *   - openpete_obj_id   "which logical object produced this span?"
 *                       — broad; written by EVERY per-object render path
 *                         (regular, specular, shaded, Spyro, cyclorama).
 *                         Consumed only by the render-cache obj_tag emit.
 *
 * They were briefly merged onto s_current, which let arbitrary
 * gameplay-moby vaddrs (popped off the moby queue by the regular/
 * specular passes, or g_Spyro / g_Cyclorama) get range-tested by
 * is_current_ui() and mis-classified as UI — breaking Hor+ widening
 * for any object whose address fell in the polybuf/static HUD range.
 * Splitting the channels restores widescreen's "only the shaded loop
 * writes the UI classifier" invariant. */

#ifndef OPENPETE_OBJ_ID_H
#define OPENPETE_OBJ_ID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stash the vaddr of the object GTE will project next (0 = world /
 * non-object). Called from each per-object render-loop iteration. */
void     openpete_obj_id_set(uint32_t obj_addr);

/* Read back the stashed object vaddr (for the obj_tag cache stamp). */
uint32_t openpete_obj_id_get(void);

/* #25: per-object vertex emission ordinal (post-incremented; reset
 * by obj_id_set). Consumed by gte_rtps_internal as the moby-tagged
 * cache entry's vkey so the matcher's per-vertex correspondence
 * pairs the SAME logical vertex across animation poses (v_local
 * churns under keyframe morphing). G1-validated stable emission
 * order; the world_vidx template applied to mobys. */
uint32_t openpete_obj_id_next_vidx(void);

/* Drop-shadow sub-tag: OR bit29 into the current obj_id and restart
 * the ordinal counter, so per-moby shadow passes pair in their own
 * stable rank group (see openpete_obj_id.c rationale). */
void openpete_obj_id_shadow_subtag(void);

/* Game-registered tag normalizer (NULL = identity), applied inside
 * obj_id_set. Maps frame-aliased object-copy addresses (double-
 * buffered packet memory) to a buffer-relative canonical form so
 * they pair across generations. See openpete_obj_id.c rationale. */
extern uint32_t (*openpete_obj_id_normalize_fn)(uint32_t obj_addr);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_OBJ_ID_H */
