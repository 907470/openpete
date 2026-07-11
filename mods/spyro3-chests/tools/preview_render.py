#!/usr/bin/env python3
"""Filled software preview of the packed S1 models vs the S3 source.

Left: what S1 will draw — pack faces through the dual-NCLIP keep rule,
texels sampled from the pack's 15bpp strip, modulated by vertex colour
/128 (flat per-face: colour of vertex 0). Right: the S3 reference —
same fill from chest_models.json + tile texels directly.

Orthographic view along +Z (front), painter-sorted by depth.
"""
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ASSETS = HERE.parent / "assets" / "s3"
sys.path.insert(0, str(HERE))
from verify_pack import Guest, walk_vstream, walk_faces, CLASSES  # noqa: E402
from convert_chests import map_vec, tile_key_of  # noqa: E402

import numpy as np


def rasterize(img, zbuf, tri, uvs, rgb, tex):
    """Barycentric fill; tri = [(x,y,depth)]*3 in pixel coords."""
    xs = [p[0] for p in tri]; ys = [p[1] for p in tri]
    x0, x1 = int(max(0, min(xs))), int(min(img.shape[1] - 1, max(xs)))
    y0, y1 = int(max(0, min(ys))), int(min(img.shape[0] - 1, max(ys)))
    if x0 > x1 or y0 > y1:
        return
    ax, ay = tri[0][0], tri[0][1]
    bx, by = tri[1][0], tri[1][1]
    cx, cy = tri[2][0], tri[2][1]
    d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if abs(d) < 1e-9:
        return
    for py in range(y0, y1 + 1):
        for px in range(x0, x1 + 1):
            l0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / d
            l1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / d
            l2 = 1 - l0 - l1
            if l0 < -0.001 or l1 < -0.001 or l2 < -0.001:
                continue
            z = l0 * tri[0][2] + l1 * tri[1][2] + l2 * tri[2][2]
            if z <= zbuf[py, px]:
                continue
            if tex is not None:
                u = l0 * uvs[0][0] + l1 * uvs[1][0] + l2 * uvs[2][0]
                v = l0 * uvs[0][1] + l1 * uvs[1][1] + l2 * uvs[2][1]
                t = tex(int(round(u)), int(round(v)))
                if t is None:
                    continue
                col = [min(255, t[k] * rgb[k] // 128) for k in range(3)]
            else:
                col = rgb
            zbuf[py, px] = z
            img[py, px] = col
