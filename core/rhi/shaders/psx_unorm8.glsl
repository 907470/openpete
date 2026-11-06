/* psx_unorm8.glsl - the exact 8-bit vertex-colour grid.
 *
 * A UNORM8 vertex colour reaches a shader as the implementation's conversion
 * of the stored byte, which equals c/255 only to within one ULP: a driver
 * that multiplies by the float32 reciprocal of 255 lands one ULP above the
 * correctly rounded quotient for 126 of the 256 byte values. Stages that must
 * agree on a colour do not all reach it through that conversion - the walked
 * scene feeds the byte as an attribute, while the retained terrain mesh
 * resolves several of its colours as integers in the vertex stage and
 * divides. Operands one ULP apart modulate the same texel to products that
 * straddle a rounding tie, and the two stages then store levels one apart.
 *
 * op_unorm8 puts a converted byte back on the exact c/255 grid: the
 * conversion error is far below half a level, so the round recovers the
 * stored byte exactly, and the quotient is the value the format defines,
 * identical on every implementation. Every stage whose colour must match
 * another's applies it at the attribute. */

#ifndef OPENPETE_PSX_UNORM8_GLSL
#define OPENPETE_PSX_UNORM8_GLSL

vec3 op_unorm8(vec3 c) { return round(c * 255.0) / 255.0; }
vec4 op_unorm8(vec4 c) { return vec4(op_unorm8(c.rgb), c.a); }

/* Colour bytes c plus their residual q, in 1/256 of a level: a split node's
 * unrounded colour. Every stage that draws one builds it here, so the walked
 * scene and the retained mesh reach the same operand. q = 0 is c's exact
 * c/255 grid value. */
vec3 op_rgb8q(ivec3 c, ivec3 q) { return (vec3(c) + vec3(q) / 256.0) / 255.0; }

/* op_unorm8 of a scene vertex colour plus its residual word (rhi_vertex.rgbq):
 * three signed bytes, red low, as an integer-valued float. */
vec4 op_unorm8q(vec4 c, float rgbq)
{
    int w = int(rgbq);
    ivec3 q = ivec3(bitfieldExtract(w, 0, 8), bitfieldExtract(w, 8, 8), bitfieldExtract(w, 16, 8));
    return vec4(op_rgb8q(ivec3(round(c.rgb * 255.0)), q), c.a);
}

#endif /* OPENPETE_PSX_UNORM8_GLSL */
