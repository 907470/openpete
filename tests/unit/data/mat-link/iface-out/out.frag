/* An output variable of its own. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(location = 1) out vec4 extra;

vec4 op_material(vec4 base) { extra = base; return base.bgra; }
