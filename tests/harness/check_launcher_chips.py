#!/usr/bin/env python3
"""check_launcher_chips.py - the launcher's corner chips, the progress view's weighted bar and the
shape pass's corner profile: one case per ctest test of the label `launcher`, beside
check_launcher.py (whose launch helper, virtual display and scratch data root every case uses).

Cases:
  corner    T4: the corner profile of every panel and chip in a launcher shot over a flat white
            clear (OPENPETE_LAUNCHER_CLEAR=255,255,255) against the analytic quarter ellipse, by the
            bench's check_corner.py method applied to the launcher's PNG. The run is
            check_launcher_shapes.py's seeded pick (one Spyro 1 entry, the shader compile forced),
            whose progress screen (shot tag progress) draws the step chip and the shader compile's
            corner chip, with imgui's output hidden (OPENPETE_LAUNCHER_TEXT_HIDE) and the drop shadow
            (OPENPETE_LAUNCHER_SHADOW=0) off, so a pixel inside a box's corner square is the clear
            blended with (8,8,8) at the box's coverage. Per corner row (rows whose centre is past the straight edge's ramp) the
            measured inset is the box's width at that row minus its integrated coverage, against
            the ellipse of the box's radii (D11's 22.8 x 18.4 layout px, scaled down together to
            fit, as the shape pass does). PASS when every row of every corner is within
            CORNER_BOUND px and the same rows against a circle of the vertical radius exceed it
            (the instrument resolves the corner's aspect).
  progress  T6: a first boot's bar position against elapsed time. A windowed drop of Spyro 1's disc
            on an empty library (OPENPETE_LAUNCHER_DROP) logs each change of the bar's whole percent
            or stage (OPENPETE_LAUNCHER_PROGRESS_LOG: "progress bar: F at T s (STAGE d/t)"); the
            first-boot stages (copy, read, translate, build, link) span t0..t1, the first and last
            such line. The bar holds each value until the next line, so each value is compared
            with the linear position (t - t0) / (t1 - t0) at its own time and at the next line's.
            PASS when every deviation is within PROGRESS_BAND, all five stages were drawn, and the
            per-stage count bar the weights replaced (each stage's done/total from 0, read from
            the same lines) exceeds the band (the instrument separates the two). Needs the
            first-boot pipeline (a *-launcher build; SKIP otherwise). SKIP when the run completed and
            more than PROGRESS_MAX_OTHERS cores were busy with other work over it (other_busy_cores).
  chips     D15, D16 and D28 on the game page (windowed, 1280x800): a library of one Spyro 1 entry
            whose launcher cache a headless run wrote, with the ahead compile on
            (OPENPETE_PRECOMPILE_AHEAD=1). The first frame showing one opaque dome (shot and
            manifest tag sky) holds the level chip, status.skybox, at the bottom-right corner
            (its box 12 layout px from the right and bottom edges, the text 8 x 6 layout px
            inside it) naming a level of Spyro 1's table by its display name, and, while the
            compile runs, the shader chip, status.shaders, "Compiling shaders N%" at the
            bottom-left corner with its spinner (a square of the text height 8 layout px inside the
            box, the text 6 layout px after it). A scripted click inside the level chip
            (click=-30/-30) at CLICK_MS logs the hover's hand cursor and the press, starts the
            crossfade (the label changes at its midpoint, 1.25 s on the sky clock) and restarts the
            rotation: the next label change follows the press by the 2.5 s crossfade, the 8 s hold
            and the next crossfade's midpoint (11.75 s), within CLOCK_TOL.

Usage: tests/harness/check_launcher_chips.py CASE [--build-dir DIR] [--out DIR]
Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (binary, xvfb-run, a disc image or the first-boot pipeline
missing; progress also on a loaded host).
"""
import argparse
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_launcher as cl  # noqa: E402
import check_launcher_flow as flow  # noqa: E402

LAYOUT_W, LAYOUT_H = 640.0, 448.0
PANEL_RX, PANEL_RY = 22.8, 18.4      # D11, layout px
EDGE_AA = 1.5                        # the shape pass's coverage ramp, framebuffer px
CORNER_BOUND = 0.15                  # px: the bench measures 0.12 on its own panel, this pass 0.107
BOX = 8
CHIP_ALPHA = 0.5
PROGRESS_BAND = 0.10                 # the weights' worst deviation over the timing runs is 0.073
# Other busy cores over the run above which T6 skips: the compile step's 16 workers share the host, so
# its units stretch unevenly and the deviation grows with the load (0.104-0.111 at 1.4-1.8 other cores,
# 0.089-0.173 at 2.5-7).
PROGRESS_MAX_OTHERS = 3.0
FIRST_BOOT = ("COPYING", "READING", "TRANSLATING", "BUILDING", "LINKING")
CHIP_PX, CHIP_PAD_X, CHIP_PAD_Y, CHIP_MARGIN, SPIN_GAP = 10.0, 8.0, 6.0, 12.0, 6.0
CLICK_MS = 6000
CLOCK_TOL = 0.35                     # s on the sky clock: one present period at llvmpipe's rate
SHADER_ENV = {"OPENPETE_PRECOMPILE_FORCE": "1", "OPENPETE_PRECOMPILE_PANEL_MS": "0",
              "OPENPETE_LAUNCHER_SELECT": "0"}


def verdict(name, bad, detail="", vacuous=False):
    """Print and return a case's verdict."""
    if vacuous:
        print(f"[launcher-chips] {name}: VACUOUS {detail}")
        return 3
    for b in bad:
        print(f"[launcher-chips] {name}: FAIL {b}")
    print(f"[launcher-chips] {name}: {'FAIL' if bad else 'PASS'} {detail}")
    return 1 if bad else 0


def layout_scale(w, h):
    """Window pixels per layout pixel at a window size."""
    return min(w / LAYOUT_W, h / LAYOUT_H)


def corner_rows(px, size, rect, rx, ry, bg, inside, flip_x, flip_y):
    """One corner's rows: [(measured inset, analytic ellipse inset, circle inset)] in px.

    The corner is mapped onto the top-left one by flipping the axes; coverage is read from the red
    channel as (bg - p) / (bg - inside). Rows whose centre lies within the straight edge's ramp
    (EDGE_AA px of the box's top) are left out: the edge crosses them."""
    w, h = size
    x0 = (w - rect[2]) if flip_x else rect[0]
    y0 = (h - rect[3]) if flip_y else rect[1]
    xa, xb = int(math.floor(x0)) - 2, int(math.ceil(x0 + rx)) + 2
    rows = []
    for k in range(int(math.ceil(ry)) + 2):
        v = int(math.floor(y0)) + k
        local = v + 0.5 - y0
        dy = ry - local
        if local < EDGE_AA:
            continue
        if dy <= 0:
            break
        cov = 0.0
        for u in range(xa, xb):
            p = px[(w - 1 - u) if flip_x else u, (h - 1 - v) if flip_y else v][0]
            cov += min(1.0, max(0.0, (bg - p) / (bg - inside)))
        meas = (xb - xa) - cov - (x0 - xa)
        q = math.sqrt(max(0.0, 1.0 - (dy / ry) ** 2))
        rows.append((meas, rx * (1.0 - q), ry * (1.0 - q)))
    return rows


def case_corner(a):
    """T4's corner profile of the launcher's boxes over a flat clear."""
    from PIL import Image
    d = cl.case_dir(a.out, "corner")
    cl.seed_library(d)
    base = os.path.join(d, "shot")
    bg = 255
    env = dict(SHADER_ENV, OPENPETE_LAUNCHER_SHOT=base, OPENPETE_LAUNCHER_MANIFEST=base,
               OPENPETE_LAUNCHER_TEXT_HIDE="1", OPENPETE_LAUNCHER_SHADOW="0",
               OPENPETE_LAUNCHER_CLEAR="%d,%d,%d" % (bg, bg, bg), OPENPETE_PRECOMPILE_AHEAD="1")
    rc, _ = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                      timeout=600)
    m, png = cl.read_json(base + ".progress.json"), base + ".progress.png"
    if m is None or not os.path.isfile(png):
        return verdict("corner", [f"no progress shot or manifest (rc {rc})"])
    im = Image.open(png).convert("RGB")
    w, h = im.size
    px = im.load()
    sc = layout_scale(w, h)
    inside = (bg + BOX) * CHIP_ALPHA
    bad, lines, worst, worst_c, nrows = [], [], 0.0, float("inf"), 0
    panels = [it for it in m.get("items", []) if it.get("kind") == "panel"]
    for it in panels:
        x0, y0, x1, y1 = it["rect"]
        k = min(1.0, (x1 - x0) * 0.5 / (PANEL_RX * sc), (y1 - y0) * 0.5 / (PANEL_RY * sc))
        rx, ry = PANEL_RX * sc * k, PANEL_RY * sc * k
        for fx in (False, True):
            for fy in (False, True):
                rows = corner_rows(px, (w, h), it["rect"], rx, ry, bg, inside, fx, fy)
                if not rows:
                    bad.append(f"{it['id']} corner {fx},{fy}: no row measured")
                    continue
                e = max(abs(r[0] - r[1]) for r in rows)
                ec = max(abs(r[0] - r[2]) for r in rows)
                nrows += len(rows)
                worst, worst_c = max(worst, e), min(worst_c, ec)
                lines.append(f"{it['id']} {'R' if fx else 'L'}{'B' if fy else 'T'}: radii {rx:.2f} x {ry:.2f}, "
                             f"{len(rows)} rows, ellipse max {e:.3f} px, circle max {ec:.3f} px")
                if e > CORNER_BOUND:
                    bad.append(f"{it['id']} corner {'R' if fx else 'L'}{'B' if fy else 'T'}: max {e:.3f} px")
    for l in lines:
        print(f"[launcher-chips] corner {l}")
    if not panels:
        return verdict("corner", [], "no panel in the manifest", vacuous=True)
    if worst_c <= CORNER_BOUND:
        bad.append(f"the circle model is within {worst_c:.3f} px on some corner: the profile does not resolve "
                   f"the aspect")
    return verdict("corner", bad, f"{len(panels)} boxes, {nrows} rows: ellipse worst {worst:.3f} px "
                                  f"(bound {CORNER_BOUND}), circle best-corner worst {worst_c:.3f} px")


BAR_LINE = re.compile(r"progress bar: ([\d.]+) at ([\d.]+) s \((.*) (-?\d+)/(-?\d+)\)")


def deviations(pts, t0, t1):
    """The bar's deviations from linear: each value at its own time and at the next point's."""
    out = []
    for i, (t, f) in enumerate(pts):
        nt = pts[i + 1][0] if i + 1 < len(pts) else t
        for tt in (t, nt):
            out.append(abs(f - (tt - t0) / (t1 - t0)))
    return out


def case_progress(a):
    """T6: a first boot's bar position against elapsed time stays within the band of linear."""
    flow.need_first_boot(a)
    d = cl.case_dir(a.out, "progress")
    env = {"OPENPETE_LAUNCHER_DROP": flow.disc_cue("spyro-1"), "OPENPETE_LAUNCHER_PROGRESS_LOG": "1",
           "OPENPETE_LAUNCHER_SELECT_GAME": "spyro-1", "OPENPETE_LAUNCHER_SHOT": os.path.join(d, "shot"),
           "OPENPETE_LAUNCHER_MANIFEST": os.path.join(d, "shot")}
    start = cl.cpu_sample()
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "5"], env,
                        timeout=1500)
    others = cl.other_busy_cores(start)
    pts, ctl, stages = [], [], set()
    for mm in BAR_LINE.finditer(log):
        f, t, st, dn, tot = float(mm.group(1)), float(mm.group(2)), mm.group(3), int(mm.group(4)), int(mm.group(5))
        tag = next((p for p in FIRST_BOOT if st.startswith(p)), None)
        if not tag:
            continue
        stages.add(tag)
        pts.append((t, f))
        own = 1.0 if tag == "LINKING" else (min(1.0, dn / tot) if tot > 0 else 0.0)
        ctl.append((t, own))
    if len(pts) < 2:
        return verdict("progress", [f"{len(pts)} first-boot bar lines (rc {rc})"])
    t0, t1 = pts[0][0], pts[-1][0]
    if t1 <= t0:
        return verdict("progress", [f"the first boot spans {t0}..{t1} s"])
    dev, dctl = deviations(pts, t0, t1), deviations(ctl, t0, t1)
    bad = []
    missing = [s for s in FIRST_BOOT if s not in stages]
    if missing:
        bad.append(f"stages never drawn: {missing}")
    if max(dev) > PROGRESS_BAND:
        i = max(range(len(dev)), key=lambda k: dev[k])
        bad.append(f"deviation {max(dev):.3f} at point {i // 2} of {len(pts)} (band {PROGRESS_BAND})")
    if max(dctl) <= PROGRESS_BAND:
        bad.append(f"the per-stage count control stays within the band ({max(dctl):.3f})")
    if rc != 0:
        bad.append(f"exit {rc}")
    with open(os.path.join(d, "bar.tsv"), "w") as f:
        f.write("t\tweighted\tper_stage\n")
        for (t, w_), (_, c) in zip(pts, ctl):
            f.write(f"{t:.3f}\t{w_:.4f}\t{c:.4f}\n")
    detail = (f"{len(pts)} points over {t1 - t0:.1f} s, stages {sorted(stages)}: max deviation {max(dev):.3f} "
              f"(band {PROGRESS_BAND}), per-stage control {max(dctl):.3f}, {others:.1f} other busy cores")
    if others > PROGRESS_MAX_OTHERS and rc == 0 and not missing:
        raise cl.Skip(f"progress: {others:.1f} other busy cores over the run (over {PROGRESS_MAX_OTHERS}): the "
                      f"compile step's units stretch unevenly under load, so the bar's deviation measures the "
                      f"host ({detail})")
    return verdict("progress", bad, detail)


CHIP_LABEL = re.compile(r"level chip: (.+) \((\S+)/(\S+)\) at sky clock ([\d.]+) s")
CHIP_PRESS = re.compile(r"level chip: pressed at sky clock ([\d.]+) s")


def item(m, ident):
    """A manifest item by id, or None."""
    return next((it for it in m.get("items", []) if it.get("id") == ident), None)


def near(v, want, tol=1.0):
    """Whether a value is within a tolerance of another."""
    return abs(v - want) <= tol


def chip_rects(m, w, h, bad, shader_detail):
    """Check the corner chips' rects in a manifest against D15 and D16."""
    sc = layout_scale(w, h)
    sky, skyp = item(m, "status.skybox"), item(m, "status.skybox.panel")
    if not sky or not skyp:
        bad.append("no level chip in the sky shot's manifest")
    else:
        x0, y0, x1, y1 = skyp["rect"]
        if not (near(x1, w - CHIP_MARGIN * sc) and near(y1, h - CHIP_MARGIN * sc)):
            bad.append(f"level chip box {skyp['rect']}: right/bottom not {CHIP_MARGIN} layout px from the edges")
        if not (near(sky["rect"][0], x0 + CHIP_PAD_X * sc) and near(sky["rect"][1], y0 + CHIP_PAD_Y * sc)):
            bad.append(f"level chip text {sky['rect']} not padded 8 x 6 layout px in {skyp['rect']}")
    sh, shp, sp = item(m, "status.shaders"), item(m, "status.shaders.panel"), item(m, "status.shaders.spinner")
    if not sh:
        shader_detail.append("no shader chip in the sky shot (the compile had ended)")
        return
    x0, y0, x1, y1 = shp["rect"]
    if not (near(x0, CHIP_MARGIN * sc) and near(y1, h - CHIP_MARGIN * sc)):
        bad.append(f"shader chip box {shp['rect']}: left/bottom not {CHIP_MARGIN} layout px from the edges")
    if not sp:
        bad.append("shader chip without its spinner")
    else:
        s0, s1 = sp["rect"][0], sp["rect"][2]
        if not near(s1 - s0, CHIP_PX * sc) or not near(s0, x0 + CHIP_PAD_X * sc):
            bad.append(f"spinner {sp['rect']} not a {CHIP_PX} layout px square 8 layout px inside {shp['rect']}")
        if not near(sh["rect"][0], s1 + SPIN_GAP * sc):
            bad.append(f"shader chip text {sh['rect']} not {SPIN_GAP} layout px after the spinner {sp['rect']}")
    shader_detail.append(f"shader chip at {[round(v, 1) for v in shp['rect']]}")


def case_chips(a):
    """D15, D16 and D28: the corner chips' places and texts on the game page, and the level chip's click."""
    d = cl.case_dir(a.out, "chips")
    flow.seed_entry(d, "spyro-1", 1)
    rc, _ = flow.nox(a, d, "cache", ["--game", "spyro-1", "--frame-limit", "1"])
    if not flow.manifest(d, "spyro-1"):
        return verdict("chips", [f"the cache run wrote no cache (exit {rc})"], vacuous=True)
    base = os.path.join(d, "shot")
    env = {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_LAUNCHER_SHOT": base, "OPENPETE_LAUNCHER_MANIFEST": base,
           "OPENPETE_LAUNCHER_INPUT": f"{CLICK_MS}:click=-30/-30,{CLICK_MS + 16000}:quit"}
    rc, log = cl.launch(a.binary, d, "page", ["--launcher-window", "1280x800"], env, timeout=900)
    bad, shader_detail = [], []
    m, png = cl.read_json(base + ".sky.json"), base + ".sky.png"
    if m is None or not os.path.isfile(png):
        return verdict("chips", [f"no sky shot (exit {rc})"])
    w, h = m.get("window") or (1280, 800)
    chip_rects(m, w, h, bad, shader_detail)
    labels = [(mm.group(1), mm.group(3), float(mm.group(4))) for mm in CHIP_LABEL.finditer(log)]
    if not labels:
        bad.append("no level chip label logged")
    for name, lid, _ in labels:
        if name == lid or not re.search(r"[A-Z]", name):
            bad.append(f"label {name!r} for {lid} is not the table's display name")
    if "level chip: hovered, the hand cursor shows" not in log:
        bad.append("no hover of the level chip logged")
    press = CHIP_PRESS.search(log)
    timing = ""
    if not press:
        bad.append("no press of the level chip logged")
    else:
        tp = float(press.group(1))
        after = [t for _, _, t in labels if t > tp]
        if "sky: next dome on request" not in log:
            bad.append("the press started no crossfade")
        if len(after) < 2:
            bad.append(f"label changes after the press at {tp:.3f} s: {after}")
        else:
            d1, d2 = after[0] - tp, after[1] - tp
            timing = f"press {tp:.2f} s, labels +{d1:.2f} s and +{d2:.2f} s"
            if not near(d1, 1.25, CLOCK_TOL):
                bad.append(f"the clicked crossfade's label changed {d1:.3f} s after the press (want 1.25)")
            if not near(d2, 11.75, CLOCK_TOL):
                bad.append(f"the next automatic label changed {d2:.3f} s after the press (want 11.75)")
    return verdict("chips", bad, f"labels {[l[0] for l in labels]}; {timing}; {'; '.join(shader_detail)}")


CASES = {"corner": case_corner, "progress": case_progress, "chips": case_chips}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case", choices=sorted(CASES))
    ap.add_argument("--build-dir", default=os.path.join(cl.ROOT, "build", "linux-release"))
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    a.binary = os.path.join(a.build_dir, "openpete")
    a.out = a.out or os.path.join(a.build_dir, "launcher-tests")
    cl.WM = os.path.join(a.build_dir, "openpete_xvfb_wm")
    cl.install_reaper()
    if not os.access(a.binary, os.X_OK):
        print(f"[launcher-chips] SKIP: {a.binary} missing")
        return 64
    try:
        return CASES[a.case](a)
    except cl.Skip as e:
        print(f"[launcher-chips] SKIP: {e}")
        return 64


if __name__ == "__main__":
    sys.exit(main())
