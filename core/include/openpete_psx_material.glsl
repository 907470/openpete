/* openpete_psx_material.glsl - the material shader contract.
 *
 * A material replaces the colour of geometry the engine has already decided
 * to draw. The engine owns main(); the mod supplies one function:
 *
 *     #version 450
 *     #extension GL_GOOGLE_include_directive : require
 *     #include "openpete_psx_material.glsl"
 *
 *     layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };
 *
 *     vec4 op_material(vec4 base)      // base = stock shading
 *     {
 *         return vec4(base.rgb * tint.rgb, base.a);
 *     }
 *
 * No main(), no output variable, no discard. The engine writes the output
 * and discards the PSX transparent-texel sentinel (base.a == 0); return it
 * unchanged and the hole stays a hole.
 *
 * The engine keeps main() because the stock fragment stage does more than
 * colour maths: the perspective divide and the per-pixel painter verdicts
 * that decide which surface exists at a pixel. A material cannot lose an
 * engine rule it never knew about.
 *
 * base carries: the perspective divide, the 4/8/16bpp VRAM decode through
 * the CLUT, the texel * vertexColour / 128 modulation, and the active HD
 * texture-pack replacement with its fog. Untextured draws arrive as the
 * interpolated vertex colour, so one op_material serves both paths.
 *
 * Fixed by the engine: blend equation, depth test and write, stencil,
 * painter slot, and the occlusion verdicts. A tint on an additive draw
 * therefore reads as added light, and mixing with base is usually the
 * intended result.
 *
 * Not for postfx shaders: set 0 binding 0 is the VRAM texture here and the
 * previous-pass image in openpete_shader_api.glsl. A shader includes one
 * contract or the other.
 *
 * Reserved for the engine: push constants, every descriptor set except
 * set 3 binding 1, and the input locations. The material's own uniform
 * block is set 3 binding 1 (std140).
 *
 * Two engine halves, one mod half. Retained terrain is drawn from GPU
 * arenas with its per-face material in flat varyings and its HD texture
 * pack folded through an atlas, so the engine compiles a material's source
 * twice, once with OP_PROFILE_TERRAIN defined, and this file supplies a
 * matching main() and helpers for each. The mod's source is identical in
 * both, and its colour must be a function of the face alone.
 *
 * The walked scene half is compiled a second time with OP_PROFILE_PORTAL
 * defined, for terrain that must yield to a portal preview (blended terrain
 * behind an opening, a portal's inner rim): its main() discards what the
 * preview covers before the painter verdicts, exactly as the engine's own
 * stages do. The retained-terrain half always runs that rule.
 *
 * The walked scene half is compiled a third time with OP_PROFILE_ACTOR
 * defined, for the opaque moby (ACTOR) group: its main() writes the
 * fragment's depth through op_actor_tie_depth (psx_key_contest.glsl), as
 * the engine's own actor stages do, so a moby face whose stored depth
 * carries the tie pull writes its true depth where the terrain key is in a
 * strictly nearer ordering-table slot. No profile adds anything the mod's
 * source sees or writes. */

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

/* Raw texel at page-relative texel coords through this face's page and
 * CLUT. An untextured face has no page and returns the transparent
 * sentinel. */
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

/* The stock result for this fragment from VRAM alone, without the texture
 * pack. Untextured faces are the vertex colour. */
vec4 op_psx_shade()
{
    vec4 col = op_color();
    if ((vMat.x & (1u << 23)) == 0u) return vec4(col.rgb, 1.0);
    vec4 c = op_psx_texel(ivec2(op_uv()));
    return vec4(op_psx_modulate(c.rgb, col.rgb), c.a);
}

/* True when the active texture pack replaces this face's texture. */
bool op_pack_active() { return (vHd & 0xFFFFu) != 0u; }

/* Raw replacement texel at page-texel coords (op_uv() space), at the pack
 * file's own resolution and filter, with its per-texel alpha and no fog
 * or modulation. Transparent when no replacement covers the face. */
vec4 op_pack_texel(vec2 page_uv)
{
    if ((vHd & 0xFFFFu) == 0u) return vec4(0.0);
    uint ei = ((vHd & 0xFFFFu) - 1u) * 4u;
    vec4 rect  = g.hd_ent[ei];
    vec4 arect = g.hd_ent[ei + 1u];
    vec2 t  = clamp((page_uv - rect.xy) / rect.zw, 0.0, 1.0);
    vec2 ht = 0.5 / arect.zw;      /* clamp-to-edge inside the atlas gutter */
    t = clamp(t, ht, 1.0 - ht);
    return texelFetch(u_hdatlas, ivec2(arect.xy + t * arect.zw), 0);
}

/* The stock result with the active texture pack: what the player sees
 * without this material, and the value passed to op_material(). */
vec4 op_pack_shade() { return ter_stock_shade(); }

/* ---- the mod's half -------------------------------------------------- */
vec4 op_material(vec4 base);

void main()
{
    /* Sector debug tints take precedence over the material: they show what
     * the engine believes, not what a mod paints. */
    ter_texel_grad_init();
    vec4 c;
    if (ter_debug_shade(c)) { op_out_color = c; return; }
    /* The portal plane-cover rule and the painter verdict, as the stock
     * terrain stage runs them. */
    if (ter_portal_discards()) discard;
    if (ter_key_loses()) discard;
    c = op_material(op_pack_shade());
    if (c.a == 0.0) discard;        /* PSX transparent-texel sentinel */
    op_out_color = c;
}

#else
/* ---- engine half: the walked scene pipelines ------------------------- */

#include "psx_key_contest.glsl"
#include "psx_clut.glsl"

layout(set = 0, binding = 0) uniform usampler2D op_vram; /* R16_UINT 1024x512 */

/* The HD texture-pack replacement covering this draw (RGBA8) when
 * op_pc.hd != 0; a 1x1 placeholder otherwise. Sample through op_pack_*. */
layout(set = 1, binding = 0) uniform sampler2D op_pack_tex;

/* Engine occlusion inputs. main() applies them before op_material() runs. */
layout(set = 2, binding = 0) uniform sampler2D op_key_terrain;
/* Drain sub-cell key records (psx_key_contest.glsl op_drain_record), the
 * terrain key image's set at binding 1. */
layout(set = 2, binding = 1) uniform usampler2D op_drain_records;
layout(set = 4, binding = 0) uniform sampler2D op_key_moby;
layout(set = 5, binding = 0) uniform sampler2D op_key_actor;
/* The moby identity owning each op_key_actor texel (op_akey_id). */
layout(set = 5, binding = 1) uniform sampler2D op_key_actor_owner;
layout(set = 6, binding = 0) uniform sampler2D op_moby_rank;

/* Engine-selected depth convention, key scope and per-game contest rule
 * (ordering-table slot contest, on for Spyro 3); see psx_key_contest.glsl.
 * The engine sets all three per pipeline, as on its own scene stages. The
 * stored value of gl_FragCoord.z follows the convention and is not an
 * interface; op_vz_from_depth returns the fragment's view-Z. */
layout(constant_id = 0) const bool OP_DEPTH_REVERSED = false;
/* The earlier name of OP_DEPTH_REVERSED, kept for shaders written against
 * it; it reads the pipeline's own depth convention. */
#define OP_DEPTH_AFFINE OP_DEPTH_REVERSED
layout(constant_id = 3) const bool OP_MOBY_TRUESCOPE = true;
layout(constant_id = 4) const bool OP_SLOT_CONTEST = false;

#ifdef OP_PROFILE_PORTAL
/* The portal profile: the opening planes ride a block at set 7, above the
 * sets this half reserves (portal_cover_scene.glsl). */
#define OP_COVER_SET 7
#define OP_PORTAL_RUNTIME 1
#include "portal_cover_scene.glsl"
#endif

layout(push_constant) uniform op_psx_material {
    int tpage_x, tpage_y;   /* texture page origin (VRAM 16-bit-pixel units) */
    int clut_x,  clut_y;    /* CLUT origin (VRAM pixel units; paletted only) */
    int bpp;                /* 4, 8, or 16; 0 marks an untextured draw       */
    int hd;                 /* 1 = op_pack_tex holds this draw's replacement  */
    vec2 hd_org;            /* the pack file's texel box within the texture   */
    vec2 hd_size;           /*   page (origin / size, page texel coords)      */
    vec4 hd_fog_a;          /* CLUT-ladder fog affine for the replacement:    */
    vec4 hd_fog_b;          /*   c' = c*a + b (identity when unfogged); .rgb  */
} op_pc;

/* The vertex stage emits UV and colour multiplied by the perspective ratio
 * vQ, and vQ itself, so screen-linear interpolation reconstructs
 * perspective-correct values after the divide. Read them through op_uv()
 * and op_color(). */
layout(location = 0) in vec2  vUV;     /* texel coords   x vQ */
layout(location = 1) in vec4  vColor;  /* modulate 0..1  x vQ */
layout(location = 2) in float vQ;      /* perspective ratio   */
layout(location = 3) flat in float vKoff;  /* this face's OT key channel     */
layout(location = 4) flat in vec2  vRank;  /* own walk rank; moby ordinal or identity */

layout(location = 0) out vec4 op_out_color;

/* Perspective-correct texel coords within the texture page. */
vec2 op_uv()    { return vUV / vQ; }
/* Perspective-correct per-vertex modulate colour (R8G8B8A8 -> 0..1). */
vec4 op_color() { return vColor / vQ; }

uint op_vram_at(int x, int y) { return texelFetch(op_vram, ivec2(x, y), 0).r; }

/* op_expand5 and op_decode555 (15-bit VRAM word -> RGBA; a: 0 = the PSX
 * transparent sentinel, 0.5 = STP-set texel, 1 = opaque) come from
 * psx_clut.glsl. */

/* Raw texel at page-relative texel coords through this draw's bpp and
 * CLUT. Pass op_uv() for the fragment's own texel. */
vec4 op_psx_texel(ivec2 uv)
{
    return op_decode555(op_texel_word(op_vram, ivec2(op_pc.tpage_x, op_pc.tpage_y),
                                      ivec2(op_pc.clut_x, op_pc.clut_y), op_pc.bpp,
                                      uv.x, uv.y));
}

/* PSX texture modulation: texel * vertexColour / 128 (128 = unchanged). */
vec3 op_psx_modulate(vec3 texel, vec3 vcol)
{
    return clamp(texel * vcol * (255.0 / 128.0), 0.0, 1.0);
}

/* The stock result for this fragment from VRAM alone. An untextured draw
 * (bpp 0) is the vertex colour, with no texel fetch and no modulation. A
 * textured face on the moby fog band between two ticks blends in its second
 * fogged-CLUT row after the modulation (op_clut_fog_mix). */
vec4 op_psx_shade()
{
    vec4 col = op_color();
    if (op_pc.bpp == 0) return vec4(col.rgb, 1.0);
    ivec2 uv = ivec2(op_uv());
    vec4 c = op_psx_texel(uv);
    vec3 rgb = op_clut_fog_mix(op_vram, op_psx_modulate(c.rgb, col.rgb), col,
                               ivec2(op_pc.tpage_x, op_pc.tpage_y),
                               ivec2(op_pc.clut_x, op_pc.clut_y), op_pc.bpp, uv.x, uv.y);
    return vec4(rgb, c.a);
}

/* ---- HD texture-pack access ------------------------------------------ */

/* True when the active texture pack replaces this draw's texture and
 * op_pack_tex holds it. False with no pack, no matching file, or a file
 * whose GPU upload has not completed (one present of stock). */
bool op_pack_active() { return op_pc.hd != 0; }

/* Raw replacement texel at page-texel coords (op_uv() space), at the pack
 * file's own resolution and filter, with its per-texel alpha and no fog
 * or modulation. */
vec4 op_pack_texel(vec2 page_uv)
{
    vec2 t = (page_uv - op_pc.hd_org) / op_pc.hd_size;
    return texture(op_pack_tex, clamp(t, 0.0, 1.0));
}

/* The stock result with the active texture pack: what the player sees
 * without this material, and the value passed to op_material(). Falls
 * back to op_psx_shade() when no replacement covers the draw. */
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
 * write, keeping base.a unless the material changes transparency. */
vec4 op_material(vec4 base);

void main()
{
#ifdef OP_PROFILE_ACTOR
    /* The tie-pull depth export of the opaque actor group, written before
     * any discard; a discarded fragment writes nothing. */
    gl_FragDepth = op_actor_tie_depth(gl_FragCoord.z, vRank, vKoff,
        op_key_from_stored(texelFetch(op_key_terrain, ivec2(gl_FragCoord.xy), 0).r),
        OP_SLOT_CONTEST, OP_DEPTH_REVERSED);
#endif
    /* The painter verdicts, as the stock stages run them: which surface
     * exists at this pixel is the engine's decision. */
#ifdef OP_PROFILE_PORTAL
    if (op_portal_cover_frag(OP_DEPTH_REVERSED)) discard;
#endif
    bool ranked = vRank.x > 0.5;
    float own = op_vz_from_depth(gl_FragCoord.z, OP_DEPTH_REVERSED);
    if (ranked &&
        op_rank_loses(texelFetch(op_moby_rank, ivec2(gl_FragCoord.xy), 0), vRank, own))
        discard;
    float koff = op_drain_koff(vKoff, vRank.y, op_uv(), op_drain_records);
    if (op_drain_discards(koff)) discard;   /* a sub-cell the drain skips */
    if (!ranked && abs(koff) > 0.25) {
        /* The additive glow fan (rank -2) runs the aura contest instead -
         * see psx_key_contest.glsl. */
        if (vRank.x < -1.5) {
            if (op_aura_loses(texelFetch(op_key_actor, ivec2(gl_FragCoord.xy), 0).r,
                              koff - 0.5))
                discard;
        } else if (op_key_loses(
                op_key_from_stored(texelFetch(op_key_terrain, ivec2(gl_FragCoord.xy), 0).r),
                op_akey_contest(op_akey_select(
                    texelFetch(op_key_actor,       ivec2(gl_FragCoord.xy), 0).r,
                    texelFetch(op_key_actor_owner, ivec2(gl_FragCoord.xy), 0),
                    op_akey_owner(vRank))),
                op_key_from_stored(texelFetch(op_key_moby,    ivec2(gl_FragCoord.xy), 0).r),
                own, koff,
                vRank.x < -0.5,     /* unranked moby faces carry rank -1 */
                op_rank_is_prop(vRank.x),
                OP_MOBY_TRUESCOPE, OP_SLOT_CONTEST))
            discard;
    }
    vec4 c = op_material(op_pack_shade());
    if (c.a == 0.0) discard;        /* PSX transparent-texel sentinel */
    op_out_color = c;
}

#endif /* OP_PROFILE_TERRAIN */

#endif /* OPENPETE_PSX_MATERIAL_GLSL */
