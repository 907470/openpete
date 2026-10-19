/* Locals named like builtins (distance) and like other members' globals
 * (near, second), the cynder palette shape; one global helper. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform cynder_skin {
    vec4 skinTexture;
    vec4 bodyPigment;
};

float facet(vec2 p)
{
    float near = 20.0, second = 20.0;
    for (int k = 0; k < 4; k++) {
        vec2 center = vec2(float(k & 1), float(k >> 1));
        float distance = dot(p - center, p - center);
        if (distance < near) { second = near; near = distance; }
        else second = min(second, distance);
    }
    return second - near;
}

vec4 op_material(vec4 base)
{
    return vec4(base.rgb * bodyPigment.rgb * (0.9 + 0.1 * facet(op_uv() + skinTexture.xy)), base.a);
}
