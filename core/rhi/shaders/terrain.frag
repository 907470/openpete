#version 450
#extension GL_GOOGLE_include_directive : require
#include "terrain_shade.glsl"

/* Terrain sector-mesh fragment stage. Everything it does lives in
 * terrain_shade.glsl, shared with the material tier's terrain engine half so
 * the two cannot drift; this file is the stock assembly of those pieces. */

layout(location = 0) out vec4 outColor;

/* The fragment-visible part of terrain.vert's TerrainPC: the fields up to
 * `snap` are the vertex stage's; `cover` and `fc` drive the portal cover
 * discard of the blended passes. */
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
} pc;

#define OP_COVER_SRC g
#include "portal_cover.glsl"

void main()
{
    vec4 c;
    if (ter_debug_shade(c)) { outColor = c; return; }
    if (pc.cover != 0 &&
        op_portal_covered(vec2(pc.fc.x * gl_FragCoord.x + pc.fc.y, pc.fc.z * gl_FragCoord.y + pc.fc.w),
                          op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED)))
        discard;
    if (ter_key_loses()) discard;
    c = ter_stock_shade();
    if (c.a == 0.0) discard;              /* PSX transparent-texel sentinel */
    outColor = c;
}
