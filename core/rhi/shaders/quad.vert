#version 450

/* Vertex-coloured scene stage — PERSPECTIVE-CORRECT (matches psx_scene.vert).
 * The CPU feeds post-projection 2D clip-space screen xy + the NDC depth
 * z = vz/(vz+4096) (openpete_native_depth_from_viewz). Recover the homogeneous
 * W (== view-Z) from z and re-homogenise so the rasterizer interpolates the
 * vertex colour PERSPECTIVE-CORRECT instead of affine:
 *   W = 4096*z/(1-z) ;  gl_Position = (clip.xy*W, z*W, W)
 * Screen position and window-space depth are unchanged (clip.xy*W/W; z*W/W = z,
 * still screen-linear), so depth sorting is identical — only the gouraud colour
 * interpolation gains perspective. Used only by the depth-tested scene pipes
 * (terrain LP, Spyro body, shadows, glows); the fullscreen blit/diff passes use
 * their own shaders. */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen, z = NDC depth */
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;  /* R8G8B8A8_UNORM -> 0..1 */
/* True view-Z when the feeder shifted z to encode OT ordering (0 = unshifted).
 * See psx_scene.vert — same rule, so both scene pipes share one vertex buffer. */
layout(location = 4) in float inW;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
/* PSX screen coord (inUV, for the shadow decal) interpolated SCREEN-LINEAR, so
 * shadow_decal.frag recovers each fragment's exact PSX screen position with no
 * mode-specific gl_FragCoord->screen affine. noperspective: screen position is
 * affine in screen space, not perspective-correct. Other frags ignore it. */
layout(location = 2) noperspective out vec2 vScreenPSX;

/* Depth encoding, chosen at pipeline creation (RHI vertex spec-constant 0) —
 * see psx_scene.vert: false = legacy z=vz/(vz+4096), true = screen-affine
 * z_ndc=1-1/vz. Must match psx_scene.vert so both scene pipes share one buffer. */
layout(constant_id = 0) const bool DEPTH_AFFINE = false;

/* PSX-affine interpolation (--texture-mapping affine, spec-constant 1): w=1
 * makes the gouraud colour interpolate SCREEN-LINEARLY like the PSX GPU
 * (no perspective divide). Same screen position; the window depth zc/W is
 * the value the perspective path's divide produces, so depth behaviour is
 * identical. vScreenPSX is already noperspective — unaffected either way.
 * Must match psx_scene.vert so both scene pipes stay consistent. */
layout(constant_id = 1) const bool AFFINE_UV = false;

void main()
{
    float z   = clamp(inPos.z, 0.0, 0.9999);        /* bound W (z->1 => W->inf) */
    float vzo = 4096.0 * z / max(1.0 - z, 1e-6);     /* view-Z the DEPTH encodes */
    vzo = max(vzo, 1.0);                              /* near-plane / z==0 guard  */
    float W = inW > 0.0 ? max(inW, 1.0) : vzo;       /* perspective W (true vz)  */
    float zc = DEPTH_AFFINE ? (vzo - 1.0) * (W / vzo) : (z * W);   /* z_ndc = zc/W */
    gl_Position = AFFINE_UV ? vec4(inPos.xy, DEPTH_AFFINE ? (vzo - 1.0) / vzo : z, 1.0)
                            : vec4(inPos.xy * W, zc, W);
    vColor = inColor;
    vUV = inUV;
    vScreenPSX = inUV;
}
