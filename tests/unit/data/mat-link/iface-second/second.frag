/* A second uniform block at set 3 binding 1 (the two alias). */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };
layout(set = 3, binding = 1, std140) uniform op_extra { vec4 tint2; };

vec4 op_material(vec4 base) { return base * tint * tint2; }
