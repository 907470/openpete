"""
.psxtrace — per-frame PSX RAM oracle capture.

Format (little-endian throughout):

  File header (32 bytes):
    0   8   magic            b"PSXTRACE"
    8   4   version          u32 = 1
   12   4   frame_count      u32   (0 = streaming/unknown; finalised by closer)
   16   4   ram_size         u32 = 0x200000 (2 MiB)
   20   4   flags            u32   (bit 0: per-record RAM is zstd-compressed)
   24   4   record_size_hint u32   (informational; 0 if variable)
   28   4   reserved         u32

  Per-frame record (variable):
     0   4   record_magic     u32 = 'FRMR' (0x524D5246 LE)
     4   4   frame_no         u32
     8   4   pad1             u32   (port 1 controller bitmask, PSX-native)
    12   4   pad2             u32   (port 2 controller bitmask, PSX-native)
    16   4   pc               u32   (CPU pc at sample instant, sanity)
    20   4   ram_payload_len  u32   (bytes that follow)
    24   N   ram_payload      raw 2 MiB if flag bit 0 clear, else zstd frame.

The frame stream ends at EOF. `frame_count` in the header is updated by the
finaliser if the writer closes cleanly; readers must tolerate `0` and just
iterate to EOF.

Companion to docs/ram-diff-vs-pcsxr.md — this is the streaming variant of the
single-shot RAM dump used for Phase 2 oracle verification (see
notes/plans/openpete-determinism-enforcement.md).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import BinaryIO, Iterator, Optional

MAGIC = b"PSXTRACE"
VERSION = 1
RAM_SIZE = 0x200000
RECORD_MAGIC = 0x524D5246  # 'FRMR'

HEADER_FMT = "<8sIIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
RECORD_HEADER_FMT = "<IIIIII"
RECORD_HEADER_SIZE = struct.calcsize(RECORD_HEADER_FMT)

FLAG_ZSTD = 1 << 0


@dataclass(frozen=True)
class TraceHeader:
    version: int
    frame_count: int
    ram_size: int
    flags: int
    record_size_hint: int

    @property
    def zstd(self) -> bool:
        return bool(self.flags & FLAG_ZSTD)


@dataclass
class FrameRecord:
    frame_no: int
    pad1: int
    pad2: int
    pc: int
    ram: bytes  # always decompressed 2 MiB on read


def write_header(fp: BinaryIO,
                 frame_count: int = 0,
                 flags: int = 0,
                 record_size_hint: int = 0) -> None:
    fp.write(struct.pack(HEADER_FMT,
                         MAGIC, VERSION, frame_count,
                         RAM_SIZE, flags, record_size_hint, 0))


def read_header(fp: BinaryIO) -> TraceHeader:
    blob = fp.read(HEADER_SIZE)
    if len(blob) != HEADER_SIZE:
        raise ValueError("psxtrace: short header")
    magic, version, frame_count, ram_size, flags, hint, _ = struct.unpack(
        HEADER_FMT, blob)
    if magic != MAGIC:
        raise ValueError(f"psxtrace: bad magic {magic!r}")
    if version != VERSION:
        raise ValueError(f"psxtrace: unsupported version {version}")
    if ram_size != RAM_SIZE:
        raise ValueError(f"psxtrace: unexpected ram_size 0x{ram_size:x}")
    return TraceHeader(version, frame_count, ram_size, flags, hint)


def write_record(fp: BinaryIO,
                 rec: FrameRecord,
                 compress: bool = False) -> None:
    payload = rec.ram
    if len(payload) != RAM_SIZE:
        raise ValueError(f"psxtrace: ram payload must be {RAM_SIZE} bytes")
    if compress:
        import zstandard as zstd
        cctx = zstd.ZstdCompressor(level=3)
        payload = cctx.compress(payload)
    fp.write(struct.pack(RECORD_HEADER_FMT,
                         RECORD_MAGIC, rec.frame_no,
                         rec.pad1, rec.pad2, rec.pc, len(payload)))
    fp.write(payload)


def read_records(fp: BinaryIO, header: TraceHeader) -> Iterator[FrameRecord]:
    decompress = None
    if header.zstd:
        import zstandard as zstd
        dctx = zstd.ZstdDecompressor()
        decompress = lambda b: dctx.decompress(b, max_output_size=RAM_SIZE)
    while True:
        hdr = fp.read(RECORD_HEADER_SIZE)
        if not hdr:
            return
        if len(hdr) != RECORD_HEADER_SIZE:
            raise ValueError("psxtrace: truncated record header")
        magic, frame_no, pad1, pad2, pc, n = struct.unpack(
            RECORD_HEADER_FMT, hdr)
        if magic != RECORD_MAGIC:
            raise ValueError(f"psxtrace: bad record magic 0x{magic:08x}")
        payload = fp.read(n)
        if len(payload) != n:
            raise ValueError("psxtrace: truncated record payload")
        ram = decompress(payload) if decompress else payload
        if len(ram) != RAM_SIZE:
            raise ValueError(
                f"psxtrace: decoded RAM is {len(ram)}, expected {RAM_SIZE}")
        yield FrameRecord(frame_no, pad1, pad2, pc, ram)


def finalise_frame_count(path: str, count: int) -> None:
    """Patch frame_count field in an already-closed trace."""
    with open(path, "r+b") as fp:
        fp.seek(12)  # field offset of frame_count
        fp.write(struct.pack("<I", count))


def recompress_to_zstd(src_path: str, dst_path: str) -> int:
    """Convert a raw-RAM .psxtrace into a zstd-compressed one. Returns count."""
    import zstandard as zstd
    cctx = zstd.ZstdCompressor(level=3)
    n = 0
    with open(src_path, "rb") as fin, open(dst_path, "wb") as fout:
        hdr = read_header(fin)
        if hdr.zstd:
            raise ValueError("source already zstd-compressed")
        write_header(fout, frame_count=hdr.frame_count,
                     flags=FLAG_ZSTD, record_size_hint=0)
        for rec in read_records(fin, hdr):
            comp = cctx.compress(rec.ram)
            fout.write(struct.pack(RECORD_HEADER_FMT,
                                   RECORD_MAGIC, rec.frame_no,
                                   rec.pad1, rec.pad2, rec.pc, len(comp)))
            fout.write(comp)
            n += 1
    if hdr.frame_count == 0:
        finalise_frame_count(dst_path, n)
    return n


def main_cli(argv: Optional[list] = None) -> int:
    import argparse
    import sys
    p = argparse.ArgumentParser(description="psxtrace inspector")
    sub = p.add_subparsers(dest="cmd", required=True)

    p_info = sub.add_parser("info", help="print header + record summary")
    p_info.add_argument("path")

    p_comp = sub.add_parser("compress",
                            help="recompress raw trace to zstd in place")
    p_comp.add_argument("src")
    p_comp.add_argument("dst")

    args = p.parse_args(argv)
    if args.cmd == "info":
        with open(args.path, "rb") as fp:
            hdr = read_header(fp)
            print(f"version={hdr.version} frames={hdr.frame_count} "
                  f"ram=0x{hdr.ram_size:x} flags=0x{hdr.flags:x} "
                  f"zstd={hdr.zstd}")
            n = 0
            first_pc = None
            last_frame = None
            for rec in read_records(fp, hdr):
                if first_pc is None:
                    first_pc = rec.pc
                last_frame = rec.frame_no
                n += 1
            print(f"records_seen={n} first_pc=0x{(first_pc or 0):08x} "
                  f"last_frame={last_frame}")
        return 0
    if args.cmd == "compress":
        n = recompress_to_zstd(args.src, args.dst)
        print(f"wrote {n} compressed records to {args.dst}")
        return 0
    p.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main_cli())
