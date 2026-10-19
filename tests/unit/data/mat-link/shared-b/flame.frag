/* The same names as shared-a/flame.frag through a named block instance. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 edge;
    vec4 middle;
    vec4 highlight;
} flame;

const vec3 WEIGHTS = vec3(0.299, 0.587, 0.114);

float heat_of(vec4 c) { return dot(c.rgb, WEIGHTS); }

vec4 op_material(vec4 base)
{
    float heat = heat_of(base);
    vec3 palette = mix(flame.edge.rgb, flame.middle.rgb, smoothstep(0.05, 0.55, heat));
    palette = mix(palette, flame.highlight.rgb, smoothstep(0.72, 1.0, heat));
    return vec4(palette * heat, base.a);
}
