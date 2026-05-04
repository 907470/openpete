"""
bk2_to_padstream.py — convert a BizHawk .bk2 movie to a flat padstream.

Output format (text, designed for Lua-side consumption):

    # psx-padstream v1 frames=N source=foo.bk2
    # frame pad1 pad2
    0 ffff ffff
    1 ffff ffff
    2 fff7 ffff
    ...

Each non-comment line: `<frame_no> <pad1_hex> <pad2_hex>`. Pad words use the
PSX convention (released-when-set; 0xFFFF = nothing pressed). pad2 is
emitted as ffff today — the bk2 LogKey we read only carries P1.

Companion: scripts/pcsx-redux-bk2-replay.lua loads this stream and drives
PCSX-Redux's pad-override API so
the openpete <-> PCSX-Redux oracle can run from the same input source.

The decode logic mirrors core/runtime/bk2_movie.c — keep them in sync if
the bk2 dialect changes.
"""

from __future__ import annotations

import argparse
import io
import sys
import zipfile
from typing import List

# bk2 P1 digital order → PSX pad bit *index* (not mask). Same table as in
# core/runtime/bk2_movie.c, just keyed by index instead of OR-mask.
BK2_P1_DIGITAL_BIT = [
    4,   # 0  Up
    6,   # 1  Down
    7,   # 2  Left
    5,   # 3  Right
    0,   # 4  Select
    3,   # 5  Start
    15,  # 6  Square
    12,  # 7  Triangle
    13,  # 8  Circle
    14,  # 9  Cross
    10,  # 10 L1
    11,  # 11 R1
    8,   # 12 L2
    9,   # 13 R2
    1,   # 14 L3
    2,   # 15 R3
    # index 16 = MODE — analog-toggle key, not a PSX bit; we stop at 16.
]


def decode_frame(line: bytes) -> int:
    """Decode one |...|...| line into a 16-bit PSX pad word.

    Returns 0xFFFF on malformed input (released-everything fallback). This
    matches the C decoder's behaviour: keep the timeline length intact.
    """
    if not line.startswith(b"|"):
        return 0xFFFF
    parts = line.split(b"|")
    # Parts: ['', seg0, seg1, ...maybe more, ''] for "|seg0|seg1|".
    if len(parts) < 3:
        return 0xFFFF
    seg1 = parts[2]
    # Skip 4 analog axes (4 commas), then read up to 16 digital chars.
    commas = 0
    i = 0
    while i < len(seg1) and commas < 4:
        if seg1[i:i + 1] == b",":
            commas += 1
        i += 1
    if commas < 4:
        return 0xFFFF
    tail = seg1[i:]
    if len(tail) < 16:
        return 0xFFFF
    bits = 0xFFFF
    for j in range(16):
        if tail[j:j + 1] != b".":
            bits &= ~(1 << BK2_P1_DIGITAL_BIT[j]) & 0xFFFF
    return bits


def parse_bk2(path: str) -> List[int]:
    with zipfile.ZipFile(path, "r") as zf:
        with zf.open("Input Log.txt") as fp:
            data = fp.read()
    frames: List[int] = []
    for raw in data.splitlines():
        # Strip CR (BizHawk writes CRLF inside the zip member sometimes).
        line = raw.rstrip(b"\r")
        if not line:
            continue
        if line.startswith(b"|") and not line.startswith(b"|["):
            # Skip the LogKey header — that line starts with "LogKey:" so
            # it's filtered above. Frame lines start with '|'.
            frames.append(decode_frame(line))
    return frames


def write_padstream(frames: List[int], out_path: str, source: str) -> None:
    if out_path == "-":
        fp = sys.stdout
        close = False
    else:
        fp = open(out_path, "w", encoding="ascii", newline="\n")
        close = True
    try:
        fp.write(f"# psx-padstream v1 frames={len(frames)} source={source}\n")
        fp.write("# frame pad1 pad2\n")
        for i, w in enumerate(frames):
            fp.write(f"{i} {w:04x} ffff\n")
    finally:
        if close:
            fp.close()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("bk2", help="input .bk2 path")
    p.add_argument("-o", "--output", default="-",
                   help="output padstream path (default stdout)")
    p.add_argument("--summary", action="store_true",
                   help="print a one-line summary to stderr after writing")
    args = p.parse_args(argv)

    frames = parse_bk2(args.bk2)
    write_padstream(frames, args.output, args.bk2)

    if args.summary:
        first_input = next((i for i, w in enumerate(frames) if w != 0xFFFF), -1)
        n_pressed = sum(1 for w in frames if w != 0xFFFF)
        print(f"[bk2->padstream] frames={len(frames)} "
              f"first_input_frame={first_input} pressed_frames={n_pressed}",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
