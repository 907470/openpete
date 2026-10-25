#!/usr/bin/env python3
"""thumbnail.py - the video thumbnail: a master frame with the title and two logos.

    thumbnail.py title --out-dir DIR [--game G] [--disc CUE] [--jobs N] [--keep-worktree]
    thumbnail.py compose (--frame FRAME.png | --clip CLIP --index N)
                 (--title TITLE.png | --title-black K.png --title-white W.png)
                 --mark MARK.png --logo LOGO.png --out-dir DIR --tag TAG [--layout A|B]

title renders the title over black and over white (DIR/title-black.png,
DIR/title-white.png, DIR/title.log). The knobs that draw it are not on main:
patches/title-matte.patch applies to the revision its `Pin:` header line
names. The step creates the throwaway worktree `credits-title` at that
revision with scripts/worktree.sh, links the untracked build inputs
`scripts/worktree.sh inputs` names (except the oracle goldens and card
fixtures) to the main checkout's, applies the patch, configures linux-release,
builds the game's target, captures the two stills under Xvfb with the
game table's [thumbnail] knobs plus OPENPETE_VIDEO_TITLE_BG=000000 and
FFFFFF, and removes the worktree (kept with --keep-worktree). The renders
are the game's font from the disc and are never committed.

compose layers over the 3840x2160 frame: a soft dark band behind the title, the
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
import re
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageFilter

import config

sys.path.insert(0, os.path.join(config.ROOT, "scripts", "lib"))
from hostenv import child_env  # noqa: E402

W, H = 3840, 2160
PATCH = os.path.join(config.HERE, "patches", "title-matte.patch")
WT_NAME = "credits-title"


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


def patch_pin():
    """The revision the title patch applies to, from its `Pin:` header line."""
    with open(PATCH) as f:
        m = re.search(r"^Pin: applies to openpete ([0-9a-f]{7,40})\b", f.read(), re.M)
    if not m:
        raise SystemExit(f"{PATCH}: no 'Pin: applies to openpete <rev>' line")
    return m.group(1)


def main_root():
    """The main checkout: the parent of the git common directory (also from a worktree)."""
    common = subprocess.run(["git", "-C", config.HERE, "rev-parse", "--path-format=absolute",
                             "--git-common-dir"], capture_output=True, text=True, check=True).stdout.strip()
    return os.path.dirname(common.rstrip("/"))


def step(log, what, cmd, cwd=None, env=None):
    """Run cmd with its output appended to log; exit naming `what` on failure."""
    with open(log, "a") as f:
        f.write(f"== {what}: {' '.join(cmd)}\n")
        f.flush()
        rc = subprocess.call(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if rc:
        raise SystemExit(f"title: {what} exited {rc} (log {log})")


def link_inputs(root, wt, log):
    """Symlink the untracked build inputs into wt from the main checkout root.

    The oracle goldens and card fixtures under tests/golden are skipped: the
    capture reads neither. A decomp build directory is linked only inside a
    checked-out decomp submodule."""
    p = subprocess.run(["bash", os.path.join(root, "scripts", "worktree.sh"), "inputs"],
                       capture_output=True, text=True, env=dict(os.environ, OPENPETE_ROOT=root))
    if p.returncode:
        raise SystemExit(f"title: worktree.sh inputs exited {p.returncode}")
    with open(log, "a") as f:
        for rel in p.stdout.split():
            src, dst = os.path.join(root, rel), os.path.join(wt, rel)
            if rel.startswith("tests/golden/") or not os.path.exists(src) or os.path.lexists(dst):
                continue
            if rel.endswith("/build") and rel.startswith("games/") and \
                    not os.path.exists(os.path.join(os.path.dirname(dst), ".git")):
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            os.symlink(src, dst)
            f.write(f"linked {rel}\n")


def capture(exe, wt, cfg, disc, bg, out, log):
    """One title still over background bg (RRGGBB) at the table's VBL, written to out."""
    t = cfg["thumbnail"]
    w, h = t["window"].split("x")
    shots = os.path.join(os.path.dirname(out), "title-shots")
    shutil.rmtree(shots, ignore_errors=True)
    os.makedirs(shots)
    knobs = {**t["env"], "OPENPETE_VIDEO_TITLE_BG": bg, "OPENPETE_VIBRATION": "0"}
    cmd = ["xvfb-run", "-a", "-s", f"-screen 0 {w}x{h}x24", exe, "--game", cfg["game"], "--no-config",
           "--disc", disc, "--renderer", "vk", "--window", t["window"], "--no-card", "--mute", "1",
           "--input-lock", "--vibration", "0", "--background", "1", "--ssaa", str(t["ssaa"]),
           "--perf-stats", "off", "--screenshot-on-frame", str(t["vbl"]), "--screenshot-native-only",
           "--screenshot-output", os.path.join(shots, "x__{n}.png"), "--frame-limit", str(t["vbl"] + 6)]
    with open(log, "a") as f:
        f.write("knobs " + " ".join(f"{k}={v}" for k, v in sorted(knobs.items())) + "\n")
    step(log, f"capture bg {bg}", cmd, cwd=wt, env=child_env(knobs))
    got = os.path.join(shots, f"x__{t['vbl']}-vk.png")
    if not os.path.exists(got):
        raise SystemExit(f"title: the capture wrote no {got} (log {log})")
    os.replace(got, out)
    shutil.rmtree(shots)


def cmd_title(a):
    """Build the pinned title patch in a throwaway worktree and render the two stills."""
    cfg = config.load(a.game)
    if "thumbnail" not in cfg:
        raise SystemExit(f"{a.game}: the table has no [thumbnail]")
    disc = os.path.abspath(a.disc or config.default_disc(a.game))
    out_dir = os.path.abspath(a.out_dir)
    os.makedirs(out_dir, exist_ok=True)
    black, white, log = (os.path.join(out_dir, n) for n in ("title-black.png", "title-white.png", "title.log"))
    for p in (black, white, log):
        if os.path.exists(p):
            os.unlink(p)
    pin = patch_pin()
    root = main_root()
    wtsh = os.path.join(root, "scripts", "worktree.sh")
    wt = os.path.join(root, "worktrees", WT_NAME)
    if os.path.lexists(wt):
        raise SystemExit(f"title: {wt} exists (a previous run's --keep-worktree?): "
                         f"scripts/worktree.sh remove {WT_NAME} --force")
    env = dict(os.environ, OPENPETE_ROOT=root)
    try:
        step(log, "worktree create", ["bash", wtsh, "create", WT_NAME, "--ref", pin], env=env)
        link_inputs(root, wt, log)
        step(log, "apply", ["git", "-C", wt, "apply", PATCH])
        step(log, "configure", ["cmake", "--preset", "linux-release"], cwd=wt)
        jobs = ["-j", str(a.jobs)] if a.jobs else []
        step(log, "build", ["cmake", "--build", "--preset", "linux-release", "--target",
                            "openpete-" + a.game.replace("-", "")] + jobs, cwd=wt)
        exe = os.path.join(wt, "build", "linux-release", "openpete")
        capture(exe, wt, cfg, disc, "000000", black, log)
        capture(exe, wt, cfg, disc, "FFFFFF", white, log)
    finally:
        if not a.keep_worktree and os.path.lexists(wt):
            step(log, "worktree remove", ["bash", wtsh, "remove", WT_NAME, "--force"], env=env)
    print(black)
    print(white)


def cmd_compose(a):
    """Compose the requested layouts."""
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


def main():
    """Dispatch the title and compose steps."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("title", help="render the title stills from the pinned patch")
    t.add_argument("--game", default="spyro-3")
    t.add_argument("--disc")
    t.add_argument("--out-dir", required=True)
    t.add_argument("--jobs", type=int, help="build parallelism (default: the generator's)")
    t.add_argument("--keep-worktree", action="store_true")
    c = sub.add_parser("compose", help="layer the title and logos over a frame")
    src = c.add_mutually_exclusive_group(required=True)
    src.add_argument("--frame")
    src.add_argument("--clip")
    c.add_argument("--index", type=int)
    ttl = c.add_mutually_exclusive_group(required=True)
    ttl.add_argument("--title")
    ttl.add_argument("--title-black")
    c.add_argument("--title-white")
    c.add_argument("--mark", required=True)
    c.add_argument("--logo", required=True)
    c.add_argument("--out-dir", required=True)
    c.add_argument("--tag", required=True)
    c.add_argument("--layout", choices=["A", "B"], action="append")
    a = ap.parse_args()
    (cmd_title if a.cmd == "title" else cmd_compose)(a)


if __name__ == "__main__":
    main()
