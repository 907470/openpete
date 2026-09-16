#!/usr/bin/env python3
"""presentation-corpus-diff.py — compare a candidate corpus to the committed golden.

The presentation golden is a committed set of native -vk PNGs (canonical +
deterministic alpha=0.5 intermediate) over the bk2 corpus. This gate diffs a
freshly captured candidate directory against it:

  - MISSING / EXTRA files are failures (the corpus shape must match). Under
    --expect-subset a golden tile the candidate does not hold is counted and
    not a failure, for a run scoped to part of the corpus by a capture filter;
    an EXTRA tile still fails, since a captured tile with no golden is a shape
    change however the run was scoped.
  - Any pixel difference is a divergence; the exit code is nonzero if ANY file
    diverges. For a presentation-NEUTRAL change that is a defect to fix; for an
    intended visual change it is the adjudication worklist (compare against the
    PSX -og reference, then promote with presentation-corpus-promote.sh).

For each divergent pair a heat-diff PNG (8x-amplified abs difference) is written
to --diff-out so the change is eyeballable at a glance.

--game selects the golden directory (tests/golden/<game>/presentation).

Exit: 0 = candidate byte-matches golden, 1 = divergence/missing/extra,
      64 = SKIP (PIL missing / golden or candidate dir absent).

See notes/plans/openpete-presentation-golden.md.
"""
import argparse, os, sys

try:
    from PIL import Image, ImageChops
except ModuleNotFoundError:
    print("[presentation-diff] SKIP: python3-PIL missing", file=sys.stderr)
    sys.exit(64)

GAMES = ("spyro-1", "spyro-2", "spyro-3")


def golden_dir(game):
    return f"tests/golden/{game}/presentation"


def list_vk(d):
    # __ident__ tiles are the alpha-0 identity leg: compared against the canon
    # tile of the same capture by presentation-ident-check.py, never golden.
    return sorted(f for f in os.listdir(d)
                  if f.endswith("-vk.png") and "__ident__" not in f) if os.path.isdir(d) else []


def diff_pair(golden, cand, diff_out):
    """Return (n_divergent_pixels, max_channel_abs). Writes a heat PNG if divergent."""
    A = Image.open(golden).convert("RGB")
    B = Image.open(cand).convert("RGB")
    if A.size != B.size:
        return (A.size[0] * A.size[1] + B.size[0] * B.size[1], 255)  # size mismatch = all-diff
    d = ImageChops.difference(A, B)
    bbox = d.getbbox()
    if bbox is None:
        return (0, 0)
    # Count pixels with any channel delta, and the worst channel delta.
    px = d.load()
    W, H = d.size
    n = 0
    mx = 0
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            m = r if r > g else g
            if b > m:
                m = b
            if m:
                n += 1
                if m > mx:
                    mx = m
    if diff_out and n:
        # 8x-amplified heat image so small deltas are visible.
        os.makedirs(os.path.dirname(diff_out), exist_ok=True)
        heat = d.point(lambda v: min(255, v * 8))
        heat.save(diff_out)
    return (n, mx)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("candidate", help="directory of freshly captured -vk PNGs")
    ap.add_argument("--game", default="spyro-1", choices=GAMES,
                    help="game slug; selects the golden directory")
    ap.add_argument("--golden", default=None,
                    help="golden directory (default: tests/golden/<game>/presentation)")
    ap.add_argument("--diff-out", default=None, help="dir for heat-diff PNGs (default: <candidate>/diff)")
    ap.add_argument("--expect-subset", action="store_true",
                    help="the candidate covers part of the corpus: golden tiles it "
                         "does not hold are counted, not failed")
    args = ap.parse_args()
    if args.golden is None:
        args.golden = golden_dir(args.game)

    if not os.path.isdir(args.candidate):
        print(f"[presentation-diff] SKIP: no candidate dir {args.candidate}")
        return 64
    if not os.path.isdir(args.golden):
        print(f"[presentation-diff] SKIP: no golden dir {args.golden} "
              f"(bless one with presentation-corpus-promote.sh)")
        return 64

    gold = list_vk(args.golden)
    cand = list_vk(args.candidate)
    gset, cset = set(gold), set(cand)

    diff_out = args.diff_out or os.path.join(args.candidate, "diff")
    fails = 0

    missing = sorted(gset - cset)
    extra = sorted(cset - gset)
    if args.expect_subset:
        print(f"SUBSET   {len(missing)} golden tile(s) outside the captured scope, not compared")
    else:
        for m in missing:
            print(f"MISSING  {m}  (in golden, not captured)"); fails += 1
    for e in extra:
        print(f"EXTRA    {e}  (captured, not in golden)"); fails += 1

    common = sorted(gset & cset)
    if not common and not (missing or extra):
        print("[presentation-diff] SKIP: golden and candidate are both empty")
        return 64
    if not common:
        print("[presentation-diff] FAIL: VACUOUS — no candidate tile has a golden twin")
        return 1

    divergent = []
    for name in common:
        heat = os.path.join(diff_out, name.replace("-vk.png", "-diff.png"))
        n, mx = diff_pair(os.path.join(args.golden, name),
                          os.path.join(args.candidate, name), heat)
        if n:
            divergent.append((name, n, mx))
            fails += 1
            print(f"DIVERGE  {name}  pixels={n} maxch={mx}")
        else:
            print(f"ok       {name}")

    miss_label = "outside scope" if args.expect_subset else "missing"
    print(f"[presentation-diff] {args.game}: {len(common)} compared, {len(divergent)} divergent, "
          f"{len(missing)} {miss_label}, {len(extra)} extra")
    if divergent:
        print(f"[presentation-diff] heat diffs in {diff_out}/ — adjudicate reference-first, "
              f"then promote with scripts/presentation-corpus-promote.sh")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
