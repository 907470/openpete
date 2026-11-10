#!/usr/bin/env python3
"""amdllpc ISA record for the terrain vertex stages.

Usage: llpc_record.py <spv-dir> <out-dir> --rga DIR [--work-dir DIR]
                      [--gfx 11.0.0,12.0.0] [--jobs N]
                      [--variants terrain,...] [--configs world_r1_a0_lto,...]

Per variant and gfxip it compiles, through RGA's packaged amdllpc, one .pipe
per configuration:
  world_r{0,1}_a{0,1}_lto : vertex + terrain.frag, DEPTH_REVERSED/AFFINE_UV
                            spec constants, whole-pipeline compile (unlinked=0)
  key_r0_a0_lto           : vertex + terrain_key_cache.frag, DEPTH_REVERSED=0
  vsonly_r{1,0}_a0        : the vertex stage alone, unlinked=1: a pre-raster
                            library compiled without its fragment stage, the
                            code a GPL fast-link runs (amdllpc's own dump of a
                            VS-only input names it PipelineLibVs)
The .pipe skeleton (resource mapping, vertex input state) is amdllpc's own
dump of an --auto-layout-desc compile of the variant. Writes per config the
ELF, objdump ISA, readobj notes, and one row of <out-dir>/llpc.tsv.

--rga is the Radeon Developer Tool Suite directory (RGA_DIR in the
environment also sets it). The Windows suite (utils/amdllpc.exe) runs through
WSL interop: its work directory must be on a Windows drive, by default
%TEMP%\openpete-terrain-isa resolved with cmd.exe and wslpath. The Linux suite
(utils/amdllpc) runs natively in a work directory under $TMPDIR.
"""
import argparse, concurrent.futures as cf, os, re, shutil, subprocess, sys, tempfile


class Rga:
    """Paths of one RGA install and the work directory amdllpc writes into."""

    def __init__(self, rga, work=None):
        rga = os.path.abspath(rga)
        u = os.path.join(rga, "utils")
        self.windows = os.path.isfile(os.path.join(u, "amdllpc.exe"))
        ext = ".exe" if self.windows else ""
        self.llpc = os.path.join(u, "amdllpc" + ext)
        self.objdump = os.path.join(u, "lc", "opencl", "bin", "llvm-objdump" + ext)
        self.readobj = os.path.join(u, "lc", "opencl", "bin", "llvm-readobj" + ext)
        missing = [p for p in (self.llpc, self.objdump, self.readobj) if not os.path.isfile(p)]
        if missing:
            sys.exit("llpc_record: not an RGA install (missing %s)" % ", ".join(missing))
        if work is None:
            work = self.default_work()
        self.work = os.path.abspath(work)

    def default_work(self):
        """A per-user work directory amdllpc can write to."""
        if not self.windows:
            return os.path.join(tempfile.gettempdir(), "openpete-terrain-isa")
        tmp = subprocess.run(["cmd.exe", "/c", "echo %TEMP%"], capture_output=True, text=True,
                             cwd="/mnt/c").stdout.strip()
        if not tmp:
            sys.exit("llpc_record: cmd.exe printed no %TEMP%; pass --work-dir on a Windows drive")
        return subprocess.run(["wslpath", "-u", tmp], capture_output=True, text=True,
                              check=True).stdout.strip() + "/openpete-terrain-isa"

    def native(self, path):
        """The path as amdllpc sees it: a Windows path for the Windows suite."""
        if not self.windows:
            return path
        return subprocess.run(["wslpath", "-w", path], capture_output=True, text=True,
                              check=True).stdout.strip()


R = None   # the Rga of this run, set by main() or by setup()


def setup(rga, work=None):
    """Select the RGA install and work directory for template() and job()."""
    global R
    R = Rga(rga, work)
    os.makedirs(R.work, exist_ok=True)
    return R


VARIANTS = ["terrain", "terrain_anim", "terrain_near", "terrain_cells_near", "terrain_cells_anim"]
MCPU = {"10.3.0": "gfx1030", "11.0.0": "gfx1100", "12.0.0": "gfx1200"}

def configs():
    out = []
    for r in (1, 0):
        for a in (0, 1):
            out.append((f"world_r{r}_a{a}_lto", "terrain.frag.spv", r, a, 0))
    out.append(("key_r0_a0_lto", "terrain_key_cache.frag.spv", 0, 0, 0))
    out.append(("vsonly_r1_a0", None, 1, 0, 1))
    out.append(("vsonly_r0_a0", None, 0, 0, 1))
    return out

def spec_block(ids_vals):
    lines = []
    for i, (cid, val) in enumerate(ids_vals):
        lines += [f"specConst.mapEntry[{i}].constantID = {cid}",
                  f"specConst.mapEntry[{i}].offset = {4*i}",
                  f"specConst.mapEntry[{i}].size = 4"]
    lines.append("specConst.uintData = " + ", ".join(str(v) for _, v in ids_vals))
    return "\n".join(lines) + "\n"

def make_pipe(tmpl, vs, fs, r, a, unlinked):
    t = re.sub(r"(\[VsSpvFile\]\nfileName = )\S+", lambda m: m.group(1) + vs, tmpl)
    if fs is None:
        t = re.sub(r"\[FsSpvFile\].*?(?=\n\[ResourceMapping\])", "", t, flags=re.S)
    else:
        t = re.sub(r"(\[FsSpvFile\]\nfileName = )\S+", lambda m: m.group(1) + fs, t)
    t = t.replace("[VsInfo]\nentryPoint = main\n",
                  "[VsInfo]\nentryPoint = main\n" + spec_block([(0, r), (1, a), (7, 1)]))
    fs_spec = [(0, r), (2, 0), (4, 0), (7, 1)] if fs == "terrain.frag.spv" else [(4, 0), (7, 1)]
    t = t.replace("[FsInfo]\nentryPoint = main\n",
                  "[FsInfo]\nentryPoint = main\n" + spec_block(fs_spec))
    t = re.sub(r"\nunlinked = \d", f"\nunlinked = {unlinked}", t)
    t = t.split("[CompileLog]")[0]
    return t

def template(variant, spvdir):
    d = f"{R.work}/tmpl_{variant}"
    if os.path.exists(f"{d}/t.pipe"):
        return open(f"{d}/t.pipe").read()
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    shutil.copy(f"{spvdir}/{variant}.vert.spv", d); shutil.copy(f"{spvdir}/terrain.frag.spv", d)
    subprocess.run([R.llpc, "--gfxip=11.0.0", "--auto-layout-desc", "--enable-pipeline-dump",
                    f"--pipeline-dump-dir={R.native(d)}", "-o", "x.elf",
                    f"{variant}.vert.spv", "terrain.frag.spv"], cwd=d, capture_output=True)
    pipes = [f for f in os.listdir(d) if f.endswith(".pipe")]
    if len(pipes) != 1:
        sys.exit(f"template dump for {variant}: {pipes}")
    t = open(f"{d}/{pipes[0]}").read()
    open(f"{d}/t.pipe", "w").write(t)
    return t

def stats(notes, isa):
    gs = re.search(r"\.hardware_stages:\s*\n(.*?)(?=\n\s{4}\.[a-z_]+:\s*\n\s{6}|\Z)", notes, re.S)
    hw = {}
    for stage in ("gs", "vs", "es", "ls", "hs"):
        m = re.search(rf"\n\s+\.{stage}:\s*\n((?:\s{{8,}}.*\n)+)", notes)
        if m and ".vgpr_count" in m.group(1):
            blk = m.group(1)
            g = lambda k: (re.search(rf"\.{k}:\s+(\S+)", blk) or [None, "?"])[1]
            hw = dict(stage=stage, vgpr=g("vgpr_count"), sgpr=g("sgpr_count"),
                      scratch=g("scratch_memory_size"))
            break
    # instruction count of the pre-raster function(s): every symbol except _amdgpu_ps_main
    n = 0; scr = 0; cur = None
    for line in isa.splitlines():
        m = re.match(r"^[0-9a-f]+ <(\S+)>:", line)
        if m: cur = m.group(1); continue
        if cur and not cur.startswith("_amdgpu_ps") and re.match(r"^\s+[sv]_|^\s+(scratch|buffer|global|ds|exp|image|flat)_", line):
            n += 1
            if re.match(r"^\s+scratch_", line): scr += 1
    hw.update(insts=n, scratch_ops=scr)
    return hw

def job(variant, gfx, cfg, spvdir, outdir):
    name, fs, r, a, unl = cfg
    tag = f"{variant}__{MCPU[gfx]}__{name}"
    d = f"{R.work}/{tag}"
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    for f in [x for x in (f"{variant}.vert.spv", fs) if x]:
        shutil.copy(f"{spvdir}/{f}", d)
    open(f"{d}/p.pipe", "w").write(make_pipe(template(variant, spvdir), f"{variant}.vert.spv", fs, r, a, unl))
    p = subprocess.run([R.llpc, f"--gfxip={gfx}", "-o", "p.elf", "p.pipe"], cwd=d, capture_output=True, text=True, errors="replace")
    od = f"{outdir}/{tag}"; os.makedirs(od, exist_ok=True)
    open(f"{od}/llpc.log", "w").write(p.stdout + p.stderr)
    shutil.copy(f"{d}/p.pipe", f"{od}/p.pipe")
    if p.returncode != 0 or not os.path.exists(f"{d}/p.elf"):
        err = (p.stdout + p.stderr).strip().splitlines()
        return tag, dict(rc=p.returncode, err=(err[0] if err else "?")[:120])
    isa = subprocess.run([R.objdump, "--disassemble", "--triple=amdgcn-amd-amdhsa", f"--mcpu={MCPU[gfx]}", "p.elf"],
                         cwd=d, capture_output=True, text=True, errors="replace").stdout
    notes = subprocess.run([R.readobj, "--elf-output-style=GNU", "--notes", "p.elf"],
                           cwd=d, capture_output=True, text=True, errors="replace").stdout
    open(f"{od}/isa.txt", "w").write(isa); open(f"{od}/notes.txt", "w").write(notes)
    shutil.copy(f"{d}/p.elf", f"{od}/p.elf")
    shutil.rmtree(d, ignore_errors=True)
    s = stats(notes, isa); s["rc"] = 0
    return tag, s

def write_tsv(out, rows):
    """Write <out>/llpc.tsv from (tag, stats) rows."""
    with open(f"{out}/llpc.tsv", "w") as fh:
        fh.write("variant\tgfx\tconfig\trc\thw_stage\tinsts\tvgpr\tsgpr\tscratch_bytes\tscratch_ops\terr\n")
        for tag, s in sorted(rows):
            v, g, c = tag.split("__")
            fh.write("\t".join(str(x) for x in (v, g, c, s.get("rc"), s.get("stage", ""), s.get("insts", ""),
                     s.get("vgpr", ""), s.get("sgpr", ""), s.get("scratch", ""), s.get("scratch_ops", ""),
                     s.get("err", ""))) + "\n")


def rga_args(ap):
    """Add the --rga and --work-dir options shared with llpc_redo.py."""
    ap.add_argument("--rga", default=os.environ.get("RGA_DIR"),
                    help="Radeon Developer Tool Suite directory (default $RGA_DIR)")
    ap.add_argument("--work-dir", default=None,
                    help="amdllpc work directory (Windows suite: on a Windows drive)")


def main():
    ap = argparse.ArgumentParser(description="amdllpc ISA record of the terrain vertex stages")
    ap.add_argument("spvdir"); ap.add_argument("outdir")
    ap.add_argument("--gfx", default="11.0.0,12.0.0",
                    help="gfxips; RGA 2.15 amdllpc rejects 10.3.0 (gfx1030)")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--variants", default=",".join(VARIANTS))
    ap.add_argument("--configs", default=",".join(c[0] for c in configs()),
                    help="subset of the configuration names")
    rga_args(ap)
    a = ap.parse_args()
    if not a.rga:
        ap.error("--rga DIR (or RGA_DIR) is required")
    gfx = a.gfx.split(",")
    bad = [g for g in gfx if g not in MCPU]
    if bad:
        ap.error(f"--gfx: unknown {','.join(bad)} (known {','.join(MCPU)})")
    cfgs = [c for c in configs() if c[0] in a.configs.split(",")]
    if not cfgs:
        ap.error("--configs selects no configuration")
    spv = os.path.abspath(a.spvdir); out = os.path.abspath(a.outdir)
    variants = a.variants.split(",")
    for v in variants:
        for f in (f"{v}.vert.spv", "terrain.frag.spv", "terrain_key_cache.frag.spv"):
            if not os.path.isfile(f"{spv}/{f}"):
                ap.error(f"{spv}/{f} missing")
    setup(a.rga, a.work_dir)
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    for v in variants:
        shutil.rmtree(f"{R.work}/tmpl_{v}", ignore_errors=True); template(v, spv)
    jobs = [(v, g, c) for v in variants for g in gfx for c in cfgs]
    rows = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(job, v, g, c, spv, out): (v, g, c) for v, g, c in jobs}
        for f in cf.as_completed(futs):
            tag, s = f.result(); rows.append((tag, s)); print(tag, s, flush=True)
    write_tsv(out, rows)
    print(f"wrote {out}/llpc.tsv")


if __name__ == "__main__":
    main()
