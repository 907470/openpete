#version 450

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
 * via (v<<3)|(v>>2) — the standard faithful expansion, rather than PsyCross's /32 darkening. Pixel exactness
 * here is cosmetic: the oracle proves gameplay state, never pixels. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;   /* texture page origin (VRAM 16-bit-pixel units) */
    int clut_x,  clut_y;    /* CLUT origin (VRAM pixel units; paletted only) */
    int bpp;                /* 4, 8, or 16 */
} pc;

layout(location = 0) in vec2 vUV;     /* texel coords within the texture */
layout(location = 1) in vec4 vColor;  /* modulate */

layout(location = 0) out vec4 outColor;

uint vram_at(int x, int y) { return texelFetch(u_vram, ivec2(x, y), 0).r; }

/* 5-bit -> 8-bit, normalized. (v<<3)|(v>>2) replicates the high bits. */
float expand5(uint v) { return float((v << 3) | (v >> 2)) / 255.0; }

/* PSX BGR555 + STP -> linear RGBA. 0 is the transparent sentinel. */
vec4 decode555(uint p)
{
    if (p == 0u) return vec4(0.0);
    return vec4(expand5( p        & 31u),
                expand5((p >> 5)  & 31u),
                expand5((p >> 10) & 31u),
                1.0 - 0.5 * float((p >> 15) & 1u)); /* STP set => 50% alpha */
}

/* Fetch the raw 16-bit color for texel (u,v), resolving CLUT for 4/8bpp.
 * Two 8-bit indices per VRAM word (low byte first); four 4-bit indices per
 * word (low nibble first) — exactly the PSX packing. */
uint sample_word(int u, int v)
{
    if (pc.bpp == 16) {
        return vram_at(pc.tpage_x + u, pc.tpage_y + v);
    } else if (pc.bpp == 8) {
        uint word = vram_at(pc.tpage_x + (u >> 1), pc.tpage_y + v);
        uint idx  = (word >> (8 * (u & 1))) & 0xFFu;
        return vram_at(pc.clut_x + int(idx), pc.clut_y);
    } else { /* 4bpp */
        uint word = vram_at(pc.tpage_x + (u >> 2), pc.tpage_y + v);
        uint nib  = (word >> (4 * (u & 3))) & 0xFu;
        return vram_at(pc.clut_x + int(nib), pc.clut_y);
    }
}

void main()
{
    vec4 c = decode555(sample_word(int(vUV.x), int(vUV.y)));
    if (c.a == 0.0) discard;
    outColor = c * vColor;
}
