#version 450

/* Milestone 1b.1 — vertex-colored passthrough. The native vertex layout is
 * fixed (position / uv / color / packed-tex-descriptor) so the same buffer
 * format feeds the textured PSX shaders that arrive in 1b.2; this stage
 * consumes only position + color + uv for now. */

layout(location = 0) in vec3 inPos;   /* clip space for 1b.1 */
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;  /* R8G8B8A8_UNORM -> 0..1 */

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;

void main()
{
    gl_Position = vec4(inPos, 1.0);
    vColor = inColor;
    vUV = inUV;
}
