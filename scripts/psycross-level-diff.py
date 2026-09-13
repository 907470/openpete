#!/usr/bin/env python3
"""Compose an A/B sheet per level from two psycross-level-corpus.sh runs.

For every tile present in both legs it writes `<level>.png`: the before frame,
the after frame, and their absolute difference amplified so a one-texel shift
is visible, side by side and labelled. It also writes `summary.txt`, the tiles
ranked by how much of the frame moved, and `contact.png`, every level's diff at
thumbnail size so a whole game reads at a glance.

Amplifying the difference is the point: the changes this instrument exists to
show are one-texel resamplings that are invisible at 1:1 and obvious at 8x.
The amplification factor is printed on each sheet so a reader never mistakes an
amplified difference for the size of the visible change.

Usage:
  scripts/psycross-level-diff.py <before-dir> <after-dir> <out-dir> [--gain N]

Exit: 0 = sheets written, 1 = no tile pair found (a leg is empty or the two
legs share no level).
"""
import os
import sys

from PIL import Image, ImageDraw
import numpy as np

GAIN = 8


def tiles(d):
    """level name -> path, over the '-og' tiles a corpus run writes."""
    out = {}
    for fn in os.listdir(d):
        if not fn.endswith("-og.png"):
            continue
        # "<level>-<tick>-og.png"
        stem = fn[: -len("-og.png")]
        level = stem.rsplit("-", 1)[0]
        out[level] = os.path.join(d, fn)
    return out


def label(img, text):
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, img.width, 14], fill=(0, 0, 0))
    d.text((4, 2), text, fill=(255, 255, 255))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    gain = GAIN
    for a in sys.argv[1:]:
        if a.startswith("--gain"):
            gain = int(a.split("=", 1)[1] if "=" in a else a[len("--gain"):])
    if len(args) < 3:
        print(__doc__)
        return 1
    bdir, adir, odir = args[0], args[1], args[2]
    os.makedirs(odir, exist_ok=True)
    for fn in os.listdir(odir):
        if fn.endswith(".png") or fn == "summary.txt":
            os.remove(os.path.join(odir, fn))

    bt, at = tiles(bdir), tiles(adir)
    shared = sorted(set(bt) & set(at))
    if not shared:
        print("no level appears in both legs", file=sys.stderr)
        return 1

    rows = []
    thumbs = []
    for lvl in shared:
        b = Image.open(bt[lvl]).convert("RGB")
        a = Image.open(at[lvl]).convert("RGB")
        if b.size != a.size:
            print(f"  {lvl}: size mismatch {b.size} vs {a.size}", file=sys.stderr)
            continue
        nb, na = np.asarray(b, np.int16), np.asarray(a, np.int16)
        delta = np.abs(na - nb)
        changed = int((delta.max(axis=2) > 0).sum())
        total = b.width * b.height
        pct = 100.0 * changed / total
        peak = int(delta.max())
        rows.append((pct, lvl, changed, total, peak))

        amp = np.clip(delta.astype(np.int32) * gain, 0, 255).astype(np.uint8)
        dimg = Image.fromarray(amp, "RGB")

        w, h = b.size
        sheet = Image.new("RGB", (w * 3 + 8, h), (24, 24, 24))
        for i, (img, cap) in enumerate(
            ((b, f"{lvl}  BEFORE"), (a, f"{lvl}  AFTER"),
             (dimg, f"diff x{gain}  {pct:.2f}% moved, peak {peak}"))
        ):
            c = img.copy()
            label(c, cap)
            sheet.paste(c, (i * (w + 4), 0))
        sheet.save(os.path.join(odir, f"{lvl}.png"))

        t = dimg.resize((w // 4, h // 4))
        label(t, f"{lvl} {pct:.1f}%")
        thumbs.append(t)

    rows.sort(reverse=True)
    with open(os.path.join(odir, "summary.txt"), "w") as fh:
        fh.write(f"{len(rows)} level(s), diff amplified x{gain} in the sheets\n\n")
        fh.write(f"{'pixels moved':>13}  {'peak':>4}  level\n")
        for pct, lvl, changed, total, peak in rows:
            fh.write(f"{pct:11.2f}%  {peak:4d}  {lvl}   ({changed}/{total})\n")

    if thumbs:
        cols = 4
        tw, th = thumbs[0].size
        r = (len(thumbs) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * tw, r * th), (24, 24, 24))
        for i, t in enumerate(thumbs):
            sheet.paste(t, ((i % cols) * tw, (i // cols) * th))
        sheet.save(os.path.join(odir, "contact.png"))

    print(f"{len(rows)} sheet(s) -> {odir}")
    for pct, lvl, _, _, peak in rows[:8]:
        print(f"  {pct:6.2f}%  peak {peak:3d}  {lvl}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
