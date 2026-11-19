#version 450
#extension GL_GOOGLE_include_directive : require
#include "terrain_shade.glsl"
#include "prepass_pack.glsl"

/* Depth pre-pass fragment for the retained terrain's WORLD faces. The main
 * pass (terrain.frag) discards a texel ter_stock_shade returns transparent:
 * the PSX sentinel 0 of a VRAM texel, or a cut-out texel of an HD-fold
 * entry. This stage keeps every fragment, so the depth attachment takes the
 * face's whole footprint, and writes the packed depth at alpha 1 where the
 * main pass paints and alpha 0 on a transparent texel; the pipeline blends
 * SRC_ALPHA, so the colour attachment changes only where the face paints.
 * Untextured faces paint their footprint. */

layout(location = 0) out vec4 outColor;

void main()
{
    ter_texel_grad_init();
    outColor = vec4(op_prepass_pack_depth(gl_FragCoord.z).rgb,
                    ter_stock_shade().a == 0.0 ? 0.0 : 1.0);
}
