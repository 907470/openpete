#!/usr/bin/env python3
"""Offline self-check for chests_s1pack.bin: parse every parked Model
with bit-exact mirrors of the S1 renderer's walks (vstream_next +
fcache_decode_into from moby_native.c) and render wireframe previews.

Catches encoder drift BEFORE an in-game run: the corrupted-columns bug
(VX packed at bit 10 instead of 11) would have failed here instantly.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ASSETS = HERE.parent / "assets" / "s3"
from convert_chests import PARK_LINK  # single source of truth

CLASSES = [194, 195, 255, 256, 257, 309, 310, 311]


def _s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


class Guest:
    """payload viewed through guest vaddrs (linked at PARK_LINK)."""
    def __init__(self, payload):
        self.p = payload

    def u32(self, va):
        return struct.unpack_from("<I", self.p, (va & 0x7FFFFFFF) - (PARK_LINK & 0x7FFFFFFF))[0]

    def u16(self, va):
        return struct.unpack_from("<H", self.p, (va & 0x7FFFFFFF) - (PARK_LINK & 0x7FFFFFFF))[0]

    def u8(self, va):
        return self.p[(va & 0x7FFFFFFF) - (PARK_LINK & 0x7FFFFFFF)]


def walk_vstream(g, frame_w0, frame_w1, nverts, s5):
    """vstream literal/short walk (single stream, frame A == pose)."""
    litp = (frame_w0 & 0x1FFFFF) | 0x80000000
    shortp = litp + ((frame_w1 >> 24) & 0xFF) * 4
    out = []
    vx = vy = vz = 0
    flag = 0
    for i in range(nverts):
        if i == 0 or flag == 0:
            w = g.u32(litp); litp += 4
            vz = _s32(w) >> 21
            vx = _s32((w << 10) & 0xFFFFFFFF) >> 21
            vy = _s32((w << 20) & 0xFFFFFFFF) >> 19
            flag = w & 1
        else:
            d = struct.unpack("<h", struct.pack("<H", g.u16(shortp)))[0]
            shortp += 2
            vz += (d >> 11) << s5
            vx -= (_s32((d << 21) & 0xFFFFFFFF) >> 27) << s5
            vy -= (_s32((d << 26) & 0xFFFFFFFF) >> 27) << s5
            flag = d & 1
        out.append((vx, vy, vz))
    return out


def walk_faces(g, fb):
    """fcache_decode_into mirror: fresh/strip strides + index packing."""
    bc = g.u32(fb)
    fp = fb + 4
    end = fb + 4 + bc
    faces = []
    guard = 0
    while fp + 8 <= end and guard < 8192:
        guard += 1
        w0 = g.u32(fp)
        fresh = _s32(w0) < 0
        tex = (w0 & 2) != 0
        sprite = fresh and (w0 & 4)
        skip = (w0 & 8) != 0
        adv = 0x14 if sprite else (0x18 if tex else 0x0C) if fresh \
            else (0x14 if tex else 0x08)
        if not sprite and not skip:
            w1 = g.u32(fp + 4)
            v4w = g.u32(fp + 8)
            f = dict(
                fresh=fresh, tex=tex, bit0=w0 & 1,
                i=[(w0 >> 20 & 0x7FC) >> 2, (w0 >> 11 & 0x7FC) >> 2,
                   (w0 >> 2 & 0x7FC) >> 2, (v4w & 0x7FC) >> 2],
                c=[(w1 >> 17) & 0x7FC, (w1 >> 8) & 0x7FC,
                   (w1 << 1) & 0x7FC, (v4w >> 9) & 0x7FC])
            if tex:
                td0, td1, td2 = (g.u32(fp + adv - 12), g.u32(fp + adv - 8),
                                 g.u32(fp + adv - 4))
                f["uv"] = [(td0 & 0xFF, td0 >> 8 & 0xFF),
                           (td1 & 0xFF, td1 >> 8 & 0xFF),
                           (td2 & 0xFF, td2 >> 8 & 0xFF),
                           (td2 >> 16 & 0xFF, td2 >> 24 & 0xFF)]
                f["clut"] = td0 >> 16
                f["tpage"] = td1 >> 16
            faces.append(f)
        fp += adv
    return faces, bc, fp == end


def check_model(g, cls, base):
    errs = []
    num_anims = _s32(g.u32(base))
    if num_anims != 1:
        errs.append(f"numAnims {num_anims}")
    ah = g.u32(base + 0x38)
    nframes = struct.unpack("<h", struct.pack("<H", g.u16(ah)))[0]
    ncol = g.u16(ah + 2)
    scale, s5 = g.u8(ah + 5), g.u8(ah + 6)
    nverts, nverts_lp = g.u8(ah + 8), g.u8(ah + 9)
    faces_p = g.u32(ah + 0x14)
    cols_p = g.u32(ah + 0x18)
    fw0, fw1 = g.u32(ah + 0x24), g.u32(ah + 0x24 + 4)
    coll = g.u32(base + 0x14)

    verts = walk_vstream(g, fw0, fw1, nverts, s5)
    faces, bc, aligned = walk_faces(g, faces_p)
    if not aligned:
        errs.append("faceblock walk misaligned")
    for f in faces:
        if max(f["i"]) >= nverts:
            errs.append(f"vert idx {max(f['i'])} >= {nverts}")
            break
    for f in faces:
        if max(f["c"]) >= ncol * 4:
            errs.append(f"colour off {max(f['c'])} >= {ncol * 4}")
            break
    ext = [(min(v[k] for v in verts), max(v[k] for v in verts))
           for k in range(3)]
    ntex = sum(1 for f in faces if f["tex"])
    print(f"cls {cls}: {nverts}v({nverts_lp}lp) {len(faces)}f({ntex}tex) "
          f"col={ncol} scale={scale} ext x{ext[0]} y{ext[1]} z{ext[2]} "
          f"coll={'%08x' % coll if coll else 'none'} "
          f"{'ERRORS: ' + '; '.join(errs) if errs else 'OK'}")
    return verts, faces, not errs


def render(cls, verts, faces, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig = plt.figure(figsize=(9, 3.2))
    for vi, (a, b, label) in enumerate([(0, 2, "top XZ"), (0, 1, "front XY"),
                                        (2, 1, "side ZY")]):
        ax = fig.add_subplot(1, 3, vi + 1)
        for f in faces:
            i = f["i"]
            ids = ([i[0], i[1], i[2], i[0]] if i[2] == i[3]
                   else [i[0], i[1], i[3], i[2], i[0]])
            try:
                xs = [verts[k][a] for k in ids]
                ys = [-verts[k][b] for k in ids]   # -Y up for display
            except IndexError:
                continue
            ax.plot(xs, ys, lw=0.5,
                    color="tab:blue" if f["tex"] else "tab:brown")
        ax.set_aspect("equal")
        ax.set_title(f"cls {cls} {label}", fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=90)
    plt.close(fig)


def main():
    pack = (ASSETS / "chests_s1pack.bin").read_bytes()
    hdr = struct.unpack_from("<2I8I9I", pack, 0)
    payload_size = hdr[1]
    vaddrs = hdr[2:10]
    payload = pack[76:76 + payload_size]
    g = Guest(payload)
    ok = True
    outdir = HERE.parent / "assets" / "s3"
    for cls, va in zip(CLASSES, vaddrs):
        verts, faces, good = check_model(g, cls, va)
        ok = ok and good
        render(cls, verts, faces, outdir / f"verify_cls{cls}.png")
    print("PACK", "OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
