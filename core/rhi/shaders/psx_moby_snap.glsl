/* psx_moby_snap.glsl - the sunk-pickup depth snap of a moby fragment.
 *
 * The game inserts every face of a moby m_DepthOffset ordering-table slots
 * (128 world each) nearer than its average depth, so a pickup set into the
 * ground paints whole over the ground it sits in. A true depth test cuts it
 * at the ground plane. The moby feeder finds, per present, the drawn opaque
 * terrain triangles each pulled moby intersects and merges them into contact
 * planes (camera space: the PSX screen unprojected at view-Z with the walk's
 * H, ofx and ofy). A fragment of that moby lying behind a contact plane,
 * within its slab (the moby's own penetration, at most its pull) and within
 * its disc (the sunk part projected onto the plane), writes a depth just in
 * front of the plane along its ray. It then beats the plane's triangles and
 * whatever lies within the slab behind them, and nothing in front of the
 * plane: a wall, crest or ledge the moby does not touch keeps the pixel by
 * true depth. Seen from the far side of a double-sided contact face, the
 * sunk part (now nearer the eye) is written just behind the plane instead,
 * so the face hides it.
 *
 * Order among snapped fragments on one ray follows their depth past the
 * plane: t = -s / S, s the signed distance past the plane in [-slab, 0) and
 * S the largest slab of the present, so a deeper fragment writes a farther
 * depth in [z_c - 2 eps, z_c - eps] (and in [z_c + eps, z_c + 2 eps] on the
 * hide side) for every moby alike. An unsunk fragment of the moby (inside the
 * disc, on the moby's side) whose point lies within 4 eps of the plane along
 * the ray is compressed onto [z_c - 4 eps, z_c - 2 eps) (on the hide side
 * (z_c + 2 eps, z_c + 4 eps]): the map is monotone in ray depth across the
 * plane, so a fragment of the moby nearer than another before the snap stays
 * nearer after it (min(z_f, g_f) < min(z_b, z_c - 2 eps) when z_f < z_b), and
 * a facet resting on the plane is not hidden by the moby's own sunk faces.
 * The fragment's point is rebuilt from its
 * raster depth plus the moby's offset off_m (the tie pull less the
 * near-floor lift), the key depth its contact geometry was built at.
 *
 * Table (set 7 binding 0, uvec4 words, written per compose target):
 *   [0] gl_FragCoord -> PSX screen affine fax, fbx, fay, fby (float bits)
 *   [1] H, ofx, ofy, S                                       (float bits)
 *   [2] entry count, plane count
 *   [3 .. 3+ne) entries sorted by mid: mid, mordinal, off_m bits,
 *               plane offset | plane count << 16
 *   then 3 words per plane: (n.xyz, D), (side * slab, disc centre.xyz),
 *               (disc radius, flags: bit 0 double-sided, bit 1 the eye on
 *               the centre side, 0, 0)
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

/* The snap margin at plane view-Z zc: half a world unit growing with
 * distance, and at least four depth quanta of the attachment (reversed D32
 * 1/vz: 6e-8 zc; forward D24 1 - 1/vz: 6e-8 zc^2). */
float op_snap_eps(float zc, bool reversed)
{
    float q = reversed ? 6e-8 * zc : 6e-8 * zc * zc;
    return max(0.5 + 2e-5 * zc, 4.0 * q);
}

/* The window depth a moby fragment writes after the snap. zout = the depth
 * the stage writes otherwise (raster, shell and tie chain); rank = its rank
 * pair; reversed = the scene's depth convention. */
float op_snap_depth(float zout, vec2 rank, bool reversed)
{
    int e = op_snap_entry(rank);
    if (e < 0) return zout;
    uvec4 E = msnap.snp[3 + e];
    vec4 fa = uintBitsToFloat(msnap.snp[0]);
    vec4 hv = uintBitsToFloat(msnap.snp[1]);
    vec2 scr = vec2(fa.x * gl_FragCoord.x + fa.y, fa.z * gl_FragCoord.y + fa.w);
    vec3 ray = vec3((scr.x - hv.y) / hv.x, (scr.y - hv.z) / hv.x, 1.0);
    vec3 p = ray * (op_vz_from_depth(gl_FragCoord.z, reversed) + uintBitsToFloat(E.z));
    float S = max(hv.w, 1.0);
    uint base = 3u + msnap.snp[2].x + 3u * (E.w & 0xFFFFu);
    uint np = E.w >> 16;
    float vz = op_vz_from_depth(zout, reversed);
    float rev = 3.0e38, hid = -1.0;
    for (uint k = 0u; k < np; k++) {
        vec4 a = uintBitsToFloat(msnap.snp[base + 3u * k]);
        vec4 b = uintBitsToFloat(msnap.snp[base + 3u * k + 1u]);
        uvec4 c = msnap.snp[base + 3u * k + 2u];
        float sd = dot(a.xyz, p) - a.w;
        float s = b.x < 0.0 ? -sd : sd;          /* side * (n.p - D) */
        if (s < -abs(b.x)) continue;
        if (length(p - sd * a.xyz - b.yzw) > uintBitsToFloat(c.x)) continue;
        float nr = dot(a.xyz, ray);
        if (abs(nr) < 1e-9) continue;
        float zc = a.w / nr;
        if (zc <= 1.0) continue;
        float eps = op_snap_eps(zc, reversed);
        if (s >= 0.0) {
            /* Unsunk, within 4 eps of the plane along the ray: compressed
             * onto the half of that band away from the plane, so it stays
             * on its side of every snapped fragment of the moby. */
            if ((c.y & 2u) != 0u) {
                if (p.z < zc && p.z > zc - 4.0 * eps)
                    rev = min(rev, zc - 4.0 * eps + 0.5 * (p.z - (zc - 4.0 * eps)));
            } else if ((c.y & 1u) != 0u) {
                if (p.z > zc && p.z < zc + 4.0 * eps)
                    hid = max(hid, zc + 4.0 * eps - 0.5 * (zc + 4.0 * eps - p.z));
            }
            continue;
        }
        float t = -s / S;
        if ((c.y & 2u) != 0u) rev = min(rev, zc - eps * (2.0 - t));
        else if ((c.y & 1u) != 0u) hid = max(hid, zc + eps * (2.0 - t));
    }
    if (!(rev < vz) && !(hid > vz)) return zout;   /* no plane moves it */
    if (rev < 3.0e38) vz = min(vz, rev);
    if (hid > 0.0) vz = max(vz, hid);
    vz = max(vz, 1.0);
    return reversed ? 1.0 / vz : 1.0 - 1.0 / vz;
}

#endif
