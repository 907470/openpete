#!/usr/bin/env python3
"""check_regions_containment.py — prove a new flat region table hashes every
byte the old one hashed.

    tools/check_regions_containment.py OLD.flat NEW.flat

Direct rows: every unmasked byte of every old DIRECT row lies inside a new
DIRECT row that does not mask it (a byte the old table masked may be
masked or hashed by the new one). Indirect rows: every old indirect row
has a new row with the same kind, base pointer, element size, mask and a
length at least as long. Exit 0 when contained, 2 with every uncovered
byte range and unmatched indirect row named, 64 on a malformed file.
"""
from __future__ import annotations

import sys

RAM = 0x200000


def parse(path: str):
    rows = []
    with open(path) as fh:
        head = fh.readline().split()
        if len(head) < 2 or head[0] != "psxtrace-regions":
            sys.exit(f"{path}: not a psxtrace-regions file")
        for line in fh:
            f = line.split()
            if not f or f[0].startswith("#"):
                continue
            if len(f) < 8:
                sys.exit(f"{path}: short row: {line.rstrip()}")
            kind, start, length, aux, elem, mask_per, mask, name = f[0], int(f[1], 16), int(f[2], 16), \
                int(f[3], 16), int(f[4], 16), int(f[5]), f[6], " ".join(f[7:])
            offsets = set() if mask == "-" else {int(x, 16) for x in mask.split(",")}
            rows.append(dict(kind=kind, start=start, length=length, aux=aux, elem=elem,
                             mask_per=mask_per, mask=offsets, name=name))
    return rows


def direct_coverage(rows):
    """Bitmap of hashed (unmasked) bytes over the DIRECT rows, RAM offsets."""
    cov = bytearray(RAM)
    for r in rows:
        if r["kind"] != "DIRECT":
            continue
        lo = r["start"] & (RAM - 1)
        for k in range(r["length"]):
            if r["mask_per"]:
                masked = (k % r["elem"]) in r["mask"] if r["elem"] else False
            else:
                masked = k in r["mask"]
            if not masked:
                cov[lo + k] = 1
    return cov


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 64
    old, new = parse(argv[1]), parse(argv[2])
    cov_old, cov_new = direct_coverage(old), direct_coverage(new)
    missing = []
    k = 0
    while k < RAM:
        if cov_old[k] and not cov_new[k]:
            j = k
            while j < RAM and cov_old[j] and not cov_new[j]:
                j += 1
            missing.append((k, j))
            k = j
        else:
            k += 1
    old_bytes, new_bytes = sum(cov_old), sum(cov_new)
    print(f"[containment] old direct bytes hashed: {old_bytes}; new: {new_bytes}")
    for lo, hi in missing:
        owner = next(r["name"] for r in old if r["kind"] == "DIRECT"
                     and (r["start"] & (RAM - 1)) <= lo < (r["start"] & (RAM - 1)) + r["length"])
        print(f"[containment] UNCOVERED 0x{0x80000000 + lo:08X}-0x{0x80000000 + hi:08X} ({hi - lo} B) old row {owner}")
    unmatched = []
    for r in old:
        if r["kind"] == "DIRECT":
            continue
        ok = any(n["kind"] == r["kind"] and n["start"] == r["start"] and n["aux"] == r["aux"]
                 and n["elem"] == r["elem"] and n["mask_per"] == r["mask_per"]
                 and n["mask"] <= r["mask"] and n["length"] >= r["length"] for n in new)
        if not ok:
            unmatched.append(r["name"])
    n_ind = sum(1 for r in old if r["kind"] != "DIRECT")
    print(f"[containment] old indirect rows: {n_ind}; matched in new: {n_ind - len(unmatched)}")
    for nm in unmatched:
        print(f"[containment] UNMATCHED indirect row {nm}")
    if missing or unmatched:
        print("[containment] FAIL")
        return 2
    print("[containment] PASS: every byte the old table hashed is hashed by the new one")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
