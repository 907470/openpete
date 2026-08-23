/* Push the stock look toward one colour.
 *
 * The whole point of the zero-C tier is that this file plus a manifest row
 * is a complete mod, so the shading stays as small as it can be — and with
 * the engine owning main(), "as small as it can be" is one function. `base`
 * arrives already decoded the way the engine would draw it, including any
 * HD texture-pack replacement and the vertex modulation, so all that is
 * left is the mix toward the tint the [[material]] row supplied in
 * `params`.
 *
 * mix, not replace: on a blended channel the material inherits the group's
 * blend equation, so forcing a flat colour into an additive draw just blows
 * it out. Keeping the base visible is what makes the tint read as tinting.
 *
 * A row with no `params` gets a zeroed block, i.e. strength 0 — the stock
 * look, unchanged. That is the intended reading of "no parameters", and it
 * is exactly what the identity-material gate rung asserts.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /* rgb = colour, a = strength 0..1 */
};

vec4 op_material(vec4 base)
{
    return vec4(mix(base.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0)), base.a);
}
