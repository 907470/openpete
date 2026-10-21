/**
 * @file ref2.frag
 * @brief mat-chain-b's scale applied to mat-chain-a's tint, written as one function.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

/** @brief Both members' params, as std140. */
layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /**< mat-chain-a's params */
    vec4 gain;     /**< mat-chain-b's params */
};

/** @brief tint, then scale. */
vec4 op_material(vec4 base)
{
    vec4 a = vec4(mix(base.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0)), base.a);
    return vec4(a.rgb * gain.rgb, a.a);
}
