#version 450

/* Fullscreen textured blit — samples a normal RGBA8 image (sampler2D) at
 * set 0, binding 0 and writes it straight out. Pairs with psx_sample.vert
 * (which passes through position + UV); used to display another renderer's
 * read-back frame (the "reference" panel of the native A/B view). The VRAM
 * sampler (psx_sample.frag) uses the same descriptor slot as a usampler2D —
 * only the image format + this shader's sampler type differ. */

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(u_tex, vUV);
}
