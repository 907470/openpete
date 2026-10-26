/* psx_moby_snap.glsl - the sunk-pickup depth snap of a moby fragment.
 *
 * The game inserts every face of a moby m_DepthOffset ordering-table slots
 * (128 world each) nearer than its average depth, so a pickup set into the
 * ground paints whole over the ground it sits in. A true depth test cuts it
 * at the ground plane. The moby feeder finds, per present, the drawn opaque
 * terrain triangles each pulled moby intersects and merges them into contact
 * planes (camera space: the PSX screen unprojected at view-Z with the walk's
 * H, ofx and ofy), each with the side of the plane the moby stands on.
 *
 * A fragment of that moby snaps in front of the terrain at its pixel when
 * all of these hold for one of its planes:
 *   - it lies on the far side of the plane from the moby's side, at most
 *     the moby's pull (128 m_DepthOffset, the plane's slab) past it;
 *   - the eye is on the moby's side;
 *   - the terrain key owner at the pixel lies in the plane: its point
 *     T = ray z_T, z_T the owner's true view-Z from the terrain key image's
 *     colour (op_key_attr_z), has |n.T - D| <= 4 + q(z_T) |n.ray|, q the
 *     colour encoding's depth quantum at z_T;
 *   - the face's ordering-table bucket is strictly nearer than the bucket
 *     of the terrain key at the pixel. On all three games the moby pass
 *     inserts before the terrain pass and both append at a bucket's tail,
 *     so a shared bucket paints the terrain over the moby: a tie goes to
 *     terrain.
 * It then writes ref - eps (2 - t), ref = min(z_T, z_c), z_c the ray's
 * crossing of the plane, t = -s / S (s the signed distance past the plane,
 * S the largest slab of the present), so a deeper fragment writes a farther
 * depth in [ref - 2 eps, ref - eps] for every moby alike. Terrain the moby
 * does not rest on (a wall, a crest or a ledge out of its plane) and terrain
 * the game sorts over the moby keep the pixel by true depth.
 *
 * Under a double-sided contact face seen from the far side (the eye not on
 * the moby's side), the sunk part lies between the eye and the face; where
 * the owner at the pixel lies in the plane it writes ref + eps (2 - t),
 * ref = max(z_T, z_c), so the face hides the whole moby.
 *
 * An unsunk fragment of the moby whose point lies within 4 eps of ref along
 * the ray, at a pixel where the plane snaps, is compressed onto
 * [ref - 4 eps, ref - 2 eps) (the hidden side: (ref + 2 eps, ref + 4 eps]):
 * the map is monotone in ray depth across the plane, so a fragment of the
 * moby nearer than another before the snap stays nearer after it, and a
 * facet resting on the plane is not hidden by the moby's own sunk faces.
 * The fragment's point is rebuilt from its raster depth plus the moby's
 * offset off_m (the tie pull less the near-floor lift), the key depth its
 * contact geometry was built at.
 *
 * Table (set 7 binding 0, uvec4 words, written per compose target):
 *   [0] gl_FragCoord -> PSX screen affine fax, fbx, fay, fby (float bits)
 *   [1] H, ofx, ofy, S                                       (float bits)
 *   [2] entry count, plane count
 *   [3 .. 3+ne) entries sorted by mid: mid, mordinal, off_m bits,
 *               plane offset | plane count << 16
 *   then 2 words per plane: (n.xyz, D), (side +-1, slab, flags: bit 0
 *               double-sided, 0)
 * A face finds its entry by mid through its rank pair (unranked actor faces
 * carry the moby identity) or, ranked, by its moby ordinal. No entry, no
 * change. */

#ifndef OPENPETE_PSX_MOBY_SNAP_GLSL
#define OPENPETE_PSX_MOBY_SNAP_GLSL

layout(std430, set = 7, binding = 0) readonly buffer MobySnap {
    uvec4 snp[];
} msnap;

/* The table entry of a face by its rank pair, or -1. */
int op_snap_entry(vec2 rank)
{
    uint ne = msnap.snp[2].x;
    if (ne == 0u) return -1;
    if (rank.x > 0.5) {
        uint o = uint(rank.y + 0.5);
        for (uint i = 0u; i < ne; i++)
            if (msnap.snp[3u + i].y == o) return int(i);
        return -1;
    }
    if (rank.y < 0.5) return -1;
    uint mid = uint(rank.y + 0.5);
    int lo = 0, hi = int(ne) - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint v = msnap.snp[3 + m].x;
        if (v == mid) return m;
        if (v < mid) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

/* The snap margin at view-Z z: half a world unit growing with distance, and
 * at least four depth quanta of the attachment (reversed D32 1/vz: 6e-8 z;
 * forward D24 1 - 1/vz: 6e-8 z^2). */
float op_snap_eps(float z, bool reversed)
{
    float q = reversed ? 6e-8 * z : 6e-8 * z * z;
    return max(0.5 + 2e-5 * z, 4.0 * q);
}

/* Whether a terrain key owner at view-Z zt on a ray with n.ray = nr lies in
 * the plane n.p = d: within 4 world plus the owner colour's depth quantum
 * (op_key_attr_store: 24 bits of z/(z + 4096), dz = (z + 4096)^2 / (4096
 * (2^24 - 1))) projected on the normal. */
bool op_snap_coplanar(float zt, float nr, float d)
{
    float q = (zt + 4096.0) * (zt + 4096.0) / (4096.0 * 16777215.0);
    return abs(zt * nr - d) <= 4.0 + q * abs(nr);
}

/* The window depth a moby fragment writes after the snap. zout = the depth
 * the stage writes otherwise (raster, shell and tie chain); rank = its rank
 * pair; koff = its key channel (absolute key + 0.5, <= 0.25 unkeyed: never
 * snaps); tkey = the decoded terrain key at the pixel (op_key_from_stored);
 * tattr = the terrain key image's colour at the pixel (its owner's true
 * view-Z; class OP_KEY_CLASS_NONE: no owner, never snaps); reversed = the scene's depth convention. */
float op_snap_depth(float zout, vec2 rank, float koff, float tkey, vec4 tattr, bool reversed)
{
    int e = op_snap_entry(rank);
    if (e < 0 || tkey >= OP_KEY_NONE || op_key_attr_class(tattr) == OP_KEY_CLASS_NONE)
        return zout;
    float zt = op_key_attr_z(tattr);
    bool key = abs(koff) > 0.25 &&
               floor(tkey / OP_KEY_SLOT) > floor((abs(koff) - 0.5) / OP_KEY_SLOT);
    uvec4 E = msnap.snp[3 + e];
    vec4 fa = uintBitsToFloat(msnap.snp[0]);
    vec4 hv = uintBitsToFloat(msnap.snp[1]);
    vec2 scr = vec2(fa.x * gl_FragCoord.x + fa.y, fa.z * gl_FragCoord.y + fa.w);
    vec3 ray = vec3((scr.x - hv.y) / hv.x, (scr.y - hv.z) / hv.x, 1.0);
    vec3 p = ray * (op_vz_from_depth(gl_FragCoord.z, reversed) + uintBitsToFloat(E.z));
    float S = max(hv.w, 1.0);
    uint base = 3u + msnap.snp[2].x + 2u * (E.w & 0xFFFFu);
    uint np = E.w >> 16;
    float vz = op_vz_from_depth(zout, reversed);
    float rev = 3.0e38, hid = -1.0;
    for (uint k = 0u; k < np; k++) {
        vec4 a = uintBitsToFloat(msnap.snp[base + 2u * k]);
        vec4 b = uintBitsToFloat(msnap.snp[base + 2u * k + 1u]);
        float nr = dot(a.xyz, ray);
        if (abs(nr) < 1e-9) continue;
        float zc = a.w / nr;
        if (zc <= 1.0 || !op_snap_coplanar(zt, nr, a.w)) continue;
        float s = b.x * (dot(a.xyz, p) - a.w);    /* > 0 on the moby's side */
        if (s < -b.y) continue;
        if (b.x * -a.w > 0.0) {
            /* The eye on the moby's side: snap in front of the owner. */
            if (!key) continue;
            float ref = min(zt, zc);
            float eps = op_snap_eps(ref, reversed);
            if (s < 0.0) {
                rev = min(rev, ref - eps * (2.0 + s / S));
            } else if (p.z > ref - 4.0 * eps) {
                /* Unsunk, within 4 eps of ref: (ref - 4 eps, zc] maps onto
                 * [ref - 4 eps, ref - 2 eps), the half away from the plane. */
                float lo = ref - 4.0 * eps;
                rev = min(rev, lo + 2.0 * eps * (p.z - lo) / max(zc - lo, 4.0 * eps));
            }
        } else {
            /* The far side of a double-sided face: hide behind the owner. */
            float ref = max(zt, zc);
            float eps = op_snap_eps(ref, reversed);
            if (s < 0.0) {
                hid = max(hid, ref + eps * (2.0 + s / S));
            } else if (p.z < ref + 4.0 * eps) {
                float hi = ref + 4.0 * eps;
                hid = max(hid, hi - 2.0 * eps * (hi - p.z) / max(hi - zc, 4.0 * eps));
            }
        }
    }
    if (!(rev < vz) && !(hid > vz)) return zout;   /* no plane moves it */
    if (rev < 3.0e38) vz = min(vz, rev);
    if (hid > 0.0) vz = max(vz, hid);
    vz = max(vz, 1.0);
    return reversed ? 1.0 / vz : 1.0 - 1.0 / vz;
}

#endif
