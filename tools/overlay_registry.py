#!/usr/bin/env python3
"""Enumerate a decomp's overlays as `<id> <name> <path>`, one per line.

Two facts are needed per overlay blob and neither is reliably in its filename.

The id is the LONG at the head of the overlay section, placed there by the
decomp's overlays.ld. The runtime reads that same word out of RAM once an
overlay is resident, so reading it from the blob uses the game's own value
instead of re-deriving one. Spyro 1's id was previously computed from the
filename; the word reproduces all 37 of those, and the arithmetic does not
generalise -- Spyro 2 orders ids independently of its filenames (level_30 is
id 15 while level_31 is 14, and level_10 is id 5).

The name is the linker section name, which is what the generated dispatch
function and the forced-entry files are keyed on. Where overlays.ld spells
LEVEL(<id>, <name>) it is the authority and the blob's id selects the entry.
Where it spells LEVEL(<name>) with no id, the filename stem is the section
name already.

Usage: overlay_registry.py <decomp-dir> <wad-dir>
"""
import os
import re
import struct
import sys

# LEVEL, and its variants for the balloon and flight levels, all spell
# (overlay_id, section_name).
LEVEL_ID_NAME = re.compile(
    r'\bLEVEL[A-Z_]*\s*\(\s*(\d+)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)')
# Overlays that are not levels (credits, titlescreen, guidebook) are declared
# as plain output sections instead of through the LEVEL macro, so they carry no
# id there and are matched by name.
SECTION = re.compile(r'^\s*\.([A-Za-z_][A-Za-z0-9_]*)\s*\{', re.MULTILINE)


def read_overlays_ld(decomp_dir):
    ld = os.path.join(decomp_dir, 'overlays.ld')
    if not os.path.isfile(ld):
        return {}, set()
    text = open(ld, encoding='utf-8', errors='replace').read()
    return ({int(i): n for i, n in LEVEL_ID_NAME.findall(text)},
            set(SECTION.findall(text)))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    decomp_dir, wad_dir = sys.argv[1], sys.argv[2]
    mapping, sections = read_overlays_ld(decomp_dir)

    seen = {}
    rows = []
    for fn in sorted(os.listdir(wad_dir)):
        if not fn.endswith('.ovl'):
            continue
        path = os.path.join(wad_dir, fn)
        with open(path, 'rb') as fh:
            head = fh.read(4)
        if len(head) < 4:
            sys.exit('error: %s is too short to carry an overlay id' % path)
        oid = struct.unpack('<I', head)[0]

        name = mapping.get(oid)
        if name is None:
            stem = fn[:-4]
            name = stem[:-5] if stem.endswith('_code') else stem
            # A section may carry an _ovl suffix where the bare name was
            # already taken elsewhere in the linker script.
            if sections and name not in sections and (name + '_ovl') not in sections:
                sys.exit('error: %s has overlay id %d, which no LEVEL() entry '
                         'names, and its stem %r matches no section in '
                         'overlays.ld' % (fn, oid, name))
        if oid in seen:
            sys.exit('error: overlay id %d claimed by both %s and %s'
                     % (oid, seen[oid], fn))
        seen[oid] = fn
        rows.append((oid, name, path))

    if not rows:
        sys.exit('error: no .ovl files under %s' % wad_dir)
    for oid, name, path in rows:
        print('%d %s %s' % (oid, name, path))


if __name__ == '__main__':
    main()
