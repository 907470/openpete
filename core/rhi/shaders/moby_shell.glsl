/* moby_shell.glsl - front-shell depth of a moby record that draws blended
 * and opaque faces, for the opaque scene stages' _shell variants
 * (OP_MOBY_SHELL: quad_key, psx_scene_key, hd_tex_cutout).
 *
 * The games' single-bucket moby draws (Spyro 1 shaded walk, Spyro 2/3
 * simple walk) link every prim of one record into one world ordering-table
 * bucket, so the record paints whole against the terrain: a gem's opaque
 * back face and its blended front face paint over terrain that lies between
 * them. Native true depth hides the back face there and the blended front,
 * which writes no depth, composes over the terrain instead.
 *
 * The shell pre-pass (moby_shell.frag) rasterises the blended faces of every
 * such record into a depth-sampled image: depth = the nearest front face in
 * the scene's own encoding, colour = that face's moby identity (rgb, the
 * actor key owner encoding op_akey_id) and its present-local rank ordinal
 * (a). An opaque fragment of the same moby behind the shell writes depth
 * shell + (own - shell) / OP_SHELL_K instead of its own: it stays behind
 * the front face, and in front of terrain more than 1/OP_SHELL_K of the
 * interval behind the front. Terrain nearer than the front keeps occluding
 * it. Everywhere else the fragment writes its own depth.
 *
 * A face of another moby or of the player between front and back (a
 * particle or glow inside the record's volume, a body interpenetrating it)
 * keeps the order true depth gives it: the original paints such blended
 * faces after the record, and an opaque one in front of the back face is
 * in front of it. The record's own blended faces behind its nearest front
 * (a second translucent layer) count with them. The intruder pre-pass
 * (moby_shell_intr.frag) keeps, per pixel, the nearest such face behind the
 * shell; where it lies in front of
 * the fragment, the fragment writes its own depth, so every such face keeps
 * the order true depth gives it, and terrain between front and back keeps
 * occluding the fragment at that pixel. */

#ifndef OPENPETE_MOBY_SHELL_GLSL
#define OPENPETE_MOBY_SHELL_GLSL

/* The fraction of the front-to-back interval an in-shell fragment keeps:
 * 1/64 of a gem 100-190 units thick is 1.6-3.0 units, and the exported
 * value stays strictly between the shell and the fragment's own depth. */
#define OP_SHELL_K 64.0

/* The shell image: depth at binding 0, owner colour at binding 1. */
layout(set = 5, binding = 0) uniform sampler2D u_shelldepth;
layout(set = 5, binding = 1) uniform sampler2D u_shellown;
#ifndef OP_SHELL_PREPASS
/* The intruder image's depth (binding 1, its colour, is not read). */
layout(set = 6, binding = 0) uniform sampler2D u_shellintr;
#endif

/* Whether the shell texel belongs to the fragment's own moby. A ranked
 * fragment (rank.x > 0.5) carries its rank ordinal in rank.y, matched
 * against the texel's alpha; an unranked actor fragment carries its moby
 * identity (op_akey_owner), matched against the texel's rgb. A cleared
 * texel (all zero) matches neither, since both values are at least 1. */
bool op_shell_owns(vec4 c, vec2 rank)
{
    if (rank.x > 0.5) return abs(floor(c.a * 255.0 + 0.5) - rank.y) < 0.5;
    float id = op_akey_owner(rank);
    return id > 0.5 && abs(op_akey_id(c) - id) < 0.5;
}

/* The window depth an opaque fragment writes: inside its own moby's shell
 * and behind it, shell + (own - shell) / OP_SHELL_K in the depth buffer's
 * encoding (affine in 1/view-Z, so the value lies between the two depths
 * whatever the convention); its rasterised depth outside the shell, in
 * front of it, and where an intruder lies between the shell and it. */
#ifndef OP_SHELL_PREPASS
float op_shell_depth(vec2 rank, bool reversed)
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    float own = gl_FragCoord.z;
    float s = texelFetch(u_shelldepth, p, 0).r;
    bool behind = reversed ? own < s : own > s;
    if (!behind || !op_shell_owns(texelFetch(u_shellown, p, 0), rank)) return own;
    float i = texelFetch(u_shellintr, p, 0).r;
    bool between = reversed ? (i < s && i > own) : (i > s && i < own);
    return between ? own : s + (own - s) / OP_SHELL_K;
}
#endif

#endif /* OPENPETE_MOBY_SHELL_GLSL */
