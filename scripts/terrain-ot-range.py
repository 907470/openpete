#!/usr/bin/env python3
"""P0 OT-range census, computed analytically (the .s drains its OT into a pure
prim chain before DrawOTag, so bucket indices never reach a capture — but the
index arithmetic is exact given per-face view-Z + the authored bias fields):

  LP index = (sum of 4 corner SZ3_16)>>5 + ((w1>>3)&0x1F)*8 + 0x40
  HP index = (sum of 4 corner SZ3_4)>>7 + ((w3>>3)&0x7)*4
  (both = floor(sum m3)/32 + bias term; tris double the shared corner)

Inputs: a fardbg log (per-face corner m3, drawn set via [fardbg-drop]) and the
level's FIELDCENSUS log (bias fields). Reports per-frame LP/HP index ranges,
the equal-depth separation, and screen-overlapping same/near-slot pairs — the
FIFO-tie set that matters for P4's discard-FIFO decision.
"""
import collections, sys

def parse_census(path):
    lpb = {}; hpb = {}
    cur = None
    for ln in open(path, errors='replace'):
        if ln.startswith('[fc-hdr]'):
            lpb.clear(); hpb.clear()
        elif ln.startswith('[fc-lpf]'):
            kv = dict(x.split('=') for x in ln.split()[1:])
            lpb[(int(kv['s']), int(kv['f']))] = int(kv['b'])
        elif ln.startswith('[fc-hpf]'):
            kv = dict(x.split('=') for x in ln.split()[1:])
            hpb[(int(kv['s']), int(kv['f']))] = int(kv['b'])
    return lpb, hpb

def parse_fardbg(path):
    faces = {}; drops = set()
    for ln in open(path, errors='replace'):
        if ln.startswith('[fardbg-face]'):
            t = ln.split()
            kv = dict(x.split('=', 1) for x in t[1:]
                      if '=' in x and not (x[0] == 'c' and x[1].isdigit()))
            cs = []
            for x in t[1:]:
                if x[0] == 'c' and x[1].isdigit():
                    v = x.split('=', 1)[1].split(',')
                    cs.append((float(v[0]), float(v[1]), float(v[2])))
            faces[(int(kv['s']), int(kv['p']), int(kv['f']))] = (cs, int(kv['fr']))
        elif ln.startswith('[fardbg-drop]'):
            kv = dict(x.split('=', 1) for x in ln.split()[1:] if '=' in x)
            drops.add((int(kv['s']), int(kv['p']), int(kv['f'])))
    return faces, drops

def main():
    fardbg, census = sys.argv[1], sys.argv[2]
    lpb, hpb = parse_census(census)
    faces, drops = parse_fardbg(fardbg)
    ents = []   # (tier, s, f, index, bbox)
    for (s, p, f), (cs, fr) in faces.items():
        if (s, p, f) in drops or p == 2 or not fr:
            continue
        m3 = [c[2] for c in cs]
        if len(m3) < 3 or min(m3) <= 0:
            continue
        if len(m3) == 3:
            m3 = m3 + [m3[2]]
        base = int(sum(m3)) >> 5
        if p == 0:
            b = lpb.get((s, f))
            if b is None: continue
            idx = base + b * 8 + 0x40
            tier = 'LP'
        else:
            b = hpb.get((s, f))
            if b is None: continue
            idx = base + b * 4
            tier = 'HP'
        xs = [c[0] for c in cs]; ys = [c[1] for c in cs]
        ents.append((tier, s, f, idx, (min(xs), min(ys), max(xs), max(ys))))
    lp = [e for e in ents if e[0] == 'LP']; hp = [e for e in ents if e[0] == 'HP']
    if not lp or not hp:
        print(f"{fardbg}: LP={len(lp)} HP={len(hp)} (insufficient)"); return
    lpr = (min(e[3] for e in lp), max(e[3] for e in lp))
    hpr = (min(e[3] for e in hp), max(e[3] for e in hp))
    print(f"LP n={len(lp)} idx {lpr[0]}..{lpr[1]}   HP n={len(hp)} idx {hpr[0]}..{hpr[1]}")
    def ov(a, b):
        return not (a[2] < b[0] or b[2] < a[0] or a[3] < b[1] or b[3] < a[1])
    # cross-tier: screen-overlapping pairs and their index delta
    deltas = collections.Counter()
    bad = []
    for tl in lp:
        for th in hp:
            if ov(tl[4], th[4]):
                d = tl[3] - th[3]
                deltas['<0' if d < 0 else ('0' if d == 0 else ('1-35' if d < 36 else '>=36'))] += 1
                if d < 36 and len(bad) < 8:
                    bad.append((tl[1], tl[2], tl[3], th[1], th[2], th[3]))
    print(f"cross-tier overlapping pairs by LPidx-HPidx: {dict(deltas)}")
    if bad:
        print("  pairs under 36 (lp s,f,idx / hp s,f,idx):", bad)
    # same-tier exact ties with screen overlap (FIFO-order reliance at runtime)
    for tier, es in (('LP', lp), ('HP', hp)):
        by = collections.defaultdict(list)
        for e in es: by[e[3]].append(e)
        ties = 0
        for idx, group in by.items():
            for i in range(len(group)):
                for j in range(i + 1, len(group)):
                    if ov(group[i][4], group[j][4]): ties += 1
        print(f"same-slot screen-overlapping {tier} pairs: {ties}")

if __name__ == '__main__':
    main()
