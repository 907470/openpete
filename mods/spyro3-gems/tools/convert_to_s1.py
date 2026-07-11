#!/usr/bin/env python3
"""Convert extracted Spyro 3 gem geometry into Spyro 1 baked-gouraud
SimpleModels and pack them (plus the chime ADPCM body) into
assets/s3/gems_s1pack.bin for the spyro3-gems mod.

Input:  assets/s3/gem_anim<N>.json (N=0..4, from extract_s3_gems.py)
        assets/s3/gem_chime_arrival.vag_body.bin (the rising-pitch chime)
Output: assets/s3/gems_s1pack.bin

S3 source semantics (extract_s3_gems.py report):
  - frame0.verts are sign-corrected ints with z UNhalved: true z = z/2
    (SpyroEdit non-animated path halves z relative to x/y).
  - colour table is a single grey; every face is colouredByMoby and the
    per-corner 7-bit fields are SHADING RAMP values (0..127), tinted at
    runtime from S3's palette D_80066998: 4 words per gem colour
    {dark1, bright1, dark2, bright2}, format 0x00BBGGRR.
  We bake the tint: a 16-step dark1->bright1 gradient table per model,
  corner colour index = ramp >> 3.

S1 target format (docs/injection-spec.md, confirmed by calibration run):
  model:  +0 s8 nverts | +1 u8 nfaces | +2 s8 nearBias(0x36) | +3 0xFF
          +4 m_Verts (== model+0x10 -> BAKED colour mode)
          +8 m_Colors | +0xC m_Faces
          +0x10 verts: 3 bytes each, order (Z,X,Y), stored value = coord/2
          colour table: u32 0x00BBGGRR, max 128 words
          faces: 8B: w0 = i0<<23|i1<<16|i2<<9|i3<<2 (flag bits 0,1 = 0)
                     w1 = c0<<23|c1<<16|c2<<9|c3<<2
          quad perimeter A,B,C,D -> i = A,B,D,C ; tri -> i2==i3, c3=c2
Pack header (matches pack_header_t in src/gems3.c):
  u32 magic 'S3G1' | u32 payload_size | u32 model_vaddr[5]
  u32 vag_vaddr | u32 vag_size | u32 pitch_base | u32 burst_rgb[5]
"""
import json, struct, os
from pathlib import Path

PARK_BASE = 0x80008000
TARGET_EXTENT = 128         # stock gems: extent [-128, 96] (mod boot log)
NEAR_BIAS = 0x36            # stock gem header byte +2 (mod boot log)
GRAD_STEPS = 16
PITCH_BASE = 0x400          # S3 chime centre pitch (unkC==unkE==0x400)
SWAP_TRI = os.environ.get("S3G_SWAP_TRI", "1") == "1"
SWAP_QUAD = os.environ.get("S3G_SWAP_QUAD", "0") == "1"
INVERT_RAMP = os.environ.get("S3G_INVERT_RAMP", "1") == "1"

HERE = Path(__file__).resolve().parent
S3 = HERE.parent / "assets" / "s3"

# S3 gem palette D_80066998 (0x00BBGGRR), colour index = substate+1,
# words {dark1, bright1, dark2, bright2} — from spyro-3 game.data.s.
PALETTE = [
    (0x000040, 0x2000FF),   # substate 0: red   (value 1)
    (0x104808, 0x20FF00),   # substate 1: green (value 2)
    (0x480020, 0xFF0080),   # substate 2: blue-violet (value 5)
    (0x005066, 0x00C0FF),   # substate 3: gold  (value 10)
    (0x240034, 0x8000C0),   # substate 4: pink  (value 25)
]


def lerp_bgr(dark, bright, t):
    out = 0
    for shift in (0, 8, 16):
        d, b = (dark >> shift) & 0xFF, (bright >> shift) & 0xFF
        out |= (d + round((b - d) * t)) << shift
    return out


def build_model(anim_json, substate, scale):
    f0 = anim_json["frame0"]
    verts = [(x, y, z / 2.0) for x, y, z in f0["verts"]]
    zmid = (min(v[2] for v in verts) + max(v[2] for v in verts)) / 2.0
    verts = [(x, y, z - zmid) for x, y, z in verts]

    if len(verts) > 127:
        raise SystemExit(f"too many verts ({len(verts)})")

    vstream = bytearray()
    for x, y, z in verts:
        def enc(c):
            return max(-128, min(127, round(c * scale / 2))) & 0xFF
        vstream += bytes([enc(z), enc(x), enc(y)])     # stream order Z,X,Y
    while len(vstream) % 4:
        vstream.append(0)

    dark, bright = PALETTE[substate]
    colours = [lerp_bgr(dark, bright, i / (GRAD_STEPS - 1)) for i in range(GRAD_STEPS)]

    def cidx(ramp):
        i = min(GRAD_STEPS - 1, max(0, ramp >> 3))
        return (GRAD_STEPS - 1 - i) if INVERT_RAMP else i

    fstream = bytearray()
    nfaces = 0
    for f in f0["faces"]:
        v = f["v"]
        c = [cidx(r) for r in f["c"]]
        c += [c[-1]] * (4 - len(c))
        packs = []
        if f.get("tri") or len(v) == 3:
            # S1 lone tris are single-sided (NCLIP>0); S3 gem tris render
            # screen-facing. Emit both windings so no facet ever drops out.
            for order in ([0, 1, 2], [0, 2, 1]):
                q, qc = [v[k] for k in order], [c[k] for k in order]
                packs.append(([q[0], q[1], q[2], q[2]],
                              [qc[0], qc[1], qc[2], qc[2]]))
        else:
            # S3 quads are stored in the same strip order S1 packs
            # (i0,i1,i2,i3 -> tris (0,1,2)+(1,3,2)); copy through.
            order = [0, 1, 2, 3] if not SWAP_QUAD else [1, 0, 3, 2]
            packs.append(([v[k] for k in order], [c[k] for k in order]))
        for i, cc in packs:
            if any(ix >= len(verts) for ix in i):
                raise SystemExit(f"vert index OOB in face {f}")
            w0 = (i[0] << 23) | (i[1] << 16) | (i[2] << 9) | (i[3] << 2)
            w1 = (cc[0] << 23) | (cc[1] << 16) | (cc[2] << 9) | (cc[3] << 2)
            fstream += struct.pack("<II", w0, w1)
            nfaces += 1
    if nfaces > 255:
        raise SystemExit(f"too many faces ({nfaces})")

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

    # one global scale so denominations keep their S3 relative sizes
    gmax = 0.0
    for a in anims:
        f0 = a["frame0"]
        zs = [z / 2.0 for _, _, z in f0["verts"]]
        zmid = (min(zs) + max(zs)) / 2.0
        for (x, y, z), zc in zip(f0["verts"], (z - zmid for z in zs)):
            gmax = max(gmax, abs(x), abs(y), abs(zc))
    scale = TARGET_EXTENT / gmax
    print(f"global extent {gmax:.1f} -> scale {scale:.3f}")

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
        burst.append(0x2C000000 | PALETTE[n][1])
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
