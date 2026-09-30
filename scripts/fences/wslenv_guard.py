"""Fence: env knobs must actually reach a Windows .exe.

WSL interop does not hand the Linux environment to a Windows child process.
Only the names listed in WSLENV cross the boundary, and a missing name raises
nothing: the exe starts, reads the knob as unset, and runs a DIFFERENT
configuration from the one the harness asked for. Every number it then produces
is real, internally consistent, and about the wrong thing — the exact failure
shape this project keeps re-walking.

It has bitten twice. The standing pacing gate ran its whole Windows path with
OPENPETE_FORCE_PACING unset (every VBL unpaced, so the grid leg had nothing to
measure) and with OPENPETE_PACE_INJECT_PRESENT_US unset (so the fault-injection
leg was a duplicate of the baseline and still reported PASS). Separately, a
matrix harness built an env dict and never passed it to subprocess at all.

So this is mechanical now. A script that can launch a Windows binary and also
sets an OPENPETE_* variable must either go through scripts/lib/wslenv.sh (or
the Python child_env helper) or name WSLENV itself.

Run: python3 scripts/fences/wslenv_guard.py [root]
Exit: 0 clean, 65 a script would silently drop a knob.
"""
import os
import re
import sys

# A file only matters if it can start a Windows binary at all.
WINDOWS_LAUNCH = re.compile(r"windows-mingw|windows-msvc|\.exe\b")

# Any mention of a knob makes a file a candidate. This deliberately does NOT
# try to decide whether the mention is a real launch: the assignment shape
# that caused the outage was `COMMON=(OPENPETE_FORCE_PACING=1)` inside a bash
# array, which no reasonable pattern separates from the same text inside an
# echo. A fence that half-parses shell fails exactly on the cases worth
# catching, so this one over-reports and requires the author to say which it
# is. The cost of a wrong answer here is a whole evidence set about the wrong
# configuration; the cost of an annotation is one line.
# Shell/attribute `NAME=`, and the Python spellings a harness actually uses:
# a dict literal `{"NAME": v}` and a subscript assignment `env["NAME"] = v`.
# The bare-`=` form alone missed every Python harness in the scratchpad, which
# is where harnesses get written.
KNOB = re.compile(r"""
      \bOPENPETE_[A-Z0-9_]+\s*=
    | ['"]OPENPETE_[A-Z0-9_]+['"]\s*[:\]]
""", re.X)

# Only an actual USE clears a file, never a mention. An earlier version
# accepted the string "WSLENV" anywhere, so a comment explaining the hazard
# exempted the very file that had it — the exemption has to be as concrete as
# the thing it excuses.
SANCTIONED = re.compile(r"""
      \bwslenv_forward\b            # sourced the shell helper and called it
    | \bchild_env\s*\(              # built the child environment properly
    | \bWSLENV\s*=                  # set it directly, deliberately (shell)
    | ['"]WSLENV['"]\s*[:\]]        # ...python dict literal or env["WSLENV"]
    | \bWSLENV_ARGS\b               # launched with what the helper produced
""", re.X)

# The escape hatch, for a file that names a knob without ever launching
# anything with it (documentation, an echo, reading a CMake cache). Spelled
# out so it is a decision someone made, not a pattern that quietly matched.
EXEMPT = re.compile(r"wslenv-guard:\s*no-launch")

SKIP_DIRS = {".git", "notes", "worktrees", "build", "__pycache__", "node_modules",
             "third_party"}


def offends(text):
    """One file's verdict: the knobs it would silently drop, or [] if it is
    clean or out of scope. Shared with the write-time hook so both entry
    points enforce one rule."""
    if not (WINDOWS_LAUNCH.search(text) and KNOB.search(text)):
        return []
    if SANCTIONED.search(text) or EXEMPT.search(text):
        return []
    return sorted(set(m.group(0).strip(" =:]\"'") for m in KNOB.finditer(text)))


def scan(root):
    bad = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if not fn.endswith((".sh", ".py", ".bash")):
                continue
            p = os.path.join(dirpath, fn)
            if os.path.abspath(p) == os.path.abspath(__file__):
                continue
            try:
                text = open(p, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            knobs = offends(text)
            if knobs:
                bad.append((os.path.relpath(p, root), knobs))
    return bad


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..")
    root = os.path.abspath(root)
    bad = scan(root)
    if not bad:
        print("fence (wslenv_guard): clean")
        return 0
    sys.stderr.write(
        "FENCE (wslenv_guard): these scripts can launch a Windows binary and "
        "set OPENPETE_* knobs, but never mention WSLENV. Under WSL the knob "
        "does not arrive and the run silently uses the default:\n")
    for path, knobs in bad:
        sys.stderr.write(f"  {path}\n      {', '.join(knobs)}\n")
    sys.stderr.write(
        "\nFix: source scripts/lib/wslenv.sh and launch via "
        "`env \"${WSLENV_ARGS[@]}\" ...`, or build the child environment with "
        "a child_env()-style helper that appends to WSLENV. If the file "
        "never launches anything with these knobs (a comment, an echo, a "
        "CMake cache read), add the marker `wslenv-guard: no-launch`.\n")
    return 65


if __name__ == "__main__":
    sys.exit(main())
