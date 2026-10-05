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
 *   ivec4 cov_cnt      x = plane count (0 = test off)
 *   dvec4 cov_margin   x = M in world units
 *   dvec4 cov_abc[6]   the plane's screen-space field 1/vz = a x + b y + c; w = outline point count
 *   vec4  cov_bbox[6]  the outline's screen bounding box x0, y0, x1, y1
 *   vec4  cov_poly[96] the outline points (x, y, 0, 0), 16 per plane
 * Screen coordinates are the scene feed's PSX screen space. */

#ifndef OPENPETE_PORTAL_COVER_GLSL
#define OPENPETE_PORTAL_COVER_GLSL

/* Whether a fragment at PSX screen point p with view-Z vz (full-world units)
 * lies inside an opening's outline at or behind its plane minus M. The
 * outline containment is the crossing rule; a plane whose field is not
 * positive at p (the plane behind the camera there) covers nothing. */
bool op_portal_covered(vec2 p, float vz)
{
    for (int pi = 0; pi < OP_COVER_SRC.cov_cnt.x; pi++) {
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
        if (vz >= 1.0 / inv - float(OP_COVER_SRC.cov_margin.x)) return true;
    }
    return false;
}

#endif
