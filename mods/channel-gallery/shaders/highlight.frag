/* Highlight whatever this material was selected onto.
 *
 * The point of the gallery is to make SELECTION visible, so the shading is
 * deliberately dumb: take the stock look and push it toward one colour. What
 * varies is not this shader but the selector that decides which geometry
 * reaches it.
 *
 * `base` is the engine's own result for this fragment — stock decode, or
 * an installed HD texture pack's replacement, with the vertex modulation
 * already applied — so a highlighted face still matches its neighbours,
 * and untextured draws arrive as the interpolated vertex colour. One
 * function serves every channel the gallery selects.
 *
 * mix, not replace: geometry stays readable, which matters for the blended
 * channels — an additive draw forced to a flat colour just blows out. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /* rgb = highlight colour, a = strength 0..1 */
};

vec4 op_material(vec4 base)
{
    return vec4(mix(base.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0)), base.a);
}
