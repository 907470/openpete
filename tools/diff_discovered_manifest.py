#!/usr/bin/env python3
"""Diagnostic: compare auto-discovered manifest entries (Pass 6) against
the hand-curated native.toml.

For each native TU the rewriter emits ``<stem>.discovered.toml`` listing
[[discovered_dispatch]] / [[discovered_outbound_recomp]] /
[[discovered_outbound_psyq]] entries. This script reports:

- entries the rewriter found that aren't in native.toml (candidates to
  delete from the hand list once we trust auto-discovery);
- entries in native.toml that the rewriter didn't find (hand-list
  workarounds / functions called from MIPS asm but not the native C).

Output is informational only — exit code is always 0. The plan §5
Phase 3 calls this manifest-diff "diagnostic, not pass/fail."

Usage:
    diff_discovered_manifest.py <manifest_dir> <rewritten_dir>
"""

from __future__ import annotations

import sys
from pathlib import Path

import tomli


def _names(entries, key):
    return {e[key] for e in entries if key in e}


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} <manifest_dir> <rewritten_dir>", file=sys.stderr)
        return 2
    manifest_dir = Path(argv[1])
    rewritten_dir = Path(argv[2])

    native_path = manifest_dir / "native.toml"
    if not native_path.exists():
        return 0
    with open(native_path, "rb") as fh:
        native = tomli.load(fh)

    hand_dispatch = _names(native.get("dispatch", []), "name")
    hand_recomp = _names(native.get("outbound_recomp", []), "name")
    hand_psyq = _names(native.get("outbound_psyq", []), "psyq_name")

    # Names listed in the *_skip lists are intentionally hand-suppressed
    # (collisions with hand-written thunks, or names that link straight
    # to PsyCross). Treat them as "covered" so the diff stays clean.
    hand_psyq |= set(native.get("outbound_psyq_skip", []))
    hand_recomp |= set(native.get("outbound_recomp_skip", []))

    auto_dispatch: set[str] = set()
    auto_recomp: set[str] = set()
    auto_psyq: set[str] = set()
    seen_files: list[Path] = []
    for path in sorted(rewritten_dir.glob("*.discovered.toml")):
        seen_files.append(path)
        with open(path, "rb") as fh:
            d = tomli.load(fh)
        for e in d.get("discovered_dispatch", []):
            auto_dispatch.add(e["name"])
        for e in d.get("discovered_outbound_recomp", []):
            auto_recomp.add(e["name"])
        for e in d.get("discovered_outbound_psyq", []):
            auto_psyq.add(e["psyq_name"])

    if not seen_files:
        return 0

    def report(label: str, hand: set, auto: set) -> None:
        only_hand = sorted(hand - auto)
        only_auto = sorted(auto - hand)
        if only_hand or only_auto:
            print(f"manifest-diff [{label}]:")
            for n in only_hand:
                print(f"  hand-only:  {n}")
            for n in only_auto:
                print(f"  auto-only:  {n}")

    print(f"manifest-diff: scanned {len(seen_files)} discovered.toml files")
    report("dispatch", hand_dispatch, auto_dispatch)
    report("outbound_recomp", hand_recomp, auto_recomp)
    report("outbound_psyq", hand_psyq, auto_psyq)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
