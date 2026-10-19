/* Shares its block name, member names, helper and constant with
 * shared-b/flame.frag (the cynder breath / dragon-gifts flame shape). */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 edge;
    vec4 middle;
    vec4 highlight;
};

const vec3 WEIGHTS = vec3(0.2126, 0.7152, 0.0722);

float heat_of(vec4 c) { return max(c.r, max(c.g, c.b)); }

vec4 op_material(vec4 base)
{
    float heat = heat_of(base);
    vec3 pigment = mix(edge.rgb, middle.rgb, smoothstep(0.03, 0.58, heat));
    pigment = mix(pigment, highlight.rgb, smoothstep(0.65, 1.0, heat));
    return vec4(pigment * dot(base.rgb, WEIGHTS), base.a);
}
