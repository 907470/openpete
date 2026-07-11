#!/usr/bin/env python3
"""extract_wad_entry.py — pull a level's Data entry out of WAD.WAD.

The level-segment asset overlay (games/spyro-1/mod_wad_overlay.c, PoC
rung 9) serves mod files named assets/wad/level<ID>.data.bin in place of
the disc's bytes. Mods may not SHIP derived game data (legal posture),
so blobs are derived locally from the user's own disc — this tool is
that derivation step:

    tools/extract_wad_entry.py --disc game.cue --level 10 \
        --out my-mod/assets/wad/level10.data.bin

Optionally --recolor-bg R,G,B patches the cyclorama background colour
(docs/level-format.md §4.6) as a minimal, format-safe edit example.
"""

import argparse
import struct
import sys
from pathlib import Path

SEC_DATA = 2048
SEC_RAW = 2352
SEC_RAW_SKEW = 24
WAD_HDR_LEVELS = 72  # byte offset of m_LevelEntry[0] (see wad.h)
WAD_SECTOR = 37      # WAD.WAD start (game_disc_layout)


def resolve_bin(disc: Path) -> Path:
    if disc.suffix.lower() == ".cue":
        for line in disc.read_text(errors="replace").splitlines():
            if "FILE" in line and '"' in line:
                name = line.split('"')[1]
                return (disc.parent / name)
        raise SystemExit(f"no FILE line in {disc}")
    return disc


class Disc:
    def __init__(self, path: Path):
        self.f = open(path, "rb")
        probe = self.f.read(12)
        self.raw = probe[:2] == b"\x00\xff" and probe[11:12] == b"\x00"

    def sector(self, n: int) -> bytes:
        off = n * SEC_RAW + SEC_RAW_SKEW if self.raw else n * SEC_DATA
        self.f.seek(off)
        return self.f.read(SEC_DATA)

    def read_span(self, start_sector: int, length: int) -> bytes:
        out = bytearray()
        s = start_sector
        while len(out) < length:
            out += self.sector(s)
            s += 1
        return bytes(out[:length])


def level_entry(disc: Disc, level_id: int):
    idx = (level_id // 10 - 1) * 6 + level_id % 10
    if not 0 <= idx < 36:
        raise SystemExit(f"level id {level_id} out of range")
    hdr = disc.sector(WAD_SECTOR)
    off, ln = struct.unpack_from("<ii", hdr, WAD_HDR_LEVELS + idx * 16 + 8)
    if ln <= 0 or off % SEC_DATA:
        raise SystemExit(f"level {level_id}: empty/unaligned Data entry")
    return WAD_SECTOR + off // SEC_DATA, ln


def recolor_bg(blob: bytearray, rgb):
    """Walk LevelHeader -> environment payload -> skip components 1..5 ->
    cyclorama background colour (docs/level-format.md §3-§4)."""
    data_off, _data_size = struct.unpack_from("<ii", blob, 8)
    pos = data_off
    for _ in range(5):  # textures, sectors, occlusion, surfaces, collision
        (size,) = struct.unpack_from("<i", blob, pos)
        if size <= 0 or pos + size > len(blob):
            raise SystemExit(f"component walk broke at 0x{pos:x}")
        pos += size
    bg = pos + 4  # skip the cyclorama component's size word
    old = tuple(blob[bg:bg + 3])
    blob[bg:bg + 3] = bytes(rgb)
    print(f"cyclorama bg colour {old} -> {tuple(rgb)} at 0x{bg:x}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", required=True, type=Path)
    ap.add_argument("--level", required=True, type=int)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--recolor-bg", type=str, default=None,
                    help="R,G,B (0-255) cyclorama background patch")
    args = ap.parse_args()

    disc = Disc(resolve_bin(args.disc))
    start, length = level_entry(disc, args.level)
    blob = bytearray(disc.read_span(start, length))
    print(f"level {args.level}: Data entry sector {start}, {length} bytes")

    if args.recolor_bg:
        rgb = [int(x) & 0xFF for x in args.recolor_bg.split(",")]
        assert len(rgb) == 3
        recolor_bg(blob, rgb)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
