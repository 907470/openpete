/* openpete_psx_material.glsl — the material-tier shading contract.
 *
 * A material replaces the COLOUR of geometry the engine already decided to
 * draw. It does not replace the fragment stage: the engine owns main(), and
 * the mod supplies one function.
 *
 *     #version 450
 *     #extension GL_GOOGLE_include_directive : require
 *     #include "openpete_psx_material.glsl"
 *
 *     layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };
 *
 *     vec4 op_material(vec4 base)      // base = faithful stock shading
 *     {
 *         return vec4(base.rgb * tint.rgb, base.a);
 *     }
 *
 * That is the whole shader. No main(), no outColor, no discard: the engine
 * writes the output, and it discards the PSX transparent-texel sentinel
 * (base.a == 0) for you — return it unchanged and the hole stays a hole.
 *
 * WHY the inversion. The stock fragment stage does more than colour maths:
 * it divides by the perspective ratio, and it runs og's per-pixel painter
 * verdicts (the OT-key contest, the prop EMBED rule, the same-moby rank
 * select) which decide WHICH surface exists at a pixel. A shader that owns
 * main() replaces all of that silently — and every rule the engine adds
 * later breaks every material already shipped. With main() engine-side, a
 * material cannot lose engine behaviour it never knew about.
 *
 * WHAT YOU GET in base: the perspective divide, the 4/8/16bpp VRAM decode
 * through the CLUT, the texel * vertexColour / 128 modulation, and any
 * active HD texture-pack replacement (with its fog affine). Untextured
 * draws arrive as the interpolated vertex colour, so one op_material serves
 * both the textured and the gouraud paths.
 *
 * WHAT YOU INHERIT and cannot change: blend equation, depth test/write,
 * stencil, painter slot, and the occlusion verdicts above. A material
 * changes colour maths and nothing else — so a tint on an ADDITIVE draw
 * reads as added light rather than as a repaint, and mixing rather than
 * replacing is usually what you want.
 *
 * NOT for postfx shaders: set 0 binding 0 is the VRAM texture here, but the
 * previous-pass image in the postfx tier (openpete_shader_api.glsl). A
 * shader includes one contract or the other, never both.
 *
 * Reserved for the engine: push constants, every descriptor set except set 3
 * binding 1, and the input locations. A material's own uniform block is set 3
 * binding 1 (std140) — the one part of the layout that is yours.
 *
 * TWO ENGINE HALVES, ONE MOD HALF. Terrain does not reach the fragment stage
 * the way the rest of the scene does: its geometry is baked into GPU arenas
 * and redrawn without the CPU walking it, so the per-draw material arrives as
 * flat varyings rather than push constants and the HD texture pack folds
 * through an atlas rather than a per-draw image. The engine therefore
 * compiles a material's source TWICE, defining OP_PROFILE_TERRAIN for the
 * retained pipelines, and this file supplies a matching main() and shading
 * helpers for each. The mod's source is byte-identical between them, and a
 * material's colour must be too: shading is a function of the face, never of
 * where the face happens to live. */

#ifndef OPENPETE_PSX_MATERIAL_GLSL
#define OPENPETE_PSX_MATERIAL_GLSL

#ifdef OP_PROFILE_TERRAIN
/* ---- engine half: the retained terrain pipelines --------------------- */

#include "terrain_shade.glsl"

layout(location = 0) out vec4 op_out_color;

/* Perspective-correct texel coords within the texture page. */
vec2 op_uv()    { return vUV / vQ; }
/* Perspective-correct per-vertex modulate colour (R8G8B8A8 -> 0..1). */
vec4 op_color() { return vColor / vQ; }

uint op_vram_at(int x, int y)  { return ter_vram_at(x, y); }
float op_expand5(uint v)       { return ter_expand5(v); }
vec4 op_decode555(uint p)      { return ter_decode555(p); }

/* PSX texture modulation: texel * vertexColour / 128 (128 = unchanged). */
vec3 op_psx_modulate(vec3 texel, vec3 vcol)
{
    return clamp(texel * vcol * (255.0 / 128.0), 0.0, 1.0);
}

/* Raw texel at page-relative texel coords, through this FACE's page and
 * CLUT (vMat, from the vertex stage). Untextured faces have no page, and
 * report the transparent sentinel. */
vec4 op_psx_texel(ivec2 uv)
{
    uint mat0 = vMat.x;
    if ((mat0 & (1u << 23)) == 0u) return vec4(0.0);
    int  tpx  = int(mat0 & 0x3FFu);
    int  tpy  = int((mat0 >> 10) & 0x1FFu);
    uint bppc = (mat0 >> 19) & 3u;
    int  clx  = int(vMat.y & 0xFFFFu);
    int  cly  = int(vMat.y >> 16);
    uint p;
    if (bppc == 2u) {          /* 16bpp direct */
        p = ter_vram_at(tpx + uv.x, tpy + uv.y);
    } else if (bppc == 1u) {   /* 8bpp CLUT */
        uint w   = ter_vram_at(tpx + (uv.x >> 1), tpy + uv.y);
        uint idx = (w >> (8 * (uv.x & 1))) & 0xFFu;
        p = ter_vram_at(clx + int(idx), cly);
    } else {                   /* 4bpp CLUT */
        uint w   = ter_vram_at(tpx + (uv.x >> 2), tpy + uv.y);
        uint nib = (w >> (4 * (uv.x & 3))) & 0xFu;
        p = ter_vram_at(clx + int(nib), cly);
    }
    return ter_decode555(p);
}

/* The stock result for this fragment, VRAM only — no texture-pack fold.
 * Untextured faces are the vertex colour alone. */
vec4 op_psx_shade()
{
    vec4 col = op_color();
    if ((vMat.x & (1u << 23)) == 0u) return vec4(col.rgb, 1.0);
    vec4 c = op_psx_texel(ivec2(op_uv()));
    return vec4(op_psx_modulate(c.rgb, col.rgb), c.a);
}

/* True when the active texture pack replaces this face's texture. On the
 * retained path a replacement is a FOLD entry baked into the face by the
 * sector build, not a per-draw image. */
bool op_pack_active() { return (vHd & 0xFFFFu) != 0u; }

/* Raw replacement texel at page-texel coords (op_uv() space). The pack
 * file's own resolution and filter; true per-texel alpha; NO fog or
 * modulation. Reports transparent when no replacement covers the face. */
vec4 op_pack_texel(vec2 page_uv)
{
    if ((vHd & 0xFFFFu) == 0u) return vec4(0.0);
    uint ei = ((vHd & 0xFFFFu) - 1u) * 4u;
    vec4 rect  = g.hd_ent[ei];
    vec4 arect = g.hd_ent[ei + 1u];
    vec2 t  = clamp((page_uv - rect.xy) / rect.zw, 0.0, 1.0);
    vec2 ht = 0.5 / arect.zw;      /* clamp-to-edge inside the shelf gutter */
    t = clamp(t, ht, 1.0 - ht);
    return texelFetch(u_hdatlas, ivec2(arect.xy + t * arect.zw), 0);
}

/* The scene result WITH the active texture pack — what the player would see
 * if this material weren't installed, and what the engine hands to
 * op_material(). */
vec4 op_pack_shade() { return ter_stock_shade(); }

/* ---- the mod's half -------------------------------------------------- */
vec4 op_material(vec4 base);

void main()
{
    /* Sector debug tints answer before anything else, mod included: they
     * exist to show what the engine believes, not what a mod paints. */
    vec4 c;
    if (ter_debug_shade(c)) { op_out_color = c; return; }
    /* og's painter verdict, exactly as the stock terrain stage runs it. */
    if (ter_key_loses()) discard;
    c = op_material(op_pack_shade());
    if (c.a == 0.0) discard;        /* PSX transparent-texel sentinel */
    op_out_color = c;
}

#else
/* ---- engine half: the walked scene pipelines ------------------------- */

#include "psx_key_contest.glsl"

layout(set = 0, binding = 0) uniform usampler2D op_vram; /* R16_UINT 1024x512 */

/* The active HD texture-pack replacement covering this draw (RGBA8), when
 * op_pc.hd != 0; a 1x1 placeholder otherwise. Sample through op_pack_*. */
layout(set = 1, binding = 0) uniform sampler2D op_pack_tex;

/* Engine occlusion inputs — not yours to read: main() has already applied
 * them by the time op_material() runs. */
layout(set = 2, binding = 0) uniform sampler2D op_key_terrain;
layout(set = 4, binding = 0) uniform sampler2D op_key_moby;
layout(set = 5, binding = 0) uniform sampler2D op_key_actor;
layout(set = 6, binding = 0) uniform sampler2D op_moby_rank;

/* Engine-selected depth encoding and key scope; see psx_key_contest.glsl. */
layout(constant_id = 0) const bool OP_DEPTH_AFFINE = false;
layout(constant_id = 3) const bool OP_MOBY_TRUESCOPE = true;

layout(push_constant) uniform op_psx_material {
    int tpage_x, tpage_y;   /* texture page origin (VRAM 16-bit-pixel units) */
    int clut_x,  clut_y;    /* CLUT origin (VRAM pixel units; paletted only) */
    int bpp;                /* 4, 8, or 16 — 0 marks an UNTEXTURED draw      */
    int hd;                 /* 1 = op_pack_tex holds this draw's replacement  */
    vec2 hd_org;            /* the pack FILE's texel box within the texture   */
    vec2 hd_size;           /*   page (origin / size, page texel coords)      */
    vec4 hd_fog_a;          /* CLUT-ladder fog affine for the replacement:    */
    vec4 hd_fog_b;          /*   c' = c*a + b (identity when unfogged); .rgb  */
} op_pc;

/* The vertex stage emits UV and colour PRE-MULTIPLIED by the perspective
 * ratio vQ, and vQ alongside them, so the rasterizer's screen-linear
 * interpolation reconstructs perspective-correct values after the divide.
 * Read them through op_uv() / op_color(), never raw. */
layout(location = 0) in vec2  vUV;     /* texel coords   x vQ (divide by vQ) */
layout(location = 1) in vec4  vColor;  /* modulate 0..1  x vQ (divide by vQ) */
layout(location = 2) in float vQ;      /* perspective ratio (psx_scene.vert) */
layout(location = 3) flat in float vKoff;  /* this face's og OT key channel  */
layout(location = 4) flat in vec2  vRank;  /* own walk rank, moby ordinal    */

layout(location = 0) out vec4 op_out_color;

/* Perspective-correct texel coords within the texture page. */
vec2 op_uv()    { return vUV / vQ; }
/* Perspective-correct per-vertex modulate colour (R8G8B8A8 -> 0..1). */
vec4 op_color() { return vColor / vQ; }

uint op_vram_at(int x, int y) { return texelFetch(op_vram, ivec2(x, y), 0).r; }

float op_expand5(uint v) { return float((v << 3) | (v >> 2)) / 255.0; }

/* 15-bit VRAM word -> RGBA. a: 0 = the PSX transparent sentinel (the engine
 * discards it), 0.5 = STP-set texel (semi-transparent), 1 = opaque. */
vec4 op_decode555(uint p)
{
    if (p == 0u) return vec4(0.0);
    return vec4(op_expand5( p        & 31u),
                op_expand5((p >> 5)  & 31u),
                op_expand5((p >> 10) & 31u),
                1.0 - 0.5 * float((p >> 15) & 1u));
}

/* Raw texel at page-relative texel coords (this draw's bpp + CLUT).
 * Pass op_uv() for this fragment's own texel. */
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

/* The stock scene result for this fragment, VRAM only. bpp 0 marks an
 * UNTEXTURED (flat/gouraud) draw — the vertex colour alone, no texel fetch
 * and no /128 modulation. */
vec4 op_psx_shade()
{
    vec4 col = op_color();
    if (op_pc.bpp == 0) return vec4(col.rgb, 1.0);
    vec4 c = op_psx_texel(ivec2(op_uv()));
    return vec4(op_psx_modulate(c.rgb, col.rgb), c.a);
}

/* ---- HD texture-pack access ------------------------------------------ */

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

/* The scene result WITH the active texture pack — what the player would see
 * if this material weren't installed. Falls back to op_psx_shade() when no
 * replacement covers the draw, so it is always safe as the base layer, and
 * it is what the engine hands to op_material(). */
vec4 op_pack_shade()
{
    if (op_pc.hd == 0) return op_psx_shade();
    vec4 c = op_pack_texel(op_uv());
    if (c.a < 0.5) return vec4(0.0);
    c.rgb = clamp(c.rgb * op_pc.hd_fog_a.rgb + op_pc.hd_fog_b.rgb, 0.0, 1.0);
    return vec4(op_psx_modulate(c.rgb, op_color().rgb), 1.0);
}

/* ---- the mod's half --------------------------------------------------
 * Supplied by the material. base = op_pack_shade(); return the colour to
 * write, keeping base.a unless you mean to change transparency. */
vec4 op_material(vec4 base);

void main()
{
    /* og's painter verdicts, exactly as the stock stages run them: which
     * surface exists at this pixel is the engine's call, not the mod's. */
    bool ranked = vRank.x > 0.5;
    float own = op_vz_from_depth(gl_FragCoord.z, OP_DEPTH_AFFINE);
    if (ranked &&
        op_rank_loses(texelFetch(op_moby_rank, ivec2(gl_FragCoord.xy), 0), vRank, own))
        discard;
    if (!ranked && abs(vKoff) > 0.25) {
        if (op_key_loses(
                op_key_from_stored(texelFetch(op_key_terrain, ivec2(gl_FragCoord.xy), 0).r),
                op_key_from_stored(texelFetch(op_key_actor,   ivec2(gl_FragCoord.xy), 0).r),
                op_key_from_stored(texelFetch(op_key_moby,    ivec2(gl_FragCoord.xy), 0).r),
                own, vKoff,
                vRank.x < -0.5,     /* unranked moby faces mark rank -1 */
                OP_MOBY_TRUESCOPE))
            discard;
    }
    vec4 c = op_material(op_pack_shade());
    if (c.a == 0.0) discard;        /* PSX transparent-texel sentinel */
    op_out_color = c;
}

#endif /* OP_PROFILE_TERRAIN */

#endif /* OPENPETE_PSX_MATERIAL_GLSL */
