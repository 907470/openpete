#!/usr/bin/env python3
"""check_launcher_title.py - the launcher's 3D title checks, one case per ctest test of the label
`launcher`, beside check_launcher_flow.py (whose launch helpers, virtual display and scratch data
root every case here uses).

Cases:
  title-cache  The 3D title drawn from a game's launcher asset cache (T7, T2, T1 on title.moby).
            A Spyro 1 entry's cache is written by a headless launch; then, at each reference
            window preset, a windowed launch over that library, with the title's tick pinned
            (OPENPETE_LAUNCHER_TITLE_TICK=TICK), reads the glyph models from the cache's
            glyphs.txt, draws the 3D title from the launcher's second present (the first holds it
            back and draws no title) and writes its dump (OPENPETE_LAUNCHER_TITLE_DUMP) and the
            manifest and shot of the first frame drawing it (the tag "title3d"); the selector's
            first-frame manifest holds neither title. The entry holds no module
            (a build without the first-boot pipeline links the game; on a *-launcher build the
            module is removed before the run), so the title is drawn from the cache alone.
            Per preset: the manifest's title.moby rect against the reference layout's within
            1 px on every edge, and no pixel title; in the shot, the rect holds drawn pixels and
            no pixel of the pixel title's orange. At 1280x800 the dump is checked against a model
            of the emitter written here from glyphs.txt and the font's light terms in
            games/spyro-1/renderer/layout.c (the layout, the wobble yaw, the turning light and
            the lit colour of each glyph's front face): each glyph's posed rect within
            RECT_TOL px and its front face colour within COLOUR_TOL per channel; a second run
            of the same build writes an identical dump (the tolerance's calibration: the
            emitter is deterministic, so the bound covers only the model's double precision
            against the emitter's float).
  title-switch  The title switch on the assets-ready tick (T8): an empty library, windowed; the
            scripted drop of Spyro 1's image is ingested and its asset extraction runs; the
            reap that logs the assets ready reads the glyph models and switches the title in
            the same call (the three lines adjacent in the log), and the next present draws
            the 3D title. The drop screen's manifest holds the pixel title and the selector's
            the 3D title.

Usage: tests/harness/check_launcher_title.py CASE [--build-dir DIR] [--out DIR]
Exit 0 PASS, 1 FAIL, 64 SKIP (binary, xvfb-run or a disc image missing).
"""
import argparse
import json
import math
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_launcher as cl  # noqa: E402
import check_launcher_flow as fl  # noqa: E402

ROOT = cl.ROOT
TICK = 37
# A posed glyph's rect edge against the model's, in pixels: float32 vertices of magnitude up to
# ~2600 px carry ~2e-4 px of rounding; the bound is fifty times that.
RECT_TOL = 0.01
# A face colour channel against the model's: the emitter truncates float32 terms to int, so a
# term within float32 rounding of a whole number may land one level away.
COLOUR_TOL = 1
TITLE = "OPENPETE"


def reference_title(preset):
    """The reference layout's title.moby rect of Spyro 1's font at a preset, or None."""
    with open(cl.REFERENCE) as f:
        ref = json.load(f)
    found = []

    def walk(o):
        if isinstance(o, dict):
            if o.get("id") == "title.moby" and o.get("font") == "spyro-1":
                found.append(o["rect"])
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    walk(ref["presets"][preset])
    return found[0] if found else None


def font_terms(game):
    """The font's model height and light terms from a game's layout.c."""
    with open(os.path.join(ROOT, "games", game, "renderer", "layout.c")) as f:
        src = f.read()
    blk = src[src.index(".font = {"):]
    num = r"(-?[\d.]+)f?"
    model_h = float(re.search(r"\.model_h\s*=\s*" + num, blk).group(1))
    add = [float(x) for x in re.search(r"\.light_add\s*=\s*\{\s*" + num + r",\s*" + num + r",\s*" + num,
                                       blk).groups()]
    hi_t = float(re.search(r"\.hi_threshold\s*=\s*" + num, blk).group(1))
    hi_g = float(re.search(r"\.hi_gain\s*=\s*" + num, blk).group(1))
    return model_h, add, hi_t, hi_g


def read_glyphs(path):
    """glyphs.txt's slots: {slot: (vertices, faces)}, faces as (indices, normal)."""
    out = {}
    with open(path) as f:
        for line in f:
            if not line.startswith("slot "):
                continue
            head, rest = line.split(" v ", 1)
            vs, fs = rest.split(" f ", 1)
            verts = [tuple(int(c) for c in t.split(",")) for t in vs.split()]
            faces = []
            for t in fs.split():
                i, n = t.split(":")
                faces.append((tuple(int(c) for c in i.split(",")), tuple(int(c) for c in n.split(","))))
            out[int(head.split()[1])] = (verts, faces)
    return out


def model_title(glyphs, terms, dump):
    """The emitter's title as modelled here: per glyph (rect, front face, (r, g, b))."""
    model_h, add, hi_t, hi_g = terms
    slots = [10 + ord(c) - ord("A") for c in TITLE]
    widths = [max(v[0] for v in glyphs[s][0]) - min(v[0] for v in glyphs[s][0]) for s in slots]
    scale = (dump["width"] - (len(slots) - 1) * dump["gap"]) / sum(widths)
    tick = dump["tick"]
    a = ((tick * 3) & 0xFF) * (2 * math.pi / 256)
    lx, ly, lz = math.cos(a) * 0.968, math.sin(a) * 1.549, -0.25
    ll = math.sqrt(lx * lx + ly * ly + lz * lz)
    light = (lx / ll, ly / ll, lz / ll)
    base = (0xB4, 0xB4, 0x3E)
    cursor, out = dump["x0"], []
    for gi, s in enumerate(slots):
        verts, faces = glyphs[s]
        xmin, xmax = min(v[0] for v in verts), max(v[0] for v in verts)
        yaw = math.cos((((tick * 8) + gi * 12) & 0xFF) * (2 * math.pi / 256)) * 0.589
        cs, sn = math.cos(yaw), math.sin(yaw)
        mid = 0.5 * (xmin + xmax)
        xs, ys = [], []
        for vx, vy, vz in verts:
            dx = vx - mid
            xs.append(cursor + (mid + dx * cs + vz * sn - xmin) * scale)
            ys.append(dump["cy"] + vy * scale)
        best, bz = 0, -2.0
        for k, (_, n) in enumerate(faces):
            ln = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
            if ln > 0 and n[2] / ln > bz:
                best, bz = k, n[2] / ln
        nx0, ny, nz0 = faces[best][1]
        nx, nz = nx0 * cs + nz0 * sn, -nx0 * sn + nz0 * cs
        nl = math.sqrt(nx * nx + ny * ny + nz * nz)
        t = max(-1.0, min(1.0, (nx * light[0] + ny * light[1] + nz * light[2]) / nl))
        hi = int((t - hi_t) * hi_g) if t > hi_t else 0
        rgb = tuple(max(0, min(255, base[c] + int(add[c] * t) + hi)) for c in range(3))
        out.append(((min(xs), min(ys), max(xs), max(ys)), best, rgb))
        cursor += (xmax - xmin) * scale + dump["gap"]
    return out


def check_model(dump, glyphs, terms):
    """Problems of a dump against the model, and the largest rect and colour deviations."""
    bad, dr, dc = [], 0.0, 0
    model = model_title(glyphs, terms, dump)
    if len(dump["glyphs"]) != len(model):
        return [f"dump holds {len(dump['glyphs'])} glyphs, the model {len(model)}"], dr, dc
    for i, (g, (rect, face, rgb)) in enumerate(zip(dump["glyphs"], model)):
        d = max(abs(g["rect"][k] - rect[k]) for k in range(4))
        v = int(g["rgba"], 16)
        got = (v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF)
        c = max(abs(got[k] - rgb[k]) for k in range(3))
        dr, dc = max(dr, d), max(dc, c)
        if d > RECT_TOL:
            bad.append(f"glyph {i}: rect {g['rect']} model {[round(x, 4) for x in rect]}")
        if g["face"] != face or c > COLOUR_TOL:
            bad.append(f"glyph {i}: face {g['face']} {got}, model face {face} {rgb}")
    return bad, dr, dc


def shot_title(png, rect):
    """In a launcher shot: the pixels inside the rect other than the background, and the pixel title's orange ones."""
    from PIL import Image
    im = Image.open(png).convert("RGB")
    px = im.load()
    x0, y0, x1, y1 = (int(round(v)) for v in rect)
    drawn = orange = 0
    for y in range(max(0, y0), min(im.size[1], y1)):
        for x in range(max(0, x0), min(im.size[0], x1)):
            c = px[x, y]
            drawn += c != cl.BG
            orange += c == cl.TITLE_ORANGE
    return drawn, orange


def title_run(a, d, tag, preset, extra=None):
    """One windowed launch over the case's library at a preset with the title's tick pinned; returns (rc, log, manifest, dump, shot)."""
    base = os.path.join(d, tag)
    dump = base + ".title.json"
    for p in (dump, base + ".selector.json", base + ".title3d.json", base + ".title3d.png"):
        if os.path.exists(p):
            os.remove(p)
    env = {"OPENPETE_LAUNCHER_MANIFEST": base, "OPENPETE_LAUNCHER_SHOT": base,
           "OPENPETE_LAUNCHER_TITLE_TICK": str(TICK), "OPENPETE_LAUNCHER_TITLE_DUMP": dump,
           "OPENPETE_LAUNCHER_INPUT": "2500:quit"}
    env.update(extra or {})
    rc, log = cl.launch(a.binary, d, tag, ["--launcher-window", preset], env, timeout=300)
    return rc, log, cl.read_json(base + ".title3d.json"), cl.read_json(dump), base + ".title3d.png"


def case_title_cache(a):
    """T7, T2 and T1 on title.moby: the 3D title drawn from the cache, against the reference and the model."""
    d = cl.case_dir(a.out, "title-cache")
    e = fl.seed_entry(d, "spyro-1", 1)
    rc, _ = fl.nox(a, d, "prime", ["--game", "spyro-1", "--frame-limit", "1"])
    glyphs_txt = os.path.join(fl.cache_dir(d, "spyro-1"), "glyphs.txt")
    if rc != 0 or not os.path.isfile(glyphs_txt):
        return fl.verdict("title-cache", [f"the priming launch (exit {rc}) wrote no {glyphs_txt}"])
    shutil.rmtree(os.path.join(e, "modules"), ignore_errors=True)
    bad, rows = [], []
    glyphs, terms = read_glyphs(glyphs_txt), font_terms("spyro-1")
    for preset in cl.PRESETS:
        rc, log, man, dump, png = title_run(a, d, "p-" + preset, preset)
        tag = preset
        if not re.search(r"title glyph models of spyro-1 read from .*glyphs\.txt", log):
            bad.append(f"{tag}: the glyph models were not read from the cache")
        if "3D title of spyro-1 drawn" not in log or "launcher title pass: first composite" not in log:
            bad.append(f"{tag}: the 3D title was not drawn")
        # A first-boot build starts the entry's launch-time module rebuild and stops it at the quit,
        # which may leave the stamp directory; the module file is what the title must not need.
        if fl.stamped(d, "spyro-1"):
            bad.append(f"{tag}: the entry holds a module")
        ref = reference_title(preset)
        moby, pixel = (man and [it for it in man.get("items", []) if it["id"] == "title.moby"]), \
            (man and [it for it in man.get("items", []) if it["id"] == "title"])
        first = cl.read_json(os.path.join(d, "p-" + preset) + ".selector.json")
        held = first and [it for it in first.get("items", []) if it["id"] in ("title", "title.moby")]
        if not first or held:
            bad.append(f"{tag}: the selector's first frame {'holds a title' if first else 'has no manifest'}")
        if not man or not moby:
            bad.append(f"{tag}: the title frame's manifest holds no title.moby")
            continue
        if pixel:
            bad.append(f"{tag}: the title frame's manifest holds the pixel title too")
        r = moby[0]["rect"]
        dev = max(abs(r[k] - ref[k]) for k in range(4)) if ref else None
        if ref is None or dev > 1.0:
            bad.append(f"{tag}: title.moby {r} reference {ref}")
        drawn, orange = shot_title(png, r) if os.path.isfile(png) else (0, -1)
        if drawn == 0 or orange != 0:
            bad.append(f"{tag}: shot {drawn} drawn pixels in the rect, {orange} pixel-title pixels")
        rows.append(f"{preset} rect dev {dev:.3f} px, {drawn} px drawn")
        if preset == "1280x800":
            if not dump:
                bad.append(f"{tag}: no title dump")
                continue
            mb, dr, dc = check_model(dump, glyphs, terms)
            bad += [f"{tag}: {x}" for x in mb]
            _, _, _, dump2, _ = title_run(a, d, "again-" + preset, preset)
            if dump2 != dump:
                bad.append(f"{tag}: a second run's dump differs")
            rows.append(f"model: {len(dump['glyphs'])} glyphs, rect dev {dr:.5f} px, colour dev {dc}, "
                        f"cap {dump['cap']:.2f} px, {dump['verts']} vertices, tick {dump['tick']}")
    for r in rows:
        print(f"[launcher-title] {r}")
    return fl.verdict("title-cache", bad)


def case_title_switch(a):
    """T8's title switch: the reap logging the assets ready switches the title; the next present draws it."""
    d = cl.case_dir(a.out, "title-switch")
    cue = fl.disc_cue("spyro-1")
    if not os.path.isfile(fl.disc_bin("spyro-1")):
        raise cl.Skip(f"{fl.disc_bin('spyro-1')} missing")
    base = os.path.join(d, "run")
    env = {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_LAUNCHER_DROP": cue, "OPENPETE_LAUNCHER_MANIFEST": base,
           "OPENPETE_LAUNCHER_SELECT_GAME": "spyro-1", "OPENPETE_LAUNCHER_SELECT_IDLE": "1",
           "OPENPETE_LAUNCHER_PICK_DELAY_MS": "1500"}
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    bad = fl.common_flow(log) + fl.ordered(
        log, r"scripted drop", r"\[library\] added", r"launcher assets of spyro-1 ready \(ingest\)",
        r"title glyph models of spyro-1 read from", r"launcher title: the 3D title of spyro-1 \(assets ready\)",
        r"launcher title: 3D title of spyro-1 drawn", r"game picked: spyro-1")
    # The sky's join (check_launcher_flow.py sky-reveal) logs in the same reap; its line is set aside.
    lines = [ln for ln in log.splitlines() if ("[launcher]" in ln or "launcher" in ln) and " sky: " not in ln]
    idx = next((i for i, ln in enumerate(lines) if re.search(r"launcher assets of spyro-1 ready \(ingest\)", ln)),
               None)
    adj = idx is not None and idx + 2 < len(lines) and "title glyph models of spyro-1 read" in lines[idx + 1] \
        and "the 3D title of spyro-1 (assets ready)" in lines[idx + 2]
    if not adj:
        bad.append("the title did not switch in the reap that logged the assets ready")
    drop, sel = cl.read_json(base + ".drop.json"), cl.read_json(base + ".selector.json")
    ids = lambda m: {it["id"] for it in (m or {}).get("items", [])}  # noqa: E731
    if "title" not in ids(drop) or "title.moby" in ids(drop):
        bad.append(f"the drop screen's manifest ids {sorted(ids(drop))}: expected the pixel title")
    if "title.moby" not in ids(sel) or "title" in ids(sel):
        bad.append(f"the selector's manifest ids {sorted(ids(sel))}: expected the 3D title")
    if rc != 0:
        bad.append(f"exit {rc}")
    return fl.verdict("title-switch", bad)


CASES = {"title-cache": case_title_cache, "title-switch": case_title_switch}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case", choices=sorted(CASES))
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build", "linux-release"))
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    a.binary = os.path.join(a.build_dir, "openpete")
    a.out = a.out or os.path.join(a.build_dir, "launcher-tests")
    cl.WM = os.path.join(a.build_dir, "openpete_xvfb_wm")
    cl.install_reaper()
    if not os.access(a.binary, os.X_OK):
        print(f"[launcher-title] SKIP: {a.binary} missing")
        return 64
    try:
        return CASES[a.case](a)
    except cl.Skip as e:
        print(f"[launcher-title] SKIP: {e}")
        return 64


if __name__ == "__main__":
    sys.exit(main())
