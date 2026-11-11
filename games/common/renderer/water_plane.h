/* water_plane.h - the water-plane split the player and moby draws share.
 *
 * Spyro 2 and 3 draw a model across a horizontal world plane in two halves.
 * Each vertex gets a signed distance to the plane; a face wholly on the
 * positive side draws stock, a face wholly on the negative side is dropped or
 * depth-cued toward the environment's far colour, and a face with vertices on
 * both sides is cut at the plane and each part takes its side's policy. The
 * player draw (spyrodraw.s) and the animated moby draw (mobydraw.s) run the
 * same split; each caller answers its own plane and keeps its own vertex form,
 * and both shade and cut through the two functions here.
 */
#ifndef OPENPETE_RENDERER_WATER_PLANE_H
#define OPENPETE_RENDERER_WATER_PLANE_H

#include <stdint.h>

/* The attributes a cut interpolates besides screen position and colour. */
#define WPLANE_ATTRS 10

/* One triangle corner on its way through the cut. `sx`, `sy` are the screen
 * position; `w` is the perspective divisor the screen position was projected
 * with, 0 when the caller interpolates screen position linearly in the cut
 * parameter as the guest draw does. `at` holds the attributes that are affine
 * in model space (depth, texture coordinates, rotated offsets), `pd` the
 * signed plane distance, `rgb` the packed 0x00BBGGRR colour and `idx` the
 * caller's vertex identity, which a cut point inherits from its edge's first
 * corner. */
typedef struct wplane_vert {
    double sx, sy, w;
    double at[WPLANE_ATTRS];
    double pd;
    uint32_t rgb;
    int idx;
} wplane_vert_t;

/** @brief The negative half's colour for one packed 0x00BBGGRR vertex colour.
 *
 * The draws run one DPCS per vertex colour at sf = 1 with the far colour in
 * RFC/GFC/BFC, cueing each channel toward it by IR0. The chain is reproduced
 * integer for integer because both of its shifts truncate, and a rounded blend
 * lands a unit above the hardware on about half the channels:
 *   IR  = SAR12((FC << 12) - (c << 16))   = FC - (c << 4)
 *   MAC = SAR12(IR * IR0 + (c << 16))
 *   out = MAC >> 4                        (the RGB2 pack), clamped to a byte
 * @param fc The far colour as the three GTE control words, sixteen times the
 *        0..255 channel.
 * @param ir0 The IR0 of the cue.
 * @param rgb The vertex colour.
 * @return The cued colour. */
uint32_t wplane_shade(const int32_t fc[3], int32_t ir0, uint32_t rgb);

/** @brief Cuts one triangle at the plane into its positive and negative polygons.
 *
 * Walks the three edges in order; each corner goes to the side its distance
 * names (pd >= 0 is positive) and each edge whose ends lie on opposite sides
 * adds its crossing to both polygons, so each output is a convex polygon of
 * at most four corners in the input's winding. The crossing sits at the
 * model-space parameter t = pd_a / (pd_a - pd_b): the affine attributes and
 * the colour (rounded per channel) take t; the screen position takes t too
 * when either corner's `w` is 0, else the perspective-correct parameter
 * t * w_b / ((1 - t) * w_a + t * w_b), which places the crossing where the
 * cut point projects.
 * @param in The triangle's three corners with their plane distances.
 * @param nat The number of `at` entries in use, at most WPLANE_ATTRS.
 * @param pos Receives the positive polygon.
 * @param np Receives its corner count, 0 or 3..4.
 * @param neg Receives the negative polygon.
 * @param nn Receives its corner count, 0 or 3..4. */
void wplane_cut(const wplane_vert_t in[3], int nat,
                wplane_vert_t pos[4], int* np, wplane_vert_t neg[4], int* nn);

#endif
