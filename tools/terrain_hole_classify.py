#!/usr/bin/env python3
"""Geometric attribution of 1-px terrain holes against the walk's triangle dump.

  terrain_hole_classify.py TRIDUMP HOLES [--out FILE]

TRIDUMP is an OPENPETE_TERRAIN_TRIDUMP file of the pose (a walk leg,
--terrain-cache 0, dumped at the capture's key); HOLES is the list
`terrain_hole_census.py holes --list` wrote for the pose's face-identity
capture. Each 1-px hole (a list line of size 1) is classified at its pixel
centre P. A capture pixel (x, y) of the 640-wide 4:3 image of height H
(448 for Spyro 1, 432 for Spyro 2 and 3) maps to dump coordinates
X = xlo + (x + 0.5) * (xhi - xlo) / 640, Y = oy + (y + 0.5) / 2, with
[xlo, xhi] the crop the dump's `# view` line names and oy = (240 - H/2) / 2
(the H/2 visible lines centred in 240: 8 at 448, 12 at 432; measured as the
offset at which the capture's identity boundaries lie on the dump's edges);
the classifier works in capture pixels, the dump's vertices mapped by the
inverse. Classes, first match wins:

  T   a T-junction: an edge (a, b) of a triangle passes within EDGE_D of P
      and a vertex m of another triangle lies on the segment (perpendicular
      distance under TV_D, 0.02 < t < 0.98, not at a or b). Sub-classes: Tq
      (m's face has more triangles than the edge's face, an HQ-subdivided
      face beside a coarser one, same sector), Ts (cross-sector), To (other
      same-sector).
  O   an opening: two triangle edges flank P within O_R px, on opposite
      sides of it, and their view depths at the points nearest P differ by
      at least O_Z units. The surfaces are that far apart along the sight
      line, so P sees past a silhouette and the background it shows is the
      correct image, not a crack. The nearest flanking pair decides.
  C   a crack: two edges of different triangles within EDGE_D of P whose
      endpoints pair up within CR_D but not within SAME: a shared edge whose
      vertex positions differ. Co same sector, Cs cross-sector.
  K   covered: P lies inside a walk triangle by more than IN_D from each of
      its edges, so the walk's geometry covers the pixel and the hole is in
      the retained cache's geometry.
  E   a raster tie on a watertight edge: P within TIE_D of an edge two
      triangles share within SAME, the two lying on opposite sides of it,
      and P's projection strictly inside the edge (0 < t < 1).
  S   an open edge: P within OPEN_D of an edge with no watertight partner
      (no other triangle shares it from the opposite side: a boundary, or a
      fold whose sharing triangle lies on the same side), and inside no
      triangle.
  U   unattributed.

Prints one line "x y class" per hole to --out (default: none), then the
class totals "T=n C=n K=n E=n S=n U=n" as the last stdout line. Exit 0; 1
when an input is missing or the list's capture is not 640 wide.
"""
import math
import sys
from collections import Counter, defaultdict

EDGE_D = 0.15
TV_D = 0.08
CR_D = 0.6
SAME = 0.03
TIE_D = 0.002
IN_D = 0.15
OPEN_D = 0.75
O_R = 1.5
O_Z = 24.0
CAP_W = 640
CLASSES = ("T", "O", "C", "K", "E", "S", "U")


def load_dump(path):
    tris, hdr = [], {}
    for line in open(path):
        if line.startswith("# view"):
            for kv in line[7:].split():
                k, v = kv.split("=")
                hdr[k] = float(v)
        if line.startswith("#"):
            continue
        f = line.split(",")
        v = ((float(f[7]), float(f[8])), (float(f[10]), float(f[11])),
             (float(f[13]), float(f[14])))
        tris.append({"sec": int(f[0]), "face": int(f[1]), "v": v,
                     "z": (float(f[9]) / 16.0, float(f[12]) / 16.0, float(f[15]) / 16.0)})
    return tris, hdr


def segd(p, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    l2 = dx * dx + dy * dy
    if l2 < 1e-12:
        return math.hypot(p[0] - a[0], p[1] - a[1]), 0.0
    t = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2
    tc = min(1.0, max(0.0, t))
    return math.hypot(p[0] - (a[0] + tc * dx), p[1] - (a[1] + tc * dy)), t


def d2(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


class Geo:
    """The dump's triangles in capture pixels, bucketed on an 8-px grid."""

    def __init__(self, path, cap_h):
        self.T, hdr = load_dump(path)
        oy = (240.0 - cap_h / 2.0) / 2.0
        lim_y = cap_h + 60
        sx = CAP_W / (hdr["xhi"] - hdr["xlo"])
        xlo = hdr["xlo"]
        self.grid = defaultdict(list)
        self.ntri = defaultdict(int)
        for i, t in enumerate(self.T):
            t["p"] = tuple(((x - xlo) * sx, (y - oy) * 2.0) for x, y in t["v"])
            self.ntri[(t["sec"], t["face"])] += 1
            xs = [p[0] for p in t["p"]]
            ys = [p[1] for p in t["p"]]
            if max(xs) < -50 or min(xs) > CAP_W + 60 or max(ys) < -50 or min(ys) > lim_y:
                continue
            for gx in range(int(max(-2, min(xs) - 2)) // 8, int(min(CAP_W + 60, max(xs) + 2)) // 8 + 1):
                for gy in range(int(max(-2, min(ys) - 2)) // 8, int(min(lim_y, max(ys) + 2)) // 8 + 1):
                    self.grid[(gx, gy)].append(i)

    def near(self, p, r=3):
        cand = set()
        for gx in range(int(p[0] - r) // 8, int(p[0] + r) // 8 + 1):
            for gy in range(int(p[1] - r) // 8, int(p[1] + r) // 8 + 1):
                cand.update(self.grid.get((gx, gy), ()))
        out = []
        for i in cand:
            q = self.T[i]["p"]
            xs = [v[0] for v in q]
            ys = [v[1] for v in q]
            if min(xs) - r <= p[0] <= max(xs) + r and min(ys) - r <= p[1] <= max(ys) + r:
                out.append(i)
        return out

    def watertight(self, nbr, i, a, b):
        """Whether another triangle shares edge (a, b) of triangle i from the opposite side."""
        c = next(u for u in self.T[i]["p"] if u is not a and u is not b)
        def side(u):
            return (b[0] - a[0]) * (u[1] - a[1]) - (b[1] - a[1]) * (u[0] - a[0])
        si = side(c)
        for j in nbr:
            if j == i:
                continue
            q = self.T[j]["p"]
            ka = [k for k in range(3) if d2(a, q[k]) < SAME]
            kb = [k for k in range(3) if d2(b, q[k]) < SAME]
            if not ka or not kb or ka[0] == kb[0]:
                continue
            if si * side(q[3 - ka[0] - kb[0]]) < 0:
                return True
        return False

    def tjunction(self, nbr, close):
        T = self.T
        for d, i, a, b, tp in sorted(close):
            if not -0.02 <= tp <= 1.02:
                continue
            for j in nbr:
                if j == i:
                    continue
                for m in T[j]["p"]:
                    if d2(m, a) < SAME * 3 or d2(m, b) < SAME * 3:
                        continue
                    dm, tm = segd(m, a, b)
                    if dm < TV_D and 0.02 < tm < 0.98:
                        ti, tj = T[i], T[j]
                        if ti["sec"] != tj["sec"]:
                            return "Ts"
                        if self.ntri[(tj["sec"], tj["face"])] > self.ntri[(ti["sec"], ti["face"])]:
                            return "Tq"
                        return "To"
        return None

    def crack(self, close):
        T = self.T
        for d, i, a, b, tp in sorted(close):
            for _, j, a2, b2, _ in close:
                if j == i:
                    continue
                for p1, p2 in (((a, a2), (b, b2)), ((a, b2), (b, a2))):
                    e1, e2 = d2(*p1), d2(*p2)
                    if e1 < CR_D and e2 < CR_D and max(e1, e2) > SAME:
                        return "Cs" if T[i]["sec"] != T[j]["sec"] else "Co"
        return None

    def depth_gap(self, p, nbr):
        """The view-depth gap of the nearest pair of triangle edges flanking p, None without a pair."""
        near = []
        for i in nbr:
            q, z = self.T[i]["p"], self.T[i]["z"]
            for k in range(3):
                k2 = (k + 1) % 3
                d, tp = segd(p, q[k], q[k2])
                if d >= O_R:
                    continue
                t = min(1.0, max(0.0, tp))
                pt = (q[k][0] + t * (q[k2][0] - q[k][0]), q[k][1] + t * (q[k2][1] - q[k][1]))
                near.append((d, (pt[0] - p[0], pt[1] - p[1]),
                             1.0 / ((1.0 - t) / z[k] + t / z[k2])))
        best = None
        for a, (da, ua, za) in enumerate(near):
            for db, ub, zb in near[a + 1:]:
                if ua[0] * ub[0] + ua[1] * ub[1] >= -0.3 * da * db:
                    continue
                if best is None or da + db < best[0]:
                    best = (da + db, abs(za - zb))
        return None if best is None else best[1]

    def classify(self, x, y):
        p = (x + 0.5, y + 0.5)
        nbr = self.near(p)
        edges, inside = [], []
        for i in nbr:
            q = self.T[i]["p"]
            area = ((q[1][0] - q[0][0]) * (q[2][1] - q[0][1]) -
                    (q[1][1] - q[0][1]) * (q[2][0] - q[0][0]))
            ds = []
            for k in range(3):
                a, b = q[k], q[(k + 1) % 3]
                d, tp = segd(p, a, b)
                ds.append(d)
                if d < OPEN_D:
                    edges.append((d, i, a, b, tp))
            if abs(area) > 1e-9:
                sg = 1 if area > 0 else -1
                if all(sg * ((q[(k + 1) % 3][0] - q[k][0]) * (p[1] - q[k][1]) -
                             (q[(k + 1) % 3][1] - q[k][1]) * (p[0] - q[k][0])) >= 0
                       for k in range(3)):
                    inside.append(min(ds))
        close = [e for e in edges if e[0] < EDGE_D]
        c = self.tjunction(nbr, close)
        if c:
            return c
        gap = None if inside else self.depth_gap(p, nbr)
        if gap is not None and gap >= O_Z:
            return "O"
        c = self.crack(close)
        if c:
            return c
        if inside and max(inside) > IN_D:
            return "K"
        for d, i, a, b, tp in sorted(close):
            if d < TIE_D and 0.0 < tp < 1.0 and self.watertight(nbr, i, a, b):
                return "E"
        if not inside:
            for d, i, a, b, tp in sorted(edges):
                if not self.watertight(nbr, i, a, b):
                    return "S"
        return "K" if inside else "U"


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 64
    out = None
    if "--out" in argv:
        k = argv.index("--out")
        out = argv[k + 1]
        argv = argv[:k] + argv[k + 2:]
    try:
        lines = open(argv[1]).read().splitlines()
        size = [tuple(map(int, l.split()[2:4])) for l in lines if l.startswith("# size")]
        if not size or size[0][0] != CAP_W:
            print("terrain_hole_classify: list %s names no 640-wide capture" % argv[1])
            return 1
        geo = Geo(argv[0], size[0][1])
    except FileNotFoundError as e:
        print("terrain_hole_classify: missing %s" % e.filename)
        return 1
    totals = Counter()
    rows = []
    for l in lines:
        if l.startswith("#") or not l.strip():
            continue
        x, y, n = map(int, l.split())
        if n != 1:
            continue
        c = geo.classify(x, y)
        totals[c[0]] += 1
        rows.append("%d %d %s\n" % (x, y, c))
    if out is not None:
        with open(out, "w") as f:
            f.writelines(rows)
    print(" ".join("%s=%d" % (c, totals[c]) for c in CLASSES))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
