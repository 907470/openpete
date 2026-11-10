#!/usr/bin/env python3
"""ISA record table for the terrain vertex stages from one record directory.

Usage: isa_table.py <record-dir>   (the directory isa_record.sh writes)
Reads <dir>/llpc/llpc.tsv, <dir>/radv/{gpl,gplaff}/stats.tsv and
<dir>/anv/{gpl,gplaff}/stats.tsv when present, writes <dir>/isa_table.tsv and
prints it. One row per variant x (DEPTH_REVERSED, AFFINE_UV) x compiler/form:
  amdllpc gfxNNNN lto   : vertex + terrain.frag (DR=1) or terrain_key_cache.frag
                          (DR=0, AU=0) compiled as one pipeline
  amdllpc gfxNNNN vsonly: the vertex stage alone (pre-raster library)
  radv gfx1102 lib      : GPL pre-raster library (what a fast link runs)
  radv gfx1102 lto      : GPL LTO link with terrain.frag (DR=1) / terrain_key_cache (DR=0)
  anv xe2 lib / lto     : the same on ANV
"""
import csv, os, sys
d = sys.argv[1]
VARS = ["terrain", "terrain_anim", "terrain_near", "terrain_cells_near", "terrain_cells_anim"]
rows = []

def tsv(p):
    if not os.path.exists(p):
        return []
    with open(p) as fh:
        return list(csv.DictReader(fh, delimiter="\t"))

for r in tsv(f"{d}/llpc/llpc.tsv"):
    c = r["config"]
    if c.startswith("vsonly"):
        form, fs = "vsonly", "-"
    elif c.startswith("key"):
        form, fs = "lto", "key"
    else:
        form, fs = "lto", "world"
    dr = c.split("_r")[1][0]; au = c.split("_a")[1][0] if "_a" in c else "0"
    if r["rc"] != "0":
        rows.append((r["variant"], dr, au, f"amdllpc {r['gfx']}", form, fs, "-", "-", "-", "-", "-", "-",
                     "FAILED: " + r["err"]))
        continue
    rows.append((r["variant"], dr, au, f"amdllpc {r['gfx']}", form, fs, r["insts"], r["vgpr"], r["sgpr"],
                 "n/a", r["scratch_bytes"], r["scratch_ops"], ""))

for drv, label in (("radv", "radv gfx1102"), ("anv", "anv xe2")):
    for arm in ("gpl", "gplaff"):
        seen = set()
        for r in tsv(f"{d}/{drv}/{arm}/stats.tsv"):
            if drv == "radv" and not r.get("shader", "").startswith("Vertex"):
                continue
            kind = r["kind"]
            if kind not in ("lib_preraster", "link_lto"):
                continue
            sp = r["spec"]                      # e.g. 01-10-71 = DR 1, AU 0
            dr = sp.split("-")[0][1]; au = sp.split("-")[1][1]
            fs = {"-": "-", "terrain": "world", "terrain_key_cache": "key"}.get(r["fs"])
            if fs is None:
                continue                         # prepass pipelines: listed in stats.tsv, not here
            key = (r["vs"], dr, au, kind, fs)
            if key in seen:
                continue
            seen.add(key)
            form = "lib" if kind == "lib_preraster" else "lto"
            if drv == "radv":
                rows.append((r["vs"], dr, au, label, form, fs, r["Instructions"], r["VGPRs"], r["SGPRs"],
                             f"{r['Spilled SGPRs']}s/{r['Spilled VGPRs']}v", r["Scratch size"], "-",
                             f"latency {r['Latency']}"))
            else:
                rows.append((r["vs"], dr, au, label, form, fs, r["instructions"], f"GRF {r['grf']}", "-",
                             f"{r['spills']}:{r['fills']}", "-", "-", f"SIMD{r['simd']} cycles {r['cycles']}"))

rows.sort(key=lambda x: (VARS.index(x[0]) if x[0] in VARS else 9, x[1], x[2], x[3], x[4], x[5]))
hdr = ("variant", "DR", "AU", "compiler", "form", "frag", "instructions", "VGPR", "SGPR",
       "spills", "scratch_bytes", "scratch_ops", "note")
with open(f"{d}/isa_table.tsv", "w") as fh:
    fh.write("\t".join(hdr) + "\n")
    for r in rows:
        fh.write("\t".join(str(x) for x in r) + "\n")
print("\t".join(hdr))
for r in rows:
    print("\t".join(str(x) for x in r))
