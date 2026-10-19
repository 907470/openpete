/* A block of 272 bytes, over the 256-byte member block. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance { vec4 v[17]; };

vec4 op_material(vec4 base) { return vec4(base.rgb * v[16].rgb, base.a); }
