#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"

/* UNTEXTURED key-pass fragment stage. The stored value is written as the
 * fragment depth in the face's form (vKeyForm, op_key_stored_depth): a flat
 * key as op_key_store(vKv) (a terrain key moved in its bucket's tier order,
 * op_key_tier_key), the player band as its band value, the prop class as
 * its true view-Z at the fragment. outColor is
 * the face's flat colour channel, which carries an actor face's moby identity into the
 * actor key image's colour attachment (psx_key_contest.glsl op_akey_id);
 * a terrain face (vCls a class byte) writes its true view-Z and class there
 * instead (op_key_attr_store); the prop image's colour is never read. */

layout(location = 5) flat in float vKv;      /* flat key view-Z, or the band value */
layout(location = 6) flat in vec4 vIdent; /* moby identity (op_akey_id) */
layout(location = 7) noperspective in float vIz; /* owner's true 1/view-Z */
layout(location = 8) flat in float vCls;          /* owner's terrain class byte */
layout(location = 9) flat in float vKeyForm;      /* OP_KEY_FORM_* */

layout(location = 0) out vec4 outColor;

/* Fragment spec-constant 4 (`SLOT_CONTEST`): the scene is submitted through
 * one ordering table; a terrain key is stored in its bucket's tier order
 * (op_key_tier_key). Set per game from game_layout.terrain.slot_ot_contest. */
layout(constant_id = 4) const bool SLOT_CONTEST = false;

void main()
{
    float kv = vKeyForm > 0.5 ? op_key_tier_key(vKv, vCls, SLOT_CONTEST) : vKv;
    gl_FragDepth = op_key_stored_depth(vKeyForm, kv, vIz);
    outColor = vCls < OP_KEY_CLASS_NONE - 0.5 ? op_key_attr_store(1.0 / max(vIz, 1e-9), vCls)
                                              : vIdent;
}
