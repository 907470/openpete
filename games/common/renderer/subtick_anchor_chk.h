// games/common/renderer/subtick_anchor_chk.h - temporary sub-tick anchor probe
// (OPENPETE_SUBTICK_ANCHOR_CHK): judges a feeder's fed screen anchor on a
// sub-tick present against a world-space reference built independently from
// the camera ring and the keyed object store.
#ifndef SUBTICK_ANCHOR_CHK_H
#define SUBTICK_ANCHOR_CHK_H

#include <stdint.h>
#include "renderer/native_gte.h"

enum { SAC_GLOW, SAC_RIBBON, SAC_MESH, SAC_N };

/** @brief OPENPETE_SUBTICK_ANCHOR_CHK, latched: 0 off, 1 the exit summary, 2 also one line per sample.
 * @return The level. */
int sac_level(void);

/** @brief Open one bracketed sub-tick present: build the reference camera from the ring pair at the present clock.
 * @param path The feeder opening it, counted per path (SAC_GLOW, or SAC_RIBBON for the tracer feeder).
 * @param alpha_fed The alpha the feeder was handed. */
void sac_present_begin(int path, double alpha_fed);

/** @brief Judge one fed anchor against the world-space reference and fold it into the statistics.
 * @param path SAC_GLOW, SAC_RIBBON or SAC_MESH.
 * @param key The anchor's keyed-store key.
 * @param prev_w The previous tick's world point, or NULL when the feeder found none.
 * @param curr_w The tick's world point.
 * @param prev_ns The keyed pair's previous stamp, 0 when none.
 * @param curr_ns The keyed pair's current stamp, 0 when none.
 * @param fed_sx The fed anchor's screen X.
 * @param fed_sy The fed anchor's screen Y.
 * @param v The view the feeder projected under. */
void sac_judge(int path, uint32_t key, const double* prev_w, const double curr_w[3],
               int64_t prev_ns, int64_t curr_ns, double fed_sx, double fed_sy, const ngte_view* v);

/** @brief Count one present a feeder served without a camera pair (bracketed == 0, no held cut) while it held anchors.
 * @param path SAC_GLOW, SAC_RIBBON or SAC_MESH. */
void sac_unbracketed_present(int path);

/** @brief Fold one anchor of an unbracketed present into that present class's residual statistics.
 *
 * Such a present draws under the tick's camera, so the reference is the
 * anchor as the capture built it.
 * @param path SAC_GLOW, SAC_RIBBON or SAC_MESH.
 * @param resid The fed anchor's distance from the captured anchor, in px. */
void sac_unbracketed_anchor(int path, double resid);

/** @brief Fold one sub-tick depth sample: the tick's depth, the fed depth and the present's true depth, in scene view-Z.
 *
 * A stale depth nearer than the true one by `gap` draws the item in front of
 * every surface between the two; one slot is OPENPETE_OT_SLOT_VZ.
 * @param path SAC_GLOW, SAC_RIBBON or SAC_MESH.
 * @param stale The depth the tick built (what a feed keeping the tick's depth draws).
 * @param fed The depth fed on this present.
 * @param truth The depth of the same lerped world point under the present's camera.
 * @param alpha The present's alpha. */
void sac_depth(int path, double stale, double fed, double truth, double alpha);

/** @brief OPENPETE_SUBTICK_TICK_DEPTH, latched: feeds a path's items at the tick's depth on sub-tick presents, for a same-binary A/B.
 *
 * `1` selects both paths, `g` the glow alone, `t` the tracers alone; unset
 * or `0` selects neither.
 * @param tracer Nonzero for the tracer path, 0 for the glow path.
 * @return 1 when the path is selected, else 0. */
int sac_tick_depth(int tracer);

#endif
