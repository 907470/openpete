#!/usr/bin/env python3
"""thumbnail.py - the video thumbnail: a master frame with the title and two logos.

    thumbnail.py (--frame FRAME.png | --clip CLIP --index N)
                 (--title TITLE.png | --title-black K.png --title-white W.png)
                 --mark MARK.png --logo LOGO.png --out-dir DIR --tag TAG [--layout A|B]

Layers over the 3840x2160 frame: a soft dark band behind the title, the
title matte (RGBA, full-frame), the OpenPete mark (NEAREST to 560 px) tilted
and the game logo (LANCZOS to 900 px wide), each with a blurred dark drop
shadow. Layout A: mark top-left tilted 10 degrees counter-clockwise, logo
top-right. Layout B: logo top-left, mark bottom-right tilted 10 degrees
clockwise, title lowered 140 px. Writes thumb-TAG-L-3840x2160.png and the
1280x720 PNG and JPEG (quality 92) per layout. --clip extracts encoded
frame N of a scene clip (yuv444p) as the frame. --title-black and
--title-white are the same title render over a black and a white
background; the matte is alpha = 1 - mean(W - K) / 255 and colour K / alpha
(the render composited over black, un-premultiplied).
"""
import argparse
import os
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageFilter

W, H = 3840, 2160


def shadow(layer, off, blur, opacity):
    """A blurred dark copy of layer's alpha, offset by off."""
    a = layer.split()[-1].point(lambda v: int(v * opacity))
    sh = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    sh.putalpha(a)
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    out = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    out.alpha_composite(sh, (off, off))
    return out


def place(canvas, layer, xy, sh=(14, 18, 0.65)):
    """Composite layer at xy over its drop shadow (offset, blur, opacity)."""
    pad = sh[1] * 3 + sh[0]
    big = Image.new("RGBA", (layer.width + 2 * pad, layer.height + 2 * pad), (0, 0, 0, 0))
    big.alpha_composite(layer, (pad, pad))
    canvas.alpha_composite(shadow(big, sh[0], sh[1], sh[2]), (xy[0] - pad, xy[1] - pad))
    canvas.alpha_composite(layer, xy)


def rot(layer, deg):
    """layer rotated deg counter-clockwise, canvas expanded."""
    return layer.rotate(deg, resample=Image.BICUBIC, expand=True)


def build(frame, title, mark, logo, layout):
    """One layout's 3840x2160 composite."""
    c = frame.copy()
    band = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bb = title.getbbox()
    vign = Image.new("RGBA", (bb[2] - bb[0] + 400, bb[3] - bb[1] + 300), (0, 0, 0, 90))
    band.alpha_composite(vign, (bb[0] - 200, bb[1] - 150))
    c.alpha_composite(band.filter(ImageFilter.GaussianBlur(120)))
    t2 = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    t2.alpha_composite(title, (0, 140 if layout == "B" else 0))
    place(c, t2, (0, 0), (10, 14, 0.75))
    m = mark.resize((560, 560), Image.NEAREST)
    s = logo.resize((900, round(900 * logo.height / logo.width)), Image.LANCZOS)
    if layout == "A":
        place(c, rot(m, 10), (90, 70))
        place(c, s, (W - s.width - 70, 60))
    else:
        place(c, s, (80, 60))
        mr = rot(m, -10)
        place(c, mr, (W - mr.width - 90, H - mr.height - 70))
    return c.convert("RGB")


def matte(black, white):
    """The RGBA title from its renders over black and over white."""
    k = np.asarray(Image.open(black).convert("RGB"), dtype=np.float64)
    w = np.asarray(Image.open(white).convert("RGB"), dtype=np.float64)
    a = np.clip(1 - (w - k).mean(axis=2) / 255, 0, 1)
    rgb = np.where((a > 0)[..., None], k / np.maximum(a, 1e-9)[..., None], 0)
    out = np.dstack([np.clip(np.round(rgb), 0, 255), np.round(a * 255)]).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def clip_frame(clip, index, tmp):
    """Encoded frame `index` of clip as an RGB PNG under tmp."""
    out = os.path.join(tmp, "frame.png")
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", clip, "-vf",
                    f"select=eq(n\\,{index})", "-frames:v", "1", "-pix_fmt", "rgb24", out], check=True)
    return out


def main():
    """Compose the requested layouts."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--frame")
    src.add_argument("--clip")
    ap.add_argument("--index", type=int)
    ttl = ap.add_mutually_exclusive_group(required=True)
    ttl.add_argument("--title")
    ttl.add_argument("--title-black")
    ap.add_argument("--title-white")
    ap.add_argument("--mark", required=True)
    ap.add_argument("--logo", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--layout", choices=["A", "B"], action="append")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        path = a.frame or clip_frame(a.clip, a.index, tmp)
        frame = Image.open(path).convert("RGBA")
    if frame.size != (W, H):
        raise SystemExit(f"frame is {frame.size[0]}x{frame.size[1]}, not {W}x{H}")
    if a.title_black and not a.title_white:
        raise SystemExit("--title-black needs --title-white")
    title = Image.open(a.title) if a.title else matte(a.title_black, a.title_white)
    mark, logo = Image.open(a.mark), Image.open(a.logo)
    os.makedirs(a.out_dir, exist_ok=True)
    for lay in a.layout or ["A", "B"]:
        im = build(frame, title, mark, logo, lay)
        base = os.path.join(a.out_dir, f"thumb-{a.tag}-{lay}")
        im.save(base + "-3840x2160.png")
        small = im.resize((1280, 720), Image.LANCZOS)
        small.save(base + "-1280x720.png")
        small.save(base + "-1280x720.jpg", quality=92)
        print(base + "-3840x2160.png")


if __name__ == "__main__":
    main()
