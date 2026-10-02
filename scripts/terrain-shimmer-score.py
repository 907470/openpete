#!/usr/bin/env python3
"""Flip-site shimmer score: tier flips should happen WHERE the PSX flips,
not nowhere.

Native leg: TIERCOLOR screenshots (LP=(255,0,0), HP=(0,0,255), fill=(0,255,0))
at consecutive ticks. og leg: the terrain-filtered OT dump (same run) rasterised
per frame, tier by prim code, painter's order, walk->screen px = (1.25*sx,
2*(sy-8)) at 640x448.

A flip site = pixel whose tier is LP in one frame and HP in the next (or vice
versa); fill counts as LP. Score = flip px per pair for each leg + dilated
overlap (a native flip within 2px of an og flip is "agreed").
"""
import argparse, collections, sys
from PIL import Image, ImageDraw

def s16(v):
    v &= 0xFFFF
    return v - 65536 if v >= 32768 else v

FAMS = {
    0x30: ([1, 3, 5], 1),      0x38: ([1, 3, 5, 7], 1),
    0x34: ([1, 4, 7], 2),      0x3C: ([1, 4, 7, 10], 2),
}
W, H = 640, 448

def og_tier_maps(path, frames):
    per = {f: [] for f in frames}
    for ln in open(path):
        if ln.startswith('#'):
            continue
        p = ln.strip().split(',')
        if len(p) < 7:
            continue
        fr = int(p[0])
        if fr not in per:
            continue
        code = int(p[5], 16)
        fam = FAMS.get(code & 0xFC)
        if not fam:
            continue
        words = [int(x, 16) for x in p[6:]]
        idxs, tier = fam
        if max(idxs) >= len(words):
            continue
        xy = [(s16(words[i] & 0xFFFF), s16(words[i] >> 16)) for i in idxs]
        per[fr].append((tier, xy))
    maps = {}
    for f, prims in per.items():
        im = Image.new('L', (W, H), 0)
        dr = ImageDraw.Draw(im)
        for tier, xy in prims:   # dump rows are walk order: far -> near
            pts = [(1.25 * x, 2.0 * (y - 8)) for x, y in xy]
            if len(pts) == 4:
                pts = [pts[0], pts[1], pts[3], pts[2]]  # TL,TR,BL,BR -> ring
            dr.polygon(pts, fill=tier)
        maps[f] = im
    return maps

def nat_tier_map(path):
    im = Image.open(path).convert('RGB')
    if im.size != (W, H):
        im = im.resize((W, H), Image.NEAREST)
    out = Image.new('L', (W, H), 0)
    po = out.load(); pi = im.load()
    for y in range(H):
        for x in range(W):
            r, g, b = pi[x, y]
            if r == 255 and g == 0 and b == 0:
                po[x, y] = 1
            elif r == 0 and g == 255 and b == 0:
                po[x, y] = 1          # fill = LP tier
            elif r == 0 and g == 0 and b == 255:
                po[x, y] = 2
    return out

def flips(a, b):
    pa, pb = a.load(), b.load()
    s = set()
    for y in range(H):
        for x in range(W):
            u, v = pa[x, y], pb[x, y]
            if u and v and u != v:
                s.add((x, y))
    return s

def dilate(s, r=2):
    out = set()
    for (x, y) in s:
        for dx in range(-r, r + 1):
            for dy in range(-r, r + 1):
                out.add((x + dx, y + dy))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ot', required=True)
    ap.add_argument('--shots', required=True, help='template with {n}')
    ap.add_argument('--frames', required=True, help='comma list')
    ap.add_argument('--og-offset', type=int, default=0,
                    help='og dump VBL = native shot VBL + offset (DrawOTag '
                         'fires on the other VBL of the tick)')
    a = ap.parse_args()
    frames = [int(x) for x in a.frames.split(',')]
    ogm = og_tier_maps(a.ot, [f + a.og_offset for f in frames])
    ogm = {f: ogm[f + a.og_offset] for f in frames}
    natm = {f: nat_tier_map(a.shots.replace('{n}', str(f))) for f in frames}
    tn = to = ta_n = ta_o = 0
    print(f"{'pair':>12} {'natflip':>8} {'ogflip':>7} {'nat-in-og%':>10} {'og-in-nat%':>10}")
    for f0, f1 in zip(frames, frames[1:]):
        nf = flips(natm[f0], natm[f1])
        of = flips(ogm[f0], ogm[f1])
        din, don = dilate(of), dilate(nf)
        ain = sum(1 for p in nf if p in din)
        aon = sum(1 for p in of if p in don)
        tn += len(nf); to += len(of); ta_n += ain; ta_o += aon
        print(f"{f0}-{f1:>6} {len(nf):>8} {len(of):>7} "
              f"{100.0*ain/len(nf) if nf else 100:>9.1f} "
              f"{100.0*aon/len(of) if of else 100:>9.1f}")
    print(f"{'TOTAL':>12} {tn:>8} {to:>7} "
          f"{100.0*ta_n/tn if tn else 100:>9.1f} {100.0*ta_o/to if to else 100:>9.1f}")

if __name__ == '__main__':
    main()
