#!/usr/bin/env python3
"""baseline-terrain-arena.py - per-level terrain-arena high-water baseline.

The retained terrain cache holds fifteen vertex arenas and eight cell-record
arenas (CN_*, CA_*: counted in records) whose capacities are
generated from this baseline (scripts/gen-terrain-arena-caps.py). An arena
that runs out returns -1 from its chunk allocator, the builder claims the
sector back to the CPU walk, and the cache asks the device thread for an
overflow buffer; the claim lasts until that buffer lands. Sizing the arenas so
the claim does not happen at all needs a demand figure per arena per level, and
a cap that binds censors that figure: `high` cannot exceed `cap`, so a row at
the cap is a clamp, not a demand. Every generating run therefore sets
OPENPETE_TC_INITIAL_CAP past any content this game holds, and
OPENPETE_TERRAIN_ANIMNEAR=1: that lever bakes poly-anim near sectors into
the near arenas instead of claiming them back, so the lever-on bake is the
larger demand and the caps cover both settings.

--game picks the game (default spyro-1). Its disc and boot flags come from
games/<game>/oracle.conf, its levels from its k_levels table, its movies and
corpus manifests from games/<game>/ (scripts/lib/game_conf.py,
scripts/lib/corpus_manifest.py), and its rows go to
baselines/terrain-arena/<game>/.

Five row kinds per level, written as `pose` rows of
baselines/terrain-arena/<game>/<level>.tsv:

  spawn    the level's own spawn camera, original LOD - what a player's first
           frames ask of the arenas.
  topdown  the whole-level-visible freecam pose with extended LOD - the
           static worst case, every sector of the level in the frustum.
  bk2      the peak over every presentation-golden row that plays in this
           level: one replay per distinct movie, and one run per distinct
           launch of a row with no movie (a cutscene or level skip). A
           traversal exceeds both static poses: a camera that moves carries
           sectors it has left beside the ones it has reached.
  demo     the attract demo's peak in this level, for the levels it visits.
  freecam  the peak over the freecam-corpus poses in this level (sub-area
           rows included), at each row's canonical VBL with extended LOD: a
           camera close to the ground routes sectors through the near arenas,
           which neither static pose does.

A traversal row is cut per level, not per run: the run log's census lines are
grouped by (level id, buffer generation), so a movie that crosses a level load
contributes to both levels and a level's own marks never carry the outgoing
level's geometry. The level id is the one the cache looks its caps up by,
from the walk's `[ter-gpu] level-edge` line: a title flyby or a cutscene is
attributed to the level id the game reports while it plays, since that id's
caps are the ones its arenas are allocated at.

Modes:

  generate    run the static sweep and write the spawn/topdown rows.
  bk2-corpus  replay the presentation-golden movie corpus and write the bk2
              rows.
  freecam     run the freecam corpus and write the freecam rows.
  demo        the 40,000-VBL attract demo: the cross-level accumulation
              assertions. The demo rows go to a scratch copy of the committed
              tree, deleted on exit; `demo --write` writes them into
              baselines/terrain-arena/.
  check       three legs. `regen` repeats all three generating runs at the
              raised cap and diffs the result against the committed files,
              refusing any censored row (a high equal to its cap) and any
              non-zero afail. `stock` repeats the static sweep at the shipped
              caps and asserts afail is 0 everywhere, which is the claim "no
              level at either pose exceeds a generated cap". `overflow` proves
              the overflow buffer: one level at a cap that cannot hold it,
              against the same level at the generated caps.

Every generating mode rewrites only the rows it owns and then recomputes
union.tsv from the files, so the four row kinds accumulate across modes.
`check` and `demo` without --write leave the committed files untouched.

--work DIR keeps every run's log in DIR (default: a temporary directory
deleted on exit) and reuses a log whose run reached its frame limit under the
same binary, disc and environment, so a sweep stopped part way resumes where
it stopped. A stamp file in DIR names those inputs; a mismatch empties DIR.

Exit: 0 pass, 1 fail or a missing corpus manifest (check and bk2-corpus; the
message names the path), 64 skip (binary or disc missing).
OPENPETE_MANIFEST_DRY_RUN=1 loads and reports the manifest, then exits 0.
"""
import argparse
import filecmp
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "scripts", "fences"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "scripts", "lib"))
import bench_guard  # noqa: F401  (import-time launcher preconditions)
import corpus_manifest
import game_conf

# Unattended launcher: no player uses the pad, and the buzzing shows up in no
# log, so it would never surface on its own. (scripts/fences/rumble_guard.py)
os.environ.setdefault("OPENPETE_VIBRATION", "0")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASE_DIR = os.path.join(ROOT, "baselines/terrain-arena")
EXIT_SKIP = 64

# Set by set_game from --game: the game's disc, boot flags, executable argv,
# movie directory, corpus manifests, output directory and level table
# [(slug, level id)] in its k_levels order.
GAME = None
DISC = None
RUN_ARGS = []
EXE = None
EXE_ARGV = []
OUT_DIR = None
BK2_DIR = None
MANIFEST = None
FREECAM_MANIFEST = None
LEVELS = []
LEVEL_BY_ID = {}

# The feed-key value the static poses are read at. Spyro 1 keys on its
# frame (a yield count); Spyro 2 and 3 key on the VBL, and the same present
# is 573 fast-boot VBLs later (scripts/presentation-game.sh
# pg_cap_settings, the freecam start 1900 against 2473).
SHOT_BY_GAME = {"spyro-1": 1755, "spyro-2": 2328, "spyro-3": 2328}
# A freecam-corpus row's canonical VBL when the row names none (the same
# pg_cap_settings default).
FREECAM_START_BY_GAME = {"spyro-1": 1900, "spyro-2": 2473, "spyro-3": 2473}


def set_game(game, build="build/linux-release"):
    """Point every per-game global at `game`."""
    global GAME, DISC, RUN_ARGS, EXE, EXE_ARGV, OUT_DIR, BK2_DIR, MANIFEST
    global FREECAM_MANIFEST, LEVELS, LEVEL_BY_ID, SHOT_VBL, FRAME_LIMIT
    g = game_conf.resolve(ROOT, game, build)
    GAME = game
    DISC = g["disc"]
    RUN_ARGS = list(g["run_args"])
    EXE_ARGV = game_conf.tree_argv(os.path.join(ROOT, build), game)
    EXE = EXE_ARGV[0]
    BK2_DIR = g["bk2_dir"]
    OUT_DIR = os.path.join(BASE_DIR, game)
    MANIFEST = corpus_manifest.corpus_manifest(ROOT, game, "presentation-corpus.manifest")
    FREECAM_MANIFEST = corpus_manifest.corpus_manifest(ROOT, game, "freecam-corpus.manifest")
    LEVELS = game_conf.level_table(ROOT, game)
    LEVEL_BY_ID = {i: n for n, i in LEVELS}
    SHOT_VBL = SHOT_BY_GAME[game]
    FRAME_LIMIT = SHOT_VBL + 1

# core/include/openpete_terrain_cache.h, the OPTC_ARENA_* enum order, which is
# the order the census prints.
ARENAS = [
    "OPAQUE", "ALPHA", "ADD", "SUB", "LP",
    "A_OPAQUE", "A_ALPHA", "A_ADD", "A_SUB", "A_LP",
    "N_OPAQUE", "N_ALPHA", "N_ADD", "N_SUB", "N_LP",
    "CN_OPAQUE", "CN_ALPHA", "CN_ADD", "CN_SUB",
    "CA_OPAQUE", "CA_ALPHA", "CA_ADD", "CA_SUB",
]

POSES = ("spawn", "topdown")
ROW_KINDS = ("spawn", "topdown", "bk2", "demo", "freecam")
# The arenas the sizing turns on; the rest sit at their floor (16,384
# vertices, or 256 records on a cell arena).
REPORT_ARENAS = ("OPAQUE", "A_OPAQUE", "N_OPAQUE", "N_LP", "CN_OPAQUE")

# Every arena's capacity for a generating run. The largest per-arena demand
# measured across the levels is OPAQUE at ~451,600 vertices, so twice that
# binds nowhere. The cell-record arenas take a sixteenth of it in records
# (OPENPETE_TC_INITIAL_CAP's rule, core/renderer/native_terrain_cache.c).
# Cost per process: 903,258 x (15 x 64 B + 5 x 32 B + 5 x 24 B) plus
# 56,453 x (4 x 176 B + 4 x 200 B) = 1.21 GB of address space per buffer
# generation, doubled for the one level edge each run crosses while the
# outgoing buffers drain. llvmpipe backs it with host memory, and only the
# written prefix is ever touched.
GEN_CAP = 903258

# The screenshot is what brings the native renderer up headless; the run must
# outlive it by a frame. The level's own geometry starts baking when the walk
# latches the new level id, which lands ~170 VBLs before this limit; the marks
# are converged there (artisans topdown reads byte-identical at 1,756 and at
# 3,000 VBLs).
SHOT_VBL = 1755      # set_game: SHOT_BY_GAME
FRAME_LIMIT = 1756   # set_game: SHOT_VBL + 1
DEMO_FRAME_LIMIT = 40000
DEMO_SHOT_VBL = 39990
# A movie replay runs to ten VBLs past its last input or its last pinned
# capture, whichever is later. The ceiling bounds the sweep's runtime against a
# corpus that grows; no committed movie reaches it.
BK2_FRAME_CEIL = 12000
# The overflow leg's cap: below every level's smallest real demand, so the
# primary buffer of the busy arenas fills within the first walks.
OVERFLOW_CAP = 16384
OVERFLOW_LEVEL_BY_GAME = {"spyro-1": "metalhead", "spyro-2": "summer_forest",
                          "spyro-3": "sunrise_spring"}
# The screenshot VBL is the static baseline's; the run outlives it, because the
# level edge that precedes it resets the arenas and the claim-back it causes
# takes a few tens of VBLs to settle. Asserting that afail has stopped rising
# needs the run to reach the far side of that transient.
OVERFLOW_TAIL = 645   # OVERFLOW_FRAME_LIMIT past SHOT_VBL on Spyro 1 (1755 -> 2400)

HWM_RE = re.compile(
    r"\[ter-gpu f=(?:\d+)\] hwm=(\S+) afail=(\S+) ovf=(\S+) caps=(\S+)MiB "
    r"svc=(\d+)/(\d+) draws-retired=(\d+) rebake=(\d+)")
# core/psx/dispatch: the overlay switch that names each level the run enters.
# games/common/renderer/terrain.c tgpu_level_edge: the level id the walk
# latched, which keys the arena caps; prev -1 is the run's first latch (no
# replacement), every other line one replacement.
LEVEL_EDGE_RE = re.compile(r"\[ter-gpu f=\d+\] level-edge (-?\d+) -> (-?\d+)")
POSE_RE = re.compile(r"freecam: topdown pose set to")


def die(msg):
    sys.stderr.write("baseline-terrain-arena: %s\n" % msg)
    sys.exit(1)


KEPT_LOG_DIR = os.path.join(ROOT, "build", "terrain-arena-failed-runs")


def keep_log(log, tag):
    """Copy a failed run's log out of the work directory, which the harness
    deletes on exit, and return the kept path. The engine's crash report
    (fault address, backtrace) is in that log and nowhere else."""
    os.makedirs(KEPT_LOG_DIR, exist_ok=True)
    kept = os.path.join(KEPT_LOG_DIR, "%s-%d.log" % (tag, int(time.time())))
    try:
        shutil.copyfile(log, kept)
    except OSError as e:
        return "%s (not kept: %s)" % (log, e)
    return kept


# ---- launching -------------------------------------------------------------

def child_env(cap, env=()):
    e = dict(os.environ)
    e["OPENPETE_TERRAIN_CACHE_FORCE"] = "1"   # llvmpipe is a software GPU
    e["OPENPETE_TERRAIN_GPU_DBG"] = "1"       # the hwm=/afail= census line
    e["OPENPETE_TERRAIN_ANIMNEAR"] = "1"      # the larger bake (module docstring)
    e["OPENPETE_VIBRATION"] = "0"
    if cap:
        e["OPENPETE_TC_INITIAL_CAP"] = str(cap)
    else:
        e.pop("OPENPETE_TC_INITIAL_CAP", None)
    for kv in env:
        k, v = kv.split("=", 1)
        e[k] = v
    return e


RUN_FRAME_LIMIT = {}   # log path -> the --frame-limit its run was launched with
WORK_KEEP = None       # --work: the directory whose logs a resumed sweep reuses


def run_complete(log, frame_limit):
    """True when `log` holds its run's exit census line (f >= frame_limit - 1)."""
    if not os.path.exists(log):
        return False
    txt = open(log, "rb").read().decode("utf-8", "replace")
    frames = FRAME_RE.findall(txt)
    return bool(frames) and int(frames[-1]) >= frame_limit - 1


def work_dir(prefix):
    """The directory a mode's runs write to: --work when given (kept, and
    emptied when its stamp names other inputs), else a fresh temporary one."""
    if WORK_KEEP is None:
        return tempfile.mkdtemp(prefix=prefix)
    d = os.path.join(WORK_KEEP, GAME, prefix.rstrip("-"))
    os.makedirs(d, exist_ok=True)
    st = os.stat(EXE)
    stamp = json.dumps({"exe": EXE, "exe_mtime": st.st_mtime_ns, "exe_size": st.st_size,
                        "disc": DISC, "run_args": RUN_ARGS,
                        "env": sorted((k, v) for k, v in os.environ.items()
                                      if k.startswith("OPENPETE_"))}, sort_keys=True)
    sp = os.path.join(d, ".stamp")
    if not os.path.exists(sp) or open(sp).read() != stamp:
        shutil.rmtree(d)
        os.makedirs(d)
        with open(sp, "w") as f:
            f.write(stamp)
    return d


def drop_work(work):
    """Delete a mode's temporary directory; a --work directory is kept."""
    if WORK_KEEP is None:
        shutil.rmtree(work, ignore_errors=True)


class _Done:
    """A run --work already holds: wait() returns its recorded exit status."""
    def __init__(self, rc):
        self._log_file = open(os.devnull, "wb")
        self._rc = rc

    def wait(self):
        return self._rc


EXIT_RE = re.compile(r"openpete-exit: reason=\S+ .*code=(-?\d+)")


def recorded_rc(log):
    """The exit status run_batch recorded beside `log`, else the code of the
    run's own `openpete-exit:` line (a run whose harness was stopped while
    it ran), or None."""
    try:
        return int(open(log + ".rc").read())
    except (OSError, ValueError):
        pass
    m = EXIT_RE.findall(open(log, "rb").read().decode("utf-8", "replace"))
    return int(m[-1]) if m else None


def launch(work, tag, cap, shot_vbl, frame_limit, extra, env=()):
    """One headless run. Returns (Popen, log path, shot path).

    `extra` is the run's own arguments; a `{legdir}` in one names a per-run
    directory under `work`, and a run given --card1 drops the game's --no-card
    boot flag. Under --work a run whose log already reached its frame limit
    is not started again."""
    log = os.path.join(work, tag + ".log")
    RUN_FRAME_LIMIT[log] = frame_limit
    shot = os.path.join(work, tag + ".png")
    vkshot = shot.replace(".png", "-vk.png")
    if WORK_KEEP is not None and run_complete(log, frame_limit) \
            and recorded_rc(log) is not None:
        return _Done(recorded_rc(log)), log, vkshot
    for f in (log, log + ".rc", shot, vkshot):
        if os.path.exists(f):
            os.remove(f)
    legdir = os.path.join(work, tag + ".leg")
    shutil.rmtree(legdir, ignore_errors=True)
    if any("{legdir}" in x for x in extra):
        os.makedirs(legdir)
        extra = [x.replace("{legdir}", legdir) for x in extra]
    run_args = [a for a in RUN_ARGS if not ("--card1" in extra and a == "--no-card")]
    card = [] if "--card1" in extra or "--no-card" in run_args else ["--no-card"]
    cmd = EXE_ARGV + ["--headless", "--no-config", "--disc-cache", "0",
                      "--vibration", "0", "--renderer", "vk", "--screenshot-native-only",
                      "--screenshot-on-frame", str(shot_vbl), "--screenshot-output", shot,
                      "--frame-limit", str(frame_limit), "--mute", "1",
                      "--disc", DISC] + card + run_args + list(extra)
    lf = open(log, "wb")
    p = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT, env=child_env(cap, env))
    p._log_file = lf
    return p, log, vkshot


def static_args(level, pose):
    a = ["--skip-to-level", level]
    if pose == "topdown":
        a += ["--lod", "extended", "--freecam-pose", "topdown"]
    else:
        a += ["--lod", "original"]
    return a


def run_batch(work, jobs):
    """Run a list of (tag, cap, shot, limit, extra[, env]) specs `jobs` at a time.

    Yields (tag, rc, log, shot) in completion-queue order.
    """
    def _run(specs, njobs):
        running, i = [], 0
        while i < len(specs) or running:
            while len(running) < njobs and i < len(specs):
                tag, cap, shot, limit, extra = specs[i][:5]
                env = specs[i][5] if len(specs[i]) > 5 else ()
                i += 1
                p, log, png = launch(work, tag, cap, shot, limit, extra, env)
                running.append((p, tag, log, png))
            p, tag, log, png = running.pop(0)
            rc = p.wait()
            p._log_file.close()
            with open(log + ".rc", "w") as f:
                f.write("%d\n" % rc)
            yield tag, rc, log, png
    return _run


# ---- census parsing --------------------------------------------------------

def census_lines(log):
    """Every census line of a run, in order, each with the level current at it.

    A line carries the cumulative state of one buffer generation; `gen` is the
    replacement count, which steps once per level edge, so (level, gen) names
    one level's residency in the arenas.
    """
    txt = open(log, "rb").read().decode("utf-8", "replace")
    out, cur = [], -1
    for line in txt.splitlines():
        m = LEVEL_EDGE_RE.search(line)
        if m:
            cur = int(m.group(2))
            continue
        m = HWM_RE.search(line)
        if not m:
            continue
        high_caps = m.group(1).split(",")
        afail = m.group(2).split(",")
        ovf = m.group(3).split(",")
        if len(high_caps) != len(ARENAS) or len(afail) != len(ARENAS):
            continue
        out.append({
            "level": cur,
            "gen": int(m.group(6)),
            "high": [int(x.split("/")[0]) for x in high_caps],
            "cap": [int(x.split("/")[1]) for x in high_caps],
            "afail": [int(x) for x in afail],
            "ovf": [int(x) for x in ovf],
            "caps_mib": float(m.group(4)),
            "svc_calls": int(m.group(5)),
            "svc_fulfilled": int(m.group(6)),
            "draws_retired": int(m.group(7)),
            "rebake": int(m.group(8)),
        })
    return out, txt


FRAME_RE = re.compile(r"\[ter-gpu f=(\d+)\] hwm=")


def read_run(log, frame_limit=None):
    """The last census line of a run plus the run-wide counts, or None.

    With frame_limit, a run whose last census line is not the exit line
    (printed at the frame limit) is refused: its log lost the tail, and the
    last line it holds is an earlier, lower sample.
    """
    lines, txt = census_lines(log)
    if not lines:
        return None
    if frame_limit is None:
        frame_limit = RUN_FRAME_LIMIT.get(log)
    if frame_limit is not None:
        frames = FRAME_RE.findall(txt)
        if not frames or int(frames[-1]) < frame_limit - 1:
            die("%s: last census line at f=%s, not the exit line at f=%d: "
                "the log lost its tail" % (log, frames[-1] if frames else "none",
                                           frame_limit))
    last = dict(lines[-1])
    last["pose_lines"] = len(POSE_RE.findall(txt))
    latches = LEVEL_EDGE_RE.findall(txt)
    last["levels"] = sorted({int(b) for _a, b in latches})
    last["edges"] = sum(1 for a, _b in latches if int(a) >= 0)
    last["lines"] = lines
    last["segments"] = census_segments(lines)
    return last


def census_segments(lines):
    """Group the census lines into one segment per buffer generation.

    Each segment reports its first high-water mark per arena and its peak per
    arena; a segment shorter than the census divider contributes no line and
    simply does not appear.
    """
    segs = []
    for c in lines:
        if not segs or segs[-1]["gen"] != c["gen"]:
            segs.append({"gen": c["gen"], "level": c["level"], "first": list(c["high"]),
                         "peak": list(c["high"]), "lines": 1})
            continue
        s = segs[-1]
        s["lines"] += 1
        s["peak"] = [max(a, b) for a, b in zip(s["peak"], c["high"])]
    return segs


def peaks_by_level(log):
    """level id -> per-arena peak over that level's own census lines.

    Lines before the walk's first latch carry level -1 and are dropped; a run
    that crosses a level edge contributes to each level it played, against
    that level's own buffer generation.
    """
    lines, _ = census_lines(log)
    out = {}
    for c in lines:
        if c["level"] < 0:
            continue
        p = out.setdefault(c["level"], [0] * len(ARENAS))
        for a in range(len(ARENAS)):
            p[a] = max(p[a], c["high"][a])
    return out


# ---- baseline files --------------------------------------------------------

def read_level(path):
    """(pose -> meta string, pose -> {arena: (high, afail)})."""
    meta, rows = {}, {}
    if not os.path.exists(path):
        return meta, rows
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("# pose\t"):
            f = line.split("\t")
            meta[f[1]] = "\t".join(f[2:])
            continue
        if line.startswith("#") or line.startswith("pose\t") or not line:
            continue
        f = line.split("\t")
        rows.setdefault(f[0], {})[f[1]] = (int(f[2]), int(f[3]))
    return meta, rows


def write_level(path, level, meta, rows):
    with open(path, "w") as f:
        f.write("# terrain-arena high-water baseline (tests/harness/baseline-terrain-arena.py)\n")
        f.write("# level\t%s\n" % level)
        for pose in ROW_KINDS:
            if pose in rows:
                f.write("# pose\t%s\t%s\n" % (pose, meta.get(pose, "")))
        f.write("pose\tarena\thigh\tafail\n")
        for pose in ROW_KINDS:
            if pose not in rows:
                continue
            for name in ARENAS:
                h, af = rows[pose].get(name, (0, 0))
                f.write("%s\t%s\t%d\t%d\n" % (pose, name, h, af))


def merge_level(out_dir, level, new_rows, new_meta):
    """Replace only the row kinds this run owns; keep the others."""
    path = os.path.join(out_dir, level + ".tsv")
    meta, rows = read_level(path)
    for pose in new_rows:
        rows[pose] = new_rows[pose]
        meta[pose] = new_meta.get(pose, "")
    write_level(path, level, meta, rows)
    return path


def rebuild_union(out_dir):
    best = {name: (0, "", "") for name in ARENAS}
    for level, _id in LEVELS:
        path = os.path.join(out_dir, level + ".tsv")
        if not os.path.exists(path):
            continue
        _, rows = read_level(path)
        for pose in rows:
            for name in ARENAS:
                h = rows[pose].get(name, (0, 0))[0]
                if h > best[name][0]:
                    best[name] = (h, level, pose)
    path = os.path.join(out_dir, "union.tsv")
    with open(path, "w") as f:
        f.write("# per-arena maximum over every level and row kind\n")
        f.write("arena\thigh\tlevel\tpose\n")
        for name in ARENAS:
            h, level, pose = best[name]
            f.write("%s\t%d\t%s\t%s\n" % (name, h, level, pose))
    return best


def row_dict(high, afail):
    return {name: (high[a], afail[a]) for a, name in enumerate(ARENAS)}


# ---- static sweep ----------------------------------------------------------

def sweep(work, cap, levels, jobs):
    """Every level at both poses. Returns {level: {pose: row}}."""
    specs = [("%s__%s" % (l, p), cap, SHOT_VBL, FRAME_LIMIT, static_args(l, p))
             for l in levels for p in POSES]
    out = {}
    for tag, rc, log, _png in run_batch(work, jobs)(specs, jobs):
        level, pose = tag.split("__")
        row = read_run(log)
        if row is None:
            die("%s %s: no census line in %s (exit %d)" % (level, pose, keep_log(log, tag), rc))
        row["exit"] = rc
        out.setdefault(level, {})[pose] = row
        sys.stderr.write("  %-16s %-8s exit=%d svc=%d/%d afail=%d caps=%.1fMiB\n" % (
            level, pose, rc, row["svc_calls"], row["svc_fulfilled"],
            sum(row["afail"]), row["caps_mib"]))
    return out


def assert_clean(data, leg):
    """Censoring and claim-back refusals, over a whole static sweep."""
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
                    bad.append("%s %s %s: high=%d equals cap - the figure is censored, "
                               "raise OPENPETE_TC_INITIAL_CAP" % (level, pose, name, r["high"][a]))
    return bad


def write_static(out_dir, data):
    for level in sorted(data):
        rows, meta = {}, {}
        for pose in POSES:
            r = data[level][pose]
            rows[pose] = row_dict(r["high"], r["afail"])
            meta[pose] = "exit=%d\tfreecam=%d\tsvc=%d/%d\tdraws_retired=%d\trebake=%d" % (
                r["exit"], r["pose_lines"], r["svc_calls"], r["svc_fulfilled"],
                r["draws_retired"], r["rebake"])
        merge_level(out_dir, level, rows, meta)
    rebuild_union(out_dir)


def cmd_generate(levels, jobs):
    os.makedirs(OUT_DIR, exist_ok=True)
    work = work_dir("terarena-gen-")
    data = sweep(work, GEN_CAP, levels, jobs)
    write_static(OUT_DIR, data)
    missing = [l for l in levels if not os.path.exists(os.path.join(OUT_DIR, l + ".tsv"))]
    if missing or not os.path.exists(os.path.join(OUT_DIR, "union.tsv")):
        die("outputs missing after the run: %s" % (missing or "union.tsv"))
    bad = assert_clean(data, "regen")
    for b in bad:
        sys.stderr.write("  REFUSED %s\n" % b)
    print("wrote the spawn/topdown rows of %d levels + union.tsv to %s%s"
          % (len(data), OUT_DIR, " (with %d refusals)" % len(bad) if bad else ""))
    drop_work(work)
    return 1 if bad else 0


# ---- bk2 corpus ------------------------------------------------------------

_MANIFEST_ROWS = None


def manifest_rows():
    """The corpus manifest's rows, loaded once; a missing manifest exits 1."""
    global _MANIFEST_ROWS
    if _MANIFEST_ROWS is None:
        _MANIFEST_ROWS = corpus_manifest.load(ROOT, MANIFEST, "pipe")
    return _MANIFEST_ROWS


def manifest_runs():
    """The runs that cover the presentation-golden rows: [(tag, bk2 path or
    None, launch args, launch env, last VBL a row of the run pins)].

    A row is `<name>[@<variant>] | start | interval | freeze | pins | launch`.
    Rows of one movie share one replay, launched with the plain row's launch
    field (else the first row's): a variant changes vertex precision, a cache
    force or a capture setting, none of which moves a vertex into an arena.
    A row with no movie file runs its launch field (a cutscene or a level
    skip), one run per distinct launch; a row with neither is the title and
    attract demo, which the demo mode covers. The movie's own .bk2.args
    sidecar (its level, its LOD mode) is spliced by the binary.
    """
    groups = {}
    for row in manifest_rows():
        f = [x.strip() for x in row.split("|")]
        name = f[0]
        base = name.split("@")[0]
        pins = [int(x) for x in (f[4].split(",") if len(f) > 4 else []) if x.strip().isdigit()]
        if len(f) > 1 and f[1].isdigit():
            pins.append(int(f[1]))
        args, env = [], []
        for tok in (f[5].split() if len(f) > 5 else []):
            if tok.startswith("docs:") or tok.startswith("PRESENTATION_LEG_TIMEOUT="):
                continue
            (env if re.match(r"^[A-Z_][A-Z0-9_]*=", tok) else args).append(tok)
        bk2 = os.path.join(BK2_DIR, base + ".bk2")
        if os.path.exists(bk2):
            key = ("bk2", base)
        elif args:
            key = ("launch", " ".join(args + env))
        else:
            continue
        g = groups.get(key)
        if g is None or ("@" in g["name"] and "@" not in name):
            g = {"name": name, "args": args, "env": env,
                 "pins": (g["pins"] if g else [])}
            groups[key] = g
        g["pins"].extend(pins)
    out, seen = [], set()
    for (kind, k), g in sorted(groups.items()):
        if kind == "bk2":
            tag, bk2 = k, os.path.join(BK2_DIR, k + ".bk2")
        else:
            tag, bk2 = g["name"].split("@")[0], None
            n = 2
            while tag in seen:
                tag = "%s-%d" % (g["name"].split("@")[0], n)
                n += 1
        seen.add(tag)
        out.append((tag, bk2, g["args"], g["env"], max(g["pins"] or [0])))
    return out


def bk2_frames(path):
    """Movie length = the '|'-prefixed input lines of its Input Log."""
    try:
        z = zipfile.ZipFile(path)
        return sum(1 for l in z.read("Input Log.txt").decode("latin-1").splitlines()
                   if l.startswith("|"))
    except Exception:
        return 0


def cmd_bk2corpus(jobs, out_dir=None, quiet=False):
    """Replay every corpus movie and write the per-level `bk2` rows."""
    out_dir = out_dir or OUT_DIR
    movies = manifest_runs()
    if not movies:
        die("the corpus manifest named no playable movie or launch")
    work = work_dir("terarena-bk2-")
    try:
        specs = []
        for tag, path, args, env, last_pin in movies:
            n = 0
            if path:
                n = bk2_frames(path)
                if n <= 0:
                    die("%s: no input log" % tag)
                args = ["--bk2", path] + args
            limit = min(max(n, last_pin) + 10, BK2_FRAME_CEIL)
            specs.append((tag, GEN_CAP, limit - 2, limit, args, env))
        peaks, bad = {}, []
        for tag, rc, log, _png in run_batch(work, jobs)(specs, jobs):
            row = read_run(log)
            if row is None:
                die("%s: no census line in %s (exit %d)" % (tag, keep_log(log, tag), rc))
            if rc != 0:
                bad.append("%s: exit %d" % (tag, rc))
            if row["draws_retired"]:
                bad.append("%s: %d draws bound a retired buffer pair"
                           % (tag, row["draws_retired"]))
            for a, name in enumerate(ARENAS):
                if row["afail"][a]:
                    bad.append("%s %s: afail=%d at the raised cap" % (tag, name, row["afail"][a]))
                if row["high"][a] and row["high"][a] == row["cap"][a]:
                    bad.append("%s %s: high=%d equals cap - censored" % (tag, name, row["high"][a]))
            got = peaks_by_level(log)
            for lid, high in got.items():
                cur = peaks.setdefault(lid, {"high": [0] * len(ARENAS), "movies": []})
                cur["high"] = [max(x, y) for x, y in zip(cur["high"], high)]
                cur["movies"].append(tag)
            if not quiet:
                sys.stderr.write("  %-42s exit=%d levels=%s\n" % (
                    tag, rc, ",".join(LEVEL_BY_ID.get(l, str(l)) for l in sorted(got))))
        for lid, cur in sorted(peaks.items()):
            level = LEVEL_BY_ID.get(lid)
            if not level:
                continue
            merge_level(out_dir, level,
                        {"bk2": row_dict(cur["high"], [0] * len(ARENAS))},
                        {"bk2": "movies=%d\t%s" % (len(cur["movies"]),
                                                   ",".join(sorted(cur["movies"])))})
        rebuild_union(out_dir)
        for b in bad:
            sys.stderr.write("  BK2 FAIL %s\n" % b)
        print("bk2 corpus: %d movies, rows for %d levels -> %s"
              % (len(movies), len(peaks), out_dir))
        return 1 if bad else 0
    finally:
        drop_work(work)


# ---- freecam corpus --------------------------------------------------------

def freecam_runs():
    """The freecam-corpus rows: [(tag, level field, pose, canonical VBL)].

    A row is `<level>[:<area>] | pos=(x, y, z) yaw=Y pitch=P | [start]`; a
    row whose level field carries `@` is an aspect or capture variant of the
    same pose and adds no run. The tag is the level field (`:` written `-`)
    plus the first six hex digits of the pose's md5, the presentation
    harness's tile key."""
    out = []
    for row in corpus_manifest.load(ROOT, FREECAM_MANIFEST, "pipe"):
        f = [x.strip() for x in row.split("|")]
        if len(f) < 2 or not f[1] or "@" in f[0]:
            continue
        start = int(f[2]) if len(f) > 2 and f[2].isdigit() else FREECAM_START_BY_GAME[GAME]
        tag = "%s-%s" % (f[0].replace(":", "-"), hashlib.md5(f[1].encode()).hexdigest()[:6])
        out.append((tag, f[0], f[1], start))
    return out


def cmd_freecam(jobs, out_dir=None):
    """Run every freecam-corpus pose and write the per-level `freecam` rows."""
    out_dir = out_dir or OUT_DIR
    rows = freecam_runs()
    if not rows:
        die("the freecam manifest named no row")
    work = work_dir("terarena-freecam-")
    try:
        specs = []
        for tag, level, pose, start in rows:
            skip = "--skip-to-area" if re.fullmatch(r".+:\d+", level) else "--skip-to-level"
            specs.append((tag, GEN_CAP, start, start + 2,
                          [skip, level, "--freecam-pose", pose, "--lod", "extended",
                           "--aspect", "16:9"]))
        peaks, bad = {}, []
        for tag, rc, log, _png in run_batch(work, jobs)(specs, jobs):
            row = read_run(log)
            if row is None:
                die("%s: no census line in %s (exit %d)" % (tag, keep_log(log, tag), rc))
            if rc != 0:
                bad.append("%s: exit %d" % (tag, rc))
            for a, name in enumerate(ARENAS):
                if row["afail"][a]:
                    bad.append("%s %s: afail=%d at the raised cap" % (tag, name, row["afail"][a]))
                if row["high"][a] and row["high"][a] == row["cap"][a]:
                    bad.append("%s %s: high=%d equals cap - censored" % (tag, name, row["high"][a]))
            for lid, high in peaks_by_level(log).items():
                cur = peaks.setdefault(lid, {"high": [0] * len(ARENAS), "rows": []})
                cur["high"] = [max(x, y) for x, y in zip(cur["high"], high)]
                cur["rows"].append(tag)
        for lid, cur in sorted(peaks.items()):
            level = LEVEL_BY_ID.get(lid)
            if not level:
                continue
            merge_level(out_dir, level,
                        {"freecam": row_dict(cur["high"], [0] * len(ARENAS))},
                        {"freecam": "rows=%d\t%s" % (len(cur["rows"]),
                                                     ",".join(sorted(cur["rows"])))})
        rebuild_union(out_dir)
        for b in bad:
            sys.stderr.write("  FREECAM FAIL %s\n" % b)
        print("freecam corpus: %d rows, rows for %d levels -> %s"
              % (len(rows), len(peaks), out_dir))
        return 1 if bad else 0
    finally:
        drop_work(work)


def print_static_vs_traversal(out_dir=None):
    """Per level, the static maximum against the traversal maximum per arena."""
    out_dir = out_dir or OUT_DIR
    print("static (spawn/topdown) vs traversal (bk2/demo) peak, vertices")
    print("  %-16s %s" % ("level", " ".join("%-19s" % a for a in REPORT_ARENAS)))
    rows = []
    for level, _id in LEVELS:
        path = os.path.join(out_dir, level + ".tsv")
        if not os.path.exists(path):
            continue
        _, r = read_level(path)
        st = {a: max(r.get(p, {}).get(a, (0, 0))[0] for p in POSES) for a in ARENAS}
        tr = {a: max([r.get(p, {}).get(a, (0, 0))[0] for p in ("bk2", "demo")] or [0])
              for a in ARENAS}
        rows.append((max(tr[a] - st[a] for a in REPORT_ARENAS), level, st, tr))
    for _d, level, st, tr in sorted(rows, reverse=True):
        print("  %-16s %s" % (level, " ".join("%8d/%-10d" % (st[a], tr[a])
                                              for a in REPORT_ARENAS)))


# ---- attract demo ----------------------------------------------------------

def read_union_rows(out_dir=None, kinds=None):
    """level -> per-arena maximum `high` over the level's pose rows, every
    row kind or only those named in `kinds`."""
    out_dir = out_dir or OUT_DIR
    if not os.path.exists(os.path.join(out_dir, "union.tsv")):
        die("no committed union.tsv - run `generate` first")
    per_level = {}
    for level, _id in LEVELS:
        lp = os.path.join(out_dir, level + ".tsv")
        if not os.path.exists(lp):
            continue
        _, r = read_level(lp)
        rows = [p for p in r if kinds is None or p in kinds]
        per_level[level] = {a: max([r[p].get(a, (0, 0))[0] for p in rows] or [0])
                            for a in ARENAS}
    return per_level


def scratch_baseline(work):
    """Copy the committed per-level files and union.tsv into `work`/baseline
    and return that directory, so a mode that merges rows reads the committed
    rows and writes none of them back."""
    out = os.path.join(work, "baseline")
    os.makedirs(out)
    for name in os.listdir(OUT_DIR):
        if name.endswith(".tsv"):
            shutil.copyfile(os.path.join(OUT_DIR, name), os.path.join(out, name))
    return out


def cmd_demo(out_dir=None):
    """The attract-demo row: the accumulation assertions, and the demo rows
    merged into `out_dir` (a scratch copy of the committed tree when None)."""
    work = work_dir("terarena-demo-")
    base_tmp = None
    try:
        if out_dir is None:
            base_tmp = tempfile.mkdtemp(prefix="terarena-demo-base-")
            out_dir = scratch_baseline(base_tmp)
        per_level = read_union_rows(out_dir)
        # A level's own demand measured without a predecessor in the same
        # buffers: its static and movie rows, never the demo row this mode
        # rewrites.
        own = read_union_rows(out_dir, kinds=("spawn", "topdown", "bk2"))
        p, log, _ = launch(work, "demo", GEN_CAP, DEMO_SHOT_VBL, DEMO_FRAME_LIMIT, [])
        rc = p.wait()
        p._log_file.close()
        row = read_run(log)
        if row is None:
            die("attract demo: no census line in %s (exit %d)" % (keep_log(log, "attract-demo"), rc))
        visited = [LEVEL_BY_ID[i] for i in row["levels"] if i in LEVEL_BY_ID]
        print("attract demo: exit=%d svc=%d/%d edges=%d draws-retired=%d rebake=%d levels=%s"
              % (rc, row["svc_calls"], row["svc_fulfilled"], row["edges"],
                 row["draws_retired"], row["rebake"], ",".join(visited) or "(none)"))

        bad = []
        if rc != 0:
            bad.append("exit %d" % rc)
        if row["edges"] == 0:
            bad.append("the run log named no level edge - every assertion below is vacuous")
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
        # segment never touched carry no claim, and neither does a first mark
        # the incoming level's own demand covers (its spawn, topdown and bk2
        # rows): a level whose first sampled walks already bake past the
        # outgoing level's small peak on an arena that level barely used (the
        # anim arenas of a level with populated colour lists after one
        # without) reads above that peak with nothing surviving.
        segs = row["segments"]
        print("  segments (one per buffer generation): %s"
              % ", ".join("gen=%d lines=%d" % (s["gen"], s["lines"]) for s in segs))
        if len(segs) < 2:
            bad.append("only %d census segment(s) - the sawtooth is unmeasured" % len(segs))
        for i in range(1, len(segs)):
            prev, cur = segs[i - 1], segs[i]
            lv = LEVEL_BY_ID.get(cur["level"])
            bound = own.get(lv, {}) if lv else {}
            for a, name in enumerate(ARENAS):
                if cur["first"][a] <= bound.get(name, 0):
                    continue
                if prev["peak"][a] and cur["first"][a] >= prev["peak"][a]:
                    bad.append("%s: first mark after the edge into gen %d is %d, not below "
                               "gen %d's peak of %d - the outgoing level's geometry survived"
                               % (name, cur["gen"], cur["first"][a], prev["gen"],
                                  prev["peak"][a]))

        # Demand, reported and not asserted: the demo traverses, the static
        # poses do not, and the bk2/demo rows are what carry the traversal.
        for a, name in enumerate(ARENAS):
            bound = max([per_level[l].get(name, 0) for l in visited if l in per_level] or [0])
            print("  demand: %-9s exit hwm=%-8d vs row %-8d afail=%d"
                  % (name, row["high"][a], bound, row["afail"][a]))

        # The demo rows: each visited level's own peak, cut per level.
        got = peaks_by_level(log)
        for lid, high in sorted(got.items()):
            level = LEVEL_BY_ID.get(lid)
            if not level:
                continue
            merge_level(out_dir, level, {"demo": row_dict(high, [0] * len(ARENAS))},
                        {"demo": "edges=%d" % row["edges"]})
        rebuild_union(out_dir)
        for b in bad:
            sys.stderr.write("  DEMO FAIL %s\n" % b)
        print("terrain-arena demo row: %s" % ("PASS" if not bad else "FAIL"))
        return 1 if bad else 0
    finally:
        drop_work(work)
        if base_tmp:
            shutil.rmtree(base_tmp, ignore_errors=True)


# ---- overflow leg ----------------------------------------------------------

def cmd_overflow():
    """One level at a cap that cannot hold it, against its generated caps.

    The claim-back a full arena causes is a transient: the cache asks the
    device-owning thread for an overflow buffer and serves from it once the
    builder installs it. Four facts make that claim transient rather than
    permanent, and the image prove the overflow draws what one buffer drew.
    """
    work = tempfile.mkdtemp(prefix="terarena-ovf-")
    level = OVERFLOW_LEVEL_BY_GAME[GAME]
    limit = SHOT_VBL + OVERFLOW_TAIL
    try:
        args = static_args(level, "spawn")
        specs = [("stock", 0, SHOT_VBL, limit, args),
                 ("tiny", OVERFLOW_CAP, SHOT_VBL, limit, args)]
        runs = {}
        for tag, rc, log, png in run_batch(work, 2)(specs, 2):
            row = read_run(log)
            if row is None:
                die("%s: no census line in %s (exit %d)" % (tag, keep_log(log, tag), rc))
            runs[tag] = (rc, row, png)
        bad = []
        for tag in ("stock", "tiny"):
            rc, row, png = runs[tag]
            if rc != 0:
                bad.append("%s: exit %d" % (tag, rc))
            if not os.path.exists(png):
                bad.append("%s: no screenshot at %s" % (tag, png))
            if row["draws_retired"]:
                bad.append("%s: %d draws bound a retired buffer pair" % (tag, row["draws_retired"]))
        _rc, tiny, tiny_png = runs["tiny"]
        _rc2, stock, stock_png = runs["stock"]

        peak_afail = max((sum(c["afail"]) for c in tiny["lines"]), default=0)
        adopted = sum(tiny["ovf"])
        tail = [sum(c["afail"]) for c in tiny["lines"][-3:]]
        if peak_afail <= 0:
            bad.append("afail never rose at cap %d - the overflow path never ran (VACUOUS)"
                       % OVERFLOW_CAP)
        if adopted <= 0:
            bad.append("no overflow buffer was installed (ovf=0) - the claim is permanent")
        if len(tail) < 2 or len(set(tail)) != 1:
            bad.append("afail still rising at exit: last census lines %s - the overflow "
                       "did not stop the claim-back" % tail)
        if sum(stock["afail"]) != 0:
            bad.append("the generated caps claimed %d sectors back on %s"
                       % (sum(stock["afail"]), level))
        if sum(stock["ovf"]) != 0:
            bad.append("the generated caps needed %d overflow buffers on %s"
                       % (sum(stock["ovf"]), level))
        if os.path.exists(tiny_png) and os.path.exists(stock_png):
            if not filecmp.cmp(tiny_png, stock_png, shallow=False):
                bad.append("the overflow run's image differs from the stock-cap run's")
        print("overflow leg (%s, cap=%d): afail peak=%d exit=%d tail=%s ovf=%s caps=%.1fMiB"
              % (level, OVERFLOW_CAP, peak_afail, sum(tiny["afail"]), tail,
                 ",".join(str(x) for x in tiny["ovf"]), tiny["caps_mib"]))
        print("  stock leg: afail=%d ovf=%d caps=%.1fMiB image=%s"
              % (sum(stock["afail"]), sum(stock["ovf"]), stock["caps_mib"],
                 "identical" if not bad or "image differs" not in " ".join(bad) else "differs"))
        for b in bad:
            sys.stderr.write("  OVERFLOW FAIL %s\n" % b)
        print("terrain-arena overflow leg: %s" % ("PASS" if not bad else "FAIL"))
        return 1 if bad else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


# ---- check -----------------------------------------------------------------

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


def cmd_check(levels, jobs):
    rc = 0
    work = tempfile.mkdtemp(prefix="terarena-chk-")
    regen_dir = os.path.join(work, "regen")
    os.makedirs(regen_dir)
    try:
        # Leg `regen`: every row kind, regenerated into a temp tree and diffed
        # against the committed files.
        sys.stderr.write("leg regen (OPENPETE_TC_INITIAL_CAP=%d)\n" % GEN_CAP)
        data = sweep(work, GEN_CAP, levels, jobs)
        bad = assert_clean(data, "regen")
        write_static(regen_dir, data)
        rc |= cmd_bk2corpus(jobs, out_dir=regen_dir, quiet=True)
        rc |= cmd_freecam(jobs, out_dir=regen_dir)
        rc |= cmd_demo(out_dir=regen_dir)
        for level in levels:
            want = os.path.join(OUT_DIR, level + ".tsv")
            got = os.path.join(regen_dir, level + ".tsv")
            if not os.path.exists(want):
                bad.append("%s: no committed baseline" % level)
                continue
            d = diff_tsv(want, got)
            if d:
                bad.append("%s differs from the committed baseline:\n%s" % (level, d))
        d = diff_tsv(os.path.join(OUT_DIR, "union.tsv"),
                     os.path.join(regen_dir, "union.tsv"))
        if d:
            bad.append("union.tsv differs:\n%s" % d)
        for b in bad:
            sys.stderr.write("  REGEN FAIL %s\n" % b)
        rc |= 1 if bad else 0

        # Leg `stock`: the generated caps. This is the claim that no level at
        # either pose sends a sector back to the CPU walk for want of room.
        sys.stderr.write("leg stock (generated caps)\n")
        sdata = sweep(work, 0, levels, jobs)
        sbad = assert_clean(sdata, "stock")
        for b in sbad:
            sys.stderr.write("  STOCK FAIL %s\n" % b)
        rc |= 1 if sbad else 0

        sys.stderr.write("leg overflow\n")
        rc |= cmd_overflow()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("terrain-arena baseline check: %s" % ("PASS" if rc == 0 else "FAIL"))
    return rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=("generate", "check", "demo", "bk2-corpus",
                                     "freecam", "overflow", "report"))
    ap.add_argument("--game", default="spyro-1", choices=game_conf.GAMES)
    ap.add_argument("--build-dir", default="build/linux-release",
                    help="build tree holding the executable, relative to the source root")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--work", default=None,
                    help="keep run logs here and reuse the complete ones (module docstring)")
    ap.add_argument("--levels", default="")
    ap.add_argument("--write", action="store_true",
                    help="demo: write the demo rows into baselines/terrain-arena/<game>/")
    args = ap.parse_args()
    global WORK_KEEP
    WORK_KEEP = os.path.abspath(args.work) if args.work else None
    set_game(args.game, args.build_dir)
    if args.mode == "report":
        print_static_vs_traversal()
        return 0
    # The modes that replay the bk2 corpus load its manifest before any
    # prerequisite check, so a missing manifest fails on every machine.
    if args.mode in ("check", "bk2-corpus"):
        manifest_rows()
    if args.mode in ("check", "freecam"):
        corpus_manifest.load(ROOT, FREECAM_MANIFEST, "pipe")
    corpus_manifest.dry_run_exit()
    if not os.path.exists(EXE) or not os.path.exists(DISC):
        sys.stderr.write("baseline-terrain-arena: SKIP (no binary or no disc)\n")
        return EXIT_SKIP
    levels = ([l for l in args.levels.split(",") if l] or [n for n, _ in LEVELS])
    if args.mode == "generate":
        return cmd_generate(levels, args.jobs)
    if args.mode == "check":
        return cmd_check(levels, args.jobs)
    if args.mode == "bk2-corpus":
        return cmd_bk2corpus(args.jobs)
    if args.mode == "freecam":
        return cmd_freecam(args.jobs)
    if args.mode == "overflow":
        return cmd_overflow()
    return cmd_demo(OUT_DIR if args.write else None)


if __name__ == "__main__":
    sys.exit(main())
