/**
 * @file tint_decal.frag
 * @brief Mix the stock colour toward one colour by a strength.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_decal.glsl"

/** @brief The row's params, as std140. */
layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /**< rgb = colour, a = strength 0..1 */
};

/** @brief Mix base.rgb toward tint.rgb by tint.a; alpha passes through. */
vec4 op_material(vec4 base)
{
    return vec4(mix(base.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0)), base.a);
}
