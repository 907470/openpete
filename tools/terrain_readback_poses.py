#!/usr/bin/env python3
"""Enumerate anim/near poses for the terrain readback manifests from level data.

Inputs, from one freecam run of the level (an overview such as the named
pose `topdown`, so the poly-anim sectors draw from the anim arena):
  EMU       the OPENPETE_TERRAIN_FP32_EMU file: per sector slot (S line) every
            decoded vertex as a world x4 integer (V lines);
  READBACK  the OPENPETE_TERRAIN_READBACK dump of the same run: a slot that
            wrote an element from an anim arena (5..9) is poly-anim.
For every poly-anim slot A and every other slot B sharing at least two world
vertices with it, the camera stands HEIGHT above the vertex of B whose
clearance (distance from A's vertex centroid less A's vertex radius) is
nearest CLEAR and at least CLEAR_MIN, looking at A's centroid. B then
crosses the camera plane (near arena) while A lies beyond the near
threshold (anim arena), and the vertices they share meet on screen.
Prints manifest rows, most shared vertices first, each after a comment
naming its slots. Yaw and pitch are in 4096ths of a turn: yaw 0 looks along
+X and rises toward +Y, pitch rises looking down. The rows are candidates:
tests/harness/check_terrain_readback.sh decides what a pose compares.

usage: terrain_readback_poses.py EMU READBACK LEVEL [--scale S] [--height H]
           [--clear C] [--clear-min M] [--max N]
  --scale      world units per x4 unit (4: the dump's x4 of world/16)
  --height     camera height above the chosen vertex (250)
  --clear      wanted clearance beyond A's vertex radius (2500)
  --clear-min  least clearance (800)
  --max        rows printed (12)
"""
import math
import struct
import sys

import numpy as np


def slots_of_emu(path):
    """Return {slot: set of world x4 vertex tuples}, the last decode of each slot."""
    out, cur = {}, None
    for ln in open(path):
        t = ln.split()
        if not t:
            continue
        if t[0] == "S":
            cur = int(t[1])
            out[cur] = set()
        elif t[0] == "V" and cur is not None:
            out[cur].add((int(t[1]), int(t[2]), int(t[3])))
    return out


def anim_slots(path):
    """Return the slots that wrote an element from an anim arena (5..9)."""
    data = open(path, "rb").read()
    n = struct.unpack_from("<6I", data, 0)[4]
    ents = np.frombuffer(data, "<u4", count=5 * n, offset=24).reshape(n, 5)
    tex = np.frombuffer(data, "<u4", offset=24 + 20 * n).reshape(-1, 2, 4)
    out = set()
    for a, _b, base, count, _p in ents:
        if 5 <= a < 10:
            w = tex[base:base + count, 1, 3]
            out.update(int(x) for x in np.unique(w[w != 0] >> 8))
    return out


def candidates(S, anim, scale, height, target, cmin):
    """Return (shared, clearance, a, b, camera, yaw, pitch) for every anim/static slot pair."""
    rows = []
    for a in sorted(anim):
        va = np.array(sorted(S[a]), float) * scale
        ca = va.mean(axis=0)
        ra = float(np.max(np.linalg.norm(va - ca, axis=1)))
        for b, vb in S.items():
            share = len(S[a] & vb)
            if b in anim or share < 2:
                continue
            pts = np.array(sorted(vb), float) * scale + np.array([0.0, 0.0, height])
            cl = np.linalg.norm(pts - ca, axis=1) - ra
            ok = cl >= cmin
            if not ok.any():
                continue
            k = int(np.argmin(np.where(ok, np.abs(cl - target), np.inf)))
            dx, dy, dz = ca - pts[k]
            yaw = int(round(math.atan2(dy, dx) / (2 * math.pi) * 4096)) % 4096
            yaw = yaw - 4096 if yaw >= 2048 else yaw
            pitch = int(round(math.atan2(-dz, math.hypot(dx, dy)) / (2 * math.pi) * 4096))
            rows.append((share, float(cl[k]), a, b, pts[k], yaw, pitch))
    rows.sort(key=lambda r: (-r[0], abs(r[1] - target)))
    return rows


def main(argv):
    """Print the candidate manifest rows."""
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 64
    emu, rb, level = argv[:3]
    opt = dict(zip(argv[3::2], argv[4::2]))
    S = slots_of_emu(emu)
    rows = candidates(S, anim_slots(rb) & set(S), float(opt.get("--scale", 4)),
                      float(opt.get("--height", 250)), float(opt.get("--clear", 2500)),
                      float(opt.get("--clear-min", 800)))
    for share, clear, a, b, p, yaw, pitch in rows[:int(opt.get("--max", 12))]:
        print(f"# anim slot {a} against static slot {b}: {share} shared vertices, "
              f"clearance {clear:.0f}")
        print(f"{level} | pos=({int(p[0])}, {int(p[1])}, {int(p[2])}) yaw={yaw} pitch={pitch}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
