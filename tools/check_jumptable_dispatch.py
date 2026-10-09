#!/usr/bin/env python3
"""Fail on a word-table `jr` the recompiler left without an index switch.

A compiler-emitted jump table dispatches as

    sltiu v0, idx, N        # the bound; the guard branch tests v0
    beq   v0, zero, default
    sll   v0, idx, 2
    lui   at, hi
    addu  at, at, v0
    lw    v0, lo(at)
    jr    v0

psxrecomp turns it into `switch (idx)` with one case per entry, each a
`goto` or a direct call of the entry's function. When it does not, the
emitted C is `call_by_address(cpu, cpu->v0); return;` and every entry
must also be a dispatch entry: an entry that is not one stub-misses, the
C function returns without running the guest epilogue, and the guest
stack pointer descends by the frame size.

This check reads every `*_full.c` of a game's generated tree, finds each
`call_by_address(cpu, cpu->R); /* jr cpu->R */` whose register was loaded
by `lw R, off(B)` with B built by `lui B` (+ `addiu B, B`) and an `addu`,
and composes the table address. A site whose table lies inside the
binary's image fails unless recomp.conf names it in JUMPTABLE_UNSWITCHED
(`<binary>:<lw address>` words); a named site that no longer exists also
fails, so the list cannot go stale. Sites whose table lies outside the
image are listed and do not fail: the bytes are not the recompiler's to
read.

Usage:
    check_jumptable_dispatch.py --game <game> [--generated-dir DIR]

Exit 0 PASS, 1 FAIL, 64 SKIP (generated tree or binaries missing).
"""
import argparse
import re
import struct
import sys
from pathlib import Path

JR = re.compile(r'call_by_address\(cpu, cpu->(\w+)\);\s+/\* jr cpu->(\w+) \*/')
LW = re.compile(r'cpu->(\w+) = cpu->read_word\(cpu->(\w+) \+ (-?\d+)\);'
                r'\s+/\* 0x([0-9A-F]{8}):')
ADDU = re.compile(r'cpu->(\w+) = cpu->(\w+) \+ cpu->(\w+);\s+/\* 0x[0-9A-F]{8}: ')
ADDIU = re.compile(r'cpu->(\w+) = cpu->(\w+) \+ (-?\d+);\s+/\* 0x[0-9A-F]{8}: ')
LUI = re.compile(r'cpu->(\w+) = 0x([0-9A-F]+) << 16;')
# Lines walked back from the `jr` for the lw/addu/addiu/lui chain: the
# sequence occupies the `jr` block, plus nops and delay-slot markers.
WINDOW = 12


def read_conf(path):
    """Parse the `NAME=value` assignments of a recomp.conf, quotes stripped."""
    conf = {}
    for line in path.read_text().splitlines():
        m = re.match(r'^([A-Z_][A-Z0-9_]*)=(.*)$', line.strip())
        if m:
            conf[m.group(1)] = m.group(2).strip().strip('"')
    return conf


def resolve(value, conf):
    """Expand `$NAME` references to earlier recomp.conf assignments."""
    return re.sub(r'\$(\w+)', lambda m: conf.get(m.group(1), ''), value)


def exe_image(path):
    """Return (load address, text bytes) of a PS-X EXE."""
    data = path.read_bytes()
    load, size = struct.unpack_from('<II', data, 0x18)
    return load, size


def table_sites(full_c):
    """Yield (lw address, table address) of each unswitched word-table `jr`."""
    lines = full_c.read_text(errors='replace').split('\n')
    for i, line in enumerate(lines):
        m = JR.search(line)
        if not m or m.group(1) == 'ra':
            continue
        reg = m.group(1)
        lo = max(i - WINDOW, 0)
        for j in range(i - 1, lo, -1):
            lw = LW.search(lines[j])
            if not lw or lw.group(1) != reg:
                continue
            base_reg, off, lw_addr = lw.group(2), int(lw.group(3)), int(lw.group(4), 16)
            found_addu, addiu, lui = False, 0, None
            for k in range(j - 1, lo, -1):
                text = lines[k]
                a = ADDU.search(text)
                if not found_addu and a and a.group(1) == base_reg and \
                        base_reg in (a.group(2), a.group(3)):
                    found_addu = True
                    continue
                a = ADDIU.search(text)
                if found_addu and a and a.group(1) == base_reg and a.group(2) == base_reg:
                    addiu += int(a.group(3))
                    continue
                u = LUI.search(text)
                if found_addu and u and u.group(1) == base_reg:
                    lui = int(u.group(2), 16) << 16
                    break
            if found_addu and lui is not None:
                yield lw_addr, (lui + addiu + off) & 0xFFFFFFFF
            break


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', required=True)
    ap.add_argument('--generated-dir', type=Path)
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    conf = read_conf(root / 'games' / args.game / 'recomp.conf')
    gen = args.generated_dir or root / 'games' / args.game / 'generated'
    psx_exe = root / resolve(conf['PSX_EXE'], conf)
    registry = gen / 'overlay_registry.txt'
    if not (gen / 'PSX.EXE_full.c').exists() or not psx_exe.exists() or not registry.exists():
        print(f'[jumptable-dispatch] {args.game}: SKIP (generated tree or {psx_exe} missing)')
        return 64

    load = int(conf['OVERLAY_LOAD_ADDR'], 16)
    binaries = [('main_exe', gen / 'PSX.EXE_full.c', exe_image(psx_exe))]
    for row in registry.read_text().split('\n'):
        f = row.split()
        if len(f) != 3:
            continue
        blob = root / f[2]
        full_c = gen / f[1] / f'{f[1]}.bin_full.c'
        if not blob.exists() or not full_c.exists():
            print(f'[jumptable-dispatch] {args.game}: SKIP ({blob} or {full_c} missing)')
            return 64
        binaries.append((f[1], full_c, (load, blob.stat().st_size)))

    carved = set(conf.get('JUMPTABLE_UNSWITCHED', '').split())
    seen, fails, outside = set(), [], []
    for name, full_c, (lo, size) in binaries:
        for lw_addr, table in table_sites(full_c):
            key = f'{name}:0x{lw_addr:08X}'
            if not lo <= table < lo + size:
                outside.append(f'{key} table 0x{table:08X}')
                continue
            seen.add(key)
            if key not in carved:
                fails.append(f'{key} table 0x{table:08X}: no index switch emitted')
    for key in sorted(carved - seen):
        fails.append(f'{key}: named in JUMPTABLE_UNSWITCHED but no such unswitched site')

    print(f'[jumptable-dispatch] {args.game}: {len(binaries)} binaries, '
          f'{len(seen)} unswitched in-image table sites ({len(seen & carved)} carved out), '
          f'{len(outside)} outside the image, {len(fails)} failures')
    for o in outside:
        print(f'  outside image: {o}')
    for f in fails:
        print(f'  FAIL {f}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
