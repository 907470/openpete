#!/usr/bin/env python3
"""terrain-claim-census.py — the S2b claim-ratchet report.

Runs the presentation corpus's bk2 rows and freecam poses headless with the
retained terrain cache ON and harvests one [tgpu-cen] line per row (the
per-walk claim-by-reason census printed under OPENPETE_TERRAIN_GPU_CENSUS=1),
then aggregates claims BY REASON into the migration roadmap for S2c: the
reason holding the most sectors on the CPU walk is the next verdict to move
into the shader.

The two corpora are aggregated SEPARATELY: bk2 rows replay real gameplay
cameras and are the honest ratchet; freecam poses force-draw every decoded
sector and near-claim behind-camera sectors (negative view-Z), which inflates
the `near` reason — their table is level-coverage colour, not the metric.

A row with NO census line is a hard failure, not a zero: it means the cache
never engaged (usable() false — on llvmpipe that is the missing
OPENPETE_TERRAIN_CACHE_FORCE=1 trap), and treating it as "no claims" would
read as perfect coverage.

Usage:
  scripts/terrain-claim-census.py [--out DIR] [--filter SUBSTR]
                                  [--jobs N] [--lod extended|original]
                                  [--bk2-only | --freecam-only]

Output: <out>/report.md + <out>/census.csv (+ per-row logs on failure).
"""
import argparse
import csv
import os
import re
import subprocess
import sys
import zipfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("OPENPETE_BIN",
                     os.path.join(ROOT, "build/linux-release/games/spyro-1/openpete-spyro1"))
DISC = os.environ.get("OPENPETE_DISC",
                      os.path.join(ROOT, "games/spyro-1/spyro-1/build/SCUS-94228.cue"))
REASONS = ["near", "portal", "anim", "scroll", "fill", "arena", "col",
           "slot", "tp", "nearb", "nodraw", "prim", "pcov"]
# `sel` (a mod material selector claiming its sectors back) is GONE, not
# zero: a material is baked into the face as a bucket and drawn from the
# cache, so there is no longer a reason code for it. Its absence is part of
# what --mods-dir now proves — a run with a selector armed produces the same
# reasons, not the same reasons plus a zero.
# nearb is a SUB-BUCKET of near (whole bounding sphere behind the near
# threshold -> the CPU walk emits nothing), not an additional claim reason —
# always 0 now that the no-draw verdict is unconditional (the fully-behind
# case never reaches the near claim). nodraw counts the
# sectors the no-draw verdict owns (neither walked nor drawn); the report
# treats them as covered. prim/pcov are SUB-BUCKETS of portal (rim-broadphase
# candidate / blended-face over an outline); both are structurally 0 now —
# the rim cull and the blended-face transparent-cover drop both run in the
# terrain vertex stage — so any nonzero portal count is the claim-all /
# no-refinement degenerate (planes momentarily unavailable, or the plane
# provider disabled). arena is the build-failure
# backstop bucket (true arena exhaustion / host OOM): the sectors it used
# to report were actually the OPTC_SLOT_DEAD decode-slot sentinel, which
# tsc_get now never populates. slot (decode-slot contest) is likewise
# structurally 0 — eviction skips GPU-owned slots; only a window with 17+
# owned sectors can still contest. Rows where a decode-cache eviction
# conflict forced the prepass's serial fallback double-counted every
# column (the retried walk summed into the same census line): baselines
# before the eviction fix carry exactly 2x inflation on such rows (the
# wild-flight-text bk2 and the beast_makers-8cf90e / gnorc_cove-c2acad
# freecam poses are known cases).
CEN_RE = re.compile(
    r"\[tgpu-cen f=(\d+)\] sub=(\d+) secs=(\d+) claims=(\d+) "
    r"near=(-?\d+) portal=(-?\d+) anim=(-?\d+) scroll=(-?\d+) fill=(-?\d+) "
    r"arena=(-?\d+) col=(-?\d+) slot=(-?\d+) tp=(-?\d+) "
    r"nearb=(-?\d+) nodraw=(-?\d+) prim=(-?\d+) pcov=(-?\d+)")


def read_manifest(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append([c.strip() for c in line.split("|")])
    return rows


def bk2_frames(name):
    """Movie length = input-frame lines in the bk2's Input Log.txt."""
    path = os.path.join(ROOT, "games/spyro-1/bk2", name + ".bk2")
    try:
        with zipfile.ZipFile(path) as z:
            log = next(n for n in z.namelist() if n.lower().startswith("input log"))
            return sum(1 for ln in z.read(log).decode("utf-8", "replace").splitlines()
                       if ln.startswith("|"))
    except Exception:
        return 0


def run_row(kind, slug, args, target_frame, lod, outdir, mods_dir=""):
    """Run one row; return (slug, dict) with the last canonical census at or
    before target_frame+1, or (slug, None) on failure (log kept)."""
    env = dict(os.environ)
    env.update({
        "OPENPETE_TERRAIN_CACHE_FORCE": "1",
        "OPENPETE_TERRAIN_GPU_CENSUS": "1",
        "MESA_LOADER_DRIVER_OVERRIDE": env.get("MESA_LOADER_DRIVER_OVERRIDE", "llvmpipe"),
    })
    lvp = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
    if os.path.exists(lvp):
        env.setdefault("VK_ICD_FILENAMES", lvp)
    # Headless runs engage the native feed (and therefore the terrain hook)
    # only when a native capture is requested — without the screenshot flags
    # the run is flat-out sim with no census lines at all.
    shots = os.path.join(outdir, "shots")
    os.makedirs(shots, exist_ok=True)
    cmd = [BIN, "--headless", "--no-config", "--disc", DISC,
           "--run-recomp", "0x80012204", "--renderer", "vk", "--mute", "1",
           "--aspect", "16:9", "--lod", lod, "--terrain-cache", "1",
           "--screenshot-native-only",
           "--screenshot-on-frame", str(target_frame),
           "--screenshot-output", os.path.join(shots, f"{slug}__{{n}}.png"),
           "--frame-limit", str(target_frame + 2),
           "--max-runtime-seconds", "280"] + args
    # A mods dir arms whatever selectors it holds. The point of running the
    # corpus with one is the terrain-material gate: an all-terrain selector
    # must leave every row's sector and claim counts exactly where they were
    # with no mods — on every row, not just the one pose it was developed
    # against, which is how the blend carve-out survived its first review.
    cmd += ["--mods-dir", mods_dir] if mods_dir else ["--mods", ""]
    try:
        p = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True,
                           text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return slug, None
    best = None
    for m in CEN_RE.finditer(p.stderr):
        f, sub = int(m.group(1)), int(m.group(2))
        if sub == 0 and f <= target_frame + 1:
            best = m
    if best is None:
        with open(os.path.join(outdir, f"FAIL-{kind}-{slug}.log"), "w") as f:
            f.write(p.stderr[-20000:])
        return slug, None
    d = {"secs": int(best.group(3)), "claims": int(best.group(4)),
         "frame": int(best.group(1))}
    for i, r in enumerate(REASONS):
        d[r] = int(best.group(5 + i))
    return slug, d


def fmt_table(rows_out, title):
    lines = [f"### {title}", "",
             "| row | GPU secs | claims | " + " | ".join(REASONS) + " | coverage |",
             "|---|---|---|" + "---|" * (len(REASONS) + 1)]
    for slug, d in rows_out:
        tot = d["secs"] + d["claims"]
        cov = f"{100.0 * d['secs'] / tot:.0f}%" if tot else "-"
        lines.append("| " + " | ".join(
            [slug, str(d["secs"]), str(d["claims"])] +
            [str(d[r]) for r in REASONS] + [cov]) + " |")
    lines.append("")
    return lines


def agg(rows_out):
    a = {r: 0 for r in REASONS}
    a["secs"] = a["claims"] = 0
    for _, d in rows_out:
        a["secs"] += d["secs"]
        a["claims"] += d["claims"]
        for r in REASONS:
            a[r] += d[r]
    return a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build/terrain-claim-census"))
    ap.add_argument("--filter", default="")
    ap.add_argument("--jobs", type=int,
                    default=int(os.environ.get("CORPUS_JOBS", 0)) or max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--lod", default="extended", choices=["extended", "original"])
    ap.add_argument("--bk2-only", action="store_true")
    ap.add_argument("--freecam-only", action="store_true")
    ap.add_argument("--mods-dir", default="",
                    help="run with this mods dir armed (default: --mods \"\", "
                         "no mods). Use it to hold a terrain material against "
                         "the whole corpus and watch the `sel` column.")
    opts = ap.parse_args()
    os.makedirs(opts.out, exist_ok=True)

    jobs = []  # (kind, slug, args, target_frame)
    if not opts.freecam_only:
        for row in read_manifest(os.path.join(ROOT, "scripts/presentation-corpus.manifest")):
            name = row[0]
            if opts.filter and opts.filter not in name:
                continue
            start = int(row[1]) if len(row) > 1 and row[1] else 1700
            if bk2_frames(name) < start:
                continue  # movie ends before the settle frame
            jobs.append(("bk2", name,
                         ["--bk2", f"games/spyro-1/bk2/{name}.bk2"], start))
    if not opts.bk2_only:
        for row in read_manifest(os.path.join(ROOT, "scripts/freecam-corpus.manifest")):
            level, pose = row[0], row[1]
            if opts.filter and opts.filter not in level:
                continue
            start = int(row[2]) if len(row) > 2 and row[2] else 1900
            import hashlib
            slug = f"{level}-{hashlib.md5(pose.encode()).hexdigest()[:6]}"
            jobs.append(("freecam", slug,
                         ["--skip-to-level", level, "--freecam-pose", pose], start))

    print(f"[claim-census] {len(jobs)} rows, lod={opts.lod}, jobs={opts.jobs}")
    results = {"bk2": [], "freecam": []}
    fails = []
    with ThreadPoolExecutor(max_workers=opts.jobs) as ex:
        futs = {ex.submit(run_row, k, s, a, t, opts.lod, opts.out,
                          opts.mods_dir): (k, s)
                for (k, s, a, t) in jobs}
        for fut, (kind, slug) in futs.items():
            slug2, d = fut.result()
            if d is None:
                fails.append((kind, slug))
                print(f"  FAIL {kind} {slug} (no census line — cache never engaged?)")
            else:
                results[kind].append((slug, d))
                print(f"  ok   {kind} {slug}: secs={d['secs']} claims={d['claims']}")

    for k in results:
        results[k].sort()

    with open(os.path.join(opts.out, "census.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kind", "row", "frame", "secs", "claims"] + REASONS)
        for kind in ("bk2", "freecam"):
            for slug, d in results[kind]:
                w.writerow([kind, slug, d["frame"], d["secs"], d["claims"]] +
                           [d[r] for r in REASONS])

    lines = [f"# Terrain claim census (S2b ratchet) — lod={opts.lod}", ""]
    if fails:
        lines += ["**FAILED ROWS (no engagement — investigate before trusting the rest):** " +
                  ", ".join(f"{k}/{s}" for k, s in fails), ""]
    for kind, title, caveat in (
            ("bk2", "bk2 movie legs (real gameplay cameras — THE ratchet)", ""),
            ("freecam", "freecam poses (level coverage; near= is INFLATED by "
                        "behind-camera force-draw)", "")):
        if not results[kind]:
            continue
        a = agg(results[kind])
        tot = a["secs"] + a["claims"] + a["nodraw"]
        cov = a["secs"] + a["nodraw"]
        lines += [f"## {title}", "",
                  f"Aggregate: {cov}/{tot} sectors off the CPU walk "
                  f"({100.0 * cov / tot:.1f}% coverage; {a['nodraw']} of those "
                  f"= behind-camera no-draw), {a['claims']} claimed:", ""]
        ranked = sorted((r for r in REASONS
                         if r not in ("nearb", "nodraw", "prim", "pcov")),
                        key=lambda r: -a[r])
        lines += ["| reason | sectors claimed | share of claims |", "|---|---|---|"]
        for r in ranked:
            if a[r] == 0:
                continue
            extra = ""
            if r == "near" and a["nearb"]:
                extra = (f" (of which {a['nearb']} = "
                         f"{100.0 * a['nearb'] / a['near']:.0f}% fully "
                         f"BEHIND the near plane — no-draw candidates)")
            if r == "portal" and (a["prim"] or a["pcov"]):
                extra = (f" (rim-cand {a['prim']}, blended-cover {a['pcov']}, "
                         f"degenerate {a['portal'] - a['prim'] - a['pcov']})")
            lines.append(f"| {r} | {a[r]} | "
                         f"{100.0 * a[r] / max(1, a['claims']):.1f}%{extra} |")
        lines.append("")
        lines += fmt_table(results[kind], "per-row detail")
    with open(os.path.join(opts.out, "report.md"), "w") as f:
        f.write("\n".join(lines))
    print(f"[claim-census] report: {os.path.join(opts.out, 'report.md')}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
