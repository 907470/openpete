#!/usr/bin/env python3
"""check_ds_region_mirror.py — the DuckStation psxtrace capture module
carries a hand-written copy of manifest/gameplay_state_regions.toml
(kRegions in third_party/duckstation/src/duckstation-regtest/
psxtrace_capture.cpp). The oracle compares hashes computed on both
sides, so the two tables must reduce to the same rows in the same order:
kind, start, length, aux, elem_size, name, mask offsets, per-element
flag. This script diffs them.

Exit 0 when identical, 1 on any difference, 64 when the DuckStation
submodule is not checked out (nothing to compare).

The cd_load_transient / gameplay_neutral flags are gate policy applied
on the openpete side only and are not part of the hash; they are not
mirrored and not compared.
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_state_regions import flatten_regions, load_regions  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "manifest" / "gameplay_state_regions.toml"
DS_FILE = (ROOT / "third_party" / "duckstation" / "src" / "duckstation-regtest"
           / "psxtrace_capture.cpp")

ROW_RE = re.compile(
    r"\{\s*(REGION_\w+)\s*,\s*([0-9A-Fa-fx]+)u\s*,\s*([0-9A-Fa-fx]+)u\s*,"
    r"\s*([0-9A-Fa-fx]+)u\s*,\s*([0-9A-Fa-fx]+)u\s*,\s*\"([^\"]+)\"\s*,"
    r"\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", re.S)
MASK_RE = re.compile(
    r"static\s+constexpr\s+uint16_t\s+(\w+)\[\]\s*=\s*\{([^}]*)\}")
COUNT_RE = re.compile(r"std::array<Region,\s*(\d+)>\s+kRegions")


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def parse_ds(src: str):
    src = strip_comments(src)
    masks = {}
    for name, body in MASK_RE.findall(src):
        masks[name] = tuple(int(x.strip(), 0) for x in body.split(",") if x.strip())
    m = COUNT_RE.search(src)
    declared = int(m.group(1)) if m else None
    rows = []
    for kind, start, length, aux, es, name, mask_sym, mcount, per_el in ROW_RE.findall(src):
        if mask_sym == "nullptr":
            mask = ()
        elif mask_sym in masks:
            mask = masks[mask_sym]
        else:
            raise ValueError(f"{name}: unknown mask symbol {mask_sym}")
        if len(mask) != int(mcount):
            raise ValueError(f"{name}: mask_count {mcount} != {len(mask)} entries in {mask_sym}")
        rows.append({
            "kind": kind[len("REGION_"):],
            "start": int(start, 0), "length": int(length, 0),
            "aux": int(aux, 0), "elem_size": int(es, 0),
            "name": name, "mask": mask, "mask_per_element": int(per_el),
        })
    return declared, rows


KEYS = ("kind", "start", "length", "aux", "elem_size", "name", "mask", "mask_per_element")


def fmt(v):
    if isinstance(v, int):
        return f"0x{v:X}"
    if isinstance(v, tuple):
        return "[" + ", ".join(f"0x{x:X}" for x in v) + "]"
    return str(v)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", default=str(MANIFEST))
    ap.add_argument("--ds-file", default=str(DS_FILE))
    args = ap.parse_args()

    ds_path = Path(args.ds_file)
    if not ds_path.is_file():
        print(f"[ds-mirror] SKIP: {ds_path} missing (DuckStation submodule not checked out)")
        return 64

    op_rows = flatten_regions(load_regions(args.manifest))
    declared, ds_rows = parse_ds(ds_path.read_text())

    problems = []
    if declared is not None and declared != len(ds_rows):
        problems.append(f"DS declares std::array<Region, {declared}> but the initializer has {len(ds_rows)} rows")
    if len(op_rows) != len(ds_rows):
        problems.append(f"region count: manifest {len(op_rows)} vs DS {len(ds_rows)}")
    for i, (a, b) in enumerate(zip(op_rows, ds_rows)):
        for k in KEYS:
            if a[k] != b[k]:
                problems.append(f"[{i}] {a['name']}: {k} manifest={fmt(a[k])} DS={fmt(b[k])}")
    for i in range(min(len(op_rows), len(ds_rows)), len(op_rows)):
        problems.append(f"[{i}] {op_rows[i]['name']}: present in manifest, absent in DS")
    for i in range(min(len(op_rows), len(ds_rows)), len(ds_rows)):
        problems.append(f"[{i}] {ds_rows[i]['name']}: present in DS, absent in manifest")

    if problems:
        print(f"[ds-mirror] FAIL: {len(problems)} difference(s) between {args.manifest} and {ds_path}")
        for p in problems:
            print("  " + p)
        return 1
    print(f"[ds-mirror] PASS: {len(op_rows)} regions identical (kind/start/length/aux/elem_size/name/mask)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
