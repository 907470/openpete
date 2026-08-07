#version 450

/* TEXTURED key-pass fragment stage: alpha-aware og-key rasterisation.
 *
 * A keyed face's og OT occupancy exists only where its OPAQUE texels land —
 * the PSX p==0 sentinel is fully transparent on every prim, so whatever sits
 * behind those texels stays visible in the original painter. Rasterising a
 * textured face's key across its full footprint (the untextured key pipe's
 * behaviour) lets a mostly-transparent face — a cloak edge, a fence lattice —
 * stamp a near key over its whole quad and key-discard everything behind the
 * holes, leaving nothing to draw there but the backdrop.
 *
 * Depth comes from terrain_key.vert (flat og key for the key image, true
 * per-vertex depth for the prop image); this stage only cuts transparent
 * texels out of the raster. The key targets are depth-only, so outColor
 * lands nowhere. Texel decode mirrors psx_scene_key.frag; UVs interpolate
 * against the position's own W (flat key => screen-affine, the PSX
 * rasteriser's own sampling; true-depth prop faces get perspective UV,
 * matching the scene draw). */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;
    int clut_x,  clut_y;
    int bpp;
} pc;

layout(location = 1) in vec2 vUV;   /* u/w — see terrain_key.vert */
layout(location = 3) in float vQ;   /* 1/w */

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
    outColor = vec4(0.0);
}
