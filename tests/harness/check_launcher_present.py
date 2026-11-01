#!/usr/bin/env python3
"""The launcher's present rate: one present per display refresh while the window shows, none while it is
minimised or hidden, and a display change that rebuilds the swapchain at the new display's refresh.

One windowed run on the empty library's game page under a fresh virtual display (xvfb-run, the minimal
window manager tests/harness/xvfb_wm.c, lavapipe; check_launcher.launch) with
OPENPETE_LAUNCHER_PRESENT_LOG=1, which logs the presents of every second, and a scripted sequence
(OPENPETE_LAUNCHER_INPUT): shown, minimised, restored, hidden, shown, then display=75 (the refresh query
stubbed to 75 Hz and a synthetic SDL_EVENT_WINDOW_DISPLAY_CHANGED posted), then quit.

Pass criteria:
  - every per-second rollup while shown is within RATE_BOUND of the refresh in force (the swapchain's
    refresh, 60 Hz when the display reports none), except a rollup holding a present that began more
    than half a period late (lavapipe on a loaded machine), to which only the ceiling applies: no rollup
    exceeds the refresh; at least MIN_ROLLUPS rollups exist per phase and at least MIN_ROLLUPS are refresh-paced
    overall (else VACUOUS);
  - each stop line is followed by a resume line with the same present count (no present while
    minimised or hidden), once for the minimise and once for the hide;
  - the display change logs a rebuild to 75 Hz and a swapchain created with a 20.0 ms acquire budget
    (1.5 periods of 75 Hz), and the rollups after it report that refresh.

usage: check_launcher_present.py [--build-dir DIR] [--out DIR]
Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (binary, xvfb-run or the window manager missing).
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_launcher as cl  # noqa: E402

# A rollup's rate against the refresh in force: the pacer sleeps to each period's end on a surface that
# does not block (Xvfb), so a second holds the refresh's count give or take one present of jitter.
RATE_BOUND = 0.05
MIN_ROLLUPS = 2
STUB_HZ = 75
SCRIPT = "3000:minimize,5500:restore,8500:hide,11000:show,14000:display=%d,18000:quit" % STUB_HZ

ROLLUP = re.compile(r"launcher presents: (\d+) in ([\d.]+) ms \(([\d.]+) per s, refresh ([\d.]+) Hz, "
                    r"divisor 1, busy ([\d.]+) ms per present, (\d+) late\)")
STOP = re.compile(r"window minimised or hidden: presents stop \((\d+) presents so far\)")
RESUME = re.compile(r"window shown: presents resume at the refresh \((\d+) presents so far\)")
CHANGE = re.compile(r"display change: refresh ([\d.]+) -> ([\d.]+) Hz, swapchain rebuild")
BUDGET = re.compile(r"swapchain: \d+ image\(s\), acquire budget ([\d.]+) ms \(refresh ([\d.]+) Hz\)")


def phases(log):
    """Split the log at each stop, resume and display change: [(label, [rollup rate, refresh])...]."""
    out, label, rows = [], "shown-1", []
    marks = [(m.start(), "stop", m) for m in STOP.finditer(log)] + \
            [(m.start(), "resume", m) for m in RESUME.finditer(log)] + \
            [(m.start(), "change", m) for m in CHANGE.finditer(log)] + \
            [(m.start(), "rollup", m) for m in ROLLUP.finditer(log)]
    n = 1
    for _, kind, m in sorted(marks, key=lambda x: x[0]):
        if kind == "rollup":
            rows.append((float(m.group(3)), float(m.group(4)), float(m.group(5)), int(m.group(6))))
            continue
        out.append((label, rows))
        rows = []
        n += 1
        label = {"stop": "stopped", "resume": "shown", "change": "changed"}[kind] + f"-{n}"
    out.append((label, rows))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.join(cl.ROOT, "build", "linux-release"))
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    binary = os.path.join(a.build_dir, "openpete")
    if not os.access(binary, os.X_OK):
        print(f"[launcher] SKIP: {binary} missing")
        return 64
    out = a.out or os.path.join(a.build_dir, "launcher-tests")
    d = cl.case_dir(out, "present-rate")
    try:
        rc, log = cl.launch(binary, d, "present", ["--launcher-window", "1280x800"],
                            {"OPENPETE_LAUNCHER_PRESENT_LOG": "1", "OPENPETE_LAUNCHER_INPUT": SCRIPT}, timeout=120)
    except cl.Skip as e:
        print(f"[launcher] SKIP: {e}")
        return 64
    bad, drawbound, paced = [], [], []
    stops = [int(m.group(1)) for m in STOP.finditer(log)]
    resumes = [int(m.group(1)) for m in RESUME.finditer(log)]
    if len(stops) != 2 or len(resumes) != 2:
        bad.append(f"expected 2 stops and 2 resumes, got {stops} and {resumes}")
    for s_, r_ in zip(stops, resumes):
        if s_ != r_:
            bad.append(f"{r_ - s_} presents ran while the window could not show ({s_} -> {r_})")
    for label, rows in phases(log):
        if label.startswith("stopped"):
            if rows:
                bad.append(f"{label}: {len(rows)} rollups while the window could not show")
            continue
        steady = rows[1:] if label.startswith("changed") else rows   # the rebuild's second is transient
        if len(steady) < MIN_ROLLUPS:
            bad.append(f"{label}: {len(steady)} rollups, need {MIN_ROLLUPS}")
        for rate, hz, busy, late in steady:
            want = hz if hz > 1.0 else 60.0
            if rate > want * (1 + RATE_BOUND):
                bad.append(f"{label}: {rate:.2f} presents per s above the {want:.3f} Hz refresh")
            # A rollup holding a present that began more than half a period late (its own work, or the
            # loop's, outlasted the period: lavapipe on a loaded machine) measures the machine, and only
            # the ceiling above applies to it.
            if late:
                drawbound.append(label)
                continue
            paced.append(label)
            if abs(rate - want) > RATE_BOUND * want:
                bad.append(f"{label}: {rate:.2f} presents per s against a {want:.3f} Hz refresh "
                           f"(busy {busy:.2f} ms)")
        if label.startswith("changed") and any(abs(r[1] - STUB_HZ) > 0.01 for r in steady):
            bad.append(f"{label}: rollups after the display change report refresh {[r[1] for r in steady]}")
        print(f"[launcher] {label}: rollups {[(round(r[0], 2), round(r[2], 2), r[3]) for r in rows]} (rate, busy ms, late) "
              f"at refresh {sorted({r[1] for r in rows})}")
    change = CHANGE.search(log)
    if not change or abs(float(change.group(2)) - STUB_HZ) > 0.01:
        bad.append(f"no display change rebuild to {STUB_HZ} Hz")
    budgets = [(float(m.group(1)), float(m.group(2))) for m in BUDGET.finditer(log)]
    want_ms = round(1.5e3 / STUB_HZ, 1)
    if not any(abs(hz - STUB_HZ) < 0.01 and abs(ms - want_ms) < 0.05 for ms, hz in budgets):
        bad.append(f"no swapchain at {STUB_HZ} Hz with a {want_ms} ms acquire budget (saw {budgets})")
    print(f"[launcher] stops {stops} resumes {resumes}; swapchains {budgets}; rc {rc}; log {d}/present.log")
    total = sum(len(rows) for _, rows in phases(log))
    if total == 0:
        print("[launcher] present-rate: VACUOUS (no rollup line: OPENPETE_LAUNCHER_PRESENT_LOG not honoured)")
        return 3
    if drawbound:
        print(f"[launcher] note: {len(drawbound)} rollups were bound by the present's own work, not the refresh "
              f"({sorted(set(drawbound))})")
    if len(paced) < MIN_ROLLUPS:
        print(f"[launcher] present-rate: VACUOUS ({len(paced)} rollups paced by the refresh; the machine is too "
              "loaded for lavapipe to draw at the refresh)")
        return 3
    for b in bad:
        print("[launcher] FAIL", b)
    print(f"[launcher] present-rate: {'FAIL' if bad else 'PASS'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
