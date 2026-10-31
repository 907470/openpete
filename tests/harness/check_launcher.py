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
            screen, the writer writes drop.json, and the file names the
            window size, holds the drop screen's elements, and every rect
            lies inside the window. The reference layout manifest
            (tests/launcher/layout-reference.json) parses, covers the six
            presets, and carries no field outside the id/kind/rect/font/px/
            cell set (no game-derived field).
  layout    The drop screen's elements against the reference layout at each
            reference preset: for every id in LAYOUT_IDS, the launcher's rect
            and the reference's on the empty-library page (the reference's
            first step). A text item's left edge and height match exactly and
            its width within TEXT_WIDTH_BOUND; any other rect within 1 px.
            The ids compared are those the launcher draws as the reference
            does: the pixel title. The drop screen's shot
            (OPENPETE_LAUNCHER_SHOT) is read too: the title's orange ink lies
            inside its rect, no pixel in the rect is a blend of the orange
            and the background, and every horizontal ink run is a multiple
            of the reference's cell (whole-pixel cells). A --game spyro-1
            launch's drop screen draws no subtitle.
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
            a window value under [game.spyro-1] does not apply (1280x800). The size and maximized state the
            launcher device was created at are asserted.
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
import subprocess
import sys

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
# A text item's width against the reference's, in pixels. Calibrated on the drop screen's seven strings
# (ProggyClean at 8, 14 and 28 layout px) at the six presets, drawn by the launcher and by the reference
# layout's text model with imgui's whole-pixel font sizes: widths differ by at most 1 px (imgui's
# CalcTextSize at some sizes and strings is one pixel wider or narrower than whole-pixel advances
# times the length), heights not at all; a centred string's left edge moves with that pixel. Text in
# whole-pixel cells (the pixel title) matches exactly.
TEXT_WIDTH_BOUND = 1.0
DROP_IDS = {"title", "drag.1", "drag.2", "browse.hint", "browse.button", "disc.note", "quit.hint"}


class Skip(Exception):
    """A precondition the machine lacks: the case exits 64."""


def case_dir(out, name):
    """The case's output directory, emptied: stale outputs give false verdicts."""
    d = os.path.join(out, name)
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    return d


def launch(binary, d, tag, args, env=None, timeout=120, screen=SCREEN, panel=0):
    """One windowed run under a fresh virtual display and the window manager; returns (rc, log text).

    The window manager reserves a bottom panel of `panel` pixels in the work area. The run starts once
    the manager has advertised itself (its ready file), so the window maps under it."""
    if not shutil.which("xvfb-run"):
        raise Skip("xvfb-run not found")
    wm = os.path.join(os.path.dirname(binary), "openpete_xvfb_wm")
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
        try:
            rc = subprocess.run(cmd, env=e, stdout=f, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            rc = -9
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


def seed_library(d):
    """A library holding one Spyro 1 entry whose disc.bin links the repository's image (no copy)."""
    if not os.path.isfile(DISC_BIN):
        raise Skip(f"{DISC_BIN} missing")
    e = os.path.join(d, "data", "openpete", "library", "00000000launcher")
    os.makedirs(e)
    os.symlink(DISC_BIN, os.path.join(e, "disc.bin"))
    with open(os.path.join(e, "disc.cue"), "w") as f:
        f.write('FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    with open(os.path.join(e, "meta.toml"), "w") as f:
        f.write('label = "Spyro the Dragon (launcher test)"\nsource = "SCUS-94228.bin"\n'
                'sha256 = "%s"\nlast_played = 1\ngame = "spyro-1"\n' % ("0" * 64))


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
        fit = re.search(r"window fit: request 1280x800, display (\d+x\d+), usable (\d+x\d+): opens (.+) \((\d+)x(\d+)\)", log)
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
    bad = []
    for tag, args, size, maxed in rows:
        rc, log = launch(a.binary, d, tag, args, {"OPENPETE_LAUNCHER_INPUT": "1500:quit"})
        dev = re.search(r"launcher device \S+ created on window \S+ \((\d+)x(\d+)(, maximized)?", log)
        got = ((int(dev.group(1)), int(dev.group(2))), bool(dev.group(3))) if dev else None
        if got != (size, maxed):
            bad.append(f"{tag}: opened {got}, expected {(size, maxed)} (rc {rc}, log {d}/{tag}.log)")
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
    bad, compared = [], 0
    for p in PRESETS:
        m, rc = drop_manifest(a.binary, d, p)
        if m is None:
            bad.append(f"{p}: no drop.json (rc {rc})")
            continue
        step = ref["presets"][p][0]
        problems = compare_items(m["items"], step["items"], LAYOUT_IDS)
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
        compared += len(LAYOUT_IDS) - len(problems)
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
    print(f"[launcher] layout: {'FAIL' if bad else 'PASS'} ({compared} of {len(LAYOUT_IDS) * len(PRESETS)} "
          f"element rects match: {', '.join(LAYOUT_IDS)})")
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
    ap.add_argument("case", choices=["manifest", "layout", "handoff", "window-fit", "window-default",
                                     "calibrate"])
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build", "linux-release"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--calibration", default=None)
    a = ap.parse_args()
    a.binary = os.path.join(a.build_dir, "openpete")
    a.out = a.out or os.path.join(a.build_dir, "launcher-tests")
    if not os.access(a.binary, os.X_OK):
        print(f"[launcher] SKIP: {a.binary} missing")
        return 64
    try:
        return {"manifest": case_manifest, "layout": case_layout, "handoff": case_handoff,
                "window-fit": case_window_fit,
                "window-default": case_window_default, "calibrate": case_calibrate}[a.case](a)
    except Skip as e:
        print(f"[launcher] SKIP: {e}")
        return 64


if __name__ == "__main__":
    sys.exit(main())
