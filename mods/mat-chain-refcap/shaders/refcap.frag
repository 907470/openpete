/**
 * @file refcap.frag
 * @brief The four highest-ranked rows of mat-chain-cap applied lowest first, written as one function.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

/** @brief The four rows' params, lowest-ranked first, as std140. */
layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 t0;
    vec4 t1;
    vec4 t2;
    vec4 t3;
};

/** @brief One tint step. */
vec4 step_tint(vec4 c, vec4 t)
{
    return vec4(mix(c.rgb, t.rgb, clamp(t.a, 0.0, 1.0)), c.a);
}

/** @brief Four tint steps. */
vec4 op_material(vec4 base)
{
    return step_tint(step_tint(step_tint(step_tint(base, t0), t1), t2), t3);
}
