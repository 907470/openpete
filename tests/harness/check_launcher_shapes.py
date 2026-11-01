#!/usr/bin/env python3
"""check_launcher_shapes.py - the launcher's shape pass (panels, chips, text scrims, the progress
bar), read from the binary's own readback: one case per ctest test of the label `launcher`.

Every run is check_launcher.py's windowed launch (its virtual display, window manager and scratch
data root). The fill and layout cases run over a library seeded with one Spyro 1 entry, picked at
once (OPENPETE_LAUNCHER_SELECT=0) with the shader compile forced (OPENPETE_PRECOMPILE_FORCE=1,
OPENPETE_PRECOMPILE_PANEL_MS=0). Two progress screens draw the step chip and the bar: the pick's
wait for the entry's launch-time asset job, at the launcher window's size (shot tag progress), and
the shader compile's after the hand-off, at the game's window size, 640x448 (tag precompile). Each
screen's first frame is read back (OPENPETE_LAUNCHER_SHOT) with its element manifest
(OPENPETE_LAUNCHER_MANIFEST). The contrast and fill cases run with OPENPETE_LAUNCHER_TEXT_HIDE=1
(imgui's output at alpha 0, so the shot holds the background and the shape pass alone); fill over
a fixed background set, each a flat clear colour (OPENPETE_LAUNCHER_CLEAR), contrast over the
level domes the launcher draws.

Cases:
  contrast  T3's method on the chip text over the launcher's own level domes: for every text item
            drawn on a chip (an id with an <id>.panel element), the WCAG ratio of its colour
            (white) over the 95th-percentile relative luminance of the readback under its rect
            (every second pixel per axis, as sweep_contrast.py's measureContrast samples). Each
            game whose disc image is present gets a library entry and its launcher asset cache
            (one headless run); per sampled level (--levels per game, evenly spaced over the
            cache's domes; default 6) the cache's domes.bin is rewritten to that level's dome
            alone, which also makes the cache stale, so the pick's launch-time asset job runs,
            held by OPENPETE_ASSET_JOB_DELAY_MS; the progress screen stays up over that dome
            and the first frame showing it opaque is read back with imgui's output hidden (shot
            and manifest tag sky). The same run with OPENPETE_LAUNCHER_SCRIM=0 (no text scrim)
            is the control. PASS when every item over every sampled dome is at 4.5:1 or more;
            the figures with 3:1 for a large item (px >= 24) are printed beside it. With --domes
            DIR the bench's rendered dome set (index.json) is the background instead, composed
            with the binary's shape layer as an affine map read from flat clears.
  fill      The chip's 50% fill against Spyro 3's dialogue-box blend: inside <id>.panel, away
            from its edge ramp, its corners, its drop shadow and the scrim, every pixel within 1
            of (background + (8,8,8)) / 2 per channel; inside <id>.scrim, away from its edge,
            within 1 of that blend * 0.88 + 8 * 0.12; outside the chip and its shadow the
            background is unchanged.
  layout    T1 on the shapes' ids at each reference preset: the bar's rect within 1 px of the
            reference layout's (tests/launcher/layout-reference.json, the progress states');
            the step chip's top and bottom within 1 px of the reference's; its left and right
            edges the step text's grown by the chip's padding (12 layout px) within 1 px (the
            step text differs from the reference's until the step names are in sentence case);
            the scrim's rect inside the chip.

Usage: tests/harness/check_launcher_shapes.py CASE [--build-dir DIR] [--out DIR] [--levels N] [--domes DIR]
Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (binary, xvfb-run or the disc image missing).
"""
import argparse
import math
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_launcher as cl  # noqa: E402

# The fixed background set: a grey ramp to white and colours of the level domes' skies, ground and
# haze, the launcher's own purple last.
BACKGROUNDS = [(0, 0, 0), (64, 64, 64), (128, 128, 128), (192, 192, 192), (224, 224, 224),
               (255, 255, 255), (135, 190, 235), (200, 225, 250), (240, 220, 170), (255, 250, 200),
               (90, 160, 60), (230, 140, 60), (24, 16, 40)]
SHADER_ENV = {"OPENPETE_PRECOMPILE_FORCE": "1", "OPENPETE_PRECOMPILE_PANEL_MS": "0",
              "OPENPETE_LAUNCHER_SELECT": "0"}
SCREENS = ["progress", "precompile"]
BOX = 8                     # Spyro 3's box colour (8,8,8)
CHIP_ALPHA = 0.5
SCRIM_ALPHA = 0.12
CHIP_PAD_X = 12.0           # the centred chip's padding, layout px
TEXT_COL = (255, 255, 255)  # chip text
CHECK_BG = (128, 64, 200)   # the third flat colour the shape layer's affine map is checked on
LINEAR_BOUND = 2.0          # levels: one rounding per blend stage (fill, scrim; shadow)
DOME_QUIT_MS = 7000         # the live-dome run's quit: past the first dome's 2.5 s fade-in


def rel_lum(r, g, b):
    """WCAG relative luminance of an sRGB byte colour (sweep_contrast.py's relLum)."""
    def f(c):
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b)


def ratio(a, b):
    """WCAG contrast ratio of two luminances."""
    return (max(a, b) + 0.05) / (min(a, b) + 0.05)


def run(a, d, tag, preset, env):
    """One seeded launch through both progress screens: ({screen: (manifest, RGB image)}, rc).

    A screen whose shot or manifest is missing is left out."""
    from PIL import Image
    r = os.path.join(d, tag)
    os.makedirs(r)
    cl.seed_library(r)
    base = os.path.join(r, "shot")
    e = dict(SHADER_ENV, OPENPETE_LAUNCHER_SHOT=base, OPENPETE_LAUNCHER_MANIFEST=base)
    e.update(env)
    rc, _ = cl.launch(a.binary, r, "run", ["--launcher-window", preset, "--frame-limit", "30"], e,
                      timeout=300)
    out = {}
    for sc in SCREENS:
        m, png = cl.read_json(base + "." + sc + ".json"), base + "." + sc + ".png"
        if m is not None and os.path.isfile(png):
            out[sc] = (m, Image.open(png).convert("RGB"))
    return out, rc


def bg_runs(a, d, case):
    """The fixed background set's runs with imgui's output hidden: [(background, screen, manifest, image)].

    Raises RuntimeError naming a background whose run drew neither screen."""
    res = []
    for bg in BACKGROUNDS:
        tag = "bg-%d-%d-%d" % bg
        shots, rc = run(a, d, tag, "1280x800", {"OPENPETE_LAUNCHER_TEXT_HIDE": "1",
                                                 "OPENPETE_LAUNCHER_CLEAR": "%d,%d,%d" % bg})
        if set(shots) != set(SCREENS):
            raise RuntimeError(f"{case} {tag}: screens {sorted(shots)} read back of {SCREENS} (rc {rc})")
        res += [(bg, sc, m, im) for sc, (m, im) in shots.items()]
    return res


def items(m):
    """The manifest's items by id."""
    return {it["id"]: it for it in (m or {}).get("items", [])}


def chip_texts(m):
    """The ids of the text items drawn on a chip."""
    by = items(m)
    return [i for i, it in by.items() if it["kind"] == "text" and i + ".panel" in by]


def p95_lum(im, rect):
    """The 95th-percentile relative luminance under a rect, every second pixel per axis, and the sample count."""
    w, h = im.size
    x0, y0, x1, y1 = (round(v) for v in rect)
    px = im.load()
    lums = sorted(rel_lum(*px[x, y]) for y in range(max(0, y0), min(h, y1), 2)
                  for x in range(max(0, x0), min(w, x1), 2))
    if not lums:
        return None, 0
    return lums[int(len(lums) * 0.95)], len(lums)


def shape_layer(a, d):
    """The binary's shape layer on the progress screen at 1280x800 as a per-pixel affine map of the
    background, out = bg * (1 - A) + C per channel, from two flat runs with imgui's output hidden
    (black gives C, white gives A), checked on a third flat colour.

    Returns (manifest, A, C, worst prediction error of the check colour) as numpy arrays."""
    import numpy as np
    fields = {}
    for bg in ((0, 0, 0), (255, 255, 255), CHECK_BG):
        shots, rc = run(a, d, "layer-%d-%d-%d" % bg, "1280x800",
                        {"OPENPETE_LAUNCHER_TEXT_HIDE": "1", "OPENPETE_LAUNCHER_CLEAR": "%d,%d,%d" % bg})
        if "progress" not in shots:
            raise RuntimeError(f"no progress screen read back over {bg} (rc {rc})")
        fields[bg] = (shots["progress"][0], np.asarray(shots["progress"][1], dtype=np.float64))
    m, c = fields[(0, 0, 0)]
    keep = 1.0 - (fields[(255, 255, 255)][1] - c) / 255.0   # A
    pred = np.asarray(CHECK_BG, dtype=np.float64) * (1.0 - keep) + c
    err = float(np.abs(pred - fields[CHECK_BG][1]).max())
    return m, keep, c, err


def dome_records(b):
    """The domes.bin records as [(level, bytes of the record)], or None when the file is malformed."""
    if b[:8] != b"OPDOMES1":
        return None
    n, at, out = int.from_bytes(b[8:12], "little"), 12, []
    for _ in range(n):
        start, ln = at, b[at]
        level = b[at + 1:at + 1 + ln].decode()
        at += 1 + ln
        nv, nt = int.from_bytes(b[at:at + 4], "little"), int.from_bytes(b[at + 4:at + 8], "little")
        at += 12 + 3 + nv * 9 + nt * 6
        out.append((level, b[start:at]))
    return out if at == len(b) else None


def sample(n, k):
    """k indices spread evenly over range(n), first and last included; all of them when k >= n."""
    if k >= n or k <= 0:
        return list(range(n))
    if k == 1:
        return [0]
    return sorted({round(i * (n - 1) / (k - 1)) for i in range(k)})


def dome_shot(a, root, tag, scrim):
    """One run over the cache's single dome: (manifest, RGB image) of the sky tag, or (None, reason)."""
    from PIL import Image
    base = os.path.join(root, tag)
    for ext in (".sky.png", ".sky.json"):
        if os.path.exists(base + ext):
            os.remove(base + ext)
    e = {"OPENPETE_LAUNCHER_SELECT": "0", "OPENPETE_ASSET_JOB_DELAY_MS": "60000",
         "OPENPETE_LAUNCHER_INPUT": "%d:quit" % DOME_QUIT_MS, "OPENPETE_LAUNCHER_TEXT_HIDE": "1",
         "OPENPETE_LAUNCHER_SHOT": base, "OPENPETE_LAUNCHER_MANIFEST": base}
    if not scrim:
        e["OPENPETE_LAUNCHER_SCRIM"] = "0"
    rc, log = cl.launch(a.binary, root, tag, ["--launcher-window", "1280x800"], e, timeout=300)
    m = cl.read_json(base + ".sky.json")
    if m is None or not os.path.isfile(base + ".sky.png"):
        return None, f"no sky shot (rc {rc}; {os.path.join(root, tag + '.log')})"
    if m.get("screen") != "sky" or "step" not in chip_texts(m):
        return None, f"the sky shot is not of the progress screen (texts {chip_texts(m)})"
    return (m, Image.open(base + ".sky.png").convert("RGB")), None


def item_ratios(m, im):
    """Per chip text item of a shot: (id, ratio of white over the p95 luminance under its rect, large)."""
    tl, out = rel_lum(*TEXT_COL), []
    by = items(m)
    for i in chip_texts(m):
        x0, y0, x1, y1 = by[i]["rect"]
        lum, n = p95_lum(im, by[i]["rect"])
        if n:
            out.append((i, ratio(tl, lum), (y1 - y0) >= 24))
    return out


def live_roots(a, d):
    """Per game with a disc image: (game, {scrim: data root}, its domes.bin records).

    Each root holds one library entry of the game and the cache a headless run wrote; the control's
    (scrim False) is a copy of it."""
    roots = []
    for g in cl.GAMES:
        if not os.path.isfile(os.path.join(cl.ROOT, "games", g, g, "build", cl.DISCS[g] + ".bin")):
            continue
        r = os.path.join(d, g)
        os.makedirs(r)
        cl.seed_library(r, (g,))
        e = {k: v for k, v in os.environ.items() if not k.startswith("OPENPETE_")}
        e.update({"XDG_DATA_HOME": os.path.join(r, "data"), "XDG_STATE_HOME": os.path.join(r, "state"),
                  "XDG_CONFIG_HOME": os.path.join(r, "config"), "OPENPETE_MSGBOX_SKIP": "1"})
        with open(os.path.join(r, "cache.log"), "w") as f:
            cl.run_session([a.binary, "--no-config", "--no-card", "--vibration", "0", "--mute", "1", "--headless",
                            "--game", g, "--frame-limit", "1"], e, f, 600)
        p = os.path.join(r, "state", "openpete", "cache", g, "domes.bin")
        recs = dome_records(open(p, "rb").read()) if os.path.isfile(p) else None
        if not recs:
            raise RuntimeError(f"{g}: the headless run wrote no domes ({os.path.join(r, 'cache.log')})")
        # The control's run gets its own copy of the root: two launches never share a data root.
        c = os.path.join(d, g + "-noscrim")
        shutil.copytree(r, c, symlinks=True)
        roots.append((g, {True: r, False: c}, recs))
    if not roots:
        raise cl.Skip("no disc image of any game")
    return roots


def case_contrast(a):
    """T3 on the chip text over the launcher's own level domes, with the no-scrim control.

    With --domes, the bench's dome set instead (contrast_bench)."""
    if a.domes:
        return contrast_bench(a)
    from concurrent.futures import ThreadPoolExecutor
    d = cl.case_dir(a.out, "contrast")
    try:
        roots = live_roots(a, d)
    except RuntimeError as e:
        print(f"[launcher] contrast: FAIL {e}")
        return 1
    res = {True: [], False: []}   # scrim: [(game, level, item, ratio, large)]
    for g, rs, recs in roots:
        for k in sample(len(recs), a.levels):
            level, rec = recs[k]
            for r in rs.values():
                with open(os.path.join(r, "state", "openpete", "cache", g, "domes.bin"), "wb") as f:
                    f.write(b"OPDOMES1" + (1).to_bytes(4, "little") + rec)
            tags = {True: f"{level}-scrim", False: f"{level}-noscrim"}
            with ThreadPoolExecutor(2) as ex:   # at most two game processes
                runs = {sc: ex.submit(dome_shot, a, rs[sc], tags[sc], sc) for sc in (True, False)}
            for sc, fut in runs.items():
                shot, why = fut.result()
                if shot is None:
                    print(f"[launcher] contrast: FAIL {g} {level} {tags[sc]}: {why}")
                    return 1
                res[sc] += [(g, level, i, q, lg) for i, q, lg in item_ratios(*shot)]
    if not res[True]:
        print("[launcher] contrast: VACUOUS (no chip text measured)")
        return 3
    for sc in (True, False):
        rows = res[sc]
        levels = {(g, lv) for g, lv, *_ in rows}
        bad = {(g, lv) for g, lv, _, q, _ in rows if q < 4.5}
        fails = sum(q < 4.5 for *_, q, _ in rows)
        fails_large = sum(q < (3.0 if lg else 4.5) for *_, q, lg in rows)
        w = min(rows, key=lambda t: t[3])
        print(f"[launcher] contrast {'scrim' if sc else 'no-scrim control'}: "
              f"{len(levels) - len(bad)} of {len(levels)} levels at 4.5:1, {len(rows) - fails} of {len(rows)} "
              f"item draws, {len(rows) - fails_large} with 3:1 for large; worst {w[3]:.2f}:1, {w[2]} on {w[0]} {w[1]}"
              + (f"; under 4.5:1: {', '.join(f'{g} {lv}' for g, lv in sorted(bad))}" if bad else ""))
    fails = sum(q < 4.5 for *_, q, _ in res[True])
    print(f"[launcher] contrast: {'FAIL' if fails else 'PASS'} (live domes of {', '.join(g for g, _, _ in roots)}; "
          f"chip text {', '.join(sorted({i for _, _, i, _, _ in res[True]}))})")
    return 1 if fails else 0


def contrast_bench(a):
    """T3 on the chip text over the level domes the bench renders (--domes), through the binary's shape layer."""
    import numpy as np
    if not os.path.isfile(os.path.join(a.domes, "index.json")):
        print(f"[launcher] SKIP: {a.domes} holds no index.json")
        return 64
    d = cl.case_dir(a.out, "contrast-bench")
    try:
        m, keep, c, err = shape_layer(a, d)
    except RuntimeError as e:
        print(f"[launcher] contrast: FAIL {e}")
        return 1
    if err > LINEAR_BOUND:
        print(f"[launcher] contrast: FAIL the shape layer is not an affine map of the background "
              f"({err:.1f} levels off on {CHECK_BG})")
        return 1
    idx = cl.read_json(os.path.join(a.domes, "index.json"))
    x0c, y0c, x1c, y1c = idx["crop"]
    by = items(m)
    texts = chip_texts(m)
    tl = rel_lum(*TEXT_COL)
    levels, n, fails, fails_large, worst = {}, 0, 0, 0, None
    lut = np.array([rel_lum(v, 0, 0) / 0.2126 for v in range(256)])   # the channel transfer, linear
    for shot in idx["shots"]:
        sky = np.fromfile(os.path.join(a.domes, shot["file"]), np.uint8).reshape(y1c - y0c, x1c - x0c, 3)
        for i in texts:
            x0, y0, x1, y1 = (round(v) for v in by[i]["rect"])
            if x0 < x0c or y0 < y0c or x1 > x1c or y1 > y1c:
                print(f"[launcher] contrast: FAIL {i}'s rect {by[i]['rect']} lies outside the dome crop {idx['crop']}")
                return 1
            ys, xs = slice(y0, y1, 2), slice(x0, x1, 2)
            bg = sky[y0 - y0c:y1 - y0c:2, x0 - x0c:x1 - x0c:2].astype(np.float64)
            out = np.clip(np.rint(bg * (1.0 - keep[ys, xs]) + c[ys, xs]), 0, 255).astype(int)
            lum = 0.2126 * lut[out[..., 0]] + 0.7152 * lut[out[..., 1]] + 0.0722 * lut[out[..., 2]]
            flat = np.sort(lum.ravel())
            q = ratio(tl, flat[int(len(flat) * 0.95)])
            large = (y1 - y0) >= 24
            n += 1
            fails += q < 4.5
            fails_large += q < (3.0 if large else 4.5)
            lv = levels.setdefault(shot["level"], [99.0, 0])
            lv[0] = min(lv[0], q)
            lv[1] += q < 4.5
            if worst is None or q < worst[0]:
                worst = (q, i, shot["level"], shot["yaw"])
    if n == 0:
        print("[launcher] contrast: VACUOUS (no chip text measured)")
        return 3
    passed = sum(1 for v in levels.values() if v[1] == 0)
    print(f"[launcher] contrast: shape layer affine within {err:.1f} levels on {CHECK_BG}; chip text {', '.join(texts)}")
    print(f"[launcher] contrast: {'FAIL' if fails else 'PASS'} ({passed} of {len(levels)} levels at 4.5:1, "
          f"{n - fails} of {n} item draws over {len(idx['shots'])} dome views, {n - fails_large} with 3:1 for large; "
          f"worst {worst[0]:.2f}:1, {worst[1]} on {worst[2]} at {worst[3]:g} deg)")
    return 1 if fails else 0


def inside_ellipse_rect(x, y, r, rx, ry, inset):
    """True when the pixel centre lies inside a rounded rect with elliptical corners shrunk by inset."""
    x0, y0, x1, y1 = r[0] + inset, r[1] + inset, r[2] - inset, r[3] - inset
    cx, cy = x + 0.5, y + 0.5
    if not (x0 <= cx <= x1 and y0 <= cy <= y1):
        return False
    ex = max(x0 + rx - cx, cx - (x1 - rx), 0.0)
    ey = max(y0 + ry - cy, cy - (y1 - ry), 0.0)
    if ex > 0 and ey > 0:
        return (ex / rx) ** 2 + (ey / ry) ** 2 <= 1.0
    return True


def fit_radii(r, scale):
    """The chip's corner radii as the launcher fits them (launcher_shape_panel)."""
    rx, ry = 22.8 * scale, 18.4 * scale
    k = min(1.0, (r[2] - r[0]) / 2 / rx, (r[3] - r[1]) / 2 / ry)
    return rx * k, ry * k


def case_fill(a):
    d = cl.case_dir(a.out, "fill")
    worst_chip = worst_scrim = worst_bg = 0
    counts = [0, 0, 0]
    bad = []
    try:
        shots = bg_runs(a, d, "fill")
    except RuntimeError as e:
        print(f"[launcher] FAIL {e}")
        return 1
    for bg, _, m, im in shots:
        by = items(m)
        scale = min(m["window"][0] / 640.0, m["window"][1] / 448.0)
        px = im.load()
        w, h = im.size
        chip = tuple((v + BOX) / 2.0 for v in bg)
        scrim = tuple(c * (1 - SCRIM_ALPHA) + BOX * SCRIM_ALPHA for c in chip)
        for i in chip_texts(m):
            pr = by[i + ".panel"]["rect"]
            rx, ry = fit_radii(pr, scale)
            sr = by.get(i + ".scrim", {}).get("rect")
            for y in range(int(pr[1]), int(math.ceil(pr[3]))):
                for x in range(int(pr[0]), int(math.ceil(pr[2]))):
                    if not inside_ellipse_rect(x, y, pr, rx, ry, 2.0):
                        continue
                    c = px[x, y]
                    if sr and sr[0] - 2 <= x + 0.5 <= sr[2] + 2 and sr[1] - 2 <= y + 0.5 <= sr[3] + 2:
                        if inside_ellipse_rect(x, y, sr, 3 * scale, 3 * scale, 2.0):
                            e = max(abs(c[k] - scrim[k]) for k in range(3))
                            worst_scrim = max(worst_scrim, e)
                            counts[1] += 1
                        continue
                    e = max(abs(c[k] - chip[k]) for k in range(3))
                    worst_chip = max(worst_chip, e)
                    counts[0] += 1
            # The background beside the chip, clear of its shadow (12 layout px out).
            out = 12 * scale
            for x, y in ((pr[0] - out, (pr[1] + pr[3]) / 2), (pr[2] + out, (pr[1] + pr[3]) / 2),
                         ((pr[0] + pr[2]) / 2, pr[1] - out)):
                c = px[int(x), int(y)]
                worst_bg = max(worst_bg, max(abs(c[k] - bg[k]) for k in range(3)))
                counts[2] += 1
    if worst_chip > 1:
        bad.append(f"chip fill {worst_chip:.1f} from (bg + 8) / 2")
    if worst_scrim > 1:
        bad.append(f"scrim {worst_scrim:.1f} from its blend")
    if worst_bg > 1:
        bad.append(f"background beside the chip moved by {worst_bg}")
    if min(counts) == 0:
        print(f"[launcher] fill: VACUOUS ({counts[0]} chip, {counts[1]} scrim, {counts[2]} background pixels)")
        return 3
    print(f"[launcher] fill: {'FAIL ' + '; '.join(bad) if bad else 'PASS'} (chip interior max |d| "
          f"{worst_chip:.2f} over {counts[0]} pixels, scrim {worst_scrim:.2f} over {counts[1]}, background "
          f"{worst_bg} over {counts[2]}; {len(BACKGROUNDS)} backgrounds)")
    return 1 if bad else 0


def reference_progress(preset):
    """The reference layout's bar and step chip on its first progress state at a preset."""
    ref = cl.read_json(cl.REFERENCE)
    for st in ref["presets"][preset]:
        by = {it["id"]: it for it in st["items"]}
        if "bar" in by and "step.panel" in by:
            return by
    return None


def case_layout(a):
    d = cl.case_dir(a.out, "shapes-layout")
    bad, compared = [], 0
    for preset in cl.PRESETS:
        shots, rc = run(a, d, preset, preset, {})
        m = shots.get("progress", (None, None))[0]
        ref = reference_progress(preset)
        by = items(m)
        if m is None or ref is None or "bar" not in by or "step.panel" not in by:
            bad.append(f"{preset}: no progress manifest with bar and step.panel (rc {rc})")
            continue
        if m["window"] != [int(v) for v in preset.split("x")]:
            bad.append(f"{preset}: the progress screen drew at {m['window']}")
            continue
        scale = min(m["window"][0] / 640.0, m["window"][1] / 448.0)
        checks = [("bar", k, by["bar"]["rect"][k], ref["bar"]["rect"][k]) for k in range(4)]
        checks += [("step.panel", k, by["step.panel"]["rect"][k], ref["step.panel"]["rect"][k]) for k in (1, 3)]
        t, p = by["step"]["rect"], by["step.panel"]["rect"]
        pad = CHIP_PAD_X * scale
        # The chip grows the line's unrounded origin, which the text rect truncates.
        checks += [("step.panel", 0, p[0], t[0] - pad), ("step.panel", 2, p[2], t[2] + pad)]
        for name, k, ours, want in checks:
            compared += 1
            if abs(ours - want) > 1.0:
                bad.append(f"{preset} {name}[{k}] {ours:.2f}, reference {want:.2f}")
        s, p = by.get("step.scrim", {}).get("rect"), by["step.panel"]["rect"]
        compared += 1
        if not s or not (p[0] <= s[0] and p[1] <= s[1] and s[2] <= p[2] and s[3] <= p[3]):
            bad.append(f"{preset}: step.scrim {s} not inside step.panel {p}")
    if compared == 0:
        print("[launcher] shapes layout: VACUOUS")
        return 3
    print(f"[launcher] shapes layout: {'FAIL ' + '; '.join(bad) if bad else 'PASS'} ({compared - len(bad)} of "
          f"{compared} checks over {len(cl.PRESETS)} presets: bar, step.panel, step.scrim)")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case", choices=["contrast", "fill", "layout"])
    ap.add_argument("--build-dir", default=os.path.join(cl.ROOT, "build", "linux-release"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--domes", default=None)
    ap.add_argument("--levels", type=int, default=6, help="levels sampled per game by contrast; 0 for all")
    a = ap.parse_args()
    a.binary = os.path.join(a.build_dir, "openpete")
    a.out = a.out or os.path.join(a.build_dir, "launcher-tests")
    cl.WM = os.path.join(a.build_dir, "openpete_xvfb_wm")
    cl.install_reaper()
    if not os.access(a.binary, os.X_OK):
        print(f"[launcher] SKIP: {a.binary} missing")
        return 64
    try:
        return {"contrast": case_contrast, "fill": case_fill, "layout": case_layout}[a.case](a)
    except cl.Skip as e:
        print(f"[launcher] SKIP: {e}")
        return 64


if __name__ == "__main__":
    sys.exit(main())
