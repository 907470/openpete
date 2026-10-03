"""
.psxtrace - per-frame PSX gameplay-state oracle capture (v2).

The trace is a stream of FNV-1a-64 hashes (one per gameplay-state region as
defined by games/<game>/manifest/gameplay_state_regions.toml) computed from PCSX-Redux,
the canonical PSX hardware oracle. openpete computes the same hashes via
core/runtime/run.c::gameplay_hash_compute_ex and value-equality-compares
them at every frame; first divergence = first leak. Hash records are tiny
(~150 B/frame) so an 8000-frame trace fits comfortably in the repo
without LFS. RAM payloads are *optional* (gated by the `has-ram` header
flag) and only used for forensic byte-attribution when a hash mismatches.

Layout (little-endian throughout):

  File header (≥40 bytes; followed by a names blob of `names_total_len`
  bytes):

     0   8   magic            b"PSXTRACE"
     8   4   version          u32 = 2
    12   4   frame_count      u32   (0 = streaming/unknown; finalised by closer)
    16   4   ram_size         u32 = 0x200000
    20   4   flags            u32   (bit 0: ram zstd-compressed; bit 1: has-ram;
                                    bit 2: table-hash block; bit 3: scm-hash block;
                                    bit 4: record pad2 holds the VBLANK count)
    24   4   region_count     u32 = N
    28   4   names_total_len  u32 = sum of (1 + len(name)) over the N names
    32   4   reserved         u32 = 0
    36   ...                  N records of (u8 len, len bytes ASCII name)
   ...                        if (flags & FLAG_TABLE_HASH): u64 FNV-1a-64 of the
                                  regions file the capture hashed from
                                  (gen_state_regions.py --out-flat)
   ...                        if (flags & FLAG_SCM_HASH): u8 len, len bytes ASCII
                                  git hash of the capturing DuckStation build

  Per-frame record (variable):

     0   4   record_magic     u32 = 'FRMR' (0x524D5246 LE)
     4   4   frame_no         u32
     8   4   pad1             u32
    12   4   pad2             u32
    16   4   pc               u32
    20   8   total_hash       u64
    28   N×8  per_region_hash u64 × region_count
   ...                        if (flags & FLAG_HAS_RAM):
                                  4 bytes ram_payload_len
                                  ram_payload_len bytes (zstd if FLAG_ZSTD)

The frame stream ends at EOF.

GTE hash sidecar `<trace>.gte` (text; `#` lines are comments, the last one
`# rows=R ops=K rom_ops=B records=N`). Row k covers the GTE instructions
executed after record k's anchor hit up to record k+1's:

  rec ops hash written residue vbl

  rec      record index k
  ops      GTE instructions folded
  hash     hex u64: FNV-1a-64 over 32-bit words (h ^= w; h *= prime) of
           each instruction word followed by the 64 registers' read view
           after it (data 0-31, control 0-31; 0 for an unwritten register)
  written  hex u64: bit i set once the game has changed register i's read
           view (observed at instruction boundaries)
  residue  hex u64: the same fold over (i, value) of every unwritten register
  vbl      VBLANK count at record k (the writer's own clock)

GTE dump `<trace>.gtedump` (binary, little-endian, 532-byte rows):
  u32 rec, u32 pc, u32 instr, u32 written_lo, u32 written_hi,
  u32 pre[64], u32 post[64]   (read view, unmasked)

V1 (raw 2 MB only, no hashes) was retired before any baseline shipped -
no migration path needed.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import BinaryIO, Iterator, List, Optional

MAGIC = b"PSXTRACE"
VERSION = 2
RAM_SIZE = 0x200000
RECORD_MAGIC = 0x524D5246  # 'FRMR'

HEADER_FMT = "<8sIIIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)

FLAG_ZSTD       = 1 << 0
FLAG_HAS_RAM    = 1 << 1
FLAG_TABLE_HASH = 1 << 2
FLAG_SCM_HASH   = 1 << 3
FLAG_VBL_STAMP  = 1 << 4   # record pad2 = capturing side's VBLANK count at the hit


@dataclass(frozen=True)
class TraceHeader:
    version: int
    frame_count: int
    ram_size: int
    flags: int
    region_count: int
    region_names: List[str] = field(default_factory=list)
    table_hash: Optional[int] = None
    scm_hash: Optional[str] = None

    @property
    def zstd(self) -> bool:
        return bool(self.flags & FLAG_ZSTD)

    @property
    def has_ram(self) -> bool:
        return bool(self.flags & FLAG_HAS_RAM)


@dataclass
class FrameRecord:
    frame_no: int
    pad1: int
    pad2: int
    pc: int
    total_hash: int
    region_hashes: List[int]
    ram: Optional[bytes] = None  # decompressed 2 MiB on read (if has_ram)


def _names_blob(names: List[str]) -> bytes:
    out = bytearray()
    for n in names:
        b = n.encode("ascii", errors="replace")
        if len(b) > 255:
            b = b[:255]
        out.append(len(b))
        out.extend(b)
    return bytes(out)


def _parse_names(blob: bytes, count: int) -> List[str]:
    names: List[str] = []
    i = 0
    for _ in range(count):
        if i >= len(blob):
            raise ValueError("psxtrace: truncated names blob")
        n = blob[i]
        i += 1
        if i + n > len(blob):
            raise ValueError("psxtrace: name spills past blob end")
        names.append(blob[i:i + n].decode("ascii", errors="replace"))
        i += n
    return names


def write_header(fp: BinaryIO,
                 region_names: List[str],
                 frame_count: int = 0,
                 flags: int = 0,
                 table_hash: Optional[int] = None,
                 scm_hash: Optional[str] = None) -> None:
    if table_hash is not None:
        flags |= FLAG_TABLE_HASH
    if scm_hash is not None:
        flags |= FLAG_SCM_HASH
    blob = _names_blob(region_names)
    fp.write(struct.pack(HEADER_FMT,
                         MAGIC, VERSION, frame_count,
                         RAM_SIZE, flags, len(region_names),
                         len(blob), 0))
    fp.write(blob)
    if flags & FLAG_TABLE_HASH:
        fp.write(struct.pack("<Q", table_hash or 0))
    if flags & FLAG_SCM_HASH:
        b = (scm_hash or "").encode("ascii")[:255]
        fp.write(bytes([len(b)]) + b)


def read_header(fp: BinaryIO) -> TraceHeader:
    raw = fp.read(HEADER_SIZE)
    if len(raw) != HEADER_SIZE:
        raise ValueError("psxtrace: short header")
    (magic, version, frame_count, ram_size, flags,
     region_count, names_len, _) = struct.unpack(HEADER_FMT, raw)
    if magic != MAGIC:
        raise ValueError(f"psxtrace: bad magic {magic!r}")
    if version != VERSION:
        raise ValueError(f"psxtrace: unsupported version {version}")
    if ram_size != RAM_SIZE:
        raise ValueError(f"psxtrace: unexpected ram_size 0x{ram_size:x}")
    blob = fp.read(names_len)
    if len(blob) != names_len:
        raise ValueError("psxtrace: truncated names blob")
    names = _parse_names(blob, region_count)
    table_hash = None
    scm_hash = None
    if flags & FLAG_TABLE_HASH:
        raw = fp.read(8)
        if len(raw) != 8:
            raise ValueError("psxtrace: truncated table-hash block")
        table_hash = struct.unpack("<Q", raw)[0]
    if flags & FLAG_SCM_HASH:
        n = fp.read(1)
        if len(n) != 1:
            raise ValueError("psxtrace: truncated scm-hash block")
        raw = fp.read(n[0])
        if len(raw) != n[0]:
            raise ValueError("psxtrace: truncated scm-hash block")
        scm_hash = raw.decode("ascii", errors="replace")
    return TraceHeader(version, frame_count, ram_size, flags,
                       region_count, names, table_hash, scm_hash)


def write_record(fp: BinaryIO,
                 rec: FrameRecord,
                 region_count: int,
                 has_ram: bool,
                 zstd: bool = False) -> None:
    if len(rec.region_hashes) != region_count:
        raise ValueError(
            f"region_hashes length {len(rec.region_hashes)} != header {region_count}")
    fp.write(struct.pack("<IIIIIQ",
                         RECORD_MAGIC, rec.frame_no,
                         rec.pad1, rec.pad2, rec.pc, rec.total_hash))
    fp.write(struct.pack(f"<{region_count}Q", *rec.region_hashes))
    if has_ram:
        if rec.ram is None:
            raise ValueError("has_ram set but record has no RAM payload")
        if len(rec.ram) != RAM_SIZE:
            raise ValueError(f"ram payload must be {RAM_SIZE} bytes")
        payload = rec.ram
        if zstd:
            import zstandard as Z
            cctx = Z.ZstdCompressor(level=3)
            payload = cctx.compress(payload)
        fp.write(struct.pack("<I", len(payload)))
        fp.write(payload)


def read_records(fp: BinaryIO,
                 header: TraceHeader) -> Iterator[FrameRecord]:
    decompress = None
    if header.zstd:
        import zstandard as Z
        dctx = Z.ZstdDecompressor()
        decompress = lambda b: dctx.decompress(b, max_output_size=RAM_SIZE)
    rec_fixed_fmt = "<IIIIIQ"
    rec_fixed_size = struct.calcsize(rec_fixed_fmt)
    hashes_fmt = f"<{header.region_count}Q"
    hashes_size = struct.calcsize(hashes_fmt)
    while True:
        head = fp.read(rec_fixed_size)
        if not head:
            return
        if len(head) != rec_fixed_size:
            raise ValueError("psxtrace: truncated record header")
        (magic, frame_no, pad1, pad2, pc, total) = struct.unpack(
            rec_fixed_fmt, head)
        if magic != RECORD_MAGIC:
            raise ValueError(f"psxtrace: bad record magic 0x{magic:08x}")
        hash_blob = fp.read(hashes_size)
        if len(hash_blob) != hashes_size:
            raise ValueError("psxtrace: truncated hash array")
        per = list(struct.unpack(hashes_fmt, hash_blob))
        ram = None
        if header.has_ram:
            len_buf = fp.read(4)
            if len(len_buf) != 4:
                raise ValueError("psxtrace: truncated ram-len")
            (n,) = struct.unpack("<I", len_buf)
            payload = fp.read(n)
            if len(payload) != n:
                raise ValueError("psxtrace: truncated ram payload")
            ram = decompress(payload) if decompress else payload
            if len(ram) != RAM_SIZE:
                raise ValueError(
                    f"psxtrace: decoded RAM is {len(ram)}, expected {RAM_SIZE}")
        yield FrameRecord(frame_no, pad1, pad2, pc, total, per, ram)


def finalise_frame_count(path: str, count: int) -> None:
    """Patch frame_count field in an already-closed trace."""
    with open(path, "r+b") as fp:
        fp.seek(12)  # field offset of frame_count
        fp.write(struct.pack("<I", count))


@dataclass
class GteRow:
    rec: int
    ops: int
    hash: int
    written: int
    residue: int
    vbl: int


def read_gte(path: str) -> List[GteRow]:
    """Rows of a `.gte` sidecar (the capture's or openpete's
    OPENPETE_GTE_HASH_OUT), in file order."""
    rows: List[GteRow] = []
    with open(path) as fp:
        for line in fp:
            if line.startswith("#") or not line.strip():
                continue
            f = line.split()
            if len(f) != 6:
                raise ValueError(f"gte: bad row {line!r}")
            rows.append(GteRow(int(f[0]), int(f[1]), int(f[2], 16),
                               int(f[3], 16), int(f[4], 16), int(f[5])))
    return rows


GTE_DUMP_ROW = struct.Struct("<5I64I64I")


@dataclass
class GteOp:
    rec: int
    pc: int
    instr: int
    written: int
    pre: tuple
    post: tuple


def read_gtedump(path: str) -> Iterator[GteOp]:
    """Rows of a `.gtedump` (capture's -psxtrace-gte-dump or openpete's
    OPENPETE_GTE_HASH_DUMP)."""
    with open(path, "rb") as fp:
        while True:
            b = fp.read(GTE_DUMP_ROW.size)
            if not b:
                return
            if len(b) != GTE_DUMP_ROW.size:
                raise ValueError("gtedump: truncated row")
            v = GTE_DUMP_ROW.unpack(b)
            yield GteOp(v[0], v[1], v[2], v[3] | (v[4] << 32),
                        v[5:69], v[69:133])


GTE_REG_NAMES = (
    "VXY0 VZ0 VXY1 VZ1 VXY2 VZ2 RGBC OTZ IR0 IR1 IR2 IR3 SXY0 SXY1 SXY2 SXYP "
    "SZ0 SZ1 SZ2 SZ3 RGB0 RGB1 RGB2 RES1 MAC0 MAC1 MAC2 MAC3 IRGB ORGB LZCS LZCR "
    "RT11RT12 RT13RT21 RT22RT23 RT31RT32 RT33 TRX TRY TRZ L11L12 L13L21 L22L23 "
    "L31L32 L33 RBK GBK BBK LR1LR2 LR3LG1 LG2LG3 LB1LB2 LB3 RFC GFC BFC OFX OFY "
    "H DQA DQB ZSF3 ZSF4 FLAG").split()


def gte_dump_diff(ref_path: str, op_path: str) -> int:
    """Print the first op whose instruction, pre view or post view differs
    between two dumps, register by register. Returns 1 on a difference."""
    for i, (a, b) in enumerate(zip(read_gtedump(ref_path), read_gtedump(op_path))):
        if a.instr != b.instr or a.pre != b.pre or a.post != b.post:
            print(f"first difference at op {i}: rec={a.rec} pc ref=0x{a.pc:08x} "
                  f"op=0x{b.pc:08x} instr ref=0x{a.instr:08x} op=0x{b.instr:08x}")
            for side, x, y in (("pre", a.pre, b.pre), ("post", a.post, b.post)):
                for r in range(64):
                    if x[r] != y[r]:
                        print(f"  {side} {r:2d} {GTE_REG_NAMES[r]:9s} ref=0x{x[r]:08x} op=0x{y[r]:08x}")
            return 1
    print("no difference over the common rows")
    return 0


def main_cli(argv: Optional[list] = None) -> int:
    import argparse
    p = argparse.ArgumentParser(description="psxtrace inspector")
    sub = p.add_subparsers(dest="cmd", required=True)
    p_info = sub.add_parser("info", help="print header + record summary")
    p_info.add_argument("path")
    p_info.add_argument("--show-frames", type=int, default=0,
                        help="print first N records (default 0)")
    p_gte = sub.add_parser("gte", help="summarise a .gte sidecar, or compare two")
    p_gte.add_argument("path")
    p_gte.add_argument("other", nargs="?")
    p_gd = sub.add_parser("gtedump-diff", help="first differing op of two .gtedump files")
    p_gd.add_argument("ref")
    p_gd.add_argument("op")
    args = p.parse_args(argv)
    if args.cmd == "gtedump-diff":
        return gte_dump_diff(args.ref, args.op)
    if args.cmd == "gte":
        a = read_gte(args.path)
        print(f"rows={len(a)} ops={sum(r.ops for r in a)}")
        if args.other:
            b = {r.rec: r for r in read_gte(args.other)}
            common = [r for r in a if r.rec in b]
            diff = [r for r in common if (r.ops, r.hash, r.written) != (b[r.rec].ops, b[r.rec].hash, b[r.rec].written)]
            dops = sum(1 for r in common if r.ops != b[r.rec].ops)
            print(f"common={len(common)} differing={len(diff)} ops_differing={dops} "
                  f"first={diff[0].rec if diff else '-'}")
            return 1 if diff else 0
        return 0
    if args.cmd == "info":
        with open(args.path, "rb") as fp:
            hdr = read_header(fp)
            print(f"version={hdr.version} frames={hdr.frame_count} "
                  f"ram=0x{hdr.ram_size:x} flags=0x{hdr.flags:x} "
                  f"zstd={hdr.zstd} has_ram={hdr.has_ram} "
                  f"regions={hdr.region_count}")
            if hdr.table_hash is not None:
                print(f"table_hash=0x{hdr.table_hash:016X}")
            if hdr.scm_hash is not None:
                print(f"scm_hash={hdr.scm_hash}")
            for i, n in enumerate(hdr.region_names):
                print(f"  region[{i}] = {n}")
            n_seen = 0
            shown = 0
            last_frame = None
            for rec in read_records(fp, hdr):
                if shown < args.show_frames:
                    print(f"  frame={rec.frame_no:5d} pc=0x{rec.pc:08x} "
                          f"total=0x{rec.total_hash:016x}")
                    shown += 1
                last_frame = rec.frame_no
                n_seen += 1
            print(f"records_seen={n_seen} last_frame={last_frame}")
        return 0
    p.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main_cli())
