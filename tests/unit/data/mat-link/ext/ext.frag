/* An extension of its own, which joins the module's extension union. */
#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_control_flow_attributes : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance { vec4 steps; };

vec4 op_material(vec4 base)
{
    vec3 c = base.rgb;
    [[unroll]] for (int i = 0; i < 4; i++) c = c * steps.x + steps.y;
    return vec4(c, base.a);
}
