#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"

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
 * matching the scene draw). A triangle with a drain record writes the key
 * of the drain sub-cell under the fragment (psx_key_contest.glsl) as its
 * depth; every other face writes the rasterised depth back unchanged. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;
    int clut_x,  clut_y;
    int bpp;
} pc;

layout(location = 1) in vec2 vUV;   /* u/w — see terrain_key.vert */
layout(location = 3) in float vQ;   /* 1/w */
layout(location = 4) flat in float vDrain; /* drain record tag */
layout(location = 5) flat in float vKv;    /* flat key view-Z */
/* Drain sub-cell key records (psx_key_contest.glsl op_drain_record). */
layout(set = 1, binding = 0) uniform usampler2D u_drain;

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
    /* bpp < 0: a material-run overflow tail, keyed full-footprint. */
    if (pc.bpp >= 0 && sample_word(int(uv.x), int(uv.y)) == 0u) discard;
    uvec4 t0, t1;
    vec2 p;
    gl_FragDepth = op_drain_record(u_drain, vDrain, uv, t0, t1, p)
        ? op_key_store(vKv + op_drain_delta(p, t0, t1, 0.0))
        : gl_FragCoord.z;
    outColor = vec4(0.0);
}
