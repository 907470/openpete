#!/usr/bin/env python3
"""Licence fence run by scripts/package-dist.sh on the staged release tree.

  dist_compliance.py scan PATH...
      Refuses when a shipped ELF or PE binary under PATH carries a marker of
      a copyleft component that must never ship: pcsx-redux's GTE
      (GPL-2.0-or-later) or a statically linked FFmpeg/x264 (GPL). Exit 1 on
      a hit, and on zero binaries scanned.

Runs from the repository root.
"""

import argparse
import mmap
import os
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


def main():
    """Parse the subcommand and run it."""
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("paths", nargs="+")
    s.set_defaults(fn=cmd_scan)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
