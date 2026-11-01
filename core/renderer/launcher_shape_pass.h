/* core/renderer/launcher_shape_pass.h - the launcher's shape pass: the
 * panels, chips, text scrims and progress bar a launcher present's imgui
 * callback adds (openpete_native_launcher_shape_add), drawn as signed-distance
 * rounded rectangles with elliptical corners, a coverage ramp at the edge
 * and a drop shadow outside the shape (launcher_shape.vert / .frag).
 *
 * One pipeline (straight-alpha blend, no depth), described for the ahead
 * compile's launcher group and created at the first present that draws a
 * shape, from the pipeline cache when the ahead compile reached it. The pass
 * runs inside the offscreen pass after the sky and before the 3D title and
 * imgui's draw data. Display owner only; writes no PSX RAM. */
#ifndef OPENPETE_LAUNCHER_SHAPE_PASS_H
#define OPENPETE_LAUNCHER_SHAPE_PASS_H

struct rhi_device;

/** @brief Describe the shape pipeline into the open description mode, in the group the caller set.
 *
 * @param dev The device, in description mode. */
void nr_launcher_shapes_describe(struct rhi_device *dev);

/** @brief Draw the shapes added since the last nr_launcher_shapes_end: every shadow, then every shape in the order added.
 *
 * Inside the offscreen pass, after the sky. Creates the pipeline and the
 * vertex buffer on the first call with shapes; on a device other than the
 * one they were created on nothing draws.
 * @param dev The presenting device. */
void nr_launcher_shapes_draw(struct rhi_device *dev);

/** @brief Drop the shapes added for the present that ended, drawn or not. */
void nr_launcher_shapes_end(void);

/** @brief Destroy the pass's pipeline and buffer on a device, logging the presents it drew.
 *
 * Called before the device is destroyed (the launcher's close, the
 * renderer's shutdown); a device the pass holds nothing on is a no-op.
 * @param dev The device. */
void nr_launcher_shapes_release(struct rhi_device *dev);

#endif /* OPENPETE_LAUNCHER_SHAPE_PASS_H */
