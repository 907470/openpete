#!/usr/bin/env python3
"""presentation-ident-check.py — the sub-tick spine's identity check.

The corpus captures an `ident` leg: the intermediate re-extract phased onto the
tick stamp (OPENPETE_INTERMEDIATE_ALPHA=0) on every canonical frame. A complete
spine reproduces the canonical dump of that frame pixel for pixel, so each
`<row>__<lod>ident__<f>-vk.png` is diffed against `<row>__<lod>canon__<f>-vk.png`
from the SAME candidate directory. No golden image is involved.

The target is zero. Until every producer reaches it, a per-tile baseline
(tests/golden/spyro-1/presentation/ident-baseline.tsv: tile, differing pixels)
holds the current residual; a tile fails when it exceeds its baseline, and a
tile without a baseline row fails on any difference. --bless rewrites the
baseline from the candidate (review the diff before committing it: a raised
count is a spine regression, not a re-bless).

Exit: 0 = every pair within baseline, 1 = a pair exceeds it / an ident tile has
no canon twin / no pair at all (a capture that produced no identity pair is
vacuous, not clean), 64 = SKIP (PIL missing / candidate dir absent).
"""
import argparse, os, sys
try:
    from PIL import Image, ImageChops
except ImportError:
    print("[presentation-ident] SKIP: python3-PIL missing", file=sys.stderr)
    sys.exit(64)

DEFAULT_BASELINE = "tests/golden/spyro-1/presentation/ident-baseline.tsv"


def diff_count(a, b):
    A = Image.open(a).convert("RGB")
    B = Image.open(b).convert("RGB")
    if A.size != B.size:
        return A.size[0] * A.size[1] + B.size[0] * B.size[1]
    d = ImageChops.difference(A, B).convert("L").point(lambda v: 255 if v else 0)
    return d.histogram()[255]


def read_baseline(path):
    base = {}
    if os.path.isfile(path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                name, n = line.split("\t")
                base[name] = int(n)
    return base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("candidate", help="directory holding both the canon and ident -vk PNGs")
    ap.add_argument("--baseline", default=DEFAULT_BASELINE)
    ap.add_argument("--bless", action="store_true", help="rewrite the baseline from this candidate")
    args = ap.parse_args()
    if not os.path.isdir(args.candidate):
        print(f"[presentation-ident] SKIP: no candidate dir {args.candidate}")
        return 64
    idents = sorted(f for f in os.listdir(args.candidate) if "__ident__" in f and f.endswith("-vk.png"))
    if not idents:
        print("[presentation-ident] FAIL: no __ident__ tiles captured (vacuous)")
        return 1
    base = read_baseline(args.baseline)
    fails = 0
    counts = {}
    total_px = 0
    for name in idents:
        canon = name.replace("__ident__", "__canon__")
        cpath = os.path.join(args.candidate, canon)
        if not os.path.isfile(cpath):
            print(f"NOCANON  {name}  (no {canon} in the candidate)"); fails += 1
            continue
        n = diff_count(cpath, os.path.join(args.candidate, name))
        counts[name] = n
        total_px += n
        allowed = base.get(name, 0)
        if n > allowed:
            print(f"EXCEED   {name}  pixels={n} baseline={allowed}"); fails += 1
        elif n:
            print(f"within   {name}  pixels={n} baseline={allowed}")
        else:
            print(f"ident    {name}")
    exact = sum(1 for n in counts.values() if n == 0)
    print(f"[presentation-ident] {len(counts)} pairs, {exact} exact, "
          f"{len(counts) - exact} within baseline or exceeding, {fails} failing, "
          f"{total_px} differing pixels in total")
    if args.bless:
        os.makedirs(os.path.dirname(args.baseline), exist_ok=True)
        with open(args.baseline, "w") as f:
            f.write("# tile\tdiffering pixels vs the canon tile of the same frame (target 0)\n")
            for name in sorted(counts):
                f.write(f"{name}\t{counts[name]}\n")
        print(f"[presentation-ident] baseline written: {args.baseline}")
        return 0
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
