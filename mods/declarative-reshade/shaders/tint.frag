/**
 * @file tint.frag
 * @brief Push the stock look toward one colour.
 *
 * The engine owns main() and calls op_material once per fragment with
 * the colour it was about to write. That is the whole material
 * contract: a mod supplies one function, and everything else about the
 * draw (texturing, vertex modulation, HD texture-pack replacement,
 * blending, depth) stays the engine's. So a shader mod with no C is one
 * function long, and this file plus a [[material]] row is a complete
 * mod.
 *
 * The tint is mixed in, not written over the base. On a blended channel
 * the material inherits the draw group's blend equation; forcing a flat
 * colour into an additive draw saturates it. Keeping the base visible
 * is what makes the result read as a tint.
 *
 * A row with no params gets a zeroed block, so tint.a is 0 and the
 * output equals the base. "No parameters" therefore means "stock look",
 * which is the safe default for a row a manifest author has not
 * finished filling in.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

/** @brief The [[material]] row's params, as std140. */
layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /**< rgb = colour, a = strength 0..1 */
};

/**
 * @brief The material function.
 * @param base The colour the engine would have written: stock decode or
 *             the HD texture-pack replacement, vertex modulation applied.
 * @return The colour to write. Alpha is passed through so the group's
 *         blend sees the value it expects.
 */
vec4 op_material(vec4 base)
{
    return vec4(mix(base.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0)), base.a);
}
