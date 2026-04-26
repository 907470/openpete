#!/usr/bin/env python3
"""Extract all gte_execute call-site PCs from the recompiled PSX.EXE_full.c
and emit them as a Lua table for use in scripts/pcsx-redux-gte-dump*.lua.

Each gte_execute call has a comment of the form:
    gte_execute(cpu, 0x0486012);  /* gte cmd 0x12 */  /* 0x80016DC4: 0x4A486012 */

We extract the PC (0x80016DC4) and the cmd word (0x4A486012). The low 6
bits of the cmd are the GTE function number (0x12 = MVMVA in this case).

Usage:
    scripts/extract-cop2-pcs.py [PSX.EXE_full.c]  # default: games/spyro-1/generated/PSX.EXE_full.c
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / 'games' / 'spyro-1' / 'generated' / 'PSX.EXE_full.c'

if len(sys.argv) > 1:
    SRC = Path(sys.argv[1])

PAT = re.compile(r'gte_execute\(cpu, [^)]+\);.*?\b(0x[0-9A-Fa-f]{8}): (0x[0-9A-Fa-f]+)')

sites = set()
with open(SRC) as f:
    for line in f:
        m = PAT.search(line)
        if m:
            pc = m.group(1)
            cmd = m.group(2)
            sites.add((pc.lower(), cmd.lower()))

# Emit sorted by PC.
for pc, cmd in sorted(sites):
    func = int(cmd, 16) & 0x3F
    print(f'    {pc.upper().replace("0X", "0x")},  -- {cmd.upper().replace("0X", "0x")} (func 0x{func:02X})')

print(f'-- {len(sites)} sites', file=sys.stderr)
