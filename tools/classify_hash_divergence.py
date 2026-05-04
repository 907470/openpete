#!/usr/bin/env python3
"""classify_hash_divergence.py — given two per-region hash CSVs from
the determinism harness, find the first divergent region and look up
the channel that owns it. Turns a raw "hashes differ" signal into
"channel X (status=open/gated) diverged at frame F via region R".

CSV format:  frame,vbl,fnv1a64,<region_name_0>,<region_name_1>,...

Exit codes:
   0 — logs are bit-identical
   1 — logs diverge (classification printed to stdout)
   2 — setup error

See notes/plans/openpete-determinism-enforcement.md Phase 5.
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

try:
    import tomllib
except ImportError:  # pragma: no cover
    import tomli as tomllib  # type: ignore


def load_csv(path: Path) -> tuple[list[str], list[list[str]]]:
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    if not rows:
        raise SystemExit(f"{path}: empty")
    return rows[0], rows[1:]


def build_region_to_channel(registry_path: Path) -> dict[str, dict]:
    if not registry_path.is_file():
        return {}
    with open(registry_path, "rb") as f:
        data = tomllib.load(f)
    region_to_channel: dict[str, dict] = {}
    for ch in data.get("channel", []):
        for region in ch.get("state_regions") or []:
            region_to_channel[region] = ch
    return region_to_channel


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("baseline", help="hash CSV from the canonical run")
    ap.add_argument("candidate", help="hash CSV from the run to classify")
    ap.add_argument(
        "--registry",
        default="manifest/render_channels.toml",
        help="render_channels.toml for region→channel lookup",
    )
    args = ap.parse_args()

    base_hdr, base_rows = load_csv(Path(args.baseline))
    cand_hdr, cand_rows = load_csv(Path(args.candidate))

    if base_hdr != cand_hdr:
        print(f"FAIL header mismatch: baseline={base_hdr[:5]}... "
              f"candidate={cand_hdr[:5]}...", file=sys.stderr)
        return 2

    # First three columns are frame,vbl,fnv1a64; rest are per-region.
    region_names = base_hdr[3:]
    region_to_channel = build_region_to_channel(Path(args.registry))

    n = min(len(base_rows), len(cand_rows))
    if len(base_rows) != len(cand_rows):
        print(f"note: row counts differ (baseline={len(base_rows)}, "
              f"candidate={len(cand_rows)}); comparing {n} common frames",
              file=sys.stderr)

    # Find the first frame where any column diverges.
    first_div_frame: int | None = None
    diverged_region_idx: list[int] = []
    for i in range(n):
        if base_rows[i] == cand_rows[i]:
            continue
        first_div_frame = int(base_rows[i][0])
        # Identify which region columns differ at this frame.
        for col in range(3, len(base_hdr)):
            if base_rows[i][col] != cand_rows[i][col]:
                diverged_region_idx.append(col - 3)
        break

    if first_div_frame is None:
        print("MATCH bit-identical across all logged frames")
        return 0

    # Group divergent regions by their owning channel.
    by_channel: dict[str, tuple[dict, list[str]]] = {}
    unowned: list[str] = []
    for idx in diverged_region_idx:
        rname = region_names[idx]
        ch = region_to_channel.get(rname)
        if ch:
            entry = by_channel.setdefault(ch["name"], (ch, []))
            entry[1].append(rname)
        else:
            unowned.append(rname)

    print(f"DIVERGE first at frame {first_div_frame}")
    for ch_name, (ch, regions) in by_channel.items():
        status = ch.get("status", "?")
        tag = "REGRESSION" if status in ("gated", "verified") else "expected"
        print(f"  channel {ch_name!r} (status={status}, {tag}): "
              f"{', '.join(regions)}")
    for rname in unowned:
        print(f"  region {rname!r}: NO CHANNEL CLAIMS THIS — orphan leak; "
              "add an entry to manifest/render_channels.toml")
    if not diverged_region_idx:
        # Total hash diverged but no per-region column did — header
        # mismatch or stale log.
        print("  (total hash diverges but no per-region column does — "
              "header mismatch?)")

    return 1


if __name__ == "__main__":
    sys.exit(main())
