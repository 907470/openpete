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
/* True view-Z, when the feeder shifted z to encode OT ordering (0 = unshifted,
 * derive W from z as above). The depth WRITTEN is always the z the feeder sent
 * — only the interpolation W changes — so ordering is untouched either way. */
layout(location = 4) in float inW;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

/* Depth encoding, chosen at pipeline creation (RHI vertex spec-constant 0):
 * false = legacy z=vz/(vz+4096) (hyperbolic in vz, interpolated screen-linearly
 * => ordering flips at near-coplanar faces); true = z_ndc=(W-1)/W=1-1/vz, which
 * IS affine in 1/vz => screen-affine => exact interpolation. W (=vz) is unchanged
 * either way, so perspective-correct UV is identical. Same monotonic near->far
 * mapping, so compare op / clear value are unchanged. */
layout(constant_id = 0) const bool DEPTH_AFFINE = false;

/* PSX-affine interpolation (--texture-mapping affine, spec-constant 1): emit
 * w=1 so the rasterizer interpolates UV/colour SCREEN-LINEARLY — the PSX GPU
 * had no perspective divide, and this is its authentic texture swim/warp.
 * Screen position is inPos.xy either way, and the window depth zc/W is the
 * same value the perspective path's hardware divide produces, so depth
 * testing/writing is identical across the two modes. */
layout(constant_id = 1) const bool AFFINE_UV = false;

void main()
{
    float z   = clamp(inPos.z, 0.0, 0.9999);        /* bound W (z->1 => W->inf) */
    float vzo = 4096.0 * z / max(1.0 - z, 1e-6);     /* view-Z the DEPTH encodes */
    vzo = max(vzo, 1.0);                              /* near-plane / z==0 guard  */
    float W = inW > 0.0 ? max(inW, 1.0) : vzo;       /* perspective W (true vz)  */
    /* Window depth stays a function of vzo alone (the feeder's ordering value);
     * W only rescales the homogeneous coords. W/vzo folds to exactly 1.0 on the
     * unshifted path, so both encodings are bit-identical to deriving W from z. */
    float zc = DEPTH_AFFINE ? (vzo - 1.0) * (W / vzo) : (z * W);   /* z_ndc = zc/W */
    gl_Position = AFFINE_UV ? vec4(inPos.xy, DEPTH_AFFINE ? (vzo - 1.0) / vzo : z, 1.0)
                            : vec4(inPos.xy * W, zc, W);
    vUV = inUV;
    vColor = inColor;
}
