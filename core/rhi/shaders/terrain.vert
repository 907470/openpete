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

layout(push_constant) uniform TerrainPC {
    vec4 clip_affine;   /* x_clip = .x*sx + .y ; y_clip = .z*sy + .w      */
    int  pass_sel;      /* 0 = WORLD, 1 = FILL (LP hole-fill), 2 = KEY    */
    int  pad0, pad1, pad2;
} pc;

layout(std140, set = 3, binding = 0) uniform TerrainGlobals {
    vec4  mrow0;    /* xyz: M[0][*] (raw int16 values)                    */
    vec4  mrow1;    /* xyz: M[1][*]                                       */
    vec4  mrow2;    /* xyz: M[2][*]                                       */
    dvec2 dcam_xy;  /* camx, camy (fractional, world/16)                  */
    dvec2 dcam_zf;  /* camz, s_fogscale                                   */
    dvec2 dof_xy;   /* ofx, ofy (GTE screen offset, px)                   */
    dvec2 dH;       /* x: H (projection distance), y: unused              */
    ivec4 fog;      /* x: fogRefBase, y: gflags, z: cullRefS2, w: unused  */
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
    dvec3 lc0 = dvec3(inC0.xyz) * 0.25lf;
    dvec3 lc1 = dvec3(double(inC0.w), double(inC1.x), double(inC1.y)) * 0.25lf;
    dvec3 lc2 = dvec3(double(inC1.z), double(inC1.w), double(inC2.x)) * 0.25lf;
    dvec3 lc3 = dvec3(inC2.yzw) * 0.25lf;
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

    /* ---- material: near-tile / CLUT-ladder row select (flat HP textured) ---- */
    uint mat0 = inMat.x;
    uint clut = inMat.y & 0xFFFFu;
    vec2 uv   = vec2(float(inUV.x), float(inUV.y));
    uint dbg_half = 0u, dbg_delta = 0u;
    if (!lp && !hqv && (gf & GF_CLUTFOG) != 0 && (sf & SF_SECINLOD) != 0u
        && (mat0 & (1u << 23)) != 0u) {
        /* per-corner int casts, then sum — the walk's exact quantisation */
        int szsum = int(m0.z * fogscale) + int(m1.z * fogscale)
                  + int(m2.z * fogscale) + int(m3c.z * fogscale);
        int key = (szsum >> 7) << 5;
        int t0k = key - fogRef + 4096;
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
    vMat = uvec2(mat0, (clut & 0x3Fu) * 16u | (((clut >> 6) & 0x1FFu) << 16));
    if ((gf & (1 << 29)) != 0)                 /* debug tint: ladder verdict */
        vMat.y |= (dbg_half << 30) | (dbg_delta << 25);

    /* ---- own vertex camera-space value ----
     * Flat/LP verts: transform own position (identical ops to the corner
     * transforms). HQ verts: rebuild the sub-quad node from the TRANSFORMED
     * parent corners (cv_mid mirrors — midpoints averaged in camera space). */
    precise dvec3 mo;
    if (!hqv) {
        dvec3 lown = dvec3(inPos.xyz) * 0.25lf;
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
        if ((gf & (1 << 30)) != 0) { kill_vert(); return; }  /* debug: key emit off */
        if (koff <= 0.25) { kill_vert(); return; }   /* keyed faces only */
        float K  = koff - 0.5;
        float zk = clamp(K / (K + 4096.0), 0.0, 0.9999);
        float kv = max(4096.0 * zk / max(1.0 - zk, 1e-6), 1.0);
        vec2 sxy = proj_of(mo);
        vec2 cxy = vec2(pc.clip_affine.x * sxy.x + pc.clip_affine.y,
                        pc.clip_affine.z * sxy.y + pc.clip_affine.w);
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
    vec2 sxy  = proj_of(mo);
    vec2 cxy  = vec2(pc.clip_affine.x * sxy.x + pc.clip_affine.y,
                     pc.clip_affine.z * sxy.y + pc.clip_affine.w);
    float zcl = DEPTH_AFFINE ? (vzo - 1.0) : (zc2 * vzo);
    gl_Position = AFFINE_UV
        ? vec4(cxy, DEPTH_AFFINE ? (vzo - 1.0) / vzo : zc2, 1.0)
        : vec4(cxy * vzo, zcl, vzo);
    vUV    = uv * q;
    vColor = col * q;
    vQ     = q;
}
