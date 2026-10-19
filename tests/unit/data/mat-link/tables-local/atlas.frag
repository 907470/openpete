/* A moved constant table from a mod-local include, beside a block. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"
#include "atlas.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 amount;
};

vec4 op_material(vec4 base)
{
    uint i = uint(clamp(base.g, 0.0, 1.0) * 399.0);
    float v = float(k_atlas[i] & 255u) / 255.0;
    return vec4(mix(base.rgb, vec3(v), amount.x), base.a);
}
