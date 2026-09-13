#!/usr/bin/env python3
"""Fence: a sub-tick scene producer reads the PRESENT clock, never the wall clock.

The native scene is built for a display instant, and every animation channel
in it (camera bracket, moby position/rotation, particle phase, terrain scroll,
the 2D-UI overlay carry, the fade ramp) derives its interpolation alpha as
(present instant - tick stamp) / period. The present instant is
openpete_interp_present_now(): the virtual present clock the host loop pins
to the display slot being produced, and to the tick-grid midpoint under
deterministic capture. The tick stamp is openpete_interp_tick_stamp(): the
PLL-gridded VBL instant the interp rings roll on. Neither is op_now().

Reading op_now() instead has now produced the same defect twice, from two
different directions:

  - Deterministic capture (2026-07): op_now() is not reproducible run-to-run,
    so the intermediate-leg golden flaked ~1/12 on every wall-clock channel.
    Fixed for the deterministic branch only; the live path kept op_now().
  - Eager production (2026-08/09): the whole span is extracted right after
    the tick, so "now" at extraction sits within a millisecond of the tick
    stamp for every intermediate. The one channel still on op_now() (the
    2D-UI overlay carry) read alpha ~ 0 on every present, and the title
    logo's drop-in stepped at the tick rate while the 3D scene glided.

Both times the mechanism was invisible to every gate: the oracle sees no
scene, headless never renders intermediates, and the golden's intermediate
leg runs the deterministic branch. So the rule becomes mechanical:

  In the scene-producer files below, a wall-clock read (op_now(),
  SDL_GetTicks*, SDL_GetPerformanceCounter, clock_gettime) is refused
  unless its line carries the marker `clock-guard: wall` — a written-down
  decision that this read is about wall time (a timing span, an input-rate
  integration, a diagnostic that prints both clocks), not an animation
  instant.

Comment text is ignored (an explanation may name op_now() freely); only
code is scanned.

Run: python3 scripts/fences/clock_guard.py [root]
Exit: 0 clean, 66 a scene producer reads the wall clock without saying so.

rumble-guard: no-launch, wslenv-guard: no-launch
"""
import fnmatch
import os
import re
import sys

WALL_CLOCK = re.compile(
    r"\b(op_now|SDL_GetTicks|SDL_GetTicksNS|SDL_GetPerformanceCounter|"
    r"clock_gettime)\s*\(")
MARKER = "clock-guard: wall"

# The files that build the native scene for a display instant, plus the
# interp core whose fallbacks define the clocks themselves.
GUARDED = (
    "games/spyro-1/spyro_native_reextract.c",
    "games/spyro-1/features/*_native.c",
    "games/spyro-1/features/freecam.c",
    "games/common/freecam_game.c",
    "games/spyro-1/features/*_interp*.c",
    "core/runtime/native_render.c",
    "core/runtime/openpete_interp.c",
    "core/runtime/openpete_interp_objs.c",
    "core/mods/mod_draw.c",
)


def guarded(rel):
    return any(fnmatch.fnmatch(rel, pat) for pat in GUARDED)


def strip_comments(text):
    """Blank out C comments, preserving newlines so line numbers survive."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if c == "\n" else " " for c in text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def offends(text):
    """Returns [(line_no, source)] for unmarked wall-clock reads in `text`."""
    raw_lines = text.splitlines()
    code_lines = strip_comments(text).splitlines()
    bad = []
    for i, code in enumerate(code_lines):
        if WALL_CLOCK.search(code) and MARKER not in raw_lines[i]:
            bad.append((i + 1, raw_lines[i].strip()))
    return bad


def scan(root):
    bad = []
    for pat in GUARDED:
        d = os.path.join(root, os.path.dirname(pat))
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            rel = os.path.join(os.path.dirname(pat), name).replace(os.sep, "/")
            if not guarded(rel):
                continue
            try:
                text = open(os.path.join(root, rel), encoding="utf-8",
                            errors="replace").read()
            except OSError:
                continue
            for line, src in offends(text):
                bad.append((rel, line, src))
    return sorted(set(bad))


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(__file__), "..", "..")
    root = os.path.abspath(root)
    bad = scan(root)
    if not bad:
        return 0
    print("clock_guard: a scene producer reads the WALL clock; the scene is "
          "built for a present instant:", file=sys.stderr)
    for rel, line, src in bad:
        print(f"  {rel}:{line}\n      {src}", file=sys.stderr)
    print("\nAn interpolation alpha is (openpete_interp_present_now() - "
          "openpete_interp_tick_stamp()) / period. A read that is genuinely "
          "about wall time (a timing span, input-rate integration, a "
          "diagnostic printing both clocks) says so on its line with "
          f"`{MARKER}`.", file=sys.stderr)
    return 66


if __name__ == "__main__":
    sys.exit(main())
