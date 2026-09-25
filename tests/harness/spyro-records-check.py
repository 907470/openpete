#!/usr/bin/env python3
"""spyro-records-check.py — the intermediate-leg assert of check_spyro_records.sh.

Reads one OPENPETE_SPYRO_RECDBG log and, for the case frame F, checks the
per-present record lines ([recdbg present f=F k=K]) against the epoch line
([recdbg f=F ...]) of the same run:

  - the drawn record count equals the expected model-call count,
  - each drawn record's pairing index equals the case's expected one
    (`pairs`: comma list in hook-call order, `-` = unpaired, `?` = unasserted),
  - a paired record interpolates (lerp=1) and its staged position lies within
    B/2 of its own canonical position on that tick (the alpha-0.5 present
    lies between the record's prev and curr, both within B of each other),
  - the shadow record lines ([recdbg shadow f=F j=J]) number the expected
    shadow-call count, each binds to a record of the list, and a shadow whose
    model record is paired interpolates (lerp=1) — the model's pairing is the
    shadow's; the exception is a model paired to a record that drew no shadow
    (the shadow appears this tick),
  - the flame record lines ([recdbg flame f=F j=J]) the same way.

Usage: spyro-records-check.py <log> <frame> <expected_m> <pairs> [expected_s] [expected_f] [B]
Prints one verdict line; exit 0 ok, 1 fail, 64 vacuous (no present lines).
"""
import re, sys, math

log, frame, em, pairs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
es = int(sys.argv[5]) if len(sys.argv) > 5 else None
ef = int(sys.argv[6]) if len(sys.argv) > 6 else None
B = float(sys.argv[7]) if len(sys.argv) > 7 else 400.0

epoch = re.compile(r'\[recdbg f=(\d+) e=\d+\] .* pos=(\S*) pairs=(\S*)')
pres = re.compile(r'\[recdbg present f=(\d+) k=(\d+)\] drawn=(\d) pair=(-?\d+) pair2=(-?\d+) lerp=(\d) alpha=([-\d.]+) pos=\((-?\d+),(-?\d+),(-?\d+)\)')

shad = re.compile(r'\[recdbg shadow f=(\d+) j=(\d+)\] model=(-?\d+) drawn=(\d) lerp=(\d) anchor=\((-?\d+),(-?\d+),(-?\d+)\)')
flam = re.compile(r'\[recdbg flame f=(\d+) j=(\d+)\] model=(-?\d+) drawn=(\d) lerp=(\d) origin=\((-?\d+),(-?\d+),(-?\d+)\)')

canon_pos = None; canon_pairs = None; recs = {}; shadows = {}; flames = {}
for line in open(log, errors='replace'):
    m = epoch.search(line)
    if m and int(m.group(1)) == frame:
        canon_pos = [tuple(map(int, p.strip('c0123456789').strip('()').split(','))) for p in re.findall(r'\(-?\d+,-?\d+,-?\d+\)', m.group(2))]
        canon_pairs = m.group(3)
        continue
    m = pres.search(line)
    if m and int(m.group(1)) == frame:
        k = int(m.group(2))
        recs[k] = dict(drawn=int(m.group(3)), pair=int(m.group(4)), pair2=int(m.group(5)),
                       lerp=int(m.group(6)), alpha=float(m.group(7)),
                       pos=(int(m.group(8)), int(m.group(9)), int(m.group(10))))
        continue
    m = shad.search(line)
    if m and int(m.group(1)) == frame:
        shadows[int(m.group(2))] = dict(model=int(m.group(3)), lerp=int(m.group(5)))
        continue
    m = flam.search(line)
    if m and int(m.group(1)) == frame:
        flames[int(m.group(2))] = dict(model=int(m.group(3)), lerp=int(m.group(5)))

if not recs:
    print(f"vacuous  no per-present record lines at f={frame}")
    sys.exit(64)

drawn = [k for k in sorted(recs) if recs[k]['drawn']]
fails = []
if len(drawn) != em:
    fails.append(f"drawn records={len(drawn)} expected {em}")
if pairs != '?':
    want = pairs.split(',')
    if len(want) != len(drawn):
        fails.append(f"pairs list has {len(want)} entries for {len(drawn)} drawn records")
    else:
        for k, w in zip(drawn, want):
            got = recs[k]['pair']
            if w == '-' and got != -1: fails.append(f"k={k} paired to {got}, expected unpaired")
            if w != '-' and got != int(w): fails.append(f"k={k} pair={got} expected {w}")
            if w != '-' and not recs[k]['lerp']: fails.append(f"k={k} paired but lerp=0")
for i, k in enumerate(drawn):
    if recs[k]['pair'] >= 0 and canon_pos and i < len(canon_pos):
        c = canon_pos[i]; p = recs[k]['pos']
        d = math.dist(c, p)
        if d > B / 2:
            fails.append(f"k={k} staged pos {p} is {d:.0f} from its canonical {c} (> B/2={B/2:.0f})")

for kind, table, want in (("shadow", shadows, es), ("flame", flames, ef)):
    if want is not None and len(table) != want:
        fails.append(f"{kind} records={len(table)} expected {want}")
    for j in sorted(table):
        mk = table[j]['model']
        if mk not in recs:
            fails.append(f"{kind} j={j} bound to model {mk}, not in the list")
        elif recs[mk]['pair'] >= 0 and recs[mk]['lerp'] and not table[j]['lerp']:
            fails.append(f"{kind} j={j} under paired model {mk} but lerp=0")

if fails:
    print(f"FAIL     f={frame}: " + "; ".join(fails))
    sys.exit(1)
desc = ",".join(f"{recs[k]['pair']}" for k in drawn)
sdesc = ",".join(f"{shadows[j]['model']}{'' if shadows[j]['lerp'] else 's'}" for j in sorted(shadows))
fdesc = ",".join(f"{flames[j]['model']}{'' if flames[j]['lerp'] else 's'}" for j in sorted(flames))
print(f"ok       f={frame}: {len(drawn)} drawn record(s), pairs={desc}, alpha={recs[drawn[0]]['alpha'] if drawn else 0:.3f}, shadows={sdesc or '-'}, flames={fdesc or '-'}")
sys.exit(0)
