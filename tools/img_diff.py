#!/usr/bin/env python3
"""img_diff.py — pixel-wise PNG comparison with HD-upscaling tolerance.

Usage:
    python3 tools/img_diff.py A.png B.png [--tolerance F] [--diff-output PATH]

Tolerance is the average per-pixel L2 distance over the 0-255 channel range
(roughly: 0 = identical, 0.05 ≈ 5% diff, 1.0 = totally different).

Exits 0 if score <= tolerance; 1 otherwise. Prints a one-line summary to
stdout. Writes a red-overlay diff PNG to --diff-output if specified.

Dependencies: Pillow (pip install pillow). No other deps so the developer loop
can run this without a heavy ML stack.
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("a", type=Path, help="first image (e.g. captured shot)")
    p.add_argument("b", type=Path, help="second image (e.g. golden reference)")
    p.add_argument("--tolerance", type=float, default=0.05,
                   help="max acceptable mean per-pixel diff (0..1, default 0.05)")
    p.add_argument("--diff-output", type=Path, default=None,
                   help="write a red-overlay diff PNG here on failure")
    args = p.parse_args()

    try:
        from PIL import Image
    except ImportError:
        print("img_diff: Pillow not installed; run: pip install pillow",
              file=sys.stderr)
        return 2

    a = Image.open(args.a).convert("RGBA")
    b = Image.open(args.b).convert("RGBA")
    if a.size != b.size:
        # Resize the smaller to the larger so HD-upscaled goldens still compare.
        target = (max(a.size[0], b.size[0]), max(a.size[1], b.size[1]))
        a = a.resize(target, Image.NEAREST)
        b = b.resize(target, Image.NEAREST)

    a_bytes = a.tobytes()
    b_bytes = b.tobytes()
    n = len(a_bytes)
    total_sq = 0
    diffs = bytearray(n)
    for i in range(n):
        d = a_bytes[i] - b_bytes[i]
        total_sq += d * d
        diffs[i] = abs(d)

    # Mean L2 distance, normalized to 0..1 against full 4-channel range.
    mean_l2 = math.sqrt(total_sq / n) / 255.0
    ok = mean_l2 <= args.tolerance

    print(f"img_diff: a={args.a} b={args.b} score={mean_l2:.4f} "
          f"tolerance={args.tolerance:.4f} {'ok' if ok else 'FAIL'}")

    if not ok and args.diff_output is not None:
        # Red highlight on any pixel where any channel differs.
        out = Image.new("RGBA", a.size)
        out_pix = bytearray(n)
        for i in range(0, n, 4):
            d = diffs[i] | diffs[i + 1] | diffs[i + 2]
            if d:
                out_pix[i + 0] = 255
                out_pix[i + 1] = 0
                out_pix[i + 2] = 0
                out_pix[i + 3] = 255
            else:
                out_pix[i + 0] = a_bytes[i + 0] // 4
                out_pix[i + 1] = a_bytes[i + 1] // 4
                out_pix[i + 2] = a_bytes[i + 2] // 4
                out_pix[i + 3] = 255
        out.frombytes(bytes(out_pix))
        out.save(args.diff_output)
        print(f"img_diff: wrote {args.diff_output}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
