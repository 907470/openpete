#version 450
#extension GL_ARB_gpu_shader_fp64 : require
#extension GL_GOOGLE_include_directive : require
#include "psx_unorm8.glsl"
#include "psx_viewz.glsl"
#include "psx_key_contest.glsl"

/* Terrain sector-mesh vertex stage - the retained-terrain GPU path
 * (extraction rearchitecture, seam 2).
 *
 * Mirrors - bit-for-bit - what the CPU walk (terrain.c walk_sector +
 * terrain_feed_native) computes per present for an UNCLAIMED sector: the GTE
 * camera transform (ngte_mac + proj_screen), the per-face verdicts (NCLIP
 * two-half back-face cull with the UNR-overflow regime, the .s LP far-cull
 * and authored admission, the HP LOD drop, face_is_near flat-vs-HQ variant
 * select), the CLUT-ladder / near-tile material select, the GTE-exact DPCS
 * fog blend, and the og OT-key (koff) for the key-resolve contest. Every
 * vertex carries its parent face's four corner positions, so face verdicts
 * are evaluated (redundantly but face-uniformly) per vertex; a culled face
 * collapses all of its vertices to a degenerate point.
 *
 * EXACTNESS CONTRACT: the CPU walk does this math in double and rounds each
 * screen coordinate to float ONCE. This stage re-runs the same ops in the
 * same order in fp64 (`precise` - no FMA contraction) and casts at the same
 * points, so the emitted float positions are identical and the raster is
 * pixel-identical. HQ sub-quad nodes are built by averaging the TRANSFORMED
 * corner values (cv_mid mirrors), never by transforming baked midpoints -
 * the CPU's float rounding differs between the two.
 *
 * Sector ownership is decided on the CPU (sector-params UBO): a claimed
 * sector renders through the unchanged CPU walk and is killed here, so no
 * face is ever drawn by both paths. Claimed sectors include everything that
 * may approach the near plane - an unclaimed vertex is GUARANTEED m3 well
 * in front of the camera, which is what makes the raw H/m3 projection (no
 * near clipping, no straddle handling) safe here. */

#ifndef TERRAIN_CELLS
layout(location = 0) in ivec4 inPos;      /* xyz: sector-local x4 fixed; w: flags16 */
layout(location = 1) in uvec4 inUV;       /* far u,v, near u,v (texels)   */
layout(location = 2) in vec4  inColFar;   /* far colour  (UNORM 0..1)     */
layout(location = 3) in vec4  inColNear;  /* near colour (UNORM 0..1)     */
layout(location = 4) in uvec4 inMat;      /* far0, far1, near0, near1     */
layout(location = 5) in uint  inFaceFlags;
layout(location = 6) in ivec4 inC0;       /* c0.xyz, c1.x   (x4 fixed)    */
layout(location = 7) in ivec4 inC1;       /* c1.yz, c2.xy                 */
layout(location = 8) in ivec4 inC2;       /* c2.z, c3.xyz                 */
#endif
#if defined(TERRAIN_ANIM) && !defined(TERRAIN_CELLS)
/* Poly-anim chunks (binding 1, rhi_terrain_prev): previous-tick twin of the
 * position + parent corners. Every local coordinate is lerped prev->cur in
 * fp64 at the build's alpha and CAST TO FLOAT before the camera transform -
 * the exact mirror of tsc_vanim_lerp filling cvF (alpha = 1 on canonical
 * builds collapses to cur bit-exactly, ints casting to float losslessly). */
layout(location = 9)  in ivec4 inPrevPos; /* prev xyz (x4 fixed), pad     */
layout(location = 10) in ivec4 inPrevC0;
layout(location = 11) in ivec4 inPrevC1;
layout(location = 12) in ivec4 inPrevC2;
/* Previous-tick far and near colour words (R,G,B,A bytes): equal to the
 * current colours except on an env-anim colour sector. */
layout(location = 13) in uvec2 inPrevCol;
#endif
#if defined(TERRAIN_NEAR) && !defined(TERRAIN_CELLS)
/* Near-variant chunks (binding 1, rhi_terrain_nearctx): the OTHER two
 * emitted-tri members' attributes, in emit order relative to self
 * (a = self+1, b = self+2 mod 3). Positions need no context - every vertex
 * already carries the parent face corners the whole triangle is built from.
 * With these the vertex stage can resolve all three members' pre-clip colour
 * and UV and re-run the walk's near-plane clip fan. */
layout(location = 9)  in uvec4 inCtxAUV;  /* a: uf, vf, un, vn            */
layout(location = 10) in uvec2 inCtxACol; /* a: far, near (R8G8B8A8)      */
layout(location = 11) in uvec4 inCtxBUV;
layout(location = 12) in uvec2 inCtxBCol;
#endif
#ifdef TERRAIN_CELLS
/* 4x4 near-cell records (rhi_terrain_cell, one per INSTANCE): the face
 * corners, face flags, raw near colours and eight cell tiles. cells_fetch()
 * derives this vertex's attributes from the record and gl_VertexIndex into
 * the same names the chunk variants read from their vertex inputs, so main()
 * runs unchanged on them. Compiled with TERRAIN_NEAR (near-variant sectors,
 * 6 vertices per cell triangle) or TERRAIN_ANIM (poly-anim sectors, 3). */
/* Locations 0-1: the twelve int16 corners two to a word (c0x | c0y << 16,
 * ...), then face_flags and the record info; cells_fetch unpacks them into
 * inC0..inC2 and inCellFace. */
layout(location = 0)  in uvec4 inCellCA;    /* c0x..c2y                   */
layout(location = 1)  in uvec4 inCellCB;    /* c2z..c3z, face_flags, info */
layout(location = 2)  in uvec4 inCellFar;   /* raw far colours i0..i3     */
layout(location = 3)  in uvec2 inCellFog;   /* zero: the fade words are built in this stage */
layout(location = 4)  in uvec4 inCellNear;  /* raw near colours i0..i3    */
layout(location = 5)  in uvec4 inCellT0;    /* tile: uv TL,TR | uv BL,BR | mat0 | mat1 */
layout(location = 6)  in uvec4 inCellT1;
layout(location = 7)  in uvec4 inCellT2;
layout(location = 8)  in uvec4 inCellT3;
layout(location = 9)  in uvec4 inCellT4;
layout(location = 10) in uvec4 inCellT5;
layout(location = 11) in uvec4 inCellT6;
layout(location = 12) in uvec4 inCellT7;
#ifdef TERRAIN_ANIM
layout(location = 13) in uvec4 inCellPA;    /* previous-tick corners,     */
layout(location = 14) in uvec2 inCellPB;    /* packed as inCellCA/CB      */
ivec4 inPrevC0, inPrevC1, inPrevC2;
#endif
ivec4 inC0, inC1, inC2;
uvec2 inCellFace;
uvec2 cellHqFog;
ivec4 inPos;
uvec4 inUV;
vec4  inColFar;
vec4  inColNear;
uvec4 inMat;
uint  inFaceFlags;
#ifdef TERRAIN_ANIM
ivec4 inPrevPos;
#endif
#ifdef TERRAIN_NEAR
uvec4 inCtxAUV;
uvec2 inCtxACol;
uvec4 inCtxBUV;
uvec2 inCtxBCol;
#endif
#endif

layout(push_constant) uniform TerrainPC {
    vec4 cbase;   /* x_base, x_span, y_base, y_span (scene_to_clip terms)  */
    vec4 cdims;   /* disp_w, clip_h, band, freeze_wf                       */
    int  pass_sel;      /* 0 = WORLD, 1 = FILL (LP hole-fill), 2 = KEY    */
    int  full_extent;   /* full-target branch of the mapping              */
    /* Material pass this draw is: -1 = every face (no material pass, and
     * the depth/key passes, which shade nothing); 0 = the stock pass;
     * >0 = a material's pass. With mat_pass >= 0 a face is kept when its
     * resolution class (baked into the face) is set in cls_mask, so a
     * face's pass membership depends on the face and this present's
     * class resolution, not on the camera or on whether the sector is
     * resident. */
    int  mat_pass;
    /* --vertex-precision integer: 1 = quantise the emitted screen point to the
     * PSX integer pixel grid (the polygon wobble), 0 = sub-pixel. Latched per
     * present from the renderer's live snap state rather than baked as a spec
     * constant, so the runtime setter needs no pipeline rebuild, and carried
     * here rather than in the globals UBO because that block is re-uploaded
     * only when the terrain walk publishes new state - a snap toggled between
     * publishes would lag the CPU walk by one publish and the two feeds would
     * disagree for those presents. */
    int  snap;
    int  cover;          /* fragment-stage fields (terrain_shade.glsl)     */
    int  pad0, pad1, pad2;
    vec4 fc;
    /* The resolution classes this material pass keeps: bit c of word c/32. */
    uvec4 cls_mask[2];
} pc;

/* scene_to_clip mirror, term for term in float - a fitted affine loses ~5
 * subpixel LSBs at the screen edges and flips rasterized edge pixels. */
vec2 clip_of(vec2 sxy) {
    float nx = sxy.x / pc.cdims.x;
    float ny = (sxy.y - pc.cdims.z) / pc.cdims.y;
    if (pc.full_extent != 0)
        return vec2((-1.0 + nx * 2.0) / pc.cdims.w, 1.0 - ny * 2.0);
    return vec2(pc.cbase.x + nx * pc.cbase.y, pc.cbase.z - ny * pc.cbase.w);
}

/* Mirrored by native_terrain_cache.c's tc_globals_ubo (C natural layout ==
 * std140 there, which the dvec4 runs' 32-byte alignment depends on) and, as a
 * prefix, by terrain.frag - a field added here must land in both. */
layout(std140, set = 5, binding = 0) uniform TerrainGlobals {
    vec4  mrow0;    /* xyz: M[0][*] (raw int16 values)                    */
    vec4  mrow1;    /* xyz: M[1][*]                                       */
    vec4  mrow2;    /* xyz: M[2][*]                                       */
    dvec2 dcam_xy;  /* camx, camy (fractional, world/16)                  */
    dvec2 dcam_zf;  /* camz, s_fogscale                                   */
    dvec2 dof_xy;   /* ofx, ofy (GTE screen offset, px)                   */
    dvec2 dH;       /* x: H (projection distance), y: poly-anim sub-tick
                     * alpha (1.0 on canonical builds; TERRAIN_ANIM only)   */
    dvec2 dnear;    /* x: near-plane view-Z (TERRAIN_NEAR clip mirror)      */
    dvec2 dspare;   /* x: H_canon, the GTE's H (limit-regime mirrors);
                     * y: env-anim colour alpha (1.0 selects the current
                     * colours; TERRAIN_ANIM only)                          */
    ivec4 fog;      /* x: fogRefBase, y: gflags, z: cullRefS2, w: the LP
                     * insert's ordering-table terms (OT_* accessors)     */
    vec4  sdv[128]; /* per-tile scroll V offsets (lq, hqA, hqB, 0)        */
    /* Portal inner-rim cull frames (portal_rim_classify mirror, fp64).
     * rim_cnt.x = active plane count; 0 disables the test (no portal, or
     * rim-candidate sectors claim to the CPU walk instead). Plane pi's
     * outline lives at rim_pv[pi*16 .. pi*16+vn-1]. */
    ivec4 rim_cnt;
    ivec4 hp_drop;  /* x: HP drop sum bound, y: corner bound (x = 0: the
                     * sum against 4 * fogRefBase alone); z: the heavy-fog
                     * colour 0x00BBGGRR (GF_HPFOG)                       */
    dvec4 rim_oxyH;      /* rim ofx, ofy, H, pad                          */
    dvec4 rim_knobs;     /* eps, near, reach, cos(angle)                  */
    dvec4 rim_abc[6];    /* screen inv-vz plane fit a,b,c; w = valid      */
    dvec4 rim_O[6];      /* in-plane basis origin (view space); w = 1 when
                          * the plane discards its rim per fragment       */
    dvec4 rim_U[6];      /* in-plane basis U                              */
    dvec4 rim_W[6];      /* in-plane basis W                              */
    dvec4 rim_nrm[6];    /* unit view normal; w = outline vn              */
    dvec4 rim_bbox[6];   /* outline UV bbox pulo, puhi, pvlo, pvhi        */
    dvec2 rim_pv[96];    /* outlines in (U,W), 16 slots per plane         */
    vec4  hd_lad;        /* HD fold: CONT-fog ramp ts, tb; count; enable  */
    vec4  hd_ent[1664];  /* 416 entries x 4 vec4 (rect/atlas/fogA/fogB)   */
    /* Portal cover planes, read by terrain.frag (portal_cover.glsl): active
     * opening planes in SCREEN space. cov_cnt.x = plane count (0 = test
     * off); cov_abc.w = the plane's outline point count (pn < 3 never
     * matches). One outline point per cov_poly slot, 16 slots per plane. */
    ivec4 cov_cnt;
    dvec4 cov_margin;    /* x = plane-cover near margin M (world units)   */
    dvec4 cov_abc[6];    /* screen inv-vz plane fit a, b, c; w = pn       */
    vec4  cov_bbox[6];   /* outline screen bbox x0, y0, x1, y1            */
    vec4  cov_poly[96];  /* outline points (px, py, 0, 0)                 */
    vec4  sdvc[128];     /* per-tile 4x4 near-cell row V offsets (rows 0-3) */
} g;

/* g.fog.y bits - keep in sync with openpete_terrain_cache.h OPTC_GF_* */
#define GF_NEARHQ    1
#define GF_CLUTFOG   2
#define GF_CULLHP    4
#define GF_HANDOFF   8
#define GF_GAPFILL   16
#define GF_NOLOD     32
#define GF_CULLLP    64
#define GF_KEYON     128
#define GF_HQPIN     256
#define GF_LADNEAR   512
#define GF_NBKEYS    1024
#define GF_CELLS4    2048
#define GF_TRI4      4096
#define GF_TRI2      8192
#define GF_LPNEAROUT 16384
#define GF_HQFOG     32768
#define GF_HPRAW     65536
#define GF_HPFOG     131072

struct TerrainSector {
    vec4 o;        /* xyz: sector origin (world/16), w: uintBitsToFloat(flags) */
    vec4 gp, gq;   /* pack LP grade affine v' = p*v8 + q*255 (rgb; SF_LPGRADE) */
};
layout(std140, set = 6, binding = 0) uniform TerrainSectors {
    TerrainSector sec[512];
} s;

/* Sector flag bits - keep in sync with openpete_terrain_cache.h OPTC_SF_* */
#define SF_DRAWN      1u
#define SF_DRAWHP     2u
#define SF_DRAWLP     4u
#define SF_LPFARONLY  8u
#define SF_LPGAPONLY  16u
#define SF_FILLLP     32u
#define SF_FILLNEAR   64u
#define SF_SECINLOD   128u
#define SF_LPGRADE    256u
#define SF_NEARVAR    512u
#define SF_LPUNCOV    1024u
#define SF_LPNEAR_SHIFT 16u          /* bits 16-27: LP near-outcode SZ3 bound */
#define SF_LPNEAR_MASK  0xFFFu

/* face_flags bits - keep in sync with rhi_terrain_vertex (rhi.h) */
#define FF_SLOT_MASK  0x1FFu
#define FF_TIER_LP    (1u << 9)
#define FF_FOGBIAS    (1u << 10)
#define FF_GAPFILL    (1u << 11)
#define FF_BORROWED   (1u << 11)   /* HP: borrowed from the HP source level
                                    * (bit 11 is FF_GAPFILL on LP faces) */
#define FF_DSIDE      (1u << 12)
#define FF_FLIP       (1u << 13)
#define FF_QUAD       (1u << 14)
#define FF_W3_40      (1u << 15)
#define FF_W3_80      (1u << 16)
#define FF_LPUNCOVX   (1u << 16)   /* LP: off the HP mesh and the borrowed HP
                                    * faces (bit 16 is FF_W3_80 on HP faces) */
#define FF_HQELIG     (1u << 17)
#define FF_LPRIDE     (1u << 17)   /* LP: uncovX and no stand-in for the scene's
                                    * HP surface, drawn by the uncov-only ride
                                    * (bit 17 is FF_HQELIG on HP faces) */
#define FF_FLOOR      (1u << 18)
#define FF_PINCELLS   (1u << 19)   /* HP: the pin sends this face to the cells */
#define FF_CELLOK     (1u << 20)   /* HP: the face may take the 4x4 cells */
#define FF_KEYCELLS   (1u << 21)   /* HP: the face's drain sub-cells ride its
                                    * key-only records (OPTC_ARENA_KN / KA) */
#define FF_LPKEY      (1u << 22)   /* HP: a borrowed face standing in for an
                                    * uncov LP face; keys with the LP base term */
/* admit (LP): bits 19-23; otbias: bits 24-28 */

/* Depth convention + PSX-affine interpolation - the scene stages' spec
 * constants (psx_scene.vert), captured at pipeline creation. The cache's
 * pre-pass pipelines are FORWARD-class, so DEPTH_REVERSED is false there and
 * they write the pre-pass image's 1 - 1/vz beside prepass.vert. */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;
layout(constant_id = 1) const bool AFFINE_UV = false;

layout(location = 0)      out vec2  vUV;    /* pre-multiplied by q (scene contract) */
layout(location = 1)      out vec4  vColor; /* pre-multiplied by q                  */
layout(location = 2) flat out uvec4 vMat;   /* x: mat0, y: clx | cly<<16 (post-ladder),
                                             * zw: HQ ladder-fade words (psx_hq_fog.glsl) */
layout(location = 3) flat out float vKoff;  /* og OT key (feed koff mirror)         */
layout(location = 4)      out float vQ;     /* q (frag divides UV/colour by it)     */
layout(location = 6)      out vec2  vKfl;   /* drain face-local coordinate x vKq   */
layout(location = 7)      out float vKq;    /* vQ: p interpolates as the UV does    */
layout(location = 8) flat out uvec4 vKc0;   /* drain sub-cell key table            */
layout(location = 9) flat out uvec4 vKc1;   /*   (psx_key_contest.glsl)            */
layout(location = 10)     out vec3  vKd;    /* fold position D x vKq                */
layout(location = 11) flat out vec4 vKn0;   /* fold corners TL.xyz TR.x             */
layout(location = 12) flat out vec4 vKn1;   /*   TR.yz BL.xy                        */
layout(location = 13) flat out vec4 vKn2;   /*   BL.z BR.xyz                        */
layout(location = 14) flat out uint vRim;  /* 1 + accepting plane of an inner rim discarded per
                                            * fragment (terrain.frag); 0 = not rim */
/* The face's terrain class byte (psx_key_contest.glsl op_key_class): the
 * key pass writes it beside the key, the scene stages bound the override
 * with it. OP_KEY_CLASS_NONE on an unkeyed face. */
layout(location = 15) flat out float vKcls;
layout(location = 5) flat out uint  vHd;    /* HD-fold: entry id (bits 0-15, 0=none)
                                             * | ladder-face 1<<16 | fogbias 1<<17  */

/* ngte_mac mirror: camera-space MAC = (M*v)/4096 in DOUBLE, with the terrain
 * walk's axis permutation and per-sector camera deltas:
 *   s4 = originX - camx; s5 = camy - originY; s6 = camz - originZ;
 *   mac = M * (s5 - Y, s6 - Z, X + s4) / 4096
 * `precise` keeps mul+add separate (no FMA), matching the C compiler. */
precise dvec3 mac_of(dvec3 lpos, dvec3 sdel) {
    precise double vx = sdel.y - lpos.y;      /* s5 - Yf */
    precise double vy = sdel.z - lpos.z;      /* s6 - Zf */
    precise double vz = lpos.x + sdel.x;      /* Xf + s4 */
    precise double m1 = (double(g.mrow0.x)*vx + double(g.mrow0.y)*vy + double(g.mrow0.z)*vz) / 4096.0lf;
    precise double m2 = (double(g.mrow1.x)*vx + double(g.mrow1.y)*vy + double(g.mrow1.z)*vz) / 4096.0lf;
    precise double m3 = (double(g.mrow2.x)*vx + double(g.mrow2.y)*vy + double(g.mrow2.z)*vz) / 4096.0lf;
    return dvec3(m1, m2, m3);
}

/* proj_screen mirror: float sx = (float)(ofx + m1*H/z), with the CPU's
 * degenerate-Z floor. On plain chunks the floor never fires (an unclaimed
 * sector guarantees m3 > 0); the near variant projects pre-clip corners that
 * can sit at or behind the eye, where it does. */
precise vec2 proj_of(dvec3 m) {
    double z = m.z > 1e-9lf ? m.z : 1e-9lf;
    precise double sx = g.dof_xy.x + m.x * g.dH.x / z;
    precise double sy = g.dof_xy.y + m.y * g.dH.x / z;
    return vec2(float(sx), float(sy));
}

/* Integer vertex-snap mirror (push_tri): the emitted screen point is floored
 * to the PSX integer pixel grid. TRUNCATE, not round - the GTE derives SXY as
 * MAC0 >> 16, an arithmetic shift. X/Y only: camera-space Z, the perspective
 * q and the UVs are untouched, so depth and texturing stay unquantised. The
 * grid is display-resolution-independent because this point is pre-normalise
 * (clip_of upscales afterwards). The face verdicts above run on the UNSNAPPED
 * projections, exactly as the walk's NCLIP runs on sv[].sx. */
vec2 snap_of(vec2 s) { return pc.snap != 0 ? floor(s) : s; }

/* HQ sub-quad node ids: 0=TL 1=TR 2=BR 3=BL; 4..7 = mT,mB,mL,mR; 8 = centre.
 * sq[q] = {TL,mT,mL,C} {mT,TR,C,mR} {mL,C,BL,mB} {C,mR,mB,BR}. */
const int NODEMAP[4][4] = int[4][4](int[4](0, 4, 6, 8),
                                    int[4](4, 1, 8, 7),
                                    int[4](6, 8, 3, 5),
                                    int[4](8, 7, 5, 2));

/* terrain_near_emit's snapped-screen overrides for a subdivided face's nodes.
 *
 * Parent corners and the interior centre carry the floor of their own
 * projection, which is what the generic snap above already produces, so only
 * a BOUNDARY MIDPOINT differs: it is placed on the SEGMENT between its two
 * snapped parent corners, at the parameter of the true camera-space
 * midpoint's projection. That keeps the midpoint collinear with the snapped
 * parent edge - the unsubdivided neighbour sharing that edge draws the same
 * line, so no T-junction crack opens - while staying perspective-correct
 * along the edge, which a plain screen midpoint would not be (the HQ tiles
 * are attached to these vertices, and a screen-parametric midpoint re-warps
 * near textures the way the subdivide exists to prevent).
 *
 * The guard is per edge: an endpoint inside the near plane projects mirrored
 * and unusable, and the neighbour sharing the edge must reach the identical
 * verdict for the shared midpoint, which is a property of the two shared
 * endpoints alone. An unsafe edge gets no override and its midpoint falls
 * back to the floor of its own projection. */
vec2 hq_snap_screen(int node, dvec3 TL, dvec3 TR, dvec3 BL, dvec3 BR, dvec3 m)
{
    if (pc.snap == 0) return proj_of(m);
    if (node < 4 || node == 8) return floor(proj_of(m));
    dvec3 ea, eb;
    if      (node == 4) { ea = TL; eb = TR; }
    else if (node == 5) { ea = BL; eb = BR; }
    else if (node == 6) { ea = TL; eb = BL; }
    else                { ea = TR; eb = BR; }
    if (ea.z < g.dnear.x || eb.z < g.dnear.x) return floor(proj_of(m));
    vec2 A = floor(proj_of(ea)), B = floor(proj_of(eb));
    vec2 mm = proj_of(m);
    precise float dx = B.x - A.x, dy = B.y - A.y;
    precise float L2 = dx * dx + dy * dy;
    precise float num = (mm.x - A.x) * dx + (mm.y - A.y) * dy;
    float tt = L2 > 0.0 ? num / L2 : 0.5;
    if (tt < 0.0) tt = 0.0; else if (tt > 1.0) tt = 1.0;
    precise float rx = A.x + tt * dx, ry = A.y + tt * dy;
    return vec2(rx, ry);
}

/* ---- 4x4 near cells (terrain.c nc4_* mirrors) ----------------------------
 * A tier-4 face (face_near_tier4) draws as a 5x5 node grid, 16 cells. A
 * cell vertex carries its cell in mat_near1 (bit 31 set, cell in bits 0-3)
 * and its corner slot [TL,TR,BL,BR] in the role bits; node (r, c) is
 * rebuilt from the transformed parent corners in the walk's halving order. */
precise dvec3 nc4_pt(dvec3 a, dvec3 b, int k) {
    if (k == 0) return a;
    if (k == 4) return b;
    precise dvec3 m = (a + b) * 0.5lf;
    if (k == 2) return m;
    precise dvec3 q = ((k == 1 ? a : b) + m) * 0.5lf;
    return q;
}
precise dvec3 nc4_node(int r, int c, dvec3 TL, dvec3 TR, dvec3 BL, dvec3 BR) {
    return nc4_pt(nc4_pt(TL, BL, r), nc4_pt(TR, BR, r), c);
}

/* nc4_snap mirror: corner and interior nodes floor their own projection; an
 * edge node sits on the segment between its edge's snapped corners at the
 * parameter of its own projection, unless an edge corner is inside the near
 * plane. */
/* nc4_snap_edge mirror: the point of the segment between the edge corners'
 * snapped projections at the parameter of the node's own projection, or the
 * floor of that projection when an edge corner is inside the near plane. */
vec2 edge_snap_screen(dvec3 ea, dvec3 eb, dvec3 m)
{
    if (ea.z < g.dnear.x || eb.z < g.dnear.x) return floor(proj_of(m));
    vec2 A = floor(proj_of(ea)), B = floor(proj_of(eb));
    vec2 mm = proj_of(m);
    precise float dx = B.x - A.x, dy = B.y - A.y;
    precise float L2 = dx * dx + dy * dy;
    precise float num = (mm.x - A.x) * dx + (mm.y - A.y) * dy;
    float tt = L2 > 0.0 ? num / L2 : 0.5;
    if (tt < 0.0) tt = 0.0; else if (tt > 1.0) tt = 1.0;
    precise float rx = A.x + tt * dx, ry = A.y + tt * dy;
    return vec2(rx, ry);
}

vec2 nc4_snap_screen(int r, int c, dvec3 TL, dvec3 TR, dvec3 BL, dvec3 BR, dvec3 m)
{
    if (pc.snap == 0) return proj_of(m);
    bool er = (r == 0 || r == 4), ec = (c == 0 || c == 4);
    if (er == ec) return floor(proj_of(m));
    dvec3 ea = er ? (r == 0 ? TL : BL) : (c == 0 ? TL : TR);
    dvec3 eb = er ? (r == 0 ? TR : BR) : (c == 0 ? BL : BR);
    return edge_snap_screen(ea, eb, m);
}

/* nc4_morph mirror: an odd-odd node moves toward the mean of its up-right
 * and down-left neighbours' emitted points, weight at = least corner SZ3 -
 * 0x40, skipped when at <= 0 or the node's own integer projection is off
 * the 512x256 GTE screen (nc4_outcode). Applied under AFFINE_UV only: the
 * morph (r_environment.s 0x80028D1C-0x80028E44) moves the screen point while
 * UV and w stay, which perspective-correct texturing renders as a warp. */
vec2 nc4_screen(int r, int c, dvec3 TL, dvec3 TR, dvec3 BL, dvec3 BR)
{
    dvec3 m = nc4_node(r, c, TL, TR, BL, BR);
    vec2 sc = nc4_snap_screen(r, c, TL, TR, BL, BR, m);
    if (!AFFINE_UV || (r & 1) == 0 || (c & 1) == 0) return sc;
    double mn = min(min(TL.z, TR.z), min(BL.z, BR.z));
    precise double at = mn * 4.0lf - 64.0lf;
    if (at <= 0.0lf || m.z <= 0.0lf) return sc;
    at = min(at, 256.0lf);             /* pinned far faces: full weight, never past it */
    vec2 op = floor(proj_of(m));
    if (op.y <= 0.0 || op.y >= 256.0 || op.x <= 0.0 || op.x >= 512.0) return sc;
    vec2 a = nc4_snap_screen(r - 1, c + 1, TL, TR, BL, BR, nc4_node(r - 1, c + 1, TL, TR, BL, BR));
    vec2 b = nc4_snap_screen(r + 1, c - 1, TL, TR, BL, BR, nc4_node(r + 1, c - 1, TL, TR, BL, BR));
    precise double mx = ((256.0lf - at) * double(sc.x) + at * 0.5lf * (double(a.x) + double(b.x))) / 256.0lf;
    precise double my = ((256.0lf - at) * double(sc.y) + at * 0.5lf * (double(a.y) + double(b.y))) / 256.0lf;
    vec2 o = vec2(float(mx), float(my));
    return pc.snap != 0 ? floor(o) : o;
}

/* ---- split triangles (terrain.c nt4_* / nt_* mirrors) --------------------
 * A tier-4 triangle (corners A = i0, B = i1, C = i2) draws as 16
 * sub-triangles over 15 nodes, node (r, c) at A(4-r-c)/4 + Bc/4 + Cr/4; a
 * tier-2 triangle as 4 sub-triangles over the grid's nodes 0, 2, 4, 9, 11
 * and 14 (the corners and edge midpoints). A sub-triangle vertex carries
 * bit 30 beside the cell bit 31 in mat_near1, its sub-triangle in bits 4-8
 * (16-19: tier 2) and its cell or quadrant in bits 0-3; its member slot is
 * the role. NT_SUB is D_8006D0F8's node ids (A, B, C per sub-triangle),
 * then D_8006D0E8's mapped onto the 15-node numbering (k_nt2_node). */
const int NT_SUB[60] = int[60](0, 1, 5,   6, 5, 1,   1, 2, 6,   7, 6, 2,
                               2, 3, 7,   8, 7, 3,   3, 4, 8,   5, 6, 9,
                               10, 9, 6,  6, 7, 10,  11, 10, 7, 7, 8, 11,
                               9, 10, 12, 13, 12, 10, 10, 11, 13, 12, 13, 14,
                               0, 2, 9,   11, 9, 2,  2, 4, 11,  9, 11, 14);
/* A node's face edge: 0 none, 1 AB, 2 CA, 3 BC (k_nt4_edge). */
const int NT4_EDGE[15] = int[15](0, 1, 1, 1, 0, 2, 0, 0, 3, 2, 0, 3, 2, 3, 0);

/* nt4_grid mirror: nodes by the drain's halving order, each the camera-space
 * mean of its two parents. */
precise dvec3 nt4_node(int n, dvec3 A, dvec3 B, dvec3 C) {
    if (n == 0) return A;
    if (n == 4) return B;
    if (n == 14) return C;
    precise dvec3 m2 = (A + B) * 0.5lf, m9 = (A + C) * 0.5lf, m11 = (B + C) * 0.5lf;
    if (n == 2) return m2;
    if (n == 9) return m9;
    if (n == 11) return m11;
    precise dvec3 r;
    if      (n == 1)  r = (A + m2) * 0.5lf;
    else if (n == 3)  r = (B + m2) * 0.5lf;
    else if (n == 5)  r = (A + m9) * 0.5lf;
    else if (n == 12) r = (C + m9) * 0.5lf;
    else if (n == 8)  r = (B + m11) * 0.5lf;
    else if (n == 13) r = (C + m11) * 0.5lf;
    else if (n == 6)  r = (A + m11) * 0.5lf;
    else if (n == 7)  r = (B + m9) * 0.5lf;
    else              r = (C + m2) * 0.5lf;   /* node 10 */
    return r;
}

/* nt4_snap mirror: a corner or interior node floors its own projection, an
 * edge node sits on its edge's snapped segment. */
vec2 nt4_screen(int n, dvec3 A, dvec3 B, dvec3 C) {
    dvec3 m = nt4_node(n, A, B, C);
    if (pc.snap == 0) return proj_of(m);
    int e = NT4_EDGE[n];
    if (e == 0) return floor(proj_of(m));
    return edge_snap_screen(e == 3 ? B : A, e == 1 ? B : C, m);
}

/* ---- drain sub-cell keys (terrain.c tn_drain_sz, tn_drain_key4 and
 * tn_drain_key3 mirrors) ----------------------------------------------------
 * A drain node's term: its SZ3 (the GTE saturates floor(hqscale * m3) to
 * [0, 0xFFFF]) masked to the 12 bits the node word keeps, for a face in the
 * near band (`drained`); outside it, the unmasked hqscale * m3. A quad cell
 * sums its four nodes, a sub-triangle SZ A + SZ B + 2 SZ C. */
double drain_sz(double m3, bool drained) {
    if (!drained) return m3 * 4.0lf;
    double sz = floor(m3 * 4.0lf);
    if (sz <= 0.0lf) return 0.0lf;
    if (sz > 65535.0lf) sz = 65535.0lf;
    return double(int(sz) & 0xFFF);
}
float drain_key4(double a, double b, double c, double d, bool drained) {
    precise double s = drain_sz(a, drained) + drain_sz(b, drained);
    s = s + drain_sz(c, drained);
    s = s + drain_sz(d, drained);
    return float(s);
}
float drain_key3(double a, double b, double c, bool drained) {
    precise double s = drain_sz(a, drained) + drain_sz(b, drained);
    s = s + 2.0lf * drain_sz(c, drained);
    return float(s);
}

double det3d(dvec3 a, dvec3 b, dvec3 c);

/* Node n's place (r, c) in the 15-node triangle grid (terrain.c k_nt4_rc). */
const int NT4_RC[30] = int[30](0, 0,  0, 1,  0, 2,  0, 3,  0, 4,  1, 0,  1, 1,  1, 2,
                               1, 3,  2, 0,  2, 1,  2, 2,  3, 0,  3, 1,  4, 0);

/* The drain key table (psx_key_contest.glsl): a face's sub-cell keys packed
 * as their excess over the least one, by grid position. */
struct dtab { uvec4 t0; uvec4 t1; float kmin; int n; };

void dtab_put(inout dtab T, int slot, uint v) {
    uint sh = uint(slot & 1) * 16u;
    int w = (slot >> 1) & 3;
    if (slot < 8) T.t0[w] |= v << sh; else T.t1[w] |= v << sh;
}

/* tn_sub_slot mirror: a sub-triangle's table entry from its nodes' grid
 * places, h the grid step (1 tier 4, 2 tier 2). An upward sub-triangle has
 * two nodes on its top row, a downward one a single node. */
int drain_sub_slot(int a, int b, int c, int h) {
    int ra = NT4_RC[a * 2], rb = NT4_RC[b * 2], rc = NT4_RC[c * 2];
    int rmin = min(ra, min(rb, rc));
    int cmin = min(NT4_RC[a * 2 + 1], min(NT4_RC[b * 2 + 1], NT4_RC[c * 2 + 1]));
    int top = int(ra == rmin) + int(rb == rmin) + int(rc == rmin);
    int i = cmin / h, j = rmin / h;
    return top == 2 ? j * 4 + i : (3 - j) * 4 + (3 - i);
}

/* Pack keys[] (on[] the sub-cells the drain inserts, slot[] their entries)
 * over their least key, clamped to 0x3FFE, a skipped sub-cell as the
 * sentinel 0x3FFF (OP_DRAIN_SKIP); grid in entry 0's top bits (terrain.c
 * tn_keycells_pack). */
void dtab_pack(inout dtab T, float keys[16], bool on[16], int slot[16], int cnt, uint grid) {
    T.t0 = uvec4(0u); T.t1 = uvec4(0u);
    if (T.n == 0) return;
    for (int i = 0; i < cnt; i++)
        dtab_put(T, slot[i], on[i] ? min(uint(keys[i] - T.kmin), 0x3FFEu) : 0x3FFFu);
    T.t0.x |= grid << 14;
}

uint quad_fold_mask(dvec3 G[25], bool on[16]);

/* tn_quad_cell_keys mirror: the keys of the 16 cells the drain inserts a
 * tier-4 quad as, over the grid of corners TL, TR, BL, BR; a cell whose key
 * is 0 carries none, nor, when `cull` (a straddling single-sided face), one
 * facing away over both of its triples (nc4_front). A folded quadrant q
 * (quad_fold_mask) sets bit 14 of entry 2 + q (tn_keycells_fold). */
dtab quad_cells_tab(dvec3 TL, dvec3 TR, dvec3 BL, dvec3 BR, bool cull, bool flip)
{
    dvec3 G[25];
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 5; c++) G[r * 5 + c] = nc4_node(r, c, TL, TR, BL, BR);
    dtab T;
    T.kmin = 0.0; T.n = 0;
    float keys[16]; bool on[16]; int slot[16];
    for (int i = 0; i < 16; i++) {
        int r = i >> 2, c = i & 3;
        dvec3 tl = G[r * 5 + c], tr = G[r * 5 + c + 1];
        dvec3 bl = G[r * 5 + c + 5], br = G[r * 5 + c + 6];
        float key = drain_key4(tl.z, tr.z, bl.z, br.z, true);
        bool f = true;
        if (cull) {
            precise double d1 = det3d(tl, tr, bl);
            f = flip ? (d1 <= 0.0lf) : (d1 >= 0.0lf);
            if (!f) {
                precise double d2 = det3d(tr, br, bl);
                f = flip ? (d2 <= 0.0lf) : (d2 >= 0.0lf);
            }
        }
        keys[i] = key; slot[i] = i;
        on[i] = f && key > 0.0;
        if (!on[i]) continue;
        if (T.n == 0 || key < T.kmin) T.kmin = key;
        T.n++;
    }
    dtab_pack(T, keys, on, slot, 16, 1u);
    if (T.n > 0) {
        uint fm = quad_fold_mask(G, on);
        for (int q = 0; q < 4; q++)
            if ((fm & (1u << q)) != 0u) dtab_put(T, 2 + q, 0x4000u);
    }
    return T;
}

/* tn_tri_cell_keys mirror: the same over a triangle's tier-4 (16) or tier-2
 * (4) sub-triangles, a sub-triangle facing away over its own triple
 * skipped when `cull`. */
dtab tri_cells_tab(dvec3 A, dvec3 B, dvec3 C, bool tier2, bool cull, bool flip)
{
    dvec3 N[15];
    for (int k = 0; k < 15; k++) N[k] = nt4_node(k, A, B, C);
    int base = tier2 ? 16 : 0, cnt = tier2 ? 4 : 16;
    dtab T;
    T.kmin = 0.0; T.n = 0;
    float keys[16]; bool on[16]; int slot[16];
    for (int i = 0; i < 16; i++) { keys[i] = 0.0; on[i] = false; slot[i] = 0; }
    for (int i = 0; i < cnt; i++) {
        int t = (base + i) * 3;
        dvec3 a = N[NT_SUB[t]], b = N[NT_SUB[t + 1]], c = N[NT_SUB[t + 2]];
        float key = drain_key3(a.z, b.z, c.z, true);
        bool f = true;
        if (cull) {
            precise double d = det3d(a, b, c);
            f = !(flip ? (d > 0.0lf) : (d < 0.0lf));
        }
        keys[i] = key;
        slot[i] = drain_sub_slot(NT_SUB[t], NT_SUB[t + 1], NT_SUB[t + 2], tier2 ? 2 : 1);
        on[i] = f && key > 0.0;
        if (!on[i]) continue;
        if (T.n == 0 || key < T.kmin) T.kmin = key;
        T.n++;
    }
    dtab_pack(T, keys, on, slot, cnt, tier2 ? 3u : 2u);
    return T;
}

/* The drain coordinate (psx_key_contest.glsl) of a 15-node triangle grid
 * node: its face-local place in quarter-edge units. */
vec2 nt4_fl(int n) { return vec2(float(NT4_RC[n * 2 + 1]), float(NT4_RC[n * 2])); }

/* terrain.c tn_quad_sides mirror: point V's side coordinates of the 2x2
 * quadrant with corners Q0..Q3 (TL, TR, BL, BR), term by term as the walk
 * computes them: the eye-plane normals of its column and row isolines,
 * dotted with V and scaled to 1 at TR and BL. */
dvec3 eye_normal(dvec3 a, dvec3 b) {
    precise double x = a.y * b.z - b.y * a.z;
    precise double y = a.z * b.x - b.z * a.x;
    precise double z = a.x * b.y - b.x * a.y;
    return dvec3(x, y, z);
}
double eye_side(dvec3 n, dvec3 v) {
    precise double d = n.x * v.x + n.y * v.y;
    d = d + n.z * v.z;
    return d;
}
vec2 quad_side_fl(dvec3 Q0, dvec3 Q1, dvec3 Q2, dvec3 Q3, dvec3 V) {
    precise dvec3 ct = (Q0 + Q1) * 0.5lf, cb = (Q2 + Q3) * 0.5lf;
    precise dvec3 rl = (Q0 + Q2) * 0.5lf, rr = (Q1 + Q3) * 0.5lf;
    dvec3 nc = eye_normal(ct, cb), nr = eye_normal(rl, rr);
    double sc = eye_side(nc, Q1), sr = eye_side(nr, Q2);
    if (sc == 0.0lf) sc = 1.0lf;
    if (sr == 0.0lf) sr = 1.0lf;
    return vec2(float(eye_side(nc, V) / sc), float(eye_side(nr, V) / sr));
}

/* terrain.c tn_quad_folds mirror, term by term: bit q set when a drawn cell
 * of 2x2 quadrant q has a node strictly (1e-6 of the scale) across one of
 * the quadrant's isoline eye planes into a skipped cell's slot. G is the
 * 5x5 node grid, on[] the drawn cells. */
uint quad_fold_mask(dvec3 G[25], bool on[16]) {
    uint fm = 0u;
    for (int qd = 0; qd < 4; qd++) {
        int r0 = (qd >> 1) * 2, c0 = (qd & 1) * 2;
        dvec3 Q0 = G[r0 * 5 + c0], Q1 = G[r0 * 5 + c0 + 2];
        dvec3 Q2 = G[(r0 + 2) * 5 + c0], Q3 = G[(r0 + 2) * 5 + c0 + 2];
        precise dvec3 ct = (Q0 + Q1) * 0.5lf, cb = (Q2 + Q3) * 0.5lf;
        precise dvec3 rl = (Q0 + Q2) * 0.5lf, rr = (Q1 + Q3) * 0.5lf;
        dvec3 nc = eye_normal(ct, cb), nr = eye_normal(rl, rr);
        double sc = eye_side(nc, Q1), sr = eye_side(nr, Q2);
        if (sc == 0.0lf) sc = 1.0lf;
        if (sr == 0.0lf) sr = 1.0lf;
        bool f = false;
        for (int k = 0; k < 4 && !f; k++) {
            int lr = k >> 1, lc = k & 1;
            if (!on[(r0 + lr) * 4 + c0 + lc]) continue;
            for (int n = 0; n < 4 && !f; n++) {
                dvec3 v = G[(r0 + lr + (n >> 1)) * 5 + c0 + lc + (n & 1)];
                double fx = eye_side(nc, v) / sc, fy = eye_side(nr, v) / sr;
                int xc = (lc != 0 ? fx < -1e-6lf : fx > 1e-6lf) ? 1 : 0;
                int xr = (lr != 0 ? fy < -1e-6lf : fy > 1e-6lf) ? 1 : 0;
                if ((xc | xr) != 0 && !on[(r0 + (lr ^ xr)) * 4 + c0 + (lc ^ xc)]) f = true;
            }
        }
        if (f) fm |= 1u << qd;
    }
    return fm;
}

/* A camera-space point in the homogeneous screen space of the fold test
 * (psx_key_contest.glsl): (OFX z + H x, OFY z + H y, z), terrain.c
 * tn_fold_corner's terms. */
vec3 fold_h(dvec3 m) {
    precise double hx = g.dof_xy.x * m.z + g.dH.x * m.x;
    precise double hy = g.dof_xy.y * m.z + g.dH.x * m.y;
    return vec3(float(hx), float(hy), float(m.z));
}

/* GTE_SAT mirror (double of the FLOAT screen coord, like the CPU's
 * NSAT(sv.sx)). */
double nsat(float v, bool unr_ovf) {
    if (unr_ovf) return double(v);
    return v < -1024.0 ? -1024.0lf : (v > 1023.0 ? 1023.0lf : double(v));
}

/* terrain.c tface_gte_flagged mirror, one corner: the .s vertex flag 0x10
 * (GTE FLAG bit 31 read for SZ3 < 0x600): SZ3 saturated at 0, SX2/SY2
 * saturated, or the UNR divide overflow (m3 < H_canon / 8). */
bool gte_flag(double mz, vec2 p) {
    if (mz <= 0.0lf) return true;
    if (mz >= 384.0lf) return false;
    if (p.x < -1024.0 || p.x > 1023.0 || p.y < -1024.0 || p.y > 1023.0) return true;
    return mz < g.dspare.x / 8.0lf;
}

/* ngte_dpcs_ch mirror (GTE-exact DPCS fog, one channel; pure int math). */
int dpcs_ch(int c8, int fc12, int ir0) {
    int mac = c8 << 16;
    int ird = clamp(((fc12 << 12) - mac) >> 12, -32768, 32767);
    int o = ((mac + ir0 * ird) >> 12) >> 4;
    return clamp(o, 0, 255);
}

/* One corner's terrain colour, the walk's terrain_color + lp_grade_apply:
 * flat HP corners fog on their OWN view-Z (GTE-exact DPCS), LP/HQ take the
 * baked raw colour, and an active replacement pack regrades LP. This is the
 * colour the feed resolves per corner BEFORE any near-plane clip, so the
 * clip mirror interpolates its output rather than re-fogging a cut vertex. */
#if defined(TERRAIN_ANIM) && !defined(TERRAIN_CELLS)
/* Env-anim colour lerp (canim_lerp mirror): one colour word at the build's
 * colour alpha of the interval prev -> cur, per channel, rounded to nearest
 * onto the 8-bit grid before the fog blend. Alpha 1 selects cur. */
vec3 canim_col(uint prev, vec3 cur)
{
    double a = g.dspare.y;
    if (a == 1.0lf) return cur;
    ivec3 c = ivec3(round(cur * 255.0));
    ivec3 p = ivec3(int(prev & 0xFFu), int((prev >> 8) & 0xFFu), int((prev >> 16) & 0xFFu));
    precise dvec3 v = dvec3(p) + dvec3(c - p) * a + 0.5lf;
    return vec3(ivec3(v)) / 255.0;
}
#endif

vec3 terrain_col(double mz, vec3 cfar, vec3 cnear, bool doFog, int fogRef,
                 double fogscale, bool grade, vec4 gp, vec4 gq)
{
    vec3 col;
    if (doFog) {
        int ir0 = fogRef - int(mz * fogscale);
        if (ir0 >= 4096) {
            col = cnear;
        } else if (ir0 <= 0) {
            col = cfar;
        } else {
            ivec3 f8 = ivec3(round(cfar  * 255.0));
            ivec3 n8 = ivec3(round(cnear * 255.0));
            col = vec3(float(dpcs_ch(f8.r, n8.r << 4, ir0)) / 255.0,
                       float(dpcs_ch(f8.g, n8.g << 4, ir0)) / 255.0,
                       float(dpcs_ch(f8.b, n8.b << 4, ir0)) / 255.0);
        }
    } else {
        col = cfar;   /* LP / HQ: raw baked, never fogged */
    }
    /* Pack-aware LP colour grade (lp_grade_apply mirror): the bake carries
     * raw authored colours; with a replacement pack active the sector's
     * grade affine (own HP-texture fit, or the level mean) rides the sector
     * params. Same float32 ops and trunc-after-+0.5 as the walk; WORLD LP
     * and fill LP alike, never HQ (the walk grades passLP only). */
    if (grade) {
        for (int c = 0; c < 3; c++) {
            precise float t = gp[c] * round(col[c] * 255.0);
            t = t + gq[c] * 255.0;
            t = t + 0.5;
            col[c] = float(clamp(int(t), 0, 255)) / 255.0;
        }
    }
    return col;
}

/* face_is_near_canonical mirror (hqscale 4.0; per-corner saturation at 0).
 * Used both for the HQ variant select (via face_is_near = this, or the
 * NOLOD short-circuit) and for the koff near-band/otslots-zero verdict
 * (ALWAYS this canonical form, even under NOLOD). */
/* Camera-space triple product of three terrain corners (the walk's
 * tface_det3): the sign of the projected NCLIP over the same triple, and
 * still defined for a corner behind the eye. */
double det3d(dvec3 a, dvec3 b, dvec3 c) {
    return a.x * (b.y * c.z - b.z * c.y)
         - a.y * (b.x * c.z - b.z * c.x)
         + a.z * (b.x * c.y - b.y * c.x);
}

bool near_canonical(double z0, double z1, double z2, double z3, uint w3_40, uint w3_80) {
    double sz0 = z0 > 0.0lf ? z0 * 4.0lf : 0.0lf, sz1 = z1 > 0.0lf ? z1 * 4.0lf : 0.0lf;
    double sz2 = z2 > 0.0lf ? z2 * 4.0lf : 0.0lf, sz3 = z3 > 0.0lf ? z3 * 4.0lf : 0.0lf;
    double sum = sz0 + sz1 + sz2 + sz3;
    if (sum <= 0.0lf) return false;                 /* fully behind camera */
    if (w3_80 != 0u) return false;                  /* hard-excluded from HQ */
    if (sum >= 8192.0lf) return false;              /* not foreground (0x2000) */
    double mn = min(min(sz0, sz1), min(sz2, sz3));
    if (mn < 320.0lf) return true;                  /* a corner SZ3 < 0x140 */
    return w3_40 == 0u;
}

/* face_near_tier4 mirror: a canonical near face with near_flag_tier2 clear
 * and a corner SZ3 under near_corner (0x140). */
bool nc4_tier4(double z0, double z1, double z2, double z3, uint w3_40, uint w3_80) {
    if (w3_40 != 0u) return false;
    if (!near_canonical(z0, z1, z2, z3, w3_40, w3_80)) return false;
    return min(min(z0, z1), min(z2, z3)) * 4.0lf < 320.0lf;
}

/** @brief Returns the LP insert's constant base, in 128-world slots. */
uint ot_lp_base() { return uint(g.fog.w) & 0xFFu; }

/** @brief Returns the HP insert's multiplier on the face's bias field. */
uint ot_hp_mul() { return (uint(g.fog.w) >> 12) & 0xFu; }

/** @brief Returns whether the LP far cull tests the whole LP bucket index. */
bool ot_farcull_keyed() { return (uint(g.fog.w) & 0x10000u) != 0u; }

/** @brief Returns an LP face's authored slot offset: the base plus its biased slots.
 *
 * Mirrors tface_ot_slots for an LP face.
 * @param ob The face's bias field.
 * @return The offset in 128-world slots. */
double ot_lp_slots(uint ob) {
    return double(ot_lp_base() + ((uint(g.fog.w) >> 8) & 0xFu) * ob);
}

/* The face-attribution identity of debug tint mode 7, 22 bits: the decode
 * slot (bits 0-8), LP tier (9), variant (10-11: 0 flat, 1 HQ 2x2 quadrant,
 * 2 4x4 cell), drain sub-cell keys (12), near-variant chunk (13), and an
 * 8-bit FNV-1a fold of the twelve parent-corner coordinates (14-21). The
 * fold is the one OPENPETE_TERRAIN_GPU_FACEDUMP writes per baked face
 * (terrain.c tgpu_corner_hash8), so a decoder names the face by slot, tier
 * and fold. */
uint attr_word(uint slot, bool lp, bool hqv, bool is4, bool drain)
{
    const int cc[12] = int[12](inC0.x, inC0.y, inC0.z, inC0.w, inC1.x, inC1.y,
                               inC1.z, inC1.w, inC2.x, inC2.y, inC2.z, inC2.w);
    uint h = 2166136261u;
    for (int i = 0; i < 12; i++) h = (h ^ (uint(cc[i]) & 0xFFFFu)) * 16777619u;
    h = (h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24)) & 0xFFu;
    uint var = is4 ? 2u : (hqv ? 1u : 0u);
#ifdef TERRAIN_NEAR
    const uint nearv = 1u;
#else
    const uint nearv = 0u;
#endif
    return (slot & 0x1FFu) | (lp ? 1u << 9 : 0u) | (var << 10) |
           (drain ? 1u << 12 : 0u) | (nearv << 13) | (h << 14);
}

void kill_vert() {
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);   /* degenerate: no raster */
    vUV = vec2(0.0); vColor = vec4(0.0); vMat = uvec4(0u);
    vKoff = 0.0; vKcls = OP_KEY_CLASS_NONE; vQ = 1.0;
    vKfl = vec2(0.0); vKq = 1.0; vKc0 = uvec4(0u); vKc1 = uvec4(0u);
    vKd = vec3(0.0); vKn0 = vec4(0.0); vKn1 = vec4(0.0); vKn2 = vec4(0.0);
    vRim = 0u;
}

/* ---- portal inner-rim cull (portal_rim_classify mirror) --------------------
 * The CPU feed classifies every emitted tri against the active portal
 * opening planes and DROPS accepted rim faces - they lie in the opening
 * plane, coincident with the destination-sky preview, and no depth ordering
 * can separate them. Same classifier here, in fp64 `precise`, on the tri
 * rebuilt from the corner attributes (flags16 bit13 = second half/sub-tri).
 * rim_uv_contains / rim_uv_edge_dist run on the plane's outline dropped
 * into its in-plane (U,W) basis, exactly as the CPU helpers. */
bool rim_uv_contains_g(int base, int pn, double u, double v) {
    bool inp = false;
    for (int i = 0, j = pn - 1; i < pn; j = i++) {
        double iu = g.rim_pv[base + i].x, iv = g.rim_pv[base + i].y;
        double ju = g.rim_pv[base + j].x, jv = g.rim_pv[base + j].y;
        if ((iv > v) != (jv > v) &&
            u < (ju - iu) * (v - iv) / (jv - iv) + iu)
            inp = !inp;
    }
    return inp;
}
double rim_uv_edge_dist_g(int base, int pn, double u, double v) {
    double best = 1e30lf;
    for (int i = 0, j = pn - 1; i < pn; j = i++) {
        double ax = g.rim_pv[base + j].x, ay = g.rim_pv[base + j].y;
        double ex = g.rim_pv[base + i].x - ax, ey = g.rim_pv[base + i].y - ay;
        double L2 = ex * ex + ey * ey;
        double t = (L2 > 1e-9lf) ? ((u - ax) * ex + (v - ay) * ey) / L2 : 0.0lf;
        if (t < 0.0lf) t = 0.0lf; else if (t > 1.0lf) t = 1.0lf;
        double dx = u - (ax + t * ex), dy = v - (ay + t * ey);
        double d = sqrt(dx * dx + dy * dy);
        if (d < best) best = d;
    }
    return best;
}
/* The inner-rim verdict of a tri (classify flags == 0 under some plane):
 * 0 = not rim; 1 = rim under a plane the CPU feed drops it for, so the
 * retained draw drops it too; 2 + pi = rim under plane pi, which discards its
 * rim per fragment (rim_O[pi].w), so the face is kept and terrain.frag
 * discards the part inside plane pi's outline (vRim = 1 + pi; the rim and
 * cover plane lists share the feed's plane order). Inputs are the emitted
 * tri's FLOAT screen coords (the feed ttri's sx/sy) and walk view-Z (m3). */
int rim_classified(vec2 s0, vec2 s1, vec2 s2, double z0, double z1, double z2) {
    precise double ofx = g.rim_oxyH.x, ofy = g.rim_oxyH.y, H = g.rim_oxyH.z;
    if (H < 1.0lf) return 0;
    double eps = g.rim_knobs.x, nearw = g.rim_knobs.y;
    double reach = g.rim_knobs.z, cosw = g.rim_knobs.w;
    double sx[3] = double[3](double(s0.x), double(s1.x), double(s2.x));
    double sy[3] = double[3](double(s0.y), double(s1.y), double(s2.y));
    double vz[3] = double[3](z0 * 16.0lf, z1 * 16.0lf, z2 * 16.0lf);
    for (int pi = 0; pi < g.rim_cnt.x; pi++) {
        if (g.rim_abc[pi].w == 0.0lf) continue;      /* frame invalid */
        precise double nx = g.rim_nrm[pi].x, ny = g.rim_nrm[pi].y,
                       nz = g.rim_nrm[pi].z;
        precise dvec3 V[3];
        double dmin = 1e30lf, dmax = 0.0lf;
        bool okz = true;
        for (int k = 0; k < 3; k++) {
            precise double inv = g.rim_abc[pi].x * sx[k]
                               + g.rim_abc[pi].y * sy[k] + g.rim_abc[pi].z;
            if (inv <= 1e-9lf) { okz = false; break; }
            precise double vzk = vz[k];
            V[k] = dvec3((sx[k] - ofx) * vzk / H, (sy[k] - ofy) * vzk / H, vzk);
            precise double dz = vzk - 1.0lf / inv;
            precise double d = abs(dz * (nx * (sx[k] - ofx) + ny * (sy[k] - ofy)
                                         + nz * H) / H);
            if (d < dmin) dmin = d;
            if (d > dmax) dmax = d;
        }
        if (!okz) continue;
        if (dmin > 400.0lf) continue;                /* RIM_CAND_WINDOW */
        int base = pi * 16, pn = int(g.rim_nrm[pi].w);
        precise dvec3 O = g.rim_O[pi].xyz, U = g.rim_U[pi].xyz,
                      W = g.rim_W[pi].xyz;
        double fu[3], fv[3];
        double fulo = 1e30lf, fuhi = -1e30lf, fvlo = 1e30lf, fvhi = -1e30lf;
        for (int k = 0; k < 3; k++) {
            precise dvec3 r = V[k] - O;
            fu[k] = r.x * U.x + r.y * U.y + r.z * U.z;
            fv[k] = r.x * W.x + r.y * W.y + r.z * W.z;
            if (fu[k] < fulo) fulo = fu[k];
            if (fu[k] > fuhi) fuhi = fu[k];
            if (fv[k] < fvlo) fvlo = fv[k];
            if (fv[k] > fvhi) fvhi = fv[k];
        }
        if (fuhi < g.rim_bbox[pi].x - 600.0lf ||     /* RIM_CAND_MARGIN */
            fulo > g.rim_bbox[pi].y + 600.0lf ||
            fvhi < g.rim_bbox[pi].z - 600.0lf ||
            fvlo > g.rim_bbox[pi].w + 600.0lf) continue;
        /* contain: any vertex or the centroid inside the outline, or a
         * vertex within `reach` of an outline edge */
        bool touches = false;
        double cu = 0.0lf, cv = 0.0lf;
        for (int k = 0; k < 3; k++) {
            cu += fu[k]; cv += fv[k];
            if (!touches && rim_uv_contains_g(base, pn, fu[k], fv[k]))
                touches = true;
        }
        if (!touches && rim_uv_contains_g(base, pn, cu / 3.0lf, cv / 3.0lf))
            touches = true;
        if (!touches && reach > 0.0lf)
            for (int k = 0; k < 3 && !touches; k++)
                if (rim_uv_edge_dist_g(base, pn, fu[k], fv[k]) <= reach)
                    touches = true;
        if (!touches) continue;                      /* RIMFAIL_CONTAIN */
        if (dmax > eps || dmin > nearw) continue;    /* RIMFAIL_DIST */
        precise dvec3 e1 = V[1] - V[0], e2 = V[2] - V[0];
        precise double fx = e1.y * e2.z - e1.z * e2.y;
        precise double fy = e1.z * e2.x - e1.x * e2.z;
        precise double fz = e1.x * e2.y - e1.y * e2.x;
        double fl = sqrt(fx * fx + fy * fy + fz * fz);
        if (fl < 1e-9lf) continue;                   /* RIMFAIL_ANGLE */
        if (abs((fx * nx + fy * ny + fz * nz) / fl) < cosw) continue;
        return g.rim_O[pi].w != 0.0lf ? 2 + pi : 1;  /* all terms pass */
    }
    return 0;
}

#ifdef TERRAIN_NEAR
/* ---- near-plane clip fan (clip_push_tri / cv_lerp mirror) -----------------
 * A near-variant sector's faces may reach or cross the eye plane, so the walk
 * Sutherland-Hodgman-clips every emitted triangle against m3 >= zc before
 * projecting it: 0 crossings -> 1 triangle, 1 -> a quad drawn as 2, 2 -> 1,
 * 3 -> dropped. The bake emits SIX vertices per source triangle (a primary
 * and a spare fan triangle, flags16 bit 15), each of which re-runs the whole
 * clip here and becomes one corner of its fan triangle; the spare dies
 * whenever the clip produced fewer than 4 output vertices.
 *
 * A clip corner carries the position, colour and UV the walk resolved BEFORE
 * the clip - fogging and the LP grade run per parent corner, and the cut
 * interpolates their OUTPUT, never re-fogs at the cut position. */
/* os/ov mirror ngte_cvert.osx/osy/oflag: the screen-space override the HQ
 * subdivide installs on its own nodes under the integer snap. A cut vertex
 * rebuilt by the clip carries none (ngte_clip_lerp zeroes oflag) and falls
 * back to the floor of its own projection, as push_tri does. */
struct cvx { dvec3 m; ivec3 c; vec2 uv; vec2 os; bool ov; vec2 fl; };

/* Unpack a baked R8G8B8A8 colour word the way the vertex-input UNORM
 * conversion unpacks the same bytes at locations 2/3. */
vec3 unrgb(uint u) {
    return vec3(float(u & 0xFFu), float((u >> 8) & 0xFFu),
                float((u >> 16) & 0xFFu)) / 255.0;
}

cvx cv_lerp_n(cvx a, cvx b, double zc) {
    precise double t = (zc - a.m.z) / (b.m.z - a.m.z);
    if (t < 0.0lf) t = 0.0lf; else if (t > 1.0lf) t = 1.0lf;
    precise double px = a.m.x + t * (b.m.x - a.m.x);
    precise double py = a.m.y + t * (b.m.y - a.m.y);
    precise float  pu = a.uv.x + float(t) * (b.uv.x - a.uv.x);
    precise float  pv = a.uv.y + float(t) * (b.uv.y - a.uv.y);
    precise float  fx = a.fl.x + float(t) * (b.fl.x - a.fl.x);
    precise float  fy = a.fl.y + float(t) * (b.fl.y - a.fl.y);
    precise double cr = double(a.c.x) + t * double(b.c.x - a.c.x) + 0.5lf;
    precise double cg = double(a.c.y) + t * double(b.c.y - a.c.y) + 0.5lf;
    precise double cb = double(a.c.z) + t * double(b.c.z - a.c.z) + 0.5lf;
    cvx r;
    r.m  = dvec3(px, py, zc);
    r.uv = vec2(pu, pv);
    r.c  = ivec3(int(cr), int(cg), int(cb));
    r.os = vec2(0.0); r.ov = false;
    r.fl = vec2(fx, fy);
    return r;
}

/* push_tri's screen resolution for one clip-fan corner: the installed
 * override when the subdivide left one, otherwise the (snapped) projection. */
vec2 scr_of(cvx v) { return v.ov ? v.os : snap_of(proj_of(v.m)); }

/* Max distance (PSX px) a crossing vertex may project from the screen centre
 * before the whole face is re-clipped at a doubled zc - the walk's own
 * CLIP_PROJ_LIMIT (crossing projections shrink ~1/zc, so this terminates
 * fast, and everything it trims lies far outside any visible frame). */
#define CLIP_PROJ_LIMIT 2048.0
#endif

#ifdef TERRAIN_CELLS
/* The two sign-extended int16 halves of a packed corner word, low first. */
ivec2 cell_s16x2(uint w) {
    return ivec2(bitfieldExtract(int(w), 0, 16), bitfieldExtract(int(w), 16, 16));
}
/* Unpack the record's corner words into inC0..inC2 (rhi_terrain_vertex's
 * c0x..c3z order, four to a vector), the face flags and info, and the fade
 * words. */
void cell_unpack() {
    inC0 = ivec4(cell_s16x2(inCellCA.x), cell_s16x2(inCellCA.y));
    inC1 = ivec4(cell_s16x2(inCellCA.z), cell_s16x2(inCellCA.w));
    inC2 = ivec4(cell_s16x2(inCellCB.x), cell_s16x2(inCellCB.y));
    inCellFace = inCellCB.zw;
    cellHqFog = inCellFog;
#ifdef TERRAIN_ANIM
    inPrevC0 = ivec4(cell_s16x2(inCellPA.x), cell_s16x2(inCellPA.y));
    inPrevC1 = ivec4(cell_s16x2(inCellPA.z), cell_s16x2(inCellPA.w));
    inPrevC2 = ivec4(cell_s16x2(inCellPB.x), cell_s16x2(inCellPB.y));
#endif
}
/* tgpu_rgb_mid mirror: the walk's per-channel (a + b) >> 1 on packed RGB. */
uint cell_rgb_mid(uint a, uint b) {
    return (((a & 0xFFu) + (b & 0xFFu)) >> 1)
         | ((((a >> 8) & 0xFFu) + ((b >> 8) & 0xFFu)) >> 1) << 8
         | ((((a >> 16) & 0xFFu) + ((b >> 16) & 0xFFu)) >> 1) << 16;
}
/* tgpu_rgb_pt mirror: node k (0..4) of the 5-node edge a..b in the drain's
 * halving order (midpoint first, then the quarter points off it). */
uint cell_rgb_pt(uint a, uint b, int k) {
    if (k == 0) return a;
    if (k == 4) return b;
    uint m = cell_rgb_mid(a, b);
    return k == 2 ? m : cell_rgb_mid(k == 1 ? a : b, m);
}
/* Node (r, c) colour of the 5x5 grid from four raw corner colours
 * (i0 = TL, i1 = TR, i2 = BR, i3 = BL): rows first, then along the row. */
uint cell_node_rgb(int r, int c, uvec4 q) {
    uint L = cell_rgb_pt(q.x, q.w, r);
    uint R = cell_rgb_pt(q.y, q.z, r);
    return cell_rgb_pt(L, R, c);
}
/* nt4_rgb_mid mirror: the triangle drain's colour average, both words
 * masked with 0xFFFEFEFF, added, shifted right once. */
uint nt4_rgb_mid(uint a, uint b) {
    return ((a & 0xFEFEFFu) + (b & 0xFEFEFFu)) >> 1;
}
/* Node n's colour of a tier-4 triangle record from corner colours q (near
 * or far): A = q.x, B = q.y, C = q.w (word 1 byte 0), nodes in nt4_node's
 * parent order. */
uint nt4_node_rgb(int n, uvec4 q) {
    uint A = q.x, B = q.y, C = q.w;
    if (n == 0) return A;
    if (n == 4) return B;
    if (n == 14) return C;
    uint m2 = nt4_rgb_mid(A, B), m9 = nt4_rgb_mid(A, C), m11 = nt4_rgb_mid(B, C);
    if (n == 2) return m2;
    if (n == 9) return m9;
    if (n == 11) return m11;
    if (n == 1)  return nt4_rgb_mid(A, m2);
    if (n == 3)  return nt4_rgb_mid(B, m2);
    if (n == 5)  return nt4_rgb_mid(A, m9);
    if (n == 12) return nt4_rgb_mid(C, m9);
    if (n == 8)  return nt4_rgb_mid(B, m11);
    if (n == 13) return nt4_rgb_mid(C, m11);
    if (n == 6)  return nt4_rgb_mid(A, m11);
    if (n == 7)  return nt4_rgb_mid(B, m9);
    return nt4_rgb_mid(C, m2);                  /* node 10 */
}
vec4 cell_unorm(uint rgb) {
    return vec4(float(rgb & 0xFFu), float((rgb >> 8) & 0xFFu),
                float((rgb >> 16) & 0xFFu), 255.0) / 255.0;
}
/* Corner q's texel (u, v) of a tile: TL,TR in x, BL,BR in y, a byte each. */
uvec2 cell_uv(uvec4 t, int q) {
    uint w = (q < 2) ? t.x : t.y;
    uint sh = uint(q & 1) * 16u;
    return uvec2((w >> sh) & 0xFFu, (w >> (sh + 8u)) & 0xFFu);
}
/* Fill the per-vertex names main() reads with what the baked cell vertex at
 * this index carried: an instance draws 8 cells x 2 triangles (TL,TR,BL) +
 * (TR,BL,BR) in cell order, each triangle 6 vertices on the near variant
 * (primary fan triangle, then the flags16-bit-15 spare) or 3 on the anim
 * variant; corner slot = triangle member + the second triangle's offset.
 * Returns false for a cell this record copy does not draw (another draw
 * class's copy carries it). */
/* A split-triangle record (info bit 24): entry lc of half h is
 * sub-triangle h*8 + lc of a tier-4 record, 16 + lc of a tier-2 one (bit
 * 25), drawn by the entry's first triangle alone (the second is killed);
 * tile uv[0..5] are nodes A, B, C's (u, v) and uv[6] the cell or quadrant. */
bool tri4_fetch(uint info, int lc, int tri2, int k, bool spare) {
    if (tri2 != 0) return false;
    uvec4 t = lc == 0 ? inCellT0 : lc == 1 ? inCellT1 : lc == 2 ? inCellT2
            : lc == 3 ? inCellT3 : lc == 4 ? inCellT4 : lc == 5 ? inCellT5
            : lc == 6 ? inCellT6 : inCellT7;
    int sub = (info & 0x2000000u) != 0u ? 16 + lc : int((info >> 7) & 1u) * 8 + lc;
    uint cell = (t.y >> 16) & 15u;
    uint f16 = 1u | (uint(k) << 3) | ((info & 0x7Fu) << 5) | 0x1000u
             | (((info >> uint(16 + lc)) & 1u) != 0u ? 0x4000u : 0u)
             | (spare ? 0x8000u : 0u);
    inPos = ivec4(inC0.xyz, int(f16));
    inUV = uvec4(cell_uv(t, k), 0u, 0u);
    inColFar = cell_unorm(nt4_node_rgb(NT_SUB[sub * 3 + k], inCellFar));
    inColNear = cell_unorm(nt4_node_rgb(NT_SUB[sub * 3 + k], inCellNear));
    inMat = uvec4(t.z, t.w, 0u, 0xC0000000u | (uint(sub) << 4) | cell);
    inFaceFlags = inCellFace.x;
#ifdef TERRAIN_ANIM
    inPrevPos = ivec4(inPrevC0.xyz, 0);
#endif
#ifdef TERRAIN_NEAR
    int ra = (k + 1) % 3, rb = (k + 2) % 3;
    inCtxAUV = uvec4(cell_uv(t, ra), 0u, 0u);
    inCtxACol = uvec2(0xFF000000u | nt4_node_rgb(NT_SUB[sub * 3 + ra], inCellFar),
                      0xFF000000u | nt4_node_rgb(NT_SUB[sub * 3 + ra], inCellNear));
    inCtxBUV = uvec4(cell_uv(t, rb), 0u, 0u);
    inCtxBCol = uvec2(0xFF000000u | nt4_node_rgb(NT_SUB[sub * 3 + rb], inCellFar),
                      0xFF000000u | nt4_node_rgb(NT_SUB[sub * 3 + rb], inCellNear));
#endif
    return true;
}

bool cells_fetch() {
    cell_unpack();
    uint info = inCellFace.y;
#ifdef TERRAIN_NEAR
    int tri = gl_VertexIndex / 6;
    int w6 = gl_VertexIndex - tri * 6;
    bool spare = w6 >= 3;
    int k = spare ? w6 - 3 : w6;
#else
    int tri = gl_VertexIndex / 3;
    int k = gl_VertexIndex - tri * 3;
    bool spare = false;
#endif
    int lc = tri >> 1, tri2 = tri & 1;
    if (((info >> uint(8 + lc)) & 1u) == 0u) return false;
    if ((info & 0x1000000u) != 0u) return tri4_fetch(info, lc, tri2, k, spare);
    int cell = int((info >> 7) & 1u) * 8 + lc;
    uvec4 t = lc == 0 ? inCellT0 : lc == 1 ? inCellT1 : lc == 2 ? inCellT2
            : lc == 3 ? inCellT3 : lc == 4 ? inCellT4 : lc == 5 ? inCellT5
            : lc == 6 ? inCellT6 : inCellT7;
    int cr = cell >> 2, cq = cell & 3;
    int role = k + tri2;
    uint f16 = 1u | (uint(role) << 3) | ((info & 0x7Fu) << 5) | 0x1000u
             | (tri2 != 0 ? 0x2000u : 0u)
             | (((info >> uint(16 + lc)) & 1u) != 0u ? 0x4000u : 0u)
             | (spare ? 0x8000u : 0u);
    inPos = ivec4(inC0.xyz, int(f16));
    inUV = uvec4(cell_uv(t, role), 0u, 0u);
    inColFar = cell_unorm(cell_node_rgb(cr + (role >> 1), cq + (role & 1), inCellFar));
    inColNear = cell_unorm(cell_node_rgb(cr + (role >> 1), cq + (role & 1), inCellNear));
    inMat = uvec4(t.z, t.w, 0u, 0x80000000u | uint(cell));
    inFaceFlags = inCellFace.x;
#ifdef TERRAIN_ANIM
    inPrevPos = ivec4(inPrevC0.xyz, 0);
#endif
#ifdef TERRAIN_NEAR
    int ra = tri2 + (k + 1) % 3, rb = tri2 + (k + 2) % 3;
    inCtxAUV = uvec4(cell_uv(t, ra), 0u, 0u);
    inCtxACol = uvec2(0xFF000000u | cell_node_rgb(cr + (ra >> 1), cq + (ra & 1), inCellFar),
                      0xFF000000u | cell_node_rgb(cr + (ra >> 1), cq + (ra & 1), inCellNear));
    inCtxBUV = uvec4(cell_uv(t, rb), 0u, 0u);
    inCtxBCol = uvec2(0xFF000000u | cell_node_rgb(cr + (rb >> 1), cq + (rb & 1), inCellFar),
                      0xFF000000u | cell_node_rgb(cr + (rb >> 1), cq + (rb & 1), inCellNear));
#endif
    return true;
}
#endif

void main()
{
    vRim = 0u;
#ifdef TERRAIN_CELLS
    if (!cells_fetch()) { kill_vert(); return; }
#endif
    uint ff   = inFaceFlags;
    uint slot = ff & FF_SLOT_MASK;
    uint sf   = floatBitsToUint(s.sec[slot].o.w);
    int  gf   = g.fog.y;
    bool lp   = (ff & FF_TIER_LP) != 0u;
    bool quad = (ff & FF_QUAD) != 0u;
    uint f16  = uint(inPos.w) & 0xFFFFu;   /* SINT attribute: strip the sign */
    bool hqv  = (f16 & 1u) != 0u;             /* HQ-subdivide variant vertex */
    int  subq = int((f16 >> 1) & 3u);         /* HQ sub-quad index           */
    int  role = int((f16 >> 3) & 3u);         /* corner slot in the sub-quad */
    bool is4  = hqv && (inMat.w & 0x80000000u) != 0u;   /* 4x4 near-cell vertex */
    bool ist  = is4 && (inMat.w & 0x40000000u) != 0u;   /* tier-4 sub-triangle  */
    int  cell = int(inMat.w & 15u);           /* its cell, row-major        */
    int  tsub = int((inMat.w >> 4) & 31u);    /* its sub-triangle (ist)     */
    bool ist2 = ist && tsub >= 16;            /* a tier-2 sub-triangle      */
    bool fillPass = (pc.pass_sel == 1);
    bool keyPass  = (pc.pass_sel == 2);

    if ((sf & SF_DRAWN) == 0u) { kill_vert(); return; }
    /* Plain vs near-variant chunk sets: a slot holds both, and the sector's
     * per-present flags pick exactly one, so a sector never draws twice. */
#ifdef TERRAIN_NEAR
    if ((sf & SF_NEARVAR) == 0u) { kill_vert(); return; }
#else
    if ((sf & SF_NEARVAR) != 0u) { kill_vert(); return; }
#endif
    /* Chunk generation vs the published slot generation: a rebuilt sector's
     * old chunk keeps drawing for presents whose (lagged) state still names
     * the old generation; the new chunk waits its turn. */
    if (((ff >> 29) & 7u) != ((sf >> 12) & 7u)) { kill_vert(); return; }

    /* Material pass: one pass per resolved member over the same arena,
     * each keeping only the faces whose resolution class maps to it. The
     * class is baked into the face's spare material bits, so it survives
     * the sector's whole residency. */
    if (pc.mat_pass >= 0) {
        const uint mc = (inMat.x >> 24) & 0xFFu;
        if (((pc.cls_mask[mc >> 7u][(mc >> 5u) & 3u] >> (mc & 31u)) & 1u) == 0u) {
            kill_vert(); return;
        }
    }

    /* ---- pass / tier admission (sector-level, CPU-computed) ---- */
    if (fillPass) {
        if (!lp || (sf & SF_FILLLP) == 0u) { kill_vert(); return; }
    } else {
        if (lp  && (sf & SF_DRAWLP) == 0u) { kill_vert(); return; }
        if (!lp && (sf & SF_DRAWHP) == 0u) { kill_vert(); return; }
    }
    /* The LP faces the HP leaves uncovered: the borrowed HP faces count as
     * cover. */
    uint ffUncov = FF_LPUNCOVX;

    /* ---- face corners -> camera space (fp64, exact walk mirror) ---- */
    dvec3 sorigin = dvec3(s.sec[slot].o.xyz);
    precise dvec3 sdel = dvec3(sorigin.x - g.dcam_xy.x,     /* s4 */
                               g.dcam_xy.y - sorigin.y,     /* s5 */
                               g.dcam_zf.x - sorigin.z);    /* s6 */
    /* Baked corner positions are x4 fixed sector-local; *0.25 is exact. */
#ifdef TERRAIN_ANIM
    /* tsc_vanim_lerp mirror: p + (c - p) * a in double, then the float cast
     * the CPU stores into cvF. Operands are local units (x4 fixed * 0.25 is
     * exact), so this is the identical expression on the identical inputs. */
    double va = g.dH.y;
    #define ANIM_LERP3(P, C) \
        dvec3(double(float(double(P.x) * 0.25lf + (double(C.x) * 0.25lf - double(P.x) * 0.25lf) * va)), \
              double(float(double(P.y) * 0.25lf + (double(C.y) * 0.25lf - double(P.y) * 0.25lf) * va)), \
              double(float(double(P.z) * 0.25lf + (double(C.z) * 0.25lf - double(P.z) * 0.25lf) * va)))
    ivec3 cc0 = inC0.xyz;
    ivec3 cc1 = ivec3(inC0.w, inC1.x, inC1.y);
    ivec3 cc2 = ivec3(inC1.z, inC1.w, inC2.x);
    ivec3 cc3 = inC2.yzw;
    ivec3 pc0 = inPrevC0.xyz;
    ivec3 pc1 = ivec3(inPrevC0.w, inPrevC1.x, inPrevC1.y);
    ivec3 pc2 = ivec3(inPrevC1.z, inPrevC1.w, inPrevC2.x);
    ivec3 pc3 = inPrevC2.yzw;
    precise dvec3 lc0 = ANIM_LERP3(pc0, cc0);
    precise dvec3 lc1 = ANIM_LERP3(pc1, cc1);
    precise dvec3 lc2 = ANIM_LERP3(pc2, cc2);
    precise dvec3 lc3 = ANIM_LERP3(pc3, cc3);
#else
    dvec3 lc0 = dvec3(inC0.xyz) * 0.25lf;
    dvec3 lc1 = dvec3(double(inC0.w), double(inC1.x), double(inC1.y)) * 0.25lf;
    dvec3 lc2 = dvec3(double(inC1.z), double(inC1.w), double(inC2.x)) * 0.25lf;
    dvec3 lc3 = dvec3(inC2.yzw) * 0.25lf;
#endif
    precise dvec3 m0 = mac_of(lc0, sdel), m1 = mac_of(lc1, sdel);
    precise dvec3 m2 = mac_of(lc2, sdel), m3c = mac_of(lc3, sdel);
    int nc = quad ? 4 : 3;

    int    fogRef   = g.fog.x + (((ff & FF_FOGBIAS) != 0u) ? 0x2000 : 0);
    double fogscale = g.dcam_zf.y;

    /* ---- NCLIP (both halves; either half admits the near verdict) ----
     * Cross products run on DOUBLES OF THE FLOAT screen coords (the CPU's
     * NSAT(sv.sx)); the GTE [-1024,1023] saturation is waived in the UNR
     * divide-overflow regime (any corner m3 < H_canon/8, the GTE's H, so
     * the regime does not move with --fov - the .s's own SXY there is
     * pinned garbage, so the TRUE facing is used). */
    vec2 p0f = proj_of(m0), p1f = proj_of(m1), p2f = proj_of(m2), p3f = proj_of(m3c);
    bool unr_ovf = (m0.z < g.dspare.x / 8.0lf) || (m1.z < g.dspare.x / 8.0lf) ||
                   (m2.z < g.dspare.x / 8.0lf) || (quad && m3c.z < g.dspare.x / 8.0lf);
    double nx0 = nsat(p0f.x, unr_ovf), ny0 = nsat(p0f.y, unr_ovf);
    double nx1 = nsat(p1f.x, unr_ovf), ny1 = nsat(p1f.y, unr_ovf);
    double nx2 = nsat(p2f.x, unr_ovf), ny2 = nsat(p2f.y, unr_ovf);
    double nx3 = nsat(p3f.x, unr_ovf), ny3 = nsat(p3f.y, unr_ovf);
    bool flip = (ff & FF_FLIP) != 0u;
    bool front, front2 = false;
    if (lp) {
        /* LP walk: HALF 1 over (i0,i1,i2), STRICTLY positive; HALF 2 over
         * (i3,i1,i2), STRICTLY negative; dside (w0 bit 7) emits regardless. */
        precise double n1 = (nx1 - nx0) * (ny2 - ny0) - (nx2 - nx0) * (ny1 - ny0);
        front = n1 > 0.0lf;
        if (quad) {
            precise double n2 = (nx1 - nx3) * (ny2 - ny3) - (nx2 - nx3) * (ny1 - ny3);
            front2 = n2 < 0.0lf;
        }
        if ((ff & FF_DSIDE) != 0u) front2 = true;
    } else {
        /* HP walk: HALF 1 over (i0,i1, quad?i3:i2), flip-adjusted, >= 0;
         * HALF 2 (quads) over (i2,i1,i3) with the flip-selected sign;
         * dside (the per-game word-3 bit) jumps straight to emit; a dside
         * tri skips its NCLIP verdict altogether. */
        double nxA = quad ? nx3 : nx2, nyA = quad ? ny3 : ny2;
        precise double n1 = (nx1 - nx0) * (nyA - ny0) - (nxA - nx0) * (ny1 - ny0);
        if (flip) n1 = -n1;
        front = n1 >= 0.0lf;
        front2 = (ff & FF_DSIDE) != 0u;
        if (quad) {
            precise double n2 = (nx1 - nx2) * (ny3 - ny2) - (nx3 - nx2) * (ny1 - ny2);
            front2 = front2 || (flip ? (n2 >= 0.0lf) : (n2 <= 0.0lf));
        }
    }

    /* ---- LP world-pass gates (gaponly / .s far cull / authored admission) ---- */
    bool gapfill_face = (gf & GF_GAPFILL) != 0 && (ff & FF_GAPFILL) != 0u;
    if (lp && !fillPass) {
        if ((sf & SF_LPGAPONLY) != 0u && (ff & FF_GAPFILL) == 0u) { kill_vert(); return; }
        if ((sf & SF_LPUNCOV) != 0u && (ff & FF_LPRIDE) == 0u) { kill_vert(); return; }
        double z3g = quad ? m3c.z : m2.z;
        double slotv = (m0.z + m1.z + m2.z + z3g) / 32.0lf;
        if ((gf & GF_NOLOD) == 0) {
            /* .s LP far cull: slot >= cull_dist>>7 drops (all faces, fill
             * tags included); a keyed game tests the whole LP bucket index
             * against (cull_dist>>7) - 1. Skipped under extended LOD, which
             * keeps far faces. */
            double fcv = slotv;
            int fclim = g.fog.z;
            if (ot_farcull_keyed()) {
                fcv += ot_lp_slots((ff >> 24) & 0x1Fu);
                fclim -= 1;
            }
            if (fcv >= double(fclim)) { kill_vert(); return; }
        }
        double thr = double((sf >> SF_LPNEAR_SHIFT) & SF_LPNEAR_MASK);
        if (!gapfill_face && (gf & GF_LPNEAROUT) != 0 && thr > 0.0lf
            && (ff & ffUncov) == 0u) {
            /* LP near outcode: a face on its sector's HP mesh, while that HP
             * draws (nonzero bound), drops when its corners all sit under
             * the sector's SZ3 bound; every other face takes the authored
             * admission below. */
            if (m0.z < thr && m1.z < thr && m2.z < thr &&
                (!quad || m3c.z < thr)) { kill_vert(); return; }
        } else if (!gapfill_face) {
            /* Authored per-face admission: slot + 8*admit >= s3, deficit
             * -31..-1 falls back to any-corner SZ3 >= (s3-8a)*8, deeper
             * rejects. (LP is a FAR tier; og drops it near.) */
            double admit = double((ff >> 19) & 0x1Fu);
            double s3v = double((g.fog.x >> 5) - 0x20);
            precise double a1 = slotv + 8.0lf * admit - s3v;
            if (a1 < 0.0lf) {
                if (a1 <= -32.0lf) { kill_vert(); return; }
                double thr = (s3v - 8.0lf * admit) * 8.0lf;
                bool any = m0.z >= thr || m1.z >= thr || m2.z >= thr ||
                           (quad && m3c.z >= thr);
                if (!any) { kill_vert(); return; }
            }
        }
        /* (The legacy lpFarOnly fog-corner near gate only exists with
         * LP_ADMIT=0, which forces the CPU walk - never mirrored here.) */
    }

    /* ---- LOD handoff: .s HP drop (tn_hp_face_drop mirror) ----
     * Dropped at sum of corner SZ3 >= m_LodDistance; with hp_drop.x set, a
     * face below that sum bound with any corner SZ3 < hp_drop.y is kept.
     * Runs BEFORE the near classify, exactly like the walk. */
    if (!lp && (gf & GF_HANDOFF) != 0) {
        double hpz3 = quad ? m3c.z : m2.z;
        precise double hpsum = (m0.z + m1.z + m2.z + hpz3) * fogscale;
        if (hpsum >= 4.0lf * double(g.fog.x)) {
            bool hpdrop = g.hp_drop.x == 0 || hpsum >= double(g.hp_drop.x);
            if (!hpdrop) {
                precise double c0 = m0.z * fogscale, c1 = m1.z * fogscale;
                precise double c2 = m2.z * fogscale, c3 = hpz3 * fogscale;
                double cmin = double(g.hp_drop.y);
                hpdrop = c0 >= cmin && c1 >= cmin && c2 >= cmin && c3 >= cmin;
            }
            if (hpdrop) { kill_vert(); return; }
        }
    }

    /* ---- FILL pass per-face gates (fully-fogged LP only, strict x4) ---- */
    if (fillPass) {
        if (((inMat.x >> 21) & 3u) != 0u) { kill_vert(); return; }  /* opaque only */
        if (fogRef - int(m0.z * 4.0lf) > 0) { kill_vert(); return; }
        if (fogRef - int(m1.z * 4.0lf) > 0) { kill_vert(); return; }
        if (fogRef - int(m2.z * 4.0lf) > 0) { kill_vert(); return; }
        if (quad && (fogRef - int(m3c.z * 4.0lf) > 0)) { kill_vert(); return; }
    }

    /* ---- face_is_near: flat-vs-HQ variant select (HP quads only).
     * The CPU gate is (straddle ? front3d : front || front2): the .s sends
     * a face either NCLIP half accepts to the same depth-sum classifier
     * (r_environment.s 0x80026CB8/0x80026CD0 -> .L80026CFC). The projected
     * NCLIP is not the .s verdict for a straddler: an HP
     * face with a corner carrying the .s vertex flag 0x10 (terrain.c
     * tface_gte_flagged: m3 <= 0, or m3 < 0x600/4 with SX/SY outside
     * [-1024, 1023] or m3 in the UNR divide-overflow regime) skips the face
     * NCLIP, and its near drain NCLIPs every sub-quad over the same two
     * triples, so its facing is the camera-space triple product over those
     * triples (front3d, the walk's tface_det3 mirror). An LP straddler (a
     * corner at or behind the eye) keeps the waiver. Behind-eye corners
     * occur only on near chunks; the flag applies to plain chunks too. ---- */
#ifdef TERRAIN_NEAR
    bool behind = m0.z <= 0.0lf || m1.z <= 0.0lf ||
                  m2.z <= 0.0lf || m3c.z <= 0.0lf;
#else
    const bool behind = false;
#endif
    bool straddle = lp ? behind
                       : (gte_flag(m0.z, p0f) || gte_flag(m1.z, p1f) ||
                          gte_flag(m2.z, p2f) || (quad && gte_flag(m3c.z, p3f)));
    bool front3d = true;
    if (!lp && straddle) {
        precise double d1 = det3d(m0, m1, quad ? m3c : m2);
        bool f1 = flip ? (d1 <= 0.0lf) : (d1 >= 0.0lf);
        bool f2 = false;
        if (quad) {
            precise double d2 = det3d(m1, m2, m3c);
            f2 = flip ? (d2 <= 0.0lf) : (d2 >= 0.0lf);
        }
        front3d = ((ff & FF_DSIDE) != 0u) || f1 || f2;
    }
    bool near = false;
    if (!lp && !fillPass && (gf & GF_NEARHQ) != 0 && quad && (straddle ? front3d : (front || front2))
        && (ff & FF_HQELIG) != 0u) {
        if ((gf & GF_HQPIN) != 0) {
            /* --no-lod: HQ pinned ON for every eligible face at every
             * distance; only the behind-camera guard and the w3&0x80
             * exclusion remain (the latter is folded into FF_HQELIG at pack
             * time). Mirrors the walk's face_is_near extended branch. */
            double sz = max(m0.z, 0.0lf) + max(m1.z, 0.0lf) + max(m2.z, 0.0lf) + max(m3c.z, 0.0lf);
            near = sz > 0.0lf;
        } else {
            near = near_canonical(m0.z, m1.z, m2.z, m3c.z,
                                  ff & FF_W3_40, ff & FF_W3_80);
        }
    }
    /* Exactly one variant of an HQ-eligible face survives: flat, the 2x2
     * subdivide, or (GF_CELLS4, a tier-4 face the bake let take the cells,
     * FF_CELLOK: the walk's tface_cells_ok) the 4x4 cells. For a face
     * the pin sends to the cells (GF_HQPIN, FF_PINCELLS: the walk's
     * tface_cells_pin, baked per face) the tier-4 distance
     * terms drop and the flag term stays: a near face with w3 & 0x40 clear
     * takes the cells (the walk's face_near_tier4 pinned branch; w3 & 0x80
     * is folded into FF_HQELIG). */
    bool pin = (gf & GF_HQPIN) != 0 && (ff & FF_PINCELLS) != 0u;
    bool t4 = near && (gf & GF_CELLS4) != 0 && (ff & FF_CELLOK) != 0u &&
              (pin ? (ff & FF_W3_40) == 0u
                   : nc4_tier4(m0.z, m1.z, m2.z, m3c.z, ff & FF_W3_40, ff & FF_W3_80));
    /* A triangle face: the flat GT3, (GF_TRI4, the tier-4 verdict over
     * A, B, C, C on a face the bake let take the cells, FF_CELLOK) its 16
     * sub-triangle records, or (GF_TRI2, any other near
     * verdict over A, B, C, C: tn_face_near) its 4 tier-2 records. Pinned
     * to the cells: every triangle not fully behind the camera with
     * w3 & 0x40 clear takes tier 4; the HQ pin sends every other one not
     * fully behind the camera to tier 2. */
    bool trinear = !quad && !lp && !fillPass && (gf & GF_NEARHQ) != 0
                && (straddle ? front3d : (front || front2)) && (ff & FF_HQELIG) != 0u;
    bool trifront = max(m0.z, 0.0lf) + max(m1.z, 0.0lf) + 2.0lf * max(m2.z, 0.0lf) > 0.0lf;
    bool t4tri = trinear && (gf & GF_TRI4) != 0 && (ff & FF_CELLOK) != 0u
              && (pin ? ((ff & FF_W3_40) == 0u && trifront)
                      : nc4_tier4(m0.z, m1.z, m2.z, m2.z, ff & FF_W3_40, ff & FF_W3_80));
    bool t2tri = trinear && !t4tri && (gf & GF_TRI2) != 0
              && ((gf & GF_HQPIN) != 0 ? trifront
                                       : near_canonical(m0.z, m1.z, m2.z, m2.z,
                                                        ff & FF_W3_40, ff & FF_W3_80));
    /* Extended LOD (GF_HPRAW, terrain.c s_hp_raw): an HP face draws its
     * near colours and base CLUT row at every distance. On a heavy-fog level
     * (GF_HPFOG, s_hp_fog) its colours DPCS-fade toward the fog colour
     * hp_drop.z and its texels fade to modulation-neutral grey on the ladder
     * ramp: hqw, the words terrain.c tn_atmo_words packs (aN = 0, bN = 0.5,
     * bit 30 the face's fog-reference bias). */
    bool hpRaw = !lp && (gf & GF_HPRAW) != 0;
    bool hpFog = hpRaw && (gf & GF_HPFOG) != 0;
    uvec2 hqw = uvec2(0x80000000u | (((ff & FF_FOGBIAS) != 0u) ? 0x40000000u : 0u),
                      128u | (128u << 10) | (128u << 20));
    vec3 fogc = vec3(float(g.hp_drop.z & 0xFF), float((g.hp_drop.z >> 8) & 0xFF),
                     float((g.hp_drop.z >> 16) & 0xFF)) / 255.0;
    /* Drain sub-cell keys (FF_KEYCELLS, terrain.c terrain_near_emit_face,
     * terrain_near_emit_tri, tn_flat_tri_emit): where the drain inserts a
     * near-band face as finer sub-cells than the variant drawn here (the 2x2
     * of a quad the drain takes to tier 4, the tier-2 split of a triangle it
     * takes to tier 4, a triangle drawn whole), each fragment of the drawn
     * triangles keys and contests at its own sub-cell's key: the vertex
     * carries the face's sub-cell key table and its face-local coordinate
     * (psx_key_contest.glsl). A triangle drawn whole takes the tier-4
     * sub-triangles or, outside tier 4, the tier-2 ones. */
    bool keyFace = !lp && (ff & FF_KEYCELLS) != 0u;
    bool drain4q = keyFace && quad
                && nc4_tier4(m0.z, m1.z, m2.z, m3c.z, ff & FF_W3_40, ff & FF_W3_80);
    bool drain4t = keyFace && !quad
                && nc4_tier4(m0.z, m1.z, m2.z, m2.z, ff & FF_W3_40, ff & FF_W3_80);
    bool flatKeys = keyFace && !quad && !fillPass && !(t4tri || t2tri)
                 && near_canonical(m0.z, m1.z, m2.z, m2.z, ff & FF_W3_40, ff & FF_W3_80);
    if (hqv != (near || t4tri || t2tri) || is4 != (t4 || t4tri || t2tri) || ist2 != t2tri) {
        kill_vert(); return;
    }
    /* nc4_front mirror: a straddling face's cell draws only when one of its
     * two triples faces. */
    if (ist && straddle && (ff & FF_DSIDE) == 0u) {
        /* nt4_emit's straddler facing: the sub-triangle's own triple. */
        precise double d = det3d(nt4_node(NT_SUB[tsub * 3], m0, m1, m2),
                                 nt4_node(NT_SUB[tsub * 3 + 1], m0, m1, m2),
                                 nt4_node(NT_SUB[tsub * 3 + 2], m0, m1, m2));
        if (flip ? (d > 0.0lf) : (d < 0.0lf)) { kill_vert(); return; }
    } else if (is4 && straddle && (ff & FF_DSIDE) == 0u) {
        int cr = cell >> 2, cq = cell & 3;
        precise dvec3 ctl = nc4_node(cr, cq, m0, m1, m3c, m2);
        precise dvec3 ctr = nc4_node(cr, cq + 1, m0, m1, m3c, m2);
        precise dvec3 cbl = nc4_node(cr + 1, cq, m0, m1, m3c, m2);
        precise dvec3 cbr = nc4_node(cr + 1, cq + 1, m0, m1, m3c, m2);
        precise double d1 = det3d(ctl, ctr, cbl);
        bool cf = flip ? (d1 <= 0.0lf) : (d1 >= 0.0lf);
        if (!cf) {
            precise double d2 = det3d(ctr, cbr, cbl);
            cf = flip ? (d2 <= 0.0lf) : (d2 >= 0.0lf);
        }
        if (!cf) { kill_vert(); return; }
    }

    /* ---- NCLIP back-face cull (flat faces; near faces dispatched before
     * the cull in the walk; fill is emit-all by design) ---- */
    if (!near && !fillPass) {
        bool cullable = lp ? ((gf & GF_CULLLP) != 0) : ((gf & GF_CULLHP) != 0);
        /* An HP straddler with a corner in front of the eye that the
         * classifier does not queue for the near drain is emitted whole with
         * no NCLIP (.L80026DB8, tri .L80027310). */
        double zq = quad ? m3c.z : m2.z;
        bool whole = !near_canonical(m0.z, m1.z, m2.z, zq, ff & FF_W3_40, ff & FF_W3_80)
                  && (m0.z > 0.0lf || m1.z > 0.0lf || m2.z > 0.0lf || zq > 0.0lf);
        bool facing = straddle ? (lp || front3d || whole) : (front || front2);
        if (cullable && !facing) { kill_vert(); return; }
    }

    /* ---- portal drops: rebuild THIS vertex's emitted tri and run the
     * feed-side inner-rim cull on it (all faces). Blended faces a portal
     * preview covers are discarded per fragment instead (terrain.frag).
     * Triangulation matches the bake exactly - flat LP (i0,i1,i2)+
     * (i1,i2,i3), flat HP (i0,i1,i3)+(i1,i3,i2), HQ sub-quad (c0,c1,c2)+
     * (c1,c2,c3) - in the walk's own emit corner ORDER ((tl,tr,bl)+
     * (tr,bl,br)); the rim classifier's terms are vertex-SET invariant. Screen coords are the feed's push_tri values:
     * the raw float projections, or their snapped form under the integer
     * vertex precision, which is what the ttri the CPU classifies carries.
     * The near variant runs both classifiers after its clip
     * instead - the feed classifies the tris it actually emits, which for a
     * clipped face are the CLIPPED ones. ---- */
#ifndef TERRAIN_NEAR
    if (g.rim_cnt.x > 0) {
        bool tri2 = (f16 & 0x2000u) != 0u;
        int rimv;
        if (!hqv) {
            precise dvec3 ta, tb, tc2;
            vec2 sa, sb, sc;
            if (!quad)      { ta = m0; tb = m1; tc2 = m2;  sa = p0f; sb = p1f; sc = p2f; }
            else if (lp) {
                if (tri2)   { ta = m1; tb = m2; tc2 = m3c; sa = p1f; sb = p2f; sc = p3f; }
                else        { ta = m0; tb = m1; tc2 = m2;  sa = p0f; sb = p1f; sc = p2f; }
            } else {
                if (tri2)   { ta = m1; tb = m3c; tc2 = m2; sa = p1f; sb = p3f; sc = p2f; }
                else        { ta = m0; tb = m1; tc2 = m3c; sa = p0f; sb = p1f; sc = p3f; }
            }
            sa = snap_of(sa); sb = snap_of(sb); sc = snap_of(sc);
            rimv = rim_classified(sa, sb, sc, ta.z, tb.z, tc2.z);
        } else if (ist) {
            /* Sub-triangle members (A,B,C), nt4_emit's order. */
            int na = NT_SUB[tsub * 3], nb = NT_SUB[tsub * 3 + 1], nc2 = NT_SUB[tsub * 3 + 2];
            precise dvec3 ta = nt4_node(na, m0, m1, m2);
            precise dvec3 tb = nt4_node(nb, m0, m1, m2);
            precise dvec3 tc2 = nt4_node(nc2, m0, m1, m2);
            vec2 sa = nt4_screen(na, m0, m1, m2);
            vec2 sb = nt4_screen(nb, m0, m1, m2);
            vec2 sc = nt4_screen(nc2, m0, m1, m2);
            rimv = rim_classified(sa, sb, sc, ta.z, tb.z, tc2.z);
        } else if (is4) {
            /* Cell members (TL,TR,BL) / (TR,BL,BR), emit_quad_cv's order. */
            precise dvec3 TL = m0, TR = m1, BL = m3c, BR = m2;
            int cr = cell >> 2, cq = cell & 3;
            ivec2 n0 = tri2 ? ivec2(cr, cq + 1) : ivec2(cr, cq);
            ivec2 n1 = tri2 ? ivec2(cr + 1, cq) : ivec2(cr, cq + 1);
            ivec2 n2 = tri2 ? ivec2(cr + 1, cq + 1) : ivec2(cr + 1, cq);
            precise dvec3 ta = nc4_node(n0.x, n0.y, TL, TR, BL, BR);
            precise dvec3 tb = nc4_node(n1.x, n1.y, TL, TR, BL, BR);
            precise dvec3 tc2 = nc4_node(n2.x, n2.y, TL, TR, BL, BR);
            vec2 sa = nc4_screen(n0.x, n0.y, TL, TR, BL, BR);
            vec2 sb = nc4_screen(n1.x, n1.y, TL, TR, BL, BR);
            vec2 sc = nc4_screen(n2.x, n2.y, TL, TR, BL, BR);
            rimv = rim_classified(sa, sb, sc, ta.z, tb.z, tc2.z);
        } else {
            /* Sub-quad nodes from the transformed parent corners (the same
             * cv_mid mirrors the own-vertex rebuild uses below). */
            precise dvec3 TL = m0, TR = m1, BL = m3c, BR = m2;
            precise dvec3 mT = (TL + TR) * 0.5lf, mB = (BL + BR) * 0.5lf;
            precise dvec3 mL = (TL + BL) * 0.5lf, mR = (TR + BR) * 0.5lf;
            precise dvec3 C  = (mT + mB) * 0.5lf;
            precise dvec3 q0, q1, q2, q3;
            if      (subq == 0) { q0 = TL; q1 = mT; q2 = mL; q3 = C;  }
            else if (subq == 1) { q0 = mT; q1 = TR; q2 = C;  q3 = mR; }
            else if (subq == 2) { q0 = mL; q1 = C;  q2 = BL; q3 = mB; }
            else                { q0 = C;  q1 = mR; q2 = mB; q3 = BR; }
            precise dvec3 ta, tb, tc2;
            int na, nb, nc2;
            if (tri2) { ta = q1; tb = q2; tc2 = q3;
                        na = NODEMAP[subq][1]; nb = NODEMAP[subq][2];
                        nc2 = NODEMAP[subq][3]; }
            else      { ta = q0; tb = q1; tc2 = q2;
                        na = NODEMAP[subq][0]; nb = NODEMAP[subq][1];
                        nc2 = NODEMAP[subq][2]; }
            vec2 sa = hq_snap_screen(na, TL, TR, BL, BR, ta);
            vec2 sb = hq_snap_screen(nb, TL, TR, BL, BR, tb);
            vec2 sc = hq_snap_screen(nc2, TL, TR, BL, BR, tc2);
            rimv = rim_classified(sa, sb, sc, ta.z, tb.z, tc2.z);
        }
        if (rimv == 1) { kill_vert(); return; }
        vRim = rimv >= 2 ? uint(rimv - 1) : 0u;
    }
#endif

    /* ---- material: near-tile / CLUT-ladder row select (flat HP textured) ---- */
    uint mat0 = inMat.x;
    uint clut = inMat.y & 0xFFFFu;
    vec2 uv   = vec2(float(inUV.x), float(inUV.y));
    uint dbg_half = 0u, dbg_delta = 0u;
    int  dbg_t0k  = -99999;   /* tint mode 6: exact ladder key (t0k) probe */
    bool ladderFace = false;
    if (!lp && !hqv && (gf & GF_CLUTFOG) != 0 && (gf & GF_HPRAW) == 0 && (sf & SF_SECINLOD) != 0u
        && (mat0 & (1u << 23)) != 0u) {
        ladderFace = true;
        /* per-corner int casts, then sum - the walk's exact quantisation */
        int szsum = int(m0.z * fogscale) + int(m1.z * fogscale)
                  + int(m2.z * fogscale) + int(m3c.z * fogscale);
        int key = (szsum >> 7) << 5;
        int t0k = key - fogRef + 4096;
        if ((gf & GF_LADNEAR) != 0) {
            /* nearest-corner rule (the walk's ladder_nearest_corner branch):
             * row = 15 - (max clamped IR0 >> 8), t0k = 256 * row */
            int irmax = 0;
            irmax = max(irmax, clamp(fogRef - int(m0.z * fogscale), 0, 0xFFF));
            irmax = max(irmax, clamp(fogRef - int(m1.z * fogscale), 0, 0xFFF));
            irmax = max(irmax, clamp(fogRef - int(m2.z * fogscale), 0, 0xFFF));
            irmax = max(irmax, clamp(fogRef - int(m3c.z * fogscale), 0, 0xFFF));
            t0k = (15 - (irmax >> 8)) << 8;
        }
        dbg_t0k = t0k;
        if (t0k <= 0) {
            if ((inMat.z & (1u << 23)) != 0u) {   /* near half textured */
                mat0 = inMat.z;
                clut = inMat.w & 0xFFFFu;
                uv   = vec2(float(inUV.z), float(inUV.w));
                dbg_half = 1u;
            }
        } else {
            int delta = min(t0k >> 8, 15);
            dbg_delta = uint(delta);
            if (delta > 0) clut = (clut + uint(delta << 6)) & 0xFFFFu;
        }
    }
    /* HD-fold entry select: the bake stores one entry id per LOD half in
     * the clut words' upper 16 bits (0 = no replacement); the drawn half's
     * id ships to the fragment stage with the CONT-fog flags (ladder faces
     * fade on the published ramp; fogbias shifts it - ff bit 10). */
    vHd = 0u;
    if (g.hd_lad.w != 0.0 && (mat0 & (1u << 23)) != 0u) {
        uint hid = (dbg_half == 1u ? (inMat.w >> 16) : (inMat.y >> 16)) & 0xFFFFu;
        if (hid != 0u)
            vHd = hid | (ladderFace ? 0x10000u : 0u)
                      | (((ff >> 10) & 1u) << 17);
    }
    /* env UV-scroll: per-tile fractional V offset for THIS build. Flat
     * faces: the LQ dv (both LOD halves share one offset - the game writes
     * the same scroll byte into all four V slots); HQ sub-quads: the
     * per-half dv (entries 1-2 vs 3-4 sit half a window apart in the
     * scroll ring). Zero for a tile that is not on the scroller list. */
    /* A borrowed face's tile id names the source level's table, whose
     * scroll state this scene does not run. */
    bool scrollOn = (f16 & 0x1000u) != 0u && (lp || (ff & FF_BORROWED) == 0u);
    float uvdv = 0.0;
    if (scrollOn) {
        int stile = int((f16 >> 5) & 0x7Fu);
        uvdv = ist2 ? g.sdv[stile][(cell >> 1) + 1]
             : is4 ? g.sdvc[stile][cell >> 2]
             : hqv ? g.sdv[stile][(subq >> 1) + 1] : g.sdv[stile].x;
        uv.y += uvdv;
    }
    vMat = uvec4(mat0, (clut & 0x3Fu) * 16u | (((clut >> 6) & 0x1FFu) << 16),
                 (hpFog && (mat0 & (1u << 23)) != 0u) ? hqw : uvec2(0u));
    /* A borrowed face samples the auxiliary VRAM bank, 512 rows below the
     * PSX VRAM: bit 31 tells the fragment stage to offset the page row,
     * and the CLUT row carries the offset here. */
    if (!lp && (ff & FF_BORROWED) != 0u)
        vMat.xy = uvec2((mat0 & 0x00FFFFFFu) | 0x80000000u, vMat.y + (512u << 16));
    /* Debug tints 0-6 repurpose vMat.y; mode 7 keeps it stock, since its
     * fragment runs the stock texel fetch and cut-out. */
    if ((gf & (1 << 29)) != 0 && ((gf >> 26) & 7) != 7) {   /* ladder verdict */
        vMat.y |= (dbg_half << 30) | (dbg_delta << 25);
        if (((uint(gf) >> 26) & 7u) == 6u)     /* mode 6: exact t0k readout */
            vMat.y = dbg_t0k <= -99999
                   ? 0u
                   : (uint(clamp(dbg_t0k + 4096, 0, 8191)) << 18)
                     | (dbg_half << 17) | (1u << 16)
                     | ((slot & 0x7Fu) << 9);   /* winner's sector slot */
    }

    /* ---- own vertex camera-space value ----
     * Flat/LP verts: transform own position (identical ops to the corner
     * transforms). HQ verts: rebuild the sub-quad node from the TRANSFORMED
     * parent corners (cv_mid mirrors - midpoints averaged in camera space).
     * The near variant instead rebuilds the WHOLE emitted triangle, clips it
     * against the near plane and becomes one corner of the clipped fan. */
    precise dvec3 mo;
    vec2 sscr;      /* the emitted screen point: push_tri's t->sx/sy mirror */
    vec2 mfl;       /* its drain face-local coordinate (psx_key_contest.glsl) */
#ifdef TERRAIN_NEAR
    vec4 col;
    {
        /* Emitted-tri member positions, in the bake's emit order. */
        bool tri2n = (f16 & 0x2000u) != 0u;
        dvec3 tp[3];
        /* Per-member screen override: set on a subdivide node (the CPU's
         * ngte_cvert.oflag), absent on a flat face's own corners. */
        vec2 tos[3] = vec2[3](vec2(0.0), vec2(0.0), vec2(0.0));
        bool tov = false;
        /* Members' drain coordinates (psx_key_contest.glsl): a triangle's
         * corners A, B, C at (0,0), (4,0), (0,4), a split's nodes at their
         * grid places, a 2x2 quadrant's corners at their isoline sides; a
         * flat quad keys whole and carries none. */
        vec2 tfl[3] = vec2[3](vec2(0.0), vec2(0.0), vec2(0.0));
        if (!hqv) {
            if (!quad)      { tp[0] = m0; tp[1] = m1; tp[2] = m2;
                              tfl[1] = vec2(4.0, 0.0); tfl[2] = vec2(0.0, 4.0); }
            else if (lp) {
                if (tri2n)  { tp[0] = m1; tp[1] = m2; tp[2] = m3c; }
                else        { tp[0] = m0; tp[1] = m1; tp[2] = m2;  }
            } else {
                if (tri2n)  { tp[0] = m1; tp[1] = m3c; tp[2] = m2; }
                else        { tp[0] = m0; tp[1] = m1; tp[2] = m3c; }
            }
        } else if (ist) {
            /* Sub-triangle members (A,B,C); each carries its emitted
             * point (the snap) as its override. */
            for (int k = 0; k < 3; k++) {
                int n = NT_SUB[tsub * 3 + k];
                tp[k]  = nt4_node(n, m0, m1, m2);
                tos[k] = nt4_screen(n, m0, m1, m2);
                tfl[k] = nt4_fl(n);
            }
            tov = true;
        } else if (is4) {
            /* Cell members (TL,TR,BL) / (TR,BL,BR); each carries its
             * emitted point (snap and morph) as its override. */
            precise dvec3 TL = m0, TR = m1, BL = m3c, BR = m2;
            int cr = cell >> 2, cq = cell & 3;
            ivec2 nn[3];
            nn[0] = tri2n ? ivec2(cr, cq + 1) : ivec2(cr, cq);
            nn[1] = tri2n ? ivec2(cr + 1, cq) : ivec2(cr, cq + 1);
            nn[2] = tri2n ? ivec2(cr + 1, cq + 1) : ivec2(cr + 1, cq);
            for (int k = 0; k < 3; k++) {
                tp[k]  = nc4_node(nn[k].x, nn[k].y, TL, TR, BL, BR);
                tos[k] = nc4_screen(nn[k].x, nn[k].y, TL, TR, BL, BR);
                tfl[k] = vec2(float(nn[k].y), float(nn[k].x));
            }
            tov = true;
        } else {
            precise dvec3 TL = m0, TR = m1, BL = m3c, BR = m2;
            precise dvec3 mT = (TL + TR) * 0.5lf, mB = (BL + BR) * 0.5lf;
            precise dvec3 mL = (TL + BL) * 0.5lf, mR = (TR + BR) * 0.5lf;
            precise dvec3 C  = (mT + mB) * 0.5lf;
            precise dvec3 q0, q1, q2, q3;
            if      (subq == 0) { q0 = TL; q1 = mT; q2 = mL; q3 = C;  }
            else if (subq == 1) { q0 = mT; q1 = TR; q2 = C;  q3 = mR; }
            else if (subq == 2) { q0 = mL; q1 = C;  q2 = BL; q3 = mB; }
            else                { q0 = C;  q1 = mR; q2 = mB; q3 = BR; }
            int base = tri2n ? 1 : 0;
            if (tri2n) { tp[0] = q1; tp[1] = q2; tp[2] = q3; }
            else       { tp[0] = q0; tp[1] = q1; tp[2] = q2; }
            for (int k = 0; k < 3; k++) tfl[k] = quad_side_fl(q0, q1, q2, q3, tp[k]);
            if (pc.snap != 0) {
                for (int k = 0; k < 3; k++)
                    tos[k] = hq_snap_screen(NODEMAP[subq][base + k],
                                            TL, TR, BL, BR, tp[k]);
                tov = true;
            }
        }
        /* Member attributes: self's are this vertex's own, the other two
         * ride the ctx twin in emit order (a = self+1, b = self+2 mod 3).
         * All six vertices of a source triangle sit at whole-triangle
         * multiples from the arena base, so gl_VertexIndex % 3 IS the slot. */
        int self = gl_VertexIndex % 3;
        int ia = (self + 1) % 3, ib = (self + 2) % 3;
        vec3 cf[3], cn[3];
        vec2 uvm[3];
        cf[self] = op_unorm8(inColFar.rgb);
        cn[self] = op_unorm8(inColNear.rgb);
        uvm[self] = uv;
        cf[ia] = unrgb(inCtxACol.x); cn[ia] = unrgb(inCtxACol.y);
        cf[ib] = unrgb(inCtxBCol.x); cn[ib] = unrgb(inCtxBCol.y);
        uvm[ia] = (dbg_half == 1u) ? vec2(inCtxAUV.z, inCtxAUV.w)
                                   : vec2(inCtxAUV.x, inCtxAUV.y);
        uvm[ib] = (dbg_half == 1u) ? vec2(inCtxBUV.z, inCtxBUV.w)
                                   : vec2(inCtxBUV.x, inCtxBUV.y);
        if (scrollOn) { uvm[ia].y += uvdv; uvm[ib].y += uvdv; }

        bool doFog   = hpRaw ? hpFog : (!lp && !hqv);
        bool doGrade = lp && (sf & SF_LPGRADE) != 0u;
        int  dbgmode = (gf >> 26) & 7;
        bool dbgraw  = (gf & (1 << 29)) != 0 && (dbgmode == 4 || dbgmode == 5);
        cvx cin[3];
        for (int k = 0; k < 3; k++) {
            /* an HQ vertex bakes its true far colour; unfogged it draws near */
            vec3 c = terrain_col(tp[k].z, hpRaw ? (hpFog ? fogc : cn[k]) : (hqv ? cn[k] : cf[k]), cn[k], doFog,
                                 fogRef, fogscale, doGrade, s.sec[slot].gp, s.sec[slot].gq);
            if (dbgraw) c = (dbgmode == 4) ? cn[k] : cf[k];
            cin[k].m  = tp[k];
            cin[k].c  = ivec3(round(c * 255.0));
            cin[k].uv = uvm[k];
            cin[k].os = tos[k];
            cin[k].ov = tov;
            cin[k].fl = tfl[k];
        }

        /* Sutherland-Hodgman against m3 >= zc, with the walk's doubling
         * retry: a crossing vertex still projecting beyond CLIP_PROJ_LIMIT
         * re-clips the whole face at a doubled zc (<= 16 rounds). */
        cvx outv[4];
        int nout = 0;
        double zc = g.dnear.x;
        for (int rnd = 0; rnd < 24; rnd++) {
            nout = 0;
            bool clipped = false;
            for (int i = 0; i < 3; i++) {
                cvx cur = cin[i], nxt = cin[(i + 1) % 3];
                bool curIn = cur.m.z >= zc, nxtIn = nxt.m.z >= zc;
                if (curIn) outv[nout++] = cur;
                if (curIn != nxtIn && nout < 4) {
                    outv[nout++] = cv_lerp_n(cur, nxt, zc);
                    clipped = true;
                }
            }
            if (nout < 3) { kill_vert(); return; }   /* fully behind the plane */
            if (clipped && rnd < 16) {
                bool sane = true;
                for (int i = 0; i < nout && sane; i++) {
                    if (outv[i].m.z != zc) continue;   /* crossings move with zc */
                    vec2 sp = proj_of(outv[i].m);
                    if (abs(sp.x - float(g.dof_xy.x)) > CLIP_PROJ_LIMIT ||
                        abs(sp.y - float(g.dof_xy.y)) > CLIP_PROJ_LIMIT) sane = false;
                }
                if (!sane) { zc *= 2.0lf; continue; }
            }
            break;
        }
        /* Fan the clipped polygon: (out0,out1,out2) + (out0,out2,out3). */
        bool spare = (f16 & 0x8000u) != 0u;
        if (spare && nout < 4) { kill_vert(); return; }
        int e0 = 0, e1 = spare ? 2 : 1, e2 = spare ? 3 : 2;
        if (g.rim_cnt.x > 0) {
            vec2 sa = scr_of(outv[e0]), sb = scr_of(outv[e1]),
                 sc = scr_of(outv[e2]);
            int rimv = rim_classified(sa, sb, sc, outv[e0].m.z, outv[e1].m.z, outv[e2].m.z);
            if (rimv == 1) { kill_vert(); return; }
            vRim = rimv >= 2 ? uint(rimv - 1) : 0u;
        }
        cvx V = outv[self == 0 ? e0 : (self == 1 ? e1 : e2)];
        mo  = V.m;
        sscr = scr_of(V);
        mfl = V.fl;
        uv  = V.uv;
        col = vec4(vec3(V.c) / 255.0, 1.0);
    }
#else
    if (!hqv) {
#ifdef TERRAIN_ANIM
        precise dvec3 lown = ANIM_LERP3(inPrevPos.xyz, inPos.xyz);
#else
        dvec3 lown = dvec3(inPos.xyz) * 0.25lf;
#endif
        mo = mac_of(lown, sdel);
        sscr = snap_of(proj_of(mo));
        /* A triangle's corner A, B or C at (0,0), (4,0), (0,4); a flat
         * quad keys whole and carries none. */
        ivec3 op = inPos.xyz;
        mfl = quad || op == inC0.xyz ? vec2(0.0)
            : op == ivec3(inC0.w, inC1.x, inC1.y) ? vec2(4.0, 0.0) : vec2(0.0, 4.0);
    } else if (ist) {
        int n = NT_SUB[tsub * 3 + role];
        mo = nt4_node(n, m0, m1, m2);
        sscr = nt4_screen(n, m0, m1, m2);
        mfl = nt4_fl(n);
    } else if (is4) {
        precise dvec3 TL = m0, TR = m1, BL = m3c, BR = m2;
        int nr = (cell >> 2) + (role >> 1), nq = (cell & 3) + (role & 1);
        mo = nc4_node(nr, nq, TL, TR, BL, BR);
        sscr = nc4_screen(nr, nq, TL, TR, BL, BR);
        mfl = vec2(float(nq), float(nr));
    } else {
        /* Parent corners in .s grid order: TL=i0, TR=i1, BL=i3, BR=i2. */
        precise dvec3 TL = m0, TR = m1, BL = m3c, BR = m2;
        precise dvec3 mT = (TL + TR) * 0.5lf, mB = (BL + BR) * 0.5lf;
        precise dvec3 mL = (TL + BL) * 0.5lf, mR = (TR + BR) * 0.5lf;
        precise dvec3 C  = (mT + mB) * 0.5lf;
        int node = NODEMAP[subq][role];
        mo = node == 0 ? TL : node == 1 ? TR : node == 2 ? BR : node == 3 ? BL
           : node == 4 ? mT : node == 5 ? mB : node == 6 ? mL : node == 7 ? mR : C;
        sscr = hq_snap_screen(node, TL, TR, BL, BR, mo);
        mfl = subq == 0 ? quad_side_fl(TL, mT, mL, C, mo)
            : subq == 1 ? quad_side_fl(mT, TR, C, mR, mo)
            : subq == 2 ? quad_side_fl(mL, C, BL, mB, mo)
            :             quad_side_fl(C, mR, mB, BR, mo);
    }
#endif

    /* ---- og OT key (feed koff mirror; ambient per face / per HQ sub-quad) ---- */
    float koff = 0.0;
    float kcls = OP_KEY_CLASS_NONE;
    dtab ktab;
    ktab.t0 = uvec4(0u); ktab.t1 = uvec4(0u); ktab.kmin = 0.0; ktab.n = 0;
    if ((gf & GF_KEYON) != 0 && !fillPass) {
        /* Foreground (near-band) faces: ALWAYS the canonical classifier
         * here (LP tests the distance band alone - word3 = 0), independent
         * of the NOLOD HQ pin. GF_NBKEYS (game_layout.terrain.nearband_ot_keys)
         * set: the game keys them through the ordinary insert with their
         * authored slots, so they take the far faces' key below. Clear: the
         * game's near queue has no OT index, so opaque ones are contest-only
         * (negative key, no slots). Blended near-band faces stay out either
         * way (the feed's koff = 0). */
        bool nearband = near_canonical(m0.z, m1.z, m2.z, quad ? m3c.z : m2.z,
                                       lp ? 0u : (ff & FF_W3_40),
                                       lp ? 0u : (ff & FF_W3_80));
        /* otkey: a whole face's 4-corner view-Z sum x4 (tris sum the shared
         * i2==i3 corner twice, like the .s); a subdivided face's triangles
         * carry their own drain sub-cell's key (drain_key4 / drain_key3,
         * emit_quad_cv and nt_emit). Rounded to float exactly where the
         * walk rounds. */
        float ok;
        if (!hqv) {
            ok = float((m0.z + m1.z + m2.z + m3c.z) * 4.0lf);
        } else if (ist) {
            ok = drain_key3(nt4_node(NT_SUB[tsub * 3], m0, m1, m2).z,
                            nt4_node(NT_SUB[tsub * 3 + 1], m0, m1, m2).z,
                            nt4_node(NT_SUB[tsub * 3 + 2], m0, m1, m2).z, nearband);
        } else if (is4) {
            int cr = cell >> 2, cq = cell & 3;
            ok = drain_key4(nc4_node(cr, cq, m0, m1, m3c, m2).z,
                            nc4_node(cr, cq + 1, m0, m1, m3c, m2).z,
                            nc4_node(cr + 1, cq, m0, m1, m3c, m2).z,
                            nc4_node(cr + 1, cq + 1, m0, m1, m3c, m2).z, nearband);
        } else {
            precise dvec3 TL = m0, TR = m1, BL = m3c, BR = m2;
            precise dvec3 mT = (TL + TR) * 0.5lf, mB = (BL + BR) * 0.5lf;
            precise dvec3 mL = (TL + BL) * 0.5lf, mR = (TR + BR) * 0.5lf;
            precise dvec3 C  = (mT + mB) * 0.5lf;
            precise dvec4 sq =
                subq == 0 ? dvec4(TL.z, mT.z, mL.z, C.z)
              : subq == 1 ? dvec4(mT.z, TR.z, C.z, mR.z)
              : subq == 2 ? dvec4(mL.z, C.z, BL.z, mB.z)
              :             dvec4(C.z, mR.z, mB.z, BR.z);
            ok = drain_key4(sq.x, sq.y, sq.z, sq.w, nearband);
        }
        /* A drawn triangle of a face the drain inserts finer keys at the
         * least of its sub-cell keys; the fragment adds its own sub-cell's
         * excess from the table (psx_key_contest.glsl op_drain_delta). */
        bool kcull = straddle && (ff & FF_DSIDE) == 0u;
        bool kflip = (ff & FF_FLIP) != 0u;
        if (hqv && !is4 && drain4q)
            ktab = quad_cells_tab(m0, m1, m3c, m2, kcull, kflip);
        else if (ist2 && (gf & GF_TRI4) != 0 && drain4t)
            ktab = tri_cells_tab(m0, m1, m2, false, kcull, kflip);
        else if (!hqv && flatKeys)
            ktab = tri_cells_tab(m0, m1, m2, !drain4t, kcull, kflip);
        if (ktab.n > 0) ok = ktab.kmin;
        if (nearband && (((mat0 >> 21) & 3u) != 0u)) {
            koff = 0.0;
        } else if (nearband && (gf & GF_NBKEYS) == 0) {
            koff = -float(double(ok) + 0.5lf);
        } else {
            uint ob = (ff >> 24) & 0x1Fu;
            double slots = lp ? ot_lp_slots(ob)
                              : double(((ff & FF_LPKEY) != 0u ? ot_lp_base() : 0u)
                                       + ot_hp_mul() * ob);
            koff = float(double(ok) + 128.0lf * slots + 0.5lf);
            /* Opaque faces only, as the walk feeds them (terrain.c
             * tn_key_attr): a blended face keeps the unbounded override. */
            if (((mat0 >> 21) & 3u) == 0u)
                kcls = op_key_class(!lp && (ff & FF_LPKEY) == 0u, float(slots));
        }
    }
    vKoff = koff;
    vKcls = kcls;
    /* The table rides only a positively keyed textured face: the key pass
     * rasters nothing else, op_drain_koff_pf moves nothing else, and the
     * walked scene locates the sub-cell through the texture coordinate
     * (psx_key_contest.glsl op_drain_record), which an untextured face
     * lacks. A folded quadrant's triangle also carries the face's corners
     * and its position D in the fold test's homogeneous screen space. */
    vKn0 = vec4(0.0); vKn1 = vec4(0.0); vKn2 = vec4(0.0);
    if (koff > 0.25 && ktab.n > 0 && (mat0 & (1u << 23)) != 0u) {
        vKc0 = ktab.t0; vKc1 = ktab.t1;
        if (hqv && !is4) vKc0.x |= uint(subq) << 30;   /* the drawn quadrant */
        uint fw = subq < 2 ? vKc0.y : vKc0.z;           /* entry 2 + subq, bit 14 */
        if (hqv && !is4 && ((fw >> (uint(subq & 1) * 16u + 14u)) & 1u) != 0u) {
            vec3 cTL = fold_h(m0), cTR = fold_h(m1), cBL = fold_h(m3c), cBR = fold_h(m2);
            vKn0 = vec4(cTL, cTR.x); vKn1 = vec4(cTR.yz, cBL.xy); vKn2 = vec4(cBL.z, cBR);
        }
    }
    else                           { vKc0 = uvec4(0u); vKc1 = uvec4(0u); }
    vec3 kd = vec3(sscr, 1.0) * float(mo.z);

    /* Debug tint mode 7 (face attribution): bits 8-29 of vRim carry this
     * face's identity word (attr_word) for terrain_shade.glsl's ter_attr. */
    if ((gf & (1 << 29)) != 0 && ((gf >> 26) & 7) == 7)
        vRim = (vRim & 0xFFu) | (attr_word(slot, lp, hqv, is4, koff > 0.25 && ktab.n > 0) << 8);

    /* ---- KEY pass: the flat og key's face (terrain_key.vert mirror) ----
     * terrain_key.vert receives the key K as view-Z from the CPU feed; the
     * cache holds K itself. terrain_key_cache.frag stores the key as its
     * fragment depth; the window depth here only places the face for the
     * clipper (op_key_clip). */
    if (keyPass) {
        if (koff <= 0.25) { kill_vert(); return; }   /* keyed faces only */
        float K  = koff - 0.5;
        float kv = max(K, 1.0);
        vec2 cxy = clip_of(sscr);
        gl_Position = op_key_clip(cxy, kv, kv);
        float fvz = float(mo.z) * 16.0;              /* TRUE per-vertex view-Z */
        float wq  = AFFINE_UV ? 1.0 : max(fvz, 1.0);
        vUV = uv / wq;
        vQ  = 1.0 / wq;
        /* x: the owner's true 1/view-Z, screen-linear under the flat key W
         * (terrain_key_cache.frag writes the owner's view-Z beside the key). */
        vColor = vec4(1.0 / max(fvz, 1.0), 0.0, 0.0, 1.0);
        /* p interpolates as the UV does (psx_key_contest.glsl). */
        vKfl = mfl / wq;
        vKd  = kd / wq;
        vKq  = 1.0 / wq;
        return;
    }

#ifndef TERRAIN_NEAR
    /* ---- colour: GTE-exact DPCS fog (flat HP); baked raw otherwise. The
     * near variant resolved this per member before its clip. ---- */
#if defined(TERRAIN_ANIM) && !defined(TERRAIN_CELLS)
    vec3 cfar8  = canim_col(inPrevCol.x, op_unorm8(inColFar.rgb));
    vec3 cnear8 = canim_col(inPrevCol.y, op_unorm8(inColNear.rgb));
#else
    vec3 cfar8  = op_unorm8(inColFar.rgb);
    vec3 cnear8 = op_unorm8(inColNear.rgb);
#endif
    vec4 col = vec4(terrain_col(mo.z, hpRaw ? (hpFog ? fogc : cnear8) : (hqv ? cnear8 : cfar8), cnear8,
                                hpRaw ? hpFog : (!lp && !hqv), fogRef, fogscale,
                                lp && (sf & SF_LPGRADE) != 0u,
                                s.sec[slot].gp, s.sec[slot].gq), 1.0);
    {   /* debug tint modes 4/5: show the baked near / far colour raw */
        int dbgmode = (gf >> 26) & 7;
        if ((gf & (1 << 29)) != 0 && dbgmode == 4) col = vec4(op_unorm8(inColNear.rgb), 1.0);
        if ((gf & (1 << 29)) != 0 && dbgmode == 5) col = vec4(op_unorm8(inColFar.rgb), 1.0);
    }
#endif

    /* ---- position: the feed + psx_scene.vert contract, exactly ----
     * feed: z = pvz = vz with vz = float(m3)*16 (near-guarded at 1);
     * scene VS: vzo = the clamped z, q = vzo/Wu (Wu = the same vz), and
     * the (zc, W) pair uses W == vzo for clip invariance, so window depth
     * and q match the CPU-fed pipeline bit-for-bit. */
    float fvz = float(mo.z) * 16.0;
    float vzs = max(fvz, 1.0);
    float vzo = op_scene_vz(vzs);
    float Wu  = vzs;                       /* inW = pvz = the same view-Z */
    float q   = AFFINE_UV ? 1.0 : (vzo / Wu);
    gl_Position = op_scene_clip(clip_of(sscr), vzo, AFFINE_UV, DEPTH_REVERSED);
    vUV    = uv * q;
    vColor = col * q;
    vQ     = q;
    vKfl = mfl * q;      /* p interpolates as the UV does (psx_key_contest.glsl) */
    vKd  = kd * q;
    vKq  = q;
}
