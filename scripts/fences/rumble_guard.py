"""Fence: a scripted launch must never rumble the human's controller.

Vibration ships ON by default, which is right for a player and wrong for every
other way this binary starts. Harnesses, capture scripts, gate batteries and
bench runs are machine-driven; a human is not holding the pad, and often is not
even at the desk. A background window buzzing a controller for ten minutes
while someone works is a real cost, and it is invisible in every log — the run
still passes, the numbers are still right, so nothing ever surfaces it.

Two layers already handle the common cases in code: headless never arms the
motors at all, and the pump-side apply gates on window focus, so an unfocused
window is silent whatever the config says. What those do not cover is a
windowed, focused, scripted run — a visual check, a screenshot capture that
brings a window up, a perf bench the author is watching. That is what this
fence is for.

The rule is one setting: an unattended launcher silences vibration, either
with `--vibration 0` on the invocation or `OPENPETE_VIBRATION=0` for the whole
script. The env spelling is usually the better of the two here — a harness
builds its argument list in several places, and a flag appended to the wrong
one of them is a silent miss.

The WSL caveat is real but belongs to another fence: an `OPENPETE_*` export
does not cross into a Windows .exe unless WSLENV names it, so a script that
launches the Windows build and sets the env spelling must go through
scripts/lib/wslenv.sh. That is exactly what wslenv_guard.py already enforces,
so the two fences compose: this one says the knob must be set, that one says
it must actually arrive.

This file names the knob and talks about Windows binaries without ever
launching one, so: wslenv-guard: no-launch

Run: python3 scripts/fences/rumble_guard.py [root]
Exit: 0 clean, 65 a script could rumble a human's pad.
"""
import os
import re
import sys

# A file only matters if it names the game binary. Matching a bare `$EXE`
# swept in icon converters and a toolchain wrapper — a fence whose output
# nobody can read is a fence nobody runs.
LAUNCH = re.compile(r"openpete-spyro1|openpete_spyro1")

# ...and only the UNATTENDED launchers are in scope. A machine-driven run is
# the hazard: nobody is holding the pad, often nobody is at the desk, and the
# run passes either way so the buzzing never surfaces in any log. An
# interactive one-off a human typed is not this fence's business — they are
# right there.
#
# The other two layers cover the rest and are enforced in code, not here:
# headless never arms the motors, and the pump-side apply gates on window
# focus, so a background window is silent whatever its arguments say. What
# neither covers is an unattended script that brings up a focused window,
# which is exactly the set below.
UNATTENDED = re.compile(r"^(check_|bench-|capture_|.*-corpus|.*_harness)")

# The flag itself, in any of the spellings a script might build it with.
SANCTIONED = re.compile(r"""
      --vibration[=\s]+0\b            # the flag, passed directly
    | --vibration[=\s]+"?\$           # the flag, forwarded from a variable
    | \bOPENPETE_VIBRATION\s*=\s*0\b  # the env spelling, shell
    | OPENPETE_VIBRATION"\s*,\s*"0    # the env spelling, python setdefault
    | OPENPETE_VIBRATION['"]\]\s*=\s*['"]0   # ...and python dict assignment
    | OPENPETE_VIBRATION['"]\s*[:,]\s*['"]0   # ...and a python dict literal
    | --vibration['"]\s*,\s*['"]0    # the flag, as python argv list members
""", re.X)

# The escape hatch, for the one kind of script that SHOULD rumble: a hands-on
# feel check, where a human is deliberately holding the pad. Spelled out so it
# is a decision someone made rather than a pattern that quietly matched.
EXEMPT = re.compile(r"rumble-guard:\s*(intentional|no-launch)")

SKIP_DIRS = {".git", "build", "__pycache__", "node_modules", "third_party",
             "dist", "mods-attic"}
SUFFIXES = (".sh", ".py", ".bash")


def offends(name, text, require_unattended=True):
    """One file's verdict: True when it launches the binary unattended and
    never silences vibration. Shared with the write-time hook, which passes
    require_unattended=False — the naming convention is a tree-scan heuristic
    for spotting harnesses among a repo's other scripts, and at write time
    there is nothing to guess: the file is a script, and it launches the game.

    Deliberately NOT exempting headless-only scripts. Whether a given
    invocation is headless cannot be read reliably out of shell that builds
    its arguments in arrays across a dozen lines, and guessing wrong here
    means the fence passes the one file worth catching. Passing
    --vibration 0 on a headless run costs nothing and makes the intent
    explicit at the launch site instead of implicit in a gate three layers
    down."""
    if not LAUNCH.search(text):
        return False
    if require_unattended and not UNATTENDED.match(name):
        return False
    if EXEMPT.search(text) or SANCTIONED.search(text):
        return False
    return True


def scan(root):
    bad = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if not name.endswith(SUFFIXES):
                continue
            path = os.path.join(dirpath, name)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
            except OSError:
                continue
            if offends(name, text):
                bad.append(os.path.relpath(path, root))
    return bad


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..")
    root = os.path.abspath(root)
    bad = scan(root)
    if not bad:
        print("fence (rumble_guard): clean")
        return 0
    sys.stderr.write(
        "FENCE (rumble_guard): these unattended launchers never silence "
        "vibration, so a machine-driven run can buzz a controller sitting on "
        "someone's desk:\n")
    for path in bad:
        sys.stderr.write(f"  {path}\n")
    sys.stderr.write(
        "\nFix: `export OPENPETE_VIBRATION=0` near the top of the script (one "
        "line, covers every invocation it builds), or `--vibration 0` on the "
        "launch itself. A script that launches the WINDOWS build with the env "
        "spelling must also forward it through scripts/lib/wslenv.sh, or it "
        "never arrives — wslenv_guard.py enforces that half. If the script is "
        "a deliberate hands-on feel check, or never launches the game, add "
        "the marker `rumble-guard: intentional` or `rumble-guard: "
        "no-launch`.\n")
    return 65


if __name__ == "__main__":
    sys.exit(main())
