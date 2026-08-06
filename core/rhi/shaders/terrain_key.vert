#version 450

/* Terrain HP KEY pass vertex stage: rasterise opaque HP terrain with depth =
 * the og OT KEY (true view-Z + 128*slots world) instead of true view-Z, into
 * the depth image the scene's LP fragments contest against
 * (psx_scene_key.frag / quad_key.frag).
 *
 * Like prepass.vert, the image is PRIVATE, so the encoding is fixed affine
 * (z_ndc = 1 - 1/kv — exact screen-linear interpolation for planes) no
 * matter what the main scene uses; consumers decode kv = 1/(1-d). The
 * (zc, W) pair uses W == the key view-Z so window depth survives the
 * hardware clipper (the scene stages' clip-invariance rule). */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen, z = canonical NDC depth (true vz) */
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 5) in float inKoff; /* +key offset + 0.5 sentinel (HP) */

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) noperspective out vec2 vScreenPSX;

void main()
{
    float z  = clamp(inPos.z, 0.0, 0.9999);
    float vz = 4096.0 * z / max(1.0 - z, 1e-6);    /* TRUE view-Z, world units */
    vz = max(vz, 1.0);
    float kv = vz + max(abs(inKoff) - 0.5, 0.0);   /* og key view-Z */
    gl_Position = vec4(inPos.xy * kv, kv - 1.0, kv);   /* z_ndc = 1 - 1/kv */
    vColor = inColor;
    vUV = inUV;
    vScreenPSX = inUV;
}
