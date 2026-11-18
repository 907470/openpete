#!/usr/bin/env python3
"""probe_run.py - run GP0 command lists on duckstation-regtest's software GPU
and return the VRAM pixels each one wrote.

A case is (x, y, w, h, words): the GP0 words to send and the VRAM rect to
read back (x and w multiples of 16; the rect is filled with 0 before the
next case). A sparse case (w and h at most 256) returns the nonzero pixels;
h | DENSE returns every pixel of the rect; w = 0 is a setup case (texture
upload) that reads nothing back and returns None. run_cases() builds probe.c into a PS-EXE with
the cases linked in, boots it in duckstation-regtest, dumps main RAM at
probe_done's entry (-ram-dump-at) and returns, per case, a dict mapping
(column, row) in VRAM to the 16-bit halfword there. Cases that would not fit
the free RAM in one boot are split over several boots.
"""
import os
import re
import pathlib
import shutil
import struct
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
RAM_END = 0x801F0000          # below the stack
DENSE = 0x80000000
CROSS = "mipsel-linux-gnu-"


def regtest_path():
    """The duckstation-regtest binary: $OPENPETE_DS_REGTEST, else the main checkout's build."""
    p = os.environ.get("OPENPETE_DS_REGTEST")
    if p:
        return pathlib.Path(p)
    root = HERE.parent.parent
    for cand in (root, root.parent.parent):   # a worktree sits at <main>/worktrees/<name>
        b = cand / "third_party/duckstation/build-regtest/bin/duckstation-regtest"
        if b.exists():
            return b
    raise SystemExit("duckstation-regtest not found; set OPENPETE_DS_REGTEST")


def applied_head():
    """The applied DuckStation head recorded in the patch series."""
    return (HERE.parent.parent / "third_party/duckstation-patches/applied-head.txt").read_text().strip()


def _build(work, cases):
    blob = bytearray()
    for (x, y, w, h, words) in cases:
        hh = h & 0xFFFF
        assert x % 16 == 0 and w % 16 == 0 and (w == 0 or hh > 0)
        assert (h & DENSE) or w <= 256 and hh <= 256
        blob += struct.pack("<5I", len(words), x, y, w, h)
        blob += struct.pack("<%dI" % len(words), *words)
    blob += struct.pack("<I", 0xFFFFFFFF)
    (work / "cases.bin").write_bytes(bytes(blob))
    for f in ("probe.c", "crt0.S", "probe.ld"):
        shutil.copy(HERE / f, work / f)
    flags = ["-march=r3000", "-mabi=32", "-mno-abicalls", "-fno-pic", "-G0", "-msoft-float",
             "-O2", "-ffreestanding", "-nostdlib", "-fno-builtin"]
    subprocess.run([CROSS + "gcc", *flags, "-T", "probe.ld", "-o", "probe.elf", "crt0.S", "probe.c", "-lgcc"],
                   cwd=work, check=True)
    subprocess.run([CROSS + "objcopy", "-O", "binary", "probe.elf", "probe.bin"], cwd=work, check=True)
    syms = {}
    out = subprocess.run([CROSS + "nm", "probe.elf"], cwd=work, check=True, capture_output=True, text=True).stdout
    for line in out.splitlines():
        a, _, n = line.split()
        syms[n] = int(a, 16) & 0xFFFFFFFF
    body = (work / "probe.bin").read_bytes()
    body += b"\0" * (-len(body) % 0x800)
    hdr = bytearray(0x800)
    hdr[0:8] = b"PS-X EXE"
    struct.pack_into("<IIII", hdr, 0x10, syms["_start"], 0, 0x80010000, len(body))
    struct.pack_into("<II", hdr, 0x30, 0x801FFF00, 0)
    hdr[0x4C:0x4C + 55] = b"Sony Computer Entertainment Inc. for North America area"
    (work / "probe.exe").write_bytes(bytes(hdr) + body)
    return syms


def _boot(work, syms, frames):
    dump = work / "ram.bin"
    for f in (dump, work / "ram.bin.txt"):
        if f.exists():
            f.unlink()
    exe = regtest_path()
    env = dict(os.environ, LIBGL_ALWAYS_SOFTWARE="1", GALLIUM_DRIVER="llvmpipe")
    cmd = [str(exe), "-frames", str(frames), "-console",
           "-ram-dump-at", "0x%08X=%s" % (syms["probe_done"], dump), str(work / "probe.exe")]
    r = subprocess.run(["sh", "-c", 'ulimit -c 0; "$@" 2>&1; exit 0', "_", *cmd], cwd=exe.parent,
                       capture_output=True, text=True)
    (work / "regtest.log").write_text("".join(l + "\n" for l in r.stdout.splitlines() if "I/CPU" not in l))
    if not dump.exists():
        return None, None
    m = re.search(r" frame (\d+) ", (work / "ram.bin.txt").read_text())
    return dump.read_bytes(), int(m.group(1))


def run_cases(cases, work, est_pixels=None, log=print, single_boot=False):
    """Plays cases and returns one {(vram_x, vram_y): halfword} per case.

    single_boot plays every case in one boot (a list that starts with setup
    cases the later ones depend on); it fails when the output overruns RAM.

    est_pixels(case) bounds the nonzero pixels a case writes, for splitting
    the cases over boots; by default the whole rect."""
    work = pathlib.Path(work)
    work.mkdir(parents=True, exist_ok=True)
    def est_default(c):
        if c[2] == 0:
            return 0
        if c[3] & DENSE:
            return (c[2] * (c[3] & 0xFFFF) + 1) // 2
        return c[2] * c[3]
    est = est_pixels or est_default
    results = []
    rate = None
    i = 0
    boot = 0
    while i < len(cases):
        blob_bytes, out_words, j = 4, 0, i
        while j < len(cases):
            c = cases[j]
            nb = 20 + 4 * len(c[4])
            nw = 1 + est(c)
            if j > i and not single_boot and 0x80010000 + 0x4000 + blob_bytes + nb + 4 * (out_words + nw) > RAM_END:
                break
            blob_bytes += nb
            out_words += nw
            j += 1
        chunk = cases[i:j]
        syms = _build(work, chunk)
        cost = sum(c[2] * (c[3] & 0xFFFF) + 64 + len(c[4]) for c in chunk)
        frames = int(cost * rate * 1.5) + 100 if rate else 400
        while True:
            ram, done_frame = _boot(work, syms, frames)
            if ram is not None:
                break
            log("boot %d: no dump within %d frames; doubling" % (boot, frames))
            frames *= 2
        rate = max(rate or 0.0, done_frame / cost)
        off = syms["probe_out"] - 0x80000000
        for (x, y, w, h, _) in chunk:
            if w == 0:
                results.append(None)
                continue
            if h & DENSE:
                hh = h & 0xFFFF
                hw = struct.unpack_from("<%dH" % (w * hh), ram, off)
                off += 4 * ((w * hh + 1) // 2)
                results.append({(x + i % w, y + i // w): v for i, v in enumerate(hw) if v})
                continue
            (n,) = struct.unpack_from("<I", ram, off)
            off += 4
            px = {}
            for (v,) in struct.iter_unpack("<I", ram[off:off + 4 * n]):
                px[(x + (v & 0xFF), y + ((v >> 8) & 0xFF))] = v >> 16
            off += 4 * n
            results.append(px)
        end = syms["probe_out"] - 0x80000000
        log("boot %d: cases %d..%d, output %d bytes, done at VBL %d" % (boot, i, j - 1, off - end, done_frame))
        if off + 0x80000000 > RAM_END:
            raise SystemExit("output overran free RAM; lower est_pixels' bound")
        boot += 1
        i = j
    return results


if __name__ == "__main__":
    import tempfile
    w = pathlib.Path(tempfile.mkdtemp())
    res = run_cases([(96, 96, 16, 16, [0x40FFFFFF, (100 << 16) | 100, (104 << 16) | 108])], w)
    print(sorted(res[0].items()))
