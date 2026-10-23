#version 450

/* UNTEXTURED key-pass fragment stage. Depth is the flat og key written by
 * terrain_key.vert; this stage writes no depth. outColor is the face's flat
 * colour channel, which carries an actor face's moby identity into the
 * actor key image's colour attachment (psx_key_contest.glsl op_akey_id);
 * the terrain and prop images never read their colour. */

layout(location = 6) flat in vec4 vIdent; /* moby identity (op_akey_id) */

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vIdent;
}
