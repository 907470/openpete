"""paths - the extract-paths stage: credits camera paths from the disc,
edited for the live level and smoothed, as --freecam-path files.

For each scene: the disc keys (tools/extract_s3_credits_paths.py, one
waypoint per key, ticks=1; a scene row with signed_positions reads x, y, z
as s16), then the table's edits: the scene's offset added to x, y, z, the last key
repeated once unless the path already ends on a repeated key
(hold_last_key); written to <out>/raw/scene_NN.path. The smoothed path
(campath.smooth_file with the [smooth] parameters) is <out>/scene_NN.path,
the one the render stage flies. <out>/manifest.json records the disc key
count of each scene, which fixes the scene's length (2N presents) in post.
"""
import json
import os
import sys
from pathlib import Path

import campath
import config

sys.path.insert(0, os.path.join(config.ROOT, "tools"))
import extract_s3_credits_paths as xp  # noqa: E402


def edited_keys(keys, scene, pcfg):
    """Return the waypoint list for one scene after the table's edits."""
    dx, dy, dz = scene["offset"]
    out = [(x + dx, y + dy, z + dz, r, p, yw) for x, y, z, r, p, yw in keys]
    if pcfg.get("hold_last_key") and out[-1] != out[-2]:
        out.append(out[-1])
    return out


def write_atomic(path, text):
    """Write text to path through a temporary file renamed into place."""
    tmp = path + ".part"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def run(cfg, disc, out_dir, scenes=None):
    """Extract, edit and smooth the paths of `scenes` (default all) into out_dir."""
    pcfg, scfg = cfg["paths"], cfg["smooth"]
    raw_dir = os.path.join(out_dir, "raw")
    os.makedirs(raw_dir, exist_ok=True)
    d = xp.Disc(xp.resolve_bin(Path(disc)))
    wad_lba, _ = xp.iso_find(d, "WAD.WAD")
    hdr = d.read_span(wad_lba, 0, pcfg["wad_header"])
    mpath = config.manifest_path(out_dir)
    manifest = {"game": cfg["game"], "keys": [None] * pcfg["scenes"]}
    if os.path.exists(mpath):
        with open(mpath) as f:
            manifest["keys"] = json.load(f)["keys"]
    for sc in cfg["scene"]:
        i = sc["index"]
        if scenes and i not in scenes:
            continue
        keys = xp.scene_keys(d, wad_lba, hdr, i, pcfg["first_entry"], sc.get("signed_positions", False))
        manifest["keys"][i] = len(keys)
        wps = edited_keys(keys, sc, pcfg)
        raw = os.path.join(raw_dir, f"scene_{i:02d}.path")
        write_atomic(raw, xp.path_spec(wps, 1, True) + "\n")
        smooth = os.path.join(out_dir, f"scene_{i:02d}.path")
        if os.path.exists(smooth):
            os.unlink(smooth)
        print(f"scene {i:2d} {sc['level']}: {len(keys)} disc keys, {len(wps)} waypoints; "
              + campath.smooth_file(raw, smooth, scfg["q_pos"], scfg["q_ang"], scfg["tol_pos"],
                                    scfg["tol_ang"], scfg["order"]))
    write_atomic(mpath, json.dumps(manifest) + "\n")
