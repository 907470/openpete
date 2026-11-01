#!/usr/bin/env python3
"""check_launcher.py - the launcher's checks, one case per ctest test of the
label `launcher`.

Every case runs the linux-release binary windowed under its own virtual X
display (xvfb-run -a, SDL_VIDEODRIVER=x11, WAYLAND_DISPLAY unset) on
lavapipe, with the minimal window manager tests/harness/xvfb_wm.c
(openpete_xvfb_wm, built beside the binary) running on it so fullscreen and
maximize requests take effect, and a scratch data root of its own (XDG_DATA_HOME,
XDG_STATE_HOME and XDG_CONFIG_HOME under the case's output directory), so
cases never share a library. Every launch passes --no-config --no-card
--vibration 0 --input-lock --mute 1. Each case deletes its output directory
before it runs and asserts the files it reads exist after.

Cases:
  manifest  The element manifest writer (OPENPETE_LAUNCHER_MANIFEST): at each
            reference window preset (1280x800, 1920x1080, 2560x1080,
            1024x768, 2560x1440, 3840x2160) an empty-library launch draws the drop
            screen (the game page with every box empty), the writer writes
            drop.json, and the file names the window size, holds the title
            and each linked game's empty box (fill, disc, name, frame), and
            every rect lies inside the window. The reference layout manifest
            (tests/launcher/layout-reference.json) parses, covers the six
            presets, and carries no field outside the id/kind/rect/font/px/
            cell set (no game-derived field).
  layout    The drop screen's elements against the reference layout at each
            reference preset: for every id in LAYOUT_IDS, the launcher's rect
            and the reference's on the empty-library page (the reference's
            first step). A text item's left edge and height match exactly and
            its width within TEXT_WIDTH_BOUND; any other rect within 1 px.
            The ids compared are those the launcher draws as the reference
            does: the pixel title and the game page's empty boxes (their
            fill, disc outline, name lines and frame; compare_page). The
            drop screen's shot
            (OPENPETE_LAUNCHER_SHOT) is read too: the title's orange ink lies
            inside its rect, no pixel in the rect is a blend of the orange
            and the background, and every horizontal ink run is a multiple
            of the reference's cell (whole-pixel cells). A --game spyro-1
            launch's drop screen draws no subtitle.
  page      The game page with library discs, at 1280x800 and 1024x768 (a
            box row narrower than the window, and one the width shrinks):
            a library of Spyro 1's disc, its asset cache written first by a
            headless launch, against the reference's page with Spyro 1 in
            the library (Spyro 2's and 3's empty boxes, every frame, and Spyro
            1's title logo by its rule: centred on the box, a quarter of its
            height below the frame's top edge, its rect inside the logo's
            size caps); a library of all three discs against the page with
            all three in it (frames and logos). The logo's size is the disc's
            logo region's, which the reference's stand-in logos do not share,
            so it is checked by the rule only. A disc dropped on the empty
            page (Spyro 2's) returns to the page with its box focused: its
            frame at the reference's focused Spyro 2 rect. An executable
            whose first-boot inputs are staged for Spyro 1 and Spyro 3 only
            (a hard link of a -launcher build's binary beside a recomp tree
            holding those two manifests) draws two boxes, at the reference's
            two-box page; SKIP for that row without a first-boot build.
  handoff   The last launcher present before the game's first present is
            black on every path into the game (OPENPETE_LAUNCHER_TRACE: every
            present from the launcher window's first through the game's
            first, each read back and its pixels other than (0,0,0)
            counted). Paths, each one run of Spyro 1 to --frame-limit 30
            over a library seeded with one entry whose disc.bin links the
            repository's disc image: the selector's pick with the launcher
            window at the game's size and at 1280x800 (a resize at the
            adoption); after the mod compile (a copy of mods/chain-duo-a in
            a scratch --mods-dir); after the shader compile's progress screen
            (OPENPETE_PRECOMPILE_FORCE=1, OPENPETE_PRECOMPILE_PANEL_MS=0);
            into --fullscreen 1; and a --disc launch without the launcher
            with the shader compile's progress screen. Each run must log the
            game's first present, a launcher present that drew (engagement),
            and a last launcher present with no pixel other than (0,0,0);
            the mod and shader runs must log their step. The --fullscreen 1
            run's game first presents at the screen's size, no launcher
            present is at the game's windowed size 640x448, and the window
            manager kept 640x448 as the geometry fullscreen returns to. A
            launcher-less --disc --fullscreen 1 launch keeps the present
            sequence recorded before the straight-to-fullscreen hand-off.
  window-fit
            The launcher window's fit to its display, each a 1280x800
            request (--launcher-window) on an empty library: on a display
            whose usable bounds hold it (stubbed 1920x1080, usable
            1920x1040; and the virtual display itself, unstubbed) it opens
            as requested; on a 1280x800 display with a 44 px panel it opens
            maximized at the requested size; on a 1366x768 display (usable
            1366x728) it is clamped to 90% of the usable bounds at 16:10,
            1048x655; with GAMESCOPE_WAYLAND_DISPLAY set it opens at the
            output size (stubbed 1920x1080). Each run's fit line and the size the device was
            created at (and its maximized state) are asserted, and so is the
            drop screen's manifest window size, the framebuffer the screens
            draw at. The display bounds come from
            OPENPETE_LAUNCHER_DISPLAY_STUB, and in the unstubbed rows from
            the virtual display and the window manager's work area: a
            1280x800 screen with a 44 px panel opens maximized and presents
            at 1280x756, a 1366x768 screen with a 40 px panel is clamped.
  window-default
            The launcher window's first size on an empty library: 1280x800
            with no configuration; --launcher-window's size over everything;
            --window's size (an explicit game window size) without
            --launcher-window, and the same size from the window key of a
            --config file; --window maximized opens it maximized at 1280x800;
            a window value under [game.spyro-1] does not apply (1280x800); a stubbed display content scale
            (OPENPETE_LAUNCHER_CONTENT_SCALE 1.5, 2) logs its scale and leaves 1280x800 off Windows. The size and maximized state the
            launcher device was created at are asserted.
  font      The body font: an empty-library launch logs the body font as
            Fredoka Medium with ProggyClean the default font, and the game
            page's name lines match the reference's Fredoka widths
            (compare_page) while the title keeps its whole-pixel cells; no font file sits beside the
            executable; after a pick hands the window to the game, the dev
            overlay logs ProggyClean as its font; the licence staging every
            package runs (scripts/dist_compliance.py licences --scope all)
            writes licences/Fredoka-OFL.txt identical to
            packaging/licences/fredoka-ofl.txt; packaging/README-dist.md.in
            credits Fredoka.
  density   The full-density framebuffer on a scaled display: each run is on
            a headless Wayland compositor (weston, or the command
            OPENPETE_TEST_WESTON names, which takes weston's arguments)
            whose one output is 2560x1600 window coordinates at scale 2. A
            --launcher-window 1280x800 launch on an empty library opens as
            requested, presents at 2560x1600 pixels, writes the drop
            screen's manifest at 1280x800 window coordinates, and draws the
            pixel title in whole cells of round(28 x 2.4 x s x 2 / 13)
            framebuffer pixels (s the layout scale); a --disc launch of
            Spyro 1 with --window 640x448 brings the game's window up at
            640x448 window coordinates and 1280x896 pixels.
  reap      A run that outlives its timeout leaves no process behind: the binary idling on the
            empty library's drop screen (no scripted quit) under a 15 s timeout, and a stand-in
            binary that ignores SIGTERM (the SIGKILL path), each through launch(); and a harness
            process SIGKILLed (as a ctest timeout does) while its run of the stand-in is in
            flight, whose guardian must end the run within 15 s. Every process
            whose environment names the run's data root is recorded while it runs (engagement:
            the binary and the window manager at least) and none may be alive after launch()
            returns. The control kills only the xvfb-run wrapper on its timeout, as a plain
            subprocess timeout does, and must find the binary still running (the instrument sees
            a leak); the control's session is then ended.
  calibrate Not a ctest case: compares the drop screen with a reference
            manifest of the same screen (--calibration PATH: per preset, a
            "drop" list of id/kind/rect items), printing each element's rect
            deltas and the worst per element.

Usage: tests/harness/check_launcher.py CASE [--build-dir DIR] [--out DIR]
           [--calibration PATH]
Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (binary or xvfb-run missing).
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REFERENCE = os.path.join(ROOT, "tests", "launcher", "layout-reference.json")
PRESETS = ["1280x800", "1920x1080", "2560x1080", "1024x768", "2560x1440", "3840x2160"]
REFERENCE_KEYS = {"id", "kind", "rect", "font", "px", "cell"}
# The virtual display: larger than the largest preset, so no request is cut.
SCREEN = "4096x2304"
DISC_BIN = os.path.join(ROOT, "games", "spyro-1", "spyro-1", "build", "SCUS-94228.bin")
DISC_CUE = os.path.join(ROOT, "games", "spyro-1", "spyro-1", "build", "SCUS-94228.cue")
# The ids the launcher draws as the reference layout does, compared by the layout case.
LAYOUT_IDS = ["title"]
# The game page's name lines (Fredoka, laid out by the reference with the browser's measureText and by the
# launcher with imgui's stb_truetype path) against the reference: the top and height exact, the width
# within NAME_WIDTH_BOUND px and the left edge (the line is centred) within half of it. Calibrated on the
# five name lines at the six presets: widths differ by -2.16 to +4.32 px, not monotonic in the size (the
# browser applies the font's kerning, imgui does not; both round the width up).
NAME_WIDTH_BOUND = 4.5
# The logo's share of its height below the frame's top edge.
LOGO_DROP = 0.25
# A text item's width against the reference's, in pixels. Calibrated on the drop screen's seven strings
# (ProggyClean at 8, 14 and 28 layout px) at the six presets, drawn by the launcher and by the reference
# layout's text model with imgui's whole-pixel font sizes: widths differ by at most 1 px (imgui's
# CalcTextSize at some sizes and strings is one pixel wider or narrower than whole-pixel advances
# times the length), heights not at all; a centred string's left edge moves with that pixel. Text in
# whole-pixel cells (the pixel title) matches exactly.
TEXT_WIDTH_BOUND = 1.0
GAMES = ("spyro-1", "spyro-2", "spyro-3")
DISCS = {"spyro-1": "SCUS-94228", "spyro-2": "SCUS-94425", "spyro-3": "SCUS-94467"}
# The window manager every windowed run starts on its display; main sets it from --build-dir, and None
# looks beside the binary (a binary outside the build directory then needs its own copy).
WM = None
DROP_IDS = {"title"} | {f"box.{g}.{p}" for g in GAMES for p in ("fill", "disc", "name.0", "frame")}


class Skip(Exception):
    """A precondition the machine lacks: the case exits 64."""


def case_dir(out, name):
    """The case's output directory, emptied: stale outputs give false verdicts."""
    d = os.path.join(out, name)
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    return d


# The sessions of the runs in flight, by session id: the reaper ends each one's processes when the
# harness is signalled (ctest's timeout sends SIGTERM).
_SESSIONS = set()
# Seconds a session's processes get between SIGTERM and SIGKILL.
REAP_GRACE_S = 3.0


def session_members(sid):
    """The pids of the live (non-zombie) processes in a session."""
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/stat") as f:
                fields = f.read().rsplit(")", 1)[1].split()
        except (OSError, IndexError):
            continue
        # After the command name: state, ppid, pgrp, session.
        if len(fields) > 3 and fields[0] != "Z" and int(fields[3]) == sid:
            out.append(int(pid))
    return out


def reap_session(sid, grace=REAP_GRACE_S):
    """End every process of a session: SIGTERM, then SIGKILL to what remains after `grace` seconds.

    The session, not the process group: the launcher starts each compiler driver in a group of its own,
    and those groups stay in the session. Returns the number of processes that were still running."""
    found = session_members(sid)
    for sig, wait in ((signal.SIGTERM, grace), (signal.SIGKILL, 2.0)):
        live = session_members(sid)
        for pid in live:
            try:
                os.kill(pid, sig)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + wait
        while live and time.monotonic() < deadline:
            time.sleep(0.05)
            live = session_members(sid)
        if not live:
            break
    return len(found)


# The write end of the guardian's pipe, once install_reaper has started it.
_GUARD = None


def track_session(sid):
    """Record a run's session as in flight, here and with the guardian."""
    _SESSIONS.add(sid)
    if _GUARD is not None:
        os.write(_GUARD, b"+%d\n" % sid)


def untrack_session(sid):
    """Record a run's session as ended."""
    _SESSIONS.discard(sid)
    if _GUARD is not None:
        os.write(_GUARD, b"-%d\n" % sid)


def _reap_all(signum, _frame):
    """Signal handler: end every run's session, then exit with the shell's code for the signal."""
    for sid in list(_SESSIONS):
        reap_session(sid, grace=1.0)
    os._exit(128 + signum)


def _guardian(r):
    """The guardian's body: follow the sessions the harness reports on the pipe and, once the pipe
    closes (the harness exited, however it died), end every session still recorded."""
    live, buf = set(), b""
    while True:
        chunk = os.read(r, 4096)
        if not chunk:
            break
        buf += chunk
        *lines, buf = buf.split(b"\n")
        for ln in lines:
            if ln[:1] == b"+":
                live.add(int(ln[1:]))
            elif ln[:1] == b"-":
                live.discard(int(ln[1:]))
    for sid in live:
        reap_session(sid, grace=1.0)


def install_reaper():
    """End every run in flight when the harness ends without ending its runs.

    A signal the harness can catch (SIGTERM, SIGINT, SIGHUP) ends the runs from its handler. A SIGKILL
    (a ctest timeout) cannot be caught, so a guardian process, forked in a session of its own before
    any thread starts, holds the read end of a pipe the harness reports each run's session on; when
    the harness dies the pipe closes and the guardian ends the sessions still in flight."""
    global _GUARD
    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(s, _reap_all)
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(w)
        os.setsid()
        for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(s, signal.SIG_IGN)
        try:
            _guardian(r)
        finally:
            os._exit(0)
    os.close(r)
    _GUARD = w


def run_session(cmd, env, out, timeout, stdout=None):
    """Run a command in a session of its own and wait for it; returns its exit code, -9 on timeout.

    Its stderr goes to `out`, and its stdout too unless `stdout` names another destination.

    On return every process left in the session (a hung binary under an xvfb-run that was killed, the
    window manager, a compiler driver) is ended, so no run outlives its case."""
    p = subprocess.Popen(cmd, env=env, stdout=out if stdout is None else stdout,
                         stderr=subprocess.STDOUT if stdout is None else out, start_new_session=True)
    track_session(p.pid)
    try:
        rc = p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        rc = -9
    finally:
        left = reap_session(p.pid)
        p.wait()
        untrack_session(p.pid)
    if rc == -9:
        print(f"[launcher] timeout after {timeout} s: {cmd[0]} and {left} process(es) of its session ended")
    return rc


def launch(binary, d, tag, args, env=None, timeout=120, screen=SCREEN, panel=0):
    """One windowed run under a fresh virtual display and the window manager; returns (rc, log text).

    The window manager reserves a bottom panel of `panel` pixels in the work area. The run starts once
    the manager has advertised itself (its ready file), so the window maps under it."""
    if not shutil.which("xvfb-run"):
        raise Skip("xvfb-run not found")
    wm = WM or os.path.join(os.path.dirname(binary), "openpete_xvfb_wm")
    if not os.access(wm, os.X_OK):
        raise Skip(f"{wm} missing")
    # The caller's OPENPETE_* knobs (OPENPETE_BACKGROUND, for one) would change the paths under test.
    e = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY" and not k.startswith("OPENPETE_")}
    e.update({"SDL_VIDEODRIVER": "x11", "OPENPETE_MSGBOX_SKIP": "1",
              "XDG_DATA_HOME": os.path.join(d, "data"), "XDG_STATE_HOME": os.path.join(d, "state"),
              "XDG_CONFIG_HOME": os.path.join(d, "config")})
    e.update(env or {})
    ready = os.path.join(d, tag + ".wm-ready")
    config = [] if "--config" in args else ["--no-config"]
    cmd = ["xvfb-run", "-a", "-s", f"-screen 0 {screen}x24", "sh", "-c",
           '"$0" --panel "$1" --ready "$2" & until [ -e "$2" ]; do sleep 0.05; done; shift 2; exec "$@"',
           wm, str(panel), ready, binary] + config + ["--no-card", "--vibration", "0", "--input-lock",
                                                       "--mute", "1"] + args
    log = os.path.join(d, tag + ".log")
    with open(log, "w") as f:
        rc = run_session(cmd, e, f, timeout)
    with open(log, errors="replace") as f:
        return rc, f.read()


def read_json(path):
    """The parsed file, or None when it does not exist."""
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return json.load(f)


def drop_manifest(binary, d, preset, extra_env=None):
    """Launch at a preset with an empty library and return the drop screen's manifest, or None."""
    base = os.path.join(d, "m-" + preset)
    env = {"OPENPETE_LAUNCHER_MANIFEST": base, "OPENPETE_LAUNCHER_SHOT": base,
           "OPENPETE_LAUNCHER_INPUT": "1500:quit"}
    env.update(extra_env or {})
    rc, _ = launch(binary, d, "drop-" + preset, ["--launcher-window", preset], env)
    return read_json(base + ".drop.json"), rc


def seed_library(d, games=("spyro-1",)):
    """A library holding one entry per game whose disc.bin links the repository's image (no copy)."""
    for i, g in enumerate(games):
        b = os.path.join(ROOT, "games", g, g, "build", DISCS[g] + ".bin")
        if not os.path.isfile(b):
            raise Skip(f"{b} missing")
        e = os.path.join(d, "data", "openpete", "library", "00000000launcher" + ("" if g == "spyro-1" else g))
        os.makedirs(e)
        os.symlink(b, os.path.join(e, "disc.bin"))
        with open(os.path.join(e, "disc.cue"), "w") as f:
            f.write('FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
        with open(os.path.join(e, "meta.toml"), "w") as f:
            f.write('label = "%s (launcher test)"\nsource = "%s.bin"\n'
                    'sha256 = "%s"\nlast_played = %d\ngame = "%s"\n' % (g, DISCS[g], "0" * 64, i + 1, g))


def trace_presents(log):
    """The OPENPETE_LAUNCHER_TRACE lines: (source, window (w, h), nonzero count) in order."""
    out = []
    for m in re.finditer(r"lxt present #\d+ (\w+) window (\d+)x(\d+) extent \d+x\d+ nonzero (-?\d+)", log):
        out.append((m.group(1), (int(m.group(2)), int(m.group(3))), int(m.group(4))))
    return out


def handoff_verdict(log):
    """Problems with one run's hand-off present sequence, and its engagement count."""
    ps = trace_presents(log)
    game = [i for i, p in enumerate(ps) if p[0] == "game"]
    if not game:
        return ["no game present traced"], 0
    before = [p for p in ps[:game[0]] if p[0] == "launcher"]
    drew = sum(1 for p in before if p[2] > 0)
    if not before:
        return ["no launcher present before the game's first"], 0
    bad = []
    if before[-1][2] != 0:
        bad.append(f"last launcher present {before[-1][1]} has {before[-1][2]} pixels other than (0,0,0)")
    return bad, drew


def run_handoff(a, d, tag, args, env=None, need=None):
    """One hand-off run: its problems, its engagement and its present sequence."""
    run = os.path.join(d, tag)
    os.makedirs(run)
    seed_library(run)
    e = {"OPENPETE_LAUNCHER_TRACE": "1", "OPENPETE_LAUNCHER_SELECT": "0"}
    e.update(env or {})
    rc, log = launch(a.binary, run, "run", args + ["--frame-limit", "30"], e, timeout=300)
    bad, drew = handoff_verdict(log)
    if rc != 0:
        bad.append(f"exit {rc}")
    if need and not re.search(need, log):
        bad.append(f"the step did not run (no line matching {need!r})")
    return bad, drew, trace_presents(log)


GAME_SIZE = (640, 448)   # Spyro 1's default game window
SCREEN_SIZE = tuple(map(int, SCREEN.split("x")))


def fullscreen_verdict(ps, log_path):
    """Problems with a launch into a fullscreen game: the game's first present is at the screen's size,
    no present before it is at the game's windowed size, and the window manager kept the game's size as
    the geometry fullscreen returns to."""
    bad = []
    game = [p for p in ps if p[0] == "game"]
    if not game or game[0][1] != SCREEN_SIZE:
        bad.append(f"the game's first present is {game[0][1] if game else None}, not fullscreen {SCREEN_SIZE}")
    at_game = [p for p in ps if p[0] == "launcher" and p[1] == GAME_SIZE]
    if at_game:
        bad.append(f"{len(at_game)} launcher presents at the game's windowed size {GAME_SIZE}")
    with open(log_path, errors="replace") as f:
        saved = re.findall(r"xvfb_wm: window \w+ fullscreen, returns to (\d+)x(\d+)", f.read())
    if [tuple(map(int, s)) for s in saved[:1]] != [GAME_SIZE]:
        bad.append(f"fullscreen returns to {saved[:1]}, not the game's size {GAME_SIZE}")
    return bad


# The present sequence of a launcher-less `--disc --fullscreen 1` launch, recorded on the build before
# the launcher's straight-to-fullscreen hand-off (two runs, identical): the black frame at the game's
# windowed size, the black frame at the fullscreen size, the game. The hand-off change keeps it.
DISC_FULLSCREEN = [("launcher", GAME_SIZE, False), ("launcher", SCREEN_SIZE, False), ("game", SCREEN_SIZE, True)]


def disc_fullscreen_sequence(a, d):
    """The launcher-less --disc --fullscreen 1 launch's present sequence: (source, size, drew) per present."""
    run = os.path.join(d, "disc-fullscreen")
    os.makedirs(run)
    rc, log = launch(a.binary, run, "run", ["--disc", DISC_CUE, "--game", "spyro-1", "--fullscreen", "1",
                                            "--frame-limit", "30"], {"OPENPETE_LAUNCHER_TRACE": "1"},
                     timeout=300)
    return rc, [(s, wh, n > 0) for s, wh, n in trace_presents(log)]


def copy_mod(d):
    """A scratch --mods-dir holding a copy of mods/chain-duo-a, enabled."""
    m = os.path.join(d, "scratch-mods")
    shutil.copytree(os.path.join(ROOT, "mods", "chain-duo-a"), os.path.join(m, "chain-duo-a"),
                    ignore=shutil.ignore_patterns(".build"))
    with open(os.path.join(m, "enabled.txt"), "w") as f:
        f.write("chain-duo-a\n")
    return m


def case_handoff(a):
    """The black last launcher present on each path into the game."""
    d = case_dir(a.out, "handoff")
    shaders = {"OPENPETE_PRECOMPILE_FORCE": "1", "OPENPETE_PRECOMPILE_PANEL_MS": "0"}
    shader_line = r"progress screen [1-9]\d* present"
    # (tag, arguments, environment, the line the step logs, launcher presents that must draw)
    runs = [
        ("page-same-size", ["--launcher-window", "640x448"], None, None, 1),
        ("page-resize", ["--launcher-window", "1280x800"], None, None, 1),
        ("mods", ["--launcher-window", "1280x800", "--mods-dir", copy_mod(d)], None,
         r"mods: 1 mod stale - compiling before boot", 2),
        ("shaders", ["--launcher-window", "1280x800"], shaders, shader_line, 2),
        ("fullscreen", ["--launcher-window", "1280x800", "--fullscreen", "1"], None, None, 1),
        ("disc-no-launcher", ["--disc", DISC_CUE, "--game", "spyro-1"], shaders, shader_line, 1),
    ]
    if not os.path.isfile(os.path.join(a.build_dir, "sdk", "openpete_mod_api.h")):
        print(f"[launcher] FAIL: no mod SDK in {a.build_dir}/sdk (the ALL target stages it); "
              "the mod path cannot run")
        return 1
    fails = 0
    for tag, args, env, need, min_drew in runs:
        bad, drew, ps = run_handoff(a, d, tag, args, env, need)
        if not bad and drew < min_drew:
            bad = [f"VACUOUS: {drew} launcher presents drew, {min_drew} expected"]
        if tag == "fullscreen":
            bad += fullscreen_verdict(ps, os.path.join(d, tag, "run.log"))
        last = [p for p in ps if p[0] == "launcher"][-1:] or [None]
        print(f"[launcher] {tag}: {len(ps)} presents traced, {drew} launcher presents drew, "
              f"last launcher present {last[0]}: {'FAIL ' + '; '.join(bad) if bad else 'PASS'}")
        fails += bool(bad)
    rc, seq = disc_fullscreen_sequence(a, d)
    ok = rc == 0 and seq == DISC_FULLSCREEN
    print(f"[launcher] disc-fullscreen: sequence {seq}: {'PASS' if ok else f'FAIL (rc {rc}), expected {DISC_FULLSCREEN}'}")
    fails += not ok
    total = len(runs) + 1
    print(f"[launcher] handoff: {'FAIL' if fails else 'PASS'} ({total - fails} of {total} paths)")
    return 1 if fails else 0


def case_window_fit(a):
    """The launcher window's fit rule against stubbed display bounds."""
    d = case_dir(a.out, "window-fit")
    # (tag, stub or (screen, panel) of the real display, extra env, expected decision, expected size,
    # maximized, the size the drop screen draws at when maximized)
    rows = [
        ("fits", "1920x1080,1920x1040", {}, "as requested", (1280, 800), False, None),
        ("fits-unstubbed", (SCREEN, 0), {}, "as requested", (1280, 800), False, None),
        ("panel", "1280x800,1280x756", {}, "maximized", (1280, 800), True, None),
        ("panel-unstubbed", ("1280x800", 44), {}, "maximized", (1280, 800), True, [1280, 756]),
        ("small", "1366x768,1366x728", {}, "clamped to 90% of the usable bounds", (1048, 655), False, None),
        ("small-unstubbed", ("1366x768", 40), {}, "clamped to 90% of the usable bounds", (1048, 655), False,
         None),
        # The outer size decides: the client fits the usable height, the client plus a 38 px title bar
        # fits only the full height (a 1080p screen at 125% under a taskbar, at 1280x800 scale).
        ("titlebar-none", "1280x840,1280x810", {}, "as requested", (1280, 800), False, None),
        ("titlebar", "1280x840,1280x810", {"OPENPETE_LAUNCHER_DECORATION_STUB": "0x38"}, "maximized",
         (1280, 800), True, None),
        ("titlebar-small", "1366x768,1366x728", {"OPENPETE_LAUNCHER_DECORATION_STUB": "16x39"},
         "clamped to 90% of the usable bounds", (986, 616), False, None),
        ("gamescope", "1920x1080,1920x1040", {"GAMESCOPE_WAYLAND_DISPLAY": "gamescope-0"},
         "at the gamescope output size", (1920, 1080), False, None),
    ]
    bad = []
    for tag, where, env, how, size, maxed, drawn in rows:
        e = dict(env, OPENPETE_LAUNCHER_INPUT="1500:quit", OPENPETE_LAUNCHER_TRACE="1",
                 OPENPETE_LAUNCHER_MANIFEST=os.path.join(d, "m-" + tag))
        screen, panel = (SCREEN, 0) if isinstance(where, str) else where
        if isinstance(where, str):
            e["OPENPETE_LAUNCHER_DISPLAY_STUB"] = where
        rc, log = launch(a.binary, d, tag, ["--launcher-window", "1280x800"], e, screen=screen, panel=panel)
        fit = re.search(r"window fit: request 1280x800, display (\d+x\d+), usable (\d+x\d+): opens (.+?) \((\d+)x(\d+)\)", log)
        dev = re.search(r"launcher device \S+ created on window \S+ \((\d+)x(\d+)(, maximized)?", log)
        m = read_json(os.path.join(d, "m-" + tag + ".drop.json"))
        got = None
        if not fit or not dev:
            bad.append(f"{tag}: no fit line or device line (rc {rc}, log {d}/{tag}.log)")
        else:
            got = (fit.group(3), (int(fit.group(4)), int(fit.group(5))), (int(dev.group(1)), int(dev.group(2))),
                   bool(dev.group(3)))
            if got != (how, size, size, maxed):
                bad.append(f"{tag}: opened {got}, expected {(how, size, size, maxed)}")
            if m is None or (not maxed and tuple(m["window"]) != size):
                bad.append(f"{tag}: drop screen drawn at {m and m['window']}, expected {size}")
            sizes = {p[1] for p in trace_presents(log) if p[0] == "launcher"}
            if drawn and tuple(drawn) not in sizes:
                bad.append(f"{tag}: no launcher present at the maximized size {drawn} (presented at {sizes})")
        print(f"[launcher] {tag}: display {fit and fit.group(1)} usable {fit and fit.group(2)}: {got}")
    print(f"[launcher] window-fit: {'FAIL' if bad else 'PASS'} ({len(rows) - len(bad)} of {len(rows)} rows)")
    for b in bad:
        print("[launcher] FAIL", b)
    return 1 if bad else 0


# The scaled display of the density case: its output in window coordinates and its scale.
SCALED_OUTPUT = (2560, 1600)
SCALED_SCALE = 2


def launch_scaled(binary, d, tag, args, env=None, timeout=180):
    """One windowed run on a fresh headless Wayland compositor whose output is SCALED_OUTPUT window
    coordinates at SCALED_SCALE; returns (rc, log text).

    The compositor's runtime directory is a short temporary one, since a Wayland socket path is at most
    107 bytes; the run starts once the socket exists."""
    weston = os.environ.get("OPENPETE_TEST_WESTON") or shutil.which("weston")
    if not weston:
        raise Skip("weston not found (OPENPETE_TEST_WESTON names a substitute)")
    rt = tempfile.mkdtemp(prefix="opwl-")
    os.chmod(rt, 0o700)
    sock = "wl-" + tag
    e = {k: v for k, v in os.environ.items()
         if k not in ("WAYLAND_DISPLAY", "DISPLAY") and not k.startswith("OPENPETE_")}
    e["XDG_RUNTIME_DIR"] = rt
    wlog = open(os.path.join(d, tag + ".weston.log"), "w")
    comp = subprocess.Popen([weston, "--backend=headless-backend.so", f"--width={SCALED_OUTPUT[0]}",
                             f"--height={SCALED_OUTPUT[1]}", f"--scale={SCALED_SCALE}",
                             f"--socket={sock}", "--idle-time=0"],
                            env=e, stdout=wlog, stderr=subprocess.STDOUT, start_new_session=True)
    track_session(comp.pid)
    try:
        deadline = time.monotonic() + 15
        while not os.path.exists(os.path.join(rt, sock)):
            if comp.poll() is not None or time.monotonic() > deadline:
                raise Skip(f"the compositor did not start (log {d}/{tag}.weston.log)")
            time.sleep(0.05)
        e.update({"SDL_VIDEODRIVER": "wayland", "WAYLAND_DISPLAY": sock, "OPENPETE_MSGBOX_SKIP": "1",
                  "XDG_DATA_HOME": os.path.join(d, "data"), "XDG_STATE_HOME": os.path.join(d, "state"),
                  "XDG_CONFIG_HOME": os.path.join(d, "config")})
        e.update(env or {})
        cmd = [binary, "--no-config", "--no-card", "--vibration", "0", "--input-lock", "--mute", "1"] + args
        log = os.path.join(d, tag + ".log")
        with open(log, "w") as f:
            rc = run_session(cmd, e, f, timeout)
        with open(log, errors="replace") as f:
            return rc, f.read()
    finally:
        reap_session(comp.pid)
        comp.wait()
        untrack_session(comp.pid)
        wlog.close()
        shutil.rmtree(rt, ignore_errors=True)


def case_density(a):
    """The launcher's and the game's framebuffer at full pixel density on a scaled display."""
    from PIL import Image
    d = case_dir(a.out, "density")
    bad, engaged = [], 0
    k = SCALED_SCALE
    # The launcher: window coordinates as requested, pixels at the scale, title cells in framebuffer pixels.
    base = os.path.join(d, "m-launcher")
    rc, log = launch_scaled(a.binary, d, "launcher", ["--launcher-window", "1280x800"],
                            {"OPENPETE_LAUNCHER_INPUT": "1500:quit", "OPENPETE_LAUNCHER_TRACE": "1",
                             "OPENPETE_LAUNCHER_MANIFEST": base, "OPENPETE_LAUNCHER_SHOT": base})
    fit = re.search(r"window fit: request 1280x800, display (\d+x\d+), usable \d+x\d+: opens (.+) \((\d+)x(\d+)\)",
                    log)
    if not fit or fit.group(2) != "as requested":
        bad.append(f"launcher: fit {fit and fit.groups()}, expected as requested (rc {rc}, log {d}/launcher.log)")
    sizes = {p[1] for p in trace_presents(log) if p[0] == "launcher"}
    if (1280 * k, 800 * k) not in sizes:
        bad.append(f"launcher: no present at {1280 * k}x{800 * k} pixels (presented at {sizes})")
    m = read_json(base + ".drop.json")
    shot = base + ".drop.png"
    title = m and next((it for it in m["items"] if it["id"] == "title"), None)
    if m is None or m["window"] != [1280, 800] or title is None or not os.path.isfile(shot):
        bad.append(f"launcher: drop manifest {m and m['window']} or title or shot missing")
    else:
        im_size = Image.open(shot).size
        sc = min(1280 / 640, 800 / 448)
        cell = max(1, round(28 * 2.4 * sc * k / 13))
        rect = [v * k for v in title["rect"]]
        ink, blend, box, outside, runs = title_ink(shot, rect)
        engaged += ink
        h = rect[3] - rect[1]
        print(f"[launcher] launcher: presents {sorted(sizes)}, shot {im_size}, title rect {title['rect']} "
              f"window coordinates, ink {ink} px, box {box}, {blend} blended, {outside} outside, "
              f"runs {sorted(runs)} (cell {cell})")
        if im_size != (1280 * k, 800 * k):
            bad.append(f"launcher: shot {im_size}")
        if abs(h - 13 * cell) > 1e-3:
            bad.append(f"launcher: title {h} framebuffer pixels tall, expected {13 * cell}")
        if ink == 0 or blend or outside or any(r % cell for r in runs):
            bad.append(f"launcher: title ink not in whole cells of {cell}: {ink} px, {blend} blended, "
                       f"{outside} outside, runs {sorted(runs)}")
    # The game: --window in window coordinates, the framebuffer at the scale.
    if not os.path.isfile(DISC_CUE):
        raise Skip(f"{DISC_CUE} missing")
    rc, log = launch_scaled(a.binary, d, "game", ["--disc", DISC_CUE, "--window", "640x448",
                                                  "--frame-limit", "30"], {"OPENPETE_WINDOW_STATE": "1"},
                            timeout=300)
    st = re.search(r"window state \(bring-up\):.* pixels=(\d+)x(\d+) window=(\d+)x(\d+)", log)
    got = st and ((int(st.group(3)), int(st.group(4))), (int(st.group(1)), int(st.group(2))))
    print(f"[launcher] game: window coordinates, pixels {got} (rc {rc})")
    if got != ((640, 448), (640 * k, 448 * k)):
        bad.append(f"game: {got}, expected {((640, 448), (640 * k, 448 * k))} (rc {rc}, log {d}/game.log)")
    else:
        engaged += 1
    for b in bad:
        print("[launcher] FAIL", b)
    if not bad and engaged == 0:
        print("[launcher] density: VACUOUS")
        return 3
    print(f"[launcher] density: {'FAIL' if bad else 'PASS'} (scale {k}, {engaged} title ink px + game row)")
    return 1 if bad else 0


def case_window_default(a):
    """The launcher window's default size and its fallback order."""
    d = case_dir(a.out, "window-default")
    scoped, keyed = os.path.join(d, "scoped.toml"), os.path.join(d, "keyed.toml")
    with open(scoped, "w") as f:
        f.write('[game.spyro-1]\nwindow = "1600x900"\n')
    with open(keyed, "w") as f:
        f.write('window = "1600x900"\n')
    rows = [
        ("default", [], (1280, 800), False),
        ("launcher-window", ["--launcher-window", "1024x640"], (1024, 640), False),
        ("window", ["--window", "1600x900"], (1600, 900), False),
        ("both", ["--launcher-window", "1024x640", "--window", "1600x900"], (1024, 640), False),
        ("window-maximized", ["--window", "maximized"], (1280, 800), True),
        ("config-key", ["--config", keyed], (1600, 900), False),
        ("game-scope", ["--config", scoped], (1280, 800), False),
    ]
    # A display content scale (stubbed) leaves the design size unscaled off Windows, where window
    # coordinates carry the scale (Wayland, macOS) or the size stays in pixels (X11).
    scaled = [("scale-1.5", "1.5"), ("scale-2", "2")]
    rows += [(tag, [], (1280, 800), False) for tag, _ in scaled]
    bad = []
    for tag, args, size, maxed in rows:
        scale = dict(scaled).get(tag)
        env = {"OPENPETE_LAUNCHER_INPUT": "1500:quit"}
        if scale:
            env["OPENPETE_LAUNCHER_CONTENT_SCALE"] = scale
        rc, log = launch(a.binary, d, tag, args, env)
        dev = re.search(r"launcher device \S+ created on window \S+ \((\d+)x(\d+)(, maximized)?", log)
        got = ((int(dev.group(1)), int(dev.group(2))), bool(dev.group(3))) if dev else None
        if got != (size, maxed):
            bad.append(f"{tag}: opened {got}, expected {(size, maxed)} (rc {rc}, log {d}/{tag}.log)")
        if scale:
            line = re.search(r"window scale: content scale ([\d.]+), design size 1280x800: requests (\d+x\d+) \((.+)\)", log)
            print(f"[launcher] {tag}: {line and line.group(0)}")
            if not line or float(line.group(1)) != float(scale) or line.group(2) != "1280x800":
                bad.append(f"{tag}: scale line {line and line.group(0)}, expected scale {scale} requesting 1280x800")
        print(f"[launcher] {tag}: {' '.join(args) or '(no setting)'}: opened {got}")
    for b in bad:
        print("[launcher] FAIL", b)
    print(f"[launcher] window-default: {'FAIL' if bad else 'PASS'} ({len(rows) - len(bad)} of {len(rows)} rows)")
    return 1 if bad else 0


def compare_items(ours, ref, ids):
    """Problems of the launcher's items against the reference's for the given ids: text left edge and
    height exact, width within TEXT_WIDTH_BOUND; any other kind within 1 px on every edge."""
    bad = []
    mine = {it["id"]: it for it in ours}
    theirs = {it["id"]: it for it in ref}
    for i in ids:
        a, b = mine.get(i), theirs.get(i)
        if a is None or b is None:
            bad.append(f"{i}: {'launcher' if a is None else 'reference'} has no such element")
            continue
        if a["kind"] != b["kind"]:
            bad.append(f"{i}: kind {a['kind']} != {b['kind']}")
        ra, rb = a["rect"], b["rect"]
        if b["kind"] == "text":
            ok = (abs(ra[0] - rb[0]) < 0.005 and abs((ra[3] - ra[1]) - (rb[3] - rb[1])) < 0.005 and
                  abs(ra[1] - rb[1]) < 0.005 and abs((ra[2] - ra[0]) - (rb[2] - rb[0])) <= TEXT_WIDTH_BOUND)
        else:
            ok = all(abs(ra[k] - rb[k]) <= 1.0 for k in range(4))
        if not ok:
            bad.append(f"{i}: launcher {ra} reference {rb}")
    return bad


def logo_problems(ours, game):
    """Problems of a box's title logo against its rule: centred on the frame, LOGO_DROP of its height below
    the frame's top edge, at most 0.92 of the frame's width wide and 0.42 of it tall, one cap met."""
    mine = {it["id"]: it["rect"] for it in ours}
    lo, fr = mine.get(f"box.{game}.logo"), mine.get(f"box.{game}.frame")
    if lo is None or fr is None:
        return [f"box.{game}.logo: {'no logo' if lo is None else 'no frame'} drawn"]
    w, h, fw = lo[2] - lo[0], lo[3] - lo[1], fr[2] - fr[0]
    bad = []
    if abs((lo[0] + lo[2]) - (fr[0] + fr[2])) / 2 > 0.5:
        bad.append(f"box.{game}.logo {lo} not centred on the frame {fr}")
    if abs(lo[1] + (1 - LOGO_DROP) * h - fr[1]) > 0.5:
        bad.append(f"box.{game}.logo {lo}: {(fr[1] - lo[1]) / h:.3f} of its height above the frame's top, "
                   f"expected {1 - LOGO_DROP}")
    if w > 0.92 * fw + 0.5 or h > 0.42 * fw + 0.5 or min(abs(w - 0.92 * fw), abs(h - 0.42 * fw)) > 0.5:
        bad.append(f"box.{game}.logo {lo}: {w:.2f}x{h:.2f} meets neither cap of a {fw:.2f} wide frame")
    return bad


def compare_page(ours, ref, ingested=(), log=None):
    """Problems of the launcher's game page against a reference step's, and the count of rects compared.

    Every frame within 1 px; an empty box's fill and disc within 1 px and its name lines' top and height
    exact, width within NAME_WIDTH_BOUND and left edge within half of it; an
    ingested box's logo by its rule (logo_problems); the box picture (not drawn yet) is skipped."""
    mine = {it["id"]: it for it in ours}
    bad, n = [], 0
    for it in ref:
        i = it["id"]
        if not i.startswith("box."):
            continue
        game, part = i.split(".", 2)[1:]
        if part == "picture":
            continue
        if part == "logo":
            bad += logo_problems(ours, game)
            n += 1
            continue
        a = mine.get(i)
        if a is None:
            bad.append(f"{i}: the launcher drew no such element")
            continue
        ra, rb = a["rect"], it["rect"]
        if part.startswith("name."):
            dw, dx = (ra[2] - ra[0]) - (rb[2] - rb[0]), ra[0] - rb[0]
            if log is not None:
                log.append((i, round(dw, 2), round(dx, 2)))
            ok = (abs(ra[1] - rb[1]) < 0.02 and abs((ra[3] - ra[1]) - (rb[3] - rb[1])) < 0.02 and
                  abs(dw) <= NAME_WIDTH_BOUND and abs(dx) <= NAME_WIDTH_BOUND / 2 + 0.01)
        else:
            ok = all(abs(ra[q] - rb[q]) <= 1.0 for q in range(4))
        n += 1
        if not ok:
            bad.append(f"{i}: launcher {ra} reference {rb}")
    return bad, n


BG = (24, 16, 40)
TITLE_ORANGE = (240, 138, 30)


def title_ink(png, rect):
    """The pixel title's drawn ink in a launcher shot: (orange pixels, pixels neither orange nor the
    background inside the rect, ink bounding box, orange pixels outside the rect, horizontal ink run
    lengths). Whole-pixel cells leave no blended pixel and runs that are multiples of the cell."""
    from PIL import Image
    im = Image.open(png).convert("RGB")
    w, h = im.size
    px = im.load()
    x0, y0, x1, y1 = (int(round(v)) for v in rect)
    ink = blend = outside = 0
    box = [w, h, -1, -1]
    runs = set()
    for y in range(max(0, y0 - 4), min(h, y1 + 4)):
        run = 0
        for x in range(max(0, x0 - 4), min(w, x1 + 4)):
            c = px[x, y]
            inside = x0 <= x < x1 and y0 <= y < y1
            if c == TITLE_ORANGE:
                ink += inside
                outside += not inside
                box = [min(box[0], x), min(box[1], y), max(box[2], x + 1), max(box[3], y + 1)]
                run += 1
            else:
                if run:
                    runs.add(run)
                run = 0
                blend += inside and c != BG
        if run:
            runs.add(run)
    return ink, blend, box, outside, runs


def case_layout(a):
    """The drop screen against the reference layout's empty-library page, per preset."""
    d = case_dir(a.out, "layout")
    ref = read_json(REFERENCE)
    if ref is None:
        print(f"[launcher] FAIL: {REFERENCE} missing")
        return 1
    bad, compared, total, deltas = [], 0, 0, []
    for p in PRESETS:
        m, rc = drop_manifest(a.binary, d, p)
        if m is None:
            bad.append(f"{p}: no drop.json (rc {rc})")
            continue
        step = ref["presets"][p][0]
        problems = compare_items(m["items"], step["items"], LAYOUT_IDS)
        compared += len(LAYOUT_IDS) - len(problems)
        total += len(LAYOUT_IDS)
        mark = len(deltas)
        page_bad, n = compare_page(m["items"], step["items"], log=deltas)
        print(f"[launcher] {p} name lines (id, width delta, left delta): {deltas[mark:]}")
        problems += page_bad
        compared += n - len(page_bad)
        total += n
        title = next((it for it in step["items"] if it["id"] == "title"), None)
        shot = os.path.join(d, "m-" + p + ".drop.png")
        if title and os.path.isfile(shot):
            ink, blend, box, outside, runs = title_ink(shot, title["rect"])
            cell = title["cell"]
            print(f"[launcher] {p} title ink: {ink} px, box {box}, {blend} blended, {outside} outside, "
                  f"runs {sorted(runs)} (cell {cell})")
            if ink == 0 or blend or outside or any(r % cell for r in runs):
                problems.append(f"title ink not in whole-pixel cells of {cell}: {ink} px, {blend} blended, "
                                f"{outside} outside, runs {sorted(runs)}")
        elif title:
            problems.append(f"no shot {shot}")
        for it in m["items"]:
            if it["id"] in LAYOUT_IDS:
                print(f"[launcher] {p} {it['id']}: {it['rect']}")
        bad += [f"{p} {x}" for x in problems]
    # A game's own launcher (--game) drew a subtitle line under the title; the redesign has none.
    base = os.path.join(d, "m-game")
    rc, _ = launch(a.binary, d, "drop-game", ["--launcher-window", "1280x800", "--game", "spyro-1"],
                   {"OPENPETE_LAUNCHER_MANIFEST": base, "OPENPETE_LAUNCHER_INPUT": "1500:quit"})
    m = read_json(base + ".drop.json")
    if m is None or "subtitle" in {it["id"] for it in m["items"]}:
        bad.append(f"--game spyro-1 drop screen: {'no manifest (rc %d)' % rc if m is None else 'a subtitle is drawn'}")
    for b in bad:
        print("[launcher] FAIL", b)
    worst = max((max(abs(w), abs(x)) for _, w, x in deltas), default=0)
    print(f"[launcher] name lines: {len(deltas)} compared, worst width/left delta {worst} px")
    if total == 0:
        print("[launcher] layout: VACUOUS (no element compared)")
        return 3
    print(f"[launcher] layout: {'FAIL' if bad else 'PASS'} ({compared} of {total} element rects match: "
          f"{', '.join(LAYOUT_IDS)} and the empty game page)")
    return 1 if bad else 0


def case_font(a):
    """The embedded body font, the overlay's default font after the hand-off, and the licence."""
    d = case_dir(a.out, "font")
    bad = []
    m, rc = drop_manifest(a.binary, d, "1280x800")
    with open(os.path.join(d, "drop-1280x800.log"), errors="replace") as f:
        log = f.read()
    if not re.search(r"launcher: body font Fredoka Medium \(embedded, \d+ bytes base85\); default font ProggyClean", log):
        bad.append("the launcher logged no Fredoka body font beside the ProggyClean default")
    # The game page's name lines are the body font's: their widths match the reference's Fredoka lines
    # (ProggyClean's fixed advance would differ by tens of pixels).
    ref = [it for it in read_json(REFERENCE)["presets"]["1280x800"][0]["items"] if ".name." in it["id"]]
    name_bad, n = compare_page((m or {}).get("items", []), ref)
    bad += name_bad
    if n == 0:
        bad.append("no name line compared")
    lines = {it["id"]: it["rect"] for it in (m or {}).get("items", []) if ".name." in it["id"]}
    fonts = [n for n in os.listdir(a.build_dir) if n.lower().endswith((".ttf", ".otf", ".ttc", ".woff", ".woff2"))]
    if fonts:
        bad.append(f"font files beside the executable: {fonts}")
    run = os.path.join(d, "handoff")
    os.makedirs(run)
    seed_library(run)
    rc, log = launch(a.binary, run, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"],
                     {"OPENPETE_LAUNCHER_SELECT": "0"}, timeout=300)
    ov = re.search(r"devui: overlay font: (\S+)", log)
    if not ov or not ov.group(1).startswith("ProggyClean"):
        bad.append(f"the dev overlay's font after the hand-off is {ov and ov.group(1)} (rc {rc})")
    stage = os.path.join(d, "stage")
    os.makedirs(stage)
    r = subprocess.run([sys.executable, "scripts/dist_compliance.py", "licences", "--stage", stage, "--scope", "all"],
                       cwd=ROOT, capture_output=True, text=True)
    shipped = os.path.join(stage, "licences", "Fredoka-OFL.txt")
    src = os.path.join(ROOT, "packaging", "licences", "fredoka-ofl.txt")
    if r.returncode != 0 or not os.path.isfile(shipped) or open(shipped, "rb").read() != open(src, "rb").read():
        bad.append(f"licence staging: rc {r.returncode}, {shipped} {'present' if os.path.isfile(shipped) else 'missing'}")
    with open(os.path.join(ROOT, "packaging", "README-dist.md.in")) as f:
        if "Fredoka" not in f.read():
            bad.append("packaging/README-dist.md.in does not credit Fredoka")
    print(f"[launcher] body lines {sorted(lines.items())}; "
          f"overlay font {ov and ov.group(1)}; licence staged {os.path.isfile(shipped)}")
    for b in bad:
        print("[launcher] FAIL", b)
    print(f"[launcher] font: {'FAIL' if bad else 'PASS'}")
    return 1 if bad else 0


def page_manifest(a, d, tag, preset, screen, env=None, args=(), binary=None, timeout=120):
    """One windowed launch at a preset writing its manifests under d; returns (the screen's manifest or None, log)."""
    base = os.path.join(d, f"m-{tag}-{preset}")
    e = {"OPENPETE_LAUNCHER_MANIFEST": base}
    e.update(env or {"OPENPETE_LAUNCHER_INPUT": "2000:quit"})
    _, log = launch(binary or a.binary, d, f"{tag}-{preset}", ["--launcher-window", preset] + list(args), e,
                    timeout=timeout)
    return read_json(f"{base}.{screen}.json"), log


def page_library(a, d, bad):
    """The page over a library of Spyro 1's disc and over one of all three, against the reference; the count compared."""
    ref = read_json(REFERENCE)["presets"]
    n = 0
    for lib, step in ((("spyro-1",), 9), (GAMES, 13)):
        run = os.path.join(d, f"lib-{len(lib)}")
        os.makedirs(run)
        seed_library(run, lib)
        # The asset cache: a headless launch runs each library game's extraction inline before it boots.
        rc, log = launch(a.binary, run, "prime", ["--headless", "--frame-limit", "1"], timeout=900)
        missing = [g for g in lib if not re.search(rf"asset cache of {g} (written|is current)", log)]
        if missing:
            bad.append(f"library {lib}: no asset cache for {missing} (rc {rc}, log {run}/prime.log)")
            continue
        for preset in ("1280x800", "1024x768"):
            m, _ = page_manifest(a, run, "page", preset, "selector")
            if m is None:
                bad.append(f"library {lib} {preset}: no selector.json")
                continue
            page_bad, k = compare_page(m["items"], ref[preset][step]["items"])
            ids = {it["id"] for it in m["items"]}
            page_bad += [f"box.{g}.logo not drawn" for g in lib if f"box.{g}.logo" not in ids]
            bad += [f"library {lib} {preset} {x}" for x in page_bad]
            n += k
            logos = [(it["id"], it["rect"]) for it in m["items"] if it["id"].endswith(".logo")]
            print(f"[launcher] page library {list(lib)} {preset}: {k} rects compared, logos {logos}")
    return n


def page_drop_focus(a, d, bad):
    """A disc dropped on the empty page returns to the page with its game's box focused (grown)."""
    run = os.path.join(d, "drop")
    os.makedirs(run)
    cue = os.path.join(ROOT, "games", "spyro-2", "spyro-2", "build", DISCS["spyro-2"] + ".cue")
    if not os.path.isfile(cue):
        raise Skip(f"{cue} missing")
    env = {"OPENPETE_LAUNCHER_DROP": cue, "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500",
           "OPENPETE_LAUNCHER_SELECT_GAME": "spyro-2"}
    m, log = page_manifest(a, run, "drop", "1280x800", "selector", env, ["--frame-limit", "5"], timeout=900)
    want = next(it["rect"] for it in read_json(REFERENCE)["presets"]["1280x800"][10]["items"]
                if it["id"] == "box.spyro-2.frame")
    got = next((it["rect"] for it in (m or {}).get("items", []) if it["id"] == "box.spyro-2.frame"), None)
    print(f"[launcher] page drop: spyro-2 frame {got}, focused reference {want}")
    if got is None or any(abs(got[q] - want[q]) > 1.0 for q in range(4)):
        bad.append(f"after a drop of spyro-2 its frame is {got}, not the focused {want} (log {run}/drop-1280x800.log)")
    return 1


def page_one_box(a, d, bad):
    """An executable with Spyro 1's first-boot inputs staged and no other game's draws one box, the
    reference's one-box page; None (a SKIP row) without a first-boot build."""
    staged = os.path.join(a.build_dir, "recomp", "manifest")
    if not os.path.isdir(os.path.join(staged, "spyro-1")):
        print(f"[launcher] page one-box: SKIP (no first-boot inputs staged in {staged})")
        return None
    exe = os.path.join(d, "one-box")
    os.makedirs(os.path.join(exe, "recomp", "manifest"))
    os.link(a.binary, os.path.join(exe, "openpete"))
    os.symlink(os.path.join(staged, "spyro-1"), os.path.join(exe, "recomp", "manifest", "spyro-1"))
    m, log = page_manifest(a, exe, "one-box", "1280x800", "drop", binary=os.path.join(exe, "openpete"))
    ids = sorted(it["id"] for it in (m or {}).get("items", []) if it["id"].startswith("box."))
    page_bad, n = compare_page((m or {}).get("items", []), read_json(REFERENCE)["presets"]["1280x800"][14]["items"])
    if any(g in i for i in ids for g in ("spyro-2", "spyro-3")):
        page_bad.append(f"boxes of games without staged inputs drawn: {ids}")
    for g in ("spyro-2", "spyro-3"):
        if not re.search(rf"game page: no box for {g}: its first-boot inputs are not staged", log):
            page_bad.append(f"no log line naming {g}'s missing inputs")
    bad += [f"one-box {x}" for x in page_bad]
    print(f"[launcher] page one-box: {ids}")
    return n


def case_page(a):
    """The game page with library discs, the focus after a drop, and the available-games rule."""
    d = case_dir(a.out, "page")
    bad = []
    n = page_library(a, d, bad) + page_drop_focus(a, d, bad)
    one = page_one_box(a, d, bad)
    for b in bad:
        print("[launcher] FAIL", b)
    print(f"[launcher] page: {'FAIL' if bad else 'PASS'} ({n + (one or 0)} rects compared; one-box row "
          f"{'run' if one is not None else 'SKIP'})")
    if n == 0:
        return 3
    return 1 if bad else 0


def check_reference_file():
    """Problems with the reference layout manifest, as strings."""
    m = read_json(REFERENCE)
    if m is None:
        return [f"{REFERENCE} missing"]
    bad = []
    if sorted(m.get("presets", {})) != sorted(PRESETS):
        bad.append(f"reference presets {sorted(m.get('presets', {}))} != {sorted(PRESETS)}")
    n = 0
    for p, steps in m.get("presets", {}).items():
        for s in steps:
            for it in s["items"]:
                n += 1
                extra = set(it) - REFERENCE_KEYS
                if extra:
                    bad.append(f"{p} {s['state']} {it.get('id')}: fields {sorted(extra)} outside the set")
    if n == 0:
        bad.append("reference manifest holds no item")
    return bad


def case_manifest(a):
    """The manifest writer and the reference layout manifest."""
    d = case_dir(a.out, "manifest")
    bad = check_reference_file()
    engaged = 0
    for p in PRESETS:
        w, h = map(int, p.split("x"))
        m, rc = drop_manifest(a.binary, d, p)
        if m is None:
            bad.append(f"{p}: no drop.json written (rc {rc}, log {d}/drop-{p}.log)")
            continue
        if m["window"] != [w, h]:
            bad.append(f"{p}: manifest window {m['window']}")
        ids = {it["id"] for it in m["items"]}
        if not DROP_IDS <= ids:
            bad.append(f"{p}: drop ids missing {sorted(DROP_IDS - ids)}")
        for it in m["items"]:
            x0, y0, x1, y1 = it["rect"]
            if not (0 <= x0 < x1 <= w and 0 <= y0 < y1 <= h):
                bad.append(f"{p}: {it['id']} rect {it['rect']} outside the window or empty")
        engaged += len(m["items"])
        print(f"[launcher] {p}: {len(m['items'])} items")
    if engaged == 0:
        print("[launcher] manifest: VACUOUS (no item written)")
        return 3
    for b in bad:
        print("[launcher] FAIL", b)
    print(f"[launcher] manifest: {'FAIL' if bad else 'PASS'} ({engaged} items over {len(PRESETS)} presets)")
    return 1 if bad else 0


def env_members(root):
    """The pids of live processes whose environment holds XDG_DATA_HOME=root."""
    want = ("XDG_DATA_HOME=" + root).encode()
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == os.getpid():
            continue
        try:
            with open(f"/proc/{pid}/environ", "rb") as f:
                env = f.read().split(b"\0")
            with open(f"/proc/{pid}/stat") as f:
                state = f.read().rsplit(")", 1)[1].split()[0]
        except (OSError, IndexError):
            continue
        if want in env and state != "Z":
            out.append(int(pid))
    return out


def comm(pid):
    """A process's command name, or '?' once it is gone."""
    try:
        with open(f"/proc/{pid}/comm") as f:
            return f.read().strip()
    except OSError:
        return "?"


def watch_run(root, run):
    """Call run() while recording every process whose environment names the data root; returns
    (run's result, {pid: command name} seen while it ran)."""
    import threading
    seen, done = {}, threading.Event()

    def poll():
        while not done.is_set():
            for pid in env_members(root):
                # The latest name: the wrapper shell execs the binary under the same pid.
                name = comm(pid)
                if name != "?":
                    seen[pid] = name
            time.sleep(0.2)
    t = threading.Thread(target=poll, daemon=True)
    t.start()
    try:
        return run(), seen
    finally:
        done.set()
        t.join()


def case_reap(a):
    """A timed-out run's processes end with it, the SIGKILL path included; the control sees the leak."""
    d = case_dir(a.out, "reap")
    bad = []
    stub = os.path.join(d, "hang-stub")
    with open(stub, "w") as f:
        f.write("#!/bin/sh\ntrap '' TERM\nsleep 600\n")
    os.chmod(stub, 0o755)
    for tag, binary, timeout in (("idle", a.binary, 15), ("ignore-term", stub, 5)):
        run = os.path.join(d, tag)
        os.makedirs(run)
        root = os.path.join(run, "data")
        t0 = time.monotonic()
        (rc, log), seen = watch_run(root, lambda: launch(binary, run, "run", ["--launcher-window", "1280x800"],
                                                         timeout=timeout))
        took = time.monotonic() - t0
        left = env_members(root)
        names = sorted(set(seen.values()))
        print(f"[launcher] reap {tag}: exit {rc} after {took:.1f} s, seen {len(seen)} {names}, alive after {len(left)}")
        if rc != -9:
            bad.append(f"{tag}: exit {rc}, not the timeout")
        want = "openpete" if binary == a.binary else "sleep"
        if want not in names or "openpete_xvfb_w" not in names:
            bad.append(f"{tag}: VACUOUS: {want} and the window manager not both seen ({names})")
        if tag == "idle" and "stamp first_present" not in log:
            bad.append("idle: the launcher never presented (log run.log)")
        if left:
            bad.append(f"{tag}: {len(left)} process(es) outlived the run: {[(p, comm(p)) for p in left]}")
    # A harness killed outright (SIGKILL, as a ctest timeout does) mid-run: its guardian ends the run.
    run = os.path.join(d, "harness-killed")
    os.makedirs(run)
    root = os.path.join(run, "data")
    child = ("import os, subprocess, sys\n"
             "sys.path.insert(0, sys.argv[1])\n"
             "import check_launcher as cl\n"
             "cl.install_reaper()\n"
             "e = dict(os.environ, XDG_DATA_HOME=sys.argv[3])\n"
             "cl.run_session([sys.argv[2]], e, subprocess.DEVNULL, 600)\n")
    h = subprocess.Popen([sys.executable, "-c", child, os.path.dirname(os.path.abspath(__file__)), stub, root])
    deadline, seen = time.monotonic() + 20, []
    while time.monotonic() < deadline and "sleep" not in seen:
        seen = [comm(q) for q in env_members(root)]
        time.sleep(0.1)
    h.kill()
    h.wait()
    deadline, left = time.monotonic() + 15, env_members(root)
    while left and time.monotonic() < deadline:
        time.sleep(0.1)
        left = env_members(root)
    print(f"[launcher] reap harness-killed: run seen {sorted(set(seen))}, harness SIGKILLed, "
          f"alive after {len(left)}")
    if "sleep" not in seen:
        bad.append(f"harness-killed: VACUOUS: the stand-in never ran ({seen})")
    if left:
        bad.append(f"harness-killed: {len(left)} process(es) outlived the killed harness: "
                   f"{[(q, comm(q)) for q in left]}")
        for q in left:
            os.kill(q, signal.SIGKILL)
    # The control: the wrapper alone killed on the timeout leaves the binary running.
    run = os.path.join(d, "control")
    os.makedirs(run)
    root = os.path.join(run, "data")
    e = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY" and not k.startswith("OPENPETE_")}
    e.update({"SDL_VIDEODRIVER": "x11", "OPENPETE_MSGBOX_SKIP": "1", "XDG_DATA_HOME": root,
              "XDG_STATE_HOME": os.path.join(run, "state"), "XDG_CONFIG_HOME": os.path.join(run, "config")})
    with open(os.path.join(run, "run.log"), "w") as f:
        p = subprocess.Popen(["xvfb-run", "-a", "-s", f"-screen 0 {SCREEN}x24", a.binary, "--no-config",
                              "--no-card", "--vibration", "0", "--input-lock", "--mute", "1",
                              "--launcher-window", "1280x800"],
                             env=e, stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
        track_session(p.pid)
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        time.sleep(1.0)
        leaked = [comm(q) for q in env_members(root)]
        reap_session(p.pid)
        untrack_session(p.pid)
    after = env_members(root)
    print(f"[launcher] reap control: wrapper killed, {len(leaked)} left running {sorted(leaked)}; "
          f"{len(after)} after the session's end")
    if "openpete" not in leaked:
        bad.append(f"control: the binary did not outlive its killed wrapper ({leaked}): the instrument sees no leak")
    if after:
        bad.append(f"control: {len(after)} process(es) survived the session's end")
    for b in bad:
        print("[launcher] FAIL", b)
    print(f"[launcher] reap: {'FAIL' if bad else 'PASS'}")
    return 1 if bad else 0


def case_calibrate(a):
    """The drop screen against a reference manifest of it: per id, the rect deltas."""
    cal = read_json(a.calibration) if a.calibration else None
    if cal is None:
        print("[launcher] calibrate needs --calibration PATH")
        return 64
    d = case_dir(a.out, "calibrate")
    worst = {}
    for p in PRESETS:
        m, rc = drop_manifest(a.binary, d, p)
        if m is None:
            print(f"[launcher] {p}: no drop.json (rc {rc})")
            return 1
        ours = {it["id"]: it["rect"] for it in m["items"]}
        for it in cal["presets"][p]["drop"]:
            r = ours.get(it["id"])
            if r is None:
                continue
            dl = [round(r[i] - it["rect"][i], 2) for i in range(4)]
            print(f"[launcher] {p} {it['id']:14s} build {r} reference {it['rect']} delta {dl}")
            k = (it["id"], it["kind"])
            worst[k] = max(worst.get(k, 0), max(abs(v) for v in dl))
    for (i, k), v in sorted(worst.items()):
        print(f"[launcher] worst |delta| {i} ({k}): {v} px")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case", choices=["manifest", "layout", "page", "handoff", "window-fit", "window-default",
                                     "font", "density", "reap", "calibrate"])
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build", "linux-release"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--calibration", default=None)
    a = ap.parse_args()
    a.binary = os.path.join(a.build_dir, "openpete")
    a.out = a.out or os.path.join(a.build_dir, "launcher-tests")
    global WM
    WM = os.path.join(a.build_dir, "openpete_xvfb_wm")
    install_reaper()
    if not os.access(a.binary, os.X_OK):
        print(f"[launcher] SKIP: {a.binary} missing")
        return 64
    try:
        return {"manifest": case_manifest, "layout": case_layout, "page": case_page, "handoff": case_handoff,
                "window-fit": case_window_fit,
                "window-default": case_window_default, "font": case_font, "density": case_density,
                "reap": case_reap, "calibrate": case_calibrate}[a.case](a)
    except Skip as e:
        print(f"[launcher] SKIP: {e}")
        return 64


if __name__ == "__main__":
    sys.exit(main())
