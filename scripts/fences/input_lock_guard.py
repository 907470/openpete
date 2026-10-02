"""Fence: an unattended windowed launch ignores host input.

A scripted run that brings up a window shares the keyboard, the mouse and the
gamepads with the host session. A key pressed while that window has
focus opens the settings menu, engages the free camera or quits the run; a
pointer crossing it steers the camera. The run still exits 0 and its numbers
are still self-consistent, so nothing in a log shows the capture was steered.

The rule is one setting: an unattended launcher that can open a window passes
`--input-lock`, or sets `OPENPETE_INPUT_LOCK=1` for the whole script. The flag
is the better spelling for a script that can be pointed at the Windows .exe,
because an argument crosses the WSL boundary and an exported knob does not
without WSLENV (wslenv_guard.py enforces that half for the env spelling).
--bk2 replay and the injection script still drive a locked run, and closing
the window still ends it.

Scope. A file is in scope when it names the game binary, its name marks it as
an unattended launcher (the rumble guard's convention), and it can open a
window: it never passes `--headless`, or it names a Windows binary, which
harnesses run windowed on the real GPU. A script whose launches all go through
scripts/lib/game.sh's pg_capture is headless by construction and spells no
`--headless` itself; the marker below states that.

This file names the knob and talks about Windows binaries without launching
anything, so: wslenv-guard: no-launch, rumble-guard: no-launch,
input-lock-guard: no-launch

Run: python3 scripts/fences/input_lock_guard.py [root]
Exit: 0 clean, 65 an unattended windowed launcher takes host input.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rumble_guard                                         # noqa: E402

LAUNCH = rumble_guard.LAUNCH
UNATTENDED = rumble_guard.UNATTENDED
SKIP_DIRS = rumble_guard.SKIP_DIRS
SUFFIXES = rumble_guard.SUFFIXES

HEADLESS = re.compile(r"--headless\b|\bpg_capture\b")
WINDOWS = re.compile(r"\.exe\b|windows-mingw")

SANCTIONED = re.compile(r"""
      --input-lock\b                       # the flag, shell or python argv
    | \bOPENPETE_INPUT_LOCK\s*=\s*(1|on)\b # the env spelling, shell
    | OPENPETE_INPUT_LOCK"\s*,\s*"1        # python setdefault
    | OPENPETE_INPUT_LOCK['"]\]\s*=\s*['"]1   # python dict assignment
    | OPENPETE_INPUT_LOCK['"]\s*[:,]\s*['"]1   # python dict literal
""", re.X)

# The escape, spelled like the rumble guard's: `intentional` for a script a
# person drives through its window (a hands-on check), `no-launch` for one
# that names the binary and never starts it windowed.
EXEMPT = re.compile(r"input-lock-guard:\s*(intentional|no-launch)")


def windowed(text):
    return WINDOWS.search(text) is not None or HEADLESS.search(text) is None


def offends(name, text, require_unattended=True):
    """True when the file launches the game unattended, can open a window and
    never locks host input. The write-time hook passes
    require_unattended=False: there the file is known to be a launcher
    whatever it is called."""
    if not LAUNCH.search(text):
        return False
    if require_unattended and not UNATTENDED.match(name):
        return False
    if not windowed(text):
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
        print("fence (input_lock_guard): clean")
        return 0
    sys.stderr.write(
        "FENCE (input_lock_guard): these unattended launchers can open a "
        "window and never lock host input, so host keyboard, mouse or pad "
        "input can steer or end the run:\n")
    for path in bad:
        sys.stderr.write(f"  {path}\n")
    sys.stderr.write(
        "\nFix: `--input-lock` on the launch itself (an argument also reaches "
        "a Windows .exe), or `export OPENPETE_INPUT_LOCK=1` near the top of "
        "the script. If a person drives the script's window by hand, or the "
        "script never starts the game windowed, add the marker "
        "`input-lock-guard: intentional` or `input-lock-guard: no-launch`.\n")
    return 65


if __name__ == "__main__":
    sys.exit(main())
