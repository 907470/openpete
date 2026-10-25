#!/usr/bin/env python3
"""midcheck.py CANON_A INTER CANON_B [--box x0,y0,x1,y1]
Bubble interpolation check. Bubbles = pale specks (L>120) that are not orange (fish).
Match each bubble of CANON_A to its nearest bubble in CANON_B (displacement d over one
tick) and to its nearest in INTER (displacement e over the half tick). Interpolated
bubbles give e/d ~ 0.5 per matched bubble; a tick-rate copy gives e ~ 0 (bubbles sit at
CANON_A's positions) or e ~ d (at CANON_B's). Prints each matched bubble and the median
ratio; the fish blob (largest orange region) is the control. Inputs are three
consecutive presents of a clip (canonical, intermediate, canonical), e.g.
ffmpeg -i clip.mkv -vf select='between(n,F,F+2)' -vsync 0 b%03d.png."""
import argparse
import numpy as np
from PIL import Image
from scipy import ndimage

ap = argparse.ArgumentParser()
ap.add_argument('a'); ap.add_argument('i'); ap.add_argument('b')
ap.add_argument('--box', default=None); ap.add_argument('--maxd', type=float, default=40.0)
o = ap.parse_args()
box = tuple(int(v) for v in o.box.split(',')) if o.box else None

def load(p):
    im = Image.open(p).convert('RGB')
    if box: im = im.crop(box)
    a = np.asarray(im, dtype=np.int16)
    orange = (a[:, :, 0] - a[:, :, 2]) > 40
    L = a.mean(axis=2)
    m = (L > 120) & ~orange
    lab, k = ndimage.label(m)
    pts = []
    if k:
        cs = ndimage.center_of_mass(m, lab, range(1, k + 1)); sz = ndimage.sum(m, lab, range(1, k + 1))
        pts = [(c[1], c[0]) for c, s in zip(cs, sz) if 3 <= s <= 200]
    lab, k = ndimage.label(orange)
    fish = None
    if k:
        sz = ndimage.sum(orange, lab, range(1, k + 1)); j = int(np.argmax(sz)) + 1
        cy, cx = ndimage.center_of_mass(orange, lab, j); fish = (cx, cy)
    return np.array(pts), fish

A, fa = load(o.a); I, fi = load(o.i); B, fb = load(o.b)
print(f'bubbles: A {len(A)}  I {len(I)}  B {len(B)}')
if fa and fi and fb:
    da = np.hypot(fb[0] - fa[0], fb[1] - fa[1]); ea = np.hypot(fi[0] - fa[0], fi[1] - fa[1])
    print(f'fish control: |A->B| {da:.1f} px  |A->I| {ea:.1f} px  ratio {ea / da if da else float("nan"):.2f}')
def nn(P, Q):
    if len(Q) == 0: return np.full(len(P), np.inf), np.zeros(len(P), int)
    d = np.hypot(P[:, None, 0] - Q[None, :, 0], P[:, None, 1] - Q[None, :, 1]); j = d.argmin(1); return d[np.arange(len(P)), j], j
dab, jb = nn(A, B); dai, ji = nn(A, I)
ratios = []
for k in range(len(A)):
    if dab[k] > o.maxd or dab[k] < 2.0 or dai[k] > o.maxd: continue
    vb = B[jb[k]] - A[k]; vi = I[ji[k]] - A[k]
    r = float(np.dot(vi, vb) / np.dot(vb, vb))   # signed projection of A->I onto A->B
    perp = float(abs(vi[0] * vb[1] - vi[1] * vb[0]) / np.hypot(*vb))
    if perp > 3.0: continue                     # I not on the A->B line: a different bubble
    ratios.append(r)
    print(f'bubble A=({A[k,0]:6.1f},{A[k,1]:6.1f})  |A->B| {dab[k]:5.1f}  proj {r:5.2f}  perp {perp:3.1f}')
if ratios:
    print(f'matched {len(ratios)}  median ratio {np.median(ratios):.2f}  (0.5 = interpolated, 0 = held at A, 1 = at B)')
else:
    print('no matched bubbles (VACUOUS)')
