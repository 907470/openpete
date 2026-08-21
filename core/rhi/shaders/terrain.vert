#version 450
#extension GL_ARB_gpu_shader_fp64 : require

/* Terrain sector-mesh vertex stage — the retained-terrain GPU path
 * (extraction rearchitecture, seam 2).
 *
 * Mirrors — bit-for-bit — what the CPU walk (terrain_native.c walk_sector +
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
 * same order in fp64 (`precise` — no FMA contraction) and casts at the same
 * points, so the emitted float positions are identical and the raster is
 * pixel-identical. HQ sub-quad nodes are built by averaging the TRANSFORMED
 * corner values (cv_mid mirrors), never by transforming baked midpoints —
 * the CPU's float rounding differs between the two.
 *
 * Sector ownership is decided on the CPU (sector-params UBO): a claimed
 * sector renders through the unchanged CPU walk and is killed here, so no
 * face is ever drawn by both paths. Claimed sectors include everything that
 * may approach the near plane — an unclaimed vertex is GUARANTEED m3 well
 * in front of the camera, which is what makes the raw H/m3 projection (no
 * near clipping, no straddle handling) safe here. */

layout(location = 0) in ivec4 inPos;      /* xyz: sector-local x4 fixed; w: flags16 */
layout(location = 1) in uvec4 inUV;       /* far u,v, near u,v (texels)   */
layout(location = 2) in vec4  inColFar;   /* far colour  (UNORM 0..1)     */
layout(location = 3) in vec4  inColNear;  /* near colour (UNORM 0..1)     */
layout(location = 4) in uvec4 inMat;      /* far0, far1, near0, near1     */
layout(location = 5) in uint  inFaceFlags;
layout(location = 6) in ivec4 inC0;       /* c0.xyz, c1.x   (x4 fixed)    */
layout(location = 7) in ivec4 inC1;       /* c1.yz, c2.xy                 */
layout(location = 8) in ivec4 inC2;       /* c2.z, c3.xyz                 */
#ifdef TERRAIN_ANIM
/* Poly-anim chunks (binding 1, rhi_terrain_prev): previous-tick twin of the
 * position + parent corners. Every local coordinate is lerped prev->cur in
 * fp64 at the build's alpha and CAST TO FLOAT before the camera transform —
 * the exact mirror of tsc_vanim_lerp filling cvF (alpha = 1 on canonical
 * builds collapses to cur bit-exactly, ints casting to float losslessly). */
layout(location = 9)  in ivec4 inPrevPos; /* prev xyz (x4 fixed), pad     */
layout(location = 10) in ivec4 inPrevC0;
layout(location = 11) in ivec4 inPrevC1;
layout(location = 12) in ivec4 inPrevC2;
#endif

layout(push_constant) uniform TerrainPC {
    vec4 cbase;   /* x_base, x_span, y_base, y_span (scene_to_clip terms)  */
    vec4 cdims;   /* disp_w, clip_h, band, freeze_wf                       */
    int  pass_sel;      /* 0 = WORLD, 1 = FILL (LP hole-fill), 2 = KEY    */
    int  full_extent;   /* full-target branch of the mapping              */
    int  pad0, pad1;
} pc;

/* scene_to_clip mirror, term for term in float — a fitted affine loses ~5
 * subpixel LSBs at the screen edges and flips rasterized edge pixels. */
vec2 clip_of(vec2 sxy) {
    float nx = sxy.x / pc.cdims.x;
    float ny = (sxy.y - pc.cdims.z) / pc.cdims.y;
    if (pc.full_extent != 0)
        return vec2((-1.0 + nx * 2.0) / pc.cdims.w, 1.0 - ny * 2.0);
    return vec2(pc.cbase.x + nx * pc.cbase.y, pc.cbase.z - ny * pc.cbase.w);
}

layout(std140, set = 3, binding = 0) uniform TerrainGlobals {
    vec4  mrow0;    /* xyz: M[0][*] (raw int16 values)                    */
    vec4  mrow1;    /* xyz: M[1][*]                                       */
    vec4  mrow2;    /* xyz: M[2][*]                                       */
    dvec2 dcam_xy;  /* camx, camy (fractional, world/16)                  */
    dvec2 dcam_zf;  /* camz, s_fogscale                                   */
    dvec2 dof_xy;   /* ofx, ofy (GTE screen offset, px)                   */
    dvec2 dH;       /* x: H (projection distance), y: poly-anim sub-tick
                     * alpha (1.0 on canonical builds; TERRAIN_ANIM only)   */
    ivec4 fog;      /* x: fogRefBase, y: gflags, z: cullRefS2, w: unused  */
    vec4  sdv[128]; /* per-tile scroll V offsets (lq, hqA, hqB, 0)        */
    /* Portal inner-rim cull frames (portal_rim_classify mirror, fp64).
     * rim_cnt.x = active plane count; 0 disables the test (no portal, or
     * rim-candidate sectors claim to the CPU walk instead). Plane pi's
     * outline lives at rim_pv[pi*16 .. pi*16+vn-1]. */
    ivec4 rim_cnt;
    ivec4 rim_pad;
    dvec4 rim_oxyH;      /* rim ofx, ofy, H, pad                          */
    dvec4 rim_knobs;     /* eps, near, reach, cos(angle)                  */
    dvec4 rim_abc[6];    /* screen inv-vz plane fit a,b,c; w = valid      */
    dvec4 rim_O[6];      /* in-plane basis origin (view space)            */
    dvec4 rim_U[6];      /* in-plane basis U                              */
    dvec4 rim_W[6];      /* in-plane basis W                              */
    dvec4 rim_nrm[6];    /* unit view normal; w = outline vn              */
    dvec4 rim_bbox[6];   /* outline UV bbox pulo, puhi, pvlo, pvhi        */
    dvec2 rim_pv[96];    /* outlines in (U,W), 16 slots per plane         */
    vec4  hd_lad;        /* HD fold: CONT-fog ramp ts, tb; count; enable  */
    vec4  hd_ent[1664];  /* 416 entries x 4 vec4 (rect/atlas/fogA/fogB)   */
    /* Blended-face portal-cover drop (portal_sky_covers mirror): active
     * opening planes in SCREEN space. cov_cnt.x = plane count (0 = test
     * off); cov_abc.w = the plane's outline point count (pn < 3 never
     * matches, the CPU test's skip). One outline point per cov_poly slot,
     * 16 slots per plane. */
    ivec4 cov_cnt;
    dvec4 cov_margin;    /* x = plane-cover near margin M (world units)   */
    dvec4 cov_abc[6];    /* screen inv-vz plane fit a, b, c; w = pn       */
    vec4  cov_bbox[6];   /* outline screen bbox x0, y0, x1, y1            */
    vec4  cov_poly[96];  /* outline points (px, py, 0, 0)                 */
} g;

/* g.fog.y bits — keep in sync with openpete_terrain_cache.h OPTC_GF_* */
#define GF_NEARHQ    1
#define GF_CLUTFOG   2
#define GF_CULLHP    4
#define GF_HANDOFF   8
#define GF_GAPFILL   16
#define GF_NOLOD     32
#define GF_CULLLP    64
#define GF_KEYON     128

struct TerrainSector {
    vec4 o;        /* xyz: sector origin (world/16), w: uintBitsToFloat(flags) */
    vec4 gp, gq;   /* pack LP grade affine v' = p*v8 + q*255 (rgb; SF_LPGRADE) */
};
layout(std140, set = 3, binding = 1) uniform TerrainSectors {
    TerrainSector sec[512];
} s;

/* Sector flag bits — keep in sync with openpete_terrain_cache.h OPTC_SF_* */
#define SF_DRAWN      1u
#define SF_DRAWHP     2u
#define SF_DRAWLP     4u
#define SF_LPFARONLY  8u
#define SF_LPGAPONLY  16u
#define SF_FILLLP     32u
#define SF_FILLNEAR   64u
#define SF_SECINLOD   128u
#define SF_LPGRADE    256u

/* face_flags bits — keep in sync with rhi_terrain_vertex (rhi.h) */
#define FF_SLOT_MASK  0x1FFu
#define FF_TIER_LP    (1u << 9)
#define FF_FOGBIAS    (1u << 10)
#define FF_GAPFILL    (1u << 11)
#define FF_DSIDE      (1u << 12)
#define FF_FLIP       (1u << 13)
#define FF_QUAD       (1u << 14)
#define FF_W3_40      (1u << 15)
#define FF_W3_80      (1u << 16)
#define FF_HQELIG     (1u << 17)
#define FF_FLOOR      (1u << 18)
/* admit: bits 19-23; otbias: bits 24-28 */

/* Depth encoding + PSX-affine interpolation — the scene stages' spec
 * constants (psx_scene.vert), captured at pipeline creation. */
layout(constant_id = 0) const bool DEPTH_AFFINE = false;
layout(constant_id = 1) const bool AFFINE_UV = false;

layout(location = 0)      out vec2  vUV;    /* pre-multiplied by q (scene contract) */
layout(location = 1)      out vec4  vColor; /* pre-multiplied by q                  */
layout(location = 2) flat out uvec2 vMat;   /* x: mat0, y: clx | cly<<16 (post-ladder) */
layout(location = 3) flat out float vKoff;  /* og OT key (feed koff mirror)         */
layout(location = 4)      out float vQ;     /* q (frag divides UV/colour by it)     */
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

/* proj_screen mirror: float sx = (float)(ofx + m1*H/z). Unclaimed sectors
 * guarantee m3 > 0 (the CPU guard z>1e-9 never fires there). */
precise vec2 proj_of(dvec3 m) {
    precise double sx = g.dof_xy.x + m.x * g.dH.x / m.z;
    precise double sy = g.dof_xy.y + m.y * g.dH.x / m.z;
    return vec2(float(sx), float(sy));
}

/* GTE_SAT mirror (double of the FLOAT screen coord, like the CPU's
 * NSAT(sv.sx)). */
double nsat(float v, bool unr_ovf) {
    if (unr_ovf) return double(v);
    return v < -1024.0 ? -1024.0lf : (v > 1023.0 ? 1023.0lf : double(v));
}

/* ngte_dpcs_ch mirror (GTE-exact DPCS fog, one channel; pure int math). */
int dpcs_ch(int c8, int fc12, int ir0) {
    int mac = c8 << 16;
    int ird = clamp(((fc12 << 12) - mac) >> 12, -32768, 32767);
    int o = ((mac + ir0 * ird) >> 12) >> 4;
    return clamp(o, 0, 255);
}

/* face_is_near_canonical mirror (hqscale 4.0; per-corner saturation at 0).
 * Used both for the HQ variant select (via face_is_near = this, or the
 * NOLOD short-circuit) and for the koff near-band/otslots-zero verdict
 * (ALWAYS this canonical form, even under NOLOD). */
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

void kill_vert() {
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);   /* degenerate: no raster */
    vUV = vec2(0.0); vColor = vec4(0.0); vMat = uvec2(0u);
    vKoff = 0.0; vQ = 1.0;
}

/* ---- portal inner-rim cull (portal_rim_classify mirror) --------------------
 * The CPU feed classifies every emitted tri against the active portal
 * opening planes and DROPS accepted rim faces — they lie in the opening
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
/* True = some plane accepts the tri as inner rim (classify flags == 0): the
 * CPU feed drops it, so the retained draw must too. Inputs are the emitted
 * tri's FLOAT screen coords (the feed ttri's sx/sy) and walk view-Z (m3). */
bool rim_classified(vec2 s0, vec2 s1, vec2 s2, double z0, double z1, double z2) {
    precise double ofx = g.rim_oxyH.x, ofy = g.rim_oxyH.y, H = g.rim_oxyH.z;
    if (H < 1.0lf) return false;
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
        return true;                                 /* all terms pass */
    }
    return false;
}

/* ---- blended-face portal-cover drop (portal_sky_covers mirror) ------------
 * The CPU feed drops any BLENDED emitted tri whose screen centroid sits
 * inside an active opening's outline polygon with the tri's mean view-Z at
 * or behind the opening plane minus the cover margin — the destination-sky
 * preview repaints that whole area, and a blended face cannot be depth-
 * punched out of it the way opaque cover is. Same test here on the rebuilt
 * emitted tri: float32 arithmetic exactly where the CPU's is float (the
 * ttri stores FLOAT screen coords but DOUBLE view-Z; `precise` blocks FMA
 * contraction), doubles exactly where the CPU computes double. */
bool cover_dropped(vec2 s0, vec2 s1, vec2 s2, double z0, double z1, double z2) {
    precise float cx = (s0.x + s1.x + s2.x) / 3.0;
    precise float cy = (s0.y + s1.y + s2.y) / 3.0;
    precise double vzsum = z0 + z1 + z2;
    for (int pi = 0; pi < g.cov_cnt.x; pi++) {
        int pn = int(g.cov_abc[pi].w);
        if (pn < 3) continue;
        vec4 bb = g.cov_bbox[pi];
        if (cx < bb.x || cx > bb.z || cy < bb.y || cy > bb.w) continue;
        bool inp = false;                            /* crossing test, float */
        int base = pi * 16;
        for (int i = 0, j = pn - 1; i < pn; j = i++) {
            precise vec2 P = g.cov_poly[base + i].xy;
            precise vec2 Q = g.cov_poly[base + j].xy;
            if ((P.y > cy) != (Q.y > cy) &&
                cx < (Q.x - P.x) * (cy - P.y) / (Q.y - P.y) + P.x)
                inp = !inp;
        }
        if (!inp) continue;
        precise double inv = g.cov_abc[pi].x * double(cx)
                           + g.cov_abc[pi].y * double(cy) + g.cov_abc[pi].z;
        if (inv <= 1e-9lf) continue;
        precise double mean = vzsum * 16.0lf / 3.0lf;
        if (mean >= 1.0lf / inv - g.cov_margin.x) return true;
    }
    return false;
}

void main()
{
    uint ff   = inFaceFlags;
    uint slot = ff & FF_SLOT_MASK;
    uint sf   = floatBitsToUint(s.sec[slot].o.w);
    int  gf   = g.fog.y;
    bool lp   = (ff & FF_TIER_LP) != 0u;
    bool quad = (ff & FF_QUAD) != 0u;
    uint f16  = uint(inPos.w);
    bool hqv  = (f16 & 1u) != 0u;             /* HQ-subdivide variant vertex */
    int  subq = int((f16 >> 1) & 3u);         /* HQ sub-quad index           */
    int  role = int((f16 >> 3) & 3u);         /* corner slot in the sub-quad */
    bool fillPass = (pc.pass_sel == 1);
    bool keyPass  = (pc.pass_sel == 2);

    if ((sf & SF_DRAWN) == 0u) { kill_vert(); return; }
    /* Chunk generation vs the published slot generation: a rebuilt sector's
     * old chunk keeps drawing for presents whose (lagged) state still names
     * the old generation; the new chunk waits its turn. */
    if (((ff >> 29) & 7u) != ((sf >> 12) & 7u)) { kill_vert(); return; }

    /* ---- pass / tier admission (sector-level, CPU-computed) ---- */
    if (fillPass) {
        if (!lp || (sf & SF_FILLLP) == 0u) { kill_vert(); return; }
    } else {
        if (lp  && (sf & SF_DRAWLP) == 0u) { kill_vert(); return; }
        if (!lp && (sf & SF_DRAWHP) == 0u) { kill_vert(); return; }
    }

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

    /* ---- NCLIP (both halves; the near verdict gates on half 1) ----
     * Cross products run on DOUBLES OF THE FLOAT screen coords (the CPU's
     * NSAT(sv.sx)); the GTE [-1024,1023] saturation is waived in the UNR
     * divide-overflow regime (any corner m3 < H/8 — the .s's own SXY there
     * is pinned garbage, so the TRUE facing is used). */
    vec2 p0f = proj_of(m0), p1f = proj_of(m1), p2f = proj_of(m2), p3f = proj_of(m3c);
    bool unr_ovf = (m0.z < g.dH.x / 8.0lf) || (m1.z < g.dH.x / 8.0lf) ||
                   (m2.z < g.dH.x / 8.0lf) || (quad && m3c.z < g.dH.x / 8.0lf);
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
         * dside (w3 & 0x4) jumps straight to emit. */
        double nxA = quad ? nx3 : nx2, nyA = quad ? ny3 : ny2;
        precise double n1 = (nx1 - nx0) * (nyA - ny0) - (nxA - nx0) * (ny1 - ny0);
        if (flip) n1 = -n1;
        front = n1 >= 0.0lf;
        if (quad) {
            precise double n2 = (nx1 - nx2) * (ny3 - ny2) - (nx3 - nx2) * (ny1 - ny2);
            front2 = ((ff & FF_DSIDE) != 0u) || (flip ? (n2 >= 0.0lf) : (n2 <= 0.0lf));
        }
    }

    /* ---- LP world-pass gates (gaponly / .s far cull / authored admission) ---- */
    bool gapfill_face = (gf & GF_GAPFILL) != 0 && (ff & FF_GAPFILL) != 0u;
    if (lp && !fillPass) {
        if ((sf & SF_LPGAPONLY) != 0u && (ff & FF_GAPFILL) == 0u) { kill_vert(); return; }
        double z3g = quad ? m3c.z : m2.z;
        double slotv = (m0.z + m1.z + m2.z + z3g) / 32.0lf;
        if ((gf & GF_NOLOD) == 0) {
            /* .s LP far cull: slot >= m_CullingDistance>>7 drops (all faces,
             * fill tags included). Skipped under extended LOD by design. */
            if (slotv >= double(g.fog.z)) { kill_vert(); return; }
        }
        if (!gapfill_face) {
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
         * LP_ADMIT=0, which forces the CPU walk — never mirrored here.) */
    }

    /* ---- LOD handoff: .s HP drop (sum of corner SZ3 >= m_LodDistance) ----
     * Runs BEFORE the near classify, exactly like the walk. */
    if (!lp && (gf & GF_HANDOFF) != 0) {
        double hpz3 = quad ? m3c.z : m2.z;
        precise double hpsum = (m0.z + m1.z + m2.z + hpz3) * fogscale;
        if (hpsum >= 4.0lf * double(g.fog.x)) { kill_vert(); return; }
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
     * The CPU gate is (front || straddle); straddling faces only exist in
     * CLAIMED sectors, so `front` alone is the unclaimed mirror. ---- */
    bool near = false;
    if (!lp && !fillPass && (gf & GF_NEARHQ) != 0 && quad && front && (ff & FF_HQELIG) != 0u) {
        if ((gf & GF_NOLOD) != 0) {
            /* --no-lod: HQ pinned ON for every eligible face; only the
             * behind-camera guard and the w3&0x80 exclusion remain (the
             * latter is folded into FF_HQELIG at pack time). */
            double sz = max(m0.z, 0.0lf) + max(m1.z, 0.0lf) + max(m2.z, 0.0lf) + max(m3c.z, 0.0lf);
            near = sz > 0.0lf;
        } else {
            near = near_canonical(m0.z, m1.z, m2.z, m3c.z,
                                  ff & FF_W3_40, ff & FF_W3_80);
        }
    }
    /* Exactly one variant of an HQ-eligible face survives. */
    if (hqv != near) { kill_vert(); return; }

    /* ---- NCLIP back-face cull (flat faces; near faces dispatched before
     * the cull in the walk; fill is emit-all by design) ---- */
    if (!near && !fillPass) {
        bool cullable = lp ? ((gf & GF_CULLLP) != 0) : ((gf & GF_CULLHP) != 0);
        if (cullable && !front && !front2) { kill_vert(); return; }
    }

    /* ---- portal drops: rebuild THIS vertex's emitted tri and run the two
     * feed-side portal classifiers on it — the inner-rim cull (all faces)
     * and the blended-face transparent-cover drop (flags16 bit 14).
     * Triangulation matches the bake exactly — flat LP (i0,i1,i2)+
     * (i1,i2,i3), flat HP (i0,i1,i3)+(i1,i3,i2), HQ sub-quad (c0,c1,c2)+
     * (c1,c2,c3) — in the walk's own emit corner ORDER ((tl,tr,bl)+
     * (tr,bl,br)), which the cover centroid's float sums need for
     * last-ulp identity; the rim classifier's terms are additionally
     * vertex-SET invariant. Screen coords are the raw float projections
     * (the feed's push_tri values; vertex-snap runs excluded the cache
     * entirely). ---- */
    bool covTest = (f16 & 0x4000u) != 0u && g.cov_cnt.x > 0;
    if (g.rim_cnt.x > 0 || covTest) {
        bool tri2 = (f16 & 0x2000u) != 0u;
        bool pdrop;
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
            pdrop = (g.rim_cnt.x > 0 &&
                     rim_classified(sa, sb, sc, ta.z, tb.z, tc2.z))
                 || (covTest &&
                     cover_dropped(sa, sb, sc, ta.z, tb.z, tc2.z));
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
            if (tri2) { ta = q1; tb = q2; tc2 = q3; }
            else      { ta = q0; tb = q1; tc2 = q2; }
            vec2 sa = proj_of(ta), sb = proj_of(tb), sc = proj_of(tc2);
            pdrop = (g.rim_cnt.x > 0 &&
                     rim_classified(sa, sb, sc, ta.z, tb.z, tc2.z))
                 || (covTest &&
                     cover_dropped(sa, sb, sc, ta.z, tb.z, tc2.z));
        }
        if (pdrop) { kill_vert(); return; }
    }

    /* ---- material: near-tile / CLUT-ladder row select (flat HP textured) ---- */
    uint mat0 = inMat.x;
    uint clut = inMat.y & 0xFFFFu;
    vec2 uv   = vec2(float(inUV.x), float(inUV.y));
    uint dbg_half = 0u, dbg_delta = 0u;
    int  dbg_t0k  = -99999;   /* tint mode 6: exact ladder key (t0k) probe */
    bool ladderFace = false;
    if (!lp && !hqv && (gf & GF_CLUTFOG) != 0 && (sf & SF_SECINLOD) != 0u
        && (mat0 & (1u << 23)) != 0u) {
        ladderFace = true;
        /* per-corner int casts, then sum — the walk's exact quantisation */
        int szsum = int(m0.z * fogscale) + int(m1.z * fogscale)
                  + int(m2.z * fogscale) + int(m3c.z * fogscale);
        int key = (szsum >> 7) << 5;
        int t0k = key - fogRef + 4096;
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
     * fade on the published ramp; fogbias shifts it — ff bit 10). */
    vHd = 0u;
    if (g.hd_lad.w != 0.0 && (mat0 & (1u << 23)) != 0u) {
        uint hid = (dbg_half == 1u ? (inMat.w >> 16) : (inMat.y >> 16)) & 0xFFFFu;
        if (hid != 0u)
            vHd = hid | (ladderFace ? 0x10000u : 0u)
                      | (((ff >> 10) & 1u) << 17);
    }
    /* env UV-scroll: per-tile fractional V offset for THIS build. Flat
     * faces: the LQ dv (both LOD halves share one offset — the game writes
     * the same scroll byte into all four V slots); HQ sub-quads: the
     * per-half dv (entries 1-2 vs 3-4 sit half a window apart in the
     * scroll ring). Zero on canonical builds. */
    if ((f16 & 0x1000u) != 0u) {
        int stile = int((f16 >> 5) & 0x7Fu);
        uv.y += hqv ? g.sdv[stile][(subq >> 1) + 1] : g.sdv[stile].x;
    }
    vMat = uvec2(mat0, (clut & 0x3Fu) * 16u | (((clut >> 6) & 0x1FFu) << 16));
    if ((gf & (1 << 29)) != 0) {               /* debug tint: ladder verdict */
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
     * parent corners (cv_mid mirrors — midpoints averaged in camera space). */
    precise dvec3 mo;
    if (!hqv) {
#ifdef TERRAIN_ANIM
        precise dvec3 lown = ANIM_LERP3(inPrevPos.xyz, inPos.xyz);
#else
        dvec3 lown = dvec3(inPos.xyz) * 0.25lf;
#endif
        mo = mac_of(lown, sdel);
    } else {
        /* Parent corners in .s grid order: TL=i0, TR=i1, BL=i3, BR=i2. */
        precise dvec3 TL = m0, TR = m1, BL = m3c, BR = m2;
        precise dvec3 mT = (TL + TR) * 0.5lf, mB = (BL + BR) * 0.5lf;
        precise dvec3 mL = (TL + BL) * 0.5lf, mR = (TR + BR) * 0.5lf;
        precise dvec3 C  = (mT + mB) * 0.5lf;
        /* sq[q] = {TL,mT,mL,C} {mT,TR,C,mR} {mL,C,BL,mB} {C,mR,mB,BR}
         * (node ids: 0=TL 1=TR 2=BR 3=BL; 4..7 = mT,mB,mL,mR; 8 = C) */
        const int nodemap[4][4] = int[4][4](int[4](0, 4, 6, 8),
                                            int[4](4, 1, 8, 7),
                                            int[4](6, 8, 3, 5),
                                            int[4](8, 7, 5, 2));
        int node = nodemap[subq][role];
        mo = node == 0 ? TL : node == 1 ? TR : node == 2 ? BR : node == 3 ? BL
           : node == 4 ? mT : node == 5 ? mB : node == 6 ? mL : node == 7 ? mR : C;
    }

    /* ---- og OT key (feed koff mirror; ambient per face / per HQ sub-quad) ---- */
    float koff = 0.0;
    if ((gf & GF_KEYON) != 0 && !fillPass) {
        /* otkey: the parent face's 4-corner view-Z sum x4 (tris sum the
         * shared i2==i3 corner twice, like the .s); the HQ subdivide
         * overrides with the SUB-QUAD's own corner sum (portal-granularity
         * bucketing). Rounded to float exactly where the walk rounds. */
        float ok;
        if (!hqv) {
            ok = float((m0.z + m1.z + m2.z + m3c.z) * 4.0lf);
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
            ok = float((sq.x + sq.y + sq.z + sq.w) * 4.0lf);
        }
        /* Foreground (near-band) faces carry no authored OT slot: the .s
         * defers them (DrawPrim, mask-protected, no OT index). ALWAYS the
         * canonical classifier here (LP tests the distance band alone —
         * word3 = 0), independent of the NOLOD HQ pin. */
        bool nearband = near_canonical(m0.z, m1.z, m2.z, quad ? m3c.z : m2.z,
                                       lp ? 0u : (ff & FF_W3_40),
                                       lp ? 0u : (ff & FF_W3_80));
        if (nearband) {
            /* contest-only (negative) for opaque; blended near-band is out */
            if (((mat0 >> 21) & 3u) == 0u)
                koff = -float(double(ok) + 0.5lf);
        } else {
            double slots = lp ? double(0x40u + 8u * ((ff >> 24) & 0x1Fu))
                              : double(4u * ((ff >> 24) & 0x1Fu));
            koff = float(double(ok) + 128.0lf * slots + 0.5lf);
        }
    }
    vKoff = koff;

    /* ---- KEY pass: rasterise the flat og key as depth (terrain_key.vert
     * mirror, including the CPU path's z round-trip) ---- */
    if (keyPass) {
        if (koff <= 0.25) { kill_vert(); return; }   /* keyed faces only */
        float K  = koff - 0.5;
        float zk = clamp(K / (K + 4096.0), 0.0, 0.9999);
        float kv = max(4096.0 * zk / max(1.0 - zk, 1e-6), 1.0);
        vec2 cxy = clip_of(proj_of(mo));
        gl_Position = vec4(cxy * kv, kv * kv / (kv + 4096.0), kv);
        float fvz = float(mo.z) * 16.0;              /* TRUE per-vertex view-Z */
        float wq  = AFFINE_UV ? 1.0 : max(fvz, 1.0);
        vUV = uv / wq;
        vQ  = 1.0 / wq;
        vColor = vec4(1.0);
        return;
    }

    /* ---- colour: GTE-exact DPCS fog (flat HP); baked raw otherwise ---- */
    vec4 col;
    if (!lp && !hqv) {
        int ir0 = fogRef - int(mo.z * fogscale);
        if (ir0 >= 4096) {
            col = vec4(inColNear.rgb, 1.0);
        } else if (ir0 <= 0) {
            col = vec4(inColFar.rgb, 1.0);
        } else {
            ivec3 f8 = ivec3(round(inColFar.rgb  * 255.0));
            ivec3 n8 = ivec3(round(inColNear.rgb * 255.0));
            col = vec4(float(dpcs_ch(f8.r, n8.r << 4, ir0)) / 255.0,
                       float(dpcs_ch(f8.g, n8.g << 4, ir0)) / 255.0,
                       float(dpcs_ch(f8.b, n8.b << 4, ir0)) / 255.0, 1.0);
        }
    } else {
        col = vec4(inColFar.rgb, 1.0);   /* LP / HQ: raw baked, never fogged */
    }
    /* Pack-aware LP colour grade (lp_grade_apply mirror): the bake carries
     * raw authored colours; with a replacement pack active the sector's
     * grade affine (own HP-texture fit, or the level mean) rides the
     * sector params. Same float32 ops and trunc-after-+0.5 as the walk;
     * WORLD LP and fill LP alike, never HQ (the walk grades passLP only). */
    if (lp && (sf & SF_LPGRADE) != 0u) {
        for (int c = 0; c < 3; c++) {
            precise float t = s.sec[slot].gp[c] * round(col[c] * 255.0);
            t = t + s.sec[slot].gq[c] * 255.0;
            t = t + 0.5;
            col[c] = float(clamp(int(t), 0, 255)) / 255.0;
        }
    }
    {   /* debug tint modes 4/5: show the baked near / far colour raw */
        int dbgmode = (gf >> 26) & 7;
        if ((gf & (1 << 29)) != 0 && dbgmode == 4) col = vec4(inColNear.rgb, 1.0);
        if ((gf & (1 << 29)) != 0 && dbgmode == 5) col = vec4(inColFar.rgb, 1.0);
    }

    /* ---- position: the feed + psx_scene.vert contract, exactly ----
     * feed: z = vz/(vz+4096) with vz = float(m3)*16 (near-guarded at 1);
     * scene VS: vzo decoded back from z, q = vzo/Wu (Wu = the same vz), and
     * the (zc, W) pair uses W == vzo for clip invariance. The round-trip is
     * kept so window depth and q match the CPU-fed pipeline bit-for-bit. */
    float fvz = float(mo.z) * 16.0;
    float vzs = max(fvz, 1.0);
    float z   = vzs / (vzs + 4096.0);
    float zc2 = clamp(z, 0.0, 0.9999);
    float vzo = max(4096.0 * zc2 / max(1.0 - zc2, 1e-6), 1.0);
    float Wu  = vzs;                       /* inW = pvz = the same view-Z */
    float q   = AFFINE_UV ? 1.0 : (vzo / Wu);
    vec2 cxy  = clip_of(proj_of(mo));
    float zcl = DEPTH_AFFINE ? (vzo - 1.0) : (zc2 * vzo);
    gl_Position = AFFINE_UV
        ? vec4(cxy, DEPTH_AFFINE ? (vzo - 1.0) / vzo : zc2, 1.0)
        : vec4(cxy * vzo, zcl, vzo);
    vUV    = uv * q;
    vColor = col * q;
    vQ     = q;
}
