#!/usr/bin/env python3
"""P2 face-set join gate: join the .s prim capture (OPENPETE_OT_DUMP_FRAMES)
against the native terrain face universe (OPENPETE_TERRAIN_FARDBG) from the
same run/frame.

Buckets:
  joined            og prim <-> native DRAWN face (corner match, tol px)
  native-missing[r] og prim matches a native face DROPPED by gate r  <- signal
  subdiv-excluded   og prim inside a nearhq-diverted face's bbox (og emits the
                    2x2 subdivide; corners can't match the parent)
  unmatched-og      og prim matching nothing (moby/HUD prims, clipped geo)
  native-extra      native DRAWN face no og prim matched (backface emit-all,
                    extended-LP, float-vs-GTE threshold flips)  <- signal
og tris are matched as their own 3-corner set first, then as a subset of a
native quad's corners (the .s emits one half of a saddle quad).
"""
import argparse, collections, math, sys

TOL = 2.5
CELL = 8

def s16(v):
    v &= 0xFFFF
    return v - 65536 if v >= 32768 else v

FAMS = {
    0x30: ([1, 3, 5], 'LP'),      0x38: ([1, 3, 5, 7], 'LP'),
    0x34: ([1, 4, 7], 'HP'),      0x3C: ([1, 4, 7, 10], 'HP'),
}

def parse_ot(path, frame=None):
    out = []
    for ln in open(path):
        if ln.startswith('#'):
            continue
        p = ln.strip().split(',')
        if len(p) < 7:
            continue
        if frame is not None and int(p[0]) != frame:
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
        out.append((tier, xy))
    return out

def parse_fardbg(path):
    faces = {}
    drops = {}
    for ln in open(path, errors='replace'):
        if ln.startswith('[fardbg-face]'):
            t = ln.split()
            kv = dict(x.split('=', 1) for x in t[1:]
                      if '=' in x and not (x[0] == 'c' and x[1].isdigit()))
            corners = []
            for x in t[1:]:
                if x[0] == 'c' and x[1].isdigit():
                    v = x.split('=', 1)[1].split(',')
                    corners.append((float(v[0]), float(v[1]), float(v[2])))
            key = (int(kv['s']), int(kv['p']), int(kv['f']))
            faces[key] = {'tier': 'LP' if int(kv['p']) == 0 else 'HP',
                          'corners': corners, 'fr': int(kv['fr']), 'q': int(kv['q'])}
        elif ln.startswith('[fardbg-drop]'):
            t = ln.split()
            kv = dict(x.split('=', 1) for x in t[1:] if '=' in x)
            drops[(int(kv['s']), int(kv['p']), int(kv['f']))] = kv['r']
    return faces, drops

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ot', required=True)
    ap.add_argument('--fardbg', required=True)
    ap.add_argument('--frame', type=int, default=None)
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()

    og = parse_ot(a.ot, a.frame)
    faces, drops = parse_fardbg(a.fardbg)

    # corner grid over ALL native faces (drawn + dropped)
    grid = collections.defaultdict(set)
    for key, F in faces.items():
        for (x, y, m3) in F['corners']:
            grid[(int(x) // CELL, int(y) // CELL)].add(key)

    def candidates(xy):
        c = set()
        x, y = xy[0]
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                c |= grid.get((int(x) // CELL + dx, int(y) // CELL + dy), set())
        return c

    def match(xy, F, tier):
        """-> summed corner distance if every og corner pairs with a distinct
        native corner within TOL, else None."""
        if F['tier'] != tier:
            return None
        cs = [(c[0], c[1]) for c in F['corners']]
        if len(xy) > len(cs):
            return None
        used = [False] * len(cs)
        total = 0.0
        for (x, y) in xy:
            bj, bd = -1, None
            for j, (cx, cy) in enumerate(cs):
                if not used[j] and abs(cx - x) <= TOL and abs(cy - y) <= TOL:
                    d = (cx - x) ** 2 + (cy - y) ** 2
                    if bd is None or d < bd:
                        bj, bd = j, d
            if bj < 0:
                return None
            used[bj] = True
            total += bd
        return total

    # nearhq bboxes to absorb og subdivide prims
    near_bbox = []
    for key, r in drops.items():
        if r == 'nearhq':
            cs = faces[key]['corners']
            xs = [c[0] for c in cs]; ys = [c[1] for c in cs]
            near_bbox.append((min(xs) - TOL, min(ys) - TOL, max(xs) + TOL, max(ys) + TOL))

    joined = collections.Counter()
    missing = collections.Counter()
    missing_faces = collections.defaultdict(list)
    subdiv = 0
    unmatched = collections.Counter()
    matched_native = set()

    # global assignment: rank every (og prim, native face) pair by (drawn
    # before dropped, corner distance) and assign greedily — first-match
    # greedy mis-pairs adjacent thin strips whose neighbours sit inside TOL
    pairs = []
    for oi, (tier, xy) in enumerate(og):
        for key in candidates(xy):
            d = match(xy, faces[key], tier)
            if d is not None:
                r = drops.get(key)
                rank = 0 if r is None else (2 if r == 'nearhq' else 1)
                pairs.append((rank, d, oi, key, r))
    pairs.sort(key=lambda p: (p[0], p[1]))
    og_assigned = {}
    for rank, d, oi, key, r in pairs:
        if oi in og_assigned or key in matched_native:
            continue
        og_assigned[oi] = (key, rank, r)
        matched_native.add(key)

    for oi, (tier, xy) in enumerate(og):
        got = og_assigned.get(oi)
        if got is None:
            if any(all(bx0 <= x <= bx1 and by0 <= y <= by1 for x, y in xy)
                   for bx0, by0, bx1, by1 in near_bbox):
                subdiv += 1
            else:
                unmatched[tier] += 1
            continue
        key, rank, r = got
        if rank == 0:
            joined[tier] += 1
        elif r == 'nearhq':
            subdiv += 1
            matched_native.discard(key)
        else:
            missing[(tier, r)] += 1
            missing_faces[(tier, r)].append((key, xy))
            matched_native.discard(key)

    extra = collections.Counter()
    extra_faces = []
    for key, F in faces.items():
        if key in drops or key in matched_native:
            continue
        if not F['fr']:
            extra[(F['tier'], 'back')] += 1        # either-half acceptance class
            continue
        xs = [c[0] for c in F['corners']]
        ys = [c[1] for c in F['corners']]
        m3s = [c[2] for c in F['corners']]
        if sum(m3s) < 0x800:                       # og subdivide band (4*sum(SZ3) < 0x2000)
            extra[(F['tier'], 'subdivband')] += 1
        elif not (max(xs) >= 0 and min(xs) < 320 and max(ys) >= 0
                  and min(ys) < 224 and min(m3s) > 0):
            extra[(F['tier'], 'offscreen')] += 1   # .s clip-code skip; native GPU-clips
        else:
            extra[(F['tier'], 'ONSCREEN')] += 1    # <- the signal
            extra_faces.append((F['tier'], key))

    print(f"og terrain-family prims: {len(og)}  native faces: {len(faces)} "
          f"(dropped {len(drops)})")
    print(f"joined: LP={joined['LP']} HP={joined['HP']}   "
          f"subdiv-excluded={subdiv}  unmatched-og: LP={unmatched['LP']} HP={unmatched['HP']}")
    if missing:
        print("native-missing (og drew, native dropped):")
        for (tier, r), n in sorted(missing.items()):
            print(f"  {tier} r={r}: {n}")
    else:
        print("native-missing: none")
    print("native-extra (drawn, no og prim): "
          + " ".join(f"{t}/{fb}={n}" for (t, fb), n in sorted(extra.items())))
    if a.v:
        for (tier, r), lst in sorted(missing_faces.items()):
            for key, xy in lst:
                print(f"  MISS {tier} r={r} s={key[0]} f={key[2]} og_xy={xy}")
        for tier, key in sorted(extra_faces)[:40]:
            cs = [(round(c[0]), round(c[1])) for c in faces[key]['corners']]
            print(f"  EXTRA {tier} s={key[0]} f={key[2]} fr=1 {cs}")

if __name__ == '__main__':
    main()
