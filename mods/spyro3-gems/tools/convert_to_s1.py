#!/usr/bin/env python3
"""Convert extracted Spyro 3 gem geometry into Spyro 1 SHADED-METAL
SimpleModels and pack them (plus the chime ADPCM body) into
assets/s3/gems_s1pack.bin for the spyro3-gems mod.

Input:  assets/s3/gem_anim<N>.json (N=0..4, from extract_s3_gems.py)
        assets/s3/gem_chime_arrival.vag_body.bin (the rising-pitch chime)
Output: assets/s3/gems_s1pack.bin

S3 source semantics (see assets/s3/shading_canonical.md — the original
"per-corner ramp" reading was a mis-parse):
  - frame0.verts are sign-corrected ints with z UNhalved: true z = z/2.
  - Every gem face is FLAT + shiny: w1 packs a signed 8-bit×3 face
    normal (n = (-IR3,-IR1,-IR2) in vert space) plus a palette-pair bit.
    S3's per-frame specular formula is bit-identical to Spyro 1's METAL
    sub-mode, so faces convert to S1 metal flat faces (w0 bits 0+1 set,
    w1 = packed normal) and the stock gem mobys' m_SpecularMetalColor
    supplies the five per-class tints via D_8006E44C rows 1..5.
  - Orientation: stock S1 gems put the tall axis on model -Y with the
    slim tip (the wobble pivot) at the origin — see AXIS_MAP below.

S1 target format (docs/injection-spec.md, confirmed by calibration run):
  model:  +0 s8 nverts | +1 u8 nfaces | +2 s8 nearBias(0x36) | +3 0xFF
          +4 m_Verts (== model+0x10, stock-gem layout)
          +8 m_Colors (vestigial 1-word table) | +0xC m_Faces
          +0x10 verts: 3 bytes each, order (Z,X,Y), stored value = coord/2
          faces: 8B: w0 = i0<<23|i1<<16|i2<<9|i3<<2 | 3 (flat+metal)
                     w1 = Nz<<24|Nx<<16|Ny<<8 ; tri iff i2==i3
Pack header (matches pack_header_t in src/gems3.c):
  u32 magic 'S3G1' | u32 payload_size | u32 model_vaddr[5]
  u32 vag_vaddr | u32 vag_size | u32 pitch_base | u32 burst_rgb[5]
"""
import json, struct, os
from pathlib import Path

PARK_BASE = 0x80008000
NEAR_BIAS = 0x36            # stock gem header byte +2 (mod boot log)
PITCH_BASE = 0x400          # S3 chime pitch: 0x400 + 0x40*n (canonical)

HERE = Path(__file__).resolve().parent
S3 = HERE.parent / "assets" / "s3"

# Collect-burst words (0x00BBGGRR): S3 hues rescaled to stock magnitude.
# The S1 particle updater decrements each channel by 8/tick and kills the
# particle only when ALL channels are exactly 0 — so channels must be
# multiples of 8 and ≤0x80 for stock 16-tick life/size (see
# assets/s3/burst_canonical.md; overbright/odd channels wrap → immortal,
# ever-growing particles).
BURST = [0x100080, 0x108000, 0x800040, 0x006080, 0x580080]

# S3 gem palette D_80066998 (0x00BBGGRR), colour index = substate+1,
# words {dark1, bright1, dark2, bright2} — from spyro-3 game.data.s.
PALETTE = [
    (0x000040, 0x2000FF),   # substate 0: red   (value 1)
    (0x104808, 0x20FF00),   # substate 1: green (value 2)
    (0x480020, 0xFF0080),   # substate 2: blue-violet (value 5)
    (0x005066, 0x00C0FF),   # substate 3: gold  (value 10)
    (0x240034, 0x8000C0),   # substate 4: pink  (value 25)
]


# Axis map: S3 JSON-space (x, y, z_true) -> S1 model-space (VX, VY, VZ).
# Stock S1 gems (S3G_DUMP_VERTS ground truth): tall axis = model Y, body
# entirely on -Y, slim tip AT THE ORIGIN (the wobble pivot), rest pose is
# identity. S3 gems: tall axis = +z_true, tip at 0, crown at max. Hence
# (VX,VY,VZ) = (sx, -sz_true, sy) — det +1 (a pure Rx(-90°)), so face
# windings are preserved. Applied to BOTH vertices and face normals.
# Do NOT centre the tall axis: the tip must stay at 0.
AXIS_MAP = [(0, +1), (2, -1), (1, +1)]


def map_vec(v3):
    return tuple(sign * v3[src] for src, sign in AXIS_MAP)


def axis_map_parity():
    """-1 if AXIS_MAP mirrors the model (odd permutation × sign flips)."""
    perm = [src for src, _ in AXIS_MAP]
    swaps = sum(1 for a in range(3) for b in range(a + 1, 3)
                if perm[a] > perm[b])
    flips = sum(1 for _, sign in AXIS_MAP if sign < 0)
    return -1 if (swaps + flips) % 2 else 1


FLIP_N = os.environ.get("S3G_FLIP_N", "1") == "1"


def face_normal(f):
    """JSON-space face normal from raw w1: n = (-IR3, -IR1, -IR2)
    (S3 mobydraw func_80033C5C; see assets/s3/shading_canonical.md).
    S3G_FLIP_N=1 negates — A/B toggle for the outward-direction sign."""
    w1 = int(f["raw"][1], 16)

    def s8(b):
        return b - 256 if b > 127 else b
    ir1, ir2, ir3 = s8((w1 >> 15) & 0xFF), s8((w1 >> 7) & 0xFF), s8((w1 >> 23) & 0xFF)
    n = (-ir3, -ir1, -ir2)
    return tuple(-c for c in n) if FLIP_N else n


def pack_metal_face(idx, n):
    """8-byte S1 SHADED face: flat+metal flags, w1 = face normal
    (b3=Nz, b2=Nx, b1=Ny — spec §A.5 flat-face layout)."""
    def c8(v):
        return max(-128, min(127, int(round(v)))) & 0xFF
    w0 = (idx[0] << 23) | (idx[1] << 16) | (idx[2] << 9) | (idx[3] << 2) | 3
    w1 = (c8(n[2]) << 24) | (c8(n[0]) << 16) | (c8(n[1]) << 8)
    return struct.pack("<II", w0, w1)


def build_model(anim_json, substate, scale):
    f0 = anim_json["frame0"]
    verts = [map_vec((x, y, z / 2.0)) for x, y, z in f0["verts"]]
    mirrored = axis_map_parity() < 0

    if len(verts) > 127:
        raise SystemExit(f"too many verts ({len(verts)})")

    vstream = bytearray()
    for x, y, z in verts:
        def enc(c):
            return max(-128, min(127, round(c * scale / 2))) & 0xFF
        vstream += bytes([enc(z), enc(x), enc(y)])     # stream order Z,X,Y
    while len(vstream) % 4:
        vstream.append(0)

    fstream = bytearray()
    nfaces = 0
    for f in f0["faces"]:
        v = f["v"]
        n = map_vec(face_normal(f))
        if mirrored:
            v = list(reversed(v))          # restore outward winding
        packs = []
        if f.get("tri") or len(v) == 3:
            # S1 lone tris are single-sided (NCLIP>0); S3 gem tris render
            # screen-facing. Emit both windings (reverse copy gets the
            # negated normal) so no facet ever drops out.
            packs.append(([v[0], v[1], v[2], v[2]], n))
            packs.append(([v[0], v[2], v[1], v[1]],
                          tuple(-c for c in n)))
        else:
            # S3 quads are stored in the same strip order S1 packs
            # (i0,i1,i2,i3 -> tris (0,1,2)+(1,3,2)); copy through.
            packs.append(([v[0], v[1], v[2], v[3]], n))
        for idx, nn in packs:
            if any(ix >= len(verts) for ix in idx):
                raise SystemExit(f"vert index OOB in face {f}")
            fstream += pack_metal_face(idx, nn)
            nfaces += 1
    if nfaces > 255:
        raise SystemExit(f"too many faces ({nfaces})")

    # All faces are flat+metal: the gouraud colour table is vestigial but
    # the baked-mode layout (m_Verts == model+0x10, ctab before faces) is
    # kept identical to the stock gems.
    colours = [PALETTE[substate][0]]
    ctab = b"".join(struct.pack("<I", w) for w in colours)
    body = bytearray()
    body += struct.pack("<bBbB", len(verts), nfaces, NEAR_BIAS, 0xFF)
    body += b"\x00" * 12                     # ptr slots filled at pack time
    body += vstream + ctab + fstream
    off_ctab = 0x10 + len(vstream)
    off_faces = off_ctab + len(ctab)
    assert off_faces - off_ctab < 0x2000
    return bytes(body), 0x10, off_ctab, off_faces


def main():
    anims = [json.loads((S3 / f"gem_anim{n}.json").read_text()) for n in range(5)]

    # one global scale so denominations keep their S3 relative sizes.
    # Constraints (stock convention): tall axis VY spans [-128, 0] (tip
    # pinned at 0), horizontal VX/VZ must stay within ±127.
    tall = max(z / 2.0 for a in anims for _, _, z in a["frame0"]["verts"])
    horiz = max(max(abs(x), abs(y)) for a in anims
                for x, y, _ in a["frame0"]["verts"])
    scale = min(128.0 / tall, 127.0 / horiz)
    print(f"tall {tall:.1f} horiz {horiz:.1f} -> scale {scale:.3f}")

    force = os.environ.get("S3G_FORCE_STATE")
    payload = bytearray()
    model_vaddr, burst = [], []
    for n, a in enumerate(anims):
        sub = int(force) if force is not None else n
        a = anims[sub]
        body, ov, oc, of = build_model(a, sub, scale)
        while len(payload) % 16:
            payload.append(0)
        base = PARK_BASE + len(payload)
        body = bytearray(body)
        body[4:16] = struct.pack("<III", base + ov, base + oc, base + of)
        payload += body
        model_vaddr.append(base)
        burst.append(0x2C000000 | BURST[n])
        print(f"state {n}: verts={a['numVerts']} faces={len(a['frame0']['faces'])} "
              f"model @{base:08x} size={len(body)}")

    vag = (S3 / "gem_chime_arrival.vag_body.bin").read_bytes()
    while len(payload) % 16:
        payload.append(0)
    vag_vaddr = PARK_BASE + len(payload)
    payload += vag

    hdr = struct.pack("<II5IIII5I",
                      0x31473353, len(payload), *model_vaddr,
                      vag_vaddr, len(vag), PITCH_BASE, *burst)
    assert len(hdr) == 4 * 15, len(hdr)  # matches sizeof(pack_header_t)
    out = S3 / "gems_s1pack.bin"
    out.write_bytes(hdr + payload)
    print(f"pack: {out} header={len(hdr)}B payload={len(payload)}B "
          f"vag={len(vag)}B @{vag_vaddr:08x} pitch_base={PITCH_BASE:#x}")


if __name__ == "__main__":
    main()
