#!/usr/bin/env python3
"""extract_s3_player.py - carve the Spyro 3 (SCUS-94467) player model
blob out of the user's own Year of the Dragon disc image.

A mod that renders Spyro 3's hover cels through the native actor render
service needs the S3 player model; that model
(mesh + boot animation set, ~136 KiB) is Activision/Insomniac data, so -
same legal posture as every openpete asset tool - mods never SHIP it:
this tool derives it locally from a disc the user supplies:

    tools/extract_s3_player.py \
        --disc "roms/SCUS-94467/SCUS-94467.bin" \
        --out  mods/<id>/data/s3_player.bin

Format facts (spyro-3 decomp, disc-verified):
  - WAD.WAD sits in the ISO9660 filesystem (LBA 500 on the known image;
    located by directory parse here, not hardcoded).
  - wadHeader = first 0x620 bytes of WAD.WAD; bytes 0..7 are
    DataHeader spyroMdls {offset, size} (spyro-3 include/common.h).
  - The segment: word0 = payload byte length, word1 = model offset + 0x800;
    the model itself starts at segment+0x800+word1-0x800 (= +word1).
  - Model header: numAnims u32 @+0 (62), total size @+0x34, anim pointer
    table @+0x3C (model-relative u32 offsets, -1 = absent).
  - HOVER = anim index 0x12 (state->anim mapping is identity in S3).
"""

import argparse
import struct
import sys
from pathlib import Path

SEC_DATA = 2048
SEC_RAW = 2352
SEC_RAW_SKEW = 24
HOVER_ANIM = 0x12


class Disc:
    def __init__(self, path: Path):
        self.f = open(path, "rb")
        probe = self.f.read(12)
        self.raw = probe[:2] == b"\x00\xff" and probe[11:12] == b"\x00"

    def sector(self, n: int) -> bytes:
        off = n * SEC_RAW + SEC_RAW_SKEW if self.raw else n * SEC_DATA
        self.f.seek(off)
        return self.f.read(SEC_DATA)

    def read_span(self, start_sector: int, byte_off: int, length: int) -> bytes:
        start_sector += byte_off // SEC_DATA
        byte_off %= SEC_DATA
        out = bytearray()
        s = start_sector
        while len(out) < byte_off + length:
            out += self.sector(s)
            s += 1
        return bytes(out[byte_off:byte_off + length])


def iso_find(disc: Disc, name: str):
    """Minimal ISO9660 root-directory lookup -> (lba, size)."""
    pvd = disc.sector(16)
    if pvd[1:6] != b"CD001":
        raise SystemExit("no ISO9660 PVD at sector 16")
    root = pvd[156:156 + 34]
    lba = struct.unpack_from("<I", root, 2)[0]
    size = struct.unpack_from("<I", root, 10)[0]
    data = disc.read_span(lba, 0, size)
    pos = 0
    while pos < len(data):
        rec_len = data[pos]
        if rec_len == 0:  # move to next sector boundary
            pos = (pos // SEC_DATA + 1) * SEC_DATA
            continue
        name_len = data[pos + 32]
        rec_name = data[pos + 33:pos + 33 + name_len].decode("ascii",
                                                             "replace")
        rec_name = rec_name.split(";")[0]
        if rec_name.upper() == name.upper():
            return (struct.unpack_from("<I", data, pos + 2)[0],
                    struct.unpack_from("<I", data, pos + 10)[0])
        pos += rec_len
    raise SystemExit(f"{name} not found in the ISO root directory")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", required=True, type=Path,
                    help=".img/.bin/.iso (CloneCD raw or cooked)")
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()

    disc = Disc(args.disc)
    wad_lba, wad_size = iso_find(disc, "WAD.WAD")
    print(f"WAD.WAD at LBA {wad_lba}, {wad_size} bytes")

    mdls_off, mdls_size = struct.unpack("<ii", disc.read_span(wad_lba, 0, 8))
    print(f"spyroMdls segment: offset {mdls_off:#x}, size {mdls_size:#x}")
    if mdls_size <= 0x800 or mdls_size > 0x100000:
        raise SystemExit("implausible spyroMdls segment size")

    seg = disc.read_span(wad_lba, mdls_off, mdls_size)
    payload_len, model_off = struct.unpack_from("<ii", seg, 0)
    print(f"segment: payload {payload_len:#x} bytes, model at +{model_off:#x}")
    model = seg[model_off:]

    num_anims = struct.unpack_from("<I", model, 0)[0]
    total = struct.unpack_from("<I", model, 0x34)[0]
    if num_anims != 62:
        raise SystemExit(f"expected 62 player anims, found {num_anims} - "
                         "not the SCUS-94467 player model?")
    model = model[:total]
    anim_off = struct.unpack_from("<i", model, 0x3C + HOVER_ANIM * 4)[0]
    if anim_off <= 0 or anim_off >= total:
        raise SystemExit(f"hover anim (0x{HOVER_ANIM:x}) not resident "
                         f"(offset {anim_off:#x})")
    nf, nc, hvs, nv = struct.unpack_from("<4B", model, anim_off)
    print(f"player model: {num_anims} anims, {total} bytes; hover @"
          f"+{anim_off:#x}: {nf} frame records, {nc} colours, "
          f"{nv} verts (head from {hvs})")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(model)
    print(f"wrote {args.out} ({len(model)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
