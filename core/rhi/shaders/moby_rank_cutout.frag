#version 450

/* TEXTURED moby RANK pass fragment stage: alpha-aware rank rasterisation.
 * A face's og paint occupancy exists only where its OPAQUE texels land —
 * the PSX p==0 sentinel is fully transparent on every prim, so a textured
 * face (dragon eyes, cloak edges) must not stamp rank over texels its own
 * paint discards (see psx_key_cutout.frag for the terrain-key precedent).
 * Texel decode mirrors psx_scene_key.frag; rank/ordinal encode mirrors
 * moby_rank.frag. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;
    int clut_x,  clut_y;
    int bpp;
} pc;

layout(location = 0) flat in vec2 vRank;  /* x = rank (>=1), y = ordinal */
layout(location = 1) in vec2 vUV;   /* u/w — see moby_rank.vert */
layout(location = 2) in float vQ;   /* 1/w */

layout(location = 0) out vec4 outColor;

uint vram_at(int x, int y) { return texelFetch(u_vram, ivec2(x, y), 0).r; }

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
    vec2 uv = vUV / vQ;
    if (sample_word(int(uv.x), int(uv.y)) == 0u) discard;
    float r = clamp(vRank.x, 0.0, 65534.0);
    float o = clamp(vRank.y, 0.0, 255.0);
    outColor = vec4(mod(r, 256.0) / 255.0, floor(r / 256.0) / 255.0,
                    o / 255.0, 1.0);
}
