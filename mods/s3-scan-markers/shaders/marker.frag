/* Scan marker: the instance's colour, with a third of the face's own
 * luminance kept so the model's shape stays readable. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 marker;   /* rgb = scan-state colour, a unused */
};

vec4 op_material(vec4 base)
{
    float luma = dot(base.rgb, vec3(0.299, 0.587, 0.114));
    return vec4(marker.rgb * (0.7 + 0.3 * clamp(luma * 2.0, 0.0, 1.0)), base.a);
}
