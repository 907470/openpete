#!/usr/bin/env python3
"""Freecam-corpus regression harness for the terrain near-LP gapfill
classifier.  Reads a TOML corpus of freecam spots + metric expectations
(see gapfill-corpus.toml for the schema and how to add a spot), boots the
headless game per case with the feature ON and (when a check needs it)
OFF, and asserts the expectations.  Software-Vulkan (llvmpipe) is forced
when available so results are deterministic across machines.

Exit codes: 0 = all PASS, 1 = a check FAILed, 64 = SKIP (binary/disc/
prereqs missing — mirrors scripts/check_psx_oracle.sh).
"""
import argparse, concurrent.futures, os, re, subprocess, sys, tempfile

try:
    import tomllib
except ModuleNotFoundError:
    try:
        import tomli as tomllib
    except ModuleNotFoundError:
        print("[gapfill-corpus] SKIP: no tomllib/tomli", file=sys.stderr)
        sys.exit(64)
try:
    from PIL import Image
except ModuleNotFoundError:
    print("[gapfill-corpus] SKIP: python3-PIL missing", file=sys.stderr)
    sys.exit(64)

LVP_ICD = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"


def run_game(binary, disc, case, gapfill, outdir, tag):
    env = dict(os.environ)
    # ab_env: which feature toggle the on/off pair flips (default: the gapfill
    # classifier; e.g. "OPENPETE_TERRAIN_HOLEFILL" for fill-neutrality cases).
    env[case.get("ab_env", "OPENPETE_TERRAIN_GAPFILL")] = "1" if gapfill else "0"
    if os.path.exists(LVP_ICD):          # deterministic software raster
        env["VK_ICD_FILENAMES"] = LVP_ICD
        env["MESA_LOADER_DRIVER_OVERRIDE"] = "llvmpipe"
    if case.get("pose"):
        env["OPENPETE_FREECAM_POSE"] = case["pose"]
    args = [binary, "--headless", "--disc", disc, "--run-recomp", "0x80012204",
            "--renderer", "vk"]
    if case.get("level"):
        args += ["--skip-to-level", case["level"]]
    if case.get("bk2"):
        # Scripted-input replay (repo-root-relative .bk2 path) for spots that
        # need gameplay to happen (e.g. a cutscene) rather than a freecam pose.
        # Pair with `level` to mirror the .bk2.args the movie was recorded with.
        args += ["--bk2", case["bk2"]]
    if case.get("lod"):
        args += ["--lod", case["lod"]]
    if case.get("mode", "full") == "terrain":
        args += ["--render-paths", "terrain"]
    out = os.path.join(outdir, tag + "-{n}.png")
    if case.get("ticks"):
        args += ["--screenshot-on-tick", ",".join(str(t) for t in case["ticks"]),
                 "--screenshot-output", out,
                 "--frame-limit", "20000", "--max-runtime-seconds", "420"]
        shots = [os.path.join(outdir, f"{tag}-{t}-vk.png") for t in case["ticks"]]
    else:
        frame = int(case.get("frame", 2100))
        args += ["--screenshot-on-frame", str(frame),
                 "--screenshot-output", out,
                 "--frame-limit", str(frame + 10), "--max-runtime-seconds", "240"]
        shots = [os.path.join(outdir, f"{tag}-{frame}-vk.png")]
    r = subprocess.run(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    missing = [s for s in shots if not os.path.exists(s)]
    if r.returncode != 0 or missing:
        raise RuntimeError(f"game run failed (rc={r.returncode}, missing={missing})")
    return shots


def diff_px(a, b, bbox=None):
    A, B = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    if A.size != B.size:
        return A.size[0] * A.size[1]
    pa, pb = A.load(), B.load()
    x0, y0 = (bbox[0], bbox[1]) if bbox else (0, 0)
    x1, y1 = (min(bbox[2], A.size[0] - 1), min(bbox[3], A.size[1] - 1)) if bbox \
             else (A.size[0] - 1, A.size[1] - 1)
    return sum(1 for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)
               if pa[x, y] != pb[x, y])


def bg_px(img, bbox, probe):
    A = Image.open(img).convert("RGB")
    pa = A.load()
    bg = pa[probe[0], probe[1]]
    x0, y0, x1, y1 = bbox[0], bbox[1], min(bbox[2], A.size[0] - 1), min(bbox[3], A.size[1] - 1)
    return sum(1 for y in range(y0, y1 + 1) for x in range(x0, x1 + 1) if pa[x, y] == bg)


def eval_case(binary, disc, case, outdir):
    checks = case.get("check", [])
    need_off = any(c["kind"].startswith("ab-") for c in checks)
    tag = re.sub(r"[^A-Za-z0-9_-]", "_", case["name"])
    on = run_game(binary, disc, case, True, outdir, tag + "-on")
    off = run_game(binary, disc, case, False, outdir, tag + "-off") if need_off else None
    results = []
    for c in checks:
        bbox = c.get("bbox")
        for k, shot_on in enumerate(on):
            label = f"{case['name']}[{os.path.basename(shot_on)}]" if len(on) > 1 else case["name"]
            if c["kind"] == "ab-identical":
                n = diff_px(shot_on, off[k], bbox)
                ok = n <= c.get("max_px", 0)
                results.append((ok, f"{label} ab-identical diff={n} max={c.get('max_px', 0)}"))
            elif c["kind"] == "ab-active":
                n = diff_px(shot_on, off[k], bbox)
                ok = n >= c["min_px"]
                results.append((ok, f"{label} ab-active diff={n} min={c['min_px']}"))
            elif c["kind"] == "bg-max":
                n = bg_px(shot_on, c["bbox"], c.get("probe", [2, 2]))
                ok = n <= c["max_px"]
                results.append((ok, f"{label} bg-max bg={n} max={c['max_px']}"))
            else:
                results.append((False, f"{label} unknown check kind {c['kind']!r}"))
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--disc", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--only", help="substring filter on case names")
    ap.add_argument("--keep", help="directory to keep screenshots in (default: temp)")
    args, _ = ap.parse_known_args()

    if not os.path.exists(args.binary) or not os.path.exists(args.disc):
        print(f"[gapfill-corpus] SKIP: missing {args.binary if not os.path.exists(args.binary) else args.disc}")
        return 64
    with open(args.corpus, "rb") as f:
        corpus = tomllib.load(f)
    cases = [c for c in corpus.get("case", [])
             if not args.only or args.only in c["name"]]
    if not cases:
        print("[gapfill-corpus] SKIP: no cases matched")
        return 64

    tmp = None
    outdir = args.keep
    if not outdir:
        tmp = tempfile.TemporaryDirectory(prefix="gapfill-corpus-")
        outdir = tmp.name
    os.makedirs(outdir, exist_ok=True)

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(eval_case, args.binary, args.disc, c, outdir): c for c in cases}
        for fut in concurrent.futures.as_completed(futs):
            case = futs[fut]
            try:
                for ok, msg in fut.result():
                    print(("PASS " if ok else "FAIL ") + msg)
                    failed += 0 if ok else 1
            except Exception as e:
                print(f"FAIL {case['name']} harness error: {e}")
                failed += 1
    print(f"[gapfill-corpus] verdict: {'PASS' if failed == 0 else f'FAIL ({failed} check(s))'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
