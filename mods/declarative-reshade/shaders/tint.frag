/* Push the stock look toward one colour.
 *
 * The whole point of the zero-C tier is that this file plus a manifest row
 * is a complete mod, so the shading stays as small as it can be: decode the
 * face the way the engine would, then mix toward the tint the [[material]]
 * row supplied in `params`.
 *
 * op_pack_shade() rather than op_psx_shade() so a tinted face still matches
 * its neighbours when an HD texture pack is installed (it falls back to the
 * stock decode when no pack is active). Untextured draws arrive with bpp 0
 * and it returns the interpolated vertex colour, so one shader serves both
 * the textured and gouraud paths.
 *
 * mix, not replace: on a blended channel the material inherits the group's
 * blend equation, so forcing a flat colour into an additive draw just blows
 * it out. Keeping the base visible is what makes the tint read as tinting.
 *
 * A row with no `params` gets a zeroed block, i.e. strength 0 — the stock
 * look, unchanged. That is the intended reading of "no parameters".
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /* rgb = colour, a = strength 0..1 */
};
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 base = op_pack_shade();
    outColor = vec4(mix(base.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0)), base.a);
}
