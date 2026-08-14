#!/usr/bin/env python3
"""Report unresolved cross-piece split targets in a recompiled .bin_full.c.

psxrecomp emits one C function per discovered guest function, but a guest
function often has to be broken into several C pieces (an external JAL needs
its continuation to be separately dispatchable; a forced entry lands inside an
existing range). Control transfers that cross a piece boundary can only be
expressed as a call, so the generator emits one of:

    call_by_address(cpu, 0x8007E024u); return;  /* jal cont: split */
    call_by_address(cpu, 0x8007E024u); return;  /* jalr cont: split */
    call_by_address(cpu, 0x800809B4u); return;  /* taken: split (mid-func) */
    call_by_address(cpu, 0x800809B4u); return;  /* not taken: split (mid-func) */
    call_by_address(cpu, 0x800809B4u); return;  /* j to split (mid-func) */

Each of those addresses needs a registered function entry. Without one the
runtime stub-misses, and — because the stub returns instead of transferring
control — the guest function's epilogue never runs: `cpu->sp` leaks one stack
frame per call until it walks out of RAM. (Lofty Castle's 0x800809B4, reached
by unlocking the chest, leaked 0x280 bytes a call.)

This tool emits the targets that have no emitted definition, one per line, in
extra-funcs format. Feed the result back to psxrecomp via `--extra-funcs=` and
re-run: a promoted target gains a definition and drops out of the next report,
so an empty report means converged. Iteration is required because promoting a
target splits its containing function, which can expose further cross-piece
transfers.

Usage:
    extract_split_targets.py <generated-full.c> <out-file> \\
        [--binary=<raw-bytes> --load-addr=0xADDR]

Passing the binary enables range filtering, which is what makes convergence
reachable: psxrecomp decodes non-code bytes (the overlay-id header word at the
load address, trailing data sections) as instructions, yielding branch targets
past the end of the image. Those can never be resolved and are never executed.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# One alternation per generator site in code_generator.cpp. Keep in sync with
# the comment strings there — a variant missing here becomes a runtime stub
# miss that no gate catches.
SPLIT_RE = re.compile(
    r"call_by_address\(cpu,\s*0x([0-9A-Fa-f]+)u\);\s*return;\s*/\*\s*"
    r"(?:jal cont: split"
    r"|jalr cont: split"
    r"|taken: split \(mid-func\)"
    r"|not taken: split \(mid-func\)"
    r"|j to split \(mid-func\))\s*\*/"
)

DEF_RE = re.compile(r"^void func_\w*?([0-9A-Fa-f]{8})\(CPUState\* cpu\)$")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=Path, help="generated <name>.bin_full.c")
    ap.add_argument("out", type=Path, help="output extra-funcs list")
    ap.add_argument("--binary", type=Path, default=None,
                    help="raw image bytes, for range filtering")
    ap.add_argument("--load-addr", default=None,
                    help="load address of --binary (e.g. 0x8007AA38)")
    args = ap.parse_args(argv[1:])

    lo = hi = None
    if args.binary is not None:
        if args.load_addr is None:
            print("--binary requires --load-addr", file=sys.stderr)
            return 2
        lo = int(args.load_addr, 0)
        hi = lo + args.binary.stat().st_size

    defined: set[int] = set()
    targets: set[int] = set()
    dropped = 0
    with args.source.open("r", encoding="utf-8") as fh:
        for line in fh:
            d = DEF_RE.match(line.rstrip("\n"))
            if d:
                defined.add(int(d.group(1), 16))
                continue
            m = SPLIT_RE.search(line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            if lo is not None and not (lo <= addr < hi):
                dropped += 1
                continue
            targets.add(addr)

    unresolved = sorted(targets - defined)
    with args.out.open("w", encoding="utf-8") as fh:
        fh.write("# unresolved split targets — auto-extracted by"
                 " extract_split_targets.py\n")
        for t in unresolved:
            fh.write(f"0x{t:08X}\n")

    note = f", {dropped} out-of-image ignored" if dropped else ""
    print(f"{len(unresolved)} unresolved of {len(targets)} split targets{note}"
          f" -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
