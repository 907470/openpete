#!/usr/bin/env python3
"""Licence checks run by scripts/package-dist.sh on the staged release tree.

  dist_compliance.py scan PATH...
      Refuses when a shipped ELF or PE binary under PATH carries a marker of
      a copyleft component that must never ship: pcsx-redux's GTE
      (GPL-2.0-or-later) or a statically linked FFmpeg/x264 (GPL). Exit 1 on
      a hit, and on zero binaries scanned.

  dist_compliance.py licences --stage DIR --scope S [--scope S...]
                              [--toolchain-dir DIR]
      Checks NOTICE.md against packaging/licences/manifest.tsv and copies the
      text of every component the archive carries into DIR/licences/.
      Exit 1 when a NOTICE.md row has no manifest row, a manifest row has no
      NOTICE.md row, a third_party/*/LICENSE* file is in no manifest row, or
      an active row's text is missing or empty.

Runs from the repository root.
"""

import argparse
import glob
import mmap
import os
import shutil
import sys

# Byte strings a clean openpete binary never contains. The bare words
# "x264" and "libavcodec" are excluded: the exe names them in --record's
# option and help text.
COPYLEFT_MARKERS = (
    b"_ZN4PCSX3GTE",        # pcsx-redux GTE (C++ namespace PCSX::GTE)
    b"x264 - core",         # x264's SEI user-data version banner
    b"Lavc",                # libavcodec ident (LIBAVCODEC_IDENT)
    b"Lavf",                # libavformat ident (LIBAVFORMAT_IDENT)
    b"libavcodec version",  # libavcodec's av_log version line
)

MANIFEST = "packaging/licences/manifest.tsv"
NOTICE = "NOTICE.md"


def fail(msg):
    """Print a refusal to stderr and exit 1."""
    print(f"[dist-compliance] REFUSED: {msg}", file=sys.stderr)
    sys.exit(1)


def is_binary(path):
    """True when the file starts with an ELF or a PE (MZ) magic."""
    try:
        with open(path, "rb") as f:
            head = f.read(4)
    except OSError:
        return False
    return head == b"\x7fELF" or head[:2] == b"MZ"


def iter_files(paths):
    """Yield every regular non-symlink file under the given files and directories."""
    for p in paths:
        if os.path.isfile(p) and not os.path.islink(p):
            yield p
            continue
        for root, _dirs, files in os.walk(p):
            for name in files:
                fp = os.path.join(root, name)
                if os.path.isfile(fp) and not os.path.islink(fp):
                    yield fp


def cmd_scan(args):
    """Scan every ELF/PE under args.paths for COPYLEFT_MARKERS."""
    n_bin = 0
    n_bytes = 0
    hits = []
    for fp in iter_files(args.paths):
        if not is_binary(fp):
            continue
        size = os.path.getsize(fp)
        n_bin += 1
        n_bytes += size
        if size == 0:
            continue
        with open(fp, "rb") as f, \
                mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
            for marker in COPYLEFT_MARKERS:
                if m.find(marker) >= 0:
                    hits.append((fp, marker.decode()))
    print(f"[dist-compliance] scanned {n_bin} ELF/PE binaries, {n_bytes} bytes")
    if n_bin == 0:
        fail("no ELF/PE binary under " + " ".join(args.paths))
    if hits:
        for fp, marker in hits:
            print(f"[dist-compliance] {fp}: contains '{marker}'", file=sys.stderr)
        fail(f"{len(hits)} copyleft marker hit(s); the binary links a"
             " component that must not ship")
    print("[dist-compliance] scan PASS: no copyleft marker")


def read_manifest():
    """Return manifest rows as (component, scope, text, shipped) tuples."""
    rows = []
    with open(MANIFEST, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) != 4 or not all(cols):
                fail(f"{MANIFEST}:{n}: expected 4 non-empty tab-separated columns")
            rows.append(tuple(cols))
    return rows


def notice_components():
    """Return the first cell of every NOTICE.md table body row."""
    out = []
    with open(NOTICE, encoding="utf-8") as f:
        for line in f:
            if not line.startswith("|"):
                continue
            cell = line.split("|")[1].strip()
            if cell == "Component" or set(cell) <= {"-", ":"}:
                continue
            out.append(cell)
    return out


def cmd_licences(args):
    """Cross-check NOTICE.md with the manifest and stage the active texts."""
    rows = read_manifest()
    shipped_rows = [r for r in rows if r[1] != "none"]
    manifest_names = {r[0] for r in shipped_rows}
    notice = notice_components()
    missing = [c for c in notice if c not in manifest_names]
    if missing:
        fail("NOTICE.md rows with no licence text in " + MANIFEST + ": "
             + "; ".join(missing))
    stale = sorted(manifest_names - set(notice))
    if stale:
        fail(MANIFEST + " rows with no NOTICE.md row: " + "; ".join(stale))
    texts = {r[2] for r in rows}
    orphan = [p for p in sorted(glob.glob("third_party/*/LICENSE*"))
              if p not in texts]
    if orphan:
        fail("third_party licence files in no " + MANIFEST + " row: "
             + ", ".join(orphan))

    dest = os.path.join(args.stage, "licences")
    os.makedirs(dest, exist_ok=True)
    scopes = set(args.scope)
    copied = 0
    for comp, scope, text, shipped in shipped_rows:
        if scope not in scopes or text == "-" or text.startswith("lib:"):
            continue
        if scope.startswith("toolchain-"):
            if not args.toolchain_dir:
                fail(f"scope {scope} active with no --toolchain-dir")
            src = os.path.join(args.toolchain_dir, text)
        else:
            src = text
        if not os.path.isfile(src) or os.path.getsize(src) == 0:
            fail(f"'{comp}' ships with no licence text: {src} missing or empty")
        shutil.copyfile(src, os.path.join(dest, shipped))
        copied += 1
    if copied == 0:
        fail("no licence text staged")
    print(f"[dist-compliance] licences PASS: {copied} texts in {dest}"
          f" (scopes: {' '.join(sorted(scopes))})")


def main():
    """Parse the subcommand and run it."""
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("paths", nargs="+")
    s.set_defaults(fn=cmd_scan)
    lic = sub.add_parser("licences")
    lic.add_argument("--stage", required=True)
    lic.add_argument("--scope", action="append", required=True,
                     choices=["all", "windows", "sdk", "toolchain-windows",
                              "toolchain-linux", "appimage"])
    lic.add_argument("--toolchain-dir")
    lic.set_defaults(fn=cmd_licences)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
