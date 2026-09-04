#!/usr/bin/env python3
"""particle-path-census.py — pool-active-but-not-drawn ticks per particle renderer.

The particle, glow, sparkle and dragon-star pools outlive their .s renderers.
A tick where a pool holds active records but the renderer was not called or
emitted nothing is a tick the sub-tick spine must NOT replay that pool on; the
retired shared RP_PARTICLES gating replayed it whenever any sibling had drawn,
and the dragon star still shares RP_PARTICLES. This runs every presentation-
corpus bk2 (plus the attract demo) with OPENPETE_PARTICLE_PATH_CENSUS=1 and
aggregates the per-row `particle path census:` exit line.

  scripts/particle-path-census.py [--out DIR] [--filter SUBSTR] [--jobs N] [--no-demo]

Output: <out>/report.md, <out>/logs/<row>.txt (the exit line).
"""
import argparse, os, re, subprocess, zipfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("OPENPETE_BIN", os.path.join(ROOT, "build/linux-release/games/spyro-1/openpete-spyro1"))
DISC = os.environ.get("OPENPETE_DISC", os.path.join(ROOT, "games/spyro-1/spyro-1/build/SCUS-94228.cue"))
EXIT_RE = re.compile(r"particle path census: ticks=(\d+) glow: active=(\d+) ran=(\d+) exposed=(\d+) first=(\d+) \| "
                     r"sparkle: active=(\d+) ran=(\d+) exposed=(\d+) first=(\d+) \| "
                     r"star: active=(\d+) ran=(\d+) exposed=(\d+) first=(\d+)")
KEYS = ["ticks", "glow_active", "glow_ran", "glow_exposed", "glow_first",
        "spark_active", "spark_ran", "spark_exposed", "spark_first",
        "star_active", "star_ran", "star_exposed", "star_first"]


def read_manifest(path):
    rows = []
    for ln in open(path):
        ln = ln.split("#", 1)[0].strip()
        if not ln:
            continue
        rows.append([c.strip() for c in ln.split("|")])
    return rows


def bk2_frames(name):
    p = os.path.join(ROOT, "games/spyro-1/bk2", name + ".bk2")
    try:
        with zipfile.ZipFile(p) as z:
            log = next(n for n in z.namelist() if n.lower().startswith("input log"))
            return sum(1 for ln in z.read(log).decode("utf-8", "replace").splitlines() if ln.startswith("|"))
    except Exception:
        return 0


def run_row(slug, args, frame_limit, outdir, timeout_s):
    env = dict(os.environ)
    env.update({"OPENPETE_PARTICLE_PATH_CENSUS": "1", "OPENPETE_VIBRATION": "0",
                "MESA_LOADER_DRIVER_OVERRIDE": env.get("MESA_LOADER_DRIVER_OVERRIDE", "llvmpipe")})
    lvp = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
    if os.path.exists(lvp):
        env.setdefault("VK_ICD_FILENAMES", lvp)
    logs = os.path.join(outdir, "logs"); os.makedirs(logs, exist_ok=True)
    shots = os.path.join(outdir, "shots"); os.makedirs(shots, exist_ok=True)
    # The hooks run headless only when a native capture is requested.
    cmd = [BIN, "--headless", "--no-config", "--disc", DISC, "--run-recomp", "0x80012204",
           "--renderer", "vk", "--mute", "1", "--aspect", "16:9", "--lod", "original", "--mods", "",
           "--screenshot-native-only", "--screenshot-on-frame", str(frame_limit - 5),
           "--screenshot-output", os.path.join(shots, f"{slug}__{{n}}.png"),
           "--frame-limit", str(frame_limit), "--max-runtime-seconds", str(timeout_s - 20)] + args
    # The exit line is a single stderr write at process exit; under a fanned-out
    # run it is occasionally torn, so a row without one is run a second time.
    for attempt in range(2):
        try:
            p = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, timeout=timeout_s)
            err = p.stderr
        except subprocess.TimeoutExpired as e:
            err = e.stderr if isinstance(e.stderr, str) else (e.stderr or b"").decode("utf-8", "replace")
        m = EXIT_RE.search(err)
        if m:
            break
    with open(os.path.join(logs, f"{slug}.txt"), "w") as f:
        f.write(f"frame_limit={frame_limit}\n" + (m.group(0) if m else "(no exit line)\n" + err[-4000:]) + "\n")
    if not m:
        return slug, None
    return slug, dict(zip(KEYS, (int(x) for x in m.groups())))


def write_report(out, results, fails):
    tot = {k: 0 for k in KEYS if not k.endswith("_first")}
    lines = ["# Particle path census", "",
             f"rows run: {len(results)} ok, {len(fails)} failed" + (": " + ", ".join(fails) if fails else ""), "",
             "exposed = ticks where the pool holds active records, the renderer did not draw, and the gate that",
             "would replay it was open (glow/sparkle: any sibling drew = the retired shared gating; star: the",
             "particle path drew = the live gating). first = first VBL of that row's first exposed tick.", "",
             "| row | ticks | glow act/ran/exposed (first) | sparkle act/ran/exposed (first) | star act/ran/exposed (first) |",
             "|---|---|---|---|---|"]
    for slug, d in sorted(results, key=lambda r: -(r[1]["glow_exposed"] + r[1]["spark_exposed"] + r[1]["star_exposed"])):
        for k in tot:
            tot[k] += d[k]
        lines.append(f"| {slug} | {d['ticks']} | {d['glow_active']}/{d['glow_ran']}/{d['glow_exposed']} ({d['glow_first']}) | "
                     f"{d['spark_active']}/{d['spark_ran']}/{d['spark_exposed']} ({d['spark_first']}) | "
                     f"{d['star_active']}/{d['star_ran']}/{d['star_exposed']} ({d['star_first']}) |")
    lines += ["", "## Totals", "", f"ticks={tot['ticks']}",
              f"glow: active={tot['glow_active']} ran={tot['glow_ran']} exposed={tot['glow_exposed']}",
              f"sparkle: active={tot['spark_active']} ran={tot['spark_ran']} exposed={tot['spark_exposed']}",
              f"star: active={tot['star_active']} ran={tot['star_ran']} exposed={tot['star_exposed']}", ""]
    open(os.path.join(out, "report.md"), "w").write("\n".join(lines))
    print("\n".join(lines[-6:]))
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build/particle-path-census"))
    ap.add_argument("--filter", default="")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("CORPUS_JOBS", 0)) or max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--no-demo", action="store_true")
    ap.add_argument("--demo-frames", type=int, default=40000)
    ap.add_argument("--settle", type=int, default=400)
    opts = ap.parse_args()
    os.makedirs(opts.out, exist_ok=True)
    jobs = []
    for row in read_manifest(os.path.join(ROOT, "scripts/presentation-corpus.manifest")):
        name = row[0]
        if opts.filter and opts.filter not in name:
            continue
        n = bk2_frames(name)
        if n <= 0:
            print(f"  SKIP {name}: no/empty bk2"); continue
        jobs.append((name, ["--bk2", f"games/spyro-1/bk2/{name}.bk2"], n + opts.settle, 600))
    if not opts.no_demo and (not opts.filter or opts.filter in "attract-demo"):
        jobs.append(("attract-demo", [], opts.demo_frames, 1800))
    print(f"[particle-path-census] {len(jobs)} rows, jobs={opts.jobs}")
    results, fails = [], []
    with ThreadPoolExecutor(max_workers=opts.jobs) as ex:
        futs = [ex.submit(run_row, s, a, fl, opts.out, to) for (s, a, fl, to) in jobs]
        for fut in futs:
            slug, d = fut.result()
            if d is None:
                fails.append(slug); print(f"  FAIL {slug}")
            else:
                results.append((slug, d))
                print(f"  ok   {slug}: ticks={d['ticks']} glow={d['glow_exposed']} sparkle={d['spark_exposed']} star={d['star_exposed']}")
    return write_report(opts.out, results, fails)


if __name__ == "__main__":
    raise SystemExit(main())
