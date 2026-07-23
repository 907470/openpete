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
 * Reserved for the engine: push constants, set 0, and input locations
 * 0-1. A material's own uniform block is set 3 binding 1 (std140). */

#ifndef OPENPETE_PSX_MATERIAL_GLSL
#define OPENPETE_PSX_MATERIAL_GLSL

layout(set = 0, binding = 0) uniform usampler2D op_vram; /* R16_UINT 1024x512 */

layout(push_constant) uniform op_psx_material {
    int tpage_x, tpage_y;   /* texture page origin (VRAM 16-bit-pixel units) */
    int clut_x,  clut_y;    /* CLUT origin (VRAM pixel units; paletted only) */
    int bpp;                /* 4, 8, or 16 */
} op_pc;

layout(location = 0) in vec2 vUV;     /* texel coords within the texture page */
layout(location = 1) in vec4 vColor;  /* per-vertex modulate (R8G8B8A8 -> 0..1) */

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

/* Raw texel at page-relative texel coords (this draw's bpp + CLUT). */
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
    if (op_pc.bpp == 0) return vec4(vColor.rgb, 1.0);
    vec4 c = op_psx_texel(ivec2(vUV));
    return vec4(op_psx_modulate(c.rgb, vColor.rgb), c.a);
}

#endif /* OPENPETE_PSX_MATERIAL_GLSL */
