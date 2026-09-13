#!/usr/bin/env python3
"""Summarise a write-census CSV (games/common/write_census.c).

    scripts/write_census_report.py <csv> <game-dir> [--field BASE:NAME=OFF..]

Each row's `ra` is mapped to its enclosing function: the greatest function
start <= ra in <game-dir>/generated/discovered_functions.log (main EXE), with
split pieces folded to their original entry via
generated/main_exe_split_targets.txt, or in the resident overlay's log for
an address inside the overlay window. Rows are grouped by
(phase, ra-function, entry column, address) with counts; a row whose
ra-function differs from the entry column is a store made before the
non-leaf's first jal (attributed to the caller) or after its epilogue
reload, and is marked `ra!=entry`.
"""
import re, sys
from bisect import bisect_right
from collections import Counter
from pathlib import Path

def load_starts(path):
    starts = []
    if not path.exists():
        return starts
    for line in path.read_text().splitlines():
        m = re.match(r'^(0x[0-9A-Fa-f]+)', line)
        if m:
            starts.append(int(m.group(1), 16))
    starts.sort()
    return starts

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    csv, game = Path(sys.argv[1]), Path(sys.argv[2])
    gen = game / 'generated'
    main_starts = load_starts(gen / 'discovered_functions.log')
    split_to_entry = {}
    st = gen / 'main_exe_split_targets.txt'
    if st.exists():
        for line in st.read_text().splitlines():
            m = re.match(r'^(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)', line)
            if m:
                split_to_entry[int(m.group(1), 16)] = int(m.group(2), 16)
    ovl_starts = {}
    report = (gen / 'overlay_fn_report.txt').read_text() if (gen / 'overlay_fn_report.txt').exists() else ''
    m = re.search(r'window 0x([0-9A-F]+)\.\.0x([0-9A-F]+)', report)
    win = (int(m.group(1), 16), int(m.group(2), 16)) if m else (0, 0)

    def func_of(ra, ovl):
        if win[0] <= ra < win[1] and ovl != 'none':
            if ovl not in ovl_starts:
                ovl_starts[ovl] = load_starts(gen / ovl / 'discovered_functions.log')
            starts = ovl_starts[ovl]
            tag = ovl + ':'
        else:
            starts, tag = main_starts, ''
        i = bisect_right(starts, ra) - 1
        if i < 0:
            return f'{tag}?'
        f = starts[i]
        f = split_to_entry.get(f, f)
        return f'{tag}0x{f:08X}'

    groups = Counter()
    seqs = {}
    n = 0
    for line in csv.read_text().splitlines()[1:]:
        f = line.split(',')
        if len(f) != 9:
            continue
        vbl, seq, addr, size, value, ra, entry, phase, ovl = f
        ra_i, entry_i = int(ra, 16), int(entry, 16)
        fn = func_of(ra_i, ovl)
        key = (int(phase), fn, f'0x{entry_i:08X}', addr, int(size))
        groups[key] += 1
        seqs.setdefault(key, set()).add(int(seq))
        n += 1
    print(f'{n} rows, {len(groups)} groups (phase, ra-func, entry, addr, size): count, distinct follow seqs')
    for key in sorted(groups, key=lambda k: (k[0], k[3], k[1], k[2])):
        phase, fn, entry, addr, size = key
        flag = '' if fn == entry else '  ra!=entry'
        print(f'  phase={phase} {addr} sz{size} ra-func={fn} entry={entry} n={groups[key]} seqs={len(seqs[key])}{flag}')

if __name__ == '__main__':
    main()
