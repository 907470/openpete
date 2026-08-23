/* Per-instance tint: shade the face normally, then push it toward the
 * colour this instance's refine callback produced.
 *
 * The include is the MATERIAL contract, not openpete_shader_api.glsl —
 * the two claim the same descriptor set differently, so a material frag
 * built on the post-process contract binds against a layout the material
 * path never set up and only some draws survive it. (This shader used to
 * do exactly that, and reached about a seventh of the mobys it selected.)
 *
 * op_pack_shade() decodes the face the way the engine would, honouring an
 * installed HD texture pack, so a tinted moby still matches its
 * neighbours. Mixing rather than replacing keeps the model's shading
 * legible — which matters here, because a flat fill would hide whether
 * the material reached the WHOLE model or just some of its faces.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /* rgb = this instance's hue, a unused */
};
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 base = op_pack_shade();
    outColor = vec4(mix(base.rgb, tint.rgb, 0.75), base.a);
}
