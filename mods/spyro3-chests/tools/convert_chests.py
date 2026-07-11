#!/usr/bin/env python3
"""Convert extracted S3 container models into S1 full-Model blobs and
pack them for the spyro3-chests mod.

Emits assets/s3/chests_s1pack.bin:
  header (76 B, mirrors pack_header_t in src/chests3.c)
  payload  — parked verbatim at PARK_LINK (0x80008000): 8 S1 Models
             (header + 1-frame AnimationHeader + colour blocks +
             faceblock + all-literal vertex stream + collision blob)
  host tail — basket/vase break ADPCM bodies + the 15bpp texel strip
             (uploaded to VRAM by the mod; never parked)

S1 target formats (calibrated against the stock Stone Hill dump,
plan: openpete-mod-spyro3-chests.md):
  Model:            +0 numAnims(1) +4 sounds[16](zero; mod copies stock
                    per level) +0x14 coll[8] +0x34 m_Data(0) +0x38 anims[1]
  AnimationHeader:  +0 numFrames u16, +2 numColors u16, +4 isSpyro u8,
                    +5 m_Scale u8(=0, stock), +6 shortShift u8(=0),
                    +7 radius u8, +8/+9 HP/LP vert counts, +0xB depth
                    scale, +0xC progressPerTick(0), +0x10 verts(0),
                    +0x14 faces, +0x18 colors, +0x1C lpFaces(=faces),
                    +0x20 lpColors, +0x24 frame0 {u32 vtxoff|coll<<21|
                    0xFF<<24; u16 coloroff; u8 shadow; u8 shortoff=nverts}
  vertex literal:   w = (VZ&0x7FF)<<21 | (VX&0x7FF)<<10 | ((VY>>1)&0x3FF)<<...
                    — see pack_vert(); VY is a 12-bit field whose LSB is
                    the next-is-short flag, so VY quantizes to even.
  fresh face record (renderer fcache walk, moby_native.c:1373):
    w0 = 0x80000000 | i0b<<20 | i1b<<11 | i2b<<2 | (tex?2:0)
    w1 = c0b<<17 | c1b<<8 | (c2b>>1)   (byte offsets = idx*4)
    w2 = i3b | c3b<<9
    textured: + [u0|v0<<8|clut<<16][u1|v1<<8|tpage<<16][u2|v2<<8|u3<<16|v3<<24]
    tri: i3=i2 (degenerate second NCLIP -> single tri, back-face culled)
    quad emit rule: (v0,v1,v2)+(v1,v3,v2) drawn iff ncl(v0,v1,v2)<0 and
    ncl(v3,v1,v2)>0 — strip order A,B,C,D.
  collision blob:   u32 boundingRadius; prims; type2 CYL = 5 words
                    {flags|2<<8|r<<16, zbot|ztop<<16, capzb|capzt<<16,
                    capr, 0}; last prim flags|=1.

Scale: FIT TO STOCK — each S3 model is uniformly scaled so its extents
fit the stock S1 model's extents (dumped, baked below). No cross-game
factor: chest size is per-game content (gems lesson).
"""
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ASSETS = HERE.parent / "assets" / "s3"

PACK_MAGIC = 0x31433353          # "S3C1"
PARK_LINK = 0x80008000
PARK_LIMIT = 0x80010000

# claimed VRAM rect (halfword columns x rows), 15bpp strip
VRAM_X, VRAM_Y = 896, 0
STRIP_W, STRIP_H = 128, 128
TPAGE_ATTR = (VRAM_X // 64) | ((VRAM_Y // 256) << 4) | (2 << 7)  # 15bpp, ABR 0

# stock per-slot constants (calibration dump, Stone Hill install pass 27)
#   env: model-unit extents (x0,x1,y0,y1,z0,z1); radius/depth/shadow: header
#   bytes; coll: stock cylinder (r, zbot, ztop, capzb, capzt, capr) or None
STOCK = {
    194: dict(env=(-296, 335, -512, 2, -342, 289), radius=9, depth=1,
              shadow=90, coll=(416, 96, 320, 96, 320, 416), bound=736),
    195: dict(env=(-427, 429, -466, 2, -304, 302), radius=10, depth=1,
              shadow=83, coll=(416, 96, 320, 96, 320, 416), bound=736),
    255: dict(env=(-172, 112, -108, 134, -76, 59), radius=3, depth=0,
              shadow=31, coll=None),
    256: dict(env=(-226, 187, -114, 122, -34, 42), radius=4, depth=0,
              shadow=44, coll=None),
    257: dict(env=(-56, 51, -62, 64, -113, -1), radius=3, depth=0,
              shadow=22, coll=None),
    309: dict(env=(-112, 92, -10, 62, -49, 3), radius=2, depth=0,
              shadow=25, coll=None),
    310: dict(env=(-65, 48, -98, 136, -224, 224), radius=4, depth=0,
              shadow=44, coll=None),
    311: dict(env=(-169, 176, -126, 118, -136, 136), radius=4, depth=0,
              shadow=34, coll=None),
}

# S1 slot <- S3 source model (key into chest_models.json "models")
MAPPING = [
    (194, "basket"),
    (195, "vase"),
    (255, "frag306_s0"), (256, "frag306_s1"), (257, "frag306_s0"),
    (309, "frag307_s0"), (310, "frag307_s1"), (311, "frag308_s0"),
]

# Winding: emit strip order (A,B,C,D). CH_FLIP=1 (default) swaps the
# emitted triangle orientation (S3's convention was globally opposite to
# S1's keep rule for the gems SHADED path; confirm for regular mobys
# in-game and lock the constant).
import os
CH_FLIP = int(os.environ.get("CH_FLIP", "1"))


def map_vec(x, y, z_true):
    """S3 (x, y, z_true up) -> S1 model (VX, VY=-z up, VZ=y): pure
    Rx(-90) rotation, det +1 (gems AXIS_MAP)."""
    return (x, -z_true, y)


def fit_anchored(verts_s1, env):
    """Chests: moby origin is the GROUND ANCHOR (stock y in [-512, +2]).
    Uniform scale from the up-extent (-minY) and the horizontal
    half-width; near-zero slivers below the anchor are clamped later,
    not allowed to poison the fit. Returns (scale, (0,0,0))."""
    x0, x1, y0, y1, z0, z1 = env
    up = max(1.0, -min(v[1] for v in verts_s1))
    hw = max(1.0, max(max(abs(v[0]), abs(v[2])) for v in verts_s1))
    stock_up = -y0
    stock_hw = max(abs(x0), x1, abs(z0), z1)
    return min(stock_up / up, stock_hw / hw), (0.0, 0.0, 0.0)


def fit_centered(verts_s1, env):
    """Fragments: debris chunks, not origin-anchored (stock bands like
    y[-113,-1]). Uniform span fit + translate our centre onto the stock
    centre. Returns (scale, translation applied AFTER scaling)."""
    x0, x1, y0, y1, z0, z1 = env
    s = 1e9
    tr = []
    for i, (lo, hi) in enumerate(((x0, x1), (y0, y1), (z0, z1))):
        vlo = min(v[i] for v in verts_s1)
        vhi = max(v[i] for v in verts_s1)
        span = max(1.0, vhi - vlo)
        s = min(s, (hi - lo) / span)
    for i, (lo, hi) in enumerate(((x0, x1), (y0, y1), (z0, z1))):
        vlo = min(v[i] for v in verts_s1)
        vhi = max(v[i] for v in verts_s1)
        tr.append((lo + hi) / 2.0 - s * (vlo + vhi) / 2.0)
    return s, tuple(tr)


def pack_vert(vx, vy, vz):
    """One literal stream word; flag bit (VY LSB) = 0 (next is literal)."""
    assert -1024 <= vx <= 1023 and -1024 <= vz <= 1023, (vx, vz)
    vy_field = int(round(vy / 2.0))          # 12-bit field, value = field*2
    assert -2048 <= vy_field <= 2047, vy
    w = ((vz & 0x7FF) << 21) | ((vx & 0x7FF) << 10)
    w |= (vy_field & 0xFFF) & ~1             # LSB clear = literal-next
    # NOTE: VY decode is (w<<20)>>19 = s12(bits0..11)<<1 — the field IS
    # bits 0..11 and bit0 doubles as the stream flag; clearing it costs
    # 2 model units of Y precision on odd values.
    return w


def s1_vy_check(w):
    vy = ((w << 20) & 0xFFFFFFFF)
    vy = (vy - (1 << 32)) if (vy & 0x80000000) else vy
    return vy >> 19


class ModelBuilder:
    def __init__(self, cls, src, stock):
        self.cls = cls
        self.src = src
        self.stock = stock
        self.warnings = []

    def build(self, base_vaddr, tiles, tile_place):
        """Emit the full Model blob linked at base_vaddr. Returns bytes."""
        src, stock = self.src, self.stock
        # --- geometry -----------------------------------------------------
        vs1 = [map_vec(x, y, z) for (x, y, z) in src["verts"]]
        anchored = self.cls in (194, 195)
        scale, tr = (fit_anchored if anchored else fit_centered)(
            vs1, stock["env"])
        verts = [(int(round(x * scale + tr[0])),
                  int(round(y * scale + tr[1])),
                  int(round(z * scale + tr[2]))) for (x, y, z) in vs1]
        if anchored:   # clamp sub-anchor slivers to the stock base plane
            y1 = stock["env"][3]
            verts = [(x, min(y, y1), z) for (x, y, z) in verts]
        nverts = len(verts)
        self.scale = scale
        # --- colours ------------------------------------------------------
        colours = [int(c, 16) & 0xFFFFFF for c in src["colours"]]
        if not colours:
            colours = [0x808080]
        ncol = len(colours)
        far = [(((c >> 16) & 0xFF) // 2) << 16 |
               (((c >> 8) & 0xFF) // 2) << 8 | ((c & 0xFF) // 2)
               for c in colours]           # LP/fog palette: half-bright
        # --- faces --------------------------------------------------------
        frecs = []
        for f in src["faces"]:
            v = f["v"]
            c = [min(ci, ncol - 1) for ci in f["c"]]
            tex = "uv" in f
            if f["tri"]:
                i = [v[0], v[1], v[2], v[2]]
                cc = [c[0], c[1], c[2], c[2]]
            else:
                i = [v[0], v[1], v[2], v[3]]
                cc = [c[0], c[1], c[2], c[3]]
            if CH_FLIP:
                # swap B<->C (verts 1,2) in strip order = reverse both tris
                if f["tri"]:
                    i = [i[0], i[2], i[1], i[1]]
                    cc = [cc[0], cc[2], cc[1], cc[1]]
                else:
                    i = [i[0], i[2], i[1], i[3]]
                    cc = [cc[0], cc[2], cc[1], cc[3]]
            rec = dict(i=i, c=cc, tex=None)
            if tex:
                uv = f["uv"]
                px, py = tile_place[tile_key_of(uv)]
                t = tiles_by_key(tiles)[tile_key_of(uv)]
                us = [px + (u - t["u0"]) for u in uv["u"]]
                vv = [py + (v_ - t["v0"]) for v_ in uv["v"]]
                if f["tri"]:
                    us = [us[0], us[1], us[2], us[2]]
                    vv = [vv[0], vv[1], vv[2], vv[2]]
                if CH_FLIP:
                    us = [us[0], us[2], us[1], us[3]]
                    vv = [vv[0], vv[2], vv[1], vv[3]]
                rec["tex"] = (us, vv)
            frecs.append(rec)

        # --- layout -------------------------------------------------------
        # model hdr 0x3C | anim hdr 0x2C | colours | far colours |
        # faceblock | vertex stream | collision
        off_model = 0
        off_anim = 0x3C
        off_cols = off_anim + 0x2C
        off_far = off_cols + 4 * ncol
        off_faces = off_far + 4 * ncol
        fb_bytes = sum(0x18 if r["tex"] else 0x0C for r in frecs)
        off_verts = off_faces + 4 + fb_bytes
        off_coll = off_verts + 4 * nverts
        coll = self.build_collision(scale)
        total = off_coll + len(coll)
        total = (total + 3) & ~3

        va = lambda off: base_vaddr + off
        blob = bytearray(total)

        # Model header
        struct.pack_into("<i", blob, 0, 1)                    # numAnims
        # sounds[16] left zero — the mod copies the live stock bytes at
        # patch time (sound def indices are PER LEVEL)
        if coll:
            struct.pack_into("<I", blob, 0x14, va(off_coll))  # coll[0]
        struct.pack_into("<I", blob, 0x34, 0)                 # m_Data
        struct.pack_into("<I", blob, 0x38, va(off_anim))      # anims[0]

        # AnimationHeader
        struct.pack_into("<hH", blob, off_anim, 1, ncol)      # frames, colors
        blob[off_anim + 4] = 0                                # isSpyro
        blob[off_anim + 5] = 0                                # m_Scale (stock)
        blob[off_anim + 6] = 0                                # shortShift
        blob[off_anim + 7] = self.stock["radius"]
        blob[off_anim + 8] = nverts                           # HP count
        blob[off_anim + 9] = nverts                           # LP count
        blob[off_anim + 0xB] = self.stock["depth"]
        blob[off_anim + 0xC] = 0                              # progress/tick
        struct.pack_into("<I", blob, off_anim + 0x10, 0)      # verts (Spyro only)
        struct.pack_into("<I", blob, off_anim + 0x14, va(off_faces))
        struct.pack_into("<I", blob, off_anim + 0x18, va(off_cols))
        struct.pack_into("<I", blob, off_anim + 0x1C, va(off_faces))  # LP=HP
        struct.pack_into("<I", blob, off_anim + 0x20, va(off_far))
        # frame0
        f0 = off_anim + 0x24
        vtx_va = va(off_verts)
        assert vtx_va < 0x80200000, "vertex stream must sit in low 2MB"
        struct.pack_into("<I", blob, f0,
                         (vtx_va & 0x1FFFFF) | (0 << 21) | (0xFF << 24))
        struct.pack_into("<H", blob, f0 + 4, 0)               # coloroff
        blob[f0 + 6] = self.stock["shadow"]
        blob[f0 + 7] = nverts                                 # shortoff: all literal

        # colours
        for i, c in enumerate(colours):
            struct.pack_into("<I", blob, off_cols + 4 * i, c)
        for i, c in enumerate(far):
            struct.pack_into("<I", blob, off_far + 4 * i, c)

        # faceblock
        struct.pack_into("<I", blob, off_faces, fb_bytes)
        fp = off_faces + 4
        for r in frecs:
            i0b, i1b, i2b, i3b = [ix * 4 for ix in r["i"]]
            c0b, c1b, c2b, c3b = [ci * 4 for ci in r["c"]]
            assert max(i0b, i1b, i2b, i3b) <= 0x7FC
            w0 = 0x80000000 | (i0b << 20) | (i1b << 11) | (i2b << 2)
            if r["tex"]:
                w0 |= 2
            w1 = (c0b << 17) | (c1b << 8) | (c2b >> 1)
            w2 = i3b | (c3b << 9)
            struct.pack_into("<3I", blob, fp, w0, w1, w2)
            fp += 12
            if r["tex"]:
                us, vv = r["tex"]
                struct.pack_into("<3I", blob, fp,
                                 (us[0] & 0xFF) | (vv[0] & 0xFF) << 8 | 0 << 16,
                                 (us[1] & 0xFF) | (vv[1] & 0xFF) << 8 |
                                 TPAGE_ATTR << 16,
                                 (us[2] & 0xFF) | (vv[2] & 0xFF) << 8 |
                                 (us[3] & 0xFF) << 16 | (vv[3] & 0xFF) << 24)
                fp += 12
        assert fp == off_verts, (fp, off_verts)

        # vertex stream (all literals)
        for i, (vx, vy, vz) in enumerate(verts):
            struct.pack_into("<I", blob, off_verts + 4 * i,
                             pack_vert(vx, vy, vz))

        # collision
        blob[off_coll:off_coll + len(coll)] = coll

        ext = [(min(v[i] for v in verts), max(v[i] for v in verts))
               for i in range(3)]
        print(f"  cls {self.cls} <- {self.src_name}: {nverts}v "
              f"{len(frecs)}f scale={scale:.3f} "
              f"ext x{ext[0]} y{ext[1]} z{ext[2]} "
              f"{'coll ' + str(len(coll)) + 'B' if coll else 'no coll'} "
              f"total {total}B")
        return bytes(blob)

    def build_collision(self, scale):
        st = self.stock
        if not st["coll"]:
            return b""
        # Reshape the stock cylinder by the fitted silhouette: preserve
        # the stock collision/model RATIOS (r/halfwidth, ztop/height).
        sr, szb, szt, sczb, sczt, scapr = st["coll"]
        x0, x1, y0, y1, z0, z1 = st["env"]
        stock_hw = max(abs(x0), x1, abs(z0), z1)
        stock_h = -y0
        vs1 = [map_vec(x, y, z) for (x, y, z) in self.src["verts"]]
        fit_hw = max(max(abs(v[0]), abs(v[2])) for v in vs1) * scale
        fit_h = -min(v[1] for v in vs1) * scale
        r = int(round(sr * fit_hw / stock_hw))
        ztop = int(round(szt * fit_h / stock_h))
        zbot = min(szb, ztop - 32)
        bound = int(round(st["bound"] * max(fit_hw / stock_hw,
                                            fit_h / stock_h)))
        w0 = 0x01 | (2 << 8) | (r << 16)
        blob = struct.pack("<I", bound & 0xFFFF)
        blob += struct.pack("<IIIII", w0,
                            (zbot & 0xFFFF) | (ztop << 16),
                            (zbot & 0xFFFF) | (ztop << 16),
                            r, 0)
        return blob


def tile_key_of(uv):
    return (uv["palette_x"], uv["palette_y"], uv["block_x"], uv["block_y"])


_tiles_cache = None
def tiles_by_key(tiles):
    global _tiles_cache
    if _tiles_cache is None:
        _tiles_cache = {(t["palette_x"], t["palette_y"],
                         t["block_x"], t["block_y"]): t for t in tiles}
    return _tiles_cache


def pack_strip(tiles):
    """Shelf-pack tiles into the 128x128 15bpp strip. Returns
    (strip bytes, {tile_key: (x,y)})."""
    place = {}
    strip = [[0] * STRIP_W for _ in range(STRIP_H)]
    x = y = shelf_h = 0
    for t in sorted(tiles, key=lambda t: (-t["w"], -t["h"])):
        w, h = t["w"], t["h"]
        if x + w > STRIP_W:
            x = 0
            y += shelf_h
            shelf_h = 0
        assert y + h <= STRIP_H, "strip overflow — grow STRIP_H"
        for r in range(h):
            row = t["texels_5551"][r]
            for cidx in range(w):
                strip[y + r][x + cidx] = int(row[cidx], 16)
        place[(t["palette_x"], t["palette_y"],
               t["block_x"], t["block_y"])] = (x, y)
        x += w
        shelf_h = max(shelf_h, h)
    raw = b"".join(struct.pack(f"<{STRIP_W}H", *row) for row in strip)
    return raw, place


def main():
    data = json.loads((ASSETS / "chest_models.json").read_text())
    models, tiles = data["models"], data["tiles"]

    strip, place = pack_strip(tiles)
    print(f"texture strip: {STRIP_W}x{STRIP_H} 15bpp "
          f"({len(strip)} bytes), {len(tiles)} tiles at {list(place.values())}")

    payload = bytearray()
    model_vaddr = []
    for cls, src_name in MAPPING:
        b = ModelBuilder(cls, models[src_name], STOCK[cls])
        b.src_name = src_name
        base = PARK_LINK + len(payload)
        blob = b.build(base, tiles, place)
        model_vaddr.append(base)
        payload += blob
        while len(payload) & 15:
            payload += b"\0"
    assert PARK_LINK + len(payload) <= PARK_LIMIT, \
        f"payload {len(payload)}B exceeds the low park window"
    print(f"payload: {len(payload)} bytes "
          f"({PARK_LIMIT - PARK_LINK - len(payload)} spare in park)")

    basket_vag = (ASSETS / "basket_break.vag_body.bin").read_bytes()
    vase_vag = (ASSETS / "vase_break.vag_body.bin").read_bytes()

    HDR = 76
    off = HDR + len(payload)
    hdr = struct.pack("<2I8I9I", PACK_MAGIC, len(payload), *model_vaddr,
                      off, len(basket_vag),
                      off + len(basket_vag), len(vase_vag),
                      off + len(basket_vag) + len(vase_vag),
                      STRIP_W, STRIP_H, VRAM_X, VRAM_Y)
    assert len(hdr) == HDR
    out = ASSETS / "chests_s1pack.bin"
    out.write_bytes(hdr + payload + basket_vag + vase_vag + strip)
    print(f"wrote {out.name}: {out.stat().st_size} bytes")


if __name__ == "__main__":
    main()
