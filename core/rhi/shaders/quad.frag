#version 450

/* Output the interpolated vertex color, perspective-divided by vQ. The
 * PSX 4/8/16bpp + CLUT VRAM sampler is psx_sample.frag. */

layout(location = 0) in vec4 vColor;  /* x ratio (divide by vQ) */
layout(location = 1) in vec2 vUV;
layout(location = 3) in float vQ;     /* perspective ratio (see quad.vert) */

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor / vQ;
}
