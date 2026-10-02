#!/usr/bin/env python3
"""Moby-vs-terrain CONTACT GATE, ground-truthed on geometry.

Design intent: moby/Spyro-vs-terrain occlusion is TRUE per-pixel geometry -
a deliberate divergence from the PSX's per-prim painter ordering. og does not
adjudicate this contest, so the reference is the scene's own geometry: one
OPENPETE_SCENEDUMP carries, for every scene vertex, both the ORDERING depth
the rasterizer tests (z, with every authored OT shift baked in) and the TRUE
view-Z (pvz, 0 = unshifted so z already encodes it).

The gate rasterises the moby/terrain subset twice with the same sampling
rule - once under the shipped ordering rule (interpolate z, min wins), once
under the true-geometry rule (re-encode pvz, min wins) - and reports every
pixel whose WINNER DOMAIN (moby vs terrain) differs. Those pixels are
exactly where an authored offset (terrain tier offsets, moby
m_DepthOffset pulls) flips a contact against geometry. The gate is clean
when the scene tests true depth with no authored offset in stored depth.

Categories: terrain = WORLD(0) + WORLD_FILL(11); moby = ACTOR(1) + PLAYER(3).
Transparent tris write no depth and are excluded. Depth encode:
z = vz/(vz+4096), screen-linear interpolation (the GPU's window-depth rule).

Usage: moby_contact_gate.py --scenedump run.log [--heat out.png] [--label L]
Exit status: 0 always (reporting tool; gate policy belongs to the caller).
"""
import argparse
import re
import sys

import numpy as np

GW, GH = 512, 464
SCALE = 4096.0
TERRAIN = (0, 11)
MOBY = (1, 3)

SD_RE = re.compile(
    r'\[sd\] #\d+ cat=(\d+) blend=(\d+) tex=\d+ grp=-?\d+(?: ko=[-0-9.]+)?'
    r'((?: \([-0-9.]+,[-0-9.]+,z[-0-9.]+,w[-0-9.]+,[0-9A-Fa-f]+\)){3})')
V_RE = re.compile(r'\(([-0-9.]+),([-0-9.]+),z([-0-9.]+),w([-0-9.]+),[0-9A-Fa-f]+\)')


def parse_scenedump(path):
    tris = []
    for ln in open(path, errors='replace'):
        m = SD_RE.search(ln)
        if not m:
            continue
        cat, blend = int(m.group(1)), int(m.group(2))
        if blend or cat not in TERRAIN + MOBY:
            continue
        vs = []
        for vm in V_RE.finditer(m.group(3)):
            sx, sy = float(vm.group(1)), float(vm.group(2))
            z, w = float(vm.group(3)), float(vm.group(4))
            vz_stored = SCALE * z / max(1.0 - z, 1e-9)
            vz_true = w if w > 0.0 else vz_stored
            vs.append((sx, sy, z, vz_true / (vz_true + SCALE)))
        if len(vs) == 3:
            tris.append((cat, vs))
    return tris


def winner_domain(tris, zi):
    """zi: vertex tuple index of the depth to interpolate (2 stored, 3 true).
    Returns int8 map: 0 uncovered, 1 terrain, 2 moby."""
    best = np.full((GH, GW), 2.0, np.float64)
    dom = np.zeros((GH, GW), np.int8)
    for cat, vs in tris:
        pts = [(sx, 2.0 * (sy - 8.0)) for sx, sy, _, _ in vs]
        (x0, y0), (x1, y1), (x2, y2) = pts
        det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(det) < 1e-9:
            continue
        lox = max(int(min(x0, x1, x2)), 0); hix = min(int(max(x0, x1, x2)) + 2, GW)
        loy = max(int(min(y0, y1, y2)), 0); hiy = min(int(max(y0, y1, y2)) + 2, GH)
        if lox >= hix or loy >= hiy:
            continue
        xs, ys = np.meshgrid(np.arange(lox, hix) + 0.5, np.arange(loy, hiy) + 0.5)
        b1 = ((xs - x0) * (y2 - y0) - (x2 - x0) * (ys - y0)) / det
        b2 = ((x1 - x0) * (ys - y0) - (xs - x0) * (y1 - y0)) / det
        b0 = 1.0 - b1 - b2
        m = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
        if not m.any():
            continue
        z = (b0[m] * vs[0][zi] + b1[m] * vs[1][zi] + b2[m] * vs[2][zi])
        iy, ix = np.nonzero(m)
        ix, iy = ix + lox, iy + loy
        upd = z < best[iy, ix]
        wix, wiy = ix[upd], iy[upd]
        best[wiy, wix] = z[upd]
        dom[wiy, wix] = 1 if cat in TERRAIN else 2
    return dom


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scenedump', required=True)
    ap.add_argument('--heat')
    ap.add_argument('--label', default='')
    a = ap.parse_args()
    tris = parse_scenedump(a.scenedump)
    if not tris:
        sys.exit(f'no scene tris parsed from {a.scenedump}')
    stored = winner_domain(tris, 2)
    true = winner_domain(tris, 3)
    both = (stored > 0) & (true > 0)
    viol = both & (stored != true)
    n_moby = int((true == 2).sum())
    lab = a.label or a.scenedump
    print(f'== {lab}: {len(tris)} tris, contact-domain px={int(both.sum())}, '
          f'moby-true px={n_moby}')
    nv = int(viol.sum())
    m2t = int((viol & (true == 2)).sum())   # geometry says moby, offsets say terrain
    t2m = nv - m2t
    print(f'  VIOLATIONS: {nv} px  (moby-hidden-by-offsets {m2t}, '
          f'moby-shown-by-offsets {t2m})')
    if a.heat:
        from PIL import Image
        img = np.zeros((GH, GW, 3), np.uint8)
        img[true == 1] = (45, 45, 45)
        img[true == 2] = (60, 90, 60)
        img[viol & (true == 2)] = (255, 60, 60)    # moby erased by offsets
        img[viol & (true == 1)] = (255, 200, 0)    # moby painted over terrain
        Image.fromarray(img).save(a.heat)
        print(f'  heat -> {a.heat} (red=moby-hidden yellow=moby-shown)')


if __name__ == '__main__':
    main()
