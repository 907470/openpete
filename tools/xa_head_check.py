#!/usr/bin/env python3
"""Check the XA decoder's sector head against the drive's guest-time rate.

Input: an OPENPETE_XA_LOG file (`h <vbl> <lba>` once per VBL while a stream
plays, `x <vbl>` when it stops). A stream segment ends at an `x` line or at
an LBA step outside [0, 64] between consecutive lines (a new stream start).
Per segment spanning at least --min-span VBL, the head's rate is the least
squares slope of LBA on VBL after the first --skip VBL (the seek, during
which the decoder holds at the start LBA). A double-speed drive reads 150 sectors per
second, 37609/15000 = 2.507267 LBA per VBL at 2250000/37609 VBL per second.

Pass: every qualifying segment's slope within --tol of that rate.
Engagement: qualifying segments; zero is VACUOUS and fails.
Verdict line: [xa-head] segments=N vbl_spanned=V slope_min=.. slope_max=..
verdict=PASS|FAIL|VACUOUS. Exit 0 PASS, 2 otherwise.
"""
import argparse
import sys

RATE = 37609 / 15000


def segments(path):
    """Yield lists of (vbl, lba) per stream segment of the log at path."""
    cur = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p or p[0].startswith("#"):
                continue
            if p[0] == "x":
                if cur:
                    yield cur
                cur = []
                continue
            if p[0] != "h":
                continue
            v, s = int(p[1]), int(p[2])
            if cur and not 0 <= s - cur[-1][1] <= 64:
                yield cur
                cur = []
            cur.append((v, s))
    if cur:
        yield cur


def slope(pts):
    """Least squares slope of the second coordinate on the first."""
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    sxx = sum((p[0] - mx) ** 2 for p in pts)
    sxy = sum((p[0] - mx) * (p[1] - my) for p in pts)
    return sxy / sxx


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--min-span", type=int, default=1000)
    ap.add_argument("--skip", type=int, default=120)
    ap.add_argument("--tol", type=float, default=0.0005)
    ap.add_argument("--tag", default="")
    a = ap.parse_args()
    n = spanned = 0
    bad = 0
    rates = []
    for seg in segments(a.log):
        v0, s0 = seg[0]
        v1, s1 = seg[-1]
        body = [p for p in seg if p[0] >= v0 + a.skip]
        ok_span = v1 - v0 >= a.min_span and len(body) >= 3
        r = slope(body) if len(body) >= 3 else float("nan")
        verdict = "skip(short)"
        if ok_span:
            n += 1
            spanned += v1 - v0
            rates.append(r)
            good = abs(r - RATE) <= a.tol
            bad += not good
            verdict = "ok" if good else "FAIL"
        print(f"segment vbl {v0}-{v1} LBA {s0}-{s1}: slope {r:.5f} "
              f"(drive {RATE:.5f}, diff {r - RATE:+.5f}) {verdict}")
    v = "VACUOUS" if n == 0 else ("FAIL" if bad else "PASS")
    rng = f"slope_min={min(rates):.5f} slope_max={max(rates):.5f}" if rates else "slope_min=- slope_max=-"
    print(f"[xa-head] {a.tag + ' ' if a.tag else ''}segments={n} vbl_spanned={spanned} {rng} "
          f"rate={RATE:.5f} tol={a.tol} verdict={v}")
    return 0 if v == "PASS" else 2


if __name__ == "__main__":
    sys.exit(main())
