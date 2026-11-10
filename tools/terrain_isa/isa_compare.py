#!/usr/bin/env python3
"""Compare two ISA records (isa_table.tsv of each) row by row.

Usage: isa_compare.py <record-a> <record-b> <out-dir> [--strip-suffix SUFFIX]

Joins <record-a>/isa_table.tsv and <record-b>/isa_table.tsv on (variant, DR,
AU, compiler, form, frag) and writes:
  <out-dir>/compare.tsv  one row per key: instructions of a and b, the ratio
                         b/a, VGPR, SGPR, spills, scratch bytes and notes of
                         both; a key present in one record only reads MISSING
  <out-dir>/summary.tsv  per compiler x form x variant, the range of each
                         column over DR, AU and frag, printed too
Columns are named *64 for a and *32 for b, the fp64 and fp32 records the tool
was written for. --strip-suffix (default _fp32) is removed from variant names
of both records before the join, so a variant built under a suffixed name
joins its base row.
"""
import argparse, collections, csv, os, re

ap = argparse.ArgumentParser(description="join two ISA records")
ap.add_argument("a"); ap.add_argument("b"); ap.add_argument("out")
ap.add_argument("--strip-suffix", default="_fp32")
args = ap.parse_args()
os.makedirs(args.out, exist_ok=True)
suf = re.compile(re.escape(args.strip_suffix) + "$") if args.strip_suffix else None


def load(p):
    out = {}
    for r in csv.DictReader(open(p), delimiter="\t"):
        v = suf.sub("", r["variant"]) if suf else r["variant"]
        out[(v, r["DR"], r["AU"], r["compiler"], r["form"], r["frag"])] = r
    return out


a, b = load(f"{args.a}/isa_table.tsv"), load(f"{args.b}/isa_table.tsv")


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


hdr = ["variant", "DR", "AU", "compiler", "form", "frag", "insts64", "insts32", "ratio", "vgpr64", "vgpr32",
       "sgpr64", "sgpr32", "spills64", "spills32", "scratchB64", "scratchB32", "note64", "note32"]
rows = []
for k in sorted(set(a) | set(b)):
    x, y = a.get(k, {}), b.get(k, {})
    i64, i32 = num(x.get("instructions")), num(y.get("instructions"))
    rows.append(list(k) + [x.get("instructions", "MISSING"), y.get("instructions", "MISSING"),
                f"{i32/i64:.3f}" if i64 and i32 else "-", x.get("VGPR", ""), y.get("VGPR", ""),
                x.get("SGPR", ""), y.get("SGPR", ""), x.get("spills", ""), y.get("spills", ""),
                x.get("scratch_bytes", ""), y.get("scratch_bytes", ""), x.get("note", ""), y.get("note", "")])
with open(f"{args.out}/compare.tsv", "w") as fh:
    fh.write("\t".join(hdr) + "\n")
    for r in rows:
        fh.write("\t".join(str(c) for c in r) + "\n")
print(f"{len(rows)} rows -> {args.out}/compare.tsv")

g = collections.defaultdict(list)
for r in csv.DictReader(open(f"{args.out}/compare.tsv"), delimiter="\t"):
    g[(r["compiler"], r["form"], r["variant"])].append(r)
def rng(rows, k):
    vals = sorted({r[k] for r in rows if r[k] not in ("", "-", "MISSING")}, key=lambda x: (len(x), x))
    if not vals: return "?"
    return vals[0] if len(vals) == 1 else f"{vals[0]}..{vals[-1]}"
def ratio(rows):
    rs = [float(r["ratio"]) for r in rows if r["ratio"] not in ("-", "")]
    return f"{min(rs):.2f}" if rs and min(rs) == max(rs) else (f"{min(rs):.2f}-{max(rs):.2f}" if rs else "?")
out = []
for (c, f, v), rows in sorted(g.items()):
    miss = sum(1 for r in rows if "MISSING" in (r["insts64"], r["insts32"]))
    out.append([c, f, v, str(len(rows)), str(miss), rng(rows, "insts64"), rng(rows, "insts32"), ratio(rows),
                rng(rows, "vgpr64"), rng(rows, "vgpr32"), rng(rows, "sgpr64"), rng(rows, "sgpr32"),
                rng(rows, "spills64"), rng(rows, "spills32"), rng(rows, "scratchB64"), rng(rows, "scratchB32")])
hdr = ["compiler","form","variant","n","missing","insts64","insts32","ratio","reg64","reg32","sgpr64","sgpr32","spill64","spill32","scrB64","scrB32"]
with open(f"{args.out}/summary.tsv", "w") as fh:
    fh.write("\t".join(hdr)+"\n")
    for r in out: fh.write("\t".join(r)+"\n")
print("\t".join(hdr))
for r in out: print("\t".join(r))
