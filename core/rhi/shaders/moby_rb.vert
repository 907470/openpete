#version 450
#extension GL_GOOGLE_include_directive : require
#include "moby_stage.glsl"

/* Moby stage readback (OPENPETE_MOBY_GSTAGE_AB, diagnostic): each vertex of
 * the stage's storage runs moby_stage and is drawn as points into an
 * RGBA32_UINT target, one draw of three vertices per instance: the instance
 * is the stage vertex, gl_VertexIndex the pixel slot. Element e of an arm
 * owns pixels 3 (base + e) + k:
 *   k 0: sx, sy, tvz bits; flags (bit 0 unr, bit 1 sat)
 *   k 1: vz, kvz, hx, hy bits
 *   k 2: ex, ey, ez bits; e
 * The storage holds the instance records (MOBY_INST_VEC4 vec4s each) from
 * vec4 0 and the vertex records (MOBY_VERT_VEC4 each) from pc.vbase: c.xyz
 * and the instance index bits in w, then p.xyz. pc.ctl nonzero draws the
 * negative control: the translation moves by 1/2304 screen px in x and y and
 * 2^-18 in relative depth at the record's centre depth (floored at the near
 * plane), which is 1/64 output px at 36 output px per screen px, and on each
 * axis by at least 2^-19 of its largest component. Pairs with
 * terrain_readback.frag, which writes vMat unchanged. */

layout(std430, set = 2, binding = 0) readonly buffer MobyStageData {
    vec4 d[];
} gs;

layout(push_constant) uniform MobyRbPc {
    uint  vbase;   /* first vertex record, in vec4s */
    uint  h;       /* the target's height */
    uint  ebase;   /* the arm's first element in the target */
    float ctl;     /* 1 draws the negative control */
} pc;

layout(location = 2) flat out uvec4 vMat;

#define RB_W 4096u
/* The control's screen offset, 1/64 output px at 36 output px per screen px. */
#define MOBY_RB_CTL_PX (1.0 / 2304.0)
/* The control's relative depth offset. */
#define MOBY_RB_CTL_Z (1.0 / 262144.0)
/* The control's least offset, relative to the largest translation component
 * (16 float spacings): on a vertex whose view-Z is small against the
 * translation it moves the depth well past the relative depth bound. */
#define MOBY_RB_CTL_TR (1.0 / 524288.0)
/* The near plane in world view-Z (moby.c MOBY_NEAR_W). */
#define MOBY_NEAR_W 16.0

void main()
{
    uint e = uint(gl_InstanceIndex);
    uint k = uint(gl_VertexIndex);
    vec4 a = gs.d[pc.vbase + MOBY_VERT_VEC4 * e];
    vec4 b = gs.d[pc.vbase + MOBY_VERT_VEC4 * e + 1u];
    uint ib = floatBitsToUint(a.w) * MOBY_INST_VEC4;
    moby_inst I = moby_inst_unpack(gs.d[ib], gs.d[ib + 1u], gs.d[ib + 2u], gs.d[ib + 3u],
                                   gs.d[ib + 4u], gs.d[ib + 5u], gs.d[ib + 6u], gs.d[ib + 7u]);
    float zc = max(I.tr.z, MOBY_NEAR_W / I.unit);
    float floor_tr = max(max(abs(I.tr.x), abs(I.tr.y)), abs(I.tr.z)) * MOBY_RB_CTL_TR;
    vec3 dtr = pc.ctl * max(vec3(zc * MOBY_RB_CTL_PX / I.H, zc * MOBY_RB_CTL_PX / I.H,
                                 zc * MOBY_RB_CTL_Z), vec3(floor_tr));
    moby_vout o = moby_stage(I, a.xyz, b.xyz, dtr);
    if (k == 0u)
        vMat = uvec4(floatBitsToUint(o.s.x), floatBitsToUint(o.s.y), floatBitsToUint(o.tvz),
                     o.unr | (o.sat << 1u));
    else if (k == 1u)
        vMat = uvec4(floatBitsToUint(o.vz), floatBitsToUint(o.kvz), floatBitsToUint(o.h.x),
                     floatBitsToUint(o.h.y));
    else
        vMat = uvec4(floatBitsToUint(o.e.x), floatBitsToUint(o.e.y), floatBitsToUint(o.e.z), e);
    uint px = 3u * (pc.ebase + e) + k;
    gl_Position = vec4((float(px % RB_W) + 0.5) * (2.0 / float(RB_W)) - 1.0,
                       1.0 - (float(px / RB_W) + 0.5) * (2.0 / float(pc.h)), 0.5, 1.0);
    gl_PointSize = 1.0;
}
