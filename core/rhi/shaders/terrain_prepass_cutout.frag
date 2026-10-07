#version 450
#extension GL_GOOGLE_include_directive : require
#include "terrain_shade.glsl"
#include "prepass_pack.glsl"

/* Depth pre-pass fragment for the retained terrain's WORLD faces. The main
 * pass (terrain.frag) discards a texel ter_stock_shade returns transparent:
 * the PSX sentinel 0 of a VRAM texel, or a cut-out texel of an HD-fold
 * entry. The pre-pass applies the same cut, so an occluder exists only where
 * the face paints; untextured faces cover their footprint. */

layout(location = 0) out vec4 outColor;

void main()
{
    ter_texel_grad_init();
    if (ter_stock_shade().a == 0.0) discard;
    outColor = op_prepass_pack_depth(gl_FragCoord.z);
}
