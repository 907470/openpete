/* A descriptor of its own beside the block. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 3) uniform sampler2D own_tex;

vec4 op_material(vec4 base) { return base * texture(own_tex, op_uv()); }
