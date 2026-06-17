#version 450

/* Scene textured vertex stage — PERSPECTIVE-CORRECT UV/colour.
 *
 * The CPU feeds post-projection 2D screen coordinates (already in clip space via
 * scene_to_clip) plus the NDC depth z = vz/(vz+4096) (openpete_native_depth_-
 * from_viewz). A plain passthrough (w=1) interpolates UV/colour AFFINELY, which
 * reproduces the PSX's non-perspective texture warp — visibly stretching ground
 * textures near the camera (grazing angle). To interpolate perspective-correct
 * instead, we recover the homogeneous W from the depth and re-homogenise:
 *
 *   z = vz/(vz+4096)  =>  vz = 4096*z/(1-z)  (the view-Z, == clip-space W)
 *   gl_Position = (clip.xy * W, z * W, W)
 *
 * After the GPU's perspective divide the screen position is unchanged
 * (clip.xy*W / W) and the window-space depth is unchanged (z*W / W = z, still
 * interpolated linearly in screen — depth/sorting identical to before), but the
 * rasterizer now divides UV/colour by W => perspective-correct sampling. No
 * vertex-format or feeder change is needed; W is derived from the existing z. */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen, z = NDC depth */
layout(location = 1) in vec2 inUV;    /* texel coords within the texture page */
layout(location = 2) in vec4 inColor; /* R8G8B8A8_UNORM -> 0..1 modulate */

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main()
{
    float z = clamp(inPos.z, 0.0, 0.9999);          /* bound W (z->1 => W->inf) */
    float W = 4096.0 * z / max(1.0 - z, 1e-6);       /* view-Z == clip-space W   */
    W = max(W, 1.0);                                  /* near-plane / z==0 guard  */
    gl_Position = vec4(inPos.xy * W, z * W, W);
    vUV = inUV;
    vColor = inColor;
}
