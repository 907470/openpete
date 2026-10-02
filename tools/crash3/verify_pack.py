#!/usr/bin/env python3
"""verify_pack.py - offline self-check of crash3_s1pack.bin.

Parses every model in the pack through bit-exact mirrors of the Spyro 1
renderer's walks (the vertex-stream literal decode and the fresh-face
record walk from moby.c), checks every index against the header
counts, and renders the ENCODED model - vertices from the stream, faces
from the faceblock, texels from the atlas PNGs - so what is verified is
what the game will read, not the converter's intermediate mesh.

Run before any in-game test; exit 1 on any error.
"""
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from convert import PACK_MAGIC, ATLAS_ID_BASE, TPAGE_MODE3, TABLE_ROW   # noqa: E402
from tgeo import raster_tri                                   # noqa: E402


def _s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


class Blob:
    """One model blob viewed as guest memory linked at vaddr 0."""

    def __init__(self, b):
        self.b = b

    def u32(self, a): return struct.unpack_from("<I", self.b, a & 0x1FFFFF)[0]
    def u16(self, a): return struct.unpack_from("<H", self.b, a & 0x1FFFFF)[0]
    def u8(self, a):  return self.b[a & 0x1FFFFF]


def walk_vstream(g, frame_w0, frame_w1, nverts, s5):
    litp = frame_w0 & 0x1FFFFF
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
            f = dict(fresh=fresh, tex=tex, dbl=w0 & 1, abe=w1 & 1,
                     i=[(w0 >> 20 & 0x7FC) >> 2, (w0 >> 11 & 0x7FC) >> 2,
                        (w0 >> 2 & 0x7FC) >> 2, (v4w & 0x7FC) >> 2],
                     c=[(w1 >> 17) & 0x7FC, (w1 >> 8) & 0x7FC,
                        (w1 << 1) & 0x7FC, (v4w >> 9) & 0x7FC])
            if tex:
                td0, td1, td2 = g.u32(fp + 12), g.u32(fp + 16), g.u32(fp + 20)
                f["uv"] = [(td0 & 0xFF, td0 >> 8 & 0xFF), (td1 & 0xFF, td1 >> 8 & 0xFF),
                           (td2 & 0xFF, td2 >> 8 & 0xFF), (td2 >> 16 & 0xFF, td2 >> 24 & 0xFF)]
                f["clut"] = td0 >> 16
                f["tpage"] = td1 >> 16
            faces.append(f)
        fp += adv
    return faces, bc, fp == end


def read_pack(path):
    data = path.read_bytes()
    magic, ver, n, table_off, payload_off, payload_size, aslots, fslots = \
        struct.unpack_from("<8I", data, 0)
    assert magic == PACK_MAGIC, "bad magic"
    models = []
    for k in range(n):
        row = struct.unpack_from("<I8sIIHHHHHHIIIII", data, table_off + TABLE_ROW * k)
        (eid, name, boff, bsize, hpv, lpv, hpf, lpf, srcv, pframe, panim,
         rwo, nrw, rfo, nrf) = row
        models.append(dict(eid=eid, name=name.rstrip(b"\0").decode(), blob=data[payload_off + boff:payload_off + boff + bsize],
                           hp_verts=hpv, lp_verts=lpv, hp_faces=hpf, lp_faces=lpf,
                           src_verts=srcv, pose_anim=panim, pose_frame=pframe,
                           reloc_words=list(struct.unpack_from(f"<{nrw}I", data, rwo)),
                           reloc_frames=list(struct.unpack_from(f"<{nrf}I", data, rfo))))
    return dict(anim_slots=aslots, frame_slots=fslots, models=models)


def check_model(m, aslots, fslots, atlases):
    errs = []
    g = Blob(m["blob"])
    if _s32(g.u32(0)) != 1:
        errs.append("numAnims != 1")
    ah = g.u32(0x38)
    for s in range(aslots):
        if g.u32(0x38 + 4 * s) != ah:
            errs.append(f"anim slot {s} not aliased")
            break
    nframes = struct.unpack("<h", struct.pack("<H", g.u16(ah)))[0]
    ncol = g.u16(ah + 2)
    s5 = g.u8(ah + 6)
    nverts, nverts_lp = g.u8(ah + 8), g.u8(ah + 9)
    hp_p, cols_p, lp_p, far_p = (g.u32(ah + 0x14), g.u32(ah + 0x18),
                                 g.u32(ah + 0x1C), g.u32(ah + 0x20))
    fw0, fw1 = g.u32(ah + 0x24), g.u32(ah + 0x28)
    for f in range(fslots):
        if g.u32(ah + 0x24 + 8 * f) != fw0 or g.u32(ah + 0x28 + 8 * f) != fw1:
            errs.append(f"frame slot {f} differs")
            break
    if nframes != 1: errs.append(f"nframes {nframes}")
    if nverts != m["hp_verts"] or nverts_lp != m["lp_verts"]:
        errs.append("table/header vertex counts disagree")
    if (fw1 >> 24) != nverts: errs.append("frame short-offset != nverts")
    if set(m["reloc_words"]) != {0x38 + 4 * s for s in range(aslots)} | \
            {ah + 0x14, ah + 0x18, ah + 0x1C, ah + 0x20}:
        errs.append("reloc_words set unexpected")
    if set(m["reloc_frames"]) != {ah + 0x24 + 8 * f for f in range(fslots)}:
        errs.append("reloc_frames set unexpected")

    verts = walk_vstream(g, fw0, fw1, nverts, s5)
    tiers = {}
    for label, fp, nv, expect in (("HP", hp_p, nverts, m["hp_faces"]),
                                  ("LP", lp_p, nverts_lp, m["lp_faces"])):
        faces, bc, aligned = walk_faces(g, fp)
        if not aligned: errs.append(f"{label} faceblock walk misaligned")
        if len(faces) != expect: errs.append(f"{label} face count {len(faces)} != {expect}")
        for f in faces:
            if max(f["i"]) >= nv:
                errs.append(f"{label} vert idx {max(f['i'])} >= {nv}"); break
        for f in faces:
            if max(f["c"]) >= ncol * 4:
                errs.append(f"{label} colour off {max(f['c'])} >= {ncol * 4}"); break
        for f in faces:
            if f["tex"]:
                if f["tpage"] & 0x180 != TPAGE_MODE3:
                    errs.append(f"{label} tpage {f['tpage']:04x} not mode 3"); break
                if f["clut"] not in atlases:
                    errs.append(f"{label} atlas id {f['clut']:04x} has no PNG"); break
        tiers[label] = faces
    cols = [g.u32(cols_p + 4 * j) for j in range(ncol)]
    ext = [(min(v[k] for v in verts), max(v[k] for v in verts)) for k in range(3)]
    ntex = sum(1 for f in tiers["HP"] if f["tex"])
    ndbl = sum(1 for f in tiers["HP"] if f["dbl"])
    print(f"{m['name']:6s}: {nverts}v({nverts_lp}lp) {len(tiers['HP'])}f/{len(tiers['LP'])}lp "
          f"({ntex} tex, {ndbl} dbl) col={ncol} ext x{ext[0]} y{ext[1]} z{ext[2]} "
          f"{'ERRORS: ' + '; '.join(errs) if errs else 'OK'}")
    return verts, tiers, cols, not errs


def render(verts, faces, cols, atlases, path, size=384):
    """Encoded-model preview: front / side / top, textured, z-buffered.
    Draws every face (no NCLIP cull) so an inside-out orientation shows
    as a lit/unlit mismatch rather than a hole."""
    from PIL import Image
    mn = [min(v[k] for v in verts) for k in range(3)]
    mx = [max(v[k] for v in verts) for k in range(3)]
    ext = max(mx[k] - mn[k] for k in range(3)) or 1
    ctr = [(mn[k] + mx[k]) / 2 for k in range(3)]
    panels = []
    for view in ("front", "side", "top"):
        img = np.zeros((size, size, 3), dtype=np.uint8) + 40
        zbuf = np.full((size, size), np.inf)

        def proj(p):
            x, y, z = [(p[k] - ctr[k]) / ext for k in range(3)]
            if view == "front":  sx, sy, sz = x, y, z        # S1 y is down
            elif view == "side": sx, sy, sz = z, y, -x
            else:                sx, sy, sz = x, z, y
            return ((sx * 0.9 + 0.5) * size, (sy * 0.9 + 0.5) * size, sz)
        for f in faces:
            i = f["i"][:3]
            p3 = [proj(verts[k]) for k in i]
            rgb = [[(cols[c // 4] >> s) & 0xFF for s in (0, 8, 16)] for c in f["c"][:3]]
            tile = uvs = None
            if f["tex"]:
                tile = atlases[f["clut"]]
                uvs = f["uv"][:3]
            raster_tri(img, zbuf, p3, rgb, uvs, tile)
        panels.append(img)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.concatenate(panels, axis=1)).save(path)


def main():
    import argparse
    from PIL import Image
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", type=Path,
                    default=HERE.parents[1] / "mods" / "crash3-models" / "assets")
    ap.add_argument("--preview", type=Path, help="directory for per-model PNGs")
    ap.add_argument("--models", help="comma-separated names to preview (default all)")
    args = ap.parse_args()
    pack = read_pack(args.assets / "crash3_s1pack.bin")
    atlases = {}
    for png in (args.assets / "textures" / "own").glob("*.png"):
        atlases[int(png.stem, 16)] = np.array(Image.open(png).convert("RGBA"))
    ok = True
    want = set(args.models.split(",")) if args.models else None
    for m in pack["models"]:
        verts, tiers, cols, good = check_model(m, pack["anim_slots"], pack["frame_slots"], atlases)
        ok &= good
        if args.preview and good and (want is None or m["name"] in want):
            render(verts, tiers["HP"], cols, atlases, args.preview / f"{m['name']}.png")
            render(verts, tiers["LP"], cols, atlases, args.preview / f"{m['name']}_lp.png")
    print(f"{len(pack['models'])} models, {len(atlases)} atlas sheet(s): "
          f"{'ALL OK' if ok else 'FAILED'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
