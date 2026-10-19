/* A block holding a member-declared struct, a matrix, a float array and
 * scalars, through a named instance: its std140 layout must survive the
 * move into the chain struct. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

struct Band { vec3 color; float width; };

layout(set = 3, binding = 1, std140) uniform op_instance {
    Band  band[2];
    mat3  grade;
    float weight[3];
    float gain;
    uint  mode;
} p;

vec4 op_material(vec4 base)
{
    vec3 c = p.grade * base.rgb;
    for (int i = 0; i < 2; i++) c = mix(c, p.band[i].color, p.band[i].width * p.weight[i]);
    if (p.mode == 1u) c *= p.gain;
    return vec4(c, base.a);
}
