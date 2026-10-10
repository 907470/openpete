#!/usr/bin/env python3
"""wad_entry_map.py - name the WAD.WAD entry that holds each overlay blob.

Reads WAD.WAD from a raw 2352-byte-sector disc image (ISO 9660 root
directory lookup), takes its entry table (8-byte {byte offset, byte length}
pairs in the first two sectors), and prints `<name> <entry index>` for every
row of an overlay registry (`<magic> <name> <ovl path>`, as
regen-recomp.sh writes it) whose blob equals one entry's bytes exactly.
Used by scripts/emit-recomp-manifest.sh on the decomp's rebuilt disc, so the
committed manifest names the entry the first-boot extractor carves.

Exit 0 when every registry row matched exactly one entry, 1 otherwise (each
unmatched or ambiguous row on stderr).

  wad_entry_map.py <disc.bin> <overlay_registry.txt>
"""
import hashlib
import struct
import sys

RAW, USER, USER_OFF = 2352, 2048, 24


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    disc = open(sys.argv[1], "rb")

    def sectors(lba, count):
        out = bytearray()
        for i in range(count):
            disc.seek((lba + i) * RAW + USER_OFF)
            out += disc.read(USER)
        return bytes(out)

    pvd = sectors(16, 1)
    root_lba, root_size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
    root = sectors(root_lba, (root_size + USER - 1) // USER)
    wad = None
    i = 0
    while i < len(root):
        rec = root[i]
        if rec == 0:
            i = (i // USER + 1) * USER
            continue
        name = root[i + 33:i + 33 + root[i + 32]].decode("latin-1")
        if name.split(";")[0] == "WAD.WAD":
            wad = (struct.unpack_from("<I", root, i + 2)[0], struct.unpack_from("<I", root, i + 10)[0])
        i += rec
    if not wad:
        sys.exit(f"{sys.argv[1]}: no WAD.WAD in the root directory")
    wad_lba, wad_size = wad
    table = sectors(wad_lba, 2)

    by_hash = {}
    for e in range(len(table) // 8):
        off, size = struct.unpack_from("<II", table, 8 * e)
        if size == 0 or off % USER or off + size > wad_size:
            continue
        blob = sectors(wad_lba + off // USER, (size + USER - 1) // USER)[:size]
        by_hash.setdefault(hashlib.sha256(blob).hexdigest(), []).append(e)

    bad = 0
    for line in open(sys.argv[2]):
        parts = line.split()
        if len(parts) != 3:
            continue
        _, name, path = parts
        hits = by_hash.get(hashlib.sha256(open(path, "rb").read()).hexdigest(), [])
        if len(hits) != 1:
            print(f"wad_entry_map: {name}: {len(hits)} entries hold {path}", file=sys.stderr)
            bad += 1
            continue
        print(f"{name} {hits[0]}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
