#version 450

/* Terrain KEY pass vertex stage: rasterise opaque keyed terrain with depth =
 * the og OT KEY (true view-Z + 128*slots world) instead of true view-Z, into
 * the depth image the scene fragments contest against
 * (psx_scene_key.frag / quad_key.frag / hd_tex_cutout.frag).
 *
 * The image is PRIVATE, so the encoding is free — and it must be the
 * canonical hyperbolic map z = kv/(kv+4096), NOT the affine 1 - 1/kv:
 * near z==1 the D32 image quantises at ~2^-24 absolute, so an affine-coded
 * key decodes with error ~kv^2 * 2^-25 world units, which crosses the
 * contest's 64-world tie margin at kv ~= 46k — beyond that distance faces
 * failed their OWN key test and whole faces vanished. The canonical map's
 * decode error is (kv+4096)^2/4096 * 2^-25 (~4096x finer at range), <64
 * out past kv ~= 3e6. Keys are FLAT per face (all three verts carry the
 * same kv), so any monotone encoding raster-interpolates exactly; the
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
    gl_Position = vec4(inPos.xy * kv, kv * kv / (kv + 4096.0), kv);   /* z_ndc = kv/(kv+4096) */
    vColor = inColor;
    vUV = inUV;
    vScreenPSX = inUV;
}
