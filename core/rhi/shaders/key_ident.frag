#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"

/* UNTEXTURED key-pass fragment stage. A flat key (vKeyFlat) is stored as
 * op_key_store(vKv), the exact encoding of the face's key, not the
 * rasterised depth, which lands a few ulps off it by GPU; the prop class and
 * the player band keep the rasterised depth (terrain_key.vert). outColor is
 * the face's flat colour channel, which carries an actor face's moby identity into the
 * actor key image's colour attachment (psx_key_contest.glsl op_akey_id);
 * a terrain face (vCls a class byte) writes its true view-Z and class there
 * instead (op_key_attr_store); the prop image's colour is never read. */

layout(location = 5) flat in float vKv;      /* flat key view-Z */
layout(location = 6) flat in vec4 vIdent; /* moby identity (op_akey_id) */
layout(location = 7) noperspective in float vIz; /* owner's true 1/view-Z */
layout(location = 8) flat in float vCls;          /* owner's terrain class byte */
layout(location = 9) flat in float vKeyFlat;      /* 1: store op_key_store(vKv) */

layout(location = 0) out vec4 outColor;

void main()
{
    gl_FragDepth = vKeyFlat > 0.5 ? op_key_store(vKv) : gl_FragCoord.z;
    outColor = vCls < OP_KEY_CLASS_NONE - 0.5 ? op_key_attr_store(1.0 / max(vIz, 1e-9), vCls)
                                              : vIdent;
}
