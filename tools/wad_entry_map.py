#!/usr/bin/env python3
"""wad_entry_map.py - name the WAD.WAD entry that holds each overlay blob.

Reads WAD.WAD from a raw 2352-byte-sector disc image (ISO 9660 root
directory lookup), takes its entry table (8-byte {byte offset, byte length}
pairs in the first two sectors), and prints `<name> <entry index>` for every
row of an overlay registry (`<magic> <name> <ovl path>`, as
regen-recomp.sh writes it) whose blob equals one entry's bytes exactly.
Used by scripts/emit-recomp-manifest.sh on the decomp's rebuilt disc, so the
committed manifest names the entry the first-boot extractor carves.

With --map, a blob no entry holds raw is matched through the post-link
transforms a mastering step applied to the disc's copy, and its line gains
the carve-time inverse: `<name> <entry> <decrypt 0|1> <restore>`, where
<restore> is `-` or comma-separated `<offset>:<word>` pairs (hex). The
transforms tried, in order:
  decrypt  the words [0x1000, key - load_addr) XORed with key, the le32 at
           byte 4 of the entry (--decrypt);
  restore  a word at a psx.map symbol (each --restore-symbol template, with
           {name} replaced by the overlay name) that differs from the blob's
           is set back to the blob's.
A blob matches an entry when the transformed entry's sha256 equals the
blob's. --exe <PSX.EXE> --exe-name <ISO name> adds a `@main_exe <restore>`
line: the disc's executable restored at --exe-restore-symbol words must
equal the decomp's PSX.EXE.

Exit 0 when every registry row (and the executable, when asked) matched
exactly one entry, 1 otherwise (each unmatched or ambiguous row on stderr).

  wad_entry_map.py <disc.bin> <overlay_registry.txt>
      [--map psx.map --load-addr ADDR [--decrypt]
       [--restore-symbol TEMPLATE]... [--exe PSX.EXE --exe-name NAME
       [--exe-restore-symbol SYMBOL]...]]
"""
import argparse
import hashlib
import re
import struct
import sys

RAW, USER, USER_OFF = 2352, 2048, 24
DECRYPT_FROM = 0x1000


def decrypt(blob, load_addr):
    """XOR the words [0x1000, key - load_addr) with key, the le32 at byte 4.

    Returns the decrypted copy, or None when the key's end lies outside the
    blob; mirrors the game loader's in-place loop over the same span."""
    if len(blob) < 8:
        return None
    key = struct.unpack_from("<I", blob, 4)[0]
    end = key - load_addr
    if key < load_addr or end > len(blob):
        return None
    out = bytearray(blob)
    off = DECRYPT_FROM
    while off < end and off + 4 <= len(out):
        struct.pack_into("<I", out, off, struct.unpack_from("<I", out, off)[0] ^ key)
        off += 4
    return bytes(out)


def restore(blob, ref, offsets):
    """Set each word of blob at offsets that differs from ref's back to ref's.

    Returns (restored copy, [(offset, word)] of the words set)."""
    out = bytearray(blob)
    rows = []
    for off in offsets:
        if off + 4 > len(out) or off + 4 > len(ref):
            continue
        want = struct.unpack_from("<I", ref, off)[0]
        if struct.unpack_from("<I", out, off)[0] != want:
            struct.pack_into("<I", out, off, want)
            rows.append((off, want))
    return bytes(out), rows


def fmt_restore(rows):
    """Render restore rows as the `-` or `<offset>:<word>,...` field."""
    return ",".join(f"0x{o:x}:0x{w:08x}" for o, w in rows) or "-"


def main():
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument("disc")
    ap.add_argument("registry")
    ap.add_argument("--map")
    ap.add_argument("--load-addr", type=lambda s: int(s, 0))
    ap.add_argument("--decrypt", action="store_true")
    ap.add_argument("--restore-symbol", action="append", default=[])
    ap.add_argument("--exe")
    ap.add_argument("--exe-name")
    ap.add_argument("--exe-restore-symbol", action="append", default=[])
    a = ap.parse_args()
    if a.map and a.load_addr is None:
        sys.exit("wad_entry_map: --map needs --load-addr")
    if a.exe and not (a.map and a.exe_name):
        sys.exit("wad_entry_map: --exe needs --map and --exe-name")
    disc = open(a.disc, "rb")

    def sectors(lba, count):
        out = bytearray()
        for i in range(count):
            disc.seek((lba + i) * RAW + USER_OFF)
            out += disc.read(USER)
        return bytes(out)

    pvd = sectors(16, 1)
    root_lba, root_size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
    root = sectors(root_lba, (root_size + USER - 1) // USER)
    files = {}
    i = 0
    while i < len(root):
        rec = root[i]
        if rec == 0:
            i = (i // USER + 1) * USER
            continue
        name = root[i + 33:i + 33 + root[i + 32]].decode("latin-1")
        files[name.split(";")[0]] = (struct.unpack_from("<I", root, i + 2)[0],
                                     struct.unpack_from("<I", root, i + 10)[0])
        i += rec
    if "WAD.WAD" not in files:
        sys.exit(f"{a.disc}: no WAD.WAD in the root directory")
    wad_lba, wad_size = files["WAD.WAD"]
    table = sectors(wad_lba, 2)

    symbols = {}
    if a.map:
        for m in re.finditer(r"^\s*0x([0-9a-fA-F]{8,16})\s+([A-Za-z_]\w*)(?:\s*=.*)?\s*$", open(a.map).read(), re.M):
            symbols.setdefault(m.group(2), int(m.group(1), 16))

    by_hash = {}
    blobs = []
    for e in range(len(table) // 8):
        off, size = struct.unpack_from("<II", table, 8 * e)
        if size == 0 or off % USER or off + size > wad_size:
            continue
        blob = sectors(wad_lba + off // USER, (size + USER - 1) // USER)[:size]
        by_hash.setdefault(hashlib.sha256(blob).hexdigest(), []).append(e)
        blobs.append((e, blob))

    bad = 0
    for line in open(a.registry):
        parts = line.split()
        if len(parts) != 3:
            continue
        _, name, path = parts
        ref = open(path, "rb").read()
        want = hashlib.sha256(ref).hexdigest()
        hits = [(e, 0, []) for e in by_hash.get(want, [])]
        if not hits and a.map:
            offsets = sorted({symbols[t.replace("{name}", name)] - a.load_addr
                              for t in a.restore_symbol
                              if t.replace("{name}", name) in symbols})
            offsets = [o for o in offsets if 0 <= o < len(ref)]
            for e, blob in blobs:
                if len(blob) != len(ref):
                    continue
                for dec in ((0, 1) if a.decrypt else (0,)):
                    t = decrypt(blob, a.load_addr) if dec else blob
                    if t is None:
                        continue
                    t, rows = restore(t, ref, offsets)
                    if hashlib.sha256(t).hexdigest() == want:
                        hits.append((e, dec, rows))
        if len(hits) != 1:
            print(f"wad_entry_map: {name}: {len(hits)} entries hold {path}", file=sys.stderr)
            bad += 1
            continue
        e, dec, rows = hits[0]
        if a.map:
            print(f"{name} {e} {dec} {fmt_restore(rows)}")
        else:
            print(f"{name} {e}")

    if a.exe:
        ref = open(a.exe, "rb").read()
        if a.exe_name not in files:
            print(f"wad_entry_map: no {a.exe_name} in the root directory", file=sys.stderr)
            return 1
        lba, size = files[a.exe_name]
        exe = sectors(lba, (size + USER - 1) // USER)[:size]
        t_addr = struct.unpack_from("<I", ref, 0x18)[0]
        offsets = sorted({symbols[s] - t_addr + 0x800 for s in a.exe_restore_symbol if s in symbols})
        exe, rows = restore(exe, ref, [o for o in offsets if 0x800 <= o < len(ref)])
        if exe != ref:
            print(f"wad_entry_map: {a.exe_name} restored does not equal {a.exe}", file=sys.stderr)
            bad += 1
        else:
            print(f"@main_exe {fmt_restore(rows)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
