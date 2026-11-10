/* A member that computes in double: its module declares the Float64
 * capability, which the device is created without, so it is refused. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

vec4 op_material(vec4 base)
{
    double k = double(base.r) * 0.5lf;
    return vec4(float(k), base.gba);
}
