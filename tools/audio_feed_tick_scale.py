#!/usr/bin/env python3
"""Score the audio device feed's timeline across live tick-scale changes.

Input: the feed timeline (OPENPETE_AUDIO_FEED_LOG) of a run under an
OPENPETE_TICK_SCALE_AT schedule, and the schedule itself. The timeline's
periodic lines come every 250 ms of device time and carry the host's VBL
(tag), the frames queued at the device (dev), the FIFO's input frames
(fifo) and the resampling ratio; the queued depth in device time is
(dev + fifo / ratio) / 44.1 ms. Events UNDERRUN and OVERFLOW are glitches.

Segments: the schedule splits the run at each change's VBL into segments of
constant scale (the first, before any change, is the baseline at 1.0). Per
segment: the depth's median and extremes over its settled part (from
SETTLE_MS of device time after the change), the underruns and overflow
events anywhere in it, and the recovery time: the device time from the
change until the depth falls to, and then stays under, the latency
ceiling (target + band) for the rest of the segment.

The band is BAND_MS plus, below scale 1, the excess of one VBL's push
over 737 frames in ms (the depth's sawtooth grows by it). Pass, per
segment: 0 underruns and 0 overflow events; settled median within the
band of the target, settled maximum under the ceiling, settled minimum at
least FLOOR_MS (one 1024-frame device period: the headroom left before a
starvation); recovery <= RECOVER_MS. Engagement: data plays (a prime) and
the segments have periodic lines.
"""
import argparse
import math
import re
import sys

RATE = 44100.0
BASE_TARGET = 5200


def push_excess(scale):
    """Output frames by which one VBL's push exceeds 737 below scale 1 (the feed's rule)."""
    if scale >= 1.0:
        return 0
    return math.ceil(737.0 / scale) - 737


def target_frames(scale):
    """Depth target of the feed at `scale` (the feed's own rule)."""
    return BASE_TARGET + 2 * push_excess(scale)


def parse_log(path):
    rows = []
    pat = re.compile(r'(\w+)=([-+0-9.]+)')
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            kv = {k: v for k, v in pat.findall(line)}
            try:
                rows.append({
                    'ms': float(parts[0]), 'ev': parts[1], 'tag': int(kv['tag']),
                    'playing': int(kv['playing']), 'dev': int(kv['dev']),
                    'fifo': int(kv['fifo']), 'ratio': float(kv['ratio']),
                    'consumed': int(kv['consumed']),
                })
            except (KeyError, ValueError):
                continue
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', required=True)
    ap.add_argument('--schedule', required=True, help='VBL:SCALE,... as OPENPETE_TICK_SCALE_AT')
    ap.add_argument('--settle-ms', type=float, default=1000.0)
    ap.add_argument('--band-ms', type=float, default=45.0)
    ap.add_argument('--recover-ms', type=float, default=1000.0)
    ap.add_argument('--end-vbl', type=int, default=None)
    ap.add_argument('--floor-ms', type=float, default=1024 / RATE * 1000.0)
    ap.add_argument('--tag', default='')
    a = ap.parse_args()

    sched = [(0, 1.0)]
    for ent in a.schedule.split(','):
        v, s = ent.split(':')
        sched.append((int(v), float(s)))
    rows = parse_log(a.log)
    if not rows:
        print(f'[tick-scale-audio] {a.tag} verdict=FAIL: empty timeline')
        return 2
    first_prime = next((r for r in rows if r['ev'] == 'prime'), None)
    if first_prime is None:
        print(f'[tick-scale-audio] {a.tag} verdict=VACUOUS: no data ever played')
        return 2

    fails = []
    out = []
    for i, (vbl0, scale) in enumerate(sched):
        vbl1 = sched[i + 1][0] if i + 1 < len(sched) else (a.end_vbl or 1 << 30)
        seg = [r for r in rows if vbl0 <= r['tag'] < vbl1 and r['consumed'] >= first_prime['consumed']]
        if not seg:
            out.append(f'seg{i} vbl={vbl0}-{vbl1} scale={scale:g} shorter than one timeline line')
            continue
        # Device time from the change: the first line at or past vbl0.
        c_change = seg[0]['consumed']
        tgt_ms = target_frames(scale) / RATE * 1000.0
        # The band widens with the target: one VBL's push spans more output
        # frames below scale 1, and the depth's sawtooth with it.
        band = a.band_ms + push_excess(scale) / RATE * 1000.0
        under = sum(1 for r in seg if r['ev'] == 'UNDERRUN')
        over = sum(1 for r in seg if r['ev'] == 'OVERFLOW')
        per = [r for r in seg if r['ev'] == 'tick' and r['playing']]
        depth = [((r['consumed'] - c_change) / RATE * 1000.0,
                  (r['dev'] + r['fifo'] / r['ratio']) / RATE * 1000.0) for r in per]
        settled = [d for t, d in depth if t >= (0 if i == 0 else a.settle_ms)]
        rec = None
        if i > 0:
            for k in range(len(depth)):
                if all(d <= tgt_ms + band for _, d in depth[k:]):
                    rec = depth[k][0]
                    break
        if settled:
            s = sorted(settled)
            med, lo, hi = s[len(s) // 2], s[0], s[-1]
        else:
            med = lo = hi = float('nan')
        line = (f'seg{i} vbl={vbl0}-{vbl1 if vbl1 < 1 << 30 else "end"} scale={scale:g} '
                f'target={tgt_ms:.0f}+-{band:.0f}ms depth_med={med:.0f}ms min={lo:.0f}ms max={hi:.0f}ms '
                f'underruns={under} overflow={over} '
                f'recovery={"-" if i == 0 else ("never" if rec is None else f"{rec:.0f}ms")}')
        out.append(line)
        seg_fail = []
        if not settled:
            seg_fail.append('no settled lines')
        if under:
            seg_fail.append(f'underruns {under}')
        if over:
            seg_fail.append(f'overflow {over}')
        if settled and abs(med - tgt_ms) > band:
            seg_fail.append(f'median depth outside target+-{band:.0f}ms')
        if settled and hi > tgt_ms + band:
            seg_fail.append(f'max depth over {tgt_ms + band:.0f}ms')
        if settled and lo < a.floor_ms:
            seg_fail.append(f'min depth under {a.floor_ms:.0f}ms')
        if i > 0 and (rec is None or rec > a.recover_ms):
            seg_fail.append(f'recovery > {a.recover_ms:g}ms')
        if seg_fail:
            fails.append(f'seg{i}: ' + ', '.join(seg_fail))
    for l in out:
        print(f'[tick-scale-audio] {a.tag} {l}')
    verdict = 'PASS' if not fails else 'FAIL: ' + '; '.join(fails)
    print(f'[tick-scale-audio] {a.tag} band={a.band_ms:g}ms floor={a.floor_ms:.0f}ms settle={a.settle_ms:g}ms '
          f'recover<={a.recover_ms:g}ms verdict={verdict}')
    return 0 if not fails else 2


if __name__ == '__main__':
    sys.exit(main())
