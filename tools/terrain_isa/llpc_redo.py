#!/usr/bin/env python3
"""Rerun the failed or empty amdllpc jobs of one llpc record directory and
rebuild its llpc.tsv from every configuration's saved notes and ISA.

Usage: llpc_redo.py <spv-dir> <llpc-dir> --rga DIR [--work-dir DIR]

A configuration directory `<variant>__<gfxNNNN>__<config>` counts as done when
its isa.txt and notes.txt exist and the ISA holds at least one pre-raster
instruction; any other one is recompiled, up to three attempts. amdllpc under
WSL interop fails a job now and then with no diagnostic; the rerun is how
those rows are recovered.
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import llpc_record as L


def load(out, tag):
    """The stats of one saved configuration, or None when it is absent or empty."""
    d = f"{out}/{tag}"
    if not (os.path.exists(f"{d}/isa.txt") and os.path.exists(f"{d}/notes.txt")):
        return None
    s = L.stats(open(f"{d}/notes.txt").read(), open(f"{d}/isa.txt").read())
    return s if s.get("insts") else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("spvdir"); ap.add_argument("outdir")
    L.rga_args(ap)
    a = ap.parse_args()
    if not a.rga:
        ap.error("--rga DIR (or RGA_DIR) is required")
    spv, out = os.path.abspath(a.spvdir), os.path.abspath(a.outdir)
    if not os.path.isdir(out):
        ap.error(f"{out}: no llpc record directory")
    L.setup(a.rga, a.work_dir)
    gfxr = {v: k for k, v in L.MCPU.items()}
    cfgs = {c[0]: c for c in L.configs()}
    rows = []
    for tag in sorted(os.listdir(out)):
        if "__" not in tag:
            continue
        v, g, c = tag.split("__")
        s = load(out, tag)
        for _ in range(3):
            if s:
                break
            print("redo", tag, flush=True)
            L.template(v, spv)
            _, s2 = L.job(v, gfxr[g], cfgs[c], spv, out)
            s = load(out, tag) if s2.get("rc") == 0 else None
        rows.append((tag, dict(s, rc=0) if s else dict(rc=1, err="failed after retries")))
    L.write_tsv(out, rows)
    print(len(rows), "rows")


if __name__ == "__main__":
    main()
