/* psx_clut.glsl - the PSX texel decode and the fogged-CLUT row mix, shared by
 * the walked scene fragment stages (psx_scene.frag, psx_scene_key.frag) and
 * the scene half of the mod material contract (openpete_psx_material.glsl).
 * The functions take the VRAM image (R16_UINT 1024x512) as an argument, since
 * each includer declares it at its own set. */

#ifndef OPENPETE_PSX_CLUT_GLSL
#define OPENPETE_PSX_CLUT_GLSL

float op_expand5(uint v) { return float((v << 3) | (v >> 2)) / 255.0; }

/* 15-bit VRAM word -> RGBA. a: 0 = the PSX transparent sentinel, which the
 * engine discards; 0.5 = STP-set (semi-transparent) texel; 1 = opaque. */
vec4 op_decode555(uint p)
{
    if (p == 0u) return vec4(0.0);
    return vec4(op_expand5( p        & 31u),
                op_expand5((p >> 5)  & 31u),
                op_expand5((p >> 10) & 31u),
                1.0 - 0.5 * float((p >> 15) & 1u));
}

/* The CLUT index of a paletted texel (4 or 8 bpp) at page-relative texel
 * coords (u, v) of the texture page at tpage. */
uint op_clut_index(usampler2D vram, ivec2 tpage, int bpp, int u, int v)
{
    if (bpp == 8) {
        uint word = texelFetch(vram, ivec2(tpage.x + (u >> 1), tpage.y + v), 0).r;
        return (word >> (8 * (u & 1))) & 0xFFu;
    }
    uint word = texelFetch(vram, ivec2(tpage.x + (u >> 2), tpage.y + v), 0).r;
    return (word >> (4 * (u & 3))) & 0xFu;
}

/* Second fogged-CLUT row of a textured face (the moby fog band between two
 * ticks). rgb is the face's modulated, clamped colour and col its vertex
 * colour. The vertex alpha byte, constant over the face and unread otherwise
 * on a textured face, is 0xFF for none, else ((d + 7) << 4) | q: the result
 * blends by q / 30 (q 1..15, weight up to 0.5) toward the modulated, clamped
 * colour of the same CLUT index d rows from the face's CLUT (d in -7..7), so
 * it is the lerp of the two rows' drawn colours, clamp included. High nibble
 * 15 or q 0 returns rgb, the path every other textured face takes, as does a
 * 16bpp face. A transparent (0) word in the second row keeps rgb. */
vec3 op_clut_fog_mix(usampler2D vram, vec3 rgb, vec4 col, ivec2 tpage, ivec2 clut,
                     int bpp, int u, int v)
{
    uint code = uint(round(clamp(col.a, 0.0, 1.0) * 255.0));
    uint q = code & 15u, hi = code >> 4;
    if (bpp == 16 || hi == 15u || q == 0u) return rgb;
    uint idx = op_clut_index(vram, tpage, bpp, u, v);
    uint w2 = texelFetch(vram, ivec2(clut.x + int(idx), clut.y + int(hi) - 7), 0).r;
    if (w2 == 0u) return rgb;
    vec3 rgb2 = clamp(op_decode555(w2).rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0);
    return mix(rgb, rgb2, float(q) / 30.0);
}

#endif /* OPENPETE_PSX_CLUT_GLSL */
