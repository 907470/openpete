#!/usr/bin/env python3
"""Pair verdicts of the cross-pipeline contraction checks from the per-compile
features: (a) whether one compiler contracts the unqualified multiply-adds of
one source differently across compilations, (b) whether the `precise`
position chain lowers to the same sequence across pipelines.

Usage: calib_pairs.py <record-dir>
Reads <dir>/llpc/calib.tsv, <dir>/radv/gpl/calib.tsv, <dir>/anv/gpl/calib.tsv.
For each compiler and variant it compares the pairs of (b):
  DR1 vs DR0 of the pre-raster library / vertex-only compile (one SPIR-V,
  spec constant DEPTH_REVERSED only), library vs LTO world (fast link vs
  LTO), LTO world vs LTO key (world vs key pipeline of one variant);
and for (a)/(b) across variants, plain (terrain) vs near (terrain_near).
A pair is SAME when the contraction-bearing features are equal: the opcode
defining every mac_of dividend, the f64 division form per reciprocal, the f32
reciprocal consumers' form, the clip_of constant-arm form, and the f32 and f64
fma counts. mul/add count deltas with equal fma counts are dead-output
elimination, not contraction, and are printed beside the verdict.
"""
import csv, os, re, sys
d = sys.argv[1]

def load(p):
    if not os.path.exists(p):
        return {}
    with open(p) as fh:
        return {os.path.basename(os.path.dirname(r["file"])) if r["file"].endswith("isa.txt") else
                os.path.basename(r["file"]): r for r in csv.DictReader(fh, delimiter="\t")}

def norm_div(s):
    return re.sub(r"=\d+", "=n", re.sub(r",per_rcp=[0-9.]+", "", s))

KEYS = ("mac_def_form", "div_form", "clip", "fma32", "fma64")

def feat(r):
    mac = ";".join(sorted(k.split("=")[0] for k in r["mac_def"].split(";") if k))
    # clip_of's constant arm only: add1 alone is op_scene_clip's zc = vzo - 1.0
    # under DEPTH_REVERSED=0, a spec-constant difference, not clip_of
    clip = ";".join(k for k in r["clip"].split(";") if not k.startswith("add1"))
    return dict(mac_def_form=mac, div_form=norm_div(r["div64_shape"]), clip=clip,
                fma32=r["fma32"], fma64=r["fma64"], mul64=r["mul64"], add64=r["add64"])

def cmp(tag, a, b, keys=KEYS):
    if a is None or b is None:
        print(f"{tag}\tMISSING"); return
    fa, fb = feat(a), feat(b)
    diff = [k for k in keys if fa[k] != fb[k]]
    dce = f"dmul64={int(fb['mul64']) - int(fa['mul64'])} dadd64={int(fb['add64']) - int(fa['add64'])}"
    print(f"{tag}\t{'SAME' if not diff else 'DIFF ' + ','.join(diff)}\t{dce}\t"
          + " | ".join(f"{k}:{fa[k]}" + ("" if fa[k] == fb[k] else f"->{fb[k]}") for k in keys))
XV = ("mac_def_form", "div_form", "clip", "fma32")   # across variants: different code, so no fma64 total

def pick(rows, pat):
    m = [r for k, r in rows.items() if re.search(pat, k)]
    return m[0] if m else None

V = ["terrain", "terrain_anim", "terrain_near", "terrain_cells_near", "terrain_cells_anim"]
llpc = load(f"{d}/llpc/calib.tsv")
for gfx in ("gfx1100", "gfx1200"):
    for v in V:
        g = lambda c: llpc.get(f"{v}__{gfx}__{c}")
        cmp(f"amdllpc {gfx} {v} vsonly DR1 vs DR0", g("vsonly_r1_a0"), g("vsonly_r0_a0"))
        cmp(f"amdllpc {gfx} {v} vsonly vs whole-pipeline world (fast link vs LTO)", g("vsonly_r1_a0"), g("world_r1_a0_lto"))
        cmp(f"amdllpc {gfx} {v} world vs key", g("world_r1_a0_lto"), g("key_r0_a0_lto"))
    for v in V[1:]:
        cmp(f"amdllpc {gfx} terrain vs {v} (world)", llpc.get(f"terrain__{gfx}__world_r1_a0_lto"), llpc.get(f"{v}__{gfx}__world_r1_a0_lto"), XV)
for drv in ("radv", "anv"):
    rows = load(f"{d}/{drv}/gpl/calib.tsv")
    if not rows:
        continue
    for v in V:
        lib1 = pick(rows, rf"_lib_preraster_{v}_01-10-71_"); lib0 = pick(rows, rf"_lib_preraster_{v}_00-10-71_")
        world = pick(rows, rf"_link_lto_{v}_01-10-71_terrain_vs"); key = pick(rows, rf"_link_lto_{v}_00-10-71_terrain_key_cache_vs")
        cmp(f"{drv} {v} lib DR1 vs DR0", lib1, lib0)
        cmp(f"{drv} {v} lib vs LTO world (fast link vs LTO)", lib1, world)
        cmp(f"{drv} {v} LTO world vs LTO key", world, key)
    base = pick(rows, r"_link_lto_terrain_01-10-71_terrain_vs")
    for v in V[1:]:
        cmp(f"{drv} terrain vs {v} (LTO world)", base, pick(rows, rf"_link_lto_{v}_01-10-71_terrain_vs"), XV)
mono = load(f"{d}/radv/mono/calib.tsv"); gpl = load(f"{d}/radv/gpl/calib.tsv")
if mono:
    for v in V:
        cmp(f"radv {v} monolithic vs LTO world", pick(mono, rf"_pipeline_{v}_01-10-71_terrain_vs"),
            pick(gpl, rf"_link_lto_{v}_01-10-71_terrain_vs"))
