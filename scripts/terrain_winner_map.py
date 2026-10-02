#!/usr/bin/env python3
"""Per-pixel terrain WINNER map: og (OT-dump painter raster) vs native
(stored-depth raster of the identity tri dump), attributed at FACE identity
granularity (sec/face/tier).

The tier-agreement instrument compares tier per pixel and is blind to
intra-tier winner flips and to coverage holes; this tool attributes both.
Inputs come from one native run per scene:

  OPENPETE_TERRAIN_TRIDUMP=<frame>,<path>   identity+depth of every fed tri
  OPENPETE_OT_DUMP_FRAMES=<frame-1>-<frame> OPENPETE_OT_DUMP_PATH=<path>
                                            the game's own OT (og truth)

Both legs are rasterised here with the SAME sampling rule (pixel centres in
walk space x2 vertical, i.e. 512x464 grid mapped from 512x240-8), so edge
pixels cancel instead of reading as disagreement.

Native winner rule mirrors the GPU (--model): 'offset' = min interpolated
stored depth (true vz + 128*otslots); 'keyresolve' = flat per-face og-key
raster + strict-less-by-half-slot
survivor filter, winner = min TRUE depth among survivors. og winner rule is
the PSX painter: prims draw in dump order (far -> near), last wins.

Census classes per both-covered pixel:
  agree           same (sec,face,tier) wins both legs
  intra-tier      matched faces, same tier, different face
  cross-tier      matched faces, different tier              (tier-map class)
  og-hole         og winner prim not fed by native (>=2 shared verts with a
                  native face => terrain coverage hole; else likely non-
                  terrain prim: moby/HUD/sky in the OT)
  native-only     og has no prim covering the pixel at all

Near-band models ('nbcontest', 'nbemit') mirror
game_layout.terrain.nearband_ot_keys = 0 / 1; see native_winner_map.
--compare OLD,NEW scores two models on the pixels where their winners
differ (compare_models). The og OT drawn at VBL N+2 is the one built by
the render whose tri dump fires at VBL N; the terrain polybuf interval of
that render comes from OPENPETE_RENDER_PATH_IVDUMP=N.

Usage:
  terrain_winner_map.py --ot ot.txt --tridump tri.txt [--og-frame N]
                        [--model M] [--heat out.png]
  terrain_winner_map.py --ot ot.txt --tridump tri.txt --og-frame N+2
                        --compare nbcontest,nbemit [--terrain-range LO-HI]
                        [--csv out.csv]
"""
import argparse
import collections
import sys

import numpy as np

# Walk space: 512 wide, 240 tall including the 8-line overscan skirt.
# Rasterise at x1 horizontally, x2 vertically for sub-line tier edges.
GW, GH = 512, 464   # y' = 2*(y-8), y in [8,240)

S16 = lambda v: (v & 0xFFFF) - 65536 if (v & 0xFFFF) >= 32768 else (v & 0xFFFF)

# POLY code families: (payload xy word indices, nverts). Colours/UVs skipped.
FAMS = {
    0x30: ([1, 3, 5], 3),        # POLY_G3
    0x38: ([1, 3, 5, 7], 4),     # POLY_G4
    0x34: ([1, 4, 7], 3),        # POLY_GT3
    0x3C: ([1, 4, 7, 10], 4),    # POLY_GT4
    0x20: ([1, 2, 3], 3),        # POLY_F3
    0x28: ([1, 2, 3, 4], 4),     # POLY_F4
    0x24: ([1, 3, 5], 3),        # POLY_FT3
    0x2C: ([1, 3, 5, 7], 4),     # POLY_FT4
}


CODES = collections.defaultdict(list)   # frame -> GPU code per poly prim
ADDRS = collections.defaultdict(list)   # frame -> polybuf address per poly prim


def parse_ot(path, want_frame):
    """-> (frame_used, [ (row_idx, [(x,y)..]) ] in dump (painter) order).
    Fills CODES/ADDRS (per frame, parallel to the prim list) for this dump."""
    CODES.clear()
    ADDRS.clear()
    frames = collections.defaultdict(list)
    for ln in open(path):
        if ln.startswith('#'):
            continue
        p = ln.strip().split(',')
        if len(p) < 7:
            continue
        fr = int(p[0])
        code = int(p[5], 16)
        fam = FAMS.get(code & 0xFC)
        if not fam:
            continue
        words = [int(x, 16) for x in p[6:]]
        idxs, nv = fam
        if max(idxs) >= len(words):
            continue
        xy = [(S16(words[i] & 0xFFFF), S16(words[i] >> 16)) for i in idxs]
        frames[fr].append(xy)
        CODES[fr].append(code)
        ADDRS[fr].append(int(p[3], 16))
    if not frames:
        sys.exit(f'no poly prims parsed from {path}')
    fr = want_frame if want_frame in frames else max(frames)
    return fr, list(enumerate(frames[fr]))


def parse_tridump(path):
    """-> list of dicts: identity + verts + true vz (world units). 16-column
    dumps predate the otkey/nearband columns (offset-era captures)."""
    tris = []
    for ln in open(path):
        if ln.startswith('#'):
            continue
        p = ln.strip().split(',')
        if len(p) not in (16, 18):
            continue
        sec, face, tier, fill, blend, textured, slots = (int(x) for x in p[:7])
        v = [(float(p[7 + 3 * k]), float(p[8 + 3 * k]), float(p[9 + 3 * k]))
             for k in range(3)]
        otkey = float(p[16]) if len(p) == 18 else 0.0
        nearband = int(p[17]) if len(p) == 18 else 0
        tris.append(dict(sec=sec, face=face, tier=tier, fill=fill,
                         blend=blend, textured=textured, slots=slots, v=v,
                         otkey=otkey, nearband=nearband))
    if not tris:
        sys.exit(f'no tris parsed from {path}')
    return tris


def to_grid(x, y):
    return x, 2.0 * (y - 8.0)


def raster(tri_xy, attr_z=None):
    """Barycentric coverage of pixel centres inside tri_xy (grid space).
    Returns (xs, ys, w) index arrays + weights, or None if degenerate."""
    (x0, y0), (x1, y1), (x2, y2) = tri_xy
    det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if abs(det) < 1e-9:
        return None
    lox = max(int(np.floor(min(x0, x1, x2))), 0)
    hix = min(int(np.ceil(max(x0, x1, x2))) + 1, GW)
    loy = max(int(np.floor(min(y0, y1, y2))), 0)
    hiy = min(int(np.ceil(max(y0, y1, y2))) + 1, GH)
    if lox >= hix or loy >= hiy:
        return None
    xs, ys = np.meshgrid(np.arange(lox, hix) + 0.5,
                         np.arange(loy, hiy) + 0.5)
    b1 = ((xs - x0) * (y2 - y0) - (x2 - x0) * (ys - y0)) / det
    b2 = ((x1 - x0) * (ys - y0) - (xs - x0) * (y1 - y0)) / det
    b0 = 1.0 - b1 - b2
    m = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
    if not m.any():
        return None
    iy, ix = np.nonzero(m)
    out = (ix + lox, iy + loy)
    if attr_z is None:
        return out
    z = b0[m] * attr_z[0] + b1[m] * attr_z[1] + b2[m] * attr_z[2]
    return out + (z,)


def og_winner_map(prims, codes=None):
    """Painter raster, fill = prim index + 1 (0 = uncovered). With `codes`
    (the prims' GPU codes), semi-transparent prims (code bit 1) are skipped:
    a blend over terrain does not replace the surface beneath it, and the
    native models skip blended terrain the same way."""
    win = np.zeros((GH, GW), np.int32)
    for idx, xy in prims:
        if codes is not None and codes[idx] & 0x02:
            continue
        pts = [to_grid(x, y) for x, y in xy]
        subs = [pts[:3]] if len(pts) == 3 else [[pts[0], pts[1], pts[2]],
                                               [pts[1], pts[3], pts[2]]]
        for s in subs:
            r = raster(s)
            if r:
                win[r[1], r[0]] = idx + 1
    return win


def face_key(t):
    mean = t['otkey'] if t['otkey'] > 0 else sum(vz for _, _, vz in t['v']) / 3.0
    return mean + 128.0 * t['slots']


KEY_MODELS = ('keyresolve', 'nbcontest', 'nbemit')


def native_winner_map(tris, model='offset'):
    """model='offset': winner = min interpolated stored depth
    (true vz + 128*slots).

    Key models: a flat per-face og-key raster (face_key = mean view-Z +
    128*slots; opaque, non-fill faces) forms the key image; a keyed face
    survives a pixel iff the image min is not more than half a slot (64)
    below its own key; winner = min TRUE depth among survivors. Fill faces
    never emit and never contest. Blended faces are skipped entirely.
    The three differ only in the near band (nearband column):
      keyresolve  near faces never emit and always survive at true depth.
      nbcontest   near faces never emit; each near fragment loses where the
                  image min is more than 64 below its OWN interpolated true
                  view-Z (op_key_loses koff<0 branch; the renderer with
                  nearband_ot_keys = 0).
      nbemit      near faces emit face_key and contest it exactly like
                  every other face (nearband_ot_keys = 1).
    None of the models sees moby/actor keys (akey/mkey), sky fans, or the
    fill stencil gate."""
    best = np.zeros((GH, GW), np.float64)
    win = np.full((GH, GW), -1, np.int32)
    kimg = None
    keyed = model in KEY_MODELS
    if keyed:
        kimg = np.full((GH, GW), 1e18, np.float64)
        for t in tris:
            if t['blend'] or t['fill']:
                continue
            if t['nearband'] and model != 'nbemit':
                continue
            xy = [to_grid(x, y) for x, y, _ in t['v']]
            r = raster(xy)
            if not r:
                continue
            ix, iy = r
            K = face_key(t)
            sel = K < kimg[iy, ix]
            kimg[iy[sel], ix[sel]] = K
    for i, t in enumerate(tris):
        if t['blend']:
            continue
        xy = [to_grid(x, y) for x, y, _ in t['v']]
        if keyed:
            q = [1.0 / max(vz, 1.0) for _, _, vz in t['v']]
        else:
            q = [1.0 / max(vz + 128.0 * t['slots'], 1.0) for _, _, vz in t['v']]
        r = raster(xy, q)
        if not r:
            continue
        ix, iy, z = r
        if keyed and not t['fill']:
            if t['nearband'] and model == 'keyresolve':
                keep = None
            elif t['nearband'] and model == 'nbcontest':
                keep = kimg[iy, ix] >= (1.0 / np.maximum(z, 1e-12)) - 64.0
            else:
                keep = kimg[iy, ix] >= face_key(t) - 64.0
            if keep is not None:
                ix, iy, z = ix[keep], iy[keep], z[keep]
                if not len(ix):
                    continue
        upd = z > best[iy, ix]
        wix, wiy = ix[upd], iy[upd]
        best[wiy, wix] = z[upd]
        win[wiy, wix] = i
    return win, best


SNAP_R = 2.0   # og verts are PSX fixed-point GTE, native is float: offsets
               # up to ~1.4px are normal. Snap og verts to the nearest native
               # vertex within this radius.


def build_identity_index(tris):
    """Exact native float vert -> spatial hash bucket; native tri lookup by
    frozenset of exact vert pairs; per-vert tri sets for partial matches."""
    ident = {}
    vert_tris = collections.defaultdict(set)
    grid = collections.defaultdict(list)
    for i, t in enumerate(tris):
        vs = tuple((x, y) for x, y, _ in t['v'])
        ident.setdefault(frozenset(vs), i)
        for v in vs:
            if v not in vert_tris:
                grid[(int(v[0]) // 2, int(v[1]) // 2)].append(v)
            vert_tris[v].add(i)
    return ident, vert_tris, grid


def snap_all(v, grid):
    """All native verts within SNAP_R of v, sorted nearest-first."""
    x, y = v
    bx, by = int(x) // 2, int(y) // 2
    out = []
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for (nx, ny) in grid.get((bx + dx, by + dy), ()):
                d = (nx - x) ** 2 + (ny - y) ** 2
                if d <= SNAP_R * SNAP_R:
                    out.append((d, (nx, ny)))
    out.sort()
    return [v for _, v in out[:4]]


def match_og_prims(prims, ident, vert_tris, grid, tris):
    """og prim index -> ('face', tri_idx) | ('hole', None) | ('other', None).

    For each og vert take every fed native vert within SNAP_R (the og path is
    PSX fixed-point GTE, native is float; dense meshes put several candidates
    inside the radius). The prim is identified by the FED TRI whose exact
    3-vert set is formed by one candidate per vert (either quad diagonal),
    minimising total snap distance. 'hole': >=2 og verts have fed-vert
    neighbours but no fed tri owns the prim — geometry the feed knows, face
    it did not emit. 'other': a non-terrain prim (moby, sky, HUD)."""
    out = {}
    for idx, xy in prims:
        cands = [snap_all(v, grid) for v in xy]
        got = sum(1 for c in cands if c)
        if got < 2:
            out[idx] = ('other', None)
            continue
        tri_ids = ([(0, 1, 2)] if len(xy) == 3 else
                   [(0, 1, 2), (1, 3, 2), (0, 1, 3), (0, 2, 3)])
        best, bd = None, 1e18
        for ids in tri_ids:
            if any(not cands[i] for i in ids):
                continue
            for a in cands[ids[0]]:
                for b in cands[ids[1]]:
                    for c in cands[ids[2]]:
                        k = frozenset((a, b, c))
                        if len(k) < 3 or k not in ident:
                            continue
                        d = sum((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2
                                for p, q in ((a, xy[ids[0]]), (b, xy[ids[1]]),
                                             (c, xy[ids[2]])))
                        if d < bd:
                            best, bd = ident[k], d
        out[idx] = ('face', best) if best is not None else ('hole', None)
    return out


def compare_models(a, tris, classify, prims, fr, ogmap, ogmatch, on_face):
    """Score two native winner models against og on the pixels where their
    winning FACE identities (sec,face,tier) differ ('disputed' pixels).

    og's winner prim at a disputed pixel is attributed in three ways:
      foreign   its polybuf address lies outside --terrain-range (the
                terrain pass's polybuf interval, OPENPETE_RENDER_PATH_IVDUMP):
                moby, Spyro, shadow, cyclorama, particle prims. Out of domain.
      exact     the prim is an exact native tri (match_og_prims 'face'):
                a model is correct iff its winner is that (sec,face).
      footprint a terrain prim with no exact native tri (near-band
                subdivision/near-clip tessellations differ between the PSX
                and native): a model is credited iff every og corner lies on
                its winner face's fed footprint and NOT on the other model's
                winner face (og_prim_on_face). On both / neither: ambiguous.
    Calibration of the footprint leg: on disputed pixels whose og prim is
    exact, the footprint verdict is computed too and tallied against the
    exact verdict (fpcal agree/wrong/ambiguous).
    Without --terrain-range every non-exact prim goes to the footprint leg."""
    old, new = a.compare.split(',')
    om, oq = native_winner_map(tris, old)
    nm, nq = native_winner_map(tris, new)
    fid = np.array([(t['sec'] << 20) | (t['face'] << 1) | (t['tier'] & 1)
                    for t in tris] + [-1], np.int64)
    dis = fid[om] != fid[nm]
    nd = int(dis.sum())
    rng = None
    if a.terrain_range:
        lo, hi = (int(x, 16) for x in a.terrain_range.split('-'))
        rng = (lo, hi)
    addrs = ADDRS[fr]
    C = collections.Counter()
    pairs = collections.Counter()
    rep = {}
    ys, xs = np.nonzero(dis)
    for y, x in zip(ys, xs):
        oi, ni = int(om[y, x]), int(nm[y, x])
        kk = (int(fid[oi]), int(fid[ni]))
        rep.setdefault(kk, (oi, ni))
        pairs[(kk, 'px')] += 1
        o = int(ogmap[y, x])
        if o == 0:
            C['og-none'] += 1
            continue
        pi = o - 1
        if rng and not (rng[0] <= addrs[pi] < rng[1]):
            C['foreign'] += 1
            continue
        fa = oi >= 0 and on_face(pi, (tris[oi]['sec'], tris[oi]['face']))
        fb = ni >= 0 and on_face(pi, (tris[ni]['sec'], tris[ni]['face']))
        fpv = 'old' if (fa and not fb) else 'new' if (fb and not fa) else \
              'amb' if (fa and fb) else 'none'
        kind, ti = ogmatch[pi]
        if kind == 'face':
            og_f = (tris[ti]['sec'], tris[ti]['face'])
            ex = ('old' if oi >= 0 and og_f == (tris[oi]['sec'], tris[oi]['face'])
                  else 'new' if ni >= 0 and og_f == (tris[ni]['sec'], tris[ni]['face'])
                  else 'none')
            C[f'exact-{ex}'] += 1
            pairs[(kk, f'exact-{ex}')] += 1
            if ex != 'none':
                C['fpcal-' + ('agree' if fpv == ex else
                              'amb' if fpv in ('amb', 'none') else 'wrong')] += 1
            continue
        C[f'fp-{fpv}'] += 1
        pairs[(kk, f'fp-{fpv}')] += 1
    old_ok = C['exact-old'] + C['fp-old']
    new_ok = C['exact-new'] + C['fp-new']
    lab = a.label or a.tridump
    print(f'== {lab} (og frame {fr}, {len(prims)} og prims, {len(tris)} native tris)')
    print(f'  disputed px {nd}: og-none {C["og-none"]}  foreign {C["foreign"]}')
    print(f'    exact:     {old} {C["exact-old"]}  {new} {C["exact-new"]}  '
          f'neither {C["exact-none"]}')
    print(f'    footprint: {old} {C["fp-old"]}  {new} {C["fp-new"]}  '
          f'ambiguous {C["fp-amb"]}  none {C["fp-none"]}')
    print(f'    fp calibration on exact px: agree {C["fpcal-agree"]}  '
          f'wrong {C["fpcal-wrong"]}  ambiguous {C["fpcal-amb"]}')
    print(f'    total: {old}-correct {old_ok}  {new}-correct {new_ok}  '
          f'net {new_ok - old_ok:+d}')

    def desc(i):
        if i < 0:
            return 'none'
        t = tris[i]
        mean = sum(v for _, _, v in t['v']) / 3.0
        return (f's{t["sec"]}/f{t["face"]}/T{t["tier"]} nb{t["nearband"]} '
                f'sl{t["slots"]} fill{t["fill"]} vz{mean:.0f} key{face_key(t):.0f}')
    top = sorted(((v, k[0]) for k, v in pairs.items() if k[1] == 'px'),
                 reverse=True)[:a.pairs]
    for c, kk in top:
        oi, ni = rep[kk]
        g = lambda s_: pairs[(kk, s_)]
        print(f'    {c:>6} px  old-ok {g("exact-old") + g("fp-old"):>6} '
              f'new-ok {g("exact-new") + g("fp-new"):>6} '
              f'(ex {g("exact-old")}/{g("exact-new")} fp {g("fp-old")}/{g("fp-new")} '
              f'amb {g("fp-amb")})  | {old}: {desc(oi)}  ->  {new}: {desc(ni)}')
    if a.csv:
        cols = ['disputed', 'og_none', 'foreign', 'exact_old', 'exact_new',
                'exact_none', 'fp_old', 'fp_new', 'fp_amb', 'fp_none',
                'fpcal_agree', 'fpcal_wrong', 'fpcal_amb', 'old_ok', 'new_ok',
                'net']
        vals = [nd, C['og-none'], C['foreign'], C['exact-old'],
                C['exact-new'], C['exact-none'], C['fp-old'], C['fp-new'],
                C['fp-amb'], C['fp-none'], C['fpcal-agree'],
                C['fpcal-wrong'], C['fpcal-amb'], old_ok, new_ok,
                new_ok - old_ok]
        import os
        fresh = not os.path.exists(a.csv)
        with open(a.csv, 'a') as f:
            if fresh:
                f.write('label,' + ','.join(cols) + '\n')
            f.write(lab + ',' + ','.join(str(v) for v in vals) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ot', required=True)
    ap.add_argument('--tridump', required=True)
    ap.add_argument('--og-frame', type=int, default=-1)
    ap.add_argument('--model', choices=('offset',) + KEY_MODELS,
                    default='offset', help='native winner rule to mirror')
    ap.add_argument('--compare', metavar='OLD,NEW',
                    help='score two models against og on the pixels where '
                         'their native winners differ')
    ap.add_argument('--csv', help='with --compare: append one summary row')
    ap.add_argument('--terrain-range', metavar='0xLO-0xHI',
                    help='with --compare: polybuf interval of the terrain '
                         'pass that built the og frame; og prims outside it '
                         'are foreign (moby/Spyro/sky/particles)')
    ap.add_argument('--pairs', type=int, default=12,
                    help='with --compare: face pairs to list')
    ap.add_argument('--heat', help='write class heat PNG')
    ap.add_argument('--label', default='')
    a = ap.parse_args()

    fr, prims = parse_ot(a.ot, a.og_frame)
    tris = parse_tridump(a.tridump)
    ident, vert_tris, grid = build_identity_index(tris)
    ogmap = og_winner_map(prims, CODES[fr] if a.compare else None)
    ogmatch = match_og_prims(prims, ident, vert_tris, grid, tris)

    # Face-level vert sets: subdivided children keep their parent's
    # (sec,face), so the union of a face's tri verts contains the parent's
    # corners. Used to resolve og's near-band OT prims: the visible pixels
    # there come from subdivided HQ twins DrawPrim'd under mask-bit
    # protection, and the OT carries only the UNSUBDIVIDED copies — which
    # exact-tri matching can never hit.
    face_verts = collections.defaultdict(set)
    face_tris2d = collections.defaultdict(list)
    for t in tris:
        for x, y, _ in t['v']:
            face_verts[(t['sec'], t['face'])].add((x, y))
        face_tris2d[(t['sec'], t['face'])].append(
            tuple((x, y) for x, y, _ in t['v']))

    def point_on_face(px, py, fkey, margin=2.0):
        """px,py within `margin` PIXELS of some tri of the face: signed
        distance to each edge line (oriented by the tri's winding) must be
        >= -margin. Pixel-space, so it holds for arbitrarily large tris."""
        import math
        for (v0, v1, v2) in face_tris2d.get(fkey, ()):
            (x0, y0), (x1, y1), (x2, y2) = v0, v1, v2
            det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if abs(det) < 1e-9:
                continue
            s = 1.0 if det > 0 else -1.0
            ok = True
            for (ax, ay), (bx, by) in ((v0, v1), (v1, v2), (v2, v0)):
                ex, ey = bx - ax, by - ay
                el = math.hypot(ex, ey)
                if el < 1e-9:
                    ok = False
                    break
                d = s * (ex * (py - ay) - ey * (px - ax)) / el
                if d < -margin:
                    ok = False
                    break
            if ok:
                return True
        return False

    _opf_cache = {}

    def og_prim_on_face(og_idx, fkey):
        """>=min(3, nverts) og corners within SNAP_R of the face's verts."""
        ck = (og_idx, fkey)
        if ck in _opf_cache:
            return _opf_cache[ck]
        _opf_cache[ck] = r = _og_prim_on_face(og_idx, fkey)
        return r

    def _og_prim_on_face(og_idx, fkey):
        """Same surface iff every og corner sits on (or within snap noise of)
        the face's fed footprint. Handles both subdivision asymmetries: an og
        parent over native children, and og OT children (midpoint verts) over
        a native parent — the near band's mask-bit HQ twin pattern. Known
        limit: an UNFED nearer face lying wholly inside the winner's
        footprint would false-agree; such holes are only counted where they
        straddle the winner's silhouette."""
        xy = prims[og_idx][1]
        vs = face_verts.get(fkey)
        if not vs:
            return False
        if all(any((nx, ny) in vs for (nx, ny) in snap_all((ox, oy), grid))
               or point_on_face(ox, oy, fkey) for (ox, oy) in xy):
            return True
        # Reverse direction: the native winner face is a subdivision CHILD
        # of the og prim (its own face record, distinct id), so the og
        # parent's corners straddle sibling children — but every child vert
        # sits inside the parent's footprint. Near-band mask-twin regime:
        # the OT carries the unsubdivided fallback; visible og pixels come
        # from the DrawPrim'd HQ twins, so footprint identity is the best
        # the OT dump can attest.
        import math
        ring = xy if len(xy) == 3 else [xy[0], xy[1], xy[3], xy[2]]
        n = len(ring)
        area2 = sum(ring[i][0] * ring[(i + 1) % n][1] -
                    ring[(i + 1) % n][0] * ring[i][1] for i in range(n))
        if abs(area2) < 1e-9:
            return False
        s = 1.0 if area2 > 0 else -1.0
        for (fx, fy) in vs:
            for i in range(n):
                (ax, ay), (bx, by) = ring[i], ring[(i + 1) % n]
                ex, ey = bx - ax, by - ay
                el = math.hypot(ex, ey)
                if el < 1e-9 or s * (ex * (fy - ay) - ey * (fx - ax)) / el < -2.0:
                    return False
        return True
    # Identity matching degrades where faces shrink toward the snap radius:
    # a far vista packs many native verts inside SNAP_R and the exact-tri
    # lookup misses, inflating og-hole. Band the census by the native
    # winner's stored view-Z so near/mid attribution
    # is read separately from the far-field noise floor.
    BANDS = ((0.0, 8192.0, 'near'), (8192.0, 32768.0, 'mid'),
             (32768.0, 1e18, 'far'))

    def band_of(q):
        vz = 1.0 / q if q > 0 else 1e18
        for lo, hi, nm in BANDS:
            if lo <= vz < hi:
                return nm
        return 'far'

    def classify(natmap, natq):
        cls = np.zeros((GH, GW), np.uint8)   # 0 none,1 agree,2 intra,3 cross,
                                             # 4 og-hole,5 og-foreign,6 native-only
        C = collections.Counter()
        for y in range(GH):
            ogrow, natrow = ogmap[y], natmap[y]
            for x in range(GW):
                o, n = ogrow[x], natrow[x]
                if o == 0:
                    if n >= 0:
                        cls[y, x] = 6; C['native-only'] += 1
                    continue
                kind, oi = ogmatch[o - 1]
                if kind == 'other':
                    # non-terrain og winner (moby/sky/HUD). Over native terrain
                    # that is a cross-DOMAIN contest (not this instrument's
                    # domain); over nothing it is outside the domain entirely.
                    if n >= 0:
                        cls[y, x] = 5; C['og-foreign'] += 1
                    continue
                if kind == 'hole' or n < 0:
                    if (n >= 0 and
                            og_prim_on_face(o - 1, (tris[n]['sec'],
                                                    tris[n]['face']))):
                        # og's unsubdivided OT copy of the face whose subdivided
                        # twin wins natively — same surface, agree.
                        cls[y, x] = 1; C['agree'] += 1
                        C[f'agree/{band_of(natq[y, x])}'] += 1
                        continue
                    cls[y, x] = 4; C['og-hole'] += 1
                    continue
                to_, tn = tris[oi], tris[n]
                if (to_['sec'], to_['face']) == (tn['sec'], tn['face']):
                    cls[y, x] = 1; C['agree'] += 1; k = 'agree'
                elif to_['tier'] == tn['tier']:
                    cls[y, x] = 2; C['intra-tier'] += 1; k = 'intra-tier'
                else:
                    cls[y, x] = 3; C['cross-tier'] += 1; k = 'cross-tier'
                C[f'{k}/{band_of(natq[y, x])}'] += 1

        for y in range(GH):        # band the holes too (native winner depth,
            for x in range(GW):    # or far when native has no coverage)
                if cls[y, x] == 4:
                    q = natq[y, x] if natmap[y, x] >= 0 else 0.0
                    C[f'og-hole/{band_of(q)}'] += 1
        return cls, C

    if a.compare:
        compare_models(a, tris, classify, prims, fr, ogmap, ogmatch,
                       og_prim_on_face)
        return
    natmap, natq = native_winner_map(tris, a.model)
    cls, C = classify(natmap, natq)
    dom = sum(C.get(k, 0) for k in ('agree', 'intra-tier', 'cross-tier',
                                    'og-hole'))
    lab = a.label or a.tridump
    print(f'== {lab} (og frame {fr}, {len(prims)} og prims, {len(tris)} native tris)')
    for k in ('agree', 'intra-tier', 'cross-tier', 'og-hole', 'og-foreign',
              'native-only'):
        v = C.get(k, 0)
        pct = 100.0 * v / dom if dom else 0
        bands = ''
        if k in ('agree', 'intra-tier', 'cross-tier', 'og-hole'):
            bands = '  [' + ' '.join(f'{nm}:{C.get(f"{k}/{nm}", 0)}'
                                     for nm in ('near', 'mid', 'far')) + ']'
        note = '' if k in ('agree', 'intra-tier', 'cross-tier', 'og-hole') \
               else ' (of terrain domain, informational)'
        print(f'  {k:<12} {v:>8}  {pct:6.2f}%{note}{bands}')
    print(f'  terrain-domain px: {dom}')
    # The near band cannot always be attributed from the OT dump: og's
    # visible near pixels are DrawPrim'd HQ subdivision twins under mask-bit
    # protection, and the OT carries fallback tessellations that can
    # cross-cut the native subdivision (neither footprint contains the
    # other). The near band carries no authored offsets in either
    # tier, so no stored-depth change acts there — the GATE metric is the
    # mid+far population, where the offsets actually live.
    gm = {k: C.get(f'{k}/mid', 0) + C.get(f'{k}/far', 0)
          for k in ('agree', 'intra-tier', 'cross-tier', 'og-hole')}
    gd = sum(gm.values())
    if gd:
        print(f'  GATE mid+far: agree {100.0 * gm["agree"] / gd:.2f}%  '
              f'intra {gm["intra-tier"]}  cross {gm["cross-tier"]}  '
              f'hole {gm["og-hole"]}  ({gd} px)')

    if a.heat:
        from PIL import Image
        pal = np.array([[0, 0, 0], [40, 40, 40], [255, 0, 0], [255, 160, 0],
                        [0, 120, 255], [90, 0, 130], [0, 255, 90]], np.uint8)
        Image.fromarray(pal[cls]).save(a.heat)
        print(f'  heat -> {a.heat}  (grey=agree red=intra orange=cross '
              f'blue=og-hole purple=og-other green=native-only)')


if __name__ == '__main__':
    main()
