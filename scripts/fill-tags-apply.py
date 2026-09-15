#!/usr/bin/env python3
"""Fold an in-game fill-tag pick session into tests/freecam/fill-tags.verdicts
and regenerate games/spyro-1/renderer/terrain_fill_tags.h.

The session file is written by the P-key inspector (core/renderer/fillpick.c);
each line is `FILL|NOFILL <level> <sector> <face>`. Membership in the verdicts
file IS the verdict, so the two cases are add and remove: FILL appends a row
(provenance `pick`, need-px 0 — the picker judges from the live scene, not from
a coverage measurement) if the face has none, and NOFILL deletes the face's row
if it has one. Either is a no-op when the file already agrees.

    scripts/fill-tags-apply.py fill-tags.session          # apply + regenerate
    scripts/fill-tags-apply.py fill-tags.session --dry-run

Verdicts stay the single frozen source of truth: nothing here writes the
generated header directly, and every change lands as a reviewable diff.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERDICTS = os.path.join(ROOT, "tests/freecam/fill-tags.verdicts")
GEN = os.path.join(ROOT, "scripts/gen-fill-tags.py")

ROW = re.compile(r"^FILL\s+(\S+)\s+(\d+)\s+(\d+)\b")


def read_session(path):
    """Session verdicts, last write per face wins."""
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^(FILL|NOFILL)\s+(\S+)\s+(\d+)\s+(\d+)\s*$", line)
            if not m:
                print(f"skipping unparsable session line: {line}", file=sys.stderr)
                continue
            out[(m.group(2), int(m.group(3)), int(m.group(4)))] = m.group(1)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("session", help="fill-tags.session from the in-game inspector")
    ap.add_argument("--dry-run", action="store_true",
                    help="report the changes without writing anything")
    ap.add_argument("--provenance", default="pick",
                    help="provenance tag for newly added rows (default: pick)")
    args = ap.parse_args()

    session = read_session(args.session)
    if not session:
        print("session is empty — nothing to apply")
        return 0

    lines = open(VERDICTS).read().splitlines(keepends=True)
    seen, removed, unchanged = set(), [], []
    kept = []

    for line in lines:
        m = ROW.match(line)
        if not m:
            kept.append(line)
            continue
        key = (m.group(1), int(m.group(2)), int(m.group(3)))
        seen.add(key)
        if session.get(key) == "NOFILL":
            removed.append(key)          # drop the row: absence is the verdict
            continue
        if key in session:
            unchanged.append(key)
        kept.append(line)
    lines = kept

    added, noop = [], []
    for key, want in session.items():
        if key in seen:
            continue
        if want == "FILL":
            level, sec, face = key
            added.append(key)
            lines.append(f"FILL   {level} {sec} {face} 0 {args.provenance}\n")
        else:
            noop.append(key)             # already absent

    for key in added:
        print(f"  + {key[0]} {key[1]} {key[2]}")
    for key in removed:
        print(f"  - {key[0]} {key[1]} {key[2]}")
    print(f"{len(added)} added, {len(removed)} removed, "
          f"{len(unchanged) + len(noop)} already agreed")

    if args.dry_run:
        print("--dry-run: nothing written")
        return 0
    if not added and not removed:
        return 0

    with open(VERDICTS, "w") as f:
        f.writelines(lines)
    print(f"wrote {os.path.relpath(VERDICTS, ROOT)}")
    subprocess.check_call([sys.executable, GEN])
    return 0


if __name__ == "__main__":
    sys.exit(main())
