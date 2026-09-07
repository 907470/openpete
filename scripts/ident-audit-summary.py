#!/usr/bin/env python3
"""ident-audit-summary.py — corpus summary over ident-audit.sh's per-row TSVs.

One row per bk2: how many ticks were audited, how many were pixel-exact, the
median and worst differing-pixel counts, and the VBL the worst tick landed on.
Rows are sorted by their worst tick, so the scene with the largest residual is
read off the top. The last line is the corpus-wide exact-tick fraction — the
single number that says how much of the spine is an identity.

The argument is either a directory of TSVs or a file listing the TSV paths to
report. ident-audit.sh passes the list, so a filtered run reports the rows it
actually ran rather than everything the output directory has accumulated.
"""
import sys
import os
import glob


def read_rows(path):
    out = []
    with open(path) as f:
        next(f, None)                      # header
        for line in f:
            parts = line.split()
            if len(parts) == 4:
                out.append(tuple(int(p) for p in parts))
    return out


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "build/ident-audit"
    if os.path.isdir(arg):
        files = sorted(glob.glob(os.path.join(arg, "*.tsv")))
    else:
        with open(arg) as f:
            files = sorted({l.strip() for l in f if l.strip()})
        files = [p for p in files if os.path.exists(p)]
    if not files:
        print(f"[ident-audit] no TSVs for {arg}")
        return 1

    summary, total, total_exact = [], 0, 0
    for path in files:
        rows = read_rows(path)
        if not rows:
            continue
        name = os.path.basename(path)[:-4]
        px = sorted(r[2] for r in rows)
        exact = sum(1 for p in px if p == 0)
        worst = max(rows, key=lambda r: r[2])
        summary.append((worst[2], name, len(rows), exact,
                        px[len(px) // 2], worst[0], worst[3]))
        total += len(rows)
        total_exact += exact

    summary.sort(reverse=True)
    print(f"\n[ident-audit] {len(summary)} rows, {total} ticks audited\n")
    print(f"{'row':<42}{'ticks':>7}{'exact':>7}{'p50':>9}{'worst':>9}"
          f"{'@vbl':>8}{'maxch':>7}")
    for worst_px, name, n, exact, p50, vbl, maxch in summary:
        print(f"{name:<42}{n:>7}{exact:>7}{p50:>9}{worst_px:>9}{vbl:>8}{maxch:>7}")

    frac = 100.0 * total_exact / total if total else 0.0
    print(f"\n[ident-audit] exact ticks: {total_exact}/{total} ({frac:.2f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
