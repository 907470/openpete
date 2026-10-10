#!/usr/bin/env python3
"""Static census of BIOS call sites in a game's generated code.

Finds every A0/B0/C0 trampoline (a function that loads `t2` with 0xA0,
0xB0 or 0xC0, `t1` with the function number and jumps through `t2`) in
the main EXE and every overlay of games/<game>/generated/, then every
split-prefix piece that falls through into one: a function whose body is
nops and a `fallthrough to next function` call to a trampoline or to
another prefix piece (the 4-byte `nop` pieces the splitter emits in
front of a trampoline). A call site is a line that calls a trampoline or
a prefix piece by name or by address (`call_by_address(cpu, 0x...u)`),
outside dispatch tables, override checks and the fallthrough lines that
link a piece to its trampoline.

Usage (repo root): tools/bios_census.py <game> [--calls A70,B32,...] [--all]
Prints one row per trampoline: file, address, table(function), direct
site count and sites, and per prefix piece the sites entering through
it. `--calls` restricts the rows to the listed calls; `--all` lists
every site instead of the first four.
"""
import argparse
import collections
import glob
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

FN_RE = re.compile(r"^void (func_[A-Za-z0-9_]+)\(CPUState\* cpu\)\n\{(.*?)\n\}\n",
                   re.S | re.M)
T2_RE = re.compile(r"cpu->t2 = (\d+);")
T1_RE = re.compile(r"cpu->t1 = (\d+);")
ADDR_RE = re.compile(r"Address: (0x[0-9A-F]+)")
FALL_RE = re.compile(r"(func_[A-Za-z0-9_]+)\(cpu\);\s*/\* fallthrough to next function \*/")
TABLES = {0xA0: "A", 0xB0: "B", 0xC0: "C"}


def source_files(game):
    """Return the main EXE and overlay translation units of a game, main EXE first."""
    gen = ROOT / "games" / game / "generated"
    exe = gen / "PSX.EXE_full.c"
    if not exe.is_file():
        sys.exit(f"bios_census: {exe} not found (build the game first)")
    return [exe] + [Path(p) for p in sorted(glob.glob(str(gen / "*" / "*.bin_full.c")))]


def body_is_prefix(body):
    """Return the fallthrough target when a function body is nops plus one fallthrough call."""
    target = None
    for line in body.splitlines():
        s = line.strip()
        if not s or s.startswith("if (psx_override_dispatch") or s.startswith("/* Address:") \
                or s.startswith("block_") or s.startswith(";  /* label compatibility"):
            continue
        if s.startswith("/* nop */"):
            continue
        m = FALL_RE.search(s)
        if m and target is None:
            target = m.group(1)
            continue
        return None
    return target


def census(game):
    """Collect trampolines, prefix pieces and their call sites for one game."""
    files = source_files(game)
    rel = lambda f: str(f.relative_to(ROOT / "games" / game / "generated"))
    tramp = {}    # name -> (file, table, fn, addr)
    prefix = {}   # name -> (file, target name, addr)
    for f in files:
        src = f.read_text()
        for m in FN_RE.finditer(src):
            name, body = m.group(1), m.group(2)
            am = ADDR_RE.search(body)
            if not am:
                continue
            t2, t1 = T2_RE.search(body), T1_RE.search(body)
            if t2 and t1 and "call_by_address(cpu, cpu->t2)" in body \
                    and int(t2.group(1)) in TABLES:
                tramp[name] = (rel(f), int(t2.group(1)), int(t1.group(1)), am.group(1))
                continue
            tgt = body_is_prefix(body)
            if tgt:
                prefix[name] = (rel(f), tgt, am.group(1))
    # A prefix piece counts when its fallthrough chain ends at a trampoline.
    def resolve(name, seen=()):
        if name in tramp:
            return name
        if name in prefix and name not in seen:
            return resolve(prefix[name][1], seen + (name,))
        return None
    pieces = {n: (v[0], resolve(n), v[2]) for n, v in prefix.items() if resolve(n)}
    targets = {}
    for n, v in tramp.items():
        targets[n] = (n, v[3])
    for n, v in pieces.items():
        targets[n] = (v[1], v[2])
    by_addr = {addr + "u": n for n, (_, addr) in targets.items()}
    sites = collections.defaultdict(list)   # entry name -> [file:line]
    call_re = re.compile(r"(func_[A-Za-z0-9_]+)\(cpu\)|(0x[0-9A-F]{8}u)")
    for f in files:
        for i, line in enumerate(f.read_text().splitlines(), 1):
            if "fallthrough to next function" in line or line.startswith("void ") \
                    or "Address:" in line or "psx_override_dispatch" in line:
                continue
            for m in call_re.finditer(line):
                n = m.group(1) if m.group(1) in targets else by_addr.get(m.group(2) or "")
                if n:
                    sites[n].append(f"{rel(f)}:{i}")
    return tramp, pieces, sites


def main():
    """Print the census table for one game."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("game")
    ap.add_argument("--calls", default="", help="comma list such as A70,B32 (hex function numbers)")
    ap.add_argument("--all", action="store_true", help="list every site")
    a = ap.parse_args()
    want = {c.strip().upper() for c in a.calls.split(",") if c.strip()}
    tramp, pieces, sites = census(a.game)
    print(f"== {a.game}: {len(tramp)} trampolines, {len(pieces)} prefix pieces")
    totals = collections.Counter()
    for name, (tf, t, fn, addr) in sorted(tramp.items(), key=lambda kv: (kv[1][0], kv[1][1], kv[1][2])):
        key = f"{TABLES[t]}{fn:X}"
        if want and key not in want:
            continue
        direct = sites.get(name, [])
        via = [(p, v[2], sites.get(p, [])) for p, v in sorted(pieces.items()) if v[1] == name]
        n = len(direct) + sum(len(s) for _, _, s in via)
        totals[key] += n
        shown = direct if a.all else direct[:4]
        print(f"{tf:45s} {addr} {TABLES[t]}0({fn:#04x}) sites={n} direct={len(direct)} "
              + " ".join(shown))
        for p, paddr, s in via:
            print(f"    prefix {paddr} -> {addr} sites={len(s)} " + " ".join(s))
    print("totals: " + " ".join(f"{k}={v}" for k, v in sorted(totals.items())))


if __name__ == "__main__":
    main()
