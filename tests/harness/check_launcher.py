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
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REFERENCE = os.path.join(ROOT, "tests", "launcher", "layout-reference.json")
PRESETS = ["1280x800", "1920x1080", "2560x1080", "1024x768", "2560x1440", "3840x2160"]
REFERENCE_KEYS = {"id", "kind", "rect", "font", "px", "cell"}
# The virtual display: larger than the largest preset, so no request is cut.
SCREEN = "4096x2304"
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
    e = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY"}
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
    ap.add_argument("case", choices=["manifest", "calibrate"])
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
        return {"manifest": case_manifest, "calibrate": case_calibrate}[a.case](a)
    except Skip as e:
        print(f"[launcher] SKIP: {e}")
        return 64


if __name__ == "__main__":
    sys.exit(main())
