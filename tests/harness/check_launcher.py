#!/usr/bin/env python3
"""check_launcher.py - the launcher's checks, one case per ctest test of the
label `launcher`.

Every case runs the linux-release binary windowed under its own virtual X
display (xvfb-run -a, SDL_VIDEODRIVER=x11, WAYLAND_DISPLAY unset) on
lavapipe, with a scratch data root of its own (XDG_DATA_HOME,
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
            the mod and shader runs must log their step.
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


def launch(binary, d, tag, args, env=None, timeout=120, screen=SCREEN):
    """One windowed run under a fresh virtual display; returns (rc, log text)."""
    if not shutil.which("xvfb-run"):
        raise Skip("xvfb-run not found")
    # The caller's OPENPETE_* knobs (OPENPETE_BACKGROUND, for one) would change the paths under test.
    e = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY" and not k.startswith("OPENPETE_")}
    e.update({"SDL_VIDEODRIVER": "x11", "OPENPETE_MSGBOX_SKIP": "1",
              "XDG_DATA_HOME": os.path.join(d, "data"), "XDG_STATE_HOME": os.path.join(d, "state"),
              "XDG_CONFIG_HOME": os.path.join(d, "config")})
    e.update(env or {})
    cmd = ["xvfb-run", "-a", "-s", f"-screen 0 {screen}x24", binary, "--no-config", "--no-card",
           "--vibration", "0", "--input-lock", "--mute", "1"] + args
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
    env = {"OPENPETE_LAUNCHER_MANIFEST": base, "OPENPETE_LAUNCHER_INPUT": "1500:quit"}
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
        last = [p for p in ps if p[0] == "launcher"][-1:] or [None]
        print(f"[launcher] {tag}: {len(ps)} presents traced, {drew} launcher presents drew, "
              f"last launcher present {last[0]}: {'FAIL ' + '; '.join(bad) if bad else 'PASS'}")
        fails += bool(bad)
    print(f"[launcher] handoff: {'FAIL' if fails else 'PASS'} ({len(runs) - fails} of {len(runs)} paths)")
    return 1 if fails else 0


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
    ap.add_argument("case", choices=["manifest", "handoff", "calibrate"])
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
        return {"manifest": case_manifest, "handoff": case_handoff,
                "calibrate": case_calibrate}[a.case](a)
    except Skip as e:
        print(f"[launcher] SKIP: {e}")
        return 64


if __name__ == "__main__":
    sys.exit(main())
