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


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("census", "split"):
        print(__doc__)
        return 64
    try:
        return census(sys.argv[2:]) if sys.argv[1] == "census" else split(sys.argv[2:])
    except FileNotFoundError as e:
        print("terrain_hole_census: missing %s" % e.filename)
        return 1


if __name__ == "__main__":
    sys.exit(main())
