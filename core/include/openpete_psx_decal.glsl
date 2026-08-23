/* openpete_psx_decal.glsl — the DECAL material profile.
 *
 * Include from a material fragment shader targeting a draw path whose stock
 * fragment stage owns per-pixel OCCLUSION rather than only colour:
 *
 *     #version 450
 *     #extension GL_GOOGLE_include_directive : require
 *     #include "openpete_psx_decal.glsl"
 *
 *     layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };
 *     layout(location = 0) out vec4 outColor;
 *     void main() {
 *         if (!op_decal_visible()) discard;   // engine's verdict, not yours
 *         vec4 c = op_decal_shade();
 *         if (c.a == 0.0) discard;
 *         outColor = vec4(c.rgb * tint.rgb, c.a);
 *     }
 *
 * WHY THIS EXISTS, and why it is a second contract rather than a flag on
 * the first. A material replaces the fragment stage. That is harmless when
 * the stock stage is pure colour maths, and destructive when it is not:
 * Spyro's moby drop-shadows draw as flat fans with the depth test OFF,
 * because their fragment shader decides, per pixel, whether the shadow is
 * occluded — reconstructing the terrain's world height from a depth
 * pre-pass and rejecting walls, steep faces, and raised platforms in
 * front. Hand a mod the plain material contract there and its shader
 * silently deletes that logic: the fan paints straight through walls.
 *
 * So the profile splits the stock stage in two. `op_decal_visible()` is
 * the engine's occlusion verdict, unchanged and not overridable.
 * `op_decal_shade()` is the sprite decode plus the PSX modulation rule.
 * A mod composes them and owns only the colour.
 *
 * Reserved for the engine: push constants, sets 0-1, input locations 0-2.
 * A material's own uniform block is set 3 binding 1 (std140).
 *
 * The maths below mirrors core/rhi/shaders/shadow_decal_tex.frag — read
 * that file for the derivation of each occluder class.
 */

#ifndef OPENPETE_PSX_DECAL_GLSL
#define OPENPETE_PSX_DECAL_GLSL

layout(set = 0, binding = 0) uniform sampler2D  op_opaque_depth; /* WORLD-opaque pre-pass */
layout(set = 1, binding = 0) uniform usampler2D op_vram;         /* R16_UINT 1024x512     */

layout(push_constant) uniform OP_DECAL_PC {
    float col0x, col0y, col0z;   /* camera rows 4096^2 * C^-1 (world reconstruct) */
    float col1x, col1y, col1z;
    float col2x, col2y, col2z;
    float cam_pos_up;            /* camera world HEIGHT (the game z field) */
    float ofx, ofy, h;           /* GTE projection regs */
    float wall_height;           /* height slack above ground before "wall" */
    float steep_nup;             /* |up normal| below which a surface is a wall face */
    float mode;                  /* 0 = draw all (band), 1 = height+steep test */
    float band;                  /* fallback view-Z band (mode 0) */
    float plat_eps;              /* raised-occluder-in-front: min rise above the plane */
    float plat_slack;            /* raised-occluder-in-front: view-Z slack */
    float uax, ubx, vay, vby;    /* PSX screen -> pre-pass uv affine */
    float pvw, pvh;              /* pre-pass raster extent within the depth image */
    int   tpage_x, tpage_y;      /* PSX texture page origin (VRAM 16bpp-pixel units) */
    int   clut_x,  clut_y;       /* CLUT origin (paletted only) */
    int   bpp;                   /* 4, 8, or 16 */
} op_pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) noperspective in vec2 vScreenPSX;

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
    if (op_pc.bpp == 16) {
        return op_vram_at(op_pc.tpage_x + u, op_pc.tpage_y + v);
    } else if (op_pc.bpp == 8) {
        uint word = op_vram_at(op_pc.tpage_x + (u >> 1), op_pc.tpage_y + v);
        uint idx  = (word >> (8 * (u & 1))) & 0xFFu;
        return op_vram_at(op_pc.clut_x + int(idx), op_pc.clut_y);
    } else { /* 4bpp */
        uint word = op_vram_at(op_pc.tpage_x + (u >> 2), op_pc.tpage_y + v);
        uint nib  = (word >> (4 * (u & 3))) & 0xFu;
        return op_vram_at(op_pc.clut_x + int(nib), op_pc.clut_y);
    }
}

/* The stock look: sprite texel * vertexColour / 128, PSX modulation.
 * alpha 0 = the transparent sentinel; discard on it as the stock stage
 * does, or the fan's whole quad paints. */
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
float op_worldup_at(vec2 s, float vz)
{
    float M1 = (s.x - op_pc.ofx) * vz / op_pc.h;
    float M2 = (s.y - op_pc.ofy) * vz / op_pc.h;
    float M3 = vz;
    float V1 = (op_pc.col1x * M1 + op_pc.col1y * M2 + op_pc.col1z * M3) / 4096.0;
    return op_pc.cam_pos_up - V1;
}

/* The engine's occlusion verdict. false = this pixel is occluded and the
 * decal must not paint. Not overridable: a material that skips this call
 * paints shadows through walls. */
bool op_decal_visible()
{
    vec2  uv = vec2(op_pc.uax * vScreenPSX.x + op_pc.ubx,
                    op_pc.vay * vScreenPSX.y + op_pc.vby);
    ivec2 hi = ivec2(int(op_pc.pvw) - 1, int(op_pc.pvh) - 1);
    ivec2 tx = min(ivec2(clamp(uv, vec2(0.0), vec2(1.0)) * vec2(op_pc.pvw, op_pc.pvh)), hi);
    float viewZ    = op_viewz_prepass(texelFetch(op_opaque_depth, tx, 0).r);
    float fanViewZ = op_viewz_scene(gl_FragCoord.z);

    float worldUpTerrain = op_worldup_at(vScreenPSX, viewZ);
    float worldUpGround  = op_worldup_at(vScreenPSX, fanViewZ);
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

#endif /* OPENPETE_PSX_DECAL_GLSL */
