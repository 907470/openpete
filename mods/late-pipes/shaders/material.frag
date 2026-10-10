#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

/* Faint green lift on whatever the selector claimed. */
vec4 op_material(vec4 base)
{
    return vec4(mix(base.rgb, vec3(0.2, 1.0, 0.3), 0.15), base.a);
}
