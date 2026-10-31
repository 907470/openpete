#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_TERRAIN_ACTOR_KEY 1
#include "terrain_shade.glsl"
#include "psx_src_tint.glsl"

/* Terrain sector-mesh fragment stage. Everything it does lives in
 * terrain_shade.glsl, shared with the material tier's terrain engine half so
 * the two cannot drift; this file is the stock assembly of those pieces. */

layout(location = 0) out vec4 outColor;

void main()
{
    ter_texel_grad_init();
    ter_key_plane_init();
    vec4 c;
    if (ter_debug_shade(c)) { outColor = c; return; }
    if (ter_portal_discards()) discard;   /* portal plane-cover rule */
    if (ter_key_loses()) discard;
    int ko = ter_key_order();             /* key-order moby in front by key */
    if (ko == 1) discard;
    c = ter_stock_shade();
    if (c.a == 0.0) discard;              /* PSX transparent-texel sentinel */
    outColor = ko == 2 ? vec4(0.0, 1.0, 1.0, 1.0) : c;
    if (op_key_tier_hit >= 0.0) outColor = op_key_tier_mark(op_key_tier_hit);
    outColor = op_src_tint_cache(outColor);
    if (ter_attr(c)) outColor = c;        /* debug tint mode 7 */
}
