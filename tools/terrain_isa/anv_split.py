#!/usr/bin/env python3
"""Split an INTEL_DEBUG=vs openpete log (ANV) into per-pipeline vertex-stage
records. Usage: anv_split.py <drv-*.err> <out-dir>

A pipe-create log line opens a segment (one compile worker, so a compile's
dumps follow its line). Each "Native code for ... vertex shader" block of a
terrain segment is written to <out-dir>/<seq>_<kind>_<vs>_<spec>_<fs>_vs_<i>.asm.gz
and its "SIMDn shader: ..." statistics line to <out-dir>/stats.tsv."""
import gzip, os, re, sys
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
STAT = re.compile(r"SIMD(\d+) shader: (\d+) instructions\. (\d+) loops\. (\d+) cycles\. (\d+):(\d+) spills:fills, (\d+) sends.*?GRF registers: (\d+)")
rows = []; seg = None; blk = None; stat = None
def flush():
    global blk, stat
    if blk is not None and seg and seg["terrain"]:
        fn = f"{seg['seq']:04d}_{seg['kind']}_{seg['vs']}_{seg['spec']}_{seg['fs']}_vs_{seg['n']}.asm.gz"
        seg["n"] += 1
        with gzip.open(os.path.join(out, fn), "wt") as fh:
            fh.write("".join(blk))
        rows.append((seg, stat, fn))
    blk = None; stat = None
with open(src, "r", errors="replace") as fh:
    for line in fh:
        m = re.search(r"pipe-create #(\d+) kind=(\S+) site=(\S+)", line)
        if m:
            flush()
            vs = re.search(r" vs=([a-z0-9_]+)\.vert", line); fs = re.search(r" fs=([a-z0-9_]+)\.frag", line)
            sp = re.search(r" vs\.spec=(\S+)", line)
            seg = dict(seq=int(m.group(1)), kind=m.group(2), vs=vs.group(1) if vs else "-",
                       fs=fs.group(1) if fs else "-",
                       spec=(sp.group(1).replace(":", "").replace(",", "-") if sp else "-"),
                       terrain=("site=terrain_cache" in line), n=0)
            continue
        if line.startswith("Native code for"):
            flush()
            blk = [line] if "vertex shader" in line else None
            continue
        if blk is not None:
            if line.startswith("shader: MESA_SHADER") or line.startswith("NIR (") or line.startswith("[rhi/vk]"):
                flush(); continue
            if stat is None:
                m = STAT.search(line)
                if m: stat = m.groups()
            blk.append(line)
flush()
with open(os.path.join(out, "stats.tsv"), "w") as fh:
    fh.write("seq\tkind\tvs\tspec\tfs\tsimd\tinstructions\tloops\tcycles\tspills\tfills\tsends\tgrf\tfile\n")
    for seg, st, fn in rows:
        st = st or ("?",) * 8
        fh.write("\t".join([str(seg["seq"]), seg["kind"], seg["vs"], seg["spec"], seg["fs"], *st, fn]) + "\n")
print(f"{len(rows)} vertex blocks; {out}/stats.tsv")
