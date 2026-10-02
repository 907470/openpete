#!/usr/bin/env python3
"""Decode the hang watchdog's dispatch trace.

The watchdog in core/psx/dispatch.c writes hang-trace.bin when a vbl
interval runs past OPENPETE_HANG_THRESHOLD dispatch entries. The file holds
three rings of (addr, ra, sp):

  interval   the first entries of the interval that tripped - the path INTO
             whatever is spinning
  previous   the same ring as it stood at the last vbl boundary, which
             survives a loop that fills the interval ring with its own body
  recent     the most recent entries, written on every dispatch - the loop
             itself, and the only ring that shows a loop entered late

For each ring this prints the most frequent addresses and, for `recent`, the
last entries in order. A tight cycle appears as a short repeating sequence
whose `ra` values all land inside one function: that function holds the loop.

Pass the decomp's psx.map to resolve addresses to `func_<addr>+<offset>`.

Usage:
    decode_hang_trace.py hang-trace.bin [psx.map] [tail-count]
"""
import collections
import os
import re
import struct
import sys

MAGIC = 0x32474E48  # 'HNG2'
RING_NAMES = ('interval', 'previous', 'recent')


def load(path):
    data = open(path, 'rb').read()
    magic, = struct.unpack_from('<I', data, 0)
    if magic != MAGIC:
        raise SystemExit(f'{path}: magic 0x{magic:08X}, expected 0x{MAGIC:08X}')
    off, rings = 4, []
    for _ in RING_NAMES:
        count, head = struct.unpack_from('<II', data, off)
        off += 8
        raw = struct.unpack_from(f'<{count * 3}I', data, off)
        off += count * 12
        ent = [(raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]) for i in range(count)]
        rings.append(ent[head:] + ent[:head])   # oldest first
    return rings


def symtab(map_path):
    syms = []
    if not map_path or not os.path.exists(map_path):
        return syms
    for line in open(map_path, errors='replace'):
        m = re.match(r'\s+0x0*([0-9a-fA-F]{8})\s+(\S+)\s*$', line)
        if not m or m.group(2).endswith('.NON_MATCHING'):
            continue
        addr = int(m.group(1), 16)
        if 0x80000000 <= addr < 0x80200000:
            syms.append((addr, m.group(2)))
    return sorted(set(syms))


def name(syms, addr):
    lo, hi, best = 0, len(syms) - 1, ''
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            delta = addr - syms[mid][0]
            best = syms[mid][1] if delta == 0 else f'{syms[mid][1]}+0x{delta:x}'
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def show(label, entries, syms, tail):
    entries = [e for e in entries if e[0]]
    print(f'=== {label}: {len(entries)} entries')
    for addr, n in collections.Counter(a for a, _, _ in entries).most_common(12):
        print(f'   0x{addr:08X}  x{n:<6d} {name(syms, addr)}')
    if tail:
        print(f'-- last {tail}, in order:')
        for addr, ra, sp in entries[-tail:]:
            print(f'   addr=0x{addr:08X} ra=0x{ra:08X} sp=0x{sp:08X}  '
                  f'{name(syms, addr)}')


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    syms = symtab(argv[2] if len(argv) > 2 else None)
    tail = int(argv[3]) if len(argv) > 3 else 40
    for label, ring in zip(RING_NAMES, load(argv[1])):
        show(label, ring, syms, tail if label == 'recent' else 0)
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
