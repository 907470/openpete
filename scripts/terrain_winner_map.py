#!/usr/bin/env python3
"""Per-pixel terrain WINNER map: og (OT-dump painter raster) vs native
(stored-depth raster of the identity tri dump), attributed at FACE identity
granularity (sec/face/tier).

The tier-agreement instrument compares tier per pixel and is blind to
intra-tier winner flips and to coverage holes; this tool attributes both.
Inputs come from one native run per scene:

  OPENPETE_TERRAIN_TRIDUMP=<frame>,<path>   identity+depth of every fed tri
  OPENPETE_OT_DUMP_FRAMES=<frame-1>-<frame> OPENPETE_OT_DUMP_PATH=<path>
                                            the game's own OT (og truth)

Both legs are rasterised here with the SAME sampling rule (pixel centres in
walk space x2 vertical, i.e. 512x464 grid mapped from 512x240-8), so edge
pixels cancel instead of reading as disagreement.

Native winner rule mirrors the GPU: window depth is screen-linear in
1/vz_stored (vz_stored = true vz + 128*otslots world units); the winner is
the max of interpolated 1/vz_stored among depth-writing tris. og winner rule
is the PSX painter: prims draw in dump order (far -> near), last wins.

Census classes per both-covered pixel:
  agree           same (sec,face,tier) wins both legs
  intra-tier      matched faces, same tier, different face   (P6d population)
  cross-tier      matched faces, different tier              (tier-map class)
  og-hole         og winner prim not fed by native (>=2 shared verts with a
                  native face => terrain coverage hole; else likely non-
                  terrain prim: moby/HUD/sky in the OT)
  native-only     og has no prim covering the pixel at all

Usage:
  terrain_winner_map.py --ot ot.txt --tridump tri.txt [--og-frame N]
                        [--heat out.png] [--csv out.csv]
"""
import argparse
import collections
import sys

import numpy as np

# Walk space: 512 wide, 240 tall including the 8-line overscan skirt.
# Rasterise at x1 horizontally, x2 vertically for sub-line tier edges.
GW, GH = 512, 464   # y' = 2*(y-8), y in [8,240)

S16 = lambda v: (v & 0xFFFF) - 65536 if (v & 0xFFFF) >= 32768 else (v & 0xFFFF)

# POLY code families: (payload xy word indices, nverts). Colours/UVs skipped.
FAMS = {
    0x30: ([1, 3, 5], 3),        # POLY_G3
    0x38: ([1, 3, 5, 7], 4),     # POLY_G4
    0x34: ([1, 4, 7], 3),        # POLY_GT3
    0x3C: ([1, 4, 7, 10], 4),    # POLY_GT4
    0x20: ([1, 2, 3], 3),        # POLY_F3
    0x28: ([1, 2, 3, 4], 4),     # POLY_F4
    0x24: ([1, 3, 5], 3),        # POLY_FT3
    0x2C: ([1, 3, 5, 7], 4),     # POLY_FT4
}


def parse_ot(path, want_frame):
    """-> (frame_used, [ (row_idx, [(x,y)..]) ] in dump (painter) order)."""
    frames = collections.defaultdict(list)
    for ln in open(path):
        if ln.startswith('#'):
            continue
        p = ln.strip().split(',')
        if len(p) < 7:
            continue
        fr = int(p[0])
        code = int(p[5], 16)
        fam = FAMS.get(code & 0xFC)
        if not fam:
            continue
        words = [int(x, 16) for x in p[6:]]
        idxs, nv = fam
        if max(idxs) >= len(words):
            continue
        xy = [(S16(words[i] & 0xFFFF), S16(words[i] >> 16)) for i in idxs]
        frames[fr].append(xy)
    if not frames:
        sys.exit(f'no poly prims parsed from {path}')
    fr = want_frame if want_frame in frames else max(frames)
    return fr, list(enumerate(frames[fr]))


def parse_tridump(path):
    """-> list of dicts: identity + verts + true vz (world units)."""
    tris = []
    for ln in open(path):
        if ln.startswith('#'):
            continue
        p = ln.strip().split(',')
        if len(p) != 16:
            continue
        sec, face, tier, fill, blend, textured, slots = (int(x) for x in p[:7])
        v = [(float(p[7 + 3 * k]), float(p[8 + 3 * k]), float(p[9 + 3 * k]))
             for k in range(3)]
        tris.append(dict(sec=sec, face=face, tier=tier, fill=fill,
                         blend=blend, textured=textured, slots=slots, v=v))
    if not tris:
        sys.exit(f'no tris parsed from {path}')
    return tris


def to_grid(x, y):
    return x, 2.0 * (y - 8.0)


def raster(tri_xy, attr_z=None):
    """Barycentric coverage of pixel centres inside tri_xy (grid space).
    Returns (xs, ys, w) index arrays + weights, or None if degenerate."""
    (x0, y0), (x1, y1), (x2, y2) = tri_xy
    det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if abs(det) < 1e-9:
        return None
    lox = max(int(np.floor(min(x0, x1, x2))), 0)
    hix = min(int(np.ceil(max(x0, x1, x2))) + 1, GW)
    loy = max(int(np.floor(min(y0, y1, y2))), 0)
    hiy = min(int(np.ceil(max(y0, y1, y2))) + 1, GH)
    if lox >= hix or loy >= hiy:
        return None
    xs, ys = np.meshgrid(np.arange(lox, hix) + 0.5,
                         np.arange(loy, hiy) + 0.5)
    b1 = ((xs - x0) * (y2 - y0) - (x2 - x0) * (ys - y0)) / det
    b2 = ((x1 - x0) * (ys - y0) - (xs - x0) * (y1 - y0)) / det
    b0 = 1.0 - b1 - b2
    m = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
    if not m.any():
        return None
    iy, ix = np.nonzero(m)
    out = (ix + lox, iy + loy)
    if attr_z is None:
        return out
    z = b0[m] * attr_z[0] + b1[m] * attr_z[1] + b2[m] * attr_z[2]
    return out + (z,)


def og_winner_map(prims):
    """Painter raster, fill = prim index + 1 (0 = uncovered)."""
    win = np.zeros((GH, GW), np.int32)
    for idx, xy in prims:
        pts = [to_grid(x, y) for x, y in xy]
        subs = [pts[:3]] if len(pts) == 3 else [[pts[0], pts[1], pts[2]],
                                               [pts[1], pts[3], pts[2]]]
        for s in subs:
            r = raster(s)
            if r:
                win[r[1], r[0]] = idx + 1
    return win


def native_winner_map(tris):
    """Depth raster: winner = max interpolated 1/vz_stored among
    depth-writing tris (blend writes no depth; FILL twins duplicate their
    WORLD tri at identical depth, so including them is a harmless tie)."""
    best = np.zeros((GH, GW), np.float64)
    win = np.full((GH, GW), -1, np.int32)
    for i, t in enumerate(tris):
        if t['blend']:
            continue
        xy = [to_grid(x, y) for x, y, _ in t['v']]
        q = [1.0 / max(vz + 128.0 * t['slots'], 1.0) for _, _, vz in t['v']]
        r = raster(xy, q)
        if not r:
            continue
        ix, iy, z = r
        upd = z > best[iy, ix]
        wix, wiy = ix[upd], iy[upd]
        best[wiy, wix] = z[upd]
        win[wiy, wix] = i
    return win


SNAP_R = 2.0   # og verts are PSX fixed-point GTE, native is float: offsets
               # up to ~1.4px are normal. Snap og verts to the nearest native
               # vertex within this radius.


def build_identity_index(tris):
    """Exact native float vert -> spatial hash bucket; native tri lookup by
    frozenset of exact vert pairs; per-vert tri sets for partial matches."""
    ident = {}
    vert_tris = collections.defaultdict(set)
    grid = collections.defaultdict(list)
    for i, t in enumerate(tris):
        vs = tuple((x, y) for x, y, _ in t['v'])
        ident.setdefault(frozenset(vs), i)
        for v in vs:
            if v not in vert_tris:
                grid[(int(v[0]) // 2, int(v[1]) // 2)].append(v)
            vert_tris[v].add(i)
    return ident, vert_tris, grid


def snap_all(v, grid):
    """All native verts within SNAP_R of v, sorted nearest-first."""
    x, y = v
    bx, by = int(x) // 2, int(y) // 2
    out = []
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for (nx, ny) in grid.get((bx + dx, by + dy), ()):
                d = (nx - x) ** 2 + (ny - y) ** 2
                if d <= SNAP_R * SNAP_R:
                    out.append((d, (nx, ny)))
    out.sort()
    return [v for _, v in out[:4]]


def match_og_prims(prims, ident, vert_tris, grid, tris):
    """og prim index -> ('face', tri_idx) | ('hole', None) | ('other', None).

    For each og vert take every fed native vert within SNAP_R (the og path is
    PSX fixed-point GTE, native is float; dense meshes put several candidates
    inside the radius). The prim is identified by the FED TRI whose exact
    3-vert set is formed by one candidate per vert (either quad diagonal),
    minimising total snap distance. 'hole': >=2 og verts have fed-vert
    neighbours but no fed tri owns the prim — geometry the feed knows, face
    it did not emit. 'other': a non-terrain prim (moby, sky, HUD)."""
    out = {}
    for idx, xy in prims:
        cands = [snap_all(v, grid) for v in xy]
        got = sum(1 for c in cands if c)
        if got < 2:
            out[idx] = ('other', None)
            continue
        tri_ids = ([(0, 1, 2)] if len(xy) == 3 else
                   [(0, 1, 2), (1, 3, 2), (0, 1, 3), (0, 2, 3)])
        best, bd = None, 1e18
        for ids in tri_ids:
            if any(not cands[i] for i in ids):
                continue
            for a in cands[ids[0]]:
                for b in cands[ids[1]]:
                    for c in cands[ids[2]]:
                        k = frozenset((a, b, c))
                        if len(k) < 3 or k not in ident:
                            continue
                        d = sum((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2
                                for p, q in ((a, xy[ids[0]]), (b, xy[ids[1]]),
                                             (c, xy[ids[2]])))
                        if d < bd:
                            best, bd = ident[k], d
        out[idx] = ('face', best) if best is not None else ('hole', None)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ot', required=True)
    ap.add_argument('--tridump', required=True)
    ap.add_argument('--og-frame', type=int, default=-1)
    ap.add_argument('--heat', help='write class heat PNG')
    ap.add_argument('--label', default='')
    a = ap.parse_args()

    fr, prims = parse_ot(a.ot, a.og_frame)
    tris = parse_tridump(a.tridump)
    ident, vert_tris, grid = build_identity_index(tris)
    ogmap = og_winner_map(prims)
    natmap = native_winner_map(tris)
    ogmatch = match_og_prims(prims, ident, vert_tris, grid, tris)

    cls = np.zeros((GH, GW), np.uint8)   # 0 none,1 agree,2 intra,3 cross,
                                         # 4 og-hole,5 og-foreign,6 native-only
    C = collections.Counter()
    for y in range(GH):
        ogrow, natrow = ogmap[y], natmap[y]
        for x in range(GW):
            o, n = ogrow[x], natrow[x]
            if o == 0:
                if n >= 0:
                    cls[y, x] = 6; C['native-only'] += 1
                continue
            kind, oi = ogmatch[o - 1]
            if kind == 'other':
                # non-terrain og winner (moby/sky/HUD). Over native terrain
                # that is a cross-DOMAIN contest (not this instrument's
                # domain); over nothing it is outside the domain entirely.
                if n >= 0:
                    cls[y, x] = 5; C['og-foreign'] += 1
                continue
            if kind == 'hole' or n < 0:
                cls[y, x] = 4; C['og-hole'] += 1
                continue
            to_, tn = tris[oi], tris[n]
            if (to_['sec'], to_['face']) == (tn['sec'], tn['face']):
                cls[y, x] = 1; C['agree'] += 1
            elif to_['tier'] == tn['tier']:
                cls[y, x] = 2; C['intra-tier'] += 1
            else:
                cls[y, x] = 3; C['cross-tier'] += 1

    dom = sum(C.get(k, 0) for k in ('agree', 'intra-tier', 'cross-tier',
                                    'og-hole'))
    lab = a.label or a.tridump
    print(f'== {lab} (og frame {fr}, {len(prims)} og prims, {len(tris)} native tris)')
    for k in ('agree', 'intra-tier', 'cross-tier', 'og-hole', 'og-foreign',
              'native-only'):
        v = C.get(k, 0)
        pct = 100.0 * v / dom if dom else 0
        print(f'  {k:<12} {v:>8}  {pct:6.2f}%{"" if k in ("agree","intra-tier","cross-tier","og-hole") else " (of terrain domain, informational)"}')
    print(f'  terrain-domain px: {dom}')

    if a.heat:
        from PIL import Image
        pal = np.array([[0, 0, 0], [40, 40, 40], [255, 0, 0], [255, 160, 0],
                        [0, 120, 255], [90, 0, 130], [0, 255, 90]], np.uint8)
        Image.fromarray(pal[cls]).save(a.heat)
        print(f'  heat -> {a.heat}  (grey=agree red=intra orange=cross '
              f'blue=og-hole purple=og-other green=native-only)')


if __name__ == '__main__':
    main()
