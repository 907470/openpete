#!/usr/bin/env python3
"""render_res_model.py - CPU model of the --resolution resample.

Recomputes render_res.frag from the base image B (`-base.png`, written
under OPENPETE_RENDER_RES_DUMP_BASE=1) and the plan the run's `[render-res]`
engagement line states (B, V, crop, per-axis filter), and compares the
result with the output image (`-vk.png`, the capture extent C).

Per axis, as the shader: copy and nearest fetch one whole texel; area at an
integer ratio averages the r texels of each output pixel in linear light;
sharp-bilinear and the fractional area weight texelFetch taps in float,
clamped to the centres of the outermost texels the source rect touches.
Copy and nearest on both axes are written unconverted; every other
combination converts to linear light, sums, and converts back.

Pass: zero differing pixels when both axes are copy or nearest, else at
most 1 LSB per channel, with the 1-LSB count reported. Preconditions of
the run: --dither 0, no mod SCENE pass, no chrome, so the output is the
resample's alone.

Usage:
  render_res_model.py --base B.png --out C.png --line '<[render-res] line>'
                      [--expect-fail]
Exit 0 on the expected verdict (pass, or fail under --expect-fail),
1 otherwise, 2 on bad input.
"""
import argparse
import re
import sys

import numpy as np
from PIL import Image


def srgb_to_linear(c):
    return np.where(c < 0.04045, c / 12.92, np.power((c + 0.055) / 1.055, 2.4))


def linear_to_srgb(c):
    c = np.maximum(c, 0.0)
    return np.where(c < 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def axis_matrix(f, r, s0, s1, d, size):
    """Weights (d x size) of one axis, as render_res.frag's axis_taps."""
    m = np.zeros((d, size), dtype=np.float64)
    length = s1 - s0
    lo, hi = int(np.floor(s0)), int(np.ceil(s1)) - 1
    li = int(length + 0.5)
    for p in range(d):
        if f <= 1:
            m[p, lo + (p * li) // d] = 1.0
        elif f == 2:
            for i in range(r):
                m[p, lo + p * r + i] += 1.0 / r
        elif f == 3:
            scale = length / d
            k = max(np.floor(d / length), 1.0)
            t = s0 + (p + 0.5) * scale
            fl = np.floor(t)
            cd = (t - fl) - 0.5
            rr = 0.5 - 0.5 / k
            mm = fl + (cd - np.clip(cd, -rr, rr)) * k + 0.5
            mm = min(max(mm, lo + 0.5), hi + 0.5)
            u = mm - 0.5
            i0 = int(np.floor(u))
            a = u - i0
            m[p, i0] += 1.0 - a
            m[p, min(i0 + 1, hi)] += a
        else:
            scale = length / d
            a0, a1 = s0 + p * scale, s0 + (p + 1) * scale
            i = int(np.floor(a0))
            n = 0
            while n < 8 and i < a1:
                ov = min(a1, i + 1) - max(a0, i)
                m[p, min(max(i, lo), hi)] += max(ov, 0.0) / scale
                i += 1
                n += 1
    return m


FILTERS = {"copy": 0, "nearest": 1, "area": 2, "sharp": 3, "areaf": 4}


def parse_line(line):
    g = {}
    for key in ("B", "V", "C", "crop", "filter", "value"):
        mt = re.search(r"\b" + re.escape(key) + r"=(\S+)", line)
        if not mt:
            raise ValueError(f"engagement line lacks {key}=")
        g[key] = mt.group(1)
    bw, bh = (int(x) for x in g["B"].split("x"))
    vx, vy, vwh = g["V"].split(",")
    vw, vh = (int(x) for x in vwh.split("x"))
    fx, fy = (FILTERS[s.split(":")[1]] for s in g["filter"].split(","))
    return dict(bw=bw, bh=bh, vx=int(vx), vy=int(vy), vw=vw, vh=vh,
                crop=float(g["crop"]), fx=fx, fy=fy, value=g["value"])


def ratio(f, length, d):
    li = int(length + 0.5)
    if f == 1:
        return d // li
    if f == 2:
        return li // d
    return 1


def model(base, pl):
    bh, bw = base.shape[:2]
    x0, x1 = pl["crop"], bw - pl["crop"]
    mx = axis_matrix(pl["fx"], ratio(pl["fx"], x1 - x0, pl["vw"]), x0, x1, pl["vw"], bw)
    my = axis_matrix(pl["fy"], ratio(pl["fy"], bh, pl["vh"]), 0.0, float(bh), pl["vh"], bh)
    rgb = base[:, :, :3].astype(np.float64) / 255.0
    if pl["fx"] <= 1 and pl["fy"] <= 1:
        ix = mx.argmax(axis=1)
        iy = my.argmax(axis=1)
        return base[:, :, :3][np.ix_(iy, ix)].astype(np.int32)
    lin = srgb_to_linear(rgb)
    tmp = np.tensordot(my, lin, axes=(1, 0))          # (vh, bw, 3)
    out = np.einsum("yac,xa->yxc", tmp, mx)
    return np.rint(np.clip(linear_to_srgb(out), 0.0, 1.0) * 255.0).astype(np.int32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--line", required=True)
    ap.add_argument("--expect-fail", action="store_true")
    a = ap.parse_args()
    try:
        pl = parse_line(a.line)
        base = np.asarray(Image.open(a.base).convert("RGBA"))
        out = np.asarray(Image.open(a.out).convert("RGBA")).astype(np.int32)
    except (OSError, ValueError, KeyError) as e:
        print(f"render_res_model: bad input: {e}")
        return 2
    if base.shape[1] != pl["bw"] or base.shape[0] != pl["bh"]:
        print(f"render_res_model: base {base.shape[1]}x{base.shape[0]} != line B {pl['bw']}x{pl['bh']}")
        return 2
    exact = pl["fx"] <= 1 and pl["fy"] <= 1
    want = model(base, pl)
    got = out[pl["vy"]:pl["vy"] + pl["vh"], pl["vx"]:pl["vx"] + pl["vw"], :3]
    d = np.abs(want - got).max(axis=2)
    n_diff = int((d > 0).sum())
    n_1 = int((d == 1).sum())
    mx = int(d.max()) if d.size else 0
    ok = n_diff == 0 if exact else mx <= 1
    verdict = "PASS" if ok else "FAIL"
    print(f"render_res_model: value={pl['value']} B={pl['bw']}x{pl['bh']} V={pl['vw']}x{pl['vh']} "
          f"filter={pl['fx']},{pl['fy']} exact={int(exact)} pixels={d.size} differing={n_diff} "
          f"lsb1={n_1} max={mx} -> {verdict}{' (expected FAIL)' if a.expect_fail else ''}")
    if d.size == 0:
        print("render_res_model: VACUOUS (empty picture area)")
        return 1
    return 0 if ok != a.expect_fail else 1


if __name__ == "__main__":
    sys.exit(main())
