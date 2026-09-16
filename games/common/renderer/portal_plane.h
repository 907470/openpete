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

/* Plane-cover near margin M in world units (OPENPETE_PORTAL_PLANE_MARGIN,
 * default 128): inside an opening's footprint the preview covers everything
 * at or behind plane − M. */
float openpete_portal_plane_margin(void);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_PORTAL_PLANE_H */
