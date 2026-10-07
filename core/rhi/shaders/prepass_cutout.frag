#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_clut.glsl"
#include "psx_texel.glsl"
#include "prepass_pack.glsl"

/* Depth pre-pass fragment for the scene's WORLD occluders in the colour run
 * (prepass.vert), drawn per material run. The scene draw
 * (psx_scene.frag, psx_scene_key.frag) discards a texel whose VRAM word is
 * the PSX transparent sentinel 0, so a textured face paints only its opaque
 * texels and whatever lies behind the holes shows through. This stage applies
 * the same cut at the same texel, so an occluder exists only where the face
 * paints. bpp < 0 marks an untextured run or a material-run overflow tail,
 * drawn over its whole footprint. The depth is prepass.vert's fixed-function
 * 1 - 1/W, so the depth run's full-footprint redraw of a face writes the
 * identical value. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;
    int clut_x,  clut_y;
    int bpp;
} pc;

layout(location = 1) in vec2 vUV;
layout(location = 3) in float vQ;

layout(location = 0) out vec4 outColor;

void main()
{
    vec2 uv = vUV / vQ;
    ivec2 ti = op_texel(uv, op_texel_grad(uv));
    if (pc.bpp >= 0 &&
        op_texel_word(u_vram, ivec2(pc.tpage_x, pc.tpage_y),
                      ivec2(pc.clut_x, pc.clut_y), pc.bpp, ti.x, ti.y) == 0u)
        discard;
    outColor = op_prepass_pack_depth(gl_FragCoord.z);
}
