/**
 * @file stock.frag
 * @brief Identity material: writes the colour the engine would have written.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

/**
 * @brief The material function.
 * @param base The engine's colour.
 * @return @p base unchanged.
 */
vec4 op_material(vec4 base)
{
    return base;
}
