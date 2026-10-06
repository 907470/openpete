/* terrain_shade.glsl - fragment shading for the retained terrain mesh.
 * Included by the material contract's terrain half; not a mod-facing
 * interface.
 *
 * The retained mesh carries its material per face as flat varyings from
 * the vertex stage (the CLUT-ladder fog row and the near-tile select are
 * camera-dependent face verdicts computed there) instead of the per-draw
 * push constants of the walked scene stages. The stock terrain stage and
 * the material tier both include this file, so they run the same decode,
 * the same key contest and the same debug tints.
 *
 * Contest scope: terrain is never ranked and never an actor, and the moby
 * true-scope is always on for the retained path, so only the terrain-key
 * and prop-key rules apply. */

#ifndef OPENPETE_TERRAIN_SHADE_GLSL
#define OPENPETE_TERRAIN_SHADE_GLSL

#extension GL_ARB_gpu_shader_fp64 : require
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */
layout(set = 1, binding = 0) uniform sampler2D u_hpkey;   /* terrain key image */
layout(set = 2, binding = 0) uniform sampler2D u_mobykey; /* prop true-depth image */
layout(set = 4, binding = 0) uniform sampler2D u_hdatlas; /* HD-fold atlas (NEAREST) */

layout(location = 0)      in vec2  vUV;    /* x vQ (divide by vQ)            */
layout(location = 1)      in vec4  vColor; /* x vQ (divide by vQ)            */
layout(location = 2) flat in uvec2 vMat;   /* x: tpx|tpy<<10|bppc<<19|textured<<23,
                                            * y: clx | cly<<16 */
layout(location = 3) flat in float vKoff;  /* OT key (0 = out of contest) */
layout(location = 4)      in float vQ;
layout(location = 5) flat in uint  vHd;    /* HD-fold entry id | flags */
layout(location = 6)      in vec2  vKfl;   /* drain face-local coordinate x vKq */
layout(location = 7)      in float vKq;
layout(location = 8) flat in uvec4 vKc0;   /* drain sub-cell key table */
layout(location = 9) flat in uvec4 vKc1;
layout(location = 10)     in vec3  vKd;    /* fold position D x vKq */
layout(location = 11) flat in vec4 vKn0;   /* fold corners (psx_key_contest.glsl) */
layout(location = 12) flat in vec4 vKn1;
layout(location = 13) flat in vec4 vKn2;

/* Prefix mirror of the vertex stage's TerrainGlobals block. Only the fields
 * this stage reads are named, but every field before them is declared so
 * the layout matches; a field added to the vertex stage's block ahead of
 * `fog`, `hd_lad` or the cover planes must be mirrored here. */
layout(std140, set = 5, binding = 0) uniform TerrainGlobals {
    vec4  mrow0; vec4 mrow1; vec4 mrow2;
    dvec2 dcam_xy; dvec2 dcam_zf; dvec2 dof_xy; dvec2 dH;
    dvec2 dnear; dvec2 dspare;
    ivec4 fog;
    vec4  pad_[302];     /* scroll_dv and the rim frames (vertex-stage state) */
    vec4  hd_lad;        /* HD fold: continuous-fog ramp ts, tb; count; enable */
    vec4  hd_ent[1664];  /* 416 entries x 4 vec4 (rect/atlas/fogA/fogB)    */
    ivec4 cov_cnt;       /* portal cover planes (portal_cover.glsl)       */
    dvec4 cov_margin;
    dvec4 cov_abc[6];
    vec4  cov_bbox[6];
    vec4  cov_poly[96];
} g;

/* gl_FragCoord.z convention, matching the vertex stage's selection. */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;
/* The earlier name of DEPTH_REVERSED, kept for mod materials written
 * against it; it reads the pipeline's own depth convention. */
#define DEPTH_AFFINE DEPTH_REVERSED

/* The fragment-visible part of terrain.vert's TerrainPC: the fields up to
 * `snap` are the vertex stage's; `cover` turns on the blended cover discard
 * and `fc` maps gl_FragCoord to PSX screen space for both portal discards. */
layout(push_constant) uniform TerrainPC {
    vec4 cbase;
    vec4 cdims;
    int  pass_sel;
    int  full_extent;
    int  mat_pass;
    int  snap;
    int  cover;          /* 1: discard what a portal preview covers        */
    int  pad0, pad1, pad2;
    vec4 fc;             /* gl_FragCoord -> PSX screen: x = fc.x * fx + fc.y, y = fc.z * fy + fc.w */
} ter_pc;

/* Nonzero on a face the vertex stage accepted as a portal's inner rim under
 * a plane that discards its rim per fragment (rim_classified). */
layout(location = 14) flat in uint vRim;

#define OP_COVER_SRC g
#include "portal_cover.glsl"

/* The portal plane-cover rule (portal_cover.glsl) for this fragment: in a
 * blended pass with the cover on, discard at or behind plane - M inside an
 * opening; on an inner-rim face, discard inside an opening at any depth
 * (OP_PORTAL_RIM_MARGIN), where the preview paints. */
bool ter_portal_discards()
{
    if (!((ter_pc.cover != 0 && g.cov_cnt.y != 0) || vRim != 0u)) return false;
    const vec2 p = vec2(ter_pc.fc.x * gl_FragCoord.x + ter_pc.fc.y,
                        ter_pc.fc.z * gl_FragCoord.y + ter_pc.fc.w);
    const float m = vRim != 0u ? OP_PORTAL_RIM_MARGIN : float(g.cov_margin.x);
    return op_portal_covered(p, op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED), m);
}
/* Fragment spec-constant 4 (`SLOT_CONTEST`): the key contest compares
 * 128-world ordering-table slot indices and resolves an equal slot to the
 * terrain. Set per game from game_layout.terrain.slot_ot_contest. */
layout(constant_id = 4) const bool SLOT_CONTEST = false;
/* Fragment spec-constant 2 (`NOTEX`): the untextured lighting view (and the
 * wireframe view, which bakes it on): a textured face's texel colour is
 * replaced by the modulation-neutral grey, so the output is the gouraud
 * vertex colour; transparent texels still cut out. */
layout(constant_id = 2) const bool NOTEX = false;


uint ter_vram_at(int x, int y) { return texelFetch(u_vram, ivec2(x, y), 0).r; }

float ter_expand5(uint v) { return float((v << 3) | (v >> 2)) / 255.0; }

vec4 ter_decode555(uint p)
{
    if (p == 0u) return vec4(0.0);            /* PSX transparent sentinel */
    return vec4(ter_expand5( p        & 31u),
                ter_expand5((p >> 5)  & 31u),
                ter_expand5((p >> 10) & 31u),
                1.0 - 0.5 * float((p >> 15) & 1u));
}

/* Debug sector tints (globals gflags bit 29). Returns true when a mode
 * answered: the caller writes `c` and stops. */
bool ter_debug_shade(out vec4 c)
{
    c = vec4(0.0);
    if ((g.fog.y & (1 << 29)) == 0) return false;
    int mode = (g.fog.y >> 26) & 7;
    if (mode == 6) {                        /* exact ladder t0k readout */
        if ((vMat.y & (1u << 16)) == 0u) {  /* ladder never ran here */
            c = vec4(0.0, 0.0, 1.0, 1.0);
        } else {
            uint enc = (vMat.y >> 18) & 0x1FFFu;   /* t0k + 4096 */
            c = vec4(float((enc >> 8) & 255u) / 255.0,
                     float(enc & 255u) / 255.0,
                     float(((vMat.y >> 17) & 1u) * 128u
                           + ((vMat.y >> 9) & 0x7Fu)) / 255.0, 1.0);
        }
        return true;
    }
    if (mode == 2 || mode == 4 || mode == 5) {  /* raw (or substituted) colour */
        c = vec4((vColor / vQ).rgb, 1.0);
    } else if (mode == 3) {                 /* UV visualiser (texel/256) */
        vec2 uvq = vUV / vQ;
        c = vec4(fract(uvq.x / 256.0), fract(uvq.y / 256.0), 0.0, 1.0);
    } else {                                /* ladder verdict */
        uint half_ = (vMat.y >> 30) & 1u;
        uint dlt   = (vMat.y >> 25) & 31u;
        c = vec4(float(half_), float(dlt) / 15.0,
                 (vMat.x & (1u << 23)) != 0u ? 0.0 : 1.0, 1.0);
    }
    return true;
}

/* The key contest (psx_key_contest.glsl), terrain subset: never ranked,
 * never an actor or a prop, true-scope on, so the actor-key arm receives the
 * never-wins sentinel. A face with a drain sub-cell table contests at the
 * key of the sub-cell under the fragment (op_drain_koff_pf) and loses a
 * fragment in a sub-cell the drain skips. */
bool ter_key_loses()
{
    if (abs(vKoff) <= 0.25) return false;
    float koff = op_drain_koff_pf(vKoff, vKfl / vKq, vKc0, vKc1, vKd / vKq, vKn0.xyz,
                                  vec3(vKn0.w, vKn1.xy), vec3(vKn1.zw, vKn2.x), vKn2.yzw);
    if (op_drain_discards(koff)) return true;
    return op_key_loses(
        op_key_from_stored(texelFetch(u_hpkey,   ivec2(gl_FragCoord.xy), 0).r),
        OP_KEY_NONE,
        op_key_from_stored(texelFetch(u_mobykey, ivec2(gl_FragCoord.xy), 0).r),
        op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED),
        koff,
        false, false, true, SLOT_CONTEST);
}

/* The stock colour for this fragment. a == 0 marks a texel the PSX treats
 * as transparent (the sentinel, or an HD replacement's cut-out); the
 * caller discards it. Untextured faces (LP, flat-gouraud HP) are the
 * vertex colour at alpha 1, except blend class 1 (mat0 bits 21-22, PSX
 * ABR 0), which takes alpha 128/255 so the alpha arena's SRC_ALPHA blend
 * draws (B+F)/2, the vertex alpha the scene intake stores for the same
 * face on the CPU walk. */
vec4 ter_stock_shade()
{
    uint mat0 = vMat.x;
    vec4 col = vColor / vQ;
    if ((mat0 & (1u << 23)) == 0u)            /* untextured: plain gouraud */
        return vec4(col.rgb, ((mat0 >> 21) & 3u) == 1u ? 128.0 / 255.0 : 1.0);
    /* HD-fold replacement: a baked entry replaces the VRAM fetch (file-box
     * mapping, alpha cut-out, the continuous pack-fog ramp), then the same
     * PSX modulation. Entries exist only on opaque faces. */
    if ((vHd & 0xFFFFu) != 0u) {
        uint ei = ((vHd & 0xFFFFu) - 1u) * 4u;
        vec4 rect  = g.hd_ent[ei];
        vec4 arect = g.hd_ent[ei + 1u];
        vec4 fogA  = g.hd_ent[ei + 2u];
        vec4 fogB  = g.hd_ent[ei + 3u];
        vec2 uvh = vUV / vQ;
        vec2 t = clamp((uvh - rect.xy) / rect.zw, 0.0, 1.0);
        /* Half a texel inside the file's atlas rect: clamp-to-edge without
         * bleeding across the atlas gutter. */
        vec2 ht = 0.5 / arect.zw;
        t = clamp(t, ht, 1.0 - ht);
        vec2 auv = arect.xy + t * arect.zw;
        vec4 hc;
        if (fogA.w > 0.5) {   /* linear-class file: manual bilinear (atlas is NEAREST) */
            vec2 tx = auv - 0.5;
            vec2 fw = fract(tx);
            ivec2 b = ivec2(floor(tx));
            vec4 s00 = texelFetch(u_hdatlas, b, 0);
            vec4 s10 = texelFetch(u_hdatlas, b + ivec2(1, 0), 0);
            vec4 s01 = texelFetch(u_hdatlas, b + ivec2(0, 1), 0);
            vec4 s11 = texelFetch(u_hdatlas, b + ivec2(1, 1), 0);
            hc = mix(mix(s00, s10, fw.x), mix(s01, s11, fw.x), fw.y);
        } else {              /* exact-multiple file: stock-identical fetch */
            hc = texelFetch(u_hdatlas, ivec2(auv), 0);
        }
        if (hc.a < 0.5) return vec4(0.0);
        if (NOTEX) return vec4(clamp(col.rgb, 0.0, 1.0), 1.0);
        if ((vHd & 0x10000u) != 0u && g.hd_lad.x != 0.0) {
            float w = op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED);
            float tt = clamp(w * g.hd_lad.x +
                             (g.hd_lad.y - ((vHd & 0x20000u) != 0u ? 8192.0 / 3840.0 : 0.0)),
                             0.0, 1.0);
            hc.rgb = clamp(hc.rgb * mix(vec3(1.0), fogA.rgb, tt) + fogB.rgb * tt, 0.0, 1.0);
        }
        return vec4(clamp(hc.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0), 1.0);
    }
    int tpx  = int(mat0 & 0x3FFu);
    int tpy  = int((mat0 >> 10) & 0x1FFu) + int(mat0 >> 31) * 512;   /* bit 31: auxiliary bank */
    uint bppc = (mat0 >> 19) & 3u;
    int clx  = int(vMat.y & 0xFFFFu);
    int cly  = int(vMat.y >> 16);

    vec2 uvq = vUV / vQ;
    int u = int(uvq.x), v = int(uvq.y);
    uint p;
    if (bppc == 2u) {          /* 16bpp direct */
        p = ter_vram_at(tpx + u, tpy + v);
    } else if (bppc == 1u) {   /* 8bpp CLUT */
        uint word = ter_vram_at(tpx + (u >> 1), tpy + v);
        uint idx  = (word >> (8 * (u & 1))) & 0xFFu;
        p = ter_vram_at(clx + int(idx), cly);
    } else {                   /* 4bpp CLUT */
        uint word = ter_vram_at(tpx + (u >> 2), tpy + v);
        uint nib  = (word >> (4 * (u & 3))) & 0xFu;
        p = ter_vram_at(clx + int(nib), cly);
    }
    vec4 c = ter_decode555(p);
    if (c.a == 0.0) return vec4(0.0);
    if (NOTEX) c.rgb = vec3(128.0 / 255.0);
    /* PSX texture modulation: texel * vertexColour / 128 (clamped). */
    return vec4(clamp(c.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0), c.a);
}

#endif /* OPENPETE_TERRAIN_SHADE_GLSL */
