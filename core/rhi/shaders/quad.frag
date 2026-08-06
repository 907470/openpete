#version 450

/* Milestone 1b.1 — output the interpolated vertex color. 1b.2 replaces
 * this with the ported PSX 4/8/16bpp + CLUT VRAM sampler. */

layout(location = 0) in vec4 vColor;  /* x ratio (divide by vQ) */
layout(location = 1) in vec2 vUV;
layout(location = 3) in float vQ;     /* perspective ratio (see quad.vert) */

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor / vQ;
}
