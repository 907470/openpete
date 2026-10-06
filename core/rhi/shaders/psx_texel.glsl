/* psx_texel.glsl - the integer texel of a fragment's texel coordinate.
 *
 * A fragment's texel coordinate is the perspective quotient of two
 * interpolated varyings (vUV / vQ). On a face whose vertices share one V (or
 * U), the exact quotient is that value at every fragment, but float rounding
 * lands it a few ulps either side, and int() then truncates 159.99998 to row
 * 159: a texel outside every vertex UV of the face, transparent or another
 * tile's. The PSX rasterizer steps the 8-bit vertex UVs with integer
 * arithmetic and samples the shared coordinate exactly.
 *
 * The fix needs no vertex data. op_texel_grad returns the coordinate's
 * screen-space gradient (fwidth). Along an axis whose gradient is under
 * OP_TEXEL_FLAT texels per pixel the coordinate is constant over the face up
 * to rounding noise, and op_texel adds OP_TEXEL_SNAP before truncating, so a
 * quotient rounded just under an integer lands on it; an axis with a real
 * gradient truncates unchanged. A face magnified past 1 / OP_TEXEL_FLAT
 * pixels per texel takes the snap too, moving its texel edges by
 * OP_TEXEL_SNAP of a texel.
 *
 * op_texel_grad is called at the top of main(), before any discard: a screen
 * derivative needs every lane of the 2x2 quad live. */
#ifndef OPENPETE_PSX_TEXEL_GLSL
#define OPENPETE_PSX_TEXEL_GLSL

/* Texels per pixel under which an axis is constant over the face. */
const float OP_TEXEL_FLAT = 1.0 / 256.0;
/* Texels added along a constant axis before truncation; above the quotient's
 * rounding error and far below one texel. */
const float OP_TEXEL_SNAP = 1.0 / 256.0;

/* The screen-space gradient magnitude of texel coordinate uv, per axis. */
vec2 op_texel_grad(vec2 uv)
{
    return fwidth(uv);
}

/* The integer texel of coordinate uv whose gradient op_texel_grad returned. */
ivec2 op_texel(vec2 uv, vec2 grad)
{
    vec2 c = uv + mix(vec2(0.0), vec2(OP_TEXEL_SNAP), lessThan(grad, vec2(OP_TEXEL_FLAT)));
    return ivec2(int(c.x), int(c.y));
}

#endif /* OPENPETE_PSX_TEXEL_GLSL */
