/**
 * @file tint_table.frag
 * @brief tint.frag plus a read of its own storage buffer at set 3 binding 2.
 *
 * The contract set's binding 2 holds a zero buffer for a shader whose
 * tables were not moved, so the read adds 0. Declaring an interface
 * besides the block at set 3 binding 1 makes the shader unlinkable: it
 * draws alone and never enters a combined module.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

/** @brief The row's params, as std140. */
layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /**< rgb = colour, a = strength 0..1 */
};

/** @brief A buffer of its own at the table binding; reads as zero. */
layout(set = 3, binding = 2, std430) readonly buffer chain_unlinkable_tab {
    vec4 zero;
};

/** @brief Mix base.rgb toward tint.rgb by tint.a, plus the buffer's zero. */
vec4 op_material(vec4 base)
{
    return vec4(mix(base.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0)) + zero.rgb, base.a);
}
