/* portal_cover.glsl - the portal plane-cover test for one fragment.
 *
 * Inside a portal opening's footprint the destination preview covers
 * everything at or behind the opening plane minus the cover margin M. Opaque
 * terrain gets that from the depth test against the preview's own depth;
 * blended terrain draws after the preview and would composite over it, so its
 * covered fragments are discarded here instead. The test runs per fragment,
 * so a blended face that straddles the opening's outline or its plane keeps
 * exactly its uncovered part.
 *
 * The including stage defines OP_COVER_SRC as the instance name of a uniform
 * block holding, in this order and std140 layout:
 *   ivec4 cov_cnt      x = plane count (0 = test off); y = 1 when the
 *                      blended-terrain cover is on
 *   vec4  cov_margin   x = M in world units
 *   vec4  cov_abc[6]   the plane's screen-space field 1/vz = a x + b y + c; w = outline point count
 *   vec4  cov_bbox[6]  the outline's screen bounding box x0, y0, x1, y1
 *   vec4  cov_poly[96] the outline points (x, y, 0, 0), 16 per plane
 * Every read converts through float(...), so a block may also declare
 * cov_margin and cov_abc as dvec4 and the test still computes in float.
 * Screen coordinates are the scene feed's PSX screen space. */

#ifndef OPENPETE_PORTAL_COVER_GLSL
#define OPENPETE_PORTAL_COVER_GLSL

/* The margin of the inner-rim discard: a face the terrain feed accepts as a
 * portal's inner rim lies within the rim classifier's band about the
 * opening plane, so its fragments are discarded inside that plane's outline
 * at any depth. */
#define OP_PORTAL_RIM_MARGIN 1.0e30

/* Whether a fragment at PSX screen point p with view-Z vz (full-world units)
 * lies inside an opening's outline at or behind its plane minus m. only < 0
 * tests every plane; only >= 0 tests plane `only` alone (an inner-rim face
 * is discarded only inside the outline of the plane that accepted it). The
 * outline containment is the crossing rule; a plane whose field is not
 * positive at p (the plane behind the camera there) covers nothing. */
bool op_portal_covered(vec2 p, float vz, float m, int only)
{
    const int lo = only < 0 ? 0 : only;
    const int hi = only < 0 ? OP_COVER_SRC.cov_cnt.x : min(only + 1, OP_COVER_SRC.cov_cnt.x);
    for (int pi = lo; pi < hi; pi++) {
        int pn = int(OP_COVER_SRC.cov_abc[pi].w);
        if (pn < 3) continue;
        vec4 bb = OP_COVER_SRC.cov_bbox[pi];
        if (p.x < bb.x || p.x > bb.z || p.y < bb.y || p.y > bb.w) continue;
        bool inp = false;
        int base = pi * 16;
        for (int i = 0, j = pn - 1; i < pn; j = i++) {
            vec2 P = OP_COVER_SRC.cov_poly[base + i].xy;
            vec2 Q = OP_COVER_SRC.cov_poly[base + j].xy;
            if ((P.y > p.y) != (Q.y > p.y) &&
                p.x < (Q.x - P.x) * (p.y - P.y) / (Q.y - P.y) + P.x)
                inp = !inp;
        }
        if (!inp) continue;
        float inv = float(OP_COVER_SRC.cov_abc[pi].x) * p.x
                  + float(OP_COVER_SRC.cov_abc[pi].y) * p.y + float(OP_COVER_SRC.cov_abc[pi].z);
        if (inv <= 1e-9) continue;
        if (vz >= 1.0 / inv - m) return true;
    }
    return false;
}

#endif
