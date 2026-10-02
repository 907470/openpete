#version 450

/* Moby RANK pass vertex stage: rasterise ranked moby faces with depth =
 * their og OT walk RANK (later paint = nearer), into the RGBA8 rank image
 * the scene fragments run the same-moby painter select against
 * (psx_scene_key.frag / quad_key.frag / hd_tex_cutout.frag).
 *
 * The image is PRIVATE and the winner rule is pure ordering, so depth is a
 * direct linear encode: z_ndc = 1 - (rank+1)/65536 - the depth LESS test
 * keeps the HIGHEST rank (og's last-painted face) per pixel. Rank is FLAT
 * per face, so any monotone encoding raster-interpolates exactly; W == 1
 * keeps the encode through the hardware clipper. The fragment stage writes
 * (rank, ordinal) as bytes; cleared pixels decode to rank 0 = "no cover".
 *
 * Textured faces cut transparent texels out of the raster (moby_rank_cutout
 * .frag) - UV interpolation mirrors terrain_key.vert's manual (u/w, 1/w)
 * pair against the TRUE per-vertex view-Z in inW, so the cutout's texel
 * lookups land on the same texels the scene draw samples. */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen; z unused here */
layout(location = 1) in vec2 inUV;
layout(location = 3) in vec2 inRank;  /* x = OT walk rank (>=1), y = moby ordinal */
layout(location = 4) in float inW;    /* TRUE per-vertex view-Z (textured cutout UV) */

layout(location = 0) flat out vec2 vRank;
layout(location = 1) out vec2 vUV;    /* u/w */
layout(location = 2) out float vQ;    /* 1/w */
layout(location = 3) out float vVz;   /* TRUE view-Z (owner-depth encode) */

/* PSX-affine UV - must track the SCENE stages' selection (see
 * terrain_key.vert: a mismatched lookup stamps rank where the face's own
 * paint discards). */
layout(constant_id = 1) const bool AFFINE_UV = false;

void main()
{
    float r = clamp(inRank.x, 0.0, 65534.0);
    gl_Position = vec4(inPos.xy, 1.0 - (r + 1.0) / 65536.0, 1.0);
    float wq = AFFINE_UV ? 1.0 : max(inW, 1.0);
    vRank = inRank;
    vUV = inUV / wq;
    vQ = 1.0 / wq;
    vVz = max(inW, 1.0);
}
