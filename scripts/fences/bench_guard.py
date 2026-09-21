"""Fence: benchmark preconditions.

Called by the bench entry points before any timed run. Refuses — with the
failed precondition named — rather than producing a number that measures
the wrong thing:

  1. Untuned binary: an exe whose build cache says OPENPETE_SKIP_DECOMP=OFF
     (read from CMakeCache, never set for a child — wslenv-guard: no-launch)
     (the -O0 dev hybrid) or that lives outside a linux-release /
     windows-mingw build tree. A dev-build fps figure is not a result.
  2. Contention: another openpete process already running — machine load
     has produced 10-20x phantom "regressions" before.

`--allow-untuned` (the caller passes allow=True) skips check 1 only.
Always PRINTS the resolved mod/texpack/opts surface next to the exe, so
every reported number carries what was actually loaded.

Shell callers run it as a command:

    python3 scripts/fences/bench_guard.py --exe "$EXE" || exit 65
"""
import argparse
import os
import subprocess
import sys


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


def check(exe, allow_untuned=False):
    _wslenv_scan()
    exe = os.path.abspath(exe)
    if not allow_untuned:
        cache = _build_cache(exe)
        if cache:
            txt = open(cache, errors="replace").read()
            if "OPENPETE_SKIP_DECOMP:BOOL=OFF" in txt:
                _die("exe's build cache has OPENPETE_SKIP_DECOMP=OFF (dev "
                     "hybrid, -O0 recomp) — not a benchmark binary. Use "
                     "build/linux-release or windows-mingw, or pass "
                     "--allow-untuned. cache: %s" % cache)
        if not ("linux-release" in exe or "windows-mingw" in exe):
            _die("exe is outside a linux-release/windows-mingw tree (%s); "
                 "numbers from other builds are not comparable. Pass "
                 "--allow-untuned to override." % exe)
    try:
        # -x (exact comm match) — a -f full-argv match would catch our own
        # wrapper shell, whose command line contains the exe path as text.
        out = subprocess.run(["pgrep", "-ax", "openpete-spyro1"],
                             capture_output=True, text=True).stdout.strip()
    except OSError:
        out = ""
    if out:
        _die("another openpete process is running — contention invalidates "
             "the run:\n%s" % out)


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
    a = ap.parse_args(argv)
    check(a.exe, allow_untuned=a.allow_untuned)
    print_surface(a.exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
