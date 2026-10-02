#!/usr/bin/env python3
"""bench-topdown.py - per-level whole-level-visible perf sweep.

For every Spyro 1 level this skips straight in, engages the freecam at the
computed whole-level top-down pose (`--freecam-pose topdown`: centre of the
sector bounds, looking straight down, high enough that every sector fits the
frame), and profiles a fixed window with --profile-out. It is the worst-case
extraction corpus: the entire level's geometry is in-frustum every present.

Each level row reports fps avg / 1%-low, intermediate & canonical
present_busy p50, the extraction phase splits (rex.terrain, ter.walk,
ter.feed, compose, sim_tick) and the triangle count, plus the computed pose
(paste it back as a --freecam-pose to reproduce a row by hand). A screenshot
per level (--screenshot, default on) supports visual verification of the
pose coverage.

Platform note: CPU scopes are valid anywhere, but real fps needs the
windows-mingw .exe on the real GPU (--exe; path args are wslpath-translated,
and the exe runs from its build dir so DLLs/assets resolve).

Usage:
  scripts/bench-topdown.py [--exe PATH] [--levels a,b,c] [--lod extended|original]
      [--extract-threads N] [--terrain-cache 0|1] [--render-fps N] [--out-dir DIR]
      [--frame-limit N] [--skip-ticks N] [--no-screenshot]

Exit: 0 on a full sweep, 64 skip (missing exe/disc), 1 if any level run died.
"""
import argparse
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "fences"))
import bench_guard

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run - nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set in the environment
# rather than at each invocation because this script builds its argument list
# in more than one place, and a flag appended to the wrong one is a silent
# miss. (scripts/fences/rumble_guard.py enforces this.)
os.environ.setdefault("OPENPETE_VIBRATION", "0")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(ROOT, "scripts")
DISC = os.path.join(ROOT, "games/spyro-1/spyro-1/build/SCUS-94228.cue")
DEFAULT_EXE = os.path.join(ROOT, "build/linux-release/games/spyro-1/openpete-spyro1")
EXIT_SKIP = 64

# games/spyro-1/features/levelwarp.c k_levels - every warpable level.
LEVELS = [
    "artisans", "stone_hill", "dark_hollow", "town_square", "toasty",
    "sunny_flight",
    "peace_keepers", "dry_canyon", "cliff_town", "ice_cavern", "doctor_shemp",
    "night_flight",
    "magic_crafters", "alpine_ridge", "high_caves", "wizard_peak", "blowhard",
    "crystal_flight",
    "beast_makers", "terrace_village", "misty_bog", "tree_tops", "metalhead",
    "wild_flight",
    "dream_weavers", "dark_passage", "lofty_castle", "haunted_towers",
    "jacques", "icy_flight",
    "gnorc_gnexus", "gnorc_cove", "twilight_harbor", "gnasty_gnorc",
    "gnastys_loot",
]

POSE_RE = re.compile(r"topdown pose set to \((-?\d+), (-?\d+), (-?\d+)\) yaw=(-?\d+) pitch=(-?\d+)")


def wslpath_w(p):
    try:
        return subprocess.check_output(["wslpath", "-w", p], text=True).strip()
    except Exception:
        return p


def run_level(exe, level, args, out_dir):
    is_win = exe.endswith(".exe")
    tr = lambda p: wslpath_w(p) if is_win else p
    opp = os.path.join(out_dir, "%s.opp" % level)
    shot = os.path.join(out_dir, "%s.png" % level)
    cmd = [
        exe, "--disc", tr(DISC),
        "--skip-to-level", level, "--freecam-pose", "topdown",
        "--lod", args.lod,
        "--extract-threads", str(args.extract_threads),
        # An active HD texture pack (a mod can carry one) rides the fold
        # into the retained cache these days, but it still costs pack
        # machinery (resolve/intern, atlas uploads) and enabled mods run
        # their hooks - point --mods-dir at an empty dir to bench the
        # vanilla pipeline; benching WITH the play dir's enabled mods is
        # the pack-cost leg, not a baseline.
    ] + (["--terrain-cache", str(args.terrain_cache)]
         if args.terrain_cache is not None else []) \
      + (["--mods-dir", tr(args.mods_dir)] if args.mods_dir else []) \
      + (["--texpack-dir", tr(args.texpack_dir)] if args.texpack_dir else []) + [
        "--renderer", "vk", "--vsync", "0",
        "--render-fps", str(args.render_fps),
        "--profile-out", tr(opp),
        "--frame-limit", str(args.frame_limit),
        "--max-runtime-seconds", str(args.max_runtime_seconds),
        # Windowed and unattended: host keyboard, mouse and pads are ignored.
        # The flag, not the env spelling, so it also reaches a Windows exe.
        # (scripts/fences/input_lock_guard.py)
        "--input-lock",
    ]
    if args.screenshot:
        cmd += ["--screenshot-on-frame", str(args.screenshot_frame),
                "--screenshot-output", tr(shot)]
    # PIPE, not a file FD: a redirected fd collides with the exe's
    # --profile-out fopen on Windows (see bench-corpus.py).
    p = subprocess.run(cmd, cwd=os.path.dirname(exe) or None,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = (p.stdout or b"").decode("utf-8", "replace")
    with open(os.path.join(out_dir, "%s.log" % level), "w") as lf:
        lf.write(out)
    m = POSE_RE.search(out)
    pose = ("pos=(%s, %s, %s) yaw=%s pitch=%s" % m.groups()) if m else None
    return {"returncode": p.returncode, "pose": pose, "opp": opp,
            "shot": shot if args.screenshot else None}


def summarize(opp, skip_ticks, label):
    cmd = [sys.executable, os.path.join(SCRIPTS, "profile-summary.py"),
           opp, "--skip-ticks", str(skip_ticks), "--label", label,
           "--json", opp + ".json"]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        with open(opp + ".json") as f:
            return json.load(f)
    except Exception:
        return None


def scope_p50(summary, scope, kind):
    for r in summary.get("cpu_scopes", []):
        if r["scope"] == scope and r["kind"] == kind:
            return r["p50_us"]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--allow-untuned", action="store_true",
                    help="skip the bench_guard tuned-binary refusal")
    ap.add_argument("--levels", default=None, help="comma list (default: all)")
    ap.add_argument("--lod", default="extended",
                    choices=["extended", "original"])
    ap.add_argument("--extract-threads", type=int, default=0)
    ap.add_argument("--terrain-cache", type=int, default=None, choices=[0, 1],
                    help="omit to follow the exe default (cache on)")
    ap.add_argument("--mods-dir", default=None,
                    help="override the exe-adjacent mods root (an empty dir "
                         "disables mods; active texpacks exclude the cache)")
    ap.add_argument("--texpack-dir", default=None,
                    help="override the base HD texpack dir (an empty dir "
                         "disables it; same cache exclusion as mod packs)")
    ap.add_argument("--render-fps", type=int, default=1000)
    ap.add_argument("--frame-limit", type=int, default=3600)
    ap.add_argument("--max-runtime-seconds", type=int, default=240)
    ap.add_argument("--skip-ticks", type=int, default=200)
    ap.add_argument("--screenshot-frame", type=int, default=2600)
    ap.add_argument("--no-screenshot", dest="screenshot", action="store_false")
    ap.add_argument("--out-dir", default=None)
    args = ap.parse_args()

    args.exe = os.path.abspath(args.exe)
    if not os.path.isfile(args.exe):
        print("SKIP: exe not found: %s" % args.exe)
        return EXIT_SKIP
    if not os.path.isfile(DISC):
        print("SKIP: disc not found: %s" % DISC)
        return EXIT_SKIP

    bench_guard.check(args.exe, allow_untuned=args.allow_untuned)
    bench_guard.print_surface(args.exe, args.mods_dir, args.texpack_dir)

    levels = args.levels.split(",") if args.levels else LEVELS
    out_dir = os.path.abspath(args.out_dir) if args.out_dir else os.path.join(
        ROOT, "build", "bench-topdown",
        "%s-x%d%s" % (args.lod, args.extract_threads,
                      "-tc" if args.terrain_cache else ""))
    os.makedirs(out_dir, exist_ok=True)

    rows, failed = [], 0
    for lv in levels:
        run = run_level(args.exe, lv, args, out_dir)
        s = summarize(run["opp"], args.skip_ticks, lv)
        row = {"level": lv, "pose": run["pose"], "rc": run["returncode"]}
        if s:
            tris = next((c["avg"] for c in s.get("census", [])
                         if c["name"] == "tris"), None)
            row.update({
                "fps": s.get("fps", {}).get("avg"),
                "low1": s.get("fps", {}).get("1pct_low"),
                "inter_p50": s.get("kinds", {}).get("intermediate", {}).get("busy_p50_us"),
                "canon_p50": s.get("kinds", {}).get("canonical", {}).get("busy_p50_us"),
                "rex_ter": scope_p50(s, "rex.terrain", "intermediate"),
                "ter_walk": scope_p50(s, "ter.walk", "intermediate"),
                "ter_feed": scope_p50(s, "ter.feed", "intermediate"),
                "compose": scope_p50(s, "compose", "intermediate"),
                "sim": scope_p50(s, "sim_tick", "canonical"),
                "tris": tris,
            })
        else:
            failed += 1
        rows.append(row)
        print("%-16s fps=%-6s 1%%low=%-6s tris=%-7s rex.ter=%-5s feed=%-5s "
              "compose=%-5s sim=%-5s  %s" % (
                  lv, row.get("fps"), row.get("low1"), row.get("tris"),
                  row.get("rex_ter"), row.get("ter_feed"),
                  row.get("compose"), row.get("sim"),
                  run["pose"] or "(no pose - level never went live?)"))

    with open(os.path.join(out_dir, "rollup.json"), "w") as f:
        json.dump({"lod": args.lod, "extract_threads": args.extract_threads,
                   "terrain_cache": args.terrain_cache,
                   "render_fps": args.render_fps, "rows": rows}, f, indent=1)
    print("rollup: %s" % os.path.join(out_dir, "rollup.json"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
