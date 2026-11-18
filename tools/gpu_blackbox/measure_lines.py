#!/usr/bin/env python3
"""measure_lines.py - measure which pixels the reference GPU draws for a line.

Plays every line from the start pixel (100, 100) to each end offset
(dx, dy) in [-64, 64]^2 on duckstation-regtest's software GPU (probe_run.py)
and writes tests/unit/data/gpu-line/line.pixels. The same sweep is repeated
with the vertices in reverse order, from two other start pixels and as a
two-colour (Gouraud) line; the script reports whether each repeat covers the
same pixels as the main sweep, shifted to the same start, and writes the
result into the table header.

Usage (openpete checkout root, about two minutes):
    tools/gpu_blackbox/measure_lines.py WORKDIR [--out PATH]
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from probe_run import run_cases, applied_head   # noqa: E402

R = 64
START = (100, 100)


def sweep(start, rev=False, gouraud=False):
    sx, sy = start
    cases = []
    for dy in range(-R, R + 1):
        for dx in range(-R, R + 1):
            ex, ey = sx + dx, sy + dy
            a, b = ((ex, ey), (sx, sy)) if rev else ((sx, sy), (ex, ey))
            x0 = (min(sx, ex) - 2) // 16 * 16
            x1 = -(-(max(sx, ex) + 3) // 16) * 16
            y0, y1 = min(sy, ey) - 2, max(sy, ey) + 3
            if gouraud:
                words = [0x50FFFFFF, (a[1] << 16) | a[0], 0x00FFFFFF, (b[1] << 16) | b[0]]
            else:
                words = [0x40FFFFFF, (a[1] << 16) | a[0], (b[1] << 16) | b[0]]
            cases.append((x0, y0, x1 - x0, y1 - y0, words))
    return cases


def rel(px, start):
    return frozenset((x - start[0], y - start[1]) for (x, y) in px)


def encode(dx, dy, pts):
    """The pixels of one line as major-axis-ordered minor-step bits, hex; None if not that shape."""
    k = max(abs(dx), abs(dy))
    xmaj = abs(dx) >= abs(dy)
    sgn_major = (1 if (dx if xmaj else dy) >= 0 else -1)
    by_major = {}
    for (x, y) in pts:
        m, n = (x, y) if xmaj else (y, x)
        if m in by_major:
            return None
        by_major[m] = n
    want = [sgn_major * i for i in range(k + 1)]
    if sorted(by_major) != sorted(want):
        return None
    minors = [by_major[m] for m in want]
    if minors[0] != 0:
        return None
    sgn_minor = 1 if (dy if xmaj else dx) >= 0 else -1
    bits = 0
    for i in range(k):
        step = minors[i + 1] - minors[i]
        if step not in (0, sgn_minor):
            return None
        if step:
            bits |= 1 << i
    return "%x" % bits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--out", default="tests/unit/data/gpu-line/line.pixels")
    a = ap.parse_args()
    work = pathlib.Path(a.work)
    fwd = run_cases(sweep(START), work / "fwd", est_pixels=lambda c: 2 * max(c[2], c[3]) + 16)
    main_sets = [rel(p, START) for p in fwd]
    empty = sum(1 for p in fwd if not p)
    badval = sum(1 for p in fwd if set(p.values()) != {0x7FFF})
    if empty or badval:
        raise SystemExit("main sweep: %d lines drew nothing, %d drew another value" % (empty, badval))
    facts = []
    for label, start, kw in (("vertices in reverse order", START, {"rev": True}),
                             ("start pixel (357, 301)", (357, 301), {}),
                             ("start pixel (611, 194)", (611, 194), {}),
                             ("two-colour (Gouraud) line", START, {"gouraud": True})):
        res = run_cases(sweep(start, **kw), work / label.split()[0], est_pixels=lambda c: 2 * max(c[2], c[3]) + 16)
        diff = sum(1 for i, p in enumerate(res) if rel(p, start) != main_sets[i])
        facts.append("# %s: %d of %d lines cover other pixels than the table's." % (label, diff, len(res)))
        print(facts[-1])
    rows = []
    i = 0
    for dy in range(-R, R + 1):
        for dx in range(-R, R + 1):
            enc = encode(dx, dy, main_sets[i])
            if enc is None:
                raise SystemExit("line (%d, %d) is not one pixel per major-axis step; the format cannot hold it" % (dx, dy))
            rows.append("%d %d %s" % (dx, dy, enc))
            i += 1
    hdr = [
        "# The pixels the reference GPU draws for a line: a measurement, not a derivation.",
        "# Taken by tools/gpu_blackbox/measure_lines.py: a PS-EXE (tools/gpu_blackbox/probe.c) booted in the",
        "# oracle reference test binary duckstation-regtest (third_party/duckstation-patches, applied head",
        "# %s), software GPU renderer, sends one GP0 0x40 monochrome line per case" % applied_head(),
        "# (colour 0xFFFFFF, drawing area (0, 0)-(1023, 511), drawing offset 0, no mask) from the start pixel",
        "# (100, 100) to (100 + dx, 100 + dy) and reads VRAM back (GP0 0xC0) over the line's bounding box",
        "# widened by 2 pixels; main RAM is dumped at the end (regtest -ram-dump-at).",
        "# Regenerate (openpete checkout root, about two minutes): tools/gpu_blackbox/measure_lines.py WORKDIR",
        "# Every case drew; each drew exactly one pixel for every major-axis coordinate from the start to",
        "# the end pixel inclusive (the major axis is x when |dx| >= |dy|, else y), both endpoints included,",
        "# no pixel outside the endpoints' bounding box.",
        "# Repeats of the whole sweep, compared with the table after shifting to the same start pixel:",
    ] + facts + [
        "# Rows: dx dy bits, one per end offset, dy then dx ascending over [-64, 64]. The pixels in",
        "# major-axis order from the start are p0 = (100, 100), ..., pk = (100 + dx, 100 + dy), k = max(|dx|, |dy|);",
        "# pixel i+1 is one major-axis step toward the end from pixel i, and also one minor-axis step toward",
        "# the end when bit i of the hexadecimal number bits is set.",
    ]
    out = pathlib.Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(hdr + rows) + "\n")
    print("wrote %s: %d lines, %d pixels" % (out, len(rows), sum(len(s) for s in main_sets)))


if __name__ == "__main__":
    main()
