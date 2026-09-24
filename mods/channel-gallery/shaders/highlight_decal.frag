/**
 * @file highlight_decal.frag
 * @brief The gallery's highlight on the decal variant of the material
 *        contract.
 *
 * Moby drop-shadows draw through a pass whose stock fragment stage
 * decides per pixel whether the shadow is occluded: it reconstructs the
 * terrain height from a depth pre-pass and rejects walls and steep faces,
 * and rejects terrain whose ordering key places it after the shadow in
 * the original paint order. That verdict is the engine's, applied by main()
 * before this function runs, so a material on this path cannot paint
 * through a wall.
 *
 * The only difference from highlight.frag is the include. Same
 * function, same body: the two contracts differ in which draw path they
 * wrap, not in what a mod writes.
 *
 * The draw blends subtractively, so a brighter tint darkens the ground
 * more. The row reads inverted compared with the opaque channels; that
 * is the material inheriting its group's compositing.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_decal.glsl"

/** @brief Pushed by the C side with material_set_params, std140. */
layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /**< rgb = highlight colour, a = strength 0..1 */
};

/** @brief The material function; alpha passes through to the group's blend. */
vec4 op_material(vec4 base)
{
    return vec4(mix(base.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0)), base.a);
}
