#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"

/* UNTEXTURED key-pass fragment stage. Depth is the flat og key written by
 * terrain_key.vert; this stage writes no depth. outColor is the face's flat
 * colour channel, which carries an actor face's moby identity into the
 * actor key image's colour attachment (psx_key_contest.glsl op_akey_id);
 * a terrain face (vCls a class byte) writes its true view-Z and class there
 * instead (op_key_attr_store); the prop image's colour is never read. */

layout(location = 6) flat in vec4 vIdent; /* moby identity (op_akey_id) */
layout(location = 7) noperspective in float vIz; /* owner's true 1/view-Z */
layout(location = 8) flat in float vCls;          /* owner's terrain class byte */

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vCls < OP_KEY_CLASS_NONE - 0.5 ? op_key_attr_store(1.0 / max(vIz, 1e-9), vCls)
                                              : vIdent;
}
