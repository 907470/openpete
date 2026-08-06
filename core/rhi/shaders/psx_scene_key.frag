#version 450

/* Textured OPAQUE scene fragment stage with the terrain KEY-RESOLVE contest.
 *
 * Identical PSX VRAM decode + modulation to psx_scene.frag, plus: a keyed
 * terrain fragment (vKoff > 0, the face's ABSOLUTE og OT key + 0.5 sentinel)
 * survives only where no strictly-nearer key owns the pixel in the key
 * image. The og key of a face is TRUE view-Z + 128*slots (the authored OT
 * offsets); stored depth itself is TRUE view-Z — og's keys decide WHICH
 * terrain surface exists at a pixel, true geometry decides everything
 * against that surface. The face's own key rides the flat vKoff (exact);
 * only the image side is quantised. Non-terrain fragments (vKoff == 0)
 * never touch the sampler. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */
layout(set = 2, binding = 0) uniform sampler2D u_hpkey; /* HP og-key depth image */

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;
    int clut_x,  clut_y;
    int bpp;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vQ;
layout(location = 3) flat in float vKoff;

layout(location = 0) out vec4 outColor;

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
    if (vKoff > 0.25) {   /* terrain face: contest the og key raster */
        float hk = texelFetch(u_hpkey, ivec2(gl_FragCoord.xy), 0).r;
        if (hk < 0.999999) {   /* clear 1.0 = nothing keyed this pixel */
            /* Keys are FLAT per face (the .s OT index scaled to world units:
             * corner-mean view-Z + 128*slot bias) and the image rasters them
             * as constants, so both sides are exact — no interpolation, no
             * encoding coupling. Decode matches terrain_key.vert's canonical
             * z = kv/(kv+4096) (affine 1-1/kv quantises past the 64-world
             * margin beyond kv~46k — far faces discarded themselves).
             * Strict less-by-half-slot: a nearer-KEYED face owns the pixel
             * (og's painter order); ties inside og's own 128-world quantum
             * fall through to true depth. */
            if (4096.0 * hk / max(1.0 - hk, 1e-9) < (vKoff - 0.5) - 64.0) discard;
        }
    }
    vec2 uv = vUV / vQ;
    vec4 col = vColor / vQ;
    vec4 c = decode555(sample_word(int(uv.x), int(uv.y)));
    if (c.a == 0.0) discard;
    vec3 rgb = clamp(c.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = vec4(rgb, c.a);
}
