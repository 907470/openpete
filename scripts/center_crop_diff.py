#!/usr/bin/env python3
"""center_crop_diff.py — pixel-identity check on the central 4:3 region.

Both inputs must share the same height. The candidate is wider than (or
equal to) the baseline; we center-crop both to the requested CROP_W x
CROP_H box and pixel-diff. Writes a magnified diff image when divergent
so the failure shows *where* in the frame the leak surfaces.
"""
from __future__ import annotations
import argparse
import sys
from PIL import Image, ImageChops


def center_crop(img: Image.Image, w: int, h: int) -> Image.Image:
    iw, ih = img.size
    if iw < w or ih < h:
        sys.exit(f"image {iw}x{ih} smaller than crop {w}x{h}")
    left = (iw - w) // 2
    top = (ih - h) // 2
    return img.crop((left, top, left + w, top + h))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--candidate", required=True)
    ap.add_argument("--crop-w", type=int, required=True)
    ap.add_argument("--crop-h", type=int, required=True)
    ap.add_argument("--overscan-top", type=int, default=0,
                    help="rows to drop from top before comparing (PSX overscan)")
    ap.add_argument("--overscan-bot", type=int, default=0,
                    help="rows to drop from bottom before comparing")
    ap.add_argument("--tolerance", type=int, default=0,
                    help="max per-channel abs diff that still passes")
    ap.add_argument("--diff-out", default=None,
                    help="write a magnified diff image when divergent")
    args = ap.parse_args()

    base = Image.open(args.baseline).convert("RGB")
    cand = Image.open(args.candidate).convert("RGB")
    if base.size[1] != cand.size[1]:
        print(f"height mismatch: baseline {base.size}, candidate {cand.size}",
              file=sys.stderr)
        return 1

    def trim_overscan(img):
        if args.overscan_top or args.overscan_bot:
            iw, ih = img.size
            return img.crop((0, args.overscan_top, iw, ih - args.overscan_bot))
        return img

    base_c = center_crop(trim_overscan(base), args.crop_w, args.crop_h)
    cand_c = center_crop(trim_overscan(cand), args.crop_w, args.crop_h)

    diff = ImageChops.difference(base_c, cand_c)
    bbox = diff.getbbox()
    if bbox is None:
        print(f"identical (crop {args.crop_w}x{args.crop_h})")
        return 0

    # Histogram of max-abs per pixel
    extrema = diff.getextrema()              # ((rmin,rmax),(gmin,gmax),(bmin,bmax))
    worst = max(hi for _, hi in extrema)
    n_diff = 0
    n_total = args.crop_w * args.crop_h
    for r, g, b in diff.getdata():
        if max(r, g, b) > args.tolerance:
            n_diff += 1
    pct = 100.0 * n_diff / n_total

    # Histogram divergence by per-pixel max-channel-abs threshold.
    # A 1-px-thick silhouette outline at high thresholds means sub-
    # pixel jitter (precision leak); a thick band at high thresholds
    # means a translated region (gameplay drift). The shape of the
    # decay across thresholds tells the difference at a glance.
    bucket_thresholds = (0, 1, 4, 16, 32, 64, 128)
    buckets = {t: 0 for t in bucket_thresholds}
    for r, g, b in diff.getdata():
        d = max(r, g, b)
        for t in bucket_thresholds:
            if d > t:
                buckets[t] += 1
    print(f"divergent pixels: {n_diff}/{n_total} ({pct:.3f}%) "
          f"worst per-channel diff: {worst}  bbox: {bbox}")
    print("histogram (>thresh): " + "  ".join(
        f">{t}={buckets[t]} ({100.0*buckets[t]/n_total:.2f}%)"
        for t in bucket_thresholds))
    if args.diff_out:
        # Amplify diff so subtle channel differences are visible
        amp = diff.point(lambda v: min(255, v * 8))
        amp.save(args.diff_out)
        print(f"diff image -> {args.diff_out}")

    return 0 if worst <= args.tolerance else 1


if __name__ == "__main__":
    sys.exit(main())
