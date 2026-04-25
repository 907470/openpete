#!/usr/bin/env python3
"""Synthesize a PS1-EXE wrapper around a raw overlay binary.

psxrecomp ingests PS1-EXE files. Spyro's level/titlescreen overlays are raw
.text+.rodata+.data blobs that get loaded to a fixed kseg0 vaddr (e.g.
0x8007AA38) — they have no PS1-EXE header. This tool builds the 2 KiB header
in front of the raw bytes so the recompiler sees a valid executable.

The resulting file is identical, byte-for-byte, to a real PS1 EXE that loads
the overlay's bytes at its overlay vaddr; only the entry point is faked
(set to load_address — the recompiler's actual entry points come from
--forced-entry / --extra-funcs, not the header).

Usage:
    synth_overlay_exe.py <input.ovl> <output.bin> --load-addr=0xADDR
"""
import argparse
import struct
import sys
from pathlib import Path


HEADER_SIZE = 2048


def synth(in_path: Path, out_path: Path, load_addr: int) -> None:
    body = in_path.read_bytes()
    header = bytearray(HEADER_SIZE)
    header[0:8] = b"PS-X EXE"
    # 0x10..0x1F: initial_pc, initial_gp, load_address, file_size
    struct.pack_into("<IIII", header, 0x10, load_addr, 0, load_addr, len(body))
    # 0x30..0x37: initial_sp, initial_fp — overlays don't get a stack of
    # their own; leave zero. The recompiler doesn't consult these.
    # 0x4C..: copyright string is optional; skip.
    out_path.write_bytes(bytes(header) + body)
    print(
        f"  wrote {out_path} "
        f"(load=0x{load_addr:08X}, size={len(body)} = 0x{len(body):X} bytes)"
    )


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", type=Path, help="raw overlay bytes (.ovl file)")
    p.add_argument("output", type=Path, help="output PS1-EXE wrapper (.bin)")
    p.add_argument(
        "--load-addr",
        required=True,
        help="kseg0 load address (e.g. 0x8007AA38)",
    )
    args = p.parse_args()
    load_addr = int(args.load_addr, 0)
    if not (0x80000000 <= load_addr < 0xA0000000):
        print(f"error: --load-addr must be in KSEG0", file=sys.stderr)
        return 1
    synth(args.input, args.output, load_addr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
