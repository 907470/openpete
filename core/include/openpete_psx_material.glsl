/* openpete_psx_material.glsl — the material-tier shading contract.
 *
 * Include from a MATERIAL fragment shader (material_register) with:
 *
 *     #version 450
 *     #extension GL_GOOGLE_include_directive : require
 *     #include "openpete_psx_material.glsl"
 *
 * and write main() in terms of op_psx_shade():
 *
 *     layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };
 *     layout(location = 0) out vec4 outColor;
 *     void main() {
 *         vec4 c = op_psx_shade();          // faithful stock shading
 *         if (c.a == 0.0) discard;          // PSX transparent sentinel
 *         outColor = vec4(c.rgb * tint.rgb, c.a);
 *     }
 *
 * This brings in the textured-scene interface (VRAM sampler, PSX material
 * push constants, vUV/vColor inputs) plus the stock PSX decode — 4/8/16bpp
 * texel fetch through the CLUT and the texel * vertexColour / 128
 * modulation rule — as functions, so a material builds ON the faithful
 * base instead of forking it. Forks of psx_scene.frag keep compiling
 * against whatever decode they copied; shaders built on this include are
 * rebuilt by the engine whenever the contract changes (its bytes are
 * hashed into every mod shader's build key).
 *
 * NOT for postfx shaders: set 0 binding 0 is the VRAM texture here, but
 * the previous-pass image in the postfx tier (openpete_shader_api.glsl).
 * A shader includes one contract or the other, never both.
 *
 * Reserved for the engine: push constants, sets 0-1, and input locations
 * 0-4 (the vertex stage writes more than this contract names — declaring
 * one of the unnamed slots reads engine state that is free to change).
 * A material's own uniform block is set 3 binding 1 (std140). */

#ifndef OPENPETE_PSX_MATERIAL_GLSL
#define OPENPETE_PSX_MATERIAL_GLSL

layout(set = 0, binding = 0) uniform usampler2D op_vram; /* R16_UINT 1024x512 */

/* The active HD texture-pack replacement covering this draw (RGBA8), when
 * op_pc.hd != 0; a 1x1 placeholder otherwise. Sample through op_pack_*. */
layout(set = 1, binding = 0) uniform sampler2D op_pack_tex;

layout(push_constant) uniform op_psx_material {
    int tpage_x, tpage_y;   /* texture page origin (VRAM 16-bit-pixel units) */
    int clut_x,  clut_y;    /* CLUT origin (VRAM pixel units; paletted only) */
    int bpp;                /* 4, 8, or 16 */
    int hd;                 /* 1 = op_pack_tex holds this draw's replacement  */
    vec2 hd_org;            /* the pack FILE's texel box within the texture   */
    vec2 hd_size;           /*   page (origin / size, page texel coords)      */
    vec4 hd_fog_a;          /* CLUT-ladder fog affine for the replacement:    */
    vec4 hd_fog_b;          /*   c' = c*a + b (identity when unfogged); .rgb  */
} op_pc;

/* The vertex stage emits UV and colour PRE-MULTIPLIED by the perspective
 * ratio vQ, and vQ alongside them, so the rasterizer's screen-linear
 * interpolation reconstructs perspective-correct values after the divide.
 * Read them through op_uv() / op_color(), never raw: the raw varyings are
 * only meaningful once divided, and they are wrong by a factor that grows
 * as geometry approaches the camera. */
layout(location = 0) in vec2 vUV;     /* texel coords   x vQ (divide by vQ) */
layout(location = 1) in vec4 vColor;  /* modulate 0..1  x vQ (divide by vQ) */
layout(location = 2) in float vQ;     /* perspective ratio (psx_scene.vert) */

/* Perspective-correct texel coords within the texture page. */
vec2 op_uv()    { return vUV / vQ; }
/* Perspective-correct per-vertex modulate colour (R8G8B8A8 -> 0..1). */
vec4 op_color() { return vColor / vQ; }

uint op_vram_at(int x, int y) { return texelFetch(op_vram, ivec2(x, y), 0).r; }

float op_expand5(uint v) { return float((v << 3) | (v >> 2)) / 255.0; }

/* 15-bit VRAM word -> RGBA. a: 0 = the PSX transparent sentinel (0x0000,
 * callers discard), 0.5 = STP-set texel (semi-transparent), 1 = opaque. */
vec4 op_decode555(uint p)
{
    if (p == 0u) return vec4(0.0);
    return vec4(op_expand5( p        & 31u),
                op_expand5((p >> 5)  & 31u),
                op_expand5((p >> 10) & 31u),
                1.0 - 0.5 * float((p >> 15) & 1u));
}

/* Raw texel at page-relative texel coords (this draw's bpp + CLUT).
 * Pass op_uv() for this fragment's own texel; the raw vUV varying is
 * still multiplied by the perspective ratio. */
vec4 op_psx_texel(ivec2 uv)
{
    uint word;
    if (op_pc.bpp == 16) {
        word = op_vram_at(op_pc.tpage_x + uv.x, op_pc.tpage_y + uv.y);
    } else if (op_pc.bpp == 8) {
        uint w   = op_vram_at(op_pc.tpage_x + (uv.x >> 1), op_pc.tpage_y + uv.y);
        uint idx = (w >> (8 * (uv.x & 1))) & 0xFFu;
        word = op_vram_at(op_pc.clut_x + int(idx), op_pc.clut_y);
    } else { /* 4bpp */
        uint w   = op_vram_at(op_pc.tpage_x + (uv.x >> 2), op_pc.tpage_y + uv.y);
        uint nib = (w >> (4 * (uv.x & 3))) & 0xFu;
        word = op_vram_at(op_pc.clut_x + int(nib), op_pc.clut_y);
    }
    return op_decode555(word);
}

/* PSX texture modulation: texel * vertexColour / 128 (128 = unchanged). */
vec3 op_psx_modulate(vec3 texel, vec3 vcol)
{
    return clamp(texel * vcol * (255.0 / 128.0), 0.0, 1.0);
}

/* The stock scene result for this fragment. bpp 0 marks an UNTEXTURED
 * (flat/gouraud) draw — the stock output is the vertex colour alone, no
 * texel fetch and no /128 modulation. Otherwise: sample + modulate;
 * a == 0.0 means the PSX transparent sentinel — the caller discards. */
vec4 op_psx_shade()
{
    vec4 col = op_color();
    if (op_pc.bpp == 0) return vec4(col.rgb, 1.0);
    vec4 c = op_psx_texel(ivec2(op_uv()));
    return vec4(op_psx_modulate(c.rgb, col.rgb), c.a);
}

/* ---- HD texture-pack access ------------------------------------------
 * A material override takes the draw away from the engine's replacement
 * path, so without these the shader only ever sees the stock VRAM decode
 * and any HD art the player installed for the same texture goes dark.
 * op_pack_shade() restores it: build the effect ON the pack's art the
 * same way op_psx_shade() builds on stock. */

/* True when the active texture pack replaces this draw's texture (and
 * op_pack_tex holds it). False: no pack, no matching file, or the file's
 * GPU upload hasn't completed (first sighting — one present of stock). */
bool op_pack_active() { return op_pc.hd != 0; }

/* Raw replacement texel at page-texel coords (op_uv() space). The pack
 * file's own resolution and filter; true per-texel alpha; NO fog or
 * modulation. */
vec4 op_pack_texel(vec2 page_uv)
{
    vec2 t = (page_uv - op_pc.hd_org) / op_pc.hd_size;
    return texture(op_pack_tex, clamp(t, 0.0, 1.0));
}

/* The scene result WITH the active texture pack: what the player would
 * see if this material weren't installed. Replacement sample + the
 * CLUT-ladder fog affine + the vertex modulate, with the opaque-scene
 * cutout rule (alpha < 0.5 -> a == 0.0, caller discards) — mirroring the
 * engine's own replacement pipe. Falls back to op_psx_shade() when no
 * replacement covers the draw, so it is always safe as the base layer. */
vec4 op_pack_shade()
{
    if (op_pc.hd == 0) return op_psx_shade();
    vec4 c = op_pack_texel(op_uv());
    if (c.a < 0.5) return vec4(0.0);
    c.rgb = clamp(c.rgb * op_pc.hd_fog_a.rgb + op_pc.hd_fog_b.rgb, 0.0, 1.0);
    return vec4(op_psx_modulate(c.rgb, op_color().rgb), 1.0);
}

#endif /* OPENPETE_PSX_MATERIAL_GLSL */
