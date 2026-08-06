#!/usr/bin/env python3
"""P0 census analysis over OPENPETE_TERRAIN_FIELDCENSUS logs:
1. Authored-field distributions: LP admission (w0&0x1F), LP OT bias
   ((w1>>3)&0x1F), HP OT bias ((w3>>3)&0x7), per level + global.
2. Admission <-> LP-OT-bias correlation (joint table + Pearson).
3. Coplanar-overlay census: same-tier faces within a sector sharing the exact
   corner-position multiset — the set that ties exactly under true depth and
   relied on same-slot FIFO order in the .s (the Toasty-walkway class).
"""
import collections, glob, math, sys

def parse(path):
    level = None
    secs = {}   # s -> {'lpv':[], 'hpv':[], 'lpf':[], 'hpf':[], 'flags':int}
    for ln in open(path, errors='replace'):
        if ln.startswith('[fc-hdr]'):
            level = int(ln.split('level=')[1].split()[0])
            secs = {}   # a new census block (boot/title fires one too); last wins
        elif ln.startswith('[fc-sec]'):
            kv = dict(x.split('=') for x in ln.split()[1:])
            secs[int(kv['s'])] = {'lpv': [], 'hpv': [], 'lpf': [], 'hpf': [],
                                  'flags': int(kv['flags'], 16)}
        elif ln.startswith('[fc-lpv]') or ln.startswith('[fc-hpv]'):
            t = ln.split()
            s = int(t[1].split('=')[1])
            key = 'lpv' if ln.startswith('[fc-lpv]') else 'hpv'
            if s in secs:
                secs[s][key] = [tuple(int(v) for v in w.split(',')) for w in t[2:]]
        elif ln.startswith('[fc-lpf]'):
            kv = dict(x.split('=') for x in ln.split()[1:])
            s = int(kv['s'])
            if s in secs:
                secs[s]['lpf'].append((int(kv['f']), int(kv['a']), int(kv['b']),
                                       int(kv['ds']),
                                       tuple(int(v) for v in kv['i'].split(','))))
        elif ln.startswith('[fc-hpf]'):
            kv = dict(x.split('=') for x in ln.split()[1:])
            s = int(kv['s'])
            if s in secs:
                secs[s]['hpf'].append((int(kv['f']), int(kv['b']), int(kv['fp']),
                                       tuple(int(v) for v in kv['i'].split(','))))
    return level, secs

def main():
    logs = sorted(glob.glob(sys.argv[1] if len(sys.argv) > 1 else '*.log'))
    adm = collections.Counter(); lpb = collections.Counter(); hpb = collections.Counter()
    fp = collections.Counter()
    joint = collections.Counter()
    cop = collections.Counter()   # (level, tier) -> overlay face count
    cop_groups = []
    n_lp = n_hp = 0
    per_level = {}
    for path in logs:
        level, secs = parse(path)
        if level is None:
            continue
        la = collections.Counter(); lb = collections.Counter()
        for s, S in secs.items():
            for f, a, b, ds, idx in S['lpf']:
                adm[a] += 1; lpb[b] += 1; joint[(a, b)] += 1; n_lp += 1
                la[a] += 1; lb[b] += 1
            for f, b, fpf, idx in S['hpf']:
                hpb[b] += 1; fp[fpf] += 1; n_hp += 1
            # coplanar overlays per tier
            for tier, faces, verts in (('LP', S['lpf'], S['lpv']),
                                       ('HP', [(f, 0, 0, 0, i) for f, b, fpf, i in S['hpf']], S['hpv'])):
                groups = collections.defaultdict(list)
                for rec in faces:
                    f, idx = rec[0], rec[4]
                    quad = idx[2] != idx[3]
                    ii = idx if quad else idx[:3]
                    try:
                        pos = tuple(sorted(verts[j] for j in ii))
                    except IndexError:
                        continue
                    groups[pos].append(f)
                for pos, fl in groups.items():
                    if len(fl) > 1:
                        cop[(path.split('/')[-1].split('.')[0], tier)] += len(fl)
                        cop_groups.append((path.split('/')[-1], tier, s, fl))
        per_level[path.split('/')[-1].split('.')[0]] = (la, lb)
    print(f"faces: LP={n_lp} HP={n_hp}")
    print("\nLP admission histogram (a: count):")
    print("  " + " ".join(f"{a}:{adm[a]}" for a in sorted(adm)))
    print("LP OT bias histogram (b: count):")
    print("  " + " ".join(f"{b}:{lpb[b]}" for b in sorted(lpb)))
    print("HP OT bias histogram:")
    print("  " + " ".join(f"{b}:{hpb[b]}" for b in sorted(hpb)))
    print(f"HP fog-push (w3&1): {dict(fp)}")
    # Pearson admit vs bias
    sa = sum(a * c for (a, b), c in joint.items()); sb = sum(b * c for (a, b), c in joint.items())
    n = sum(joint.values())
    ma, mb = sa / n, sb / n
    cov = sum((a - ma) * (b - mb) * c for (a, b), c in joint.items()) / n
    va = sum((a - ma) ** 2 * c for (a, b), c in joint.items()) / n
    vb = sum((b - mb) ** 2 * c for (a, b), c in joint.items()) / n
    r = cov / math.sqrt(va * vb) if va > 0 and vb > 0 else float('nan')
    print(f"\nadmission~LPbias Pearson r = {r:.3f}  (means a={ma:.2f} b={mb:.2f})")
    diag = sum(c for (a, b), c in joint.items() if a == b)
    print(f"identical a==b: {diag}/{n} ({100*diag/n:.1f}%)")
    print("\nCoplanar-overlay census (same-tier, exact corner-position multiset):")
    tot = collections.Counter()
    for (lvl, tier), cnt in sorted(cop.items()):
        print(f"  {lvl:<18} {tier}: {cnt} faces in overlay groups")
        tot[tier] += cnt
    print(f"  TOTAL: LP={tot['LP']} HP={tot['HP']} of LP={n_lp} HP={n_hp}")
    for g in cop_groups[:15]:
        print("   ex:", g)

if __name__ == '__main__':
    main()
