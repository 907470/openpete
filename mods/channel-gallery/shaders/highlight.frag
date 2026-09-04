/**
 * @file highlight.frag
 * @brief Highlight whatever this material was selected onto.
 *
 * The gallery makes selection visible, so the shading is a plain tint.
 * What varies from row to row is the selector that decides which
 * geometry reaches this function, not the function.
 *
 * base is the engine's own result for the fragment: stock decode or an
 * installed HD texture pack's replacement, with the vertex modulation
 * applied. A highlighted face therefore still matches its neighbours in
 * texture and lighting, and an untextured draw arrives as its
 * interpolated vertex colour. One function serves every channel.
 *
 * The tint is mixed, not written over the base, so geometry stays
 * readable. On the blended channels an additive draw forced to a flat
 * colour saturates.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

/** @brief Pushed by the C side with material_set_params, std140. */
layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /**< rgb = highlight colour, a = strength 0..1 */
};

/** @brief The material function; alpha passes through to the group's blend. */
vec4 op_material(vec4 base)
{
    return vec4(mix(base.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0)), base.a);
}
