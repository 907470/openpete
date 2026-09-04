#!/usr/bin/env python3
"""adopt-census.py — the render-time-transient colour population.

Runs every bk2 in the presentation corpus manifest to the end of its movie
(plus a settle margin so a cutscene the movie only starts still plays out)
and the 20k-tick attract demo, headless with a native capture requested so
the moby feed runs, under OPENPETE_MOBY_ADOPT_CENSUS=1. Harvests each run's
`moby adopt census:` exit line and its per-frame `[adopt-cen ...]` example
lines, then aggregates accepted adoptions BY MOBY CLASS across the corpus.

A row with no exit census line is a hard failure (the feed never ran or the
run died), never a zero.

Usage:
  scripts/adopt-census.py [--out DIR] [--filter SUBSTR] [--jobs N]
                          [--lod original|extended] [--no-demo] [--demo-frames N]
Output: <out>/report.md, <out>/census.csv, <out>/logs/<row>.adopt.log
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
EXIT_RE = re.compile(r"moby adopt census: frames=(\d+) faces=(\d+) classes=(\d+)((?: cls=\d+:faces=\d+,frames=\d+)*)")
CLS_RE = re.compile(r"cls=(\d+):faces=(\d+),frames=(\d+)")
FEED_RE = re.compile(r"render-paths: feed audit: fed_path_frames=\d+ unmarked_path_frames=\d+[^\n]*")
LINE_RE = re.compile(r"\[adopt-cen f=(\d+)\] cls=(\d+) faces=(\d+) moby=([0-9A-F]+) fkey=([0-9A-F]+) "
                     r"walk=([0-9A-F/]+) adopt=([0-9A-F/]+)")


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
    path = os.path.join(ROOT, "games/spyro-1/bk2", name + ".bk2")
    try:
        with zipfile.ZipFile(path) as z:
            log = next(n for n in z.namelist() if n.lower().startswith("input log"))
            return sum(1 for ln in z.read(log).decode("utf-8", "replace").splitlines()
                       if ln.startswith("|"))
    except Exception:
        return 0


def run_row(slug, args, frame_limit, lod, outdir, timeout_s):
    env = dict(os.environ)
    env.update({"OPENPETE_MOBY_ADOPT_CENSUS": "1",
                "MESA_LOADER_DRIVER_OVERRIDE": env.get("MESA_LOADER_DRIVER_OVERRIDE", "llvmpipe")})
    lvp = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
    if os.path.exists(lvp):
        env.setdefault("VK_ICD_FILENAMES", lvp)
    shots = os.path.join(outdir, "shots")
    logs = os.path.join(outdir, "logs")
    os.makedirs(shots, exist_ok=True)
    os.makedirs(logs, exist_ok=True)
    # The native feed (and with it the adopt) runs headless only when a
    # native capture is requested; one shot near the end keeps it live for
    # the whole run.
    cmd = [BIN, "--headless", "--no-config", "--disc", DISC,
           "--run-recomp", "0x80012204", "--renderer", "vk", "--mute", "1",
           "--aspect", "16:9", "--lod", lod, "--mods", "",
           "--screenshot-native-only",
           "--screenshot-on-frame", str(frame_limit - 5),
           "--screenshot-output", os.path.join(shots, f"{slug}__{{n}}.png"),
           "--frame-limit", str(frame_limit),
           "--max-runtime-seconds", str(timeout_s - 20)] + args
    try:
        p = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True,
                           text=True, timeout=timeout_s)
        err = p.stderr
    except subprocess.TimeoutExpired as e:
        err = (e.stderr or b"").decode("utf-8", "replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
    lines = LINE_RE.findall(err)
    with open(os.path.join(logs, f"{slug}.adopt.log"), "w") as f:
        for m in LINE_RE.finditer(err):
            f.write(m.group(0) + "\n")
    m = EXIT_RE.search(err)
    if not m and not lines:
        with open(os.path.join(logs, f"FAIL-{slug}.log"), "w") as f:
            f.write(err[-20000:])
        return slug, None
    # The exit line is kept beside the example lines so the report can be
    # rebuilt from the logs (--report-only) without re-running the corpus.
    fa = FEED_RE.search(err)
    with open(os.path.join(logs, f"{slug}.exit.txt"), "w") as f:
        f.write(f"frame_limit={frame_limit}\n" + (m.group(0) if m else "(no exit line)") + "\n")
        if fa:
            f.write(fa.group(0) + "\n")
    return slug, parse_row(m, lines, frame_limit, fa)


def parse_row(m, lines, frame_limit, feed=None):
    """Totals come from the exit line when present. Every per-frame line
    carries that frame's face count for its class, so the same totals are
    reconstructed from the lines when the exit line is missing (a torn
    stderr write at exit); `frames` is then the count of distinct frames."""
    if m:
        per_cls = {int(c): (int(fa), int(fr)) for c, fa, fr in CLS_RE.findall(m.group(4))}
        frames, faces = int(m.group(1)), int(m.group(2))
    else:
        per_cls = {}
        frames_seen = set()
        for f_, cls, fcount, *_ in lines:
            fa_, fr_ = per_cls.get(int(cls), (0, 0))
            per_cls[int(cls)] = (fa_ + int(fcount), fr_ + 1)
            frames_seen.add(int(f_))
        frames, faces = len(frames_seen), sum(fa for fa, fr in per_cls.values())
    examples = {}
    for f_, cls, faces, moby, fkey, walk, adopt in lines:
        examples.setdefault(int(cls), (int(f_), moby, fkey, walk, adopt))
    # The render-path feed audit's exit line rides along: an unmarked feed is
    # a sub-tick flicker defect the corpus run surfaces for free.
    unmarked = feed.group(0).split("unmarked_path_frames=", 1)[1] if feed else ""
    return {"frames": frames, "faces": faces, "reconstructed": m is None,
            "per_cls": per_cls, "examples": examples, "frame_limit": frame_limit,
            "feed_unmarked": unmarked}


def rows_from_logs(outdir):
    logs = os.path.join(outdir, "logs")
    results = []
    for fn in sorted(os.listdir(logs)):
        if not fn.endswith(".exit.txt"):
            continue
        slug = fn[:-len(".exit.txt")]
        text = open(os.path.join(logs, fn)).read()
        fl = int(re.search(r"frame_limit=(\d+)", text).group(1))
        m = EXIT_RE.search(text)
        lines = LINE_RE.findall(open(os.path.join(logs, f"{slug}.adopt.log")).read())
        results.append((slug, parse_row(m, lines, fl, FEED_RE.search(text))))
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build/adopt-census"))
    ap.add_argument("--filter", default="")
    ap.add_argument("--jobs", type=int,
                    default=int(os.environ.get("CORPUS_JOBS", 0)) or max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--lod", default="original", choices=["original", "extended"])
    ap.add_argument("--no-demo", action="store_true")
    ap.add_argument("--demo-frames", type=int, default=40000,
                    help="VBL frame limit for the attract-demo row (20k ticks = 40000 VBLs)")
    ap.add_argument("--settle", type=int, default=400,
                    help="VBLs run past the end of each movie")
    ap.add_argument("--report-only", action="store_true",
                    help="rebuild report.md/census.csv from <out>/logs without running")
    opts = ap.parse_args()
    os.makedirs(opts.out, exist_ok=True)
    if opts.report_only:
        results = rows_from_logs(opts.out)
        fails = [fn[len("FAIL-"):-4] for fn in os.listdir(os.path.join(opts.out, "logs"))
                 if fn.startswith("FAIL-")]
        return write_report(opts, results, fails)

    jobs = []  # (slug, args, frame_limit, timeout)
    for row in read_manifest(os.path.join(ROOT, "scripts/presentation-corpus.manifest")):
        name = row[0]
        if opts.filter and opts.filter not in name:
            continue
        n = bk2_frames(name)
        if n <= 0:
            print(f"  SKIP {name}: no/empty bk2")
            continue
        jobs.append((name, ["--bk2", f"games/spyro-1/bk2/{name}.bk2"], n + opts.settle, 600))
    if not opts.no_demo and (not opts.filter or opts.filter in "attract-demo"):
        jobs.append(("attract-demo", [], opts.demo_frames, 1800))

    print(f"[adopt-census] {len(jobs)} rows, lod={opts.lod}, jobs={opts.jobs}")
    results, fails = [], []
    with ThreadPoolExecutor(max_workers=opts.jobs) as ex:
        futs = {ex.submit(run_row, s, a, fl, opts.lod, opts.out, to): s for (s, a, fl, to) in jobs}
        for fut in futs:
            slug, d = fut.result()
            if d is None:
                fails.append(slug)
                print(f"  FAIL {slug} (no exit census line)")
            else:
                results.append((slug, d))
                cls = ",".join(f"{c}:{fa}" for c, (fa, fr) in sorted(d["per_cls"].items()))
                print(f"  ok   {slug}: frames={d['frames']} faces={d['faces']} [{cls}]")

    return write_report(opts, results, fails)


def write_report(opts, results, fails):
    agg = {}
    for slug, d in results:
        for c, (fa, fr) in d["per_cls"].items():
            a = agg.setdefault(c, {"faces": 0, "frames": 0, "rows": [], "example": None})
            a["faces"] += fa
            a["frames"] += fr
            a["rows"].append(slug)
            if a["example"] is None and c in d["examples"]:
                a["example"] = (slug,) + d["examples"][c]

    with open(os.path.join(opts.out, "census.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["row", "frame_limit", "adopt_frames", "adopt_faces", "class", "class_faces", "class_frames"])
        for slug, d in sorted(results):
            if not d["per_cls"]:
                w.writerow([slug, d["frame_limit"], 0, 0, "", 0, 0])
            for c, (fa, fr) in sorted(d["per_cls"].items()):
                w.writerow([slug, d["frame_limit"], d["frames"], d["faces"], c, fa, fr])

    lines = ["# Moby adoption census", "",
             f"rows run: {len(results)} ok, {len(fails)} failed; lod={opts.lod}", ""]
    if fails:
        lines += ["## FAILED rows (no census line)", ""] + [f"- {s}" for s in fails] + [""]
    lines += ["## Adoptions by moby class (all rows)", "",
              "| class | faces | frames | rows | example (row f moby fkey walk -> adopt) |",
              "|---|---|---|---|---|"]
    for c, a in sorted(agg.items(), key=lambda kv: -kv[1]["faces"]):
        ex = a["example"]
        exs = f"{ex[0]} f={ex[1]} moby={ex[2]} fkey={ex[3]} {ex[4]} -> {ex[5]}" if ex else "-"
        lines.append(f"| {c} | {a['faces']} | {a['frames']} | {len(a['rows'])} | {exs} |")
    bad = [(slug, d["feed_unmarked"]) for slug, d in sorted(results)
           if d["feed_unmarked"] and not d["feed_unmarked"].startswith("0")]
    lines += ["", "## Render-path feed audit (unmarked feeds = sub-tick flicker)", ""]
    lines += [f"- {slug}: unmarked_path_frames={u}" for slug, u in bad] or ["- none"]
    lines += ["", "## Per row", "", "| row | frame limit | adopt frames | adopt faces | classes |", "|---|---|---|---|---|"]
    for slug, d in sorted(results):
        cls = ", ".join(f"{c}:{fa}" for c, (fa, fr) in sorted(d["per_cls"].items())) or "-"
        rec = " (from lines)" if d.get("reconstructed") else ""
        lines.append(f"| {slug}{rec} | {d['frame_limit']} | {d['frames']} | {d['faces']} | {cls} |")
    with open(os.path.join(opts.out, "report.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"[adopt-census] wrote {opts.out}/report.md")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
