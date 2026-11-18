#!/usr/bin/env python3
"""measure_uv.py - measure which texel the reference GPU reads at each pixel
of a textured triangle and of a textured sprite.

Plays textured triangles (GP0 0x25, raw texture, opaque) and sprites
(GP0 0x65) on duckstation-regtest's software GPU (probe_run.py) over two
15-bit direct texture pages whose texels hold their own coordinates: page A
(VRAM x 512) holds 0x8000 | u | (v & 0x7F) << 8, page B (VRAM x 768) holds
0x8000 | u | (v >> 1) << 8. Every case is drawn once on each page; a pixel's
texel is read off the two halfwords (bit 15 set marks a drawn pixel, so a
texel never reads as the transparent 0x0000). Writes
tests/unit/data/gpu-uv/uv.texels.

Cases: the textured 2D triangles and sprites a census of the three games
recorded (--census FILE..., `[uvc]` lines), then a synthetic sweep: sprites
over sizes and start texels, axis-aligned quads (fed as the two triangles
(0,1,2), (1,3,2)) unmirrored, mirrored, scaled and skewed in u and v, and
seeded random triangles. Checks reported in the header: the same random
triangles moved to another position, with their vertices in another order,
and the synthetic quads drawn as one GP0 0x2D quad.

Usage (openpete checkout root):
    tools/gpu_blackbox/measure_uv.py WORKDIR [--census LOG ...] [--out PATH] [--full-out PATH]
    tools/gpu_blackbox/measure_uv.py WORKDIR --verify tests/unit/data/gpu-uv/uv.texels
"""
import argparse
import collections
import pathlib
import random
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from probe_run import run_cases, applied_head, DENSE   # noqa: E402

PAGE = {"A": (512, 0x108), "B": (768, 0x10C)}


def tex_page(which):
    """A setup case uploading one 256x256 15-bit page."""
    x, _ = PAGE[which]
    words = [0xA0000000, (0 << 16) | x, (256 << 16) | 256]
    for v in range(256):
        hv = (v & 0x7F) if which == "A" else (v >> 1)
        for u in range(0, 256, 2):
            a = 0x8000 | u | (hv << 8)
            b = 0x8000 | (u + 1) | (hv << 8)
            words.append(a | (b << 16))
    return (0, 0, 0, 0, words)


def window(xs, ys):
    x0 = (min(xs) - 2) // 16 * 16
    x1 = -(-(max(xs) + 3) // 16) * 16
    y0, y1 = min(ys) - 2, max(ys) + 3
    assert x0 >= 0 and y0 >= 0 and x1 <= 512 and y1 <= 512, (xs, ys)
    return x0, y0, x1 - x0, (y1 - y0) | DENSE


def tri_case(t, page):
    """t = ((x, y, u, v) * 3) -> a GP0 0x25 case on page."""
    _, tp = PAGE[page]
    (a, b, c) = t
    words = [0x25808080,
             (a[1] & 0xFFFF) << 16 | (a[0] & 0xFFFF), a[2] | a[3] << 8,
             (b[1] & 0xFFFF) << 16 | (b[0] & 0xFFFF), b[2] | b[3] << 8 | tp << 16,
             (c[1] & 0xFFFF) << 16 | (c[0] & 0xFFFF), c[2] | c[3] << 8]
    return window([p[0] for p in t], [p[1] for p in t]) + (words,)


def quad_case(q, page):
    """q = 4 corners (x, y, u, v) in GP0 order -> a GP0 0x2D case."""
    _, tp = PAGE[page]
    words = [0x2D808080]
    for i, p in enumerate(q):
        words.append((p[1] & 0xFFFF) << 16 | (p[0] & 0xFFFF))
        words.append(p[2] | p[3] << 8 | ((tp << 16) if i == 1 else 0))
    return window([p[0] for p in q], [p[1] for p in q]) + (words,)


def sprite_case(s, page):
    x, y, w, h, u, v = s
    _, tp = PAGE[page]
    words = [0xE1000400 | tp, 0x65808080, (y << 16) | x, u | v << 8, (h << 16) | w]
    return window([x, x + w - 1], [y, y + h - 1]) + (words,)


def decode(pa, pb):
    """Two page readbacks -> {(x, y): (u, v)}; raises on an inconsistent pixel."""
    if set(pa) != set(pb):
        raise SystemExit("pages A and B cover different pixels")
    out = {}
    for k, a in pa.items():
        b = pb[k]
        if not (a & 0x8000 and b & 0x8000) or (a & 0xFF) != (b & 0xFF) or ((a >> 9) & 0x3F) != ((b >> 8) & 0x3F):
            raise SystemExit("pixel %s: halfwords %04x %04x do not decode" % (k, a, b))
        out[k] = (a & 0xFF, ((b >> 8) & 0x7F) << 1 | ((a >> 8) & 1))
    return out


def place(pts, at=(32, 16)):
    """Translates points so the bounding box starts near `at`, keeping x mod 16 and y mod 2."""
    mx, my = min(p[0] for p in pts), min(p[1] for p in pts)
    dx = at[0] + (mx % 16) - mx
    dy = at[1] + (my % 2) - my
    return dx, dy


def sprite_from_tri(t):
    xs, ys = [p[0] for p in t], [p[1] for p in t]
    x, y = min(xs), min(ys)
    w, h = max(xs) - x, max(ys) - y
    u = next(p[2] for p in t if p[0] == x)
    v = next(p[3] for p in t if p[1] == y)
    return (x, y, w, h, u, v)


def read_census(paths):
    tris, sprites = collections.OrderedDict(), collections.OrderedDict()
    pat = re.compile(r"^\[uvc\] (\d) m([0-9A-F]+) w([0-9A-F]+) (\S+) (\S+) (\S+) @(-?\d+),(-?\d+)")
    flags = collections.Counter()
    for p in paths:
        for line in open(p, errors="replace"):
            m = pat.match(line)
            if not m:
                continue
            src, mode, twin = int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16)
            flags[(src, mode, twin)] += 1
            ox, oy = int(m.group(7)), int(m.group(8))
            t = tuple(tuple(int(v) for v in m.group(i).split(",")) for i in (4, 5, 6))
            t = tuple((p[0] + ox, p[1] + oy, p[2], p[3]) for p in t)
            dx, dy = place(t)
            t = tuple((p[0] + dx, p[1] + dy, p[2], p[3]) for p in t)
            if src == 1:
                s = sprite_from_tri(t)
                sprites[s] = 1
            else:
                tris[t] = 1
    return list(tris), list(sprites), flags


def synthetic(rng):
    sprites, tris = [], []
    for w in (1, 2, 3, 8, 17, 64):
        for h in (1, 2, 3, 8, 17, 64):
            for (u, v) in ((0, 0), (1, 255), (127, 3), (200, 200), (255, 128)):
                sprites.append((32 + (w % 16), 16 + (h % 2), w, h, u, v))
    quads = []
    for (w, h) in ((1, 1), (2, 3), (8, 8), (16, 15), (37, 20), (64, 64), (255, 16)):
        for (us, vs) in ((1, 1), (-1, 1), (1, -1), (-1, -1), (2, 1), (1, 2), (0.5, 1), (1, 0.5), (3 / 7, 1), (1, 5 / 9)):
            for u0 in (0, 3, 120):
                du = round(w * abs(us)) if w * abs(us) <= 255 else 255
                dv = round(h * abs(vs)) if h * abs(vs) <= 255 else 255
                ul, ur = (u0, u0 + du) if us > 0 else (u0 + du, u0)
                vt, vb = (u0 // 2, u0 // 2 + dv) if vs > 0 else (u0 // 2 + dv, u0 // 2)
                if max(ul, ur) > 255 or max(vt, vb) > 255:
                    continue
                x, y = 32 + (w * 7 % 16), 16 + (h % 2)
                q = ((x, y, ul, vt), (x + w, y, ur, vt), (x, y + h, ul, vb), (x + w, y + h, ur, vb))
                quads.append(q)
    for k in range(200):
        w, h = rng.randint(1, 40), rng.randint(1, 40)
        x, y = 40, 20
        sk = rng.randint(-6, 6)
        q = ((x, y, rng.randint(0, 255), rng.randint(0, 255)), (x + w, y + sk, rng.randint(0, 255), rng.randint(0, 255)),
             (x - sk, y + h, rng.randint(0, 255), rng.randint(0, 255)), (x + w - sk, y + h + sk, rng.randint(0, 255), rng.randint(0, 255)))
        quads.append(q)
    for q in quads:
        tris.append((q[0], q[1], q[2]))
        tris.append((q[1], q[3], q[2]))
    rnd = []
    for k in range(600):
        span = rng.choice((4, 16, 48))
        t = tuple((40 + rng.randint(0, span), 20 + rng.randint(0, span), rng.randint(0, 255), rng.randint(0, 255))
                  for _ in range(3))
        rnd.append(t)
    return sprites, tris, rnd


def run_pairs(cases_fn, items, work, log):
    """Draws every item on both pages; returns {(x, y): (u, v)} per item."""
    # Each boot re-uploads the pages: run_cases splits only between cases, so
    # chunk here, with the uploads at the head of every chunk, by output size.
    out = []
    budget = 1200 * 1024
    i = 0
    while i < len(items):
        chunk = [tex_page("A"), tex_page("B")]
        used = 0
        j = i
        while j < len(items):
            ca, cb = cases_fn(items[j], "A"), cases_fn(items[j], "B")
            cost = 2 * (ca[2] * (ca[3] & 0xFFFF) * 2 + 8 * len(ca[4]) + 64)
            if j > i and used + cost > budget:
                break
            chunk += [ca, cb]
            used += cost
            j += 1
        res = run_cases(chunk, work, log=lambda m: None, single_boot=True)[2:]
        for k in range(0, len(res), 2):
            out.append(decode(res[k], res[k + 1]))
        log("  items %d..%d of %d" % (i, j - 1, len(items)))
        i = j
    return out


def rle(vals):
    if not vals:
        return "-"
    parts = [str(vals[0])]
    d = [((b - a + 128) % 256) - 128 for a, b in zip(vals, vals[1:])]
    i = 0
    while i < len(d):
        j = i
        while j < len(d) and d[j] == d[i]:
            j += 1
        parts.append("%d*%d" % (d[i], j - i) if j - i > 1 else str(d[i]))
        i = j
    return ",".join(parts)


def rows(px):
    out = []
    by_y = collections.defaultdict(list)
    for (x, y) in px:
        by_y[y].append(x)
    for y in sorted(by_y):
        xs = sorted(by_y[y])
        start = 0
        for k in range(1, len(xs) + 1):
            if k == len(xs) or xs[k] != xs[k - 1] + 1:
                run = xs[start:k]
                us = [px[(x, y)][0] for x in run]
                vs = [px[(x, y)][1] for x in run]
                out.append("R %d %d %d %s %s" % (y, run[0], len(run), rle(us), rle(vs)))
                start = k
    return out


def parse_table(path):
    """The cases of a uv.texels table: [(kind, inputs, {(x, y): (u, v)})]."""
    cases = []
    for line in open(path):
        f = line.split()
        if not f or f[0].startswith("#"):
            continue
        if f[0] == "T":
            v = [int(x) for x in f[1:]]
            cases.append(("T", tuple(tuple(v[4 * i:4 * i + 4]) for i in range(3)), {}))
        elif f[0] == "S":
            cases.append(("S", tuple(int(x) for x in f[1:]), {}))
        else:
            y, x, n = int(f[1]), int(f[2]), int(f[3])
            for which, code in ((0, f[4]), (1, f[5])):
                parts = code.split(",")
                val = int(parts[0])
                vals = [val]
                for p in parts[1:]:
                    d, _, r = p.partition("*")
                    for _ in range(int(r or 1)):
                        val += int(d)
                        vals.append(val)
                assert len(vals) == n
                for i, t in enumerate(vals):
                    px = cases[-1][2].setdefault((x + i, y), [0, 0])
                    px[which] = t & 0xFF
    return [(k, inp, {xy: tuple(t) for xy, t in px.items()}) for (k, inp, px) in cases]


def verify(path, work, log):
    cases = parse_table(path)
    tris = [c for c in cases if c[0] == "T"]
    sprs = [c for c in cases if c[0] == "S"]
    t_px = run_pairs(tri_case, [c[1] for c in tris], work, log)
    s_px = run_pairs(sprite_case, [c[1] for c in sprs], work, log)
    bad = [c[1] for c, p in zip(tris + sprs, t_px + s_px) if p != c[2]]
    log("verify %s: %d triangle and %d sprite cases re-measured, %d differ" % (path, len(tris), len(sprs), len(bad)))
    for b in bad[:8]:
        log("  differs: %s" % (b,))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--census", nargs="*", default=[])
    ap.add_argument("--out", default="tests/unit/data/gpu-uv/uv.texels")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--census-keep", type=int, default=300,
                    help="census triangles written to --out (seeded sample); --full-out gets all")
    ap.add_argument("--full-out", default=None)
    ap.add_argument("--verify", default=None,
                    help="re-measure every case of this table and report the cases that differ; writes nothing")
    a = ap.parse_args()
    work = pathlib.Path(a.work)
    log = print
    rng = random.Random(a.seed)
    if a.verify:
        return verify(a.verify, work, log)
    c_tris, c_sprites, flags = read_census(a.census)
    s_sprites, s_tris, rnd = synthetic(rng)
    sprites = c_sprites + [s for s in s_sprites if s not in c_sprites]
    tris = c_tris + s_tris + rnd
    log("cases: %d census triangles, %d census sprites, %d synthetic sprites, %d synthetic quad triangles, %d random triangles"
        % (len(c_tris), len(c_sprites), len(sprites) - len(c_sprites), len(s_tris), len(rnd)))
    t_px = run_pairs(tri_case, tris, work, log)
    s_px = run_pairs(sprite_case, sprites, work, log)
    facts = []
    empty_s = sum(1 for p in s_px if not p)
    if empty_s:
        raise SystemExit("%d sprites drew nothing" % empty_s)
    # Moved: the random triangles 300 px right and 211 px down.
    mv = [tuple((p[0] + 300 + 0, p[1] + 211, p[2], p[3]) for p in t) for t in rnd]
    mv_px = run_pairs(tri_case, mv, work, log)
    base = t_px[len(c_tris) + len(s_tris):]
    d = sum(1 for p, q in zip(base, mv_px) if {(x + 300, y + 211): t for (x, y), t in p.items()} != q)
    facts.append("# the random triangles moved by (+300, +211): %d of %d differ in pixels or texels." % (d, len(rnd)))
    # Vertex order: every other order of the random triangles' vertices.
    perms = ((1, 2, 0), (2, 0, 1), (0, 2, 1), (2, 1, 0), (1, 0, 2))
    pr = [tuple(t[i] for i in pm) for t in rnd[:300] for pm in perms]
    pr_px = run_pairs(tri_case, pr, work, log)
    pr_diff = [k for k, q in enumerate(pr_px) if q != base[k // len(perms)]]
    d = len(pr_diff)
    pr_keep = sorted(set(pr_diff) | set(range(40 * len(perms))))
    facts.append("# the first 300 random triangles in each of the 5 other vertex orders: %d of %d differ." % (d, len(pr)))
    # Quads: the synthetic quads as one GP0 0x2D against their two triangles.
    nq = len(s_tris) // 2
    quads = [(s_tris[2 * k][0], s_tris[2 * k][1], s_tris[2 * k][2], s_tris[2 * k + 1][1]) for k in range(nq)]
    q_px = run_pairs(quad_case, quads, work, log)
    off = len(c_tris)
    d = 0
    for k in range(nq):
        u = dict(t_px[off + 2 * k])
        u.update(t_px[off + 2 * k + 1])
        overlap = set(t_px[off + 2 * k]) & set(t_px[off + 2 * k + 1])
        if u != q_px[k] or overlap:
            d += 1
    facts.append("# the synthetic quads as one GP0 0x2D quad: %d of %d differ from their two triangles' pixels and texels (or the triangles overlap)." % (d, nq))
    for f in facts:
        log(f)
    empty_t = sum(1 for p in t_px if not p)
    lines = [
        "# The texel the reference GPU reads at each pixel of a textured triangle or sprite: a measurement,",
        "# not a derivation. Taken by tools/gpu_blackbox/measure_uv.py: a PS-EXE (tools/gpu_blackbox/probe.c)",
        "# booted in the oracle reference test binary duckstation-regtest (third_party/duckstation-patches,",
        "# applied head %s), software GPU renderer, uploads two 15-bit texture pages whose" % applied_head(),
        "# texels encode their own (u, v), draws each case on both (GP0 0x25 raw-texture opaque triangle, or",
        "# GP0 0x65 raw-texture sprite after GP0 0xE1; texture window none, drawing area (0, 0)-(1023, 511),",
        "# offset 0, no dither) and reads VRAM back (GP0 0xC0); main RAM is dumped at the end (regtest -ram-dump-at).",
        "# Re-measure these cases and compare (openpete checkout root, a few minutes):",
        "#   tools/gpu_blackbox/measure_uv.py WORKDIR --verify tests/unit/data/gpu-uv/uv.texels",
        "# Regenerate from a new census: tools/gpu_blackbox/measure_uv.py WORKDIR --census <logs> (the logs'",
        "# [uvc] lines: each textured 2D triangle fed to the texel selection in ui_sprite.c, as source, draw-mode",
        "# bits 11-13, texture window word, vertex 0's u v, vertices 1 and 2 relative to vertex 0 with u v, @vertex 0).",
        "# Cases: %d census triangles and %d census sprites (the textured 2D the three games fed over their" % (len(c_tris), len(c_sprites)),
        "# attract demos and menu, inventory, dialogue, atlas, guidebook and credits movies, moved to near (32, 16)",
        "# keeping x mod 16 and y mod 2), %d synthetic sprites, %d synthetic quad triangles, %d random triangles." % (
            len(sprites) - len(c_sprites), len(s_tris), len(rnd)),
        "# Census flags seen (source 4 = POLY_FT4 triangle, 1 = SPRT; draw-mode flip bits; texture window word):",
    ] + ["#   source %d mode-bits %04X window %08X: %d distinct triangles" % (k[0], k[1], k[2], n) for k, n in sorted(flags.items())] + [
        "# Every sprite drew; %d of %d triangles drew no pixel (zero or sub-pixel area)." % (empty_t, len(tris)),
        "# Checks against the cases below:",
    ] + facts + [
        "# After the triangle cases above come %d of the reordered random triangles: those that differ from" % len(pr_keep),
        "# their first order (%d) and every order of the first 40; then the sprite cases." % len(pr_diff),
        "# Rows: T x0 y0 u0 v0 x1 y1 u1 v1 x2 y2 u2 v2 opens a triangle case (vertex order of the GP0 command);",
        "# S x y w h u v opens a sprite case; each following R y x n U V is a run of n drawn pixels of row y",
        "# from column x. U and V list the run's texel coordinates: the first value, then the steps between",
        "# neighbours (mod 256, in -128..127), d*r for r equal steps d. A case lists every drawn pixel.",
    ]
    keep = set(range(len(tris)))
    if len(c_tris) > a.census_keep:
        drop = set(range(len(c_tris))) - set(random.Random(a.seed).sample(range(len(c_tris)), a.census_keep))
        keep -= drop
    lines_full = list(lines)
    lines.insert(lines.index("# Checks against the cases below:"),
                 "# This file holds a seeded sample of %d of the %d census triangles (--census-keep, --seed %d)."
                 % (min(a.census_keep, len(c_tris)), len(c_tris), a.seed))
    for k, (t, px) in enumerate(zip(tris, t_px)):
        body = ["T " + " ".join("%d %d %d %d" % p for p in t)] + rows(px)
        lines_full += body
        if k in keep:
            lines += body
    for k in pr_keep:
        body = ["T " + " ".join("%d %d %d %d" % p for p in pr[k])] + rows(pr_px[k])
        lines += body
        lines_full += body
    for s, px in zip(sprites, s_px):
        body = ["S %d %d %d %d %d %d" % s] + rows(px)
        lines += body
        lines_full += body
    if a.full_out:
        pathlib.Path(a.full_out).write_text("\n".join(lines_full) + "\n")
    out = pathlib.Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")
    log("wrote %s: %d triangle cases, %d sprite cases, %d pixels" % (
        out, len(tris), len(sprites), sum(len(p) for p in t_px) + sum(len(p) for p in s_px)))


if __name__ == "__main__":
    sys.exit(main())
