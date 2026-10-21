/**
 * @file scale_decal.frag
 * @brief Scale the colour per channel.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_decal.glsl"

/** @brief The row's params, as std140. */
layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 gain;     /**< rgb = per-channel factor */
};

/** @brief Multiply base.rgb by gain.rgb; alpha passes through. */
vec4 op_material(vec4 base)
{
    return vec4(base.rgb * gain.rgb, base.a);
}
