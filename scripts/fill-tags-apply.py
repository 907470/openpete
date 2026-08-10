#!/usr/bin/env python3
"""Fold an in-game fill-tag pick session into tests/freecam/fill-tags.verdicts
and regenerate games/spyro-1/features/terrain_fill_tags.h.

The session file is written by the P-key inspector (core/runtime/fillpick.c);
each line is `FILL|NOFILL <level> <sector> <face>`. A row the verdicts file
already carries has its verdict rewritten in place, keeping the measured
need-px and the provenance that earned it; a row it does not carry is appended
with `pick:` provenance and need-px 0 (the picker judges from the live scene,
not from a coverage measurement, so there is no pixel count to record).

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

ROW = re.compile(r"^(FILL|NOFILL)\s+(\S+)\s+(\d+)\s+(\d+)(?:\s+(\d+)\s+(\S+))?\s*$")


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
    seen, changed, unchanged = set(), [], []

    for i, line in enumerate(lines):
        m = ROW.match(line)
        if not m:
            continue
        key = (m.group(2), int(m.group(3)), int(m.group(4)))
        if key not in session:
            continue
        seen.add(key)
        want = session[key]
        if m.group(1) == want:
            unchanged.append((key, want))
            continue
        # Rewrite only the verdict column; the measurement columns stand.
        rest = line[len(m.group(1)):].lstrip()
        lines[i] = f"{want:<6} {rest}"
        changed.append((key, m.group(1), want))

    added = []
    for key, want in session.items():
        if key in seen:
            continue
        level, sec, face = key
        added.append((key, want))
        lines.append(f"{want:<6} {level} {sec} {face} 0 {args.provenance}\n")

    for key, was, now in changed:
        print(f"  {key[0]} {key[1]} {key[2]}: {was} -> {now}")
    for key, want in added:
        print(f"  {key[0]} {key[1]} {key[2]}: (new) -> {want}")
    print(f"{len(changed)} changed, {len(added)} added, "
          f"{len(unchanged)} already agreed")

    if args.dry_run:
        print("--dry-run: nothing written")
        return 0
    if not changed and not added:
        return 0

    with open(VERDICTS, "w") as f:
        f.writelines(lines)
    print(f"wrote {os.path.relpath(VERDICTS, ROOT)}")
    subprocess.check_call([sys.executable, GEN])
    return 0


if __name__ == "__main__":
    sys.exit(main())
