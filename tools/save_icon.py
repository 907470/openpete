#!/usr/bin/env python3
"""save_icon.py - where a game's memory-card save-file header sits on its
disc, and the C table that hands that location to the runtime.

The game writes a 512-byte save-file header at the start of its card file:
"SC", the icon flag, the block count, the title, then the icon CLUT at
+0x60 (16 BGR555 entries) and the 16x16 4bpp icon frames from +0x80
(tools/mcs_icon_to_ico.py decodes it). The header is a constant in the
game's executable or in one of its WAD.WAD overlays, so the runtime reads
the icon from the player's own disc at the location the game's manifest
(games/<game>/manifest/save_icon.toml) records; no icon byte is committed
or built into the executable.

The manifest names the file the way the first-boot manifest
(games/<game>/manifest/recomp/<serial>/manifest.toml) names a binary it
carves: `input = "PSX.EXE"` for the executable (read by its ISO 9660 name,
recomp.conf's EXE_FILENAME), or `wad_entry = N` for WAD.WAD entry N (the
index into the 8-byte {byte offset, byte length} table in the WAD's first
two sectors). `offset` is the header's byte offset in that file (the
executable's offset counts its 2048-byte PS-X EXE header); `sha256` is the
digest of the 512 header bytes on the retail disc.

Subcommands:

  locate --disc BIN --exe-name NAME --header REF [--game G]
      Search the executable, then every WAD.WAD entry in index order, for
      the 512 bytes of REF (the header as the decomp's data listing holds
      it: tools/extract_save_header.py output, or Spyro 1's
      assets/save_header.databin.bin) and print the manifest for the first
      hit. Exit 1 when no file holds it.

  header --manifest TOML --out H
      The C table the game's build record compiles (games/common/
      game_build.c.in). Refuses a manifest with both or neither of input and
      wad_entry, an input other than "PSX.EXE", a negative entry, an offset
      that is not an integer, or a sha256 that is not 64 hex digits.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib  # type: ignore

RAW, USER, USER_OFF = 2352, 2048, 24
HEADER_SIZE = 512
FORMAT = 1


def iso_files(disc: bytes):
    """Return {name: (lba, size)} for the root directory of a raw 2352-byte-sector image, and a sector reader."""
    def sectors(lba, count):
        return b"".join(disc[(lba + i) * RAW + USER_OFF:(lba + i) * RAW + USER_OFF + USER]
                        for i in range(count))
    pvd = sectors(16, 1)
    root_lba, root_size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
    root = sectors(root_lba, (root_size + USER - 1) // USER)
    files, i = {}, 0
    while i < len(root):
        rec = root[i]
        if rec == 0:
            i = (i // USER + 1) * USER
            continue
        name = root[i + 33:i + 33 + root[i + 32]].decode("latin-1").split(";")[0]
        files[name] = (struct.unpack_from("<I", root, i + 2)[0], struct.unpack_from("<I", root, i + 10)[0])
        i += rec
    return files, sectors


def cmd_locate(a) -> int:
    """Print the save_icon manifest for the first file on the disc holding the reference header."""
    ref = Path(a.header).read_bytes()[:HEADER_SIZE]
    if len(ref) != HEADER_SIZE or ref[:2] != b"SC":
        print(f"save_icon: {a.header} is not a 512-byte SC header", file=sys.stderr)
        return 1
    files, sectors = iso_files(Path(a.disc).read_bytes())

    def read(name):
        lba, size = files[name]
        return sectors(lba, (size + USER - 1) // USER)[:size]

    hit = None
    if a.exe_name in files:
        off = read(a.exe_name).find(ref)
        if off >= 0:
            hit = ('input = "PSX.EXE"', off)
    if hit is None and "WAD.WAD" in files:
        wad = read("WAD.WAD")
        for e in range(2 * USER // 8):
            eoff, elen = struct.unpack_from("<II", wad, 8 * e)
            if elen == 0 or eoff + elen > len(wad):
                continue
            off = wad[eoff:eoff + elen].find(ref)
            if off >= 0:
                hit = (f"wad_entry = {e}", off)
                break
    if hit is None:
        print(f"save_icon: no file on {a.disc} holds the header", file=sys.stderr)
        return 1
    print("# The save-file header the game writes at the start of its memory-card")
    print("# file, read at run time for the window icon (tools/save_icon.py).")
    print(f"format = {FORMAT}")
    if a.game:
        print(f'game = "{a.game}"')
    print()
    print("[save_icon]")
    print(hit[0])
    print(f"offset = 0x{hit[1]:X}")
    print(f'sha256 = "{hashlib.sha256(ref).hexdigest()}"')
    return 0


def load_manifest(path: str):
    """Return (wad_entry, offset, sha256) from a save_icon manifest; wad_entry -1 names the executable."""
    with open(path, "rb") as f:
        doc = tomllib.load(f)
    if doc.get("format") != FORMAT:
        raise ValueError(f"{path}: format {doc.get('format')!r}, want {FORMAT}")
    t = doc.get("save_icon")
    if not isinstance(t, dict):
        raise ValueError(f"{path}: no [save_icon] table")
    has_input, has_entry = "input" in t, "wad_entry" in t
    if has_input == has_entry:
        raise ValueError(f"{path}: [save_icon] names both or neither of input and wad_entry")
    if has_input and t["input"] != "PSX.EXE":
        raise ValueError(f'{path}: input {t["input"]!r}, the only input is "PSX.EXE"')
    entry = -1
    if has_entry:
        entry = t["wad_entry"]
        if not isinstance(entry, int) or entry < 0:
            raise ValueError(f"{path}: wad_entry {entry!r} is not a non-negative integer")
    off = t.get("offset")
    if not isinstance(off, int) or off < 0:
        raise ValueError(f"{path}: offset {off!r} is not a non-negative integer")
    sha = t.get("sha256")
    if not isinstance(sha, str) or len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
        raise ValueError(f"{path}: sha256 {sha!r} is not 64 lowercase hex digits")
    return entry, off, sha


def cmd_header(a) -> int:
    """Write the C table of the manifest's location."""
    try:
        entry, off, sha = load_manifest(a.manifest)
    except ValueError as e:
        print(f"save_icon: {e}", file=sys.stderr)
        return 1
    Path(a.out).write_text(
        "/* Generated by tools/save_icon.py header from the game's\n"
        " * manifest/save_icon.toml; regenerate, never edit. */\n"
        "#ifndef OPENPETE_SAVE_ICON_GEN_H\n"
        "#define OPENPETE_SAVE_ICON_GEN_H\n\n"
        '#include "openpete_save_icon.h"\n\n'
        "static const openpete_save_icon_loc_t openpete_save_icon_loc = {\n"
        f"    {entry}, 0x{off:X}u, \"{sha}\",\n"
        "};\n\n"
        "#endif\n")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    lo = sub.add_parser("locate")
    lo.add_argument("--disc", required=True)
    lo.add_argument("--exe-name", required=True)
    lo.add_argument("--header", required=True)
    lo.add_argument("--game")
    he = sub.add_parser("header")
    he.add_argument("--manifest", required=True)
    he.add_argument("--out", required=True)
    a = ap.parse_args()
    return cmd_locate(a) if a.cmd == "locate" else cmd_header(a)


if __name__ == "__main__":
    sys.exit(main())
