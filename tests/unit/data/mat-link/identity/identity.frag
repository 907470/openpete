/* Returns its parameter: an identity member. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

vec4 op_material(vec4 base) { return base; }
