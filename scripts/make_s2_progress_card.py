#!/usr/bin/env python3
"""Builds a Spyro 2 memory card whose File 1 holds level progress.

The base is the card games/spyro-2/bk2/s2-create-save.bk2 leaves on a blank
card: one file, BASCUS-94425SP2RR, a 0x200-byte header (title, clut, icon)
then three 0x600-byte records the new game writes with no progress. The
tool runs the movie (or takes --base), then edits record 0 and rewrites its
checksum. The record layout is the title overlay's writer
(func_titlescreen_and_loading_8007677C) and reader (..._800769BC):

  +0x000        level number (D_80066F90), the level the load enters
  +0x010..0x02F levels visited, one byte per level index (D_8006B084); the
                title lists a record as empty while +0x011 is zero
  +0x050..0x06F talisman, one byte per level index (D_8006B264)
  +0x070..0x08F orb bits, one byte per level index (D_80069FD0)
  +0x0B8..0x153 progress table D_80064670 (0x9C bytes)
  +0x5A8        u32 byte sum of +0x000..+0x5A7 (func_..._80076614)

Level index 0 is Summer Forest (level number 10), 1 Glimmer, 2 Idol
Springs. Summer Forest's portal update (level_10 0x80076968-0x80076C44)
spawns a destination's status icons when its talisman byte is set: one orb
icon (class 0x71) per orb of the level (D_80064834) while the progress
halfword D_800646F6 (table +0x86) is nonzero, lit when the orb's bit is set,
then the talisman icon (the portal's +0x50 class).

The defaults write the fixture games/spyro-2/cards/idol-springs-talisman.mcr:
level 10, visited 0-2, Idol Springs talisman, both of its orbs (mask 0x03),
D_800646F6 = 1. The save icon (header +0x60..+0x1FF) is zeroed. Output is
byte-identical across runs of the same build.

Usage:
  scripts/make_s2_progress_card.py --bin build/linux-release/openpete \\
      --out games/spyro-2/cards/idol-springs-talisman.mcr
  scripts/make_s2_progress_card.py --base CARD --out OUT
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

FILE_NAME = b"BASCUS-94425SP2RR"
BLOCK = 0x2000
HEADER = 0x200
RECORD = 0x600
CHECKSUM = 0x5A8
ICON = (0x60, HEADER)
LEVEL = 0x000
VISITED = 0x010
TALISMAN = 0x050
ORBS = 0x070
PROGRESS = 0x0B8
ORB_ICON_FLAG = PROGRESS + 0x86  # D_800646F6 - D_80064670
CREATE_LIMIT = 1400  # tests/harness/check_card_boot.sh save row


def file_offset(card):
    """Returns the card offset of the save file's first block, or None."""
    for b in range(1, 16):
        d = card[0x80 * b:0x80 * b + 0x80]
        if d[0] == 0x51 and d[10:10 + len(FILE_NAME)] == FILE_NAME:
            return BLOCK * b
    return None


def make_base(binary, disc, out):
    """Runs s2-create-save.bk2 on a blank card at OUT headless."""
    cmd = [binary, "--game", "spyro-2", "--headless", "--no-config", "--disc", disc,
           "--renderer", "vk", "--mute", "1", "--vibration", "0",
           "--card1", out, "--no-card2", "--bk2", "games/spyro-2/bk2/s2-create-save.bk2",
           "--frame-limit", str(CREATE_LIMIT)]
    env = dict(os.environ, OPENPETE_VIBRATION="0")
    r = subprocess.run(cmd, env=env, stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        sys.stdout.buffer.write(r.stdout[-4000:])
        sys.exit(f"base run exited {r.returncode}")


def edit(card, args):
    """Writes the progress into record 0 of CARD and returns the new image."""
    off = file_offset(card)
    if off is None:
        sys.exit(f"no {FILE_NAME.decode()} file on the base card")
    b = bytearray(card)
    b[off + ICON[0]:off + ICON[1]] = bytes(ICON[1] - ICON[0])
    r = off + HEADER
    if any(b[r + VISITED:r + VISITED + 0x20]) or any(b[r + TALISMAN:r + ORBS + 0x20]):
        sys.exit("record 0 of the base card already holds progress")
    b[r + LEVEL] = args.level
    for i in args.visited:
        b[r + VISITED + i] = 1
    for i in args.talisman:
        b[r + TALISMAN + i] = 1
    for spec in args.orbs:
        i, mask = (int(x, 0) for x in spec.split(":"))
        b[r + ORBS + i] = mask
    b[r + ORB_ICON_FLAG:r + ORB_ICON_FLAG + 2] = (1).to_bytes(2, "little")
    b[r + CHECKSUM:r + CHECKSUM + 4] = (sum(b[r:r + CHECKSUM]) & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(b)


def main():
    """Parses the arguments, makes or reads the base card and writes the edited card."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--bin", help="openpete binary that plays the base movie")
    src.add_argument("--base", help="a card s2-create-save.bk2 left")
    ap.add_argument("--disc", default="games/spyro-2/spyro-2/build/SCUS-94425.cue")
    ap.add_argument("--level", type=int, default=10)
    ap.add_argument("--visited", type=int, nargs="*", default=[0, 1, 2])
    ap.add_argument("--talisman", type=int, nargs="*", default=[2])
    ap.add_argument("--orbs", nargs="*", default=["2:0x03"], metavar="INDEX:MASK")
    args = ap.parse_args()
    if args.base:
        with open(args.base, "rb") as f:
            card = f.read()
    else:
        tmp = tempfile.mkdtemp(prefix="s2card-")
        try:
            make_base(args.bin, args.disc, os.path.join(tmp, "card1.mcr"))
            with open(os.path.join(tmp, "card1.mcr"), "rb") as f:
                card = f.read()
        finally:
            shutil.rmtree(tmp)
    with open(args.out, "wb") as f:
        f.write(edit(card, args))
    print(f"{args.out}: {len(card)} bytes")


if __name__ == "__main__":
    main()
