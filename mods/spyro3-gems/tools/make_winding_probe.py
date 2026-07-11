#!/usr/bin/env python3
"""Winding probe: replace the gem models with a diagnostic model that
shows, from a top-down camera, which vertex winding S1's SHADED
renderer keeps. Four horizontal faces at height VY=-40, all packed with
an up normal (metal shading identical), separated in X/Z:

    back row  (VZ=-45):  TRI A = (0,1,2)      TRI B = (0,2,1)
    front row (VZ=+45):  QUAD A = (0,1,2,3)   QUAD B = (0,2,1,3)

Viewed from above: whichever of each pair is visible names the
convention. Writes assets/s3/gems_s1pack.bin (probe; rerun
convert_to_s1.py to restore the real pack).
"""
import struct
from pathlib import Path

PARK = 0x80008000
S3 = Path(__file__).resolve().parent.parent / "assets" / "s3"

def enc(c):
    return max(-128, min(127, round(c / 2))) & 0xFF

def vert(vx, vy, vz):
    return bytes([enc(vz), enc(vx), enc(vy)])

verts = [
    # tri A (left, back) — LARGE
    (-110, -40, -80), (-10, -40, -80), (-60, -40, -10),
    # tri B (right, back) — SMALL (half size: visible small tri => B wins)
    (40, -40, -60), (80, -40, -60), (60, -40, -30),
    # quad A (left, front) — LARGE
    (-110, -40, 20), (-10, -40, 20), (-110, -40, 100), (-10, -40, 100),
    # quad B (right, front) — SMALL
    (40, -40, 40), (80, -40, 40), (40, -40, 80), (80, -40, 80),
]
UP = 0x80  # Nz=0, Nx=0, Ny=-128 (up): w1 = 0<<24 | 0<<16 | 0x80<<8

def face(i0, i1, i2, i3):
    w0 = (i0 << 23) | (i1 << 16) | (i2 << 9) | (i3 << 2) | 3
    w1 = UP << 8
    return struct.pack("<II", w0, w1)

faces = (
    face(0, 1, 2, 2) +          # TRI A: direct
    face(3, 5, 4, 4) +          # TRI B: swapped (0,2,1 of its triple)
    face(6, 7, 8, 9) +          # QUAD A: direct strip
    face(10, 12, 11, 13)        # QUAD B: middle-swapped strip
)

vstream = b"".join(vert(*v) for v in verts)
while len(vstream) % 4:
    vstream += b"\x00"
ctab = struct.pack("<I", 0x000040)
body = struct.pack("<bBbB", len(verts), 4, 0x36, 0xFF) + b"\x00" * 12
body += vstream + ctab + faces
off_ctab = 0x10 + len(vstream)
off_faces = off_ctab + 4

model_vaddrs = []
payload = bytearray()
for n in range(5):
    while len(payload) % 16:
        payload.append(0)
    base = PARK + len(payload)
    b = bytearray(body)
    b[4:16] = struct.pack("<III", base + 0x10, base + off_ctab, base + off_faces)
    payload += b
    model_vaddrs.append(base)

vag = (S3 / "gem_chime_arrival.vag_body.bin").read_bytes()
while len(payload) % 16:
    payload.append(0)
vag_vaddr = PARK + len(payload)
payload += vag

hdr = struct.pack("<II5IIII5I", 0x31473353, len(payload), *model_vaddrs,
                  vag_vaddr, len(vag), 0x400,
                  *([0x2C100080] * 5))
(S3 / "gems_s1pack.bin").write_bytes(hdr + payload)
print("winding probe pack written:",
      f"{len(payload)}B payload, tri A/B back row, quad A/B front row")
