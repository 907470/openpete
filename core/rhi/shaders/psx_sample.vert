#version 450

/* PSX VRAM sampler, vertex stage.
 *
 * Passthrough of the fixed native vertex layout (position / uv / color).
 * UVs are in TEXEL units within the texture page (0..w, 0..h) - the PSX
 * sampler indexes VRAM by integer texel, exactly like the hardware. The
 * per-draw texture page / CLUT / bpp ride a push constant (one material per
 * draw batch), not the packed per-vertex `tex` field: a native renderer
 * sorts by material, so PSX-era per-vertex tpage packing is unnecessary. */

layout(location = 0) in vec3 inPos;   /* clip space for the smoke test */
layout(location = 1) in vec2 inUV;    /* texel coords within the texture */
layout(location = 2) in vec4 inColor; /* R8G8B8A8_UNORM -> 0..1 modulate */

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main()
{
    gl_Position = vec4(inPos, 1.0);
    vUV = inUV;
    vColor = inColor;
}
