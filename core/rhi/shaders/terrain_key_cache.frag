#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"

/* Retained-terrain KEY-pass fragment stage - the psx_key_cutout.frag
 * counterpart for terrain.vert's pass_sel==2 output: rasterise the flat og
 * OT key as depth, with TEXTURED faces stamping only the pixels their
 * opaque texels paint (og OT occupancy - a fence lattice must not
 * key-discard what its holes show). Untextured faces raster full-footprint.
 * The material comes from the vMat varying (the vertex stage already ran
 * the near-tile / CLUT-ladder select), not a push constant. A face with a
 * drain sub-cell table writes the key of the sub-cell containing the
 * fragment (psx_key_contest.glsl; a folded quadrant's skipped slot through
 * op_drain_fold_delta) as its depth, as psx_key_cutout.frag and
 * terrain_key.frag do on the walk; every other face writes the rasterised
 * depth back unchanged. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */

layout(location = 0)      in vec2  vUV;    /* u/wq (recover via vQ)          */
layout(location = 2) flat in uvec2 vMat;
layout(location = 3) flat in float vKoff;  /* the face's key + 0.5           */
layout(location = 4)      in float vQ;     /* 1/wq                           */
layout(location = 6)      in vec2  vKfl;   /* face-local coordinate x vKq    */
layout(location = 7)      in float vKq;
layout(location = 8) flat in uvec4 vKc0;
layout(location = 9) flat in uvec4 vKc1;
layout(location = 10)     in vec3  vKd;    /* fold position D x vKq           */
layout(location = 11) flat in vec4 vKn0;   /* fold corners TL.xyz TR.x        */
layout(location = 12) flat in vec4 vKn1;   /*   TR.yz BL.xy                   */
layout(location = 13) flat in vec4 vKn2;   /*   BL.z BR.xyz                   */

layout(location = 0) out vec4 outColor;

uint vram_at(int x, int y) { return texelFetch(u_vram, ivec2(x, y), 0).r; }

void main()
{
    uint mat0 = vMat.x;
    if ((mat0 & (1u << 23)) != 0u) {          /* textured: transparent-texel cutout */
        int tpx  = int(mat0 & 0x3FFu);
        int tpy  = int((mat0 >> 10) & 0x1FFu);
        uint bppc = (mat0 >> 19) & 3u;
        int clx  = int(vMat.y & 0xFFFFu);
        int cly  = int(vMat.y >> 16);
        vec2 uvq = vUV / vQ;
        int u = int(uvq.x), v = int(uvq.y);
        uint p;
        if (bppc == 2u) {
            p = vram_at(tpx + u, tpy + v);
        } else if (bppc == 1u) {
            uint word = vram_at(tpx + (u >> 1), tpy + v);
            uint idx  = (word >> (8 * (u & 1))) & 0xFFu;
            p = vram_at(clx + int(idx), cly);
        } else {
            uint word = vram_at(tpx + (u >> 2), tpy + v);
            uint nib  = (word >> (4 * (u & 3))) & 0xFu;
            p = vram_at(clx + int(nib), cly);
        }
        if (p == 0u) discard;                  /* PSX transparent sentinel */
    }
    if (op_drain_grid(vKc0) == OP_DRAIN_NONE) {
        gl_FragDepth = gl_FragCoord.z;
    } else {
        float d = op_drain_delta(vKfl / vKq, vKc0, vKc1, 0.0);
        if (d >= OP_DRAIN_SKIP && op_drain_folded(vKc0))
            d = op_drain_fold_delta(vKd / vKq, vKn0.xyz, vec3(vKn0.w, vKn1.xy),
                                    vec3(vKn1.zw, vKn2.x), vKn2.yzw, vKc0, vKc1, 0.0);
        if (d >= OP_DRAIN_SKIP) discard;       /* a sub-cell the drain skips */
        gl_FragDepth = op_key_store(max(vKoff - 0.5, 1.0) + d);
    }
    outColor = vec4(1.0);                      /* colour attachment unused */
}
