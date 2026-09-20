#!/usr/bin/env python3
"""baseline-terrain-arena.py — per-level terrain-arena high-water baseline.

The retained terrain cache holds fifteen vertex arenas of fixed capacity. An
arena that runs out returns -1 from its chunk allocator and the builder claims
the sector back to the CPU walk, which is a degradation of the cache. Sizing
the arenas needs a demand figure per arena per level, and a cap that binds
censors that figure: `high` cannot exceed `cap`, so a row at the cap is a
clamp, not a demand. Every generating run therefore sets
OPENPETE_TC_INITIAL_CAP past any content this game holds.

Each level is measured at two poses:

  spawn    the level's own spawn camera, original LOD — what a player's first
           frames ask of the arenas.
  topdown  the whole-level-visible freecam pose with extended LOD — the
           worst case, every sector of the level in the frustum at once.

Modes:

  generate  run the sweep and write baselines/terrain-arena/<level>.tsv plus
            union.tsv (per-arena maximum over every level and pose).
  check     two legs. `regen` repeats the sweep at the raised cap into a temp
            dir and diffs it against the committed files, refusing any
            censored row (a high equal to its cap) and any non-zero afail.
            `stock` repeats it at the shipped caps and asserts afail is 0
            everywhere, which is the claim "no level at either pose exceeds a
            shipped cap".
  demo      the 40,000-VBL attract demo, which crosses several level loads.
            The cross-level accumulation row. It asserts that the arenas do
            not carry one level's geometry into the next: one replacement
            fulfilled per level load, and, per arena, the first high-water
            mark measured after each level edge below that arena's peak in
            the segment before it — the sawtooth. It also reports each
            arena's exit mark against the union of the visited levels' rows,
            which is a demand figure and not an assertion: the demo camera
            traverses a level while the baseline's two poses are static, so
            a traversal legitimately asks for more than either pose.

Exit: 0 pass, 1 fail, 64 skip (binary or disc missing).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "fences"))
import bench_guard  # noqa: F401  (import-time launcher preconditions)

# Unattended launcher: no pad is being held, and the buzzing shows up in no
# log, so it would never surface on its own. (scripts/fences/rumble_guard.py)
os.environ.setdefault("OPENPETE_VIBRATION", "0")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISC = os.path.join(ROOT, "games/spyro-1/spyro-1/build/SCUS-94228.cue")
EXE = os.path.join(ROOT, "build/linux-release/games/spyro-1/openpete-spyro1")
OUT_DIR = os.path.join(ROOT, "baselines/terrain-arena")
EXIT_SKIP = 64

# games/spyro-1/features/levelwarp.c k_levels — every warpable level, in that
# table's order, with the level id the game reports at a load.
LEVELS = [
    ("artisans", 10), ("stone_hill", 11), ("dark_hollow", 12),
    ("town_square", 13), ("toasty", 14), ("sunny_flight", 15),
    ("peace_keepers", 20), ("dry_canyon", 21), ("cliff_town", 22),
    ("ice_cavern", 23), ("doctor_shemp", 24), ("night_flight", 25),
    ("magic_crafters", 30), ("alpine_ridge", 31), ("high_caves", 32),
    ("wizard_peak", 33), ("blowhard", 34), ("crystal_flight", 35),
    ("beast_makers", 40), ("terrace_village", 41), ("misty_bog", 42),
    ("tree_tops", 43), ("metalhead", 44), ("wild_flight", 45),
    ("dream_weavers", 50), ("dark_passage", 51), ("lofty_castle", 52),
    ("haunted_towers", 53), ("jacques", 54), ("icy_flight", 55),
    ("gnorc_gnexus", 60), ("gnorc_cove", 61), ("twilight_harbor", 62),
    ("gnasty_gnorc", 63), ("gnastys_loot", 64),
]
LEVEL_BY_ID = {i: n for n, i in LEVELS}

# core/include/openpete_terrain_cache.h, the OPTC_ARENA_* enum order, which is
# the order the census prints.
ARENAS = [
    "OPAQUE", "ALPHA", "ADD", "SUB", "LP",
    "A_OPAQUE", "A_ALPHA", "A_ADD", "A_SUB", "A_LP",
    "N_OPAQUE", "N_ALPHA", "N_ADD", "N_SUB", "N_LP",
]

POSES = ("spawn", "topdown")

# Every arena's capacity for a generating run. The largest per-arena demand
# measured across the levels is OPAQUE at ~451,600 vertices, so twice that
# binds nowhere. Cost per process: 903,258 x (15 x 64 B + 5 x 32 B + 5 x 24 B)
# = 1.12 GB of address space per buffer generation, doubled for the one level
# edge each run crosses while the outgoing buffers drain. llvmpipe backs it
# with host memory, and only the written prefix is ever touched.
GEN_CAP = 903258

# The screenshot is what brings the native renderer up headless; the run must
# outlive it by a frame. The level's own geometry starts baking when the walk
# latches the new level id, which lands ~170 VBLs before this limit; the marks
# are converged there (artisans topdown reads byte-identical at 1,756 and at
# 3,000 VBLs).
SHOT_VBL = 1755
FRAME_LIMIT = 1756
DEMO_FRAME_LIMIT = 40000
DEMO_SHOT_VBL = 39990

HWM_RE = re.compile(
    r"\[ter-gpu f=\d+\] hwm=(\S+) afail=(\S+) svc=(\d+)/(\d+) draws-retired=(\d+) rebake=(\d+)")
# core/psx/dispatch: the overlay switch that names each level the run enters.
LEVEL_LOAD_RE = re.compile(r"active overlay: \S+ -> level_(\d+)")
POSE_RE = re.compile(r"freecam: topdown pose set to")


def die(msg):
    sys.stderr.write("baseline-terrain-arena: %s\n" % msg)
    sys.exit(1)


def child_env(cap):
    e = dict(os.environ)
    e["OPENPETE_TERRAIN_CACHE_FORCE"] = "1"   # llvmpipe is a software GPU
    e["OPENPETE_TERRAIN_GPU_DBG"] = "1"       # the hwm=/afail= census line
    e["OPENPETE_VIBRATION"] = "0"
    if cap:
        e["OPENPETE_TC_INITIAL_CAP"] = str(cap)
    else:
        e.pop("OPENPETE_TC_INITIAL_CAP", None)
    return e


def launch(work, level, pose, cap, shot_vbl, frame_limit, extra=None):
    """One headless run. Returns (Popen, log path, shot path)."""
    tag = "%s__%s" % (level or "demo", pose)
    log = os.path.join(work, tag + ".log")
    shot = os.path.join(work, tag + ".png")
    for f in (log, shot, shot.replace(".png", "-vk.png")):
        if os.path.exists(f):
            os.remove(f)
    cmd = [EXE, "--headless", "--no-config", "--disc-cache", "0",
           "--vibration", "0", "--renderer", "vk", "--screenshot-native-only",
           "--screenshot-on-frame", str(shot_vbl), "--screenshot-output", shot,
           "--frame-limit", str(frame_limit), "--no-card", "--mute", "1",
           "--disc", DISC]
    if level:
        cmd += ["--skip-to-level", level]
    if pose == "topdown":
        cmd += ["--lod", "extended", "--freecam-pose", "topdown"]
    else:
        cmd += ["--lod", "original"]
    cmd += extra or []
    lf = open(log, "wb")
    p = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT, env=child_env(cap))
    p._log_file = lf
    return p, log, shot


def read_run(log):
    """Parse the last census line of a run log.

    Returns a dict with high[], afail[], svc calls/fulfilled, draws_retired
    and the pose engagement count, or None when the run printed no census.
    """
    txt = open(log, "rb").read().decode("utf-8", "replace")
    census = list(HWM_RE.finditer(txt))
    last = census[-1] if census else None
    if not last:
        return None
    high_caps = last.group(1).split(",")
    afail = [int(x) for x in last.group(2).split(",")]
    if len(high_caps) != len(ARENAS) or len(afail) != len(ARENAS):
        return None
    high = [int(x.split("/")[0]) for x in high_caps]
    cap = [int(x.split("/")[1]) for x in high_caps]
    return {
        "high": high, "cap": cap, "afail": afail,
        "svc_calls": int(last.group(3)), "svc_fulfilled": int(last.group(4)),
        "draws_retired": int(last.group(5)),
        "rebake": int(last.group(6)),
        "pose_lines": len(POSE_RE.findall(txt)),
        "levels": sorted({int(x) for x in LEVEL_LOAD_RE.findall(txt)}),
        "edges": len(LEVEL_LOAD_RE.findall(txt)),
        "segments": census_segments(census),
    }


def census_segments(census):
    """Group the run's census lines into one segment per buffer generation.

    Every census line carries svc=<calls>/<fulfilled>, and `fulfilled` steps
    once per arena replacement, so consecutive lines sharing it are the lines
    measured against one set of buffers. Each segment reports its first
    high-water mark per arena and its peak per arena; a segment shorter than
    the census divider contributes no line and simply does not appear.
    """
    segs = []
    for m in census:
        gen = int(m.group(4))
        high = [int(x.split("/")[0]) for x in m.group(1).split(",")]
        if len(high) != len(ARENAS):
            continue
        if not segs or segs[-1]["gen"] != gen:
            segs.append({"gen": gen, "first": list(high), "peak": list(high), "lines": 1})
            continue
        s = segs[-1]
        s["lines"] += 1
        s["peak"] = [max(a, b) for a, b in zip(s["peak"], high)]
    return segs


def sweep(work, cap, levels):
    """Run every level at both poses, `jobs` at a time. Returns {level: {pose: row}}."""
    jobs = []
    for level in levels:
        for pose in POSES:
            jobs.append((level, pose))
    out = {}
    running = []
    i = 0
    while i < len(jobs) or running:
        while len(running) < ARGS.jobs and i < len(jobs):
            level, pose = jobs[i]
            i += 1
            p, log, _ = launch(work, level, pose, cap, SHOT_VBL, FRAME_LIMIT)
            running.append((p, log, level, pose))
        p, log, level, pose = running.pop(0)
        rc = p.wait()
        p._log_file.close()
        row = read_run(log)
        if row is None:
            die("%s %s: no census line in %s (exit %d)" % (level, pose, log, rc))
        row["exit"] = rc
        out.setdefault(level, {})[pose] = row
        sys.stderr.write("  %-16s %-8s exit=%d svc=%d/%d afail=%d\n" % (
            level, pose, rc, row["svc_calls"], row["svc_fulfilled"], sum(row["afail"])))
    return out


def write_level(path, level, rows):
    with open(path, "w") as f:
        f.write("# terrain-arena high-water baseline (scripts/baseline-terrain-arena.py)\n")
        f.write("# level\t%s\n" % level)
        for pose in POSES:
            r = rows[pose]
            f.write("# pose\t%s\texit=%d\tfreecam=%d\tsvc=%d/%d\tdraws_retired=%d\trebake=%d\n" % (
                pose, r["exit"], r["pose_lines"], r["svc_calls"],
                r["svc_fulfilled"], r["draws_retired"], r["rebake"]))
        f.write("pose\tarena\thigh\tafail\n")
        for pose in POSES:
            r = rows[pose]
            for a, name in enumerate(ARENAS):
                f.write("%s\t%s\t%d\t%d\n" % (pose, name, r["high"][a], r["afail"][a]))


def write_union(path, data):
    best = {name: (0, "", "") for name in ARENAS}
    for level in sorted(data):
        for pose in POSES:
            r = data[level][pose]
            for a, name in enumerate(ARENAS):
                if r["high"][a] > best[name][0]:
                    best[name] = (r["high"][a], level, pose)
    with open(path, "w") as f:
        f.write("# per-arena maximum over every level and pose\n")
        f.write("arena\thigh\tlevel\tpose\n")
        for name in ARENAS:
            h, level, pose = best[name]
            f.write("%s\t%d\t%s\t%s\n" % (name, h, level, pose))
    return best


def assert_clean(data, leg):
    """Censoring and claim-back refusals, over a whole sweep."""
    bad = []
    for level in sorted(data):
        for pose in POSES:
            r = data[level][pose]
            if r["exit"] != 0:
                bad.append("%s %s: exit %d" % (level, pose, r["exit"]))
            if pose == "topdown" and r["pose_lines"] == 0:
                bad.append("%s %s: the freecam pose never engaged" % (level, pose))
            if r["draws_retired"]:
                bad.append("%s %s: %d draws bound a retired buffer pair"
                           % (level, pose, r["draws_retired"]))
            for a, name in enumerate(ARENAS):
                if r["afail"][a]:
                    bad.append("%s %s %s: afail=%d (sectors claimed back for want of room)"
                               % (level, pose, name, r["afail"][a]))
                if leg == "regen" and r["high"][a] and r["high"][a] == r["cap"][a]:
                    bad.append("%s %s %s: high=%d equals cap — the figure is censored, "
                               "raise OPENPETE_TC_INITIAL_CAP" % (level, pose, name, r["high"][a]))
    return bad


def cmd_generate(levels):
    os.makedirs(OUT_DIR, exist_ok=True)
    for name in os.listdir(OUT_DIR):
        if name.endswith(".tsv"):
            os.remove(os.path.join(OUT_DIR, name))
    work = tempfile.mkdtemp(prefix="terarena-gen-")
    data = sweep(work, GEN_CAP, levels)
    for level in sorted(data):
        write_level(os.path.join(OUT_DIR, level + ".tsv"), level, data[level])
    write_union(os.path.join(OUT_DIR, "union.tsv"), data)
    missing = [l for l in levels if not os.path.exists(os.path.join(OUT_DIR, l + ".tsv"))]
    if missing or not os.path.exists(os.path.join(OUT_DIR, "union.tsv")):
        die("outputs missing after the run: %s" % (missing or "union.tsv"))
    bad = assert_clean(data, "regen")
    for b in bad:
        sys.stderr.write("  REFUSED %s\n" % b)
    print("wrote %d level files + union.tsv to %s%s"
          % (len(data), OUT_DIR, " (with %d refusals)" % len(bad) if bad else ""))
    shutil.rmtree(work, ignore_errors=True)
    return 1 if bad else 0


def cmd_check(levels):
    rc = 0
    work = tempfile.mkdtemp(prefix="terarena-chk-")
    try:
        sys.stderr.write("leg regen (OPENPETE_TC_INITIAL_CAP=%d)\n" % GEN_CAP)
        data = sweep(work, GEN_CAP, levels)
        bad = assert_clean(data, "regen")
        for level in sorted(data):
            got = os.path.join(work, level + ".regen.tsv")
            write_level(got, level, data[level])
            want = os.path.join(OUT_DIR, level + ".tsv")
            if not os.path.exists(want):
                bad.append("%s: no committed baseline" % level)
                continue
            d = diff_tsv(want, got)
            if d:
                bad.append("%s differs from the committed baseline:\n%s" % (level, d))
        got_u = os.path.join(work, "union.regen.tsv")
        write_union(got_u, data)
        d = diff_tsv(os.path.join(OUT_DIR, "union.tsv"), got_u)
        if d:
            bad.append("union.tsv differs:\n%s" % d)
        for b in bad:
            sys.stderr.write("  REGEN FAIL %s\n" % b)
        rc |= 1 if bad else 0

        # Leg `stock`: the shipped caps. This is the claim that no level at
        # either pose sends a sector back to the CPU walk for want of arena
        # room. It fails today on the topdown pose of the levels whose
        # poly-anim opaque demand exceeds OPTC_ARENA_A_OPAQUE's 65,536, and
        # passes once the arenas are sized per level from these baselines.
        sys.stderr.write("leg stock (shipped caps)\n")
        sdata = sweep(work, 0, levels)
        sbad = assert_clean(sdata, "stock")
        for b in sbad:
            sys.stderr.write("  STOCK FAIL %s\n" % b)
        rc |= 1 if sbad else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("terrain-arena baseline check: %s" % ("PASS" if rc == 0 else "FAIL"))
    return rc


def diff_tsv(want, got):
    a = [l for l in open(want).read().splitlines() if not l.startswith("#")]
    b = [l for l in open(got).read().splitlines() if not l.startswith("#")]
    if a == b:
        return ""
    out = []
    for i in range(max(len(a), len(b))):
        la = a[i] if i < len(a) else "<missing>"
        lb = b[i] if i < len(b) else "<missing>"
        if la != lb:
            out.append("    -%s\n    +%s" % (la, lb))
    return "\n".join(out[:20])


def read_union():
    path = os.path.join(OUT_DIR, "union.tsv")
    if not os.path.exists(path):
        die("no committed union.tsv — run `generate` first")
    per_level = {}
    for level, _id in LEVELS:
        lp = os.path.join(OUT_DIR, level + ".tsv")
        if not os.path.exists(lp):
            continue
        rows = {}
        for line in open(lp):
            if line.startswith("#") or line.startswith("pose\t"):
                continue
            pose, arena, high, afail = line.split("\t")
            rows[arena] = max(rows.get(arena, 0), int(high))
        per_level[level] = rows
    return per_level


def cmd_demo():
    work = tempfile.mkdtemp(prefix="terarena-demo-")
    try:
        per_level = read_union()
        p, log, _ = launch(work, None, "demo", GEN_CAP, DEMO_SHOT_VBL, DEMO_FRAME_LIMIT)
        rc = p.wait()
        p._log_file.close()
        row = read_run(log)
        if row is None:
            die("attract demo: no census line in %s (exit %d)" % (log, rc))
        visited = [LEVEL_BY_ID[i] for i in row["levels"] if i in LEVEL_BY_ID]
        print("attract demo: exit=%d svc=%d/%d edges=%d draws-retired=%d rebake=%d levels=%s"
              % (rc, row["svc_calls"], row["svc_fulfilled"], row["edges"],
                 row["draws_retired"], row["rebake"], ",".join(visited) or "(none)"))

        bad = []
        if rc != 0:
            bad.append("exit %d" % rc)
        if row["edges"] == 0:
            bad.append("the run log named no level load — every assertion below is vacuous")
        # One arena replacement per level edge. A shortfall is a level whose
        # geometry was baked into the outgoing buffers.
        if row["svc_fulfilled"] != row["edges"]:
            bad.append("%d replacements fulfilled for %d level edges"
                       % (row["svc_fulfilled"], row["edges"]))
        if row["draws_retired"]:
            bad.append("%d draws bound a retired buffer pair" % row["draws_retired"])
        for a, name in enumerate(ARENAS):
            if row["afail"][a]:
                bad.append("%s: afail=%d (sectors claimed back for want of room)"
                           % (name, row["afail"][a]))

        # The sawtooth. Each segment is one set of buffers; the mark measured
        # first after an edge must sit below the previous segment's peak, or
        # that segment's geometry survived the edge. Arenas the previous
        # segment never touched carry no claim.
        segs = row["segments"]
        print("  segments (one per buffer generation): %s"
              % ", ".join("gen=%d lines=%d" % (s["gen"], s["lines"]) for s in segs))
        if len(segs) < 2:
            bad.append("only %d census segment(s) — the sawtooth is unmeasured" % len(segs))
        for i in range(1, len(segs)):
            prev, cur = segs[i - 1], segs[i]
            for a, name in enumerate(ARENAS):
                if prev["peak"][a] and cur["first"][a] >= prev["peak"][a]:
                    bad.append("%s: first mark after the edge into gen %d is %d, not below "
                               "gen %d's peak of %d — the outgoing level's geometry survived"
                               % (name, cur["gen"], cur["first"][a], prev["gen"],
                                  prev["peak"][a]))

        # Demand, reported and not asserted (see the module docstring).
        for a, name in enumerate(ARENAS):
            bound = max([per_level[l].get(name, 0) for l in visited if l in per_level] or [0])
            print("  demand: %-9s exit hwm=%-8d vs row %-8d afail=%d"
                  % (name, row["high"][a], bound, row["afail"][a]))
        for b in bad:
            sys.stderr.write("  DEMO FAIL %s\n" % b)
        print("terrain-arena demo row: %s" % ("PASS" if not bad else "FAIL"))
        return 1 if bad else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=("generate", "check", "demo"))
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--levels", default="")
    ARGS = ap.parse_args()
    if not os.path.exists(EXE) or not os.path.exists(DISC):
        sys.stderr.write("baseline-terrain-arena: SKIP (no binary or no disc)\n")
        return EXIT_SKIP
    levels = ([l for l in ARGS.levels.split(",") if l] or [n for n, _ in LEVELS])
    if ARGS.mode == "generate":
        return cmd_generate(levels)
    if ARGS.mode == "check":
        return cmd_check(levels)
    return cmd_demo()


if __name__ == "__main__":
    sys.exit(main())
