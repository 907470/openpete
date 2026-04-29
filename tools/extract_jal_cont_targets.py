#!/usr/bin/env python3
"""Extract jal-cont split-target addresses from a recompiled .bin_full.c.

When psxrecomp encounters an external JAL (target outside this binary, not in
external/manual symbols), it splits the calling function at that site and
emits:
    cpu->ra = 0xXXXXXX24;
    call_by_address(cpu, 0x80052D64u);  /* external jal */
    call_by_address(cpu, 0x8007E024u); return;  /* jal cont: split */

The split-cont address (0x8007E024 above) needs a registered function entry
or runtime dispatch will stub-miss. This tool scans the recompiled C and
emits each unique cont address, one per line, in extra-funcs format.
"""

from __future__ import annotations

import re
import sys

PATTERN = re.compile(
    r"call_by_address\(cpu,\s*0x([0-9A-Fa-f]+)u\);\s*return;\s*/\*\s*jal cont: split\s*\*/"
)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: extract_jal_cont_targets.py <bin_full.c> <out.txt>",
              file=sys.stderr)
        return 2
    src_path, out_path = argv[1], argv[2]
    targets: set[int] = set()
    with open(src_path, "r", encoding="utf-8") as fh:
        for line in fh:
            m = PATTERN.search(line)
            if m:
                targets.add(int(m.group(1), 16))
    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write("# jal-cont split targets — auto-extracted by extract_jal_cont_targets.py\n")
        for t in sorted(targets):
            fh.write(f"0x{t:08X}\n")
    print(f"wrote {len(targets)} jal-cont targets to {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
