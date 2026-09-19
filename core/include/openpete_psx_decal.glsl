/* openpete_psx_decal.glsl — the material contract for moby drop-shadows.
 *
 * Same mod-facing function as openpete_psx_material.glsl: the engine owns
 * main(), the mod supplies op_material(base).
 *
 *     #version 450
 *     #extension GL_GOOGLE_include_directive : require
 *     #include "openpete_psx_decal.glsl"
 *
 *     layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };
 *
 *     vec4 op_material(vec4 base) { return vec4(base.rgb * tint.rgb, base.a); }
 *
 * A separate include because the draw path differs. Moby drop-shadows are
 * flat fans drawn with the depth test off; their fragment stage decides per
 * pixel whether the shadow is occluded by reconstructing the terrain's
 * world height from the opaque depth pre-pass and rejecting walls, steep
 * faces and raised platforms in front. That path has its own vertex stage,
 * push block and samplers. A mod that writes one op_material() can be
 * pointed at either contract.
 *
 * main() applies the occlusion verdict before op_material() runs; a
 * material cannot paint a shadow through a wall.
 *
 * Reserved for the engine: push constants, sets 0-1, input locations 0-2.
 * The material's own uniform block is set 3 binding 1 (std140). */

#ifndef OPENPETE_PSX_DECAL_GLSL
#define OPENPETE_PSX_DECAL_GLSL

layout(set = 0, binding = 0) uniform sampler2D  op_opaque_depth; /* terrain-opaque pre-pass */
layout(set = 1, binding = 0) uniform usampler2D op_vram;         /* R16_UINT 1024x512       */

layout(push_constant) uniform OP_DECAL_PC {
    float col0x, col0y, col0z;   /* camera rows 4096^2 * C^-1 (world reconstruct) */
    float col1x, col1y, col1z;
    float col2x, col2y, col2z;
    float cam_pos_up;            /* camera world height (the game's z field) */
    float ofx, ofy, h;           /* GTE projection registers */
    float wall_height;           /* height slack above ground before "wall" */
    float steep_nup;             /* |up normal| below which a surface is a wall face */
    float mode;                  /* 0 = draw all (band), 1 = height and steepness test */
    float band;                  /* fallback view-Z band (mode 0) */
    float plat_eps;              /* raised occluder in front: min rise above the plane */
    float plat_slack;            /* raised occluder in front: view-Z slack */
    float uax, ubx, vay, vby;    /* PSX screen -> pre-pass uv affine */
    float pvw, pvh;              /* pre-pass raster extent within the depth image */
    float fax, fbx, fay, fby;    /* gl_FragCoord -> PSX screen affine, this pass */
    int   tpage;                 /* texture page origin, x | y << 10 */
    int   clut;                  /* CLUT origin x | y << 10, bpp code << 20 */
} op_pc;

/* The material block is 29 floats + 2 ints = 124 bytes, inside the 128-byte
 * guaranteed maxPushConstantsSize. VRAM coordinates are 10 bits of x (0..1023)
 * and 9 bits of y (0..511), and the bpp code is 2 bits, so the five PSX
 * material fields ride two words and leave room for the affine. */
int op_tpage_x() { return  op_pc.tpage        & 0x3FF; }
int op_tpage_y() { return (op_pc.tpage >> 10) & 0x1FF; }
int op_clut_x()  { return  op_pc.clut         & 0x3FF; }
int op_clut_y()  { return (op_pc.clut  >> 10) & 0x1FF; }
int op_bpp()     { return 4 << ((op_pc.clut >> 20) & 3); }   /* 0/1/2 -> 4/8/16 */

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

/* ---- PSX VRAM decode ------------------------------------------------ */
uint  op_vram_at(int x, int y) { return texelFetch(op_vram, ivec2(x, y), 0).r; }
float op_expand5(uint v) { return float((v << 3) | (v >> 2)) / 255.0; }
vec4  op_decode555(uint p)
{
    if (p == 0u) return vec4(0.0);                /* PSX transparent sentinel */
    return vec4(op_expand5( p        & 31u),
                op_expand5((p >> 5)  & 31u),
                op_expand5((p >> 10) & 31u),
                1.0 - 0.5 * float((p >> 15) & 1u));
}
uint op_decal_word(int u, int v)
{
    int tx = op_tpage_x(), ty = op_tpage_y();
    int bpp = op_bpp();
    if (bpp == 16) {
        return op_vram_at(tx + u, ty + v);
    } else if (bpp == 8) {
        uint word = op_vram_at(tx + (u >> 1), ty + v);
        uint idx  = (word >> (8 * (u & 1))) & 0xFFu;
        return op_vram_at(op_clut_x() + int(idx), op_clut_y());
    } else { /* 4bpp */
        uint word = op_vram_at(tx + (u >> 2), ty + v);
        uint nib  = (word >> (4 * (u & 3))) & 0xFu;
        return op_vram_at(op_clut_x() + int(nib), op_clut_y());
    }
}

/* The stock shadow colour: sprite texel * vertexColour / 128. Alpha 0 is
 * the transparent sentinel; main() discards it, otherwise the whole fan
 * quad would paint. */
vec4 op_decal_shade()
{
    vec4 c = op_decode555(op_decal_word(int(vUV.x), int(vUV.y)));
    if (c.a == 0.0) return c;
    return vec4(clamp(c.rgb * vColor.rgb * (255.0 / 128.0), 0.0, 1.0), c.a);
}

/* ---- world reconstruction ------------------------------------------- */
float op_viewz_scene(float d)   { return 4096.0 * d / max(1.0 - d, 1e-6); }
float op_viewz_prepass(float d) { return 1.0 / max(1.0 - d, 1e-9); }

vec2 op_screen_of_texel(ivec2 t)
{
    return vec2(((float(t.x) + 0.5) / op_pc.pvw - op_pc.ubx) / op_pc.uax,
                ((float(t.y) + 0.5) / op_pc.pvh - op_pc.vby) / op_pc.vay);
}
vec3 op_world_of_texel(ivec2 t)
{
    float vz = op_viewz_prepass(texelFetch(op_opaque_depth, t, 0).r);
    vec2  s  = op_screen_of_texel(t);
    float M1 = (s.x - op_pc.ofx) * vz / op_pc.h;
    float M2 = (s.y - op_pc.ofy) * vz / op_pc.h;
    float M3 = vz;
    float V0 = (op_pc.col0x * M1 + op_pc.col0y * M2 + op_pc.col0z * M3) / 4096.0;
    float V1 = (op_pc.col1x * M1 + op_pc.col1y * M2 + op_pc.col1z * M3) / 4096.0;
    float V2 = (op_pc.col2x * M1 + op_pc.col2y * M2 + op_pc.col2z * M3) / 4096.0;
    return vec3(V2, -V0, -V1);
}
/* This fragment's own PSX screen position, recovered from its window
 * coordinate through the pass's inverse affine. The position is exact per
 * fragment; an interpolated vertex attribute carrying it disagreed with the
 * fragment's own position on triangles the rasterizer clipped to thin
 * slivers, and the lookups below then read a neighbouring surface. */
vec2 op_screen_own()
{
    return vec2(op_pc.fax * gl_FragCoord.x + op_pc.fbx,
                op_pc.fay * gl_FragCoord.y + op_pc.fby);
}

float op_worldup_at(vec2 s, float vz)
{
    float M1 = (s.x - op_pc.ofx) * vz / op_pc.h;
    float M2 = (s.y - op_pc.ofy) * vz / op_pc.h;
    float M3 = vz;
    float V1 = (op_pc.col1x * M1 + op_pc.col1y * M2 + op_pc.col1z * M3) / 4096.0;
    return op_pc.cam_pos_up - V1;
}

/* The engine's occlusion verdict: false = this pixel is occluded and the
 * decal must not paint. main() applies it; a material never sees the
 * fragments it rejects. */
bool op_decal_visible()
{
    vec2  sOwn = op_screen_own();
    vec2  uv = vec2(op_pc.uax * sOwn.x + op_pc.ubx,
                    op_pc.vay * sOwn.y + op_pc.vby);
    ivec2 hi = ivec2(int(op_pc.pvw) - 1, int(op_pc.pvh) - 1);
    ivec2 tx = min(ivec2(clamp(uv, vec2(0.0), vec2(1.0)) * vec2(op_pc.pvw, op_pc.pvh)), hi);
    float viewZ    = op_viewz_prepass(texelFetch(op_opaque_depth, tx, 0).r);
    float fanViewZ = op_viewz_scene(gl_FragCoord.z);

    float worldUpTerrain = op_worldup_at(sOwn, viewZ);
    float worldUpGround  = op_worldup_at(sOwn, fanViewZ);
    float height = worldUpTerrain - worldUpGround;

    vec3  P0 = op_world_of_texel(tx);
    vec3  Px = op_world_of_texel(min(tx + ivec2(1, 0), hi));
    vec3  Py = op_world_of_texel(min(tx + ivec2(0, 1), hi));
    vec3  N  = cross(Px - P0, Py - P0);
    float nlen = length(N);
    float nup  = (nlen > 1e-6) ? abs(N.z) / nlen : 1.0;

    if (op_pc.mode > 0.5) {
        if (viewZ + op_pc.plat_slack < fanViewZ && height > op_pc.plat_eps) return false;
        if (op_pc.steep_nup > 0.0) {
            if (nup < op_pc.steep_nup) return false;
            float flat01 = clamp((nup - op_pc.steep_nup) / (0.9 - op_pc.steep_nup), 0.0, 1.0);
            if (height > op_pc.wall_height * flat01) return false;
        } else {
            if (height > op_pc.wall_height) return false;
        }
    } else {
        if (viewZ + op_pc.band < fanViewZ) return false;
    }
    return true;
}

/* ---- the mod's half --------------------------------------------------
 * Supplied by the material, with the main contract's signature. base =
 * op_decal_shade(); return the colour to write. */
vec4 op_material(vec4 base);

layout(location = 0) out vec4 op_out_color;

void main()
{
    if (!op_decal_visible()) discard;
    vec4 c = op_material(op_decal_shade());
    if (c.a == 0.0) discard;        /* PSX transparent-texel sentinel */
    op_out_color = c;
}

#endif /* OPENPETE_PSX_DECAL_GLSL */
