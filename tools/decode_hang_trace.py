#!/usr/bin/env python3
"""Decode the hang watchdog's dispatch trace.

The watchdog in core/psx/dispatch.c writes hang-trace.bin when a vbl
interval runs past OPENPETE_HANG_THRESHOLD dispatch entries, a call-free
loop past its quanta, or the stall monitor's limit. The file holds three
rings of (addr, ra, sp):

  interval   the first entries of the interval that tripped - the path INTO
             whatever is spinning
  previous   the same ring as it stood at the last vbl boundary, which
             survives a loop that fills the interval ring with its own body
  recent     the most recent entries, written on every dispatch - the loop
             itself, and the only ring that shows a loop entered late

An 'HNG3' file follows the rings with the trip block: the trip address, the
guest registers at the trip, the resident overlay's id and name, and the
count and first addresses of the guest accesses that reached no device.
'HNG2' files end after the rings.

For each ring this prints the most frequent addresses and, for `recent`, the
last entries in order. A tight cycle appears as a short repeating sequence
whose `ra` values all land inside one function: that function holds the loop.

Pass the decomp's psx.map to resolve addresses to `func_<addr>+<offset>`.
Every overlay of a game links at the same address, so a symbol is taken
from the executable's sections and from the resident overlay's section
only: the trip block's overlay name, or --overlay NAME for an 'HNG2' file
(the name the log's "active overlay:" line gives, which is the map's
section name). With neither, an address inside an overlay section resolves
to no symbol.

Usage:
    decode_hang_trace.py hang-trace.bin [psx.map] [tail-count] [--overlay NAME]
"""
import collections
import os
import re
import struct
import sys

MAGIC2 = 0x32474E48  # 'HNG2'
MAGIC3 = 0x33474E48  # 'HNG3'
RING_NAMES = ('interval', 'previous', 'recent')
REG_NAMES = ('zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
             't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
             's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
             't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra', 'pc', 'hi', 'lo')
OVERLAY_NAME_BYTES = 32
UNMAPPED_KEEP = 8
# Sections of the executable itself; every other allocated section of the
# link map is an overlay that shares its load address with the others.
EXE_SECTIONS = ('main', 'main_bss', 'header')


def load(path):
    """Return (rings, trip) where trip is None for an 'HNG2' file."""
    data = open(path, 'rb').read()
    magic, = struct.unpack_from('<I', data, 0)
    if magic not in (MAGIC2, MAGIC3):
        raise SystemExit(f'{path}: magic 0x{magic:08X}, expected 0x{MAGIC2:08X} or 0x{MAGIC3:08X}')
    off, rings = 4, []
    for _ in RING_NAMES:
        count, head = struct.unpack_from('<II', data, off)
        off += 8
        raw = struct.unpack_from(f'<{count * 3}I', data, off)
        off += count * 12
        ent = [(raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]) for i in range(count)]
        rings.append(ent[head:] + ent[:head])   # oldest first
    if magic == MAGIC2:
        return rings, None
    trip_addr, have = struct.unpack_from('<II', data, off)
    off += 8
    regs = struct.unpack_from('<35I', data, off)
    off += 35 * 4
    ovl_id, = struct.unpack_from('<I', data, off)
    off += 4
    ovl_name = data[off:off + OVERLAY_NAME_BYTES].split(b'\0', 1)[0].decode('ascii', 'replace')
    off += OVERLAY_NAME_BYTES
    n_unmapped, kept = struct.unpack_from('<QI', data, off)
    off += 12
    unmapped = []
    for i in range(min(kept, UNMAPPED_KEEP)):
        addr, func, width, write = struct.unpack_from('<IIBB', data, off + i * 12)
        unmapped.append((addr, func, width, write))
    trip = {'addr': trip_addr, 'regs': regs if have else None, 'overlay_id': ovl_id,
            'overlay': ovl_name, 'unmapped_n': n_unmapped, 'unmapped': unmapped}
    return rings, trip


def symtab(map_path, overlay):
    """Function symbols of the executable's sections and of `overlay`'s section."""
    syms = []
    if not map_path or not os.path.exists(map_path):
        return syms
    keep = set(EXE_SECTIONS) | ({overlay} if overlay else set())
    section = None
    for line in open(map_path, errors='replace'):
        sec = re.match(r'^\.([A-Za-z0-9_]+)(\s|$)', line)
        if sec:
            section = sec.group(1)
            continue
        m = re.match(r'\s+0x0*([0-9a-fA-F]{8})\s+(\S+)\s*$', line)
        if not m or m.group(2).endswith('.NON_MATCHING') or section not in keep:
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


def show_trip(trip, syms):
    print(f"=== trip at 0x{trip['addr']:08X} {name(syms, trip['addr'])}")
    print(f"   overlay: {trip['overlay'] or 'none'} (id {trip['overlay_id']})")
    regs = trip['regs']
    if regs is None:
        print('   registers: none registered')
    else:
        for i in range(0, len(REG_NAMES), 6):
            print('   ' + '  '.join(f'{REG_NAMES[j]}=0x{regs[j]:08X}'
                                     for j in range(i, min(i + 6, len(REG_NAMES)))))
        print(f"   ra resolves to {name(syms, regs[31]) or '(no symbol)'}; "
              "pc is current only in a build regenerated with PC tracking")
    print(f"   guest accesses that reached no device: {trip['unmapped_n']}")
    for addr, func, width, write in trip['unmapped']:
        print(f"   {'store' if write else 'load'} of {width} at 0x{addr:08X}, "
              f"last function entered 0x{func:08X} {name(syms, func)}")


def main(argv):
    args = list(argv[1:])
    overlay_flag = None
    if '--overlay' in args:
        i = args.index('--overlay')
        if i + 1 >= len(args):
            raise SystemExit(__doc__)
        overlay_flag = args[i + 1]
        del args[i:i + 2]
    if not args:
        raise SystemExit(__doc__)
    rings, trip = load(args[0])
    overlay = overlay_flag or (trip['overlay'] if trip else None)
    syms = symtab(args[1] if len(args) > 1 else None, overlay)
    tail = int(args[2]) if len(args) > 2 else 40
    if len(args) > 1 and not overlay:
        print('note: no overlay named; overlay addresses resolve to no symbol')
    if trip:
        show_trip(trip, syms)
    for label, ring in zip(RING_NAMES, rings):
        show(label, ring, syms, tail if label == 'recent' else 0)
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
