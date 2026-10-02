#!/usr/bin/env python3
"""psxtrace_loads.py - read the reference's <trace>.loads sidecar and place
openpete's CD reads against it.

The DuckStation capture module writes one line per CdRead when run with
-psxtrace-cd-read-pc / -psxtrace-cd-read-cb-slot:

    idx issue_rec issue_vbl issue_tick issue_line sectors dst mode
        done_rec done_vbl done_tick done_line intr

rec is the record index at the event (records written so far: 0 before
the first anchor hit), vbl DuckStation's VBLANK count, tick the CPU tick,
line the scanline (vblank begins at the display end line, 240 in NTSC).
A read the game waits on without reaching the anchor between issue and
completion is synchronous (issue_rec == done_rec); one the game polls
across anchor hits is asynchronous.

openpete's side is its console log (`CdRead sectors=... hit=H vbl=V`,
`read complete (hit=H vbl=V)`, `load delay: ... deferred D VBL(s)`) and
the OPENPETE_ANCHOR_VBL_LOG file (`hit vbl` per anchor hit).

Commands:
  table  LOADS                       print every reference read classified
  align  LOADS OPLOG                 pair reads by index, check the (sectors,
                                     dst) sequence, print hit/VBL deltas
  vbl    TRACE [OPVBL]               per-record VBL-delta histogram of the
                                     trace (pad2 stamps), and of openpete's log
  cdctl  CDCTL                       CdControl command histogram
"""
from __future__ import annotations

import collections
import os
import re
import sys
from dataclasses import dataclass
from typing import List, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psxtrace_format as P  # noqa: E402

VBLANK_LINE = 240  # NTSC vertical_display_end


@dataclass
class RefRead:
    idx: int
    issue_rec: int
    issue_vbl: int
    issue_tick: int
    issue_line: int
    sectors: int
    dst: int
    mode: int
    done_rec: Optional[int]
    done_vbl: Optional[int]
    done_tick: Optional[int]
    done_line: Optional[int]
    intr: Optional[int]
    note: str = ""

    @property
    def polling(self) -> str:
        if self.done_rec is None:
            return self.note or "open"
        if self.issue_rec == 0 and self.done_rec == 0:
            return "pre-anchor"
        return "sync" if self.issue_rec == self.done_rec else "async"


def parse_loads(path: str) -> tuple[dict, List[RefRead], List[str]]:
    header: dict = {}
    reads: List[RefRead] = []
    notes: List[str] = []
    with open(path) as fh:
        first = fh.readline().split()
        if not first or first[0] != "psxtrace-loads":
            raise ValueError(f"{path}: not a psxtrace-loads file")
        header["version"] = int(first[1])
        for kv in first[2:]:
            k, _, v = kv.partition("=")
            header[k] = v
        for line in fh:
            line = line.rstrip("\n")
            if not line:
                continue
            if line.startswith("#"):
                notes.append(line[1:].strip())
                continue
            f = line.split()
            if len(f) < 13:
                raise ValueError(f"{path}: short row: {line}")
            done = f[8] != "-"
            reads.append(RefRead(
                idx=int(f[0]), issue_rec=int(f[1]), issue_vbl=int(f[2]),
                issue_tick=int(f[3]), issue_line=int(f[4]), sectors=int(f[5]),
                dst=int(f[6], 16), mode=int(f[7], 16),
                done_rec=int(f[8]) if done else None,
                done_vbl=int(f[9]) if done else None,
                done_tick=int(f[10]) if done else None,
                done_line=int(f[11]) if done else None,
                intr=int(f[12]) if done else None,
                note="" if done else (f[13] if len(f) > 13 else "open")))
    return header, reads, notes


@dataclass
class OpRead:
    idx: int
    hit: int
    vbl: int
    sectors: int
    dst: int
    mode: int
    delay: int = 0
    done_hit: Optional[int] = None
    done_vbl: Optional[int] = None


RE_CDREAD = re.compile(r"CdRead sectors=(\d+) buf=0x([0-9A-Fa-f]+) mode=0x([0-9A-Fa-f]+) .*hit=(\d+) vbl=(-?\d+)")
RE_DEFER = re.compile(r"read deferred (\d+) VBL\(s\)")
RE_DONE = re.compile(r"read complete \(hit=(\d+) vbl=(-?\d+)\)")


def parse_oplog(path: str) -> List[OpRead]:
    reads: List[OpRead] = []
    pending: List[OpRead] = []
    with open(path, errors="replace") as fh:
        for line in fh:
            m = RE_CDREAD.search(line)
            if m:
                r = OpRead(len(reads), int(m.group(4)), int(m.group(5)), int(m.group(1)),
                           int(m.group(2), 16), int(m.group(3), 16))
                reads.append(r)
                pending.append(r)
                continue
            m = RE_DEFER.search(line)
            if m and pending:
                pending[-1].delay = int(m.group(1))
                continue
            m = RE_DONE.search(line)
            if m and pending:
                # completions fire in issue order
                r = pending.pop(0)
                r.done_hit, r.done_vbl = int(m.group(1)), int(m.group(2))
    return reads


def cmd_table(loads: str) -> int:
    header, reads, notes = parse_loads(loads)
    print(f"# {loads}: build={header.get('build')} cd_read_pc={header.get('cd_read_pc')} reads={len(reads)}")
    print(f"{'idx':>3} {'polling':<10} {'sect':>4} {'dst':<10} {'i_rec':>6} {'d_rec':>6} {'hits':>5} "
          f"{'i_vbl':>6} {'d_vbl':>6} {'vbls':>5} {'vbl/sec':>7} {'i_line':>6} {'d_line':>6} intr")
    by_mode = collections.Counter()
    for r in reads:
        by_mode[r.polling] += 1
        if r.done_rec is None:
            print(f"{r.idx:>3} {r.polling:<10} {r.sectors:>4} 0x{r.dst:08X} {r.issue_rec:>6} {'-':>6} {'-':>5} "
                  f"{r.issue_vbl:>6} {'-':>6} {'-':>5} {'-':>7} {r.issue_line:>6} {'-':>6} -")
            continue
        hits = r.done_rec - r.issue_rec
        vbls = r.done_vbl - r.issue_vbl
        print(f"{r.idx:>3} {r.polling:<10} {r.sectors:>4} 0x{r.dst:08X} {r.issue_rec:>6} {r.done_rec:>6} {hits:>5} "
              f"{r.issue_vbl:>6} {r.done_vbl:>6} {vbls:>5} {vbls / r.sectors:>7.3f} {r.issue_line:>6} {r.done_line:>6} {r.intr}")
    print("# polling modes:", dict(by_mode))
    for n in notes:
        if "handler" in n or "stray" in n or "issued=" in n:
            print("#", n)
    # completion phase: where inside the frame the reference completes
    lines = [r.done_line for r in reads if r.done_line is not None and r.polling == "async"]
    if lines:
        in_vblank = sum(1 for l in lines if l >= VBLANK_LINE)
        print(f"# async completions: {len(lines)}, {in_vblank} inside vblank (line >= {VBLANK_LINE}), "
              f"lines min={min(lines)} max={max(lines)}")
    return 0


def cmd_align(loads: str, oplog: str) -> int:
    _, ref, _ = parse_loads(loads)
    op = parse_oplog(oplog)
    n = min(len(ref), len(op))
    print(f"# reference reads={len(ref)} openpete reads={len(op)}")
    seq_ok = True
    print(f"{'idx':>3} {'polling':<10} {'sect':>4} {'dst':<10} {'ref_i':>6} {'op_i':>6} {'d_i':>5} "
          f"{'ref_hits':>8} {'op_hits':>7} {'ref_vbls':>8} {'op_vbls':>7} {'op_delay':>8} seq")
    first_shift = None
    for i in range(n):
        r, o = ref[i], op[i]
        same = (r.sectors, r.dst) == (o.sectors, o.dst)
        if not same:
            seq_ok = False
        ref_hits = (r.done_rec - r.issue_rec) if r.done_rec is not None else None
        op_hits = (o.done_hit - o.hit) if o.done_hit is not None else None
        ref_vbls = (r.done_vbl - r.issue_vbl) if r.done_vbl is not None else None
        op_vbls = (o.done_vbl - o.vbl) if o.done_vbl is not None else None
        d_i = o.hit - r.issue_rec
        if first_shift is None and d_i != 0 and r.issue_rec != 0:
            first_shift = i
        fmt = lambda v: "-" if v is None else str(v)  # noqa: E731
        print(f"{i:>3} {r.polling:<10} {r.sectors:>4} 0x{r.dst:08X} {r.issue_rec:>6} {o.hit:>6} {d_i:>5} "
              f"{fmt(ref_hits):>8} {fmt(op_hits):>7} {fmt(ref_vbls):>8} {fmt(op_vbls):>7} {o.delay:>8} "
              f"{'ok' if same else 'MISMATCH ' + str((o.sectors, hex(o.dst)))}")
    print(f"# sequence (sectors, dst) identical over the {n} paired reads: {seq_ok}; "
          f"reference {len(ref)} reads, openpete {len(op)}; first issue-hit shift at read {first_shift}")
    return 0 if seq_ok else 1


def vbl_hist(deltas: List[int], label: str) -> None:
    c = collections.Counter(deltas)
    print(f"# {label}: {len(deltas)} deltas, histogram " + ", ".join(f"{k}:{v}" for k, v in sorted(c.items())))


def cmd_vbl(trace: str, opvbl: Optional[str]) -> int:
    with open(trace, "rb") as fp:
        hdr = P.read_header(fp)
        if not hdr.flags & P.FLAG_VBL_STAMP:
            print(f"{trace}: no VBL stamps (flag bit 4 clear)", file=sys.stderr)
            return 1
        stamps = [rec.pad2 for rec in P.read_records(fp, hdr)]
    ref_d = [b - a for a, b in zip(stamps, stamps[1:])]
    vbl_hist(ref_d, f"reference {trace}")
    if opvbl:
        op = []
        with open(opvbl) as fh:
            for line in fh:
                if line.startswith("#"):
                    continue
                f = line.split()
                if len(f) == 2:
                    op.append(int(f[1]))
        op_d = [b - a for a, b in zip(op, op[1:])]
        vbl_hist(op_d, f"openpete {opvbl}")
        n = min(len(ref_d), len(op_d))
        diff = [(i + 1, ref_d[i], op_d[i]) for i in range(n) if ref_d[i] != op_d[i]]
        print(f"# per-record delta mismatches over {n} records: {len(diff)}"
              + (f"; first at record {diff[0][0]} (ref {diff[0][1]}, op {diff[0][2]})" if diff else ""))
    return 0


def cmd_cdctl(path: str) -> int:
    c = collections.Counter()
    with open(path) as fh:
        for line in fh:
            if line.startswith("#") or line.startswith("psxtrace-cdctl"):
                continue
            f = line.split()
            if len(f) >= 3:
                c[int(f[2], 16)] += 1
    names = {0x01: "Nop", 0x02: "Setloc", 0x06: "ReadN", 0x09: "Pause", 0x0E: "Setmode",
             0x10: "Getloc" + "L", 0x11: "GetlocP", 0x15: "SeekL", 0x1B: "ReadS", 0x1E: "GetTD",
             0x13: "GetTN", 0x0A: "Init", 0x03: "Play"}
    for cmd, n in sorted(c.items()):
        print(f"0x{cmd:02X} {names.get(cmd, '?'):<8} {n}")
    return 0


def main(argv: List[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "table":
        return cmd_table(argv[2])
    if cmd == "align":
        return cmd_align(argv[2], argv[3])
    if cmd == "vbl":
        return cmd_vbl(argv[2], argv[3] if len(argv) > 3 else None)
    if cmd == "cdctl":
        return cmd_cdctl(argv[2])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
