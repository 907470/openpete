#!/usr/bin/env python3
"""classify_hash_divergence.py - given two per-region hash CSVs from
the determinism harness, find every divergent region and look up the
channel that owns it in the game's channel registry. Turns a raw
"hashes differ" signal into "channel X (status=open/gated) diverged at
tick T via region R".

CSV format:  frame,vbl,fnv1a64,<region_name_0>,<region_name_1>,...

Verdicts (the last stdout line is `VERDICT <verdict> rows=<N>`, N the
number of rows compared):
  CLEAN       every row of both logs is identical.
  EXPECTED    every divergent region belongs to a channel whose status
              is `open`.
  REGRESSION  a divergent region belongs to a gated, verified or
              audited_clean channel, to a channel with any other status,
              or to no channel (an orphan); or the total hash diverges
              with no region column diverging. A game without a
              registry file has no channels, so each of its divergent
              regions is an orphan.
  BROKEN      the logs cannot be compared: a file is missing, empty or
              has no data rows, the headers differ, the row counts
              differ, or a region column names no region of the game's
              gameplay_state_regions.toml.

Exit codes:
   0 - CLEAN
   1 - EXPECTED or REGRESSION (classification printed to stdout)
   2 - BROKEN
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

REGRESSION_STATUSES = ("gated", "verified", "audited_clean")


class Broken(Exception):
    """The two logs cannot be compared; the message names the reason."""


def load_csv(path: Path) -> tuple[list[str], list[list[str]]]:
    """Read a hash log; raise Broken when it is missing or empty."""
    if not path.is_file():
        raise Broken(f"{path}: missing")
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    if not rows:
        raise Broken(f"{path}: empty")
    return rows[0], rows[1:]


def load_region_names(path: Path) -> set[str]:
    """Return the `name` of every [[region]] row of a state-region manifest."""
    if not path.is_file():
        raise Broken(f"state-region manifest not found: {path}")
    with open(path, "rb") as f:
        data = tomllib.load(f)
    return {r["name"] for r in data.get("region", []) if "name" in r}


def build_region_to_channel(registry_path: Path) -> dict[str, dict]:
    """Map each state-region name to the channel that lists it; {} when the
    game has no registry file."""
    if not registry_path.is_file():
        return {}
    with open(registry_path, "rb") as f:
        data = tomllib.load(f)
    region_to_channel: dict[str, dict] = {}
    for ch in data.get("channel", []):
        for region in ch.get("state_regions") or []:
            region_to_channel[region] = ch
    return region_to_channel


def classify(args: argparse.Namespace) -> tuple[str, int]:
    """Compare the two logs, print the per-channel report, and return
    (verdict, rows compared). Raises Broken."""
    base_hdr, base_rows = load_csv(Path(args.baseline))
    cand_hdr, cand_rows = load_csv(Path(args.candidate))

    if base_hdr != cand_hdr:
        raise Broken(f"header mismatch: baseline={base_hdr[:5]}... "
                     f"candidate={cand_hdr[:5]}...")
    if base_hdr[:3] != ["frame", "vbl", "fnv1a64"]:
        raise Broken(f"not a gameplay hash log header: {base_hdr[:3]}")
    if len(base_rows) != len(cand_rows):
        raise Broken(f"row counts differ (baseline={len(base_rows)}, "
                     f"candidate={len(cand_rows)})")
    if not base_rows:
        raise Broken("no data rows: nothing compared")

    # The column names must be regions of the game named by --game, so a
    # log from another game, or a manifest the binary was not built from,
    # is refused instead of classified against the wrong registry.
    region_names = base_hdr[3:]
    known = load_region_names(Path(args.state_regions))
    foreign = [r for r in region_names if r not in known]
    if foreign:
        raise Broken(f"{len(foreign)} column(s) name no region of "
                     f"{args.state_regions}: {foreign[:5]}")

    region_to_channel = build_region_to_channel(Path(args.registry))
    n = len(base_rows)

    # Every region that diverges on any row, with the tick it first does;
    # a later divergence in a gated channel is not hidden behind an earlier
    # one in an open channel.
    first_div_frame: int | None = None
    total_only_frame: int | None = None
    region_first: dict[int, int] = {}
    for i in range(n):
        if base_rows[i] == cand_rows[i]:
            continue
        frame = int(base_rows[i][0])
        if first_div_frame is None:
            first_div_frame = frame
        any_region = False
        for col in range(3, len(base_hdr)):
            if base_rows[i][col] != cand_rows[i][col]:
                region_first.setdefault(col - 3, frame)
                any_region = True
        if not any_region and base_rows[i][2] != cand_rows[i][2] \
                and total_only_frame is None:
            total_only_frame = frame

    if first_div_frame is None:
        print(f"MATCH bit-identical across {n} logged rows")
        return "CLEAN", n

    by_channel: dict[str, tuple[dict, list[str]]] = {}
    unowned: list[str] = []
    for idx in sorted(region_first, key=lambda k: (region_first[k], k)):
        rname = region_names[idx]
        label = f"{rname}@{region_first[idx]}"
        ch = region_to_channel.get(rname)
        if ch:
            by_channel.setdefault(ch["name"], (ch, []))[1].append(label)
        else:
            unowned.append(label)

    regression = False
    print(f"DIVERGE first at frame {first_div_frame} "
          f"({len(region_first)} region(s) over {n} rows)")
    for ch_name, (ch, regions) in by_channel.items():
        status = ch.get("status", "?")
        if status == "open":
            tag = "expected"
        else:
            tag = "REGRESSION"
            regression = True
        print(f"  channel {ch_name!r} (status={status}, {tag}): "
              f"{', '.join(regions)}")
    for label in unowned:
        regression = True
        print(f"  region {label!r}: REGRESSION - no channel in "
              f"{args.registry} claims this region (orphan)")
    if total_only_frame is not None:
        regression = True
        print(f"  frame {total_only_frame}: REGRESSION - total hash "
              "diverges but no region column does")
    return ("REGRESSION" if regression else "EXPECTED"), n


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("baseline", help="hash CSV from the canonical run")
    ap.add_argument("candidate", help="hash CSV from the run to classify")
    ap.add_argument("--game", default="spyro-1",
                    help="game slug; selects the manifests below")
    ap.add_argument("--registry", default=None,
                    help="render_channels.toml for region->channel lookup "
                         "(default games/<game>/manifest/render_channels.toml;"
                         " absent = no channels)")
    ap.add_argument("--state-regions", default=None,
                    help="gameplay_state_regions.toml the log columns must "
                         "name (default games/<game>/manifest/"
                         "gameplay_state_regions.toml)")
    args = ap.parse_args()
    manifest = Path("games") / args.game / "manifest"
    if args.registry is None:
        args.registry = str(manifest / "render_channels.toml")
    if args.state_regions is None:
        args.state_regions = str(manifest / "gameplay_state_regions.toml")

    try:
        verdict, rows = classify(args)
    except Broken as e:
        print(f"BROKEN {e}")
        print("VERDICT BROKEN rows=0")
        return 2
    print(f"VERDICT {verdict} rows={rows}")
    return 0 if verdict == "CLEAN" else 1


if __name__ == "__main__":
    sys.exit(main())
