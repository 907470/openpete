/* Discards from a function op_material calls. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 cut;
};

void clip_dark(vec4 c) { if (c.r + c.g + c.b < cut.x) discard; }

vec4 op_material(vec4 base)
{
    clip_dark(base);
    return base;
}
