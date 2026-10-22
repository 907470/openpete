#!/usr/bin/env python3
"""extract_s3_credits_paths.py - convert the Spyro 3 (SCUS-94467) end-credits
camera paths into --freecam-path specs, read from the user's own disc image.

    tools/extract_s3_credits_paths.py \
        --disc games/spyro-3/spyro-3/build/SCUS-94467.cue \
        --out-dir /tmp/s3-credits-paths

writes scene_00.path .. scene_11.path, one spec per file, for
`--freecam-path "$(cat scene_00.path)"`.

Format facts (spyro-3 decomp, disc-verified):
  - The credits overlay (func_credits_80074BA0) plays 12 scenes; scene k is
    WAD.WAD entry 183 + k (wadHeader + 0x5B8 + k*8, {offset, size}); entry
    195 is empty.
  - An entry opens with a sub-file table: {offset, size} pairs from +0x08,
    offsets relative to the entry. The camera path is the third pair
    (+0x18 offset, +0x1C size).
  - Path block: +0x00 playback counter (0 on disc), +0x04 block length,
    +0x08 key count, +0x10 keys of 12 bytes: u16 x, y, z, roll, pitch, yaw.
  - Each tick the overlay takes key (counter >> 1), the counter advancing by
    the VBLs since the last tick (D_8006C648), so one key per two VBLs. It
    writes x, y, z << 2 to g_Camera+0x28/+0x2C/+0x30 and the three angles to
    +0x44/+0x46/+0x48 as stored, with no interpolation between keys.
  - func_8004F6C4 builds the view as Rz(+0x44)·Rx(+0x46)·Ry(+0x48), each
    angle masked to 12 bits (0x1000 per turn): +0x44 is the roll about the
    view axis. The Spyro 3 freecam stores x, y, z to +0x28 and roll, pitch,
    yaw to +0x44/+0x46/+0x48 in the same units, so a key maps to a waypoint
    "4x,4y,4z,yaw,pitch,roll" with each angle wrapped to [-0x800, 0x7FF].

With --step 1 (the default) a waypoint per key and ticks=1 make the freecam
land on each key on its own tick, as the game does at two VBLs per tick.
--step N keeps every Nth key (and the last) with ticks=N, the freecam then
interpolating linearly where the game holds each key. The freecam path loops,
so its last waypoint is followed by a one-segment move back to the first.
"""

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_s3_player import Disc, iso_find  # noqa: E402

CREDITS_FIRST_ENTRY = 183
CREDITS_SCENES = 12
WAD_HEADER_SIZE = 0x620
PATH_SUBFILE = 0x18
KEY_BASE = 0x10
KEY_SIZE = 12


def resolve_bin(disc: Path) -> Path:
    """Return the image a .cue names, else the .bin beside it with the cue's stem, else the path itself."""
    if disc.suffix.lower() == ".cue":
        for line in disc.read_text(errors="replace").splitlines():
            if "FILE" in line and '"' in line:
                named = disc.parent / line.split('"')[1]
                if named.exists():
                    return named
                stem = disc.with_suffix(".bin")
                if stem.exists():
                    return stem
                raise SystemExit(f"{disc} names {named.name}, absent, and {stem.name} is absent too")
        raise SystemExit(f"no FILE line in {disc}")
    return disc


def wrap12(a: int) -> int:
    """Wrap a 12-bit stored angle to [-0x800, 0x7FF]."""
    return ((a + 0x800) & 0xFFF) - 0x800


def scene_keys(disc: Disc, wad_lba: int, hdr: bytes, scene: int):
    """Return scene's keys as (x, y, z, roll, pitch, yaw) in g_Camera units."""
    entry = CREDITS_FIRST_ENTRY + scene
    off, size = struct.unpack_from("<ii", hdr, entry * 8)
    if size <= 0:
        raise SystemExit(f"scene {scene}: WAD entry {entry} is empty")
    p_off, p_size = struct.unpack_from("<ii", disc.read_span(wad_lba, off, 0x20),
                                       PATH_SUBFILE)
    if not 0 < p_size <= size:
        raise SystemExit(f"scene {scene}: implausible path sub-file size {p_size:#x}")
    blk = disc.read_span(wad_lba, off + p_off, p_size)
    _, length, n = struct.unpack_from("<iii", blk, 0)
    if n <= 0 or KEY_BASE + n * KEY_SIZE > 4 + length:
        raise SystemExit(f"scene {scene}: {n} keys do not fit the {length:#x}-byte block")
    keys = []
    for j in range(n):
        x, y, z, roll, pitch, yaw = struct.unpack_from("<6H", blk, KEY_BASE + j * KEY_SIZE)
        keys.append((x << 2, y << 2, z << 2, wrap12(roll), wrap12(pitch), wrap12(yaw)))
    return keys


def path_spec(keys, step: int, roll: bool) -> str:
    """Return the --freecam-path spec for keys sampled every step keys."""
    idx = list(range(0, len(keys), step))
    if idx[-1] != len(keys) - 1:
        idx.append(len(keys) - 1)
    wps = []
    for i in idx:
        x, y, z, r, p, yw = keys[i]
        wp = f"{x},{y},{z},{yw},{p}"
        wps.append(f"{wp},{r}" if roll else wp)
    return ";".join([f"ticks={step}"] + wps)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--disc", required=True, type=Path,
                    help=".cue or .bin/.iso (raw or cooked)")
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--step", type=int, default=1,
                    help="keep every Nth key, ticks=N per segment (default 1)")
    ap.add_argument("--no-roll", action="store_true",
                    help="write x,y,z,yaw,pitch waypoints, dropping the roll")
    ap.add_argument("--scene", type=int, action="append",
                    help="scene index 0-11 (repeatable; default all)")
    args = ap.parse_args()
    if args.step < 1:
        raise SystemExit("--step must be at least 1")

    disc = Disc(resolve_bin(args.disc))
    wad_lba, _ = iso_find(disc, "WAD.WAD")
    hdr = disc.read_span(wad_lba, 0, WAD_HEADER_SIZE)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    for scene in args.scene or range(CREDITS_SCENES):
        if not 0 <= scene < CREDITS_SCENES:
            raise SystemExit(f"scene {scene} out of range 0-{CREDITS_SCENES - 1}")
        keys = scene_keys(disc, wad_lba, hdr, scene)
        spec = path_spec(keys, args.step, not args.no_roll)
        out = args.out_dir / f"scene_{scene:02d}.path"
        out.unlink(missing_ok=True)
        out.write_text(spec + "\n")
        print(f"scene {scene:2d}: {len(keys)} keys -> {spec.count(';')} waypoints, "
              f"{len(spec)} chars -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
