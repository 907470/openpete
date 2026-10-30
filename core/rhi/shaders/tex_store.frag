#version 450

/* The record colour times texel (0, 0) of the image at set 2 binding 0 of a
 * tex_ssbo_set2 pipeline (storage_pull_tex.vert): an unbound or wrong image
 * changes the output. */

layout(set = 2, binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor * texelFetch(u_tex, ivec2(0, 0), 0);
}
