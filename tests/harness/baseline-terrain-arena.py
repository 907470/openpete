#!/usr/bin/env python3
"""baseline-terrain-arena.py — per-level terrain-arena high-water baseline.

The retained terrain cache holds fifteen vertex arenas and eight cell-record
arenas (CN_*, CA_*: counted in records) whose capacities are
generated from this baseline (scripts/gen-terrain-arena-caps.py). An arena
that runs out returns -1 from its chunk allocator, the builder claims the
sector back to the CPU walk, and the cache asks the device thread for an
overflow buffer; the claim lasts until that buffer lands. Sizing the arenas so
the claim does not happen at all needs a demand figure per arena per level, and
a cap that binds censors that figure: `high` cannot exceed `cap`, so a row at
the cap is a clamp, not a demand. Every generating run therefore sets
OPENPETE_TC_INITIAL_CAP past any content this game holds.

Four row kinds per level, written as `pose` rows of
baselines/terrain-arena/<level>.tsv:

  spawn    the level's own spawn camera, original LOD — what a player's first
           frames ask of the arenas.
  topdown  the whole-level-visible freecam pose with extended LOD — the
           static worst case, every sector of the level in the frustum.
  bk2      the peak over every presentation-golden movie that plays in this
           level. A traversal exceeds both static poses: a camera that moves
           carries sectors it has left beside the ones it has reached.
  demo     the attract demo's peak in this level, for the levels it visits.

A traversal row is cut per level, not per run: the run log's census lines are
grouped by (level id, buffer generation), so a movie that crosses a level load
contributes to both levels and a level's own marks never carry the title
flyby or the outgoing level's geometry.

Modes:

  generate    run the static sweep and write the spawn/topdown rows.
  bk2-corpus  replay the presentation-golden movie corpus and write the bk2
              rows.
  demo        the 40,000-VBL attract demo: the cross-level accumulation
              assertions, plus the demo rows.
  check       three legs. `regen` repeats all three generating runs at the
              raised cap and diffs the result against the committed files,
              refusing any censored row (a high equal to its cap) and any
              non-zero afail. `stock` repeats the static sweep at the shipped
              caps and asserts afail is 0 everywhere, which is the claim "no
              level at either pose exceeds a generated cap". `overflow` proves
              the overflow buffer: one level at a cap that cannot hold it,
              against the same level at the generated caps.

Every mode rewrites only the rows it owns and then recomputes union.tsv from
the files, so the four row kinds accumulate across modes.

Exit: 0 pass, 1 fail or a missing corpus manifest (check and bk2-corpus; the
message names the path), 64 skip (binary or disc missing).
OPENPETE_MANIFEST_DRY_RUN=1 loads and reports the manifest, then exits 0.
"""
import argparse
import filecmp
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "scripts", "fences"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "scripts", "lib"))
import bench_guard  # noqa: F401  (import-time launcher preconditions)
import corpus_manifest

# Unattended launcher: no pad is being held, and the buzzing shows up in no
# log, so it would never surface on its own. (scripts/fences/rumble_guard.py)
os.environ.setdefault("OPENPETE_VIBRATION", "0")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DISC = os.path.join(ROOT, "games/spyro-1/spyro-1/build/SCUS-94228.cue")
EXE = os.path.join(ROOT, "build/linux-release/games/spyro-1/openpete-spyro1")
OUT_DIR = os.path.join(ROOT, "baselines/terrain-arena")
BK2_DIR = os.path.join(ROOT, "games/spyro-1/bk2")
MANIFEST = corpus_manifest.corpus_manifest(ROOT, "spyro-1", "presentation-corpus.manifest")
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
    "CN_OPAQUE", "CN_ALPHA", "CN_ADD", "CN_SUB",
    "CA_OPAQUE", "CA_ALPHA", "CA_ADD", "CA_SUB",
]

POSES = ("spawn", "topdown")
ROW_KINDS = ("spawn", "topdown", "bk2", "demo")
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
SHOT_VBL = 1755
FRAME_LIMIT = 1756
DEMO_FRAME_LIMIT = 40000
DEMO_SHOT_VBL = 39990
# A movie replay runs to one VBL past its last input. The ceiling bounds the
# sweep's runtime against a corpus that grows; no committed movie reaches it.
BK2_FRAME_CEIL = 4200
# The overflow leg's cap: below every level's smallest real demand, so the
# primary buffer of the busy arenas fills within the first walks.
OVERFLOW_CAP = 16384
OVERFLOW_LEVEL = "metalhead"
# The screenshot VBL is the static baseline's; the run outlives it, because the
# level edge that precedes it resets the arenas and the claim-back it causes
# takes a few tens of VBLs to settle. Asserting that afail has stopped rising
# needs the run to reach the far side of that transient.
OVERFLOW_SHOT = 1755
OVERFLOW_FRAME_LIMIT = 2400

HWM_RE = re.compile(
    r"\[ter-gpu f=(?:\d+)\] hwm=(\S+) afail=(\S+) ovf=(\S+) caps=(\S+)MiB "
    r"svc=(\d+)/(\d+) draws-retired=(\d+) rebake=(\d+)")
# core/psx/dispatch: the overlay switch that names each level the run enters.
LEVEL_LOAD_RE = re.compile(r"active overlay: \S+ -> level_(\d+)")
POSE_RE = re.compile(r"freecam: topdown pose set to")


def die(msg):
    sys.stderr.write("baseline-terrain-arena: %s\n" % msg)
    sys.exit(1)


# ---- launching -------------------------------------------------------------

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


RUN_FRAME_LIMIT = {}   # log path -> the --frame-limit its run was launched with


def launch(work, tag, cap, shot_vbl, frame_limit, extra):
    """One headless run. Returns (Popen, log path, shot path)."""
    log = os.path.join(work, tag + ".log")
    RUN_FRAME_LIMIT[log] = frame_limit
    shot = os.path.join(work, tag + ".png")
    vkshot = shot.replace(".png", "-vk.png")
    for f in (log, shot, vkshot):
        if os.path.exists(f):
            os.remove(f)
    cmd = [EXE, "--headless", "--no-config", "--disc-cache", "0",
           "--vibration", "0", "--renderer", "vk", "--screenshot-native-only",
           "--screenshot-on-frame", str(shot_vbl), "--screenshot-output", shot,
           "--frame-limit", str(frame_limit), "--no-card", "--mute", "1",
           "--disc", DISC] + list(extra)
    lf = open(log, "wb")
    p = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT, env=child_env(cap))
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
    """Run a list of (tag, cap, shot, limit, extra) specs `jobs` at a time.

    Yields (tag, rc, log, shot) in completion-queue order.
    """
    def _run(specs, njobs):
        running, i = [], 0
        while i < len(specs) or running:
            while len(running) < njobs and i < len(specs):
                tag, cap, shot, limit, extra = specs[i]
                i += 1
                p, log, png = launch(work, tag, cap, shot, limit, extra)
                running.append((p, tag, log, png))
            p, tag, log, png = running.pop(0)
            rc = p.wait()
            p._log_file.close()
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
        m = LEVEL_LOAD_RE.search(line)
        if m:
            cur = int(m.group(1))
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
    last["levels"] = sorted({int(x) for x in LEVEL_LOAD_RE.findall(txt)})
    last["edges"] = len(LEVEL_LOAD_RE.findall(txt))
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

    Lines before the first level load (the title flyby) carry level -1 and are
    dropped; a run that crosses a level edge contributes to each level it
    played, against that level's own buffer generation.
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
            die("%s %s: no census line in %s (exit %d)" % (level, pose, log, rc))
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
                    bad.append("%s %s %s: high=%d equals cap — the figure is censored, "
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
    work = tempfile.mkdtemp(prefix="terarena-gen-")
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
    shutil.rmtree(work, ignore_errors=True)
    return 1 if bad else 0


# ---- bk2 corpus ------------------------------------------------------------

_MANIFEST_ROWS = None


def manifest_rows():
    """The corpus manifest's rows, loaded once; a missing manifest exits 1."""
    global _MANIFEST_ROWS
    if _MANIFEST_ROWS is None:
        _MANIFEST_ROWS = corpus_manifest.load(ROOT, MANIFEST, "pipe")
    return _MANIFEST_ROWS


def manifest_bk2s():
    """Every distinct movie named by a presentation-golden manifest row.

    A row is `<bk2>[@<variant>] | ...`; a variant changes only the row's launch
    flags (vertex precision, a cache force), none of which moves a vertex into
    an arena, so one replay per movie covers the row set. The movie's own
    .bk2.args sidecar (its level, its LOD mode) is spliced by the binary.
    """
    names = [row.split("|")[0].strip().split("@")[0] for row in manifest_rows()]
    out = []
    for n in sorted(set(names)):
        if os.path.exists(os.path.join(BK2_DIR, n + ".bk2")):
            out.append(n)
    return out


def bk2_frames(path):
    """Movie length = the '|'-prefixed input lines of its Input Log."""
    try:
        z = zipfile.ZipFile(path)
        return sum(1 for l in z.read("Input Log.txt").decode("latin-1").splitlines()
                   if l.startswith("|"))
    except Exception:
        return 0


def cmd_bk2corpus(jobs, out_dir=OUT_DIR, quiet=False):
    """Replay every corpus movie and write the per-level `bk2` rows."""
    movies = manifest_bk2s()
    if not movies:
        die("the corpus manifest named no playable movie")
    work = tempfile.mkdtemp(prefix="terarena-bk2-")
    try:
        specs = []
        for name in movies:
            path = os.path.join(BK2_DIR, name + ".bk2")
            n = bk2_frames(path)
            if n <= 0:
                die("%s: no input log" % name)
            limit = min(n + 10, BK2_FRAME_CEIL)
            specs.append((name, GEN_CAP, limit - 2, limit, ["--bk2", path]))
        peaks, bad = {}, []
        for tag, rc, log, _png in run_batch(work, jobs)(specs, jobs):
            row = read_run(log)
            if row is None:
                die("%s: no census line in %s (exit %d)" % (tag, log, rc))
            if rc != 0:
                bad.append("%s: exit %d" % (tag, rc))
            if row["draws_retired"]:
                bad.append("%s: %d draws bound a retired buffer pair"
                           % (tag, row["draws_retired"]))
            for a, name in enumerate(ARENAS):
                if row["afail"][a]:
                    bad.append("%s %s: afail=%d at the raised cap" % (tag, name, row["afail"][a]))
                if row["high"][a] and row["high"][a] == row["cap"][a]:
                    bad.append("%s %s: high=%d equals cap — censored" % (tag, name, row["high"][a]))
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
        shutil.rmtree(work, ignore_errors=True)


def print_static_vs_traversal(out_dir=OUT_DIR):
    """Per level, the static maximum against the traversal maximum per arena."""
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

def read_union_rows(out_dir=OUT_DIR, kinds=None):
    """level -> per-arena maximum `high` over the level's pose rows, every
    row kind or only those named in `kinds`."""
    if not os.path.exists(os.path.join(out_dir, "union.tsv")):
        die("no committed union.tsv — run `generate` first")
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


def cmd_demo(out_dir=OUT_DIR):
    work = tempfile.mkdtemp(prefix="terarena-demo-")
    try:
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
            bad.append("only %d census segment(s) — the sawtooth is unmeasured" % len(segs))
        for i in range(1, len(segs)):
            prev, cur = segs[i - 1], segs[i]
            lv = LEVEL_BY_ID.get(cur["level"])
            bound = own.get(lv, {}) if lv else {}
            for a, name in enumerate(ARENAS):
                if cur["first"][a] <= bound.get(name, 0):
                    continue
                if prev["peak"][a] and cur["first"][a] >= prev["peak"][a]:
                    bad.append("%s: first mark after the edge into gen %d is %d, not below "
                               "gen %d's peak of %d — the outgoing level's geometry survived"
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
        shutil.rmtree(work, ignore_errors=True)


# ---- overflow leg ----------------------------------------------------------

def cmd_overflow():
    """One level at a cap that cannot hold it, against its generated caps.

    The claim-back a full arena causes is a transient: the cache asks the
    device-owning thread for an overflow buffer and serves from it once the
    builder installs it. Four facts make that claim transient rather than
    permanent, and the image prove the overflow draws what one buffer drew.
    """
    work = tempfile.mkdtemp(prefix="terarena-ovf-")
    try:
        args = static_args(OVERFLOW_LEVEL, "spawn")
        specs = [("stock", 0, OVERFLOW_SHOT, OVERFLOW_FRAME_LIMIT, args),
                 ("tiny", OVERFLOW_CAP, OVERFLOW_SHOT, OVERFLOW_FRAME_LIMIT, args)]
        runs = {}
        for tag, rc, log, png in run_batch(work, 2)(specs, 2):
            row = read_run(log)
            if row is None:
                die("%s: no census line in %s (exit %d)" % (tag, log, rc))
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
            bad.append("afail never rose at cap %d — the overflow path never ran (VACUOUS)"
                       % OVERFLOW_CAP)
        if adopted <= 0:
            bad.append("no overflow buffer was installed (ovf=0) — the claim is permanent")
        if len(tail) < 2 or len(set(tail)) != 1:
            bad.append("afail still rising at exit: last census lines %s — the overflow "
                       "did not stop the claim-back" % tail)
        if sum(stock["afail"]) != 0:
            bad.append("the generated caps claimed %d sectors back on %s"
                       % (sum(stock["afail"]), OVERFLOW_LEVEL))
        if sum(stock["ovf"]) != 0:
            bad.append("the generated caps needed %d overflow buffers on %s"
                       % (sum(stock["ovf"]), OVERFLOW_LEVEL))
        if os.path.exists(tiny_png) and os.path.exists(stock_png):
            if not filecmp.cmp(tiny_png, stock_png, shallow=False):
                bad.append("the overflow run's image differs from the stock-cap run's")
        print("overflow leg (%s, cap=%d): afail peak=%d exit=%d tail=%s ovf=%s caps=%.1fMiB"
              % (OVERFLOW_LEVEL, OVERFLOW_CAP, peak_afail, sum(tiny["afail"]), tail,
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
                                     "overflow", "report"))
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--levels", default="")
    args = ap.parse_args()
    if args.mode == "report":
        print_static_vs_traversal()
        return 0
    # The modes that replay the bk2 corpus load its manifest before any
    # prerequisite check, so a missing manifest fails on every machine.
    if args.mode in ("check", "bk2-corpus"):
        manifest_rows()
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
    if args.mode == "overflow":
        return cmd_overflow()
    return cmd_demo()


if __name__ == "__main__":
    sys.exit(main())
