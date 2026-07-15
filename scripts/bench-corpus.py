#!/usr/bin/env python3
"""bench-corpus.py — replay the bk2 corpus through the profiler and produce
per-movie + rollup performance reports, with optional regression compare.

For each games/spyro-1/bk2/*.bk2 (its .args sidecar is auto-spliced by --bk2)
this runs the game WINDOWED (never --headless: headless disables the native
renderer and intermediate frames) with --profile-out + --frame-time-log, then
summarizes each trace via profile-summary.py. A rollup aggregates every movie;
--baseline compares against a blessed rollup and fails on regressions.

Platform note: on WSL/llvmpipe the CPU scopes are valid but the GPU numbers are
a software rasterizer — real GPU measurement needs the windows-mingw-opt .exe
(pass it with --exe; path args are wslpath-translated automatically).

Usage:
  scripts/bench-corpus.py [--exe PATH] [--movies a,b,c] [--frame-limit N]
      [--render-fps N] [--out-dir DIR] [--skip-ticks N] [--gpu-timers]
      [--baseline rollup.json] [--threshold-pct P] [--threshold-us U]
      [--keep-traces] [--max-runtime-seconds N]

Exit: 0 clean, 1 regression vs baseline, 64 skip (missing exe/disc/corpus).
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(ROOT, "scripts")
DISC = os.path.join(ROOT, "games/spyro-1/spyro-1/build/SCUS-94228.cue")
BK2_DIR = os.path.join(ROOT, "games/spyro-1/bk2")
DEFAULT_EXE = os.path.join(ROOT, "build/linux-release/games/spyro-1/openpete-spyro1")
EXIT_SKIP = 64

DEVICE_RE = re.compile(r"device='([^']*)'")
EXIT_RE = re.compile(r"exit: reason=([a-z0-9-]+)")


def wslpath_w(p):
    try:
        return subprocess.check_output(["wslpath", "-w", p], text=True).strip()
    except Exception:
        return p


def run_movie(exe, movie_path, opp_path, args, log_path):
    is_win = exe.endswith(".exe")
    disc = wslpath_w(DISC) if is_win else DISC
    bk2 = wslpath_w(movie_path) if is_win else movie_path
    opp = wslpath_w(opp_path) if is_win else opp_path
    # NOTE: do NOT also pass --frame-time-log — a second concurrent file open
    # collides with --profile-out's fopen on the Windows exe and leaks stderr
    # into the .opp. fps comes from the trace's own present cadence anyway.
    cmd = [
        exe, "--disc", disc, "--run-recomp", "0x80012204",
        "--bk2", bk2, "--bk2-stop-on-end",
        "--renderer", "vk", "--vsync", "0",
        "--render-fps", str(args.render_fps),
        "--frame-limit", str(args.frame_limit),
        "--max-runtime-seconds", str(args.max_runtime_seconds),
        "--profile-out", opp,
    ]
    env = dict(os.environ)
    if args.gpu_timers:
        env["OPENPETE_GPU_TIMERS"] = "1"
    # Capture via PIPE, not a file FD: redirecting a Windows .exe's stdout to a
    # real file descriptor makes its --profile-out fopen collide with the
    # redirected fd, leaking late stderr into the .opp trace. PIPE avoids it.
    p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout or b""
    with open(log_path, "wb") as lf:
        lf.write(out)
    stderr_txt = out.decode("utf-8", "replace")
    dev = DEVICE_RE.search(stderr_txt)
    reason = EXIT_RE.search(stderr_txt)
    return {
        "returncode": p.returncode,
        "device": dev.group(1) if dev else "unknown",
        "exit_reason": reason.group(1) if reason else "unknown",
    }


def summarize(opp_path, json_path, txt_path, label, skip_ticks):
    cmd = [sys.executable, os.path.join(SCRIPTS, "profile-summary.py"),
           opp_path, "--skip-ticks", str(skip_ticks), "--label", label,
           "--json", json_path]
    with open(txt_path, "wb") as tf:
        subprocess.run(cmd, stdout=tf, stderr=subprocess.STDOUT)
    try:
        with open(json_path) as f:
            return json.load(f)
    except Exception:
        return None


def platform_tag(exe, device):
    build = os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(exe))))
    soft = any(s in device.lower() for s in ("llvmpipe", "swiftshader", "software"))
    return {"build": build, "device": device, "software_gpu": soft,
            "tag": "%s/%s" % (build, "SOFTWARE" if soft else device.split("(")[0].strip())}


def build_rollup(per_movie, platform):
    """Aggregate cpu_scopes / gpu_zones across movies by (scope, kind)."""
    agg = {}   # kind -> {"cpu"/"gpu": {scope: {"sum_ms":, "p99_max":, "p99_movie":}}}
    for m in per_movie:
        s = m["summary"]
        if not s:
            continue
        for group in ("cpu_scopes", "gpu_zones"):
            for r in s[group]:
                key = (group, r["kind"], r["scope"])
                a = agg.setdefault(key, {"sum_ms": 0.0, "p99_max": 0, "p99_movie": "",
                                         "movies": 0})
                a["sum_ms"] += r["sum_ms"]
                a["movies"] += 1
                if r["p99_us"] > a["p99_max"]:
                    a["p99_max"] = r["p99_us"]
                    a["p99_movie"] = s["label"] or s["trace"]
    rows = []
    for (group, kind, scope), a in agg.items():
        rows.append({"group": group, "kind": kind, "scope": scope,
                     "sum_ms": round(a["sum_ms"], 1), "p99_max_us": a["p99_max"],
                     "p99_worst_movie": a["p99_movie"], "movies": a["movies"]})
    rows.sort(key=lambda r: (r["group"], -r["sum_ms"]))
    return {"platform": platform, "movies": len(per_movie), "scopes": rows,
            "per_movie": [{"movie": m["movie"], "exit_reason": m["run"]["exit_reason"],
                           "presents": (m["summary"] or {}).get("presents_total", 0),
                           "fps": (m["summary"] or {}).get("fps", {})}
                          for m in per_movie]}


def render_rollup_text(roll):
    L = []
    p = roll["platform"]
    banner = "SOFTWARE-GPU (CPU valid, GPU numbers invalid)" if p["software_gpu"] else "real-GPU"
    L.append("# openpete bench-corpus rollup  platform=%s  %s" % (p["tag"], banner))
    L.append("# movies=%d  device=%s" % (roll["movies"], p["device"]))
    L.append("[per-movie]           presents   fps_avg  exit")
    for pm in roll["per_movie"]:
        L.append("%-22s%-11d%-9s%s" % (pm["movie"][:22], pm["presents"],
                                       pm["fps"].get("avg", "?"), pm["exit_reason"]))
    for group, title in (("cpu_scopes", "cpu.scope"), ("gpu_zones", "gpu.zone")):
        rows = [r for r in roll["scopes"] if r["group"] == group]
        if not rows:
            continue
        L.append("[%s]%skind          sum_ms   p99_max_us  worst_movie" %
                 (title, " " * max(1, 16 - len(title))))
        for r in rows:
            L.append("%-18s%-12s%9.1f%12d  %s" %
                     (r["scope"][:18], r["kind"], r["sum_ms"], r["p99_max_us"],
                      r["p99_worst_movie"]))
    return "\n".join(L)


def compare_baseline(roll, baseline_path, thr_pct, thr_us):
    with open(baseline_path) as f:
        base = json.load(f)
    bp, cp = base["platform"]["tag"], roll["platform"]["tag"]
    if bp != cp:
        print("[bench] baseline platform %s != current %s — refusing cross-platform "
              "compare" % (bp, cp), file=sys.stderr)
        return EXIT_SKIP
    bmap = {(r["group"], r["kind"], r["scope"]): r for r in base["scopes"]}
    regressions = []
    for r in roll["scopes"]:
        b = bmap.get((r["group"], r["kind"], r["scope"]))
        if not b or b["p99_max_us"] <= 0:
            continue
        d_us = r["p99_max_us"] - b["p99_max_us"]
        d_pct = 100.0 * d_us / b["p99_max_us"]
        if d_pct > thr_pct and d_us > thr_us:
            regressions.append((r["group"], r["kind"], r["scope"],
                                b["p99_max_us"], r["p99_max_us"], d_pct))
    if regressions:
        print("\n[bench] REGRESSIONS vs baseline (p99_max, >%.0f%% and >%dus):"
              % (thr_pct, thr_us), file=sys.stderr)
        for (g, k, s, bv, cv, dp) in regressions:
            print("  %s %s %s: %d -> %d us (+%.1f%%)" % (s, k, g, bv, cv, dp),
                  file=sys.stderr)
        return 1
    print("[bench] no regressions vs baseline (%d scopes checked)" % len(roll["scopes"]),
          file=sys.stderr)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--movies", default=None, help="comma list of bk2 stems (default: all)")
    ap.add_argument("--frame-limit", type=int, default=20000)
    ap.add_argument("--render-fps", type=int, default=120)
    ap.add_argument("--max-runtime-seconds", type=int, default=600)
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "out/bench"))
    ap.add_argument("--skip-ticks", type=int, default=60)
    ap.add_argument("--gpu-timers", action="store_true")
    ap.add_argument("--baseline", default=None)
    ap.add_argument("--threshold-pct", type=float, default=10.0)
    ap.add_argument("--threshold-us", type=int, default=100)
    ap.add_argument("--keep-traces", action="store_true")
    ap.add_argument("--run-id", default="run")
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        print("[bench] exe not found: %s" % args.exe, file=sys.stderr)
        return EXIT_SKIP
    if not os.path.exists(DISC):
        print("[bench] disc not found: %s" % DISC, file=sys.stderr)
        return EXIT_SKIP

    movies = sorted(glob.glob(os.path.join(BK2_DIR, "*.bk2")))
    if args.movies:
        want = set(args.movies.split(","))
        movies = [m for m in movies
                  if os.path.basename(m)[:-4] in want]
    if not movies:
        print("[bench] no movies matched", file=sys.stderr)
        return EXIT_SKIP

    out = os.path.join(args.out_dir, args.run_id)
    os.makedirs(out, exist_ok=True)

    per_movie = []
    platform = None
    for mp in movies:
        stem = os.path.basename(mp)[:-4]
        opp = os.path.join(out, stem + ".opp")
        log = os.path.join(out, stem + ".log")
        print("[bench] %s ..." % stem, file=sys.stderr, flush=True)
        run = run_movie(args.exe, mp, opp, args, log)
        if platform is None:
            platform = platform_tag(args.exe, run["device"])
        summ = None
        if os.path.exists(opp):
            summ = summarize(opp, os.path.join(out, stem + ".json"),
                             os.path.join(out, stem + ".txt"), stem, args.skip_ticks)
        if not args.keep_traces and os.path.exists(opp):
            os.remove(opp)
        per_movie.append({"movie": stem, "run": run, "summary": summ})
        print("[bench]   exit=%s presents=%s" %
              (run["exit_reason"], (summ or {}).get("presents_total", "?")),
              file=sys.stderr)

    roll = build_rollup(per_movie, platform)
    with open(os.path.join(out, "rollup.json"), "w") as f:
        json.dump(roll, f, indent=2)
    txt = render_rollup_text(roll)
    with open(os.path.join(out, "rollup.txt"), "w") as f:
        f.write(txt + "\n")
    print("\n" + txt)
    print("\n[bench] wrote %s" % os.path.join(out, "rollup.json"), file=sys.stderr)

    if args.baseline:
        return compare_baseline(roll, args.baseline, args.threshold_pct, args.threshold_us)
    return 0


if __name__ == "__main__":
    sys.exit(main())
