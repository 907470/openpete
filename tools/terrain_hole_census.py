#!/usr/bin/env python3
"""Terrain hole census and edge/face split over face-identity captures.

A face-identity capture (OPENPETE_TERRAIN_FACEID=1) holds, per pixel, the
24-bit identity of the terrain face covering it: bit 23 set for a retained
face (terrain_faceid.frag), bit 22 set and bit 23 clear for a walked face
(native_render.c scene_faceid_debug), anything else background (the scene
keeps only terrain). Coverage is a property of the pixel, independent of
which identity is drawn, so two arms of one pose compare pixel for pixel.

  census BASE ARM [--png-out DIR]
      BASE and ARM are face-identity PNGs of one pose (fp64 and a test arm).
      Prints one line of key=value counts:
        faces      distinct retained identities in BASE (faces visible)
        walked     distinct walked identities in BASE
        cov        terrain pixels in BASE
        pxdiff     pixels whose value differs between BASE and ARM: content
                   outside the terrain draws is the same in both arms, so
                   every one is a terrain coverage or identity change; the
                   census's primary count, free of the bit classification
        holes      pixels terrain in BASE, background in ARM
        rholes     pixels background in BASE, terrain in ARM
        iso_b      BASE pixels whose identity matches none of their four
                   neighbours (background counts as an identity), and iso_a
                   the same in ARM; iso_d = iso_a - iso_b
        idchg      pixels terrain in both whose identity differs
        idchg_int  of those, pixels interior in both images (all eight
                   neighbours carry the pixel's own identity in each image):
                   a face replaced by another, not an edge moved
      Exit 0; 1 when an image is missing or the sizes differ.

  split IDB IDA COLB COLA [--components N]
      IDB/IDA the face-identity PNGs and COLB/COLA the stock colour PNGs of
      one pose in the two arms. Every pixel whose colour differs is an edge
      pixel when it lies within one pixel (eight-neighbourhood, itself
      included) of an identity boundary in IDB and within one pixel of one
      in IDA, else a face pixel. Prints the counts, the face pixels' maximum
      channel delta, and the N largest 8-connected face-pixel components
      with their bounding box, the identities under them in each arm, and
      their maximum delta; then the 8-connected components of pixels whose
      colour differs by 8 or more, largest delta first, with how many of
      their pixels are edge pixels.

  holes ID [--list FILE]
      ID is one face-identity PNG of a capture holding terrain alone
      (--render-paths terrain,portal,transition: no mobys, sky, particles or
      HUD). The background is the pixels equal to the clear colour, 0x000000
      (the face-identity capture clears to black and every terrain draw is
      recoloured to an identity with bit 22 or bit 23 set, never 0). Prints
      one line of key=value counts:
        faces, walked, cov  as for census (cov: pixels not the clear colour)
        pins       clear pixels whose eight neighbours are all not clear:
                   1-px holes
        holes      8-connected clear components of at most 16 pixels that
                   touch no image edge, every pixel of their 8-neighbour ring
                   not clear: holes of any size up to 16 px, pins included
        hole_px    pixels in those components
        foreign    pixels neither clear nor carrying bit 22 or 23: a draw the
                   recolour missed (or a path the capture should not hold).
                   Nonzero makes the capture impure; the caller refuses it.
      With --list, writes a "# size W H" line, then one line "x y size" per
      hole pixel (size = its component's pixel count) to FILE.

  arms IDA IDB COLA COLB
      One bin of the precision A/B: the face-identity PNGs and the stock
      colour PNGs of one pose in two arms (fp64 against TERRAIN_FP32_ARM2 is
      bin 1, TERRAIN_FP32_ARM2 against TERRAIN_FP32 bin 2). Prints:
        px, edge, face, maxd  as split's diff, edge, face and max delta
        tri        retained triangles visible in either arm
        vanish     triangles with pixels in IDA and none in IDB; vanish_px
                   their IDA pixels, vanish4 those with 4 or more;
                   vanish_vis (vanish_vis_px) those of which half or more
                   of the pixels differ in colour (otherwise a same-colour
                   triangle took them: a depth tie of coincident surfaces)
        appear     the same from IDB to IDA (appear_px, appear4, appear_vis)
        repaint    triangles whose pixels common to both arms number at
                   least 4 and of which half or more differ in colour;
                   repaint_px their differing pixels; repaint_int those
                   with a differing pixel off every identity boundary (a
                   repaint whose differing pixels all sit on edges is a
                   sliver following its edges)
      A flip is a visible vanish or appear, or a repaint_int: a face that
      changes identity or repaints wholesale. An edge shift moves pixels between
      neighbouring triangles and changes no triangle's set.

  classes ID CLS
      CLS is a debug-tint-15 capture (each retained face painted with its
      verdict-class word, terrain.vert) of the pose ID is the face-identity
      capture of, in the same arm. Each retained triangle of ID takes the
      most common word over its pixels; prints, per class bit, the
      triangles carrying it and their pixels (name=tris/px): cells4 tri4
      tri2 water hpraw hpfog rim hq lp near tjs snap affine fogoff fogpp
      nolod keyed fill blendfar. A triangle whose run-wide bits (snap,
      affine, fogoff, fogpp, nolod) differ from the majority's carries a
      mixed word (a blended face) and is counted as impure, not classed.

Engagement: a census line with faces == 0 and walked == 0 compared nothing
and is marked VACUOUS by the caller.
"""
import sys

import numpy as np
from PIL import Image

RET = 0x800000
WALK = 0x400000


def load_ids(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.uint32)
    return a[:, :, 0] | (a[:, :, 1] << 8) | (a[:, :, 2] << 16)


def is_terrain(ids):
    return (ids & (RET | WALK)) != 0


def boundary(ids):
    """Pixels with a 4-neighbour of a different identity."""
    b = np.zeros(ids.shape, dtype=bool)
    d = ids[:, 1:] != ids[:, :-1]
    b[:, 1:] |= d
    b[:, :-1] |= d
    d = ids[1:, :] != ids[:-1, :]
    b[1:, :] |= d
    b[:-1, :] |= d
    return b


def dilate(m):
    p = np.pad(m, 1)
    out = np.zeros(m.shape, dtype=bool)
    h, w = m.shape
    for dy in (0, 1, 2):
        for dx in (0, 1, 2):
            out |= p[dy:dy + h, dx:dx + w]
    return out


def isolated(ids):
    c = ids[1:-1, 1:-1]
    return int(np.count_nonzero((ids[1:-1, :-2] != c) & (ids[1:-1, 2:] != c) &
                                (ids[:-2, 1:-1] != c) & (ids[2:, 1:-1] != c)))


def census(argv):
    b = load_ids(argv[0])
    a = load_ids(argv[1])
    if b.shape != a.shape:
        print("census: size %s vs %s" % (b.shape, a.shape))
        return 1
    faces = len(np.unique(b[(b & RET) != 0]))
    walked = len(np.unique(b[((b & WALK) != 0) & ((b & RET) == 0)]))
    tb, ta = is_terrain(b), is_terrain(a)
    holes = int(np.count_nonzero(tb & ~ta))
    rholes = int(np.count_nonzero(ta & ~tb))
    chg = tb & ta & (b != a)
    interior = ~dilate(boundary(b)) & ~dilate(boundary(a))
    iso_b, iso_a = isolated(b), isolated(a)
    print("faces=%d walked=%d cov=%d pxdiff=%d holes=%d rholes=%d iso_b=%d iso_a=%d iso_d=%d "
          "idchg=%d idchg_int=%d"
          % (faces, walked, int(np.count_nonzero(tb)), int(np.count_nonzero(b != a)), holes,
             rholes, iso_b, iso_a, iso_a - iso_b, int(np.count_nonzero(chg)),
             int(np.count_nonzero(chg & interior))))
    return 0


def components(mask):
    """8-connected components of a boolean mask, as lists of (y, x)."""
    h, w = mask.shape
    seen = np.zeros(mask.shape, dtype=bool)
    out = []
    for y0, x0 in zip(*np.nonzero(mask)):
        if seen[y0, x0]:
            continue
        stack = [(y0, x0)]
        seen[y0, x0] = True
        px = []
        while stack:
            y, x = stack.pop()
            px.append((y, x))
            for yy in (y - 1, y, y + 1):
                for xx in (x - 1, x, x + 1):
                    if 0 <= yy < h and 0 <= xx < w and mask[yy, xx] and not seen[yy, xx]:
                        seen[yy, xx] = True
                        stack.append((yy, xx))
        out.append(px)
    out.sort(key=len, reverse=True)
    return out


def split(argv):
    ncomp = 8
    if "--components" in argv:
        k = argv.index("--components")
        ncomp = int(argv[k + 1])
        argv = argv[:k] + argv[k + 2:]
    b = load_ids(argv[0])
    a = load_ids(argv[1])
    cb = np.asarray(Image.open(argv[2]).convert("RGB"), dtype=np.int16)
    ca = np.asarray(Image.open(argv[3]).convert("RGB"), dtype=np.int16)
    if cb.shape[:2] != b.shape or ca.shape[:2] != b.shape or a.shape != b.shape:
        print("split: image sizes differ")
        return 1
    d = np.abs(cb - ca).max(axis=2)
    diff = d > 0
    near = dilate(boundary(b)) & dilate(boundary(a))
    edge = diff & near
    face = diff & ~near
    print("diff=%d edge=%d face=%d edge_max=%d face_max=%d"
          % (int(diff.sum()), int(edge.sum()), int(face.sum()),
             int(d[edge].max()) if edge.any() else 0, int(d[face].max()) if face.any() else 0))
    big = components(diff & (d >= 8))
    print("delta8_components=%d" % len(big))
    for px in sorted(big, key=lambda c: -max(int(d[p]) for p in c))[:ncomp]:
        ys = [p[0] for p in px]
        xs = [p[1] for p in px]
        ne = sum(1 for p in px if near[p])
        print("  d8 px=%d box=(%d,%d)-(%d,%d) maxd=%d edge_px=%d ids_base=%s ids_arm=%s"
              % (len(px), min(xs), min(ys), max(xs), max(ys), max(int(d[p]) for p in px), ne,
                 ",".join("%06x" % v for v in sorted({int(b[p]) for p in px})[:4]),
                 ",".join("%06x" % v for v in sorted({int(a[p]) for p in px})[:4])))
    comps = components(face)
    print("face_components=%d" % len(comps))
    for px in comps[:ncomp]:
        ys = [p[0] for p in px]
        xs = [p[1] for p in px]
        idsb = sorted({int(b[p]) for p in px})
        idsa = sorted({int(a[p]) for p in px})
        same = sum(1 for p in px if b[p] == a[p])
        print("  comp px=%d box=(%d,%d)-(%d,%d) maxd=%d same_id_px=%d nid_base=%d nid_arm=%d "
              "ids_base=%s ids_arm=%s"
              % (len(px), min(xs), min(ys), max(xs), max(ys), max(int(d[p]) for p in px), same,
                 len(idsb), len(idsa), ",".join("%06x" % v for v in idsb[:4]),
                 ",".join("%06x" % v for v in idsa[:4])))
    return 0


CLEAR = 0x000000
HOLE_MAX = 16


def holes(argv):
    out = None
    if "--list" in argv:
        k = argv.index("--list")
        out = argv[k + 1]
        argv = argv[:k] + argv[k + 2:]
    ids = load_ids(argv[0])
    h, w = ids.shape
    clear = ids == CLEAR
    terr = is_terrain(ids)
    foreign = int(np.count_nonzero(~clear & ~terr))
    p = np.pad(clear, 1, constant_values=True)
    nclear = sum(p[1 + dy:1 + dy + h, 1 + dx:1 + dx + w].astype(np.int32)
                 for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dy or dx)
    pins = int(np.count_nonzero(clear & (nclear == 0)))
    # A clear component of at most HOLE_MAX pixels off the image edge is
    # enclosed by non-clear pixels: 8-connectivity puts every clear pixel of
    # its 8-neighbour ring inside the component.
    found = []
    for px in components(clear):
        if len(px) > HOLE_MAX:
            continue
        if any(y in (0, h - 1) or x in (0, w - 1) for y, x in px):
            continue
        found.append(px)
    faces = len(np.unique(ids[(ids & RET) != 0]))
    walked = len(np.unique(ids[((ids & WALK) != 0) & ((ids & RET) == 0)]))
    print("faces=%d walked=%d cov=%d pins=%d holes=%d hole_px=%d foreign=%d"
          % (faces, walked, int(np.count_nonzero(~clear)), pins, len(found),
             sum(len(px) for px in found), foreign))
    if out is not None:
        with open(out, "w") as f:
            f.write("# size %d %d\n" % (w, h))
            for px in found:
                for y, x in sorted(px):
                    f.write("%d %d %d\n" % (x, y, len(px)))
    return 0


def arms(argv):
    ia, ib = load_ids(argv[0]), load_ids(argv[1])
    ca = np.asarray(Image.open(argv[2]).convert("RGB"), dtype=np.int16)
    cb = np.asarray(Image.open(argv[3]).convert("RGB"), dtype=np.int16)
    if ib.shape != ia.shape or ca.shape[:2] != ia.shape or cb.shape[:2] != ia.shape:
        print("arms: image sizes differ")
        return 1
    d = np.abs(ca - cb).max(axis=2)
    diff = d > 0
    near = dilate(boundary(ia)) & dilate(boundary(ib))
    ra, rb = (ia & RET) != 0, (ib & RET) != 0
    ua, na = np.unique(ia[ra], return_counts=True)
    ub, nb = np.unique(ib[rb], return_counts=True)
    ca_, cb_ = dict(zip(ua.tolist(), na.tolist())), dict(zip(ub.tolist(), nb.tolist()))
    van = [n for f, n in ca_.items() if f not in cb_]
    app = [n for f, n in cb_.items() if f not in ca_]
    # A vanished (appeared) triangle is visible when half or more of its
    # pixels in the arm holding it differ in colour; otherwise another
    # triangle of the same colour took its pixels (a depth tie between
    # coincident surfaces).
    ua_d, na_d = np.unique(ia[ra & diff], return_counts=True)
    ub_d, nb_d = np.unique(ib[rb & diff], return_counts=True)
    da, db = dict(zip(ua_d.tolist(), na_d.tolist())), dict(zip(ub_d.tolist(), nb_d.tolist()))
    van_v = [n for f, n in ca_.items() if f not in cb_ and 2 * da.get(f, 0) >= n]
    app_v = [n for f, n in cb_.items() if f not in ca_ and 2 * db.get(f, 0) >= n]
    same = ra & (ia == ib)
    uc, nc = np.unique(ia[same], return_counts=True)
    ud, nd = np.unique(ia[same & diff], return_counts=True)
    dmap = dict(zip(ud.tolist(), nd.tolist()))
    rep = [f for f, n in zip(uc.tolist(), nc.tolist()) if n >= 4 and 2 * dmap.get(f, 0) >= n]
    # A repaint with no differing pixel away from an identity boundary is a
    # sliver whose every pixel sits on an edge: its colour moved with the
    # edge's interpolation, not with a verdict.
    ui, ni = np.unique(ia[same & diff & ~near], return_counts=True)
    imap = dict(zip(ui.tolist(), ni.tolist()))
    rep_int = [f for f in rep if imap.get(f, 0) > 0]
    rep = [dmap[f] for f in rep]
    print("px=%d edge=%d face=%d maxd=%d tri=%d vanish=%d vanish_px=%d vanish4=%d "
          "appear=%d appear_px=%d appear4=%d vanish_vis=%d vanish_vis_px=%d appear_vis=%d "
          "appear_vis_px=%d repaint=%d repaint_px=%d repaint_int=%d"
          % (int(diff.sum()), int((diff & near).sum()), int((diff & ~near).sum()),
             int(d.max()), len(set(ca_) | set(cb_)), len(van), sum(van),
             sum(1 for n in van if n >= 4), len(app), sum(app), sum(1 for n in app if n >= 4),
             len(van_v), sum(van_v), len(app_v), sum(app_v), len(rep), sum(rep), len(rep_int)))
    return 0


CLASS_BITS = ("cells4", "tri4", "tri2", "water", "hpraw", "hpfog", "rim", "hq", "lp", "near",
              "tjs", "snap", "affine", "fogoff", "fogpp", "nolod", "keyed", "fill", "blendfar")


def classes(argv):
    ids = load_ids(argv[0])
    cw = load_ids(argv[1])
    if cw.shape != ids.shape:
        print("classes: image sizes differ")
        return 1
    # A triangle's class word is the most common word over its pixels: a
    # blended face mixes its word with the destination, and the tint
    # capture's key contest can hand a pixel to another face.
    m = (ids & RET) != 0
    tri, word = ids[m], cw[m] & 0x3FFFFF
    key = (tri.astype(np.uint64) << np.uint64(32)) | word.astype(np.uint64)
    uk, nk = np.unique(key, return_counts=True)
    best = {}
    for k, n in zip(uk.tolist(), nk.tolist()):
        t, w = k >> 32, k & 0xFFFFFFFF
        if n > best.get(t, (0, 0))[0]:
            best[t] = (n, w)
    ut, nt = np.unique(tri, return_counts=True)
    px = dict(zip(ut.tolist(), nt.tolist()))
    # The run-wide bits (snap affine fogoff fogpp nolod) are one value for
    # every face of a capture; a triangle whose modal word disagrees with
    # the majority's is a mixed word (a blended face) and is dropped.
    gmask = sum(1 << CLASS_BITS.index(n) for n in ("snap", "affine", "fogoff", "fogpp", "nolod"))
    votes = {}
    for _, w in best.values():
        votes[w & gmask] = votes.get(w & gmask, 0) + 1
    gwin = max(votes, key=votes.get) if votes else 0
    impure = [t for t, (_, w) in best.items() if (w & gmask) != gwin]
    for t in impure:
        del best[t]
    out = ["ret_px=%d tris=%d impure=%d" % (int(m.sum()), len(best), len(impure))]
    for k, n in enumerate(CLASS_BITS):
        ts = [t for t, (_, w) in best.items() if w & (1 << k)]
        out.append("%s=%d/%d" % (n, len(ts), sum(px[t] for t in ts)))
    print(" ".join(out))
    return 0


def main():
    cmds = {"census": census, "split": split, "holes": holes,
            "arms": arms, "classes": classes}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        return 64
    try:
        return cmds[sys.argv[1]](sys.argv[2:])
    except FileNotFoundError as e:
        print("terrain_hole_census: missing %s" % e.filename)
        return 1


if __name__ == "__main__":
    sys.exit(main())
