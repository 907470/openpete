/* core/renderer/launcher_title_pass.h - the launcher's 3D title pass on the
 * launcher's device: the glyph triangles drawn depth-tested into an
 * offscreen target at twice the size per axis of the window rect their
 * bounds take (fewer samples when that passes 4096 texels a side), then
 * composited over that rect of the launcher's offscreen image with each
 * output pixel the box average of its 2x2 samples (overlay_resolve.frag),
 * before imgui draws.
 *
 * Two pipelines: the glyph pipeline (quad.vert / quad.frag, depth-tested,
 * opaque) and the composite (psx_sample.vert / overlay_resolve.frag,
 * straight-alpha blend). Both are described for the launcher's ahead
 * compile (its first group) and created on the launcher's thread when the
 * title is first drawn, from the pipeline cache when the ahead compile
 * reached them. Display owner only; writes no PSX RAM. */
#ifndef OPENPETE_LAUNCHER_TITLE_PASS_H
#define OPENPETE_LAUNCHER_TITLE_PASS_H

#include "openpete_native_render.h" /* openpete_hud_vert */

struct rhi_device;

/** @brief Describe the title pass's two pipelines into the open description mode, in the group the caller set.
 *
 * @param dev The launcher's device, in description mode. */
void nr_launcher_title_describe(struct rhi_device *dev);

/** @brief Take the title's triangles for the next presents on a device, creating the pass's pipelines, buffer and target when first needed.
 *
 * x and y are window pixels of a w x h window, z the view-Z of the overlay
 * depth band; the target covers the vertices' bounds (padded, clamped to the
 * window, its extent rounded up to 64 px) at 2 samples per pixel per axis.
 * Runs outside a frame (a target resize waits for the device to idle).
 * @param dev The launcher's device; NULL clears.
 * @param v The triangles, 3 vertices each; NULL or @p n below 3 clears.
 * @param n The vertex count.
 * @param w The window width in pixels.
 * @param h The window height in pixels.
 * @return 1 when the title draws on the next present (as nothing when its
 *         bounds lie outside the window), else 0 (a pipeline or the target
 *         could not be created). */
int nr_launcher_title_set(struct rhi_device *dev, const openpete_hud_vert *v, int n, int w, int h);

/** @brief Render the title into its target: inside a frame, before the offscreen pass opens.
 *
 * Draws nothing unless the device is the one the title was set on and a
 * title is set.
 * @param dev The presenting device. */
void nr_launcher_title_prepass(struct rhi_device *dev);

/** @brief Composite the target rendered by nr_launcher_title_prepass this present over its rect, box-filtering each block.
 *
 * Inside the offscreen pass, before imgui draws.
 * @param dev The presenting device. */
void nr_launcher_title_composite(struct rhi_device *dev);

/** @brief Destroy the pass's pipelines, buffers and target on a device, logging the presents it drew.
 *
 * Called when the device closes or the game adopts it.
 * @param dev The device; a device the pass holds nothing on is a no-op. */
void nr_launcher_title_release(struct rhi_device *dev);

#endif /* OPENPETE_LAUNCHER_TITLE_PASS_H */
