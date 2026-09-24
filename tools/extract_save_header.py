#!/usr/bin/env python3
"""Slice a byte range of a PSX image out of a decomp's spimdisasm data
listing and write it as a raw binary.

The Spyro 2 and Spyro 3 decomps keep the memory-card save-file header
(SaveFileHeader: "SC" magic, Shift-JIS title, CLUT, 16x16 4bpp icon frames)
as .word/.byte directives in a data .s file rather than as an .incbin asset.
spimdisasm prefixes every data line with `/* <rom> <vaddr> [<bytes>] */`;
the bytes field is the line's content in image order, and lines without it
(.byte) carry their value in the directive. The listing is the image, so
the range read here is byte-identical to the built executable or overlay.

    extract_save_header.py <data.s> --vaddr 0x8006410C --size 512 --out hdr.bin
"""

import argparse
import re
import sys
from pathlib import Path

LINE = re.compile(
    r"/\*\s*[0-9A-Fa-f]+\s+([0-9A-Fa-f]{8})(?:\s+([0-9A-Fa-f]+))?\s*\*/\s*"
    r"\.(word|half|short|byte)\s+(\S+)")
WIDTH = {"word": 4, "half": 2, "short": 2, "byte": 1}


def image_bytes(text: str) -> dict[int, int]:
    """Map vaddr -> byte for every data line in the listing."""
    out: dict[int, int] = {}
    for m in LINE.finditer(text):
        vaddr = int(m.group(1), 16)
        hexbytes, directive, value = m.group(2), m.group(3), m.group(4)
        width = WIDTH[directive]
        if hexbytes:
            data = bytes.fromhex(hexbytes)
        else:
            data = (int(value, 0) & ((1 << (8 * width)) - 1)).to_bytes(
                width, "little")
        if len(data) != width:
            sys.exit(f"line at {vaddr:#x}: {len(data)} bytes for .{directive}")
        for i, b in enumerate(data):
            out[vaddr + i] = b
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("listing", type=Path, help="spimdisasm data .s file")
    ap.add_argument("--vaddr", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--size", type=lambda s: int(s, 0), default=512)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    image = image_bytes(args.listing.read_text())
    missing = [a for a in range(args.vaddr, args.vaddr + args.size)
               if a not in image]
    if missing:
        sys.exit(f"{args.listing}: {len(missing)} bytes of "
                 f"[{args.vaddr:#x}, {args.vaddr + args.size:#x}) absent, "
                 f"first {missing[0]:#x}")
    data = bytes(image[a] for a in range(args.vaddr, args.vaddr + args.size))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
