#!/usr/bin/env python3
"""Read a glitch-mark file and print its rings as tables.

The mark (the glitch_mark binding, or OPENPETE_GLITCH_MARK_AT) writes the
last N presents and the last N terrain walks as text. This prints them, and
flags the two shapes a one-present artifact takes:

  SWING   the row's terrain triangle count differs from BOTH neighbours by
          more than --swing, and the neighbours agree with each other within
          that bound. That is a value that left and came back: one present
          drawn from geometry its neighbours did not have.
  STAMP-BACK
          the scene stamp went backwards: this present composed an older
          terrain build than the present before it. The stamp advances on
          almost every present, so only a backwards step is informative.
  GAP     the serial or ordinal skipped, so the rows either side are not
          neighbours in time.

Both rings are checked: a headless run composes no presents, so there the
walk ring is the only one with rows.

Usage: tools/glitch_mark_read.py <file> [--swing N] [--quiet] [--flags-only]
Exit:  0 read, 1 unreadable, 2 flags found (with --flags-only).
"""

import argparse
import re
import sys

FIELD = re.compile(r"(\w+)=(\([^)]*\)|\S+)")


def parse(path):
    head, presents, walks = {}, [], []
    lost_p, lost_w = [], []
    with open(path, "r", encoding="utf-8", errors="replace") as fp:
        for line in fp:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            tag, _, rest = line.partition(" ")
            row = dict(FIELD.findall(rest))
            if tag == "mark":
                head = row
            elif tag == "P":
                # "P lost serial=N": the ring recycled that slot before the
                # mark was serviced. Carried through as a row so a gap in the
                # present stream is visible rather than silently closed up.
                (presents if "serial" in row and "tick" in row else lost_p).append(row)
            elif tag == "W":
                (walks if "ord" in row and "tick" in row else lost_w).append(row)
    return head, presents, walks, lost_p, lost_w


def num(row, key, default=0):
    v = row.get(key, default)
    try:
        return int(v)
    except (TypeError, ValueError):
        try:
            return float(v)
        except (TypeError, ValueError):
            return default


def flag_rows(rows, seq_key, tri_key, stamp_key, swing):
    """Return one flag string per row, parallel to `rows`."""
    out = []
    for i, r in enumerate(rows):
        f = []
        prev = rows[i - 1] if i > 0 else None
        nxt = rows[i + 1] if i + 1 < len(rows) else None
        if prev is not None and num(r, seq_key) != num(prev, seq_key) + 1:
            f.append("GAP")
        if prev is not None and nxt is not None:
            a, b, c = num(prev, tri_key), num(r, tri_key), num(nxt, tri_key)
            if abs(b - a) > swing and abs(b - c) > swing and abs(a - c) <= swing:
                f.append("SWING%+d" % (b - (a + c) // 2))
        if stamp_key and prev is not None:
            # The stamp advances on nearly every present, so a change is the
            # normal case and says nothing. A stamp that goes BACKWARDS is
            # the anomaly: that present composed an older terrain build than
            # the present before it.
            try:
                a = int(r.get(stamp_key, "0"), 16)
                b = int(prev.get(stamp_key, "0"), 16)
                if a < b:
                    f.append("STAMP-BACK%d" % (a - b))
            except ValueError:
                pass
        out.append(",".join(f))
    return out


def table(title, rows, cols, flags):
    print("== %s (%d row%s) ==" % (title, len(rows), "" if len(rows) == 1 else "s"))
    if not rows:
        return
    hdr = [c[0] for c in cols] + ["flags"]
    body = [[str(r.get(c[1], "-")) for c in cols] + [flags[i]]
            for i, r in enumerate(rows)]
    w = [max(len(hdr[j]), max((len(b[j]) for b in body), default=0))
         for j in range(len(hdr))]
    print("  ".join(h.ljust(w[j]) for j, h in enumerate(hdr)).rstrip())
    for b in body:
        print("  ".join(v.ljust(w[j]) for j, v in enumerate(b)).rstrip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--swing", type=int, default=64,
                    help="triangle-count difference from both neighbours that "
                         "counts as a swing (default 64)")
    ap.add_argument("--flags-only", action="store_true",
                    help="print only flagged rows; exit 2 when any was found")
    ap.add_argument("--quiet", action="store_true", help="summary only")
    a = ap.parse_args()

    try:
        head, presents, walks, lost_p, lost_w = parse(a.file)
    except OSError as e:
        print("cannot read %s: %s" % (a.file, e), file=sys.stderr)
        return 1

    pflags = flag_rows(presents, "serial", "wTris", "stamp", a.swing)
    wflags = flag_rows(walks, "ord", "tris", None, a.swing)

    if head:
        print("mark: " + " ".join("%s=%s" % kv for kv in head.items()))

    if not a.quiet:
        pshow, pf = presents, pflags
        wshow, wf = walks, wflags
        if a.flags_only:
            pshow, pf = zip(*[(r, f) for r, f in zip(presents, pflags) if f]) \
                if any(pflags) else ([], [])
            wshow, wf = zip(*[(r, f) for r, f in zip(walks, wflags) if f]) \
                if any(wflags) else ([], [])
        table("presents", list(pshow),
              [("serial", "serial"), ("tick", "tick"), ("kind", "kind"),
               ("shows", "shows"), ("alpha", "alpha"), ("stamp", "stamp"),
               ("tris", "tris"), ("tc", "tercache"), ("age", "gridage"),
               ("cam", "cam"), ("yaw", "yaw"), ("pitch", "pitch"),
               ("wOrd", "wOrd"), ("wSec", "wSectors"), ("wTris", "wTris"),
               ("wNear", "wNearFaces"), ("wOccl", "wOccl")], list(pf))
        table("walks", list(wshow),
              [("ord", "ord"), ("tick", "tick"), ("sub", "sub"),
               ("sectors", "sectors"), ("tris", "tris"),
               ("nearFaces", "nearFaces"), ("nearTris", "nearTris"),
               ("occl", "occl")], list(wf))

    np = sum(1 for f in pflags if f)
    nw = sum(1 for f in wflags if f)
    print("flagged: %d present row(s), %d walk row(s) (swing threshold %d tris)"
          % (np, nw, a.swing))
    if lost_p or lost_w:
        print("lost to ring recycling: %d present row(s), %d walk row(s)"
              % (len(lost_p), len(lost_w)))
    if a.flags_only and (np or nw):
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
