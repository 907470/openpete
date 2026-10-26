#!/usr/bin/env python3
"""og_matched_leg.py -- adjudicate an A/B tile pair against -og at matched
vertex precision.

The -og reference (PsyCross's software render of the same tick) rasterises
the GTE's floored integer screen coordinates; the native renderer draws
unrounded float positions, so a per-pixel |tile - og| in the default mode
scores a sub-pixel displacement on every edge (og_align.py fits it out). This
leg removes the displacement at the source instead: each build renders the
pose twice, once in the default float mode and once with
`--vertex-precision integer --raster-quantize Q`, where the native vertices
are floored to the same PSX pixel lattice og uses. The matched tiles are
scored against og per pixel with no alignment.

The verdict is given only when the change being judged survives the mode:
the leg counts the A-vs-B changed pixels in float mode and in matched mode,
and refuses (verdict "lost") when no matched-mode changed pixel lies within
--near px of a float-mode changed pixel. It also reports the residual og
offset in matched mode (og_align.fit_shift over the unchanged pixels around
the change; about 0 when the modes match) and og_align's aligned verdict on
the float pair, so the two scorings can be compared.

Two further confounders are removed or masked. og maps textures affinely
(screen-linear), the native default perspective-correctly, so texels inside
a face move even where the face edges match: every leg (float and matched)
passes --texture-mapping affine unless --no-affine is given. The game draws
near-band terrain (the faces its walk defers to the near/HQ queues,
r_environment.s 0x80026CFC) subdivided before the affine draw and native does
not, so their texels cannot match under either mapping: each leg runs with
OPENPETE_TERRAIN_TRIDUMP, the leg's walked terrain triangles with the
canonical near-band flag (face_is_near_canonical, without the --lod extended
HQ pin), and the opaque triangles are rasterised with true depth into a
per-pixel owner map (nearest terrain face, PSX x * W/512, (y - (240 - H/2) /
2) * 2; on the CPU rasteriser the retained terrain cache is off, so the walk
holds every terrain face). A changed pixel whose terrain owner in either leg
is a near-band face (dilated by 1 px) leaves the colour score and is scored
by owner and edge only: owner = mean |blur(x) - blur(og)| with a 5x5 box
blur, which averages the texel layout out and keeps the owner's colour;
edge = mean |grad(x) - grad(og)| of the Sobel gradient magnitude of the
sigma-1 blurred luminance. The owner map knows terrain only: a moby drawn
over near-band terrain in both legs is masked with it.

Usage:
  tests/harness/og_matched_leg.py --game G --vbl N [--level L]
      [--pose "pos=(x, y, z) yaw=Y pitch=P"] [--bk2 PATH]
      --a BIN [--b BIN] [--a-env K=V ...] [--b-env K=V ...]
      [--quantize Q] [--lod L] [--out DIR] [--jobs J] [--near PX]
      [--box x0,x1,y0,y1] [--no-affine] [--no-nearband-mask] [--reuse]
      [--json]

  --game        spyro-1 | spyro-2 | spyro-3; picks the disc cue.
  --vbl         the VBL captured (--screenshot-on-frame).
  --level/--pose/--bk2  the row: --skip-to-level, --freecam-pose, --bk2
                (a path relative to the repository root, or absolute).
  --a, --b      the reference and candidate binaries (an openpete
                executable inside its build directory, which holds the
                recomp modules); --b defaults to --a, for an A/B that differs
                only by environment.
  --a-env, --b-env  K=V set for that leg only (repeatable), e.g.
                --b-env OPENPETE_MOBY_SNAP=0.
  --quantize    the --raster-quantize multiplier of the matched leg
                (default 2). og rasterises its integer vertices at the
                output resolution (no PSX-cell blocks), so the matched grid
                is the output grid: 2 (1024x480 cells) is finer than a
                640-wide tile and scores as 0 (no resample) there; 1
                (512x240 cells) is coarser than og.
  --lod         --lod for every leg (default extended).
  --out         capture directory (default a fresh temp dir); its
                <leg>__<mode>/ subdirectories are deleted before each run.
  --jobs        concurrent game processes (default 2).
  --near        px radius of the survival test (default 4).
  --box         x0,x1,y0,y1: score only inside this rectangle.
  --no-affine   render every leg with the native default perspective-correct
                texture mapping instead of --texture-mapping affine.
  --no-nearband-mask  score near-band terrain pixels by colour like the rest
                (no tridump is read; --reuse captures need no tri.txt).
  --reuse       score the PNGs already in --out; launch nothing.
  --json        print the result as JSON.

Every leg runs headless on the CPU Vulkan rasteriser (scripts/lib/
swrender.sh) at --aspect 4:3 with --no-config --no-card --mute 1 and
OPENPETE_VIBRATION=0. A leg whose binary mtime changes during its run is
reported and the run exits 2: rerun once the rebuild is done.

Output: float and matched changed-pixel counts and the survival count; the
matched-mode residual og offset; matched mean |A-og| and |B-og| over the
matched changed pixels outside the near-band mask with per-pixel closer
counts; for the masked pixels the count and the owner and edge means per
side; a verdict per class (colour: A or B when that side's mean is lower
and it is closer on more pixels; near band: the side lower on both owner
and edge; else "split"); the matched verdict, the verdict of the class with
more scored pixels, "split" when the other class holds at least a third as
many and disagrees, "lost" when the change does not survive; the float
og_align aligned verdict; and whether the two verdicts agree.
Exit 0 = verdict given, 1 = no verdict (lost, split or nothing changed),
2 = capture failed, 64 = usage.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import og_align  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
CUES = {"spyro-1": "games/spyro-1/spyro-1/build/SCUS-94228.cue",
        "spyro-2": "games/spyro-2/spyro-2/build/SCUS-94425.cue",
        "spyro-3": "games/spyro-3/spyro-3/build/SCUS-94467.cue"}
LVP = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"


def leg_env(extra):
    """Environment of one leg: the CPU rasteriser pin of swrender.sh, no
    rumble, plus the leg's own K=V pairs."""
    env = dict(os.environ)
    if os.path.exists(LVP):
        env["VK_ICD_FILENAMES"] = LVP
    env["LIBGL_ALWAYS_SOFTWARE"] = "1"
    env.setdefault("GALLIUM_DRIVER", "llvmpipe")
    env.pop("MESA_LOADER_DRIVER_OVERRIDE", None)
    env["OPENPETE_VIBRATION"] = "0"
    for kv in extra:
        k, _, v = kv.partition("=")
        env[k] = v
    return env


def capture(a, binary, envs, mode, d):
    """Run one leg into directory d; returns (vk path, og path, error)."""
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    cmd = [binary, "--game", a.game, "--disc", CUES[a.game], "--headless",
           "--no-config", "--disc-cache", "0", "--renderer", "vk",
           "--aspect", "4:3", "--lod", a.lod, "--no-card", "--mute", "1"]
    if a.level:
        cmd += ["--skip-to-level", a.level]
    if a.pose:
        cmd += ["--freecam-pose", a.pose]
    if a.bk2:
        cmd += ["--bk2", a.bk2]
    if mode == "matched":
        cmd += ["--vertex-precision", "integer",
                "--raster-quantize", str(a.quantize)]
    if not a.no_affine:
        cmd += ["--texture-mapping", "affine"]
    envs = list(envs)
    tri = os.path.join(d, "tri.txt")
    if not a.no_nearband_mask:
        envs.append("OPENPETE_TERRAIN_TRIDUMP=%d,%s" % (a.vbl, tri))
    cmd += ["--screenshot-on-frame", str(a.vbl), "--screenshot-output",
            os.path.join(d, "x__{n}.png"), "--frame-limit", str(a.vbl + 10)]
    m0 = os.stat(binary).st_mtime_ns
    with open(os.path.join(d, "run.log"), "w") as log:
        rc = subprocess.call(["timeout", "-k", "10", "900"] + cmd, cwd=ROOT,
                             env=leg_env(envs), stdout=log,
                             stderr=subprocess.STDOUT)
    vk = os.path.join(d, "x__%d-vk.png" % a.vbl)
    og = os.path.join(d, "x__%d-og.png" % a.vbl)
    if os.stat(binary).st_mtime_ns != m0:
        return vk, og, "binary rebuilt during the run"
    missing = [p for p in (vk, og) + (() if a.no_nearband_mask else (tri,))
               if not os.path.exists(p)]
    if missing:
        return vk, og, "rc=%d, missing %s" % (rc, " ".join(missing))
    return vk, og, None


def nearband_mask(tri, h, w):
    """Per-pixel near-band terrain owner map of one leg's tridump.

    Rasterises the opaque terrain triangles of tri (an
    OPENPETE_TERRAIN_TRIDUMP file) at pixel centres with a perspective-correct
    nearest-depth test and returns the pixels whose nearest face carries the
    near-band flag. PSX x maps to x * w / 512 (the 4:3 viewport), PSX y to
    (y - (240 - h / 2) / 2) * 2 (the visible lines centred in 240)."""
    zt = np.full((h, w), np.inf)
    nb = np.zeros((h, w), bool)
    yoff = (240.0 - h / 2.0) / 2.0
    with open(tri) as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.split(",")
            if int(p[4]) != 0:
                continue
            v = [(float(p[7 + 3 * k]) * w / 512.0,
                  (float(p[8 + 3 * k]) - yoff) * 2.0, float(p[9 + 3 * k]))
                 for k in range(3)]
            if min(q[2] for q in v) <= 0:
                continue
            (ax, ay, az), (bx, by, bz), (cx, cy, cz) = v
            x0 = max(0, int(np.floor(min(ax, bx, cx))))
            x1 = min(w - 1, int(np.ceil(max(ax, bx, cx))))
            y0 = max(0, int(np.floor(min(ay, by, cy))))
            y1 = min(h - 1, int(np.ceil(max(ay, by, cy))))
            det = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
            if x0 > x1 or y0 > y1 or abs(det) < 1e-9:
                continue
            gy, gx = np.mgrid[y0:y1 + 1, x0:x1 + 1]
            qx, qy = gx + 0.5, gy + 0.5
            b1 = ((qx - ax) * (cy - ay) - (cx - ax) * (qy - ay)) / det
            b2 = ((bx - ax) * (qy - ay) - (qx - ax) * (by - ay)) / det
            b0 = 1.0 - b1 - b2
            ins = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
            z = 1.0 / (b0 / az + b1 / bz + b2 / cz)
            sub = zt[y0:y1 + 1, x0:x1 + 1]
            win = ins & (z < sub)
            sub[win] = z[win]
            nb[y0:y1 + 1, x0:x1 + 1][win] = int(p[17]) != 0
    return nb


def owner_edge(x, og, sel):
    """Owner and edge distances of image x from og over the pixels sel.

    Owner is the mean summed-channel |box5(x) - box5(og)|, edge the mean
    |sobel magnitude| difference of the sigma-1 blurred luminance."""
    bx = ndimage.uniform_filter(x, size=(5, 5, 1))
    bo = ndimage.uniform_filter(og, size=(5, 5, 1))

    def grad(im):
        lum = ndimage.gaussian_filter(im.mean(axis=2), 1.0)
        return np.hypot(ndimage.sobel(lum, 0), ndimage.sobel(lum, 1))
    return (float(np.abs(bx - bo).sum(axis=2)[sel].mean()),
            float(np.abs(grad(x) - grad(og))[sel].mean()))


def side(a, b):
    """"A" when a < b, "B" when b < a, else "tie"."""
    return "A" if a < b else "B" if b < a else "tie"


def score(paths, box=None, near_px=4, pad=64, tris=None):
    """Score the four captures; paths maps (leg, mode) to (vk, og), tris
    (optional) maps (leg, "matched") to that leg's tridump path."""
    img = {k: (og_align.load_rgb(v), og_align.load_rgb(o))
           for k, (v, o) in paths.items()}
    h, w = img[("A", "float")][0].shape[:2]
    region = np.ones((h, w), bool)
    if box:
        region[:] = False
        region[box[2]:box[3], box[0]:box[1]] = True
    res = {"og_same": {m: bool((img[("A", m)][1] == img[("B", m)][1]).all())
                       for m in ("float", "matched")}}
    ch = {}
    for m in ("float", "matched"):
        ch[m] = (img[("A", m)][0] != img[("B", m)][0]).any(axis=2) & region
        res[m + "_changed"] = int(ch[m].sum())
    grown = ndimage.binary_dilation(ch["float"], iterations=near_px) \
        if ch["float"].any() else ch["float"]
    res["survived"] = int((ch["matched"] & grown).sum())
    # Float pair, aligned (og_align's scoring).
    fa = og_align.score([img[("A", "float")][0], img[("B", "float")][0]],
                        img[("A", "float")][1], box)
    res["float_aligned"] = None if fa is None else {
        "verdict": fa.get("verdict", "undecided"),
        "A": fa["A"], "B": fa["B"],
        "fit": {k: fa["fit"][k] for k in ("dx", "dy", "psx_dx", "psx_dy")}
        if fa["fit"] else None}
    # Matched pair, raw.
    a_m, og_m = img[("A", "matched")]
    b_m = img[("B", "matched")][0]
    sc = ch["matched"]
    if sc.any():
        ys, xs = np.nonzero(sc)
        win = np.zeros_like(sc)
        win[max(0, ys.min() - pad):ys.max() + pad + 1,
            max(0, xs.min() - pad):xs.max() + pad + 1] = True
        fit_mask = win & region & ~ndimage.binary_dilation(
            (a_m != b_m).any(axis=2), iterations=3)
    else:
        fit_mask = region
    fit = og_align.fit_shift(a_m, og_m, fit_mask, 3.0)
    if fit:
        res["matched_fit"] = {"dx": fit["dx"], "dy": fit["dy"],
                              "psx_dx": fit["dx"] / (w / 512.0),
                              "psx_dy": fit["dy"] / (
                                  h / 216.0 if h % 216 == 0 else h / 224.0),
                              "corr": fit["corr"]}
    else:
        res["matched_fit"] = None
    nb = np.zeros_like(sc)
    if tris:
        for t in tris.values():
            nb |= nearband_mask(t, h, w)
        nb = ndimage.binary_dilation(nb, iterations=1)
    col = sc & ~nb
    res["nearband"] = {"masked": int((sc & nb).sum()),
                       "frame_px": int(nb.sum())} if tris else None
    if tris and (sc & nb).any():
        oa, ea = owner_edge(a_m, og_m, sc & nb)
        ob, eb = owner_edge(b_m, og_m, sc & nb)
        res["nearband"].update({"owner_A": oa, "owner_B": ob,
                                "edge_A": ea, "edge_B": eb})
    res["matched"] = None
    votes = []                       # (scored px, verdict) per pixel class
    if (sc & nb).any():
        n = res["nearband"]
        ov = side(n["owner_A"], n["owner_B"])
        ev = side(n["edge_A"], n["edge_B"])
        n["verdict"] = ov if ov == ev else "split"
        votes.append((n["masked"], n["verdict"]))
    if col.any():
        da = np.abs(a_m - og_m).sum(axis=2)[col]
        db = np.abs(b_m - og_m).sum(axis=2)[col]
        mean_v = side(da.mean(), db.mean())
        cnt_v = side(-(da < db).sum(), -(db < da).sum())
        res["matched"] = {"A": float(da.mean()), "B": float(db.mean()),
                          "closer_A": int((da < db).sum()),
                          "closer_B": int((db < da).sum()),
                          "scored": int(col.sum()),
                          "verdict": mean_v if mean_v == cnt_v else "split"}
        votes.append((int(col.sum()), res["matched"]["verdict"]))
    if res["survived"] == 0 or not sc.any():
        res["verdict"] = "lost" if res["float_changed"] else "nothing"
    else:
        votes.sort(reverse=True)
        res["verdict"] = votes[0][1]
        if len(votes) == 2 and votes[1][1] != votes[0][1] and \
                3 * votes[1][0] >= votes[0][0]:
            res["verdict"] = "split"
    fv = res["float_aligned"]["verdict"] if res["float_aligned"] else None
    res["agree"] = (fv == res["verdict"]) if fv in ("A", "B") and \
        res["verdict"] in ("A", "B") else None
    return res


def fmt(r):
    """Text form of a score() result."""
    out = ["changed px: float %d, matched %d, matched within reach of "
           "float %d" % (r["float_changed"], r["matched_changed"],
                         r["survived"])]
    if not all(r["og_same"].values()):
        out.append("WARNING: og differs between A and B legs: %s" %
                   r["og_same"])
    f = r["matched_fit"]
    if f:
        out.append("matched residual og offset dx=%+.3f dy=%+.3f px (PSX "
                   "%+.2f, %+.2f) corr=%.3f" % (f["dx"], f["dy"], f["psx_dx"],
                                                f["psx_dy"], f["corr"]))
    m = r["matched"]
    if m:
        out.append("matched colour |A-og|=%.1f |B-og|=%.1f closer A %d, B %d "
                   "(%d px scored): %s" % (m["A"], m["B"], m["closer_A"],
                                           m["closer_B"], m["scored"],
                                           m["verdict"]))
    n = r.get("nearband")
    if n:
        out.append("near-band masked %d changed px (%d px of the tile)%s" % (
            n["masked"], n["frame_px"], "" if "owner_A" not in n else
            ": owner A %.1f B %.1f, edge A %.1f B %.1f: %s" % (
                n["owner_A"], n["owner_B"], n["edge_A"], n["edge_B"],
                n["verdict"])))
    fa = r["float_aligned"]
    if fa:
        out.append("float aligned |A-og|=%.1f |B-og|=%.1f verdict %s" % (
            fa["A"].get("aligned", float("nan")),
            fa["B"].get("aligned", float("nan")), fa["verdict"]))
    out.append("matched verdict: %s%s" % (
        r["verdict"], "" if r["agree"] is None else
        " (agrees with float aligned)" if r["agree"] else
        " (DISAGREES with float aligned)"))
    return "\n".join(out)


def main(argv):
    """Parse arguments, capture the four legs, score, print."""
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, choices=sorted(CUES))
    ap.add_argument("--vbl", type=int, required=True)
    ap.add_argument("--level")
    ap.add_argument("--pose")
    ap.add_argument("--bk2")
    ap.add_argument("--a", required=True)
    ap.add_argument("--b")
    ap.add_argument("--a-env", action="append", default=[])
    ap.add_argument("--b-env", action="append", default=[])
    ap.add_argument("--quantize", type=int, default=2)
    ap.add_argument("--lod", default="extended")
    ap.add_argument("--out")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--near", type=int, default=4)
    ap.add_argument("--box")
    ap.add_argument("--no-affine", action="store_true")
    ap.add_argument("--no-nearband-mask", action="store_true")
    ap.add_argument("--reuse", action="store_true")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    a.b = a.b or a.a
    a.a, a.b = os.path.abspath(a.a), os.path.abspath(a.b)
    if a.bk2:
        a.bk2 = os.path.abspath(os.path.join(ROOT, a.bk2))
    if a.reuse and not a.out:
        print("og_matched_leg: --reuse needs --out", file=sys.stderr)
        return 64
    out = a.out or tempfile.mkdtemp(prefix="og_matched_leg.")
    box = tuple(map(int, a.box.split(","))) if a.box else None
    legs = {("A", m): (a.a, a.a_env) for m in ("float", "matched")}
    legs.update({("B", m): (a.b, a.b_env) for m in ("float", "matched")})
    paths, errs = {}, []
    if a.reuse:
        for (leg, m) in legs:
            d = os.path.join(out, "%s__%s" % (leg, m))
            paths[(leg, m)] = (os.path.join(d, "x__%d-vk.png" % a.vbl),
                               os.path.join(d, "x__%d-og.png" % a.vbl))
    else:
        with ThreadPoolExecutor(max(1, a.jobs)) as ex:
            futs = {k: ex.submit(capture, a, b, e, k[1],
                                 os.path.join(out, "%s__%s" % k))
                    for k, (b, e) in legs.items()}
            for k, fu in futs.items():
                vk, og, err = fu.result()
                paths[k] = (vk, og)
                if err:
                    errs.append("%s %s: %s" % (k[0], k[1], err))
    if errs:
        print("og_matched_leg: capture failed: " + "; ".join(errs),
              file=sys.stderr)
        return 2
    tris = None if a.no_nearband_mask else {
        (leg, "matched"): os.path.join(out, "%s__matched" % leg, "tri.txt")
        for leg in ("A", "B")}
    r = score(paths, box, a.near, tris=tris)
    r["out"] = out
    print(json.dumps(r, indent=1) if a.json else fmt(r) + "\ncaptures: " +
          out)
    return 0 if r["verdict"] in ("A", "B") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
