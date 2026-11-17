"""Fence: a scripted windowed Linux launch runs on a private Xvfb display.

A harness that opens a game window on the session's display puts it on the
user's desktop and takes focus (under WSLg, the Windows desktop), and a
compositor that stops answering stalls the run with no log line. The one
mechanism is scripts/lib/xvfb.sh: shell harnesses source it and launch under
its OP_XVFB prefix, Python harnesses take the prefix from
scripts/lib/hostenv.py xvfb_prefix. It also carries the declared escape,
OPENPETE_HARNESS_REAL_DISPLAY=1.

Scope. A file is in scope when it launches the game binary (rumble_guard's
`launches`, or scripts/lib/game.sh's PG_BIN followed by arguments, or
game_conf's default_exe), its name marks it as an unattended launcher (the rumble guard's
convention), and it can open a window: it passes `--window`, or never passes
`--headless` (input_lock_guard's HEADLESS: a script whose launches all go
through pg_capture is headless by construction). The check is per file, not
per launch: a file that uses the helper anywhere passes.

Markers, each for a file the helper does not fit:
  `xvfb-guard: windows-only`    every windowed launch is a Windows .exe
  `xvfb-guard: private-display` the file builds its own private display
                                (a window manager or compositor under test)
  `xvfb-guard: intentional`     a person drives the window on purpose
  `xvfb-guard: no-launch`       the file never starts the game windowed

This file names the binary and the flags without launching anything, so:
rumble-guard: no-launch, wslenv-guard: no-launch,
input-lock-guard: no-launch, sandbox-guard: no-launch, xvfb-guard: no-launch

Run: python3 scripts/fences/xvfb_guard.py [root]
Exit: 0 clean, 65 a windowed launcher can open its window on the session's
display.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import input_lock_guard                                     # noqa: E402
import rumble_guard                                         # noqa: E402

WINDOW = re.compile(r"--window\b")
# The resolved binary of scripts/lib/game.sh and game_conf.py, which
# rumble_guard's LAUNCH does not name: PG_BIN followed by an argument (not
# by a test's closing bracket), or the default_exe key.
RESOLVED = re.compile(r"""\$\{?PG_BIN\}?"?[ \t]+(?!\]|-[a-z]?[ \t]*\])["$-]
                       | \[["']default_exe["']\]""", re.X)
SANCTIONED = re.compile(r"scripts/lib/xvfb\.sh|\bop_xvfb_(?:prefix|set)\b|\bxvfb_prefix\(")
EXEMPT = re.compile(r"xvfb-guard:\s*(windows-only|private-display|intentional|no-launch)")


def offends(name, text, require_unattended=True):
    """True when the file launches the game unattended where it can open a
    window and never takes its display from scripts/lib/xvfb.sh. The
    write-time hook passes require_unattended=False."""
    if require_unattended and not rumble_guard.UNATTENDED.match(name):
        return False
    code = "\n".join(rumble_guard.logical_lines(rumble_guard.code_text(name, text)))
    if not (rumble_guard.launches(name, text) or RESOLVED.search(code)):
        return False
    if not WINDOW.search(code) and input_lock_guard.HEADLESS.search(code):
        return False
    return not (EXEMPT.search(text) or SANCTIONED.search(code))


def scan(root):
    bad = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in rumble_guard.SKIP_DIRS]
        for name in filenames:
            if not name.endswith(rumble_guard.SUFFIXES):
                continue
            path = os.path.join(dirpath, name)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
            except OSError:
                continue
            if offends(name, text):
                bad.append(os.path.relpath(path, root))
    return sorted(bad)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..")
    bad = scan(os.path.abspath(root))
    if not bad:
        print("fence (xvfb_guard): clean")
        return 0
    sys.stderr.write(
        "FENCE (xvfb_guard): these unattended launchers can open a game "
        "window on the session's display (the user's desktop):\n")
    for path in bad:
        sys.stderr.write(f"  {path}\n")
    sys.stderr.write(
        "\nFix: source scripts/lib/xvfb.sh and launch under \"${OP_XVFB[@]}\" "
        "(op_xvfb_prefix TAG WxH), or in Python prepend hostenv.xvfb_prefix's "
        "argv. A file the helper does not fit carries one marker: "
        "`xvfb-guard: windows-only`, `xvfb-guard: private-display`, "
        "`xvfb-guard: intentional` or `xvfb-guard: no-launch`.\n")
    return 65


if __name__ == "__main__":
    sys.exit(main())
