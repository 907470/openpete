#!/usr/bin/env python3
"""Split a RADV_DEBUG=shaders,shaderstats openpete log into per-pipeline
records. Usage: radv_split.py <radv-*.err> <out-dir>

A pipe-create log line opens a segment (compiles are serial on one worker,
so a compile's dumps follow its line). For every terrain segment each ACO
"After lowering to hw instructions" program is written to
<out-dir>/<seq>_<kind>_<vs>_<spec>_<fs>_<stage>.aco.gz and its SHADER STATS
to <out-dir>/stats.tsv."""
import gzip, os, re, sys
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
rows = []
seg = None; prog = None; prog_lines = []; pending_stats = None; last_name = None
def flush_prog(name):
    global prog, prog_lines
    if seg and seg["terrain"] and prog is not None:
        st = "vs" if "SW (VS)" in prog else "fs" if "SW (FS)" in prog else "other"
        fn = f"{seg['seq']:04d}_{seg['kind']}_{seg['vs']}_{seg['spec']}_{seg['fs']}_{st}_{len(seg['progs'])}.aco.gz"
        with gzip.open(os.path.join(out, fn), "wt") as fh:
            fh.write("".join(prog_lines))
        seg["progs"].append((st, fn))
    prog = None; prog_lines = []
with open(src, "r", errors="replace") as fh:
    for line in fh:
        m = re.search(r"pipe-create #(\d+) kind=(\S+) site=(\S+)", line)
        if m:
            flush_prog(None)
            vs = re.search(r" vs=([a-z0-9_]+)\.vert", line); fs = re.search(r" fs=([a-z0-9_]+)\.frag", line)
            sp = re.search(r" vs\.spec=(\S+)", line)
            seg = dict(seq=int(m.group(1)), kind=m.group(2), site=m.group(3),
                       vs=vs.group(1) if vs else "-", fs=fs.group(1) if fs else "-",
                       spec=(sp.group(1).replace(":", "").replace(",", "-") if sp else "-"),
                       terrain=("terrain" in line and "site=terrain_cache" in line), progs=[], line=line.strip())
            continue
        if line.startswith("After lowering to hw instructions:"):
            flush_prog(None); prog = ""; prog_lines = []; continue
        if prog is not None:
            if prog == "" and line.startswith("ACO shader stage:"):
                prog = line
            if re.match(r"^[A-Z][A-Za-z ]+Shader[A-Za-z ]*:\s*$", line) or line.startswith("*** SHADER STATS"):
                flush_prog(None)
            else:
                prog_lines.append(line); continue
        if line.startswith("*** SHADER STATS ***"):
            pending_stats = dict(name=last_name); continue
        if pending_stats is not None:
            if line.startswith("****"):
                if seg and seg["terrain"]:
                    rows.append((seg, pending_stats))
                pending_stats = None; continue
            k, _, v = line.partition(":")
            pending_stats[k.strip()] = v.strip(); continue
        if re.match(r"^[A-Z][A-Za-z ]+Shader[A-Za-z ]*:?\s*$", line):
            last_name = line.strip().rstrip(":")
flush_prog(None)
keys = ["SGPRs", "VGPRs", "Spilled SGPRs", "Spilled VGPRs", "Code size", "Scratch size", "Instructions",
        "Copies", "Branches", "Latency", "Inverse Throughput", "VALU", "SALU", "VMEM", "SMEM", "Subgroups per SIMD"]
with open(os.path.join(out, "stats.tsv"), "w") as fh:
    fh.write("seq\tkind\tvs\tspec\tfs\tshader\t" + "\t".join(keys) + "\n")
    for seg, st in rows:
        fh.write("\t".join([str(seg["seq"]), seg["kind"], seg["vs"], seg["spec"], seg["fs"], str(st.get("name"))] +
                           [st.get(k, "") for k in keys]) + "\n")
print(f"{len(rows)} stats rows; files in {out}")
