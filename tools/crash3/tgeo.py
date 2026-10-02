#!/usr/bin/env python3
"""tgeo.py - decode a Crash 3 model (TGEO) posed by one animation frame
(SVTX) into a plain triangle mesh, and render a textured preview of it.

Everything here is a port of the format reference (cbhacks/CrashEdit:
ModelEntry.ConvertIndices, Frame.LoadNew/MakeVertices, ModelTexture,
GLViewer.ProcessTextureInfoC2, AnimationRenderer.RenderFrame). Where
CrashEdit does something odd (the frame offset added un-swapped, the
special-vertex indexing) it is ported as-is and the preview decides.

TGEO entry = 5 or 6 items:
  [0] 80-byte info: scale x/y/z s32 @0/4/8, TPAG eIDs from @0xC,
      struct count @0x2C, texture-record count @0x34, vertex count
      @0x38, colour count @0x3C, TPAG count @0x40, poly count @0x44,
      animated-texture count @0x48, special-vertex count @0x4C.
  [1] u32 poly items, [2] RGBx colours, [3] 12-byte texture records,
  [4] u32 animated-texture records, [5] (compressed models) u32 position
      descriptors, one per non-special vertex.

Poly items: 0xFFFFFFFF ends the list; an item with a zero high half is a
colour item (c1 = bits 2-8, c2 = bits 9-15); anything else is a triangle
item: texture bits 0-7, animated bit 8, colour bits 9-15, position key
bits 16-23, duplicate flag bit 26, subtype bits 28-29, type bits 30-31.
Types 0/1 continue a strip over previous vertices, type 2 restarts.

SVTX frame (Crash 3 layout): s16 x/y/z offset, s16 unknown, u32 (unused,
0 in the demo), u32 zero, vertex count @16, collision-box count @20,
header size @24, 0x28-byte collision boxes, `special` raw 3-byte
vertices, then the vertex bitstream (24 bits/vertex, or the model's
per-vertex delta descriptors for compressed models). Vertex bytes x8.

Texture record (12 B): u1 v1 | clutx:4 cluty_lo:4 | cluty_hi | u2 v2 |
segment:4 _:1 blend:2 cm_lo:1 | cm_hi:1 _:1 page:6 | u3 v3 | u4 v4.
Texel x in the page = u + segment * 64 * (4 >> colourmode).
"""
import struct
from pathlib import Path

import numpy as np

from nsf import eid_to_name

MODEL_SCALE = 3200.0 * 128.0     # CrashEdit ModelC1 * AnimC1


# ------------------------------------------------------------ model side

class TexRec:
    __slots__ = ("u", "v", "clutx", "cluty", "seg", "blend", "cm", "page",
                 "x", "y")

    def __init__(self, b):
        self.u = [b[0], b[4], b[8], b[10]]
        self.v = [b[1], b[5], b[9], b[11]]
        self.clutx = b[2] & 0xF
        cluty1 = (b[2] >> 4) & 0xF
        self.cluty = (b[3] << 2) | ((cluty1 >> 2) & 3)
        self.seg = b[6] & 0xF
        self.blend = (b[6] >> 5) & 3
        self.cm = ((b[6] >> 7) | ((b[7] << 1) & 2)) & 3
        self.page = (b[7] >> 2) & 0x3F
        xoff = (1 << (2 - self.cm)) * 64 * self.seg
        # inclusive-edge nudge: the max side of each axis steps one texel
        # out so a rect spanning [a, b] covers b's column/row too
        tx = [u + xoff for u in self.u]
        ty = list(self.v)
        x = list(tx)
        y = list(ty)
        if tx[0] > tx[1] or tx[0] > tx[2]: x[0] += 1
        if tx[1] > tx[0] or tx[1] > tx[2]: x[1] += 1
        if tx[2] > tx[1] or tx[2] > tx[0]: x[2] += 1
        if tx[3] > tx[1] or tx[3] > tx[2] or tx[3] > tx[0]: x[3] += 1
        if ty[0] > ty[1] or ty[0] > ty[2]: y[0] += 1
        if ty[1] > ty[0] or ty[1] > ty[2]: y[1] += 1
        if ty[2] > ty[1] or ty[2] > ty[0]: y[2] += 1
        if ty[3] > ty[1] or ty[3] > ty[2] or ty[3] > ty[0]: y[3] += 1
        self.x, self.y = x, y

    def key(self, page_eid):
        return (page_eid, self.cm, self.clutx, self.cluty, self.seg,
                min(self.x[:3]), min(self.y[:3]),
                max(self.x[:3]), max(self.y[:3]), self.blend)


class Model:
    def __init__(self, entry):
        info = entry.items[0]
        self.entry = entry
        self.name = entry.name
        self.scale = struct.unpack_from("<3i", info, 0)
        self.n_struct = struct.unpack_from("<I", info, 0x2C)[0]
        self.n_tex = struct.unpack_from("<I", info, 0x34)[0]
        self.n_verts = struct.unpack_from("<I", info, 0x38)[0]
        self.n_col = struct.unpack_from("<I", info, 0x3C)[0]
        self.n_tpag = struct.unpack_from("<I", info, 0x40)[0]
        self.n_poly = struct.unpack_from("<I", info, 0x44)[0]
        self.n_anim_tex = struct.unpack_from("<I", info, 0x48)[0]
        self.special = struct.unpack_from("<i", info, 0x4C)[0]
        self.tpags = [struct.unpack_from("<I", info, 0xC + 4 * k)[0]
                      for k in range(self.n_tpag)]
        self.items = entry.items
        it = entry.items
        self.polys = list(struct.unpack_from(f"<{len(it[1]) // 4}I", it[1], 0))
        self.colors = [(it[2][4 * k], it[2][4 * k + 1], it[2][4 * k + 2])
                       for k in range(len(it[2]) // 4)]
        self.texrecs = [TexRec(it[3][12 * k:12 * k + 12])
                        for k in range(len(it[3]) // 12)]
        self.anim_tex = list(struct.unpack_from(f"<{len(it[4]) // 4}i", it[4], 0))
        self.positions = None
        if len(it) >= 6:
            self.positions = []
            for k in range(len(it[5]) // 4):
                w = struct.unpack_from("<i", it[5], 4 * k)[0]
                self.positions.append(((w >> 25) & 0x7F, (w >> 9) & 0xFF,
                                       (w >> 17) & 0xFF, (w >> 6) & 7,
                                       (w >> 3) & 7, w & 7))  # x y z xb zb yb
        self.tris = self._convert_indices()

    # CrashEdit ModelEntry.ConvertIndices, verbatim in behaviour.
    def _convert_indices(self):
        NULLPTR = 0x57
        polys = self.polys
        p = {}
        v = -self.special if self.positions is not None else 0
        vtx = []
        structs = []
        for item in polys:
            if item == 0xFFFFFFFF:
                break
            if item & 0xFFFF0000:
                t = dict(tex=item & 0xFF, animated=bool((item >> 8) & 1),
                         color=(item >> 9) & 0x7F, key=(item >> 16) & 0xFF,
                         dup=(item >> 26) & 1, subtype=(item >> 28) & 3,
                         type=(item >> 30) & 3)
                if t["dup"] == 0:
                    if t["key"] != NULLPTR:
                        p[t["key"]] = v
                    vtx.append(v); v += 1
                else:
                    vtx.append(p[t["key"]])
                structs.append(t)
            else:
                structs.append(dict(c1=(item >> 2) & 0x7F, c2=(item >> 9) & 0x7F))
        tris = []
        lastvalidcc = -3
        lastccpos = -1
        lastaapos = -1
        lastcolor = -1
        lastnonbb = -1
        cur_v = 0
        n = len(structs)

        def ci(i):
            return structs[i]["color"]

        for i in range(n):
            s = structs[i]
            if "c1" in s:
                lastcolor = i
                continue
            tt = s["type"]
            if tt == 0:
                lastaapos = cur_v
                lastnonbb = i
                c2 = structs[lastcolor]["c1"] if lastcolor + 1 == i else ci(i - 1)
                if lastcolor + 1 == i:
                    c3 = structs[lastcolor]["c2"]
                elif lastcolor + 2 == i:
                    c3 = structs[lastcolor]["c1"]
                else:
                    c3 = ci(i - 2)
                tris.append(dict(v=[vtx[cur_v], vtx[cur_v - 1], vtx[cur_v - 2]],
                                 c=[s["color"], c2, c3], tex=s["tex"],
                                 type=tt, subtype=s["subtype"],
                                 animated=s["animated"]))
            elif tt == 1:
                if lastcolor < lastnonbb - 2:
                    c3 = ci(lastnonbb - 2)
                elif lastcolor == lastnonbb - 2:
                    c3 = structs[lastcolor]["c1"]
                elif lastcolor == lastnonbb - 1:
                    c3 = structs[lastcolor]["c2"]
                else:
                    c3 = structs[lastcolor]["c2"]
                c2 = structs[lastcolor]["c1"] if lastcolor + 1 == i else ci(i - 1)
                v3 = vtx[lastccpos] if lastccpos > lastaapos else vtx[lastaapos - 2]
                tris.append(dict(v=[vtx[cur_v], vtx[cur_v - 1], v3],
                                 c=[s["color"], c2, c3], tex=s["tex"],
                                 type=tt, subtype=s["subtype"],
                                 animated=s["animated"]))
            elif tt == 2:
                if i + 2 < n and lastvalidcc + 2 < i:
                    lastvalidcc = i
                    tris.append(dict(v=[vtx[cur_v], vtx[cur_v + 1], vtx[cur_v + 2]],
                                     c=[s["color"], ci(i + 1), ci(i + 2)],
                                     tex=s["tex"], type=tt,
                                     subtype=s["subtype"],
                                     animated=s["animated"]))
                lastccpos = cur_v
                lastnonbb = i
            cur_v += 1
        return tris

    def resolve_texture(self, tri, frame_no=0):
        """GLViewer.ProcessTextureInfoC2 for a static pose: the record a
        triangle samples, or None for untextured. Out-of-table lookups
        are untextured, never dropped."""
        idx, animated = tri["tex"], tri["animated"]
        if idx == 0 and not animated:
            return None
        tex_id = idx - 1
        if animated:
            tex_id += 1
            if tex_id >= len(self.anim_tex):
                return None
            a = self.anim_tex[tex_id]
            off = a & 0x7FF
            if off == 0:
                return None
            tex_id = off - 1
            if a < 0:                              # LOD field: closest LOD
                tex_id += (a >> 29) & 3
            else:                                  # flipbook
                latency = (a >> 26) & 0x1F
                delay = (a >> 19) & 0x7F
                mask = (a >> 12) & 0x7F
                leap = (a >> 11) & 1
                tex_id += (frame_no // (1 + latency) + delay) & mask
                if leap:
                    tex_id += 1
                    if tex_id >= len(self.anim_tex):
                        return None
                    a = self.anim_tex[tex_id]
                    tex_id = (a & 0x7FF) - 1 + ((a >> 29) & 3)
        if tex_id < 0 or tex_id >= len(self.texrecs):
            return None
        return self.texrecs[tex_id]


# ------------------------------------------------------------ frame side

class Frame:
    def __init__(self, data):
        self.offset = struct.unpack_from("<3h", data, 0)
        self.n_verts = struct.unpack_from("<I", data, 16)[0]
        self.n_coll = struct.unpack_from("<I", data, 20)[0]
        self.hdr_size = struct.unpack_from("<I", data, 24)[0]
        self.coll = []
        for i in range(self.n_coll):
            self.coll.append(struct.unpack_from("<10i", data, 28 + 0x28 * i))
        fake = 28 + self.n_coll * 0x28
        self.special = (self.hdr_size - fake) // 3
        self.special_verts = [tuple(data[fake + 3 * i:fake + 3 * i + 3])
                              for i in range(self.special)]
        self.stream = data[fake:]
        if len(self.stream) % 4:
            raise ValueError("frame bitstream not word aligned")

    def _bits(self):
        """Bit array in CrashEdit's 'temporals' order: each u32 word LE,
        bit 31 first."""
        words = np.frombuffer(self.stream, dtype="<u4")
        bits = np.zeros(len(words) * 32, dtype=np.uint8)
        for j in range(32):
            bits[j::32] = (words >> (31 - j)) & 1
        return bits

    def positions(self, model):
        """Decoded vertex bytes (x, y, z) x8, CrashEdit MakeVertices.
        Returns a list indexed like CrashEdit's `verts` array."""
        verts = [None] * self.n_verts
        bits = self._bits()
        if model.positions is not None:
            xa = ya = za = 0
            bn = self.special * 24
            for i, (px, py, pz, xb, zb, yb) in enumerate(model.positions):
                if xb == 7: xa = 0
                if yb == 7: ya = 0
                if zb == 7: za = 0

                def field(nb):
                    nonlocal bn
                    val = (-1 << nb) if bits[bn] else 0
                    bn += 1
                    for b in range(nb):
                        val |= int(bits[bn]) << (nb - 1 - b)
                        bn += 1
                    return val
                xv = field(xb); zv = field(zb); yv = field(yb)
                xa = (xa + xv + (px << 1)) & 0xFF
                ya = (ya + yv + py) & 0xFF
                za = (za + zv + pz) & 0xFF
                if i < self.n_verts:
                    verts[i] = (xa, ya, za)
        elif len(bits) >= (self.n_verts - self.special) * 24:
            # uncompressed: bytes in little-endian word order
            raw = np.frombuffer(self.stream, dtype=np.uint8)
            bi = self.special * 3
            for i in range(self.special, self.n_verts):
                verts[i] = (int(raw[bi]), int(raw[bi + 1]), int(raw[bi + 2]))
                bi += 3
        for i in range(self.n_verts):
            if verts[i] is not None:
                verts[i] = tuple(c * 8 for c in verts[i])
        return verts


# ----------------------------------------------------------------- mesh

class Mesh:
    """Static pose: verts in CrashEdit viewer space (GL, y up); tris with
    per-corner colour (RGB), optional texture (record, page eid, per-corner
    nudged texel coords), nocull flag."""

    def __init__(self, name):
        self.name = name
        self.verts = []          # [(x, y, z) float]
        self.tris = []           # dicts: v[3], rgb[3], tex|None, nocull
        self.pose = ""
        self.stats = {}


def texel_fetch(page, cm, x, y):
    """One texel of a page as 5551 (CrashEdit crash1-generic.frag)."""
    row = y * 512
    if cm == 0:
        b = page[row + (x >> 1)]
        idx = (b & 0xF) if (x & 1) == 0 else (b >> 4) & 0xF
        return idx
    if cm == 1:
        return page[row + x]
    o = row + x * 2
    return page[o] | (page[o + 1] << 8)


def clut_lookup(page, clutx, cluty, idx):
    o = cluty * 512 + (clutx * 16 + idx) * 2
    return page[o] | (page[o + 1] << 8)


def decode_tile(page, rec):
    """Texels of a record's bbox as a (h, w) uint16 5551 array, plus the
    bbox origin (x0, y0). x/y are nudged texel coords (exclusive on the
    max side), so the tile spans [x0, x1) x [y0, y1)."""
    x0, x1 = min(rec.x[:3]), max(rec.x[:3])
    y0, y1 = min(rec.y[:3]), max(rec.y[:3])
    w, h = max(1, x1 - x0), max(1, y1 - y0)
    out = np.zeros((h, w), dtype=np.uint16)
    pw = 1024 >> rec.cm          # texels per 512-byte row at this depth
    for yy in range(h):
        py = (y0 + yy) & 0x7F
        for xx in range(w):
            px = (x0 + xx) % pw
            t = texel_fetch(page, rec.cm, px, py)
            if rec.cm != 2:
                t = clut_lookup(page, rec.clutx, rec.cluty, t)
            out[yy, xx] = t
    return out, (x0, y0)


def decode_pose(lib, model_entry, anim_entry, frame_idx=0):
    m = Model(model_entry)
    fr = Frame(anim_entry.items[frame_idx])
    verts = fr.positions(m)
    mesh = Mesh(m.name)
    mesh.pose = f"{anim_entry.name}:{frame_idx}"
    off = [c / 4.0 for c in fr.offset]
    scl = [c / MODEL_SCALE for c in m.scale]
    # CrashEdit: (v.X, v.Z, v.Y) + (XOffset, YOffset, ZOffset)/4, then the
    # per-axis scale - ported as-is.
    for v in verts:
        if v is None:
            mesh.verts.append(None)
            continue
        g = (v[0] + off[0], v[2] + off[1], v[1] + off[2])
        mesh.verts.append((g[0] * scl[0], g[1] * scl[1], g[2] * scl[2]))
    mesh.special = fr.special
    mesh.coll = fr.coll
    n_tex = n_skip = 0
    for t in m.tris:
        nocull = t["subtype"] in (0, 2)
        flip = ((t["type"] == 2) ^ (t["subtype"] == 3)) and not nocull
        rec = m.resolve_texture(t)
        order = [2, 1, 0] if flip else [0, 1, 2]
        vi = [t["v"][k] + fr.special for k in order]
        if any(i < 0 or i >= len(mesh.verts) or mesh.verts[i] is None for i in vi):
            n_skip += 1
            continue
        rgb = [m.colors[min(t["c"][k], len(m.colors) - 1)] for k in order]
        tex = None
        if rec is not None:
            if (t["type"] != 2 and not flip) or (t["type"] == 2 and t["subtype"] == 1):
                slots = [2, 1, 0]
            else:
                slots = [0, 1, 2]
            page_eid = m.tpags[rec.page] if rec.page < len(m.tpags) else None
            if page_eid in lib.pages:
                tex = dict(rec=rec, page=page_eid,
                           xy=[(rec.x[s], rec.y[s]) for s in slots])
                n_tex += 1
        mesh.tris.append(dict(v=vi, rgb=rgb, tex=tex, nocull=nocull,
                              src_type=t["type"], src_subtype=t["subtype"]))
    used = sorted({i for t in mesh.tris for i in t["v"]})
    mesh.stats = dict(verts=len(verts), used_verts=len(used),
                      special=fr.special, tris=len(mesh.tris),
                      textured=n_tex, skipped=n_skip,
                      colors=len(m.colors), texrecs=len(m.texrecs))
    mesh.model = m
    return mesh


def default_pose(lib, model_entry, prefer=None):
    """(anim entry, frame) for a model: first GOOL-listed animation
    present in the library, else an SVTX with the same vertex count
    (name-prefix match preferred), else None."""
    anims = lib.anims_for(model_entry.eid)
    if prefer:
        a = lib.anim(prefer)
        return (a, 0) if a else (None, 0)
    if anims:
        return anims[0], 0
    nv = struct.unpack_from("<I", model_entry.items[0], 0x38)[0]
    cands = [a for a in lib.anims.values()
             if struct.unpack_from("<I", a.items[0], 16)[0] == nv]
    cands.sort(key=lambda a: (a.name[:3] != model_entry.name[:3], a.name))
    return (cands[0], 0) if cands else (None, 0)


# -------------------------------------------------------------- preview

def rgb5551(t):
    return ((t & 0x1F) * 255 // 31, ((t >> 5) & 0x1F) * 255 // 31,
            ((t >> 10) & 0x1F) * 255 // 31)


def raster_tri(img, zbuf, pts, cols, uvs, tile):
    """Z-buffered barycentric fill with perspective-free interpolation."""
    H, W = zbuf.shape
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    x0, x1 = max(0, int(min(xs))), min(W - 1, int(max(xs)) + 1)
    y0, y1 = max(0, int(min(ys))), min(H - 1, int(max(ys)) + 1)
    if x0 > x1 or y0 > y1:
        return
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = pts
    d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if abs(d) < 1e-9:
        return
    px, py = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
    l0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / d
    l1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / d
    l2 = 1 - l0 - l1
    inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
    z = l0 * az + l1 * bz + l2 * cz
    sub = zbuf[y0:y1 + 1, x0:x1 + 1]
    win = inside & (z < sub)
    if not win.any():
        return
    col = np.stack([l0 * cols[0][k] + l1 * cols[1][k] + l2 * cols[2][k]
                    for k in range(3)], axis=-1)
    if tile is not None:
        u = l0 * uvs[0][0] + l1 * uvs[1][0] + l2 * uvs[2][0]
        v = l0 * uvs[0][1] + l1 * uvs[1][1] + l2 * uvs[2][1]
        th, tw = tile.shape[:2]
        ui = np.clip(u.astype(int), 0, tw - 1)
        vi = np.clip(v.astype(int), 0, th - 1)
        tx = tile[vi, ui]                      # (h, w, 3) RGB or zeros
        alpha = tile[vi, ui, 3] if tile.shape[2] == 4 else None
        col = col * tx[..., :3] / 128.0
        if alpha is not None:
            win &= alpha > 0
    col = np.clip(col, 0, 255).astype(np.uint8)
    img[y0:y1 + 1, x0:x1 + 1][win] = col[win]
    sub[win] = z[win]


def tile_rgba(lib, tex):
    rec, page = tex["rec"], lib.pages[tex["page"]]
    t, (x0, y0) = decode_tile(page, rec)
    h, w = t.shape
    out = np.zeros((h, w, 4), dtype=np.uint8)
    for yy in range(h):
        for xx in range(w):
            p = int(t[yy, xx])
            out[yy, xx, :3] = rgb5551(p)
            out[yy, xx, 3] = 255 if p else 0
    return out, (x0, y0)


def preview(lib, mesh, path, size=512, views=("front", "side", "top")):
    """Textured orthographic renders (three views side by side)."""
    from PIL import Image
    pts = [v for v in mesh.verts if v is not None]
    if not pts:
        return
    mn = [min(p[k] for p in pts) for k in range(3)]
    mx = [max(p[k] for p in pts) for k in range(3)]
    ext = max(mx[k] - mn[k] for k in range(3)) or 1.0
    ctr = [(mn[k] + mx[k]) / 2 for k in range(3)]
    tiles = {}
    panels = []
    for view in views:
        img = np.zeros((size, size, 3), dtype=np.uint8) + 40
        zbuf = np.full((size, size), np.inf)

        def proj(p):
            x, y, z = [(p[k] - ctr[k]) / ext for k in range(3)]
            if view == "front":  sx, sy, sz = x, -y, z      # look down -z
            elif view == "side": sx, sy, sz = z, -y, -x     # look down +x
            else:                sx, sy, sz = x, z, -y      # look down -y
            return ((sx * 0.9 + 0.5) * size, (sy * 0.9 + 0.5) * size, sz)

        for t in mesh.tris:
            p3 = [proj(mesh.verts[i]) for i in t["v"]]
            tile = None; uvs = None
            if t["tex"] is not None:
                key = id(t["tex"]["rec"])
                if key not in tiles:
                    tiles[key] = tile_rgba(lib, t["tex"])
                tile, (x0, y0) = tiles[key]
                uvs = [(x - x0, y - y0) for (x, y) in t["tex"]["xy"]]
            raster_tri(img, zbuf, p3, t["rgb"], uvs, tile)
        panels.append(img)
    out = np.concatenate(panels, axis=1)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(out).save(path)


if __name__ == "__main__":
    import argparse
    from nsf import Library, load_demo
    ap = argparse.ArgumentParser(description="decode + preview Crash 3 demo models")
    ap.add_argument("--disc", required=True)
    ap.add_argument("--model", action="append", help="eName(s), default all")
    ap.add_argument("--anim", help="pose animation eName (single --model)")
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--out", type=Path, default=Path("preview"))
    args = ap.parse_args()
    lib = Library(load_demo(args.disc))
    names = args.model or sorted(m.name for m in lib.models.values())
    for nm in names:
        me = lib.model(nm)
        if not me:
            print(f"{nm}: no such model"); continue
        a, f = default_pose(lib, me, args.anim)
        if a is None:
            print(f"{nm}: no animation poses it"); continue
        if args.frame:
            f = args.frame
        mesh = decode_pose(lib, me, a, f)
        s = mesh.stats
        pts = [v for v in mesh.verts if v is not None]
        ext = [(round(min(p[k] for p in pts), 3), round(max(p[k] for p in pts), 3))
               for k in range(3)]
        print(f"{nm} @ {mesh.pose}: {s['used_verts']}/{s['verts']} verts used "
              f"(special {s['special']}), {s['tris']} tris ({s['textured']} tex, "
              f"{s['skipped']} skipped), ext x{ext[0]} y{ext[1]} z{ext[2]}, "
              f"coll={mesh.coll[:1]}")
        preview(lib, mesh, args.out / f"{nm}.png")
