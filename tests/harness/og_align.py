#!/usr/bin/env python3
"""og_align.py -- score native tiles against the PsyCross -og reference, raw
and aligned.

The -og reference rasterises the GTE's integer screen coordinates (SXY =
MAC0 >> 16, a floor), while the native renderer places every vertex at its
unrounded projection; og content therefore sits up-left of native content by
the mean floor residual, about half a PSX pixel per axis (one output pixel in
y and 0.6-0.8 in x at 640x432, twice that at 1280x864). A per-pixel |tile-og|
taken without alignment scores that displacement on every edge and texel
boundary, so a change that moves an edge toward its true position can read as
moving away from og. This tool fits one translation of og onto the native
tile over pixels outside the change, then reports |tile-og| over the changed
pixels both raw and with og resampled by the fit.

The fit maximises the correlation of gradient magnitudes (edges, not
colours: og samples textures affinely and draws its own 1-px seams, so its
colours are not truth). It searches whole output pixels first, then a
1/8-pixel grid around the best integer shift. A reference narrower than the
tile (the og leg stays 4:3 while a Hor+ tile is wider) is compared over the
tile's centre columns, where Hor+ keeps the 4:3 image at the same scale.

Usage:
  tests/harness/og_align.py A.png [B.png] OG.png [--box x0,x1,y0,y1]
      [--max-shift PX] [--json]
With B, the scored pixels are those where A and B differ and the fit uses
the pixels at least 3 px away from them inside their bounding box grown by
--pad; without B, every pixel is scored and fitted. --box limits both to a rectangle of the tile.

Output: the fitted shift (og content displaced by dx, dy output pixels from
the native content; negative is up-left) in output and in PSX pixels, the
correlation, the near-best spread, and per tile the raw and aligned mean
|tile-og| over the scored pixels, plus for an A/B pair how many scored pixels
each side is closer on and the aligned verdict: A or B when that side's
mean is lower at every near-best shift, else undecided. Exit 0 = scored, 1 = nothing to score or to fit, 64 =
usage.
"""
import argparse
import json
import sys

import numpy as np
from PIL import Image
from scipy import ndimage


def load_rgb(path):
    """Decode a PNG to a float RGB array."""
    with Image.open(path) as im:
        return np.asarray(im.convert("RGB")).astype(np.float64)


def centre_columns(tile_w, og_w):
    """Whole-column offset of the og image inside a wider Hor+ tile (the
    floor of the exact centring offset); 0 when equal."""
    if og_w > tile_w:
        raise ValueError("og wider than the tile (%d > %d)" % (og_w, tile_w))
    return (tile_w - og_w) // 2


def gradient(img, sigma):
    """Gradient magnitude of the luma of an RGB array after a Gaussian blur."""
    g = ndimage.gaussian_filter(img.mean(axis=2), sigma)
    gy, gx = np.gradient(g)
    return np.hypot(gx, gy)


def shifted(img, dx, dy):
    """og content moved by (dx, dy): out(x, y) = img(x + dx, y + dy).

    Bilinear; edge pixels repeat."""
    return ndimage.shift(img, (-dy, -dx), order=1, mode="nearest")


def masked_corr(a, b, m):
    """Pearson correlation of a and b over the boolean mask m."""
    x, y = a[m], b[m]
    x = x - x.mean()
    y = y - y.mean()
    d = np.sqrt((x * x).sum() * (y * y).sum())
    return float((x * y).sum() / d) if d > 0 else -1.0


def fit_shift(native, og, fit_mask, max_shift):
    """Fit the translation of og content relative to native over fit_mask.

    Returns a dict: dx, dy (output pixels, og(x + dx, y + dy) ~ native(x,
    y)), corr at the fit, and near (the ranges of dx and dy whose
    correlation is within 0.005 of the best) or None when the mask is
    empty."""
    if fit_mask.sum() < 256:
        return None
    sigma = 0.7 * max(1.0, native.shape[0] / 432.0)
    gn, go = gradient(native, sigma), gradient(og, sigma)
    r = int(np.ceil(max_shift))
    m = fit_mask.copy()
    m[:r + 1, :] = m[-r - 1:, :] = False
    m[:, :r + 1] = m[:, -r - 1:] = False
    best = (-2.0, 0.0, 0.0)
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            c = masked_corr(gn, shifted(go, dx, dy), m)
            if c > best[0]:
                best = (c, float(dx), float(dy))
    scores = []
    for fy in np.arange(-0.875, 0.876, 0.125):
        for fx in np.arange(-0.875, 0.876, 0.125):
            dx, dy = best[1] + fx, best[2] + fy
            scores.append((masked_corr(gn, shifted(go, dx, dy), m), dx, dy))
    c, dx, dy = max(scores)
    near = [(x, y) for cc, x, y in scores if cc > c - 0.005]
    return {"dx": dx, "dy": dy, "corr": c, "near": near,
            "near_dx": [min(x for x, _ in near), max(x for x, _ in near)],
            "near_dy": [min(y for _, y in near), max(y for _, y in near)]}


def mean_abs(a, b, m):
    """Mean summed-channel |a - b| over mask m."""
    return float(np.abs(a - b).sum(axis=2)[m].mean())


def score(tiles, og, box=None, max_shift=3.0, pad=64):
    """Score one or two native tiles against og, raw and aligned.

    tiles: [A] or [A, B] as float RGB arrays of one size; og: float RGB, as
    tall as the tiles and no wider. For a pair the fit window is the bounding
    box of the scored pixels grown by pad pixels, so the fit measures the
    displacement where the change is and a distant 2D overlay (drawn at the
    same integer positions in both legs, so unshifted) does not pull it.
    Returns the result dict or None when nothing is scored."""
    a = tiles[0]
    h, w = a.shape[:2]
    if og.shape[0] != h:
        raise ValueError("og height %d != tile height %d" % (og.shape[0], h))
    x0 = centre_columns(w, og.shape[1])
    tiles = [t[:, x0:x0 + og.shape[1]] for t in tiles]
    h, w = tiles[0].shape[:2]
    region = np.zeros((h, w), bool)
    if box:
        bx0, bx1, by0, by1 = box
        region[max(0, by0):by1, max(0, bx0 - x0):max(0, bx1 - x0)] = True
    else:
        region[:] = True
    if len(tiles) == 2:
        changed = (tiles[0] != tiles[1]).any(axis=2)
        scored = changed & region
        near = np.zeros((h, w), bool)
        if scored.any():
            ys, xs = np.nonzero(scored)
            near[max(0, ys.min() - pad):ys.max() + pad + 1,
                 max(0, xs.min() - pad):xs.max() + pad + 1] = True
        fit_mask = region & near & ~ndimage.binary_dilation(changed,
                                                            iterations=3)
    else:
        scored = region
        fit_mask = region
    if not scored.any():
        return None
    fit = fit_shift(tiles[0], og, fit_mask, max_shift)
    out = {"tile_w": int(a.shape[1]), "og_w": int(og.shape[1]),
           "col_offset": int(x0), "scored": int(scored.sum()), "fit": fit}
    psx_sx, psx_sy = w / 512.0, h / 216.0 if h % 216 == 0 else h / 224.0
    if fit:
        # A Hor+ tile of odd excess width centres og on a half column; the
        # PSX-pixel shift is measured from that exact centre.
        frac = (a.shape[1] - og.shape[1]) / 2.0 - x0
        fit["psx_dx"] = (fit["dx"] + frac) / psx_sx
        fit["psx_dy"] = fit["dy"] / psx_sy
        ogal = np.stack([shifted(og[..., i], fit["dx"], fit["dy"])
                         for i in range(3)], axis=2)
    names = "AB"
    for i, t in enumerate(tiles):
        out[names[i]] = {"raw": mean_abs(t, og, scored)}
        if fit:
            out[names[i]]["aligned"] = mean_abs(t, ogal, scored)
    if len(tiles) == 2:
        for key, ref in (("raw", og), ("aligned", ogal if fit else None)):
            if ref is None:
                continue
            da = np.abs(tiles[0] - ref).sum(axis=2)[scored]
            db = np.abs(tiles[1] - ref).sum(axis=2)[scored]
            out["closer_" + key] = {"A": int((da < db).sum()),
                                    "B": int((db < da).sum())}
        if fit:
            # The verdict is decided only when every near-best shift agrees.
            a_wins = 0
            for ndx, ndy in fit["near"]:
                o = np.stack([shifted(og[..., i], ndx, ndy) for i in range(3)],
                             axis=2)
                a_wins += mean_abs(tiles[0], o, scored) < \
                    mean_abs(tiles[1], o, scored)
            n = len(fit["near"])
            out["verdict"] = "A" if a_wins == n else "B" if a_wins == 0 \
                else "undecided"
            out["near_a_closer"] = [int(a_wins), n]
    if fit:
        del fit["near"]
    return out


def fmt(res):
    """One-paragraph text form of a score() result."""
    f = res["fit"]
    lines = []
    if f:
        lines.append(
            "fit dx=%+.3f dy=%+.3f px (PSX %+.2f, %+.2f) corr=%.3f "
            "near dx %+.2f..%+.2f dy %+.2f..%+.2f%s" % (
                f["dx"], f["dy"], f["psx_dx"], f["psx_dy"], f["corr"],
                f["near_dx"][0], f["near_dx"][1], f["near_dy"][0],
                f["near_dy"][1],
                " (tile cols %d..%d)" % (res["col_offset"],
                                         res["col_offset"] + res["og_w"])
                if res["col_offset"] else ""))
    else:
        lines.append("fit: none (fewer than 256 unchanged pixels)")
    for k in "AB":
        if k in res:
            s = "%s |-og| raw=%.1f" % (k, res[k]["raw"])
            if "aligned" in res[k]:
                s += " aligned=%.1f" % res[k]["aligned"]
            lines.append(s)
    for key in ("raw", "aligned"):
        c = res.get("closer_" + key)
        if c:
            lines.append("closer (%s): A %d, B %d of %d" % (
                key, c["A"], c["B"], res["scored"]))
    if "verdict" in res:
        lines.append("aligned verdict: %s (A closer in %d of %d near-best "
                     "shifts)" % (res["verdict"], res["near_a_closer"][0],
                                  res["near_a_closer"][1]))
    return "\n".join(lines)


def main(argv):
    """Parse arguments, score, print; returns the exit status."""
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+", help="A.png [B.png] OG.png")
    ap.add_argument("--box", help="x0,x1,y0,y1 in tile pixels")
    ap.add_argument("--max-shift", type=float, default=3.0,
                    help="largest |shift| searched, output pixels (3)")
    ap.add_argument("--pad", type=int, default=64,
                    help="fit window margin around an A/B change, px (64)")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    if len(a.images) not in (2, 3):
        ap.print_usage(sys.stderr)
        return 64
    box = tuple(map(int, a.box.split(","))) if a.box else None
    imgs = [load_rgb(p) for p in a.images]
    try:
        res = score(imgs[:-1], imgs[-1], box, a.max_shift, a.pad)
    except ValueError as e:
        print("og_align: %s" % e, file=sys.stderr)
        return 64
    if res is None:
        print("og_align: nothing scored (A and B identical in the box)")
        return 1
    print(json.dumps(res, indent=1) if a.json else fmt(res))
    return 0 if res["fit"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
