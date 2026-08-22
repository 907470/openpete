/* Highlight whatever this material was selected onto.
 *
 * The point of the gallery is to make SELECTION visible, so the shading is
 * deliberately dumb: take the stock look and push it toward one colour. What
 * varies is not this shader but the selector that decides which geometry
 * reaches it.
 *
 * op_pack_shade() is the base layer rather than op_psx_shade() so a
 * highlighted face still matches its neighbours when an HD texture pack is
 * installed (it falls back to the stock decode when no pack is active).
 * Untextured draws arrive with bpp 0 and op_pack_shade() returns the
 * interpolated vertex colour, so one shader serves both.
 *
 * mix, not replace: geometry stays readable, which matters for the blended
 * channels — an additive draw forced to a flat colour just blows out. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /* rgb = highlight colour, a = strength 0..1 */
};
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 base = op_pack_shade();
    outColor = vec4(mix(base.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0)), base.a);
}
