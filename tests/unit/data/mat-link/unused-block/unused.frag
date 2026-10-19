/* A block the member declares and never reads. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance { vec4 unused_tint; };

vec4 op_material(vec4 base) { return base.gbra; }
