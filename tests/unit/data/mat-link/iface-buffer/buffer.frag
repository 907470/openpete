/* A storage buffer of its own beside the block. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 4, std430) readonly buffer own_data { vec4 k[]; };

vec4 op_material(vec4 base) { return base * k[0]; }
