"""Fence: benchmark preconditions.

Called by the bench entry points before any timed run. Refuses - with the
failed precondition named - rather than producing a number that measures
the wrong thing:

  1. Untuned binary: an exe whose build cache names a preset include other
     than linux-release*.cmake / windows-mingw*.cmake (the -O0 linux-debug
     build, or a configure that named no preset), whose C/C++ flags carry
     -fsanitize, or that lives outside a linux-release / windows-mingw
     build tree. A dev-build fps figure is not a result.
  2. Every other precondition of scripts/bench/preflight.py for the exe's
     target (`windows` for a .exe, else `local`): the timing window held by
     this job, idle by process name on both sides, Linux and Windows load,
     the local fleet golden lock, and on Windows the display awake. Machine
     load has produced 10-20x phantom "regressions" before.

`--allow-untuned` (the caller passes allow=True) skips check 1 only.
`--functional` (functional=True) reports check 2 without refusing: the
caller produces no timing verdict (bench-corpus.py --functional marks its
rollup, and every compare refuses such a rollup against a timed one).
Always PRINTS the resolved mod/texpack/opts surface next to the exe, so
every reported number carries what was actually loaded.

build_identity() names a tree for comparison: the preset class (the
basename of the cache's CMAKE_PROJECT_openpete_INCLUDE file, identical in a
tree and in a worktree's tree), the arm (the linkage the configure wrote to
ARM_FILE at the tree root) and the source commit.

Shell callers run it as a command from inside the timing window
(scripts/bench/timing_lock.py run), since the command cannot hold it:

    python3 scripts/fences/bench_guard.py --exe "$EXE" || exit 65
"""
import argparse
import csv
import fnmatch
import io
import os
import subprocess
import sys

# Written by the top-level CMakeLists.txt on every configure: the resolved
# OPENPETE_RECOMP_LINKAGE, one word.
ARM_FILE = "openpete-recomp-linkage.txt"

# Windows-side CPU load, percent of all cores, above which a Windows-target
# run is refused; the mean of WIN_LOAD_SAMPLES one-second typeperf samples.
FOREIGN_LOAD_PCT = 20.0
WIN_LOAD_SAMPLES = 3


def _wslenv_scan():
    """Refuse a tree where a harness would silently drop an env knob.

    Cheap (~30 ms) and sited here because a bench run configured through the
    environment is the case where losing a knob is invisible: the run
    completes, the numbers are real, and they are about the default."""
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    try:
        import wslenv_guard
    except ImportError:
        return
    bad = wslenv_guard.scan(os.path.abspath(os.path.join(here, "..", "..")))
    if bad:
        _die("scripts would drop env knobs crossing to a Windows exe: "
             + ", ".join(p for p, _ in bad)
             + " (run scripts/fences/wslenv_guard.py)")


def _die(reason):
    sys.stderr.write("FENCE (bench_guard): %s\n" % reason)
    sys.exit(65)


def _build_cache(exe):
    d = os.path.dirname(os.path.abspath(exe))
    for _ in range(6):
        c = os.path.join(d, "CMakeCache.txt")
        if os.path.isfile(c):
            return c
        nd = os.path.dirname(d)
        if nd == d:
            break
        d = nd
    return None


def _cache_entries(cache):
    """Return (preset include path or None, [C/C++ flag values]) from a cache.

    The include entry is matched by its `CMAKE_PROJECT_openpete_INCLUDE:`
    prefix with any type: a preset's cacheVariables set it untyped, so every
    cache spells it `:UNINITIALIZED=`."""
    include, flags = None, []
    for line in open(cache, errors="replace"):
        key, sep, val = line.rstrip("\n").partition("=")
        if not sep:
            continue
        name = key.split(":", 1)[0]
        if name == "CMAKE_PROJECT_openpete_INCLUDE":
            include = val
        elif name in ("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS"):
            flags.append(val)
    return include, flags


def _cache_value(cache, wanted):
    """The value of one cache entry (any type), or None."""
    for line in open(cache, errors="replace"):
        key, sep, val = line.rstrip("\n").partition("=")
        if sep and key.split(":", 1)[0] == wanted:
            return val
    return None


def _git(src, *args):
    try:
        r = subprocess.run(["git", "-C", src] + list(args),
                           capture_output=True, text=True)
    except OSError:
        return None
    return r.stdout.strip() if r.returncode == 0 else None


def build_identity(exe):
    """The comparable identity of the build tree holding `exe`, or None when
    no CMakeCache.txt is found above it.

    Keys: tree (absolute path), preset_class (the preset include's basename
    without `.cmake`; None when the cache names none), arm (ARM_FILE's word;
    None when the file is missing), arm_file, source, commit, and dirty
    (tracked changes in the source checkout)."""
    cache = _build_cache(exe)
    if not cache:
        return None
    tree = os.path.dirname(cache)
    include, _ = _cache_entries(cache)
    preset = os.path.basename(include) if include else None
    if preset and preset.endswith(".cmake"):
        preset = preset[:-len(".cmake")]
    arm = None
    arm_path = os.path.join(tree, ARM_FILE)
    if os.path.isfile(arm_path):
        arm = open(arm_path).read().strip() or None
    src = _cache_value(cache, "CMAKE_HOME_DIRECTORY")
    commit = _git(src, "rev-parse", "HEAD") if src else None
    status = _git(src, "status", "--porcelain", "--untracked-files=no") if src else None
    return {"tree": tree, "preset_class": preset, "arm": arm,
            "arm_file": arm_path, "source": src, "commit": commit,
            "dirty": bool(status)}


def _windows_cpu_load():
    """Mean total CPU load on the Windows side, or None when typeperf.exe
    cannot run or prints no sample."""
    try:
        out = subprocess.run(["typeperf.exe", r"\Processor(_Total)\% Processor Time",
                              "-sc", str(WIN_LOAD_SAMPLES)],
                             capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    vals = []
    for row in csv.reader(io.StringIO(out.replace("\r", ""))):
        if len(row) == 2:
            try:
                vals.append(float(row[1]))
            except ValueError:
                pass
    return sum(vals) / len(vals) if vals else None


def _refuse_untuned(exe):
    """Die when `exe` is not a tuned benchmark binary (check 1)."""
    cache = _build_cache(exe)
    if cache:
        include, flags = _cache_entries(cache)
        base = os.path.basename(include or "")
        if not (fnmatch.fnmatch(base, "linux-release*.cmake")
                or fnmatch.fnmatch(base, "windows-mingw*.cmake")):
            _die("exe's build cache names preset include %r, not "
                 "linux-release*.cmake or windows-mingw*.cmake - not a "
                 "benchmark binary. Use build/linux-release or "
                 "windows-mingw, or pass --allow-untuned. cache: %s"
                 % (base or "(none)", cache))
        if any("-fsanitize" in f for f in flags):
            _die("exe's build cache compiles with -fsanitize - not a "
                 "benchmark binary. Pass --allow-untuned to override. "
                 "cache: %s" % cache)
    if not ("linux-release" in exe or "windows-mingw" in exe):
        _die("exe is outside a linux-release/windows-mingw tree (%s); "
             "numbers from other builds are not comparable. Pass "
             "--allow-untuned to override." % exe)


# Held for the life of a Python caller: the Windows display holder.
_DISPLAY = None

# The harnesses behind this fence run the exe from its build tree and
# measure steady-state presents after boot, where a 9P exe path costs
# nothing measured; a boot-time leg goes through scripts/bench/preflight.py
# without this acceptance.
HARNESS_ACCEPTS = ("windows-local",)


def check(exe, allow_untuned=False, functional=False, hold=True):
    """Refuse (exit 65) unless every benchmark precondition holds.

    The checks are scripts/bench/preflight.py's for the exe's target
    (`windows` for a .exe, else `local`), printed one per line. With `hold`
    (a Python caller) this process takes the timing window for the rest of
    its life unless it already runs inside it, and on a Windows target
    wakes the display and holds it awake until exit; the CLI (hold=False)
    requires its caller to run inside the window. `functional` prints the
    checks without refusing on them and takes no window."""
    global _DISPLAY
    _wslenv_scan()
    exe = os.path.abspath(exe)
    if not allow_untuned:
        _refuse_untuned(exe)
    bench = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench")
    sys.path.insert(0, os.path.normpath(bench))
    import preflight
    import timing_lock
    target = "windows" if exe.lower().endswith(".exe") else "local"
    if hold and not functional:
        timing_lock.ensure_held("bench_guard " + os.path.basename(sys.argv[0]))
        if target == "windows" and _DISPLAY is None:
            _DISPLAY = timing_lock.hold_display()
    accepts = set(HARNESS_ACCEPTS) | ({"tuned"} if allow_untuned else set())
    checks = preflight.apply_accepts(
        preflight.run_checks(target, exe, (), {}), accepts)
    checks = [c for c in checks if c.name not in ("window", "present-mode")]
    bad = preflight.report(target, checks, sys.stderr)
    if bad and functional:
        sys.stderr.write("bench_guard: functional run, not refused: %s\n" % ", ".join(bad))
    elif bad:
        _die("preconditions failed: " + ", ".join(bad))


def print_surface(exe, mods_dir=None, texpack_dir=None):
    exe_dir = os.path.dirname(os.path.abspath(exe))
    sidecars = [f for f in ("openpete.toml",)
                if os.path.isfile(os.path.join(exe_dir, f))]
    print("bench_guard: exe=%s" % exe)
    print("bench_guard: sidecars=%s mods_dir=%s texpack_dir=%s"
          % (",".join(sidecars) or "none", mods_dir or "none",
             texpack_dir or "none"))


def main(argv=None):
    ap = argparse.ArgumentParser(description="benchmark precondition fence")
    ap.add_argument("--exe", required=True,
                    help="the binary the timed run will launch")
    ap.add_argument("--allow-untuned", action="store_true",
                    help="skip the tuned-build check, keep the contention check")
    ap.add_argument("--functional", action="store_true",
                    help="report contention without refusing (no timing verdict)")
    a = ap.parse_args(argv)
    check(a.exe, allow_untuned=a.allow_untuned, functional=a.functional, hold=False)
    print_surface(a.exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
