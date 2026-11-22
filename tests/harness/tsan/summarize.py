#!/usr/bin/env python3
"""Sort the ThreadSanitizer reports of a check_tsan.sh output directory.

Usage: summarize.py OUTDIR [--full] [--prefix STEM]

--prefix reads only OUTDIR/STEM.tsan.* (one game's run); the default reads
every OUTDIR/*.tsan.*.

Each report in OUTDIR/*.tsan.* is keyed by the first project frame of each of
its two accesses (function and file:line) and assigned to a module by the
file of the first project frame of either access. Modules named in WATCH are
printed per unique report with both access stacks; the rest are counted per
module. A report with no project frame counts under "external".
"""
import collections
import glob
import os
import re
import sys

WATCH = [
    ("moby", re.compile(r"renderer/moby[^/]*\.c")),
    ("native_render", re.compile(r"renderer/native_render\.c")),
    ("workpool", re.compile(r"runtime/work_pool\.c")),
    ("terrain", re.compile(r"renderer/terrain\.c")),
    ("subtick_view", re.compile(r"renderer/subtick_view\.c")),
    ("reextract", re.compile(r"renderer/reextract[^/]*\.c")),
]
FRAME = re.compile(r"^\s+#\d+\s+(\S+)\s+(\S+?):(\d+)(?::\d+)?\s")
ACCESS = re.compile(r"^\s+(Previous )?(read|write|atomic read|atomic write)\b.*?(thread T\d+|main thread)", re.I)


def rel(path):
    """Return a source path relative to the tree root (from core/, games/, third_party/ or tests/)."""
    m = re.search(r"/((?:core|games|third_party|tests)/.*)$", path)
    return m.group(1) if m else path


def project_frame(frames):
    """Return the first frame whose file lies in the openpete tree outside third_party."""
    for fn, path, line in frames:
        if "/openpete/" in path and "/third_party/" not in path:
            return fn, rel(path), int(line)
    return None


def parse(path):
    """Yield (kind, [(access_header, frames)]) per report in a TSan log file."""
    text = open(path, errors="replace").read()
    for block in text.split("WARNING: ThreadSanitizer: ")[1:]:
        kind = block.split("(", 1)[0].strip().splitlines()[0]
        body = block.split("SUMMARY:")[0]
        accesses, cur = [], None
        for ln in body.splitlines():
            m = ACCESS.match(ln)
            if m:
                cur = (ln.strip(), [])
                accesses.append(cur)
                continue
            if ln.strip().startswith(("Mutex", "Location", "Thread T")):
                cur = None
                continue
            f = FRAME.match(ln)
            if f and cur is not None:
                cur[1].append(f.groups())
        yield kind, accesses


def module_of(pf):
    """Return the WATCH module name of a project frame, else its directory."""
    if pf is None:
        return "external"
    for name, rx in WATCH:
        if rx.search(pf[1]):
            return name
    return os.path.dirname(pf[1]) or pf[1]


def main():
    out = sys.argv[1]
    full = "--full" in sys.argv
    stem = "*"
    if "--prefix" in sys.argv:
        stem = glob.escape(sys.argv[sys.argv.index("--prefix") + 1])
    uniq = collections.OrderedDict()
    for f in sorted(glob.glob(os.path.join(glob.escape(out), stem + ".tsan.*"))):
        game = os.path.basename(f).split(".tsan")[0]
        for kind, acc in parse(f):
            pfs = [project_frame(fr) for _, fr in acc[:2]]
            mods = [module_of(p) for p in pfs]
            watched = [m for m in mods if m in dict(WATCH)]
            tops = [rel(fr[0][1]) for _, fr in acc[:2] if fr]
            tp = [t.split("/")[0] + "/" + t.split("/")[1] for t in tops if t.startswith("third_party/")]
            mod = (watched[0] if watched else tp[0] if tp
                   else next((m for m in mods if m != "external"), "external"))
            key = (kind, tuple((p[0], p[1], p[2]) if p else None for p in pfs))
            e = uniq.setdefault(key, {"mod": mod, "games": collections.Counter(), "acc": acc})
            e["games"][game] += 1
    per_mod = collections.Counter()
    per_mod_uniq = collections.Counter()
    for key, e in uniq.items():
        per_mod[e["mod"]] += sum(e["games"].values())
        per_mod_uniq[e["mod"]] += 1
    print("module                       unique  reports")
    for m, n in per_mod.most_common():
        print(f"{m:28s} {per_mod_uniq[m]:6d} {n:8d}")
    print()
    for key, e in uniq.items():
        if e["mod"] not in dict(WATCH) and not full:
            continue
        kind, pfs = key
        print(f"== [{e['mod']}] {kind}  {dict(e['games'])}")
        for hdr, frames in e["acc"][:2]:
            print(f"   {hdr}")
            for fn, path, line in frames[:6]:
                print(f"      {fn}  {rel(path)}:{line}")
        print()


if __name__ == "__main__":
    main()
