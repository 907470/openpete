#!/usr/bin/env python3
"""bench-corpus.py - replay a game's bk2 corpus through the profiler and
produce per-movie + rollup performance reports, with optional regression
compare.

For each games/<game>/bk2/*.bk2 (its .args sidecar is auto-spliced by --bk2;
the game's boot flags, RUN_ARGS of games/<game>/oracle.conf, precede it) this
runs the game WINDOWED (headless disables the native renderer and
intermediate frames) with --profile-out, then summarizes each trace via
profile-summary.py. A rollup aggregates every movie; --baseline compares
against a blessed rollup and fails on regressions.

A rollup records what makes two rollups comparable: the game, the movie set,
the platform tag (the device plus the preset class, the basename of the
tree's CMAKE_PROJECT_openpete_INCLUDE file, so two trees of one preset class
on one device compare), the arm (the linkage the configure wrote to
openpete-recomp-linkage.txt at the tree root; a tree without the file is
refused), the source commit, and the mode. Every movie's engaged ticks (the
canonical presents the summary counted) are recorded; a movie with none
makes the run VACUOUS.

--functional runs headless and does not refuse on contention: it proves the
path runs, produces no timing verdict, and its rollup compares only against
another functional rollup (identity only). tests/harness/bench-ab.py refuses
it.

Platform note: on WSL/llvmpipe the CPU scopes are valid but the GPU numbers are
a software rasterizer - real GPU measurement needs the windows-mingw .exe
(pass it with --exe; path args are wslpath-translated automatically).

Usage:
  tests/harness/bench-corpus.py [--game G] [--exe PATH] [--movies a,b,c]
      [--frame-limit N] [--render-fps N] [--out-dir DIR] [--skip-ticks N]
      [--gpu-timers] [--functional] [--baseline rollup.json]
      [--threshold-pct P] [--threshold-us U] [--keep-traces]
      [--max-runtime-seconds N]
  tests/harness/bench-corpus.py --compare-only CURRENT.json --baseline BASE.json

A run resumes: each finished movie writes <stem>.done.json beside its
summary, carrying the run's fingerprint (exe size and mtime, source commit,
the timing flags and every OPENPETE_* knob in the environment). Rerunning the
same command with the same --run-id reuses every movie whose record matches
and replays the rest, so a stop or a crash costs at most the movie in
progress; a record with another fingerprint is refused unless --fresh, which
discards the run directory's movies. SIGINT or SIGTERM stops the movie in
progress (its exe is terminated) and exits 130; a file named STOP in the run
directory stops the run after the movie in progress. Output lives under a
persistent directory (default ${XDG_DATA_HOME:-~/.local/share}/openpete/bench);
an --out-dir under /tmp is refused, since /tmp does not survive a restart.

Exit: 0 clean, 1 regression vs baseline, 2 VACUOUS (a movie engaged no
tick), 64 skip (missing exe/disc/corpus) or a refused compare (platform tag,
mode, game or arm differ), 65 refused precondition (bench_guard, missing arm
file, an --out-dir under /tmp, a fingerprint mismatch without --fresh),
130 stopped (signal or STOP file; rerun the command to resume).
"""
import argparse
import glob
import json
import os
import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parents[2] / "scripts" / "lib"))
from hostenv import child_env, scratch_cards  # noqa: E402
import re
import shlex
import signal
import subprocess
import sys

import game_conf  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "scripts", "fences"))
import bench_guard

# Unattended launcher: every run this script starts has controller vibration
# off. Vibration ships on by default, which is right for a player and wrong
# for a scripted run - no player uses the pad, and the buzzing shows up in no
# log. Forwarded through child_env with the run's other knobs, since a
# Windows exe launched from WSL receives only the names WSLENV lists.
# (scripts/fences/rumble_guard.py, scripts/fences/wslenv_guard.py)
RUN_KNOBS = {"OPENPETE_VIBRATION": "0"}
# The player's memory cards are never opened: runs boot on scratch cards.
scratch_cards()

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPTS = os.path.join(ROOT, "scripts")
EXIT_VACUOUS = 2
EXIT_SKIP = 64
EXIT_REFUSED = 65
EXIT_STOPPED = 130
DEFAULT_OUT = os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"),
                           "openpete", "bench")
# Knobs that differ per process without changing what a run measures: the
# scratch card directory scratch_cards() makes for each process.
RUN_LOCAL_KNOBS = {"OPENPETE_CARD_DIR"}
# The movie in progress, so a stop signal can end its exe before exiting.
_CHILD = None


class Stopped(Exception):
    """A stop signal arrived; the run directory keeps every finished movie."""


def _on_stop(signum, _frame):
    """Terminate the movie in progress and unwind to main's stop exit."""
    if _CHILD is not None and _CHILD.poll() is None:
        _CHILD.terminate()
    raise Stopped(signal.Signals(signum).name)
# 2: game, arm and commit recorded. 3: every run launched with --no-config.
ROLLUP_SCHEMA = 3

DEVICE_RE = re.compile(r"device='([^']*)'")
EXIT_RE = re.compile(r"exit: reason=([a-z0-9-]+)")


def wslpath_w(p):
    try:
        return subprocess.check_output(["wslpath", "-w", p], text=True).strip()
    except Exception:
        return p


def run_movie(exe, game, movie_path, opp_path, args, log_path):
    is_win = exe.endswith(".exe")
    disc = wslpath_w(game["disc"]) if is_win else game["disc"]
    bk2 = wslpath_w(movie_path) if is_win else movie_path
    opp = wslpath_w(opp_path) if is_win else opp_path
    # NOTE: do NOT also pass --frame-time-log - a second concurrent file open
    # collides with --profile-out's fopen on the Windows exe and leaks stderr
    # into the .opp. fps comes from the trace's own present cadence anyway.
    cmd = [
        exe, "--disc", disc] + game["run_args"] + [
        "--bk2", bk2, "--bk2-stop-on-end",
        "--renderer", "vk", "--vsync", "0",
        "--render-fps", str(args.render_fps),
        "--frame-limit", str(args.frame_limit),
        "--max-runtime-seconds", str(args.max_runtime_seconds),
        "--profile-out", opp,
        # Windowed and unattended: host keyboard, mouse and pads are ignored.
        # The flag, not the env spelling, so it also reaches a Windows exe.
        # (scripts/fences/input_lock_guard.py)
        "--input-lock",
        # No openpete.toml: a config file beside the exe (or in the user
        # config dir) sets render options such as ssaa, lod and aspect, so two
        # arms with different files would time different work.
        "--no-config",
    ] + (["--headless"] if args.functional else [])
    # Through child_env so every knob survives a Windows launch from WSL: set
    # directly it would be dropped by interop and the run would quietly use
    # the default.
    knobs = dict(RUN_KNOBS)
    if args.gpu_timers:
        knobs["OPENPETE_GPU_TIMERS"] = "1"
    env = child_env(knobs)
    # Capture via PIPE, not a file FD: redirecting a Windows .exe's stdout to a
    # real file descriptor makes its --profile-out fopen collide with the
    # redirected fd, leaking late stderr into the .opp trace. PIPE avoids it.
    global _CHILD
    _CHILD = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out = _CHILD.communicate()[0] or b""
    except BaseException:
        if _CHILD.poll() is None:
            _CHILD.kill()
            _CHILD.wait()
        raise
    p, _CHILD = _CHILD, None
    with open(log_path, "wb") as lf:
        lf.write(("# %s\n" % shlex.join(cmd)).encode())
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


def run_fingerprint(exe, identity, args):
    """What makes a finished movie of this run reusable by a resumed one."""
    st = os.stat(exe)
    return {"exe": exe, "exe_size": st.st_size, "exe_mtime_ns": st.st_mtime_ns,
            "commit": (identity or {}).get("commit"), "game": args.game,
            "functional": args.functional, "frame_limit": args.frame_limit,
            "render_fps": args.render_fps, "max_runtime_seconds": args.max_runtime_seconds,
            "skip_ticks": args.skip_ticks, "gpu_timers": args.gpu_timers,
            "knobs": {k: v for k, v in sorted(os.environ.items())
                      if k.startswith("OPENPETE_") and k not in RUN_LOCAL_KNOBS}}


def load_done(path, fp):
    """The finished-movie record at `path`: (record, None) when it matches `fp`,
    (None, reason) when it exists and does not, (None, None) when absent."""
    if not os.path.exists(path):
        return None, None
    try:
        with open(path) as f:
            rec = json.load(f)
    except (OSError, ValueError) as e:
        return None, "unreadable (%s)" % e
    if rec.get("fingerprint") != fp:
        diff = sorted(k for k in set(fp) | set(rec.get("fingerprint") or {})
                      if fp.get(k) != (rec.get("fingerprint") or {}).get(k))
        return None, "fingerprint differs in %s" % ", ".join(diff)
    return rec, None


def write_atomic(path, obj):
    """Write `obj` as JSON so a crash leaves the old file or the new, never half."""
    tmp = path + ".part"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def platform_tag(identity, device):
    """The comparison tag: the preset class plus the device. The tree's
    directory name and absolute path stay out of it, so two trees of one
    preset class on one device (a checkout's and a worktree's) compare."""
    soft = any(s in device.lower() for s in ("llvmpipe", "swiftshader", "software"))
    preset = identity["preset_class"] or "unknown-preset"
    return {"tree": identity["tree"], "preset_class": preset,
            "device": device, "software_gpu": soft,
            "tag": "%s/%s" % (preset, "SOFTWARE" if soft else device.split("(")[0].strip())}


def build_rollup(per_movie, platform, meta):
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
    pm_rows = [{"movie": m["movie"], "exit_reason": m["run"]["exit_reason"],
                "returncode": m["run"]["returncode"],
                "args_sidecar": m["args_sidecar"],
                "presents": (m["summary"] or {}).get("presents_total", 0),
                "engaged_ticks": engaged_ticks(m["summary"]),
                "fps": (m["summary"] or {}).get("fps", {}),
                "scheduler": (m["summary"] or {}).get("scheduler")}
               for m in per_movie]
    roll = {"schema": ROLLUP_SCHEMA, "platform": platform,
            "movies": len(per_movie),
            "movie_set": sorted(m["movie"] for m in per_movie),
            "engaged_ticks": sum(r["engaged_ticks"] for r in pm_rows),
            "scopes": rows, "per_movie": pm_rows}
    roll.update(meta)
    return roll


def engaged_ticks(summary):
    """Canonical presents the summary counted (after --skip-ticks and the
    transient filter): the ticks the run measured."""
    return ((summary or {}).get("kinds", {}).get("canonical", {})
            .get("presents", 0))


def render_rollup_text(roll):
    L = []
    p = roll["platform"]
    banner = "SOFTWARE-GPU (CPU valid, GPU numbers invalid)" if p["software_gpu"] else "real-GPU"
    if roll["mode"] == "functional":
        banner = "FUNCTIONAL (headless, contention not refused: no timing verdict)"
    L.append("# openpete bench-corpus rollup  game=%s  platform=%s  arm=%s  %s"
             % (roll["game"], p["tag"], roll["build"]["arm"], banner))
    L.append("# movies=%d  engaged_ticks=%d  device=%s  commit=%s%s"
             % (roll["movies"], roll["engaged_ticks"], p["device"],
                (roll["build"]["commit"] or "?")[:12],
                " (dirty)" if roll["build"]["dirty"] else ""))
    L.append("[per-movie]            presents   ticks    fps_avg  exit")
    for pm in roll["per_movie"]:
        L.append("%-22s %-11d%-9d%-9s%s" % (pm["movie"][:22], pm["presents"],
                                           pm["engaged_ticks"],
                                           pm["fps"].get("avg", "?"), pm["exit_reason"]))
    sch = [(pm["movie"], pm.get("scheduler")) for pm in roll["per_movie"] if pm.get("scheduler")]
    if sch:
        L.append("[scheduler]            cost_ema  cost_dev  tail_us  pred/s  meas/s  level            afford")
        for movie, c in sch:
            top = sorted(c["afford_dist"].items(), key=lambda kv: -kv[1])[:3]
            L.append("%-22s %9.1f %9.1f %8.1f %7.1f %7.1f  %-16s %s"
                     % (movie[:22], c["cost_ema_p50_us"], c["cost_dev_p50_us"], c["tail_p50_us"],
                        c["predicted_presents_per_s"], c["measured_presents_per_s"],
                        " ".join("%s:%.0f%%" % kv for kv in c["level_dist"].items())[:16],
                        " ".join("%s:%.0f%%" % kv for kv in top)))
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


def comparability(base, roll):
    """Why two rollups do not compare, or None when they do: a rollup without
    the recorded identity, or a different platform tag (preset class or
    device), mode, game or arm. A differing arm is an A/B, which
    tests/harness/bench-ab.py prices."""
    for name, r in (("baseline", base), ("current", roll)):
        if r.get("schema", 1) < ROLLUP_SCHEMA:
            return ("%s rollup predates the recorded game/arm/commit or --no-config (schema %s); "
                    "re-run it" % (name, r.get("schema", 1)))
    checks = (("platform", base["platform"]["tag"], roll["platform"]["tag"]),
              ("mode", base["mode"], roll["mode"]),
              ("game", base["game"], roll["game"]),
              ("arm", base["build"]["arm"], roll["build"]["arm"]))
    for what, b, c in checks:
        if b != c:
            return "baseline %s %s != current %s" % (what, b, c)
    return None


def compare_baseline(roll, baseline_path, thr_pct, thr_us):
    with open(baseline_path) as f:
        base = json.load(f)
    why = comparability(base, roll)
    if why:
        print("[bench] %s - refusing the compare" % why, file=sys.stderr)
        return EXIT_SKIP
    if base["movie_set"] != roll["movie_set"]:
        print("[bench] note: movie sets differ (baseline %d, current %d movies)"
              % (len(base["movie_set"]), len(roll["movie_set"])), file=sys.stderr)
    if roll["mode"] == "functional":
        print("[bench] identity accepted (platform %s, game %s, arm %s); "
              "functional rollups carry no timing verdict"
              % (roll["platform"]["tag"], roll["game"], roll["build"]["arm"]),
              file=sys.stderr)
        return 0
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
    ap.add_argument("--game", default="spyro-1", choices=game_conf.GAMES,
                    help="disc, boot flags, bk2 corpus and default exe")
    ap.add_argument("--exe", default=None,
                    help="default: the executable running <game> in build/linux-release "
                         "(openpete at the tree root)")
    ap.add_argument("--allow-untuned", action="store_true",
                    help="skip the bench_guard tuned-binary refusal")
    ap.add_argument("--functional", action="store_true",
                    help="headless, contention reported not refused; no timing verdict")
    ap.add_argument("--movies", default=None, help="comma list of bk2 stems (default: all)")
    ap.add_argument("--frame-limit", type=int, default=20000)
    ap.add_argument("--render-fps", type=int, default=120)
    ap.add_argument("--max-runtime-seconds", type=int, default=600)
    ap.add_argument("--out-dir", default=DEFAULT_OUT,
                    help="persistent results root (default %(default)s); /tmp is refused")
    ap.add_argument("--fresh", action="store_true",
                    help="discard the run directory's finished movies instead of resuming")
    ap.add_argument("--skip-ticks", type=int, default=60)
    ap.add_argument("--gpu-timers", action="store_true")
    ap.add_argument("--baseline", default=None)
    ap.add_argument("--compare-only", default=None, metavar="ROLLUP",
                    help="compare this rollup against --baseline without running")
    ap.add_argument("--threshold-pct", type=float, default=10.0)
    ap.add_argument("--threshold-us", type=int, default=100)
    ap.add_argument("--keep-traces", action="store_true")
    ap.add_argument("--run-id", default="run")
    args = ap.parse_args()

    if args.compare_only:
        if not args.baseline:
            ap.error("--compare-only needs --baseline")
        with open(args.compare_only) as f:
            roll = json.load(f)
        return compare_baseline(roll, args.baseline, args.threshold_pct, args.threshold_us)

    game = game_conf.resolve(ROOT, args.game)
    exe = os.path.abspath(args.exe or game["default_exe"])
    if not os.path.exists(exe):
        print("[bench] exe not found: %s" % exe, file=sys.stderr)
        return EXIT_SKIP
    if not os.path.exists(game["disc"]):
        print("[bench] disc not found: %s" % game["disc"], file=sys.stderr)
        return EXIT_SKIP

    bench_guard.check(exe, allow_untuned=args.allow_untuned,
                      functional=args.functional)
    bench_guard.print_surface(exe)
    identity = bench_guard.build_identity(exe)
    if not identity or not identity["arm"]:
        print("[bench] REFUSED: no arm file %s - the tree was configured "
              "before the configure wrote it; reconfigure the tree"
              % (identity["arm_file"] if identity else "(no CMakeCache.txt above the exe)"),
              file=sys.stderr)
        return EXIT_REFUSED

    movies = sorted(glob.glob(os.path.join(game["bk2_dir"], "*.bk2")))
    if args.movies:
        want = set(args.movies.split(","))
        movies = [m for m in movies
                  if os.path.basename(m)[:-4] in want]
    if not movies:
        print("[bench] no movies matched", file=sys.stderr)
        return EXIT_SKIP

    out_root = os.path.realpath(os.path.abspath(args.out_dir))
    if out_root == "/tmp" or out_root.startswith("/tmp/"):
        print("[bench] REFUSED: --out-dir %s is under /tmp, which a restart empties; "
              "use a persistent directory (default %s)" % (out_root, DEFAULT_OUT),
              file=sys.stderr)
        return EXIT_REFUSED
    out = os.path.join(out_root, args.run_id)
    os.makedirs(out, exist_ok=True)
    # A stale rollup from an earlier run must never be read as this one's.
    for stale in ("rollup.json", "rollup.txt"):
        if os.path.exists(os.path.join(out, stale)):
            os.remove(os.path.join(out, stale))
    stop_file = os.path.join(out, "STOP")
    if os.path.exists(stop_file):
        os.remove(stop_file)

    fp = run_fingerprint(exe, identity, args)
    plan = []
    for mp in movies:
        stem = os.path.basename(mp)[:-4]
        done = os.path.join(out, stem + ".done.json")
        rec, why = load_done(done, fp)
        if why and not args.fresh:
            print("[bench] REFUSED: %s: %s; rerun with the run's original command, "
                  "another --run-id, or --fresh" % (done, why), file=sys.stderr)
            return EXIT_REFUSED
        if args.fresh and os.path.exists(done):
            os.remove(done)
            rec = None
        plan.append((mp, stem, done, rec))
    reused = sum(1 for p in plan if p[3])
    if reused:
        print("[bench] resuming %s: %d of %d movies finished" % (out, reused, len(plan)),
              file=sys.stderr)

    signal.signal(signal.SIGTERM, _on_stop)
    signal.signal(signal.SIGINT, _on_stop)
    per_movie = []
    platform = None
    try:
        for mp, stem, done, rec in plan:
            if rec:
                run, summ = rec["run"], None
                try:
                    with open(os.path.join(out, stem + ".json")) as f:
                        summ = json.load(f)
                except (OSError, ValueError):
                    pass
            else:
                if os.path.exists(stop_file):
                    raise Stopped("STOP file")
                opp = os.path.join(out, stem + ".opp")
                log = os.path.join(out, stem + ".log")
                for stale in (opp, log, os.path.join(out, stem + ".json"),
                              os.path.join(out, stem + ".txt")):
                    if os.path.exists(stale):
                        os.remove(stale)
                # Every leg re-reads the preconditions: a process started or
                # a display that slept since the last movie moves this one.
                bench_guard.check(exe, allow_untuned=args.allow_untuned,
                                  functional=args.functional)
                print("[bench] %s ..." % stem, file=sys.stderr, flush=True)
                run = run_movie(exe, game, mp, opp, args, log)
                summ = None
                if os.path.exists(opp):
                    summ = summarize(opp, os.path.join(out, stem + ".json"),
                                     os.path.join(out, stem + ".txt"), stem, args.skip_ticks)
                if not args.keep_traces and os.path.exists(opp):
                    os.remove(opp)
                write_atomic(done, {"movie": stem, "fingerprint": fp, "run": run})
                print("[bench]   exit=%s presents=%s ticks=%d" %
                      (run["exit_reason"], (summ or {}).get("presents_total", "?"),
                       engaged_ticks(summ)), file=sys.stderr)
            if platform is None:
                platform = platform_tag(identity, run["device"])
            per_movie.append({"movie": stem, "run": run, "summary": summ,
                              "args_sidecar": os.path.exists(mp + ".args")})
    except Stopped as e:
        print("[bench] STOPPED (%s): %d of %d movies finished in %s; rerun the same "
              "command to resume" % (e, len(per_movie), len(plan), out), file=sys.stderr)
        return EXIT_STOPPED

    meta = {"game": args.game, "mode": "functional" if args.functional else "timed",
            "run_args": game["run_args"], "build": identity}
    roll = build_rollup(per_movie, platform, meta)
    with open(os.path.join(out, "rollup.json"), "w") as f:
        json.dump(roll, f, indent=2)
    txt = render_rollup_text(roll)
    with open(os.path.join(out, "rollup.txt"), "w") as f:
        f.write(txt + "\n")
    print("\n" + txt)
    print("\n[bench] wrote %s" % os.path.join(out, "rollup.json"), file=sys.stderr)

    vacuous = [pm["movie"] for pm in roll["per_movie"] if pm["engaged_ticks"] == 0]
    if vacuous:
        print("[bench] VACUOUS: no engaged tick in %s" % ", ".join(vacuous),
              file=sys.stderr)
        return EXIT_VACUOUS
    if args.baseline:
        return compare_baseline(roll, args.baseline, args.threshold_pct, args.threshold_us)
    return 0


if __name__ == "__main__":
    sys.exit(main())
