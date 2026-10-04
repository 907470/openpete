#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_clut.glsl"

/* PSX VRAM sampler, fragment stage.
 *
 * A faithful, integer-exact port of PsyCross's 4/8/16bpp + CLUT decoder
 * (third_party/PsyCross/src/render/PsyX_render.cpp, samplePSX / decodeRG),
 * reading a clean R16_UINT VRAM image via texelFetch instead of PsyCross's
 * GL_RG32F byte-pack hack. One uint per PSX VRAM pixel, so nibble/byte
 * extraction and CLUT lookups are exact integer ops with no float fudge.
 *
 * Decode is PSX 16-bit "BGR555 + STP": bits 0-4 R, 5-9 G, 10-14 B, 15 STP
 * (semi-transparent). A stored value of 0 is the transparent sentinel
 * (PSX texture convention) and is discarded. 5-bit channels expand to 8-bit
 * via (v<<3)|(v>>2) - the standard faithful expansion, rather than
 * PsyCross's /32 darkening. Pixel exactness here is cosmetic: the oracle
 * proves gameplay state, never pixels. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;   /* texture page origin (VRAM 16-bit-pixel units) */
    int clut_x,  clut_y;    /* CLUT origin (VRAM pixel units; paletted only) */
    int bpp;                /* 4, 8, or 16 */
} pc;

layout(location = 0) in vec2 vUV;     /* texel coords within the texture */
layout(location = 1) in vec4 vColor;  /* modulate */

layout(location = 0) out vec4 outColor;

/* The raw 16-bit colour of texel (u, v) through this draw's bpp and CLUT
 * (op_texel_word). */
uint sample_word(int u, int v)
{
    return op_texel_word(u_vram, ivec2(pc.tpage_x, pc.tpage_y),
                         ivec2(pc.clut_x, pc.clut_y), pc.bpp, u, v);
}

void main()
{
    vec4 c = op_decode555(sample_word(int(vUV.x), int(vUV.y)));
    if (c.a == 0.0) discard;
    outColor = c * vColor;
}
