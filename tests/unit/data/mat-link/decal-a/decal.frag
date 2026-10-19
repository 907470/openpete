/* A decal-contract member. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_decal.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };

vec4 op_material(vec4 base) { return vec4(mix(base.rgb, tint.rgb, tint.a), base.a); }
