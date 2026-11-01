#version 450

/* The launcher shape pass's vertex stage (core/renderer/launcher_shape_pass.c):
 * one quad per shape, its corners already in clip space. Every shading term
 * is the fragment stage's, from gl_FragCoord and the per-draw push block. */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen position */

void main()
{
    gl_Position = vec4(inPos.xy, 0.0, 1.0);
}
