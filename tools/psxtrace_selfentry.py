#!/usr/bin/env python3
"""Recorder inputs for the DuckStation `.vblpos` sidecar (psxtrace patch 0029).

`scan GAME`: lists every generated function whose body contains
`goto block_<self>;`, a branch back to the function's first instruction.
The reference counts that loop-back as an execution at PC == T, while
openpete's per-entry gate (`psx_override_dispatch` at the top of every
generated body) sits before the entry label and does not see it, so the
recorder never chooses such a function as a `.vblpos` target. One line per
function, `<overlay> 0x<addr>`; `<overlay>` is `main` for the executable's
`PSX.EXE_full.c` and the directory name under `generated/` for an overlay;
directories whose name starts with `_` hold intermediate passes and are
skipped.

`library GAME`: the PsyQ library spans the recorder treats as non-game
code: each symbol listed in `games/<game>/manifest/psyq.toml` (the thunked
functions; a function absent from the manifest is translated and counts as
game code), from its address to the next symbol of the linker map inside
the manifest's `range_start`..`range_end`. One line per span, `0x<lo>
0x<hi>` with `hi` exclusive.

Both write to stdout, or to `--out PATH`; `--count` prints the summary
line only.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

FUNC_RE = re.compile(r"^void (\w+)\(CPUState\* cpu\)\s*$")
GATE_RE = re.compile(r"psx_override_dispatch\(cpu, 0x([0-9A-Fa-f]{8})u\)")


def scan_file(path: Path):
    """Yield the entry address of every function in `path` that jumps to its own entry label."""
    lines = path.read_text(errors="replace").splitlines()
    i = 0
    n = len(lines)
    while i < n:
        if not FUNC_RE.match(lines[i]) or i + 1 >= n or lines[i + 1] != "{":
            i += 1
            continue
        j = i + 2
        addr = None
        body_has = False
        while j < n and lines[j] != "}":
            if addr is None:
                m = GATE_RE.search(lines[j])
                if m:
                    addr = m.group(1).upper()
            elif not body_has and ("goto block_%s;" % addr) in lines[j]:
                body_has = True
            j += 1
        if addr is not None and body_has:
            yield int(addr, 16)
        i = j + 1


def cmd_scan(game: str):
    """Return `(overlay, addr)` for every self-entry function of `game`, sorted."""
    gen = ROOT / "games" / game / "generated"
    if not gen.is_dir():
        sys.exit("no generated C: %s" % gen)
    rows = []
    for path in sorted(gen.rglob("*_full.c")):
        rel = path.relative_to(gen)
        if rel.parts[0].startswith("_"):
            continue  # a recompiler intermediate pass, not a linked body
        overlay = "main" if len(rel.parts) == 1 else rel.parts[0]
        for addr in scan_file(path):
            rows.append((overlay, addr))
    return sorted(set(rows), key=lambda r: (r[0] != "main", r[0], r[1]))


def read_psyq(game: str):
    """Return `(range_start, range_end, [symbol addresses])` from the game's psyq.toml."""
    text = (ROOT / "games" / game / "manifest" / "psyq.toml").read_text()
    lo = int(re.search(r"^range_start\s*=\s*(0x[0-9a-fA-F]+)", text, re.M).group(1), 16)
    hi = int(re.search(r"^range_end\s*=\s*(0x[0-9a-fA-F]+)", text, re.M).group(1), 16)
    addrs = sorted({int(a, 16) for a in re.findall(r"^addr\s*=\s*(0x[0-9a-fA-F]+)", text, re.M)})
    return lo, hi, addrs


def read_map_symbols(game: str, lo: int, hi: int):
    """Return the sorted symbol addresses of the game's linker map inside [lo, hi)."""
    path = ROOT / "games" / game / game / "build" / "psx.map"
    if not path.is_file():
        sys.exit("no linker map: %s" % path)
    sym_re = re.compile(r"^\s+0x([0-9a-fA-F]{16})\s+([A-Za-z_]\w*)\s*$")
    out = set()
    for line in path.read_text(errors="replace").splitlines():
        m = sym_re.match(line)
        if m:
            a = int(m.group(1), 16) & 0xFFFFFFFF
            if lo <= a < hi:
                out.add(a)
    return sorted(out)


def cmd_library(game: str):
    """Return the `[lo, hi)` spans of the thunked PsyQ functions of `game`, merged where adjacent."""
    lo, hi, addrs = read_psyq(game)
    bounds = read_map_symbols(game, lo, hi)
    spans = []
    for a in addrs:
        if not lo <= a < hi:
            continue
        nxt = next((b for b in bounds if b > a), hi)
        if spans and spans[-1][1] == a:
            spans[-1] = (spans[-1][0], nxt)
        else:
            spans.append((a, nxt))
    return spans


def main(argv=None) -> int:
    """Parse the command line, run the subcommand and write its rows."""
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("cmd", choices=["scan", "library"])
    p.add_argument("game")
    p.add_argument("--out")
    p.add_argument("--count", action="store_true")
    a = p.parse_args(argv)
    if a.cmd == "scan":
        rows = cmd_scan(a.game)
        lines = ["%s 0x%08X" % r for r in rows]
        main_n = sum(1 for r in rows if r[0] == "main")
        summary = "self-entry %s: %d functions (%d main, %d overlay)" % (
            a.game, len(rows), main_n, len(rows) - main_n)
    else:
        spans = cmd_library(a.game)
        lines = ["0x%08X 0x%08X" % s for s in spans]
        summary = "library %s: %d spans, %d bytes" % (a.game, len(spans), sum(h - l for l, h in spans))
    if a.count:
        print(summary)
        return 0
    text = "".join(l + "\n" for l in lines)
    if a.out:
        Path(a.out).write_text(text)
        print(summary, file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
