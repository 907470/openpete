/* portal_plane.h : the plane-cover near margin, shared by every game's portal
 * feeder and by the shared terrain feeder.
 *
 * The margin is one number in one place because three sites must agree on it:
 * the portal preview's own test depth, the terrain feeder's transparent-cover
 * drop, and the core cover-check instrument. Two copies drift and the drop
 * then disagrees with what the preview actually paints. */
#ifndef OPENPETE_COMMON_RENDERER_PORTAL_PLANE_H
#define OPENPETE_COMMON_RENDERER_PORTAL_PLANE_H
#ifdef __cplusplus
extern "C" {
#endif

/** @brief The plane-cover near margin M in world units: inside an opening's footprint the preview covers everything at or behind plane - M.
 *
 * Read from OPENPETE_PORTAL_PLANE_MARGIN on the first call and latched;
 * default 128, a negative value clamps to 0. The plane alone covers every
 * measured intruder at or behind it (the doorway moby, the extended terrain
 * band: the same cover counts at M 0..256); the margin widens cover into the
 * near band, where an arch jamb slit opens toward the original's width as M
 * rises to about 256. The ceiling is the portal-name text mobys hovering about 192-256
 * in front of the plane, which lose pieces at 256, and inner-frame erosion from
 * about 512. 128 sits below the text band with the jamb mostly recovered.
 * @return M. */
float openpete_portal_plane_margin(void);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_PORTAL_PLANE_H */
