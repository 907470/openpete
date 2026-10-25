#!/usr/bin/env python3
"""flickdet.py CLIP [--start S] [--dur D] [--w 960] [--thr 40] [--blob 80]
Single-frame polygon flicker detector. A pixel is a flicker pixel at frame t when it
differs from both neighbouring frames by more than --thr gray levels while the
neighbours agree within --thr/2. A frame is reported when its largest 8-connected
blob of flicker pixels has at least --blob pixels (a whole polygon, not texture
shimmer). Prints frame index (within the window), time, blob area, blob bbox,
the blob's mean |delta|; then a summary with the count of reported frames.
A hit with another overlapping hit within 2 frames is a moving object, not
reported."""
import argparse
import glob
import os
import subprocess
import numpy as np
from scipy import ndimage

ap = argparse.ArgumentParser()
ap.add_argument('clip'); ap.add_argument('--start', type=float, default=0.0)
ap.add_argument('--dur', type=float, default=10.0); ap.add_argument('--w', type=int, default=960)
ap.add_argument('--thr', type=int, default=40); ap.add_argument('--blob', type=int, default=80)
ap.add_argument('--fps', type=float, default=60.0)
ap.add_argument('--seq', action='store_true', help='CLIP is a directory of PNG presents (sorted by name)')
a = ap.parse_args()
h = a.w * 9 // 16
frames = []
names = []
if a.seq:
    from PIL import Image
    for f in sorted(glob.glob(os.path.join(a.clip, '*.png'))):
        im = Image.open(f).convert('L').resize((a.w, h), Image.BILINEAR)
        frames.append(np.asarray(im, dtype=np.int16)); names.append(os.path.basename(f))
else:
    cmd = ['ffmpeg', '-v', 'error', '-ss', str(a.start), '-t', str(a.dur), '-i', a.clip,
           '-vf', f'scale={a.w}:{h}', '-pix_fmt', 'gray', '-f', 'rawvideo', '-']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    n = a.w * h
    while True:
        b = p.stdout.read(n)
        if len(b) < n: break
        frames.append(np.frombuffer(b, dtype=np.uint8).reshape(h, a.w).astype(np.int16))
    p.wait()
hits = 0
st8 = np.ones((3, 3), dtype=bool)
found = []   # (t, area, x0, y0, x1, y1, md)
for t in range(1, len(frames) - 1):
    dp = np.abs(frames[t] - frames[t - 1]); dn = np.abs(frames[t] - frames[t + 1]); dpn = np.abs(frames[t - 1] - frames[t + 1])
    m = (dp > a.thr) & (dn > a.thr) & (dpn < a.thr // 2)
    if int(m.sum()) < a.blob: continue
    lab, k = ndimage.label(m, structure=st8)
    if k == 0: continue
    sizes = ndimage.sum(m, lab, range(1, k + 1))
    j = int(np.argmax(sizes)) + 1
    area = int(sizes[j - 1])
    if area < a.blob: continue
    ys, xs = np.nonzero(lab == j)
    md = float(dp[lab == j].mean())
    found.append((t, area, int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()), md))
def overlap(p, q):
    return not (p[4] < q[2] - 8 or q[4] < p[2] - 8 or p[5] < q[3] - 8 or q[5] < p[3] - 8)
# an isolated hit: no hit within 2 frames whose blob box overlaps (a fast-moving object is a run of hits)
for i, f in enumerate(found):
    near = [g for g in found if g is not f and abs(g[0] - f[0]) <= 2 and overlap(f, g)]
    if near: continue
    hits += 1
    nm = names[f[0]] if names else ''
    print(f'frame {f[0]} {nm} t={a.start + f[0] / a.fps:.3f}s blob={f[1]} bbox=({f[2]},{f[3]})-({f[4]},{f[5]}) mean_delta={f[6]:.0f} at {a.w}x{h}')
print(f'summary: {len(frames)} frames, {hits} isolated polygon-flicker frames ({len(found)} raw), thr {a.thr}, blob {a.blob}')
