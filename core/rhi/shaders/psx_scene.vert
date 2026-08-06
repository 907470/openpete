#version 450

/* Scene textured vertex stage — PERSPECTIVE-CORRECT UV/colour with
 * CLIP-INVARIANT depth.
 *
 * The CPU feeds post-projection 2D screen coordinates (already in clip space
 * via scene_to_clip) plus the NDC depth z (openpete_native_depth_from_viewz;
 * the encoding is selected by DEPTH_AFFINE below). Two view-Z values meet
 * here and they are NOT always the same:
 *
 *   - vzo: the view-Z the DEPTH encodes. Feeders may shift it to port the
 *     PSX OT's authored ordering (terrain slot offsets, moby tie pulls).
 *   - inW: the TRUE view-Z (perspective W) for UV/colour interpolation
 *     (0 = unshifted; derive from z).
 *
 * The homogeneous position MUST use W == vzo. gl_Position's (zc, W) pair is
 * linearly interpolated by the hardware CLIPPER; only a matched pair keeps
 * window depth screen-linear through clipping. An earlier version emitted
 * W = inW with zc built from vzo — exact per vertex, but any partially
 * off-screen shifted triangle got its clip-vertex depth perspective-mixed
 * toward the near vertex, which could pull a distant face in FRONT of
 * everything (a screen-huge lake face with one near-camera vertex covered
 * the ground with its own texture wherever the clipper ran).
 *
 * UV/colour still interpolate perspective-correct in inW, exactly: emit
 * attr*q with q = vzo/inW plus the scalar q itself. The rasterizer divides
 * varyings by W=vzo, so attr*q/vzo == attr/inW and q/vzo == 1/inW are both
 * screen-linear; the fragment stage recovers attr = (attr*q)/q — identical
 * to hardware W=inW interpolation on unclipped triangles, and exact on
 * clipped ones. */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen, z = NDC depth */
layout(location = 1) in vec2 inUV;    /* texel coords within the texture page */
layout(location = 2) in vec4 inColor; /* R8G8B8A8_UNORM -> 0..1 modulate */
/* True view-Z, when the feeder shifted z to encode OT ordering (0 = unshifted,
 * derive W from z as above). The depth WRITTEN is always the z the feeder sent
 * — only the UV/colour interpolation basis changes. */
layout(location = 4) in float inW;
/* Terrain og OT key (rhi_vertex.koff): the face's ABSOLUTE .s insertion key
 * in world units + 0.5 sentinel (0 = not keyed); keyed fragments run the
 * key-resolve contest against it. Per-face constant -> flat. */
layout(location = 5) in float inKoff;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
/* Interpolation ratio (vzo/inW): fragment divides vUV/vColor by this to
 * recover perspective-correct-in-inW values. 1.0 on unshifted vertices. */
layout(location = 2) out float vQ;
layout(location = 3) flat out float vKoff;

/* Depth encoding, chosen at pipeline creation (RHI vertex spec-constant 0):
 * false = legacy z=vz/(vz+4096) (hyperbolic in vz, interpolated screen-linearly
 * => ordering flips at near-coplanar faces); true = z_ndc=(W-1)/W=1-1/vz, which
 * IS affine in 1/vz => screen-affine => exact interpolation. Same monotonic
 * near->far mapping, so compare op / clear value are unchanged. */
layout(constant_id = 0) const bool DEPTH_AFFINE = false;

/* PSX-affine interpolation (--texture-mapping affine, spec-constant 1): emit
 * w=1 so the rasterizer interpolates UV/colour SCREEN-LINEARLY — the PSX GPU
 * had no perspective divide, and this is its authentic texture swim/warp.
 * Screen position is inPos.xy either way, and the window depth is the same
 * value the perspective path's hardware divide produces, so depth
 * testing/writing is identical across the two modes. */
layout(constant_id = 1) const bool AFFINE_UV = false;

void main()
{
    float z   = clamp(inPos.z, 0.0, 0.9999);        /* bound W (z->1 => W->inf) */
    float vzo = 4096.0 * z / max(1.0 - z, 1e-6);     /* view-Z the DEPTH encodes */
    vzo = max(vzo, 1.0);                              /* near-plane / z==0 guard  */
    float Wu = inW > 0.0 ? max(inW, 1.0) : vzo;      /* UV/colour perspective W  */
    float q  = AFFINE_UV ? 1.0 : (vzo / Wu);
    float zc = DEPTH_AFFINE ? (vzo - 1.0) : (z * vzo);   /* z_ndc = zc/vzo */
    gl_Position = AFFINE_UV ? vec4(inPos.xy, DEPTH_AFFINE ? (vzo - 1.0) / vzo : z, 1.0)
                            : vec4(inPos.xy * vzo, zc, vzo);
    vUV = inUV * q;
    vColor = inColor * q;
    vQ = q;
    vKoff = inKoff;
}
