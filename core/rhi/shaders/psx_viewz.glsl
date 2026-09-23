/* psx_viewz.glsl — the scene vertex stream's depth unit, shared by every
 * vertex stage that reads it.
 *
 * The CPU feed carries full-world view-Z unencoded: inPos.z is the ORDERING
 * view-Z (feeders may shift it to port the ordering table's authored order),
 * inW the TRUE view-Z (the perspective W for UV/colour). The stages clamp
 * both into [1, OP_VZ_FAR] and write the depth buffer from the ordering
 * view-Z. Mirror of openpete_native_render.h (OPENPETE_DEPTH_VIEWZ_SCALE,
 * OPENPETE_DEPTH_VIEWZ_FAR). */

#ifndef OPENPETE_PSX_VIEWZ_GLSL
#define OPENPETE_PSX_VIEWZ_GLSL

/* The hyperbolic depth curve's scale (key images, legacy scene depth). */
#define OP_VZ_SCALE 4096.0
/* The farthest kept view-Z: 4096 * 0.9999 / (1 - 0.9999) in float, the
 * view-Z whose hyperbolic depth is 0.9999. Bounds W. */
#define OP_VZ_FAR 40949108.0

/* A fed view-Z, clamped to the range the stages rasterise: the near guard at
 * 1 and the far bound at OP_VZ_FAR. */
float op_scene_vz(float vz)
{
    return clamp(vz, 1.0, OP_VZ_FAR);
}

/* The hyperbolic depth curve z = vz / (vz + 4096). */
float op_vz_hyperbolic(float vz)
{
    return vz / (vz + OP_VZ_SCALE);
}

#endif
