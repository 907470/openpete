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

/* The hyperbolic depth curve's scale (the key images). */
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

/* Scene depth, written by every scene vertex stage through this one
 * expression. Reversed (spec constant 0 true: the float depth attachment):
 * z_ndc = 1/vz, compared GREATER_OR_EQUAL against a clear of 0, so float32
 * keeps about 2^-24 relative precision at every distance. Forward (the D24
 * fallback and the FORWARD-class pre-pass): z_ndc = 1 - 1/vz, compared
 * LESS_OR_EQUAL against a clear of 1. Both are affine in 1/vz, so the
 * rasteriser interpolates them exactly in screen space. Returns the clip
 * position of clip-space screen cxy at ordering view-Z vzo: on the
 * perspective path w = vzo and z is the numerator of z_ndc (1 or vzo - 1),
 * a (z, w) pair the clipper interpolates linearly without disturbing the
 * window depth; on the screen-affine UV path w = 1 and z = z_ndc. The near
 * clip z <= w holds at vzo >= 1, which op_scene_vz guarantees. */
vec4 op_scene_clip(vec2 cxy, float vzo, bool affineUV, bool reversed)
{
    float zc = reversed ? 1.0 : vzo - 1.0;
    return affineUV ? vec4(cxy, zc / vzo, 1.0) : vec4(cxy * vzo, zc, vzo);
}

#endif
