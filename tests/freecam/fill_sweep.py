#!/usr/bin/env python3
"""Terrain fill-tag sweep: derive the LP fill-face list by measurement.

Per pose (tests/freecam/fill-sweep-poses.manifest), five headless legs:
  M        native vk, --lod extended, all fill mechanisms off, with the
           OPENPETE_TERRAIN_FACEMAP per-LP-face footprint dump on stderr
  MO       native vk, --lod original, all fill mechanisms off (the handoff
           drops in-band LP whose sparse HP never covers the footprint -
           a MODE-dependent need the two-og mask cannot see)
  BLANK-vk native vk, --render-paths none        (pure background)
  OG       PsyCross,  terrain-only               (the game's own terrain)
  OGSAT    PsyCross,  terrain-only, m_LodDistance doubled (pure-HP reference)
  BLANK-og PsyCross,  --render-paths none

Masks (blank-leg method - never detect the background by colour; the native
clear is the per-level draw-env colour and screen corners can be covered):
  unc       = M  == BLANK-vk          nothing native drew
  ogc       = OG != BLANK-og          the game draws terrain here
  hpc       = OGSAT != BLANK-og       authored HP covers here
  need_data = ogc AND NOT hpc         sightlines only LP covers (level-data fact)
  need_orig = ogc AND (MO == BLANK-vk)  original-mode holes (mode-dependent)
  need      = need_data OR need_orig

Tag rule per pose, unioned across poses per (level, sector, face):
  AUTO tier:   secdraw==0, not fully-fogged, |footprint ∩ need_data| >= MIN_OV
  REVIEW tier: secdraw==0, fully-fogged,     |footprint ∩ need_data| >= MIN_OV
    (fully-fogged need is genuinely two-faced: level-boundary channels must
     fill while LP silhouette fringes over HP islands must not - a pixel
     threshold would be a tuned constant, so these rank for human review.)

Fully-fogged: every corner m3 >= (fogRefBase + fogbias*0x2000)/4, walk units.
Walk screen space is 512x240 with the 8-line overscan crop:
  screenshot px = (1.25*sx, 2*(sy-8)) at 640x448.

og legs and blanks are invariant under native knobs -> cached across runs.
Exit 0 = report produced, 64 = SKIP (binary/disc/PIL missing).
"""
import argparse, concurrent.futures, os, re, subprocess, sys
from collections import defaultdict

try:
    from PIL import Image
except ModuleNotFoundError:
    print("[fill-sweep] SKIP: python3-PIL missing", file=sys.stderr)
    sys.exit(64)

LVP_ICD = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
FRAME = 2101
MIN_M3 = 4.0        # walk units: skip faces with a corner nearer than 64 world units
MIN_OV = 16         # px of need_data overlap to become a candidate


def base_env():
    env = dict(os.environ)
    if os.path.exists(LVP_ICD):
        env["VK_ICD_FILENAMES"] = LVP_ICD
        env["MESA_LOADER_DRIVER_OVERRIDE"] = "llvmpipe"
    return env


def run_leg(binary, disc, level, pose, leg, outdir, frame=FRAME):
    """Capture one leg into outdir; returns the artifact path (png or log)."""
    art = {"M": f"{outdir}/M-{frame}-vk.png",       "MO": f"{outdir}/MO-{frame}-vk.png",
           "BLANKV": f"{outdir}/BLANKV-{frame}-vk.png",
           "OG": f"{outdir}/OG-{frame}-og.png",     "OGSAT": f"{outdir}/OGSAT-{frame}-og.png",
           "BLANKO": f"{outdir}/BLANKO-{frame}-og.png"}[leg]
    fmlog = f"{outdir}/facemap.log"
    if os.path.exists(art) and (leg != "M" or os.path.exists(fmlog)):
        return art
    env = base_env()
    rend, paths, lod = "vk", "terrain", "extended"
    if leg == "M":
        env.update(OPENPETE_TERRAIN_FACEMAP=str(frame), OPENPETE_TERRAIN_GAPFILL="0",
                   OPENPETE_TERRAIN_HOLEFILL="0")
    elif leg == "MO":
        env.update(OPENPETE_TERRAIN_GAPFILL="0", OPENPETE_TERRAIN_HOLEFILL="0")
        lod = "original"
    elif leg == "BLANKV":
        paths = "none"
    elif leg == "OG":
        rend, lod = "psycross", "original"
    elif leg == "OGSAT":
        rend, lod = "psycross", "original"
        # 2x the authored 0x8000: far beyond anything visible through fog, but
        # inside the .s frame prim budget - >=4x makes the big swamp levels
        # (misty_bog/beast_makers/ice_cavern) exhaust it and hang, crash, or
        # render empty. Validated pixel-equal (mod far-fog palette rows) to
        # full saturation on the levels that tolerate both.
        env["OPENPETE_ENV_LODDIST"] = "0x10000"
    elif leg == "BLANKO":
        rend, paths, lod = "psycross", "none", "original"
    tag = leg
    args = [binary, "--headless", "--disc", disc, "--disc-cache", "0",
            "--renderer", rend, "--mute", "1", "--aspect", "4:3",
            "--skip-to-level", level, "--lod", lod,
            "--render-paths", paths,
            "--screenshot-on-frame", str(frame),
            "--screenshot-output", f"{outdir}/{tag}-{{n}}.png",
            "--frame-limit", str(frame + 10), "--max-runtime-seconds", "380"]
    if pose != "default":
        # "default" = the canonical load-in camera (no freecam) - one
        # og-maximally-valid region view per level for free
        x, y, z, yaw, pitch = pose.split(",")
        args += ["--freecam-pose", f"pos=({x}, {y}, {z}) yaw={yaw} pitch={pitch}"]
    if rend == "vk":
        args.append("--screenshot-native-only")
    stderr = open(fmlog, "w") if leg == "M" else subprocess.DEVNULL
    r = subprocess.run(args, env=env, stdout=subprocess.DEVNULL, stderr=stderr, timeout=400)
    if leg == "M":
        stderr.close()
    if r.returncode != 0 or not os.path.exists(art):
        raise RuntimeError(f"{level} {leg}: capture failed (rc={r.returncode})")
    return art


def img(path):
    im = Image.open(path).convert("RGB")
    return list(im.getdata()), im.size[0], im.size[1]


FACE_RE = re.compile(
    r"\[facemap\] s=(\d+) f=(\d+) q=(\d) gf=(\d) flr=(\d) fb=(\d) fr=(\d) secdraw=(\d)"
    r"((?: v\d=[-\d.]+,[-\d.]+,[-\d.]+)+)")
HDR_RE = re.compile(r"\[facemap-hdr\] .*fogRefBase=(-?\d+)")


def parse_facemap(path):
    faces, fogref = [], None
    for line in open(path):
        m = FACE_RE.match(line)
        if m:
            vs = [tuple(float(x) for x in v.split("=")[1].split(","))
                  for v in m.group(9).split()]
            faces.append({"sec": int(m.group(1)), "f": int(m.group(2)),
                          "quad": int(m.group(3)), "gf": int(m.group(4)),
                          "flr": int(m.group(5)), "fb": int(m.group(6)),
                          "secdraw": int(m.group(8)), "v": vs})
            continue
        m = HDR_RE.match(line)
        if m:
            fogref = int(m.group(1))
    if fogref is None:
        raise RuntimeError(f"{path}: no [facemap-hdr] (dump incomplete)")
    return faces, fogref


def raster_tri(pts, w, h, out):
    (x0, y0), (x1, y1), (x2, y2) = pts
    minx = max(0, int(min(x0, x1, x2))); maxx = min(w - 1, int(max(x0, x1, x2)) + 1)
    miny = max(0, int(min(y0, y1, y2))); maxy = min(h - 1, int(max(y0, y1, y2)) + 1)
    if minx > maxx or miny > maxy: return
    d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
    if abs(d) < 1e-9: return
    for yy in range(miny, maxy + 1):
        for xx in range(minx, maxx + 1):
            a = ((y1 - y2) * (xx + 0.5 - x2) + (x2 - x1) * (yy + 0.5 - y2)) / d
            b = ((y2 - y0) * (xx + 0.5 - x2) + (x0 - x2) * (yy + 0.5 - y2)) / d
            if a >= -1e-6 and b >= -1e-6 and (1.0 - a - b) >= -1e-6:
                out.add(yy * w + xx)


def footprint(F, w, h):
    if any(v[2] < MIN_M3 for v in F["v"]): return None
    P = [(1.25 * v[0], 2.0 * (v[1] - 8.0)) for v in F["v"]]
    out = set()
    if F["quad"] and len(P) == 4:
        # quad ring order is not trusted: union both diagonals (slight
        # overcover only widens the candidate set; the fill stays stencil-gated)
        raster_tri((P[0], P[1], P[2]), w, h, out); raster_tri((P[1], P[3], P[2]), w, h, out)
        raster_tri((P[0], P[1], P[3]), w, h, out); raster_tri((P[0], P[3], P[2]), w, h, out)
    else:
        raster_tri((P[0], P[1], P[2]), w, h, out)
    return out


def analyze_pose(level, tag, outdir, tags, frame=FRAME):
    M, w, h = img(f"{outdir}/M-{frame}-vk.png")
    MO, _, _ = img(f"{outdir}/MO-{frame}-vk.png")
    BV, _, _ = img(f"{outdir}/BLANKV-{frame}-vk.png")
    OG, _, _ = img(f"{outdir}/OG-{frame}-og.png")
    OS, _, _ = img(f"{outdir}/OGSAT-{frame}-og.png")
    BO, _, _ = img(f"{outdir}/BLANKO-{frame}-og.png")
    need = [1 if (OG[i] != BO[i] and (OS[i] == BO[i] or MO[i] == BV[i])) else 0
            for i in range(w * h)]
    faces, fogref = parse_facemap(f"{outdir}/facemap.log")
    new_faces = 0
    for F in faces:
        if F["secdraw"]: continue                    # WORLD-drawn under extended
        fp = footprint(F, w, h)
        if not fp: continue
        ov = sum(1 for i in fp if need[i])
        if ov < MIN_OV: continue
        thr = (fogref + (0x2000 if F["fb"] else 0)) / 4.0
        fogged = all(v[2] >= thr for v in F["v"])
        key = (level, F["sec"], F["f"])
        rec = tags.get(key)
        if rec is None:
            rec = tags[key] = {"tier": "review" if fogged else "auto", "ov": 0,
                               "poses": [], "gf": F["gf"], "flr": F["flr"]}
            new_faces += 1
        elif rec["tier"] == "review" and not fogged:
            rec["tier"] = "auto"      # in-band need at any pose outranks fogged
        rec["ov"] += ov
        rec["poses"].append(f"{tag}:{ov}")
    # native-need residual: og-covered px native leaves uncovered that NO
    # candidate face reaches (the raster-sliver class the stencil fill owns)
    unc = [1 if M[i] == BV[i] else 0 for i in range(w * h)]
    ogc_unc = sum(1 for i in range(w * h) if OG[i] != BO[i] and unc[i])
    return new_faces, sum(need), ogc_unc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--disc", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--out", help="derived tag list path (default <cache>/derived-tags.txt)")
    ap.add_argument("--jobs", type=int, default=5)
    ap.add_argument("--only", help="substring filter on level or tag")
    args = ap.parse_args()

    if not os.path.exists(args.binary) or not os.path.exists(args.disc):
        print("[fill-sweep] SKIP: missing binary/disc"); return 64
    poses = []
    for line in open(args.manifest):
        line = line.split("#")[0].strip()
        if not line: continue
        parts = [p.strip() for p in line.split("|")]
        level, pose, tag = parts[0], parts[1], parts[2]
        frame = int(parts[3]) if len(parts) > 3 else FRAME   # optional per-pose frame
        if args.only and args.only not in level and args.only not in tag: continue
        poses.append((level, pose, tag, frame))
    if not poses:
        print("[fill-sweep] SKIP: no poses matched"); return 64

    # capture (cached legs skipped inside run_leg)
    os.makedirs(args.cache, exist_ok=True)
    jobs = []
    for level, pose, tag, frame in poses:
        outdir = os.path.join(args.cache, f"{level}__{tag}")
        os.makedirs(outdir, exist_ok=True)
        for leg in ("M", "MO", "BLANKV", "OG", "OGSAT", "BLANKO"):
            jobs.append((level, pose, leg, outdir, frame))
    fails = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_leg, args.binary, args.disc, lv, po, lg, od, fr): (lv, lg)
                for lv, po, lg, od, fr in jobs}
        for fut in concurrent.futures.as_completed(futs):
            try: fut.result()
            except Exception as e:
                print(f"[fill-sweep] FAIL {futs[fut]}: {e}"); fails += 1
    if fails:
        print(f"[fill-sweep] {fails} capture(s) failed; report covers the rest")

    # attribute, in manifest order (the order IS the marginal-new-faces curve)
    tags = {}
    print(f"\n{'level':<15}{'pose':<22}{'new':>5}{'need px':>9}{'resid px':>9}")
    for level, pose, tag, frame in poses:
        outdir = os.path.join(args.cache, f"{level}__{tag}")
        try:
            new, needpx, resid = analyze_pose(level, tag, outdir, tags, frame)
            print(f"{level:<15}{tag:<22}{new:>5}{needpx:>9}{resid:>9}")
        except Exception as e:
            print(f"{level:<15}{tag:<22}  ERROR: {e}")

    out = args.out or os.path.join(args.cache, "derived-tags.txt")
    auto = {k: v for k, v in tags.items() if v["tier"] == "auto"}
    review = {k: v for k, v in tags.items() if v["tier"] == "review"}
    with open(out, "w") as f:
        f.write("# terrain fill-face tags, derived by fill_sweep.py - do not hand-edit\n")
        f.write("# tier level sector face total-need-px gf-classifier poses\n")
        for tier, d in (("auto", auto), ("review", review)):
            for (lv, s, fc), v in sorted(d.items(), key=lambda kv: -kv[1]["ov"]):
                f.write(f"{tier} {lv} {s} {fc} {v['ov']} gf={v['gf']} {','.join(v['poses'])}\n")
    gf_agree = sum(1 for v in auto.values() if v["gf"])
    print(f"\n[fill-sweep] auto={len(auto)} (classifier-agreed {gf_agree}) "
          f"review={len(review)}  ->  {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
