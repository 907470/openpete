#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"
#define OP_SHELL_PREPASS 1
#include "moby_shell.glsl"

/* Moby front-shell intruder pre-pass fragment stage (vertex stage
 * psx_scene.vert; moby_shell.glsl): keeps, per pixel, the nearest intruder
 * behind the shell in the scene's own depth encoding. Intruders are the
 * candidates (shell_intruder_face) of another moby or of no moby, and the
 * record's own blended faces behind its nearest front (a second translucent
 * layer); the record's own opaque faces are discarded. The feed carries the
 * face's moby identity in the rank attribute's x (0 for a face of no moby)
 * and 1 in its y on a blended face. */

layout(location = 4) flat in vec2 vRank;

layout(location = 0) out vec4 outColor;

/* gl_FragCoord.z convention - see psx_scene_key.frag. */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    float s = texelFetch(u_shelldepth, p, 0).r;
    float own = gl_FragCoord.z;
    if (DEPTH_REVERSED ? own >= s : own <= s) discard;      /* not behind the shell */
    bool mine = vRank.x > 0.5 && abs(op_akey_id(texelFetch(u_shellown, p, 0)) - vRank.x) < 0.5;
    if (mine && vRank.y < 0.5) discard;                     /* the record's own opaque face */
    outColor = vec4(0.0);
}
