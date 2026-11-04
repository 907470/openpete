#!/usr/bin/env python3
"""Verdict of one check_late_switch_subtick.sh row (see that script for the
instrument, the reference states and the pass criterion).

Usage: late_switch_check.py <inter.log> <inter-dir> <canon-dir> <game> <bk2>
                            <lo> <hi> <keys>
Exit 0 PASS, 1 FAIL or VACUOUS.
"""
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

PART = re.compile(r'\[late-part vbl=(\d+) leg=(canon|inter) a=(-?[\d.]+) ep=(\d+) new=(\d+)\] '
                  r'id=(-?\d+):(\d+) cls=(-?\d+) st=(\S+) c=([0-9a-f]+),([0-9a-f]+) v=(\S*)')
MOBY = re.compile(r'\[late-moby vbl=(\d+) leg=inter a=(-?[\d.]+) src=(\w+) pass=(\w+)\] m=([0-9a-f]+)(?::(\d+))? '
                  r'cls=([0-9a-f]+) n=(\d+) c=\(([-\d.]+),([-\d.]+)\) w=\(([-\d.]+),([-\d.]+),([-\d.]+)\)')


def alpha_bin(a):
    """The judged phase a printed alpha stands for: 0, 0.5 or 1 (an unbracketed present prints -1)."""
    a = float(a)
    if a < 0 or a >= 0.999:
        return 1.0
    return 0.0 if a < 0.25 else 0.5


def parse_keys(keys):
    out = {}
    for tok in keys.split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            out[k] = v
    return out


def verts(s):
    if not s:
        return []
    return [tuple(float(c) for c in p.split(',')) for p in s.split(';')]


def colour(w):
    v = int(w, 16)
    return [(v >> (8 * k)) & 0xFF for k in range(3)]


def on_segment(x, a, b, tol, wrap=None):
    """Whether x lies between a and b (inclusive, within tol); wrap names a circle's period."""
    if wrap:
        d = (b - a + wrap / 2) % wrap - wrap / 2
        e = (x - a + wrap / 2) % wrap - wrap / 2
        lo, hi = min(0.0, d), max(0.0, d)
        return lo - tol <= e <= hi + tol
    return min(a, b) - tol <= x <= max(a, b) + tol


def identity_leg(idir, cdir, vbls):
    """Pixel differences between each alpha-1 intermediate capture and the canonical capture of its VBL."""
    worst = 0
    missing = []
    for v in vbls:
        fi, fc = Path(idir) / f'i__{v}-vk.png', Path(cdir) / f'c__{v}-vk.png'
        if not fi.is_file() or not fc.is_file():
            missing.append(v)
            continue
        a = np.asarray(Image.open(fi).convert('RGB'), dtype=np.int16)
        b = np.asarray(Image.open(fc).convert('RGB'), dtype=np.int16)
        if a.shape != b.shape:
            worst = max(worst, a.shape[0] * a.shape[1])
            continue
        worst = max(worst, int(np.any(a != b, axis=2).sum()))
    return worst, missing


def check_part(lines, lo, hi, opts, msgs):
    vtol, stol = float(opts.get('vtol', 0.02)), float(opts.get('stol', 0.002))
    canon, inter = {}, {}
    for m in lines:
        vbl, leg, a, ep, new = int(m[1]), m[2], m[3], int(m[4]), int(m[5])
        slot, ser, st = int(m[6]), int(m[7]), [float(x) for x in m[9].split(',')]
        rec = dict(slot=slot, ser=ser, cls=int(m[8]), st=st, c=(colour(m[10]), colour(m[11])),
                   v=verts(m[12]), ep=ep, new=new)
        if leg == 'canon':
            # The last feed of an epoch is its canonical present.
            g = canon.setdefault(ep, {})
            if g.get('_vbl') != vbl:
                g.clear()
                g['_vbl'] = vbl
                g['recs'] = []
            g['recs'].append(rec)
        else:
            inter.setdefault((vbl, alpha_bin(a)), []).append(rec)

    def keyed(recs):
        out, unp = {}, 0
        for r in recs:
            if r['slot'] < 0:
                unp += 1
            else:
                out[(r['slot'], r['ser'])] = r
        return out, unp

    fails = 0
    counts = dict(intervals=0, held=0, deferred=0, reused=0, held_drawn=0, lerped=0)
    vmax = 0.0
    for v in range(lo + 1, hi + 2, 2):
        presents = {a: inter.get((v, a)) for a in (0.0, 0.5, 1.0)}
        if any(p is None for p in presents.values()):
            msgs.append(f'  vbl {v}: missing an intermediate present ({sorted(k for k, p in presents.items() if p is None)})')
            fails += 1
            continue
        newest = presents[1.0][0]['new'] if presents[1.0] else presents[0.0][0]['new']
        if newest not in canon or newest - 1 not in canon:
            msgs.append(f'  vbl {v}: canonical sets {newest - 1}/{newest} not traced')
            fails += 1
            continue
        r0, u0 = keyed(canon[newest - 1]['recs'])
        r1, u1 = keyed(canon[newest]['recs'])
        counts['intervals'] += 1
        held = set(r0) - set(r1)
        counts['held'] += len(held)
        counts['deferred'] += len(set(r1) - set(r0))
        slots1 = {s for s, _ in r1}
        counts['reused'] += sum(1 for s, _ in held if s in slots1)
        for a, recs in presents.items():
            got, unp = keyed(recs)
            slots = [r['slot'] for r in recs if r['slot'] >= 0]
            if len(slots) != len(set(slots)):
                msgs.append(f'  vbl {v} a={a}: a slot drawn twice')
                fails += 1
            ref, uref = (r1, u1) if a == 1.0 else (r0, u0)
            if set(got) != set(ref) or unp != uref:
                extra, gone = len(set(got) - set(ref)), len(set(ref) - set(got))
                msgs.append(f'  vbl {v} a={a}: membership differs from tick {"K" if a == 1.0 else "K-1"}: '
                            f'{extra} drawn not in it, {gone} of it not drawn, unpaired {unp}/{uref}')
                fails += 1
            if a != 0.5:
                bad = 0
                for k in set(got) & set(ref):
                    g, r = got[k], ref[k]
                    if any(abs(x - y) > stol for x, y in zip(g['st'], r['st'])) or g['c'] != r['c']:
                        bad += 1
                        continue
                    if len(g['v']) != len(r['v']):
                        bad += 1
                        continue
                    for p, q in zip(g['v'], r['v']):
                        vmax = max(vmax, abs(p[0] - q[0]), abs(p[1] - q[1]))
                        if abs(p[0] - q[0]) > vtol or abs(p[1] - q[1]) > vtol:
                            bad += 1
                            break
                if bad:
                    msgs.append(f'  vbl {v} a={a}: {bad} records differ from tick {"K" if a == 1.0 else "K-1"}')
                    fails += 1
                continue
            bad = 0
            for k, g in got.items():
                if k not in r0:
                    continue
                if k in held:
                    counts['held_drawn'] += 1
                    if any(abs(x - y) > stol for x, y in zip(g['st'], r0[k]['st'])) or g['c'] != r0[k]['c']:
                        bad += 1
                    continue
                s0, s1 = r0[k]['st'], r1[k]['st']
                ok = all(on_segment(g['st'][i], s0[i], s1[i], stol) for i in range(4))
                ok = ok and on_segment(g['st'][4], s0[4], s1[4], stol, wrap=256.0)
                for e in range(2):
                    ok = ok and all(on_segment(g['c'][e][ch], r0[k]['c'][e][ch], r1[k]['c'][e][ch], 1.0)
                                    for ch in range(3))
                if ok and (s0 != s1 or r0[k]['c'] != r1[k]['c']):
                    counts['lerped'] += 1
                bad += 0 if ok else 1
            if bad:
                msgs.append(f'  vbl {v} a=0.5: {bad} records off their K-1..K segment')
                fails += 1
    counts['vmax'] = round(vmax, 4)
    return fails, counts


def check_moby(lines, lo, hi, opts, msgs):
    ctol = float(opts.get('ctol', 0.05))
    want = int(opts['cls'], 16) if 'cls' in opts else None
    # A moby is in the row's scope when any traced present draws it at the
    # row's class: a moby whose class changes between two ticks is one moby.
    # Its identity is the pass, its address and the slot's allocation serial
    # (0 when the line carries none), so a slot handed to a new record is a
    # second identity.
    pres, seen, scope = {}, set(), set()
    for m in lines:
        seen.add((int(m[1]), alpha_bin(m[2])))
        if m[4] not in ('REG', 'SIM'):
            continue
        if want is None or int(m[7], 16) == want:
            scope.add(m[5])
    for m in lines:
        if m[4] not in ('REG', 'SIM') or m[5] not in scope:
            continue
        rec = dict(src=m[3], n=int(m[8]), c=(float(m[9]), float(m[10])),
                   w=(float(m[11]), float(m[12]), float(m[13])))
        ident = m[4] + ':' + m[5] + ':' + (m[6] or '0')
        pres.setdefault((int(m[1]), alpha_bin(m[2])), []).append((ident, rec))
    fails = 0
    counts = dict(intervals=0, held=0, deferred=0, reused=0, held_drawn=0, lerped=0)
    dmax_ghost = dmax_pair = 0.0
    for v in range(lo + 1, hi + 2, 2):
        need = [(v - 2, 1.0), (v, 0.0), (v, 0.5), (v, 1.0)]
        if any(k not in seen for k in need):
            msgs.append(f'  vbl {v}: missing a present ({[k for k in need if k not in seen]})')
            fails += 1
            continue
        lists = {k: pres.get(k, []) for k in need}
        for k, lst in lists.items():
            # By pass and address: a slot drawn as its old and its new record
            # in one present is drawn twice.
            ms = [m.rsplit(':', 1)[0] for m, _ in lst]
            if len(ms) != len(set(ms)):
                msgs.append(f'  vbl {k[0]} a={k[1]}: a moby drawn twice')
                fails += 1
        r0 = dict(lists[(v - 2, 1.0)])
        r1 = dict(lists[(v, 1.0)])
        counts['intervals'] += 1
        held = set(r0) - set(r1)
        counts['held'] += len(held)
        counts['deferred'] += len(set(r1) - set(r0))
        addr1 = {k.rsplit(':', 1)[0] for k in r1}
        counts['reused'] += sum(1 for k in held if k.rsplit(':', 1)[0] in addr1)
        for a in (0.0, 0.5):
            got = dict(lists[(v, a)])
            if set(got) != set(r0):
                msgs.append(f'  vbl {v} a={a}: membership differs from tick K-1: '
                            f'{len(set(got) - set(r0))} drawn not in it, {len(set(r0) - set(got))} of it not drawn')
                fails += 1
            bad = 0
            for m, g in got.items():
                if m not in r0:
                    continue
                if m in held:
                    if a == 0.5:
                        counts['held_drawn'] += 1
                    # The centroid is judged at alpha 0 alone, where the
                    # present's camera is tick K-1's.
                    d = max(abs(g['c'][0] - r0[m]['c'][0]), abs(g['c'][1] - r0[m]['c'][1])) if a == 0.0 else 0.0
                    dmax_ghost = max(dmax_ghost, d)
                    if g['w'] != r0[m]['w'] or g['n'] != r0[m]['n'] or d > ctol:
                        bad += 1
                    continue
                if a == 0.0:
                    d = max(abs(g['c'][0] - r0[m]['c'][0]), abs(g['c'][1] - r0[m]['c'][1]))
                    dmax_pair = max(dmax_pair, d)
                    if any(abs(x - y) > 0.002 for x, y in zip(g['w'], r0[m]['w'])):
                        bad += 1
                else:
                    w0, w1 = r0[m]['w'], r1[m]['w']
                    if not all(on_segment(g['w'][i], w0[i], w1[i], 0.002) for i in range(3)):
                        bad += 1
                    elif w0 != w1:
                        counts['lerped'] += 1
            if bad:
                msgs.append(f'  vbl {v} a={a}: {bad} mobys off tick K-1 or their K-1..K segment')
                fails += 1
    counts['dmax_ghost'] = round(dmax_ghost, 4)
    counts['dmax_pair'] = round(dmax_pair, 4)
    return fails, counts


def main():
    log, idir, cdir, game, bk2, lo, hi, keys = sys.argv[1:9]
    lo, hi = int(lo), int(hi)
    opts = parse_keys(keys)
    kind = opts.get('kind', 'part')
    text = Path(log).read_text(errors='replace')
    msgs = []
    if kind == 'part':
        fails, counts = check_part(list(PART.finditer(text)), lo, hi, opts, msgs)
    else:
        fails, counts = check_moby(list(MOBY.finditer(text)), lo, hi, opts, msgs)
    worst, missing = identity_leg(idir, cdir, list(range(lo - 2, hi + 1, 2)))
    if missing:
        msgs.append(f'  identity leg: captures missing at {missing}')
        fails += 1
    if worst:
        msgs.append(f'  identity leg: alpha 1 differs from canonical in {worst} px')
        fails += 1
    vac = [k for k in ('held', 'deferred', 'reused') if counts[k] < int(opts.get(k, 1))]
    verdict = 'FAIL' if fails else ('VACUOUS' if vac or counts['intervals'] == 0 else 'PASS')
    for line in msgs:
        print(line)
    if vac:
        print(f'  engagement below floor: {", ".join(vac)}')
    print(f'[late-switch] game={game} bk2={bk2} span={lo}-{hi} kind={kind} '
          + ' '.join(f'{k}={v}' for k, v in counts.items())
          + f' identity_px={worst} verdict={verdict}')
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
