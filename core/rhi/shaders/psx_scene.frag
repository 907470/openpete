#version 450

/* Phase 3a.3 — textured native-scene fragment stage.
 *
 * Same PSX 4/8/16bpp + CLUT VRAM decode as psx_sample.frag, but with the PSX
 * TEXTURE-MODULATION rule for gouraud-textured polys: out = texel * color / 128
 * (8-bit), i.e. a vertex colour of 128 means "unchanged", 255 ~2x, 0 black.
 * In normalised floats that is texel * color_n * (255/128), clamped. Using the
 * /255 of psx_sample.frag here would darken every textured moby by ~half (the
 * modulation-brightness bug).
 *
 * Determinism-irrelevant: pixel output only. Semi-transparency (STP + the 4 ABR
 * modes) is a later 3a.3 step; here the STP bit only affects the discard
 * sentinel, and the pipeline blends opaque. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;   /* texture page origin (VRAM 16-bit-pixel units) */
    int clut_x,  clut_y;    /* CLUT origin (VRAM pixel units; paletted only) */
    int bpp;                /* 4, 8, or 16 */
} pc;

layout(location = 0) in vec2 vUV;     /* texel coords x ratio (divide by vQ) */
layout(location = 1) in vec4 vColor;  /* per-vertex modulate x ratio (divide by vQ) */
layout(location = 2) in float vQ;     /* perspective ratio (see psx_scene.vert) */

layout(location = 0) out vec4 outColor;

/* Untextured lighting view (rhi_set_notex): replace the texel colour with the
 * modulation-neutral grey (128/255) so out = the raw gouraud vertex colour —
 * texel alpha/discard keep real silhouettes. */
layout(constant_id = 2) const bool NOTEX = false;

uint vram_at(int x, int y) { return texelFetch(u_vram, ivec2(x, y), 0).r; }

float expand5(uint v) { return float((v << 3) | (v >> 2)) / 255.0; }

vec4 decode555(uint p)
{
    if (p == 0u) return vec4(0.0);            /* PSX transparent sentinel */
    return vec4(expand5( p        & 31u),
                expand5((p >> 5)  & 31u),
                expand5((p >> 10) & 31u),
                1.0 - 0.5 * float((p >> 15) & 1u));
}

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
    vec4 col = vColor / vQ;
    vec4 c = decode555(sample_word(int(uv.x), int(uv.y)));
    if (c.a == 0.0) discard;
    if (NOTEX) c.rgb = vec3(128.0 / 255.0);
    /* PSX texture modulation: texel * vertexColour / 128 (clamped). */
    vec3 rgb = clamp(c.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = vec4(rgb, c.a);
}
