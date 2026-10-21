/**
 * @file lift.frag
 * @brief Add a constant to the colour.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

/** @brief The row's params, as std140. */
layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 lift;     /**< rgb = added per channel */
};

/** @brief Add lift.rgb to base.rgb; alpha passes through. */
vec4 op_material(vec4 base)
{
    return vec4(base.rgb + lift.rgb, base.a);
}
