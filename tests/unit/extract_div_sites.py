#!/usr/bin/env python3
"""Copy the first div and divu statement of a generated PSX.EXE_full.c into a translation unit of their own.

Each statement is the recompiler's text verbatim, wrapped in
`void div_site(CPUState* cpu, uint32_t n, uint32_t d)` (and `divu_site`),
which loads n into the instruction's rs register and d into its rt register
before running it. Sites whose rs and rt are the same register or $zero are
skipped, since n and d could not be set independently. Refuses (exit 1)
when the input holds no usable site of either kind.

usage: extract_div_sites.py <PSX.EXE_full.c> <out.c>
"""

import re
import sys

# The trailing comment carries the instruction word; funct 0x1A is div,
# 0x1B divu (SPECIAL opcode, rd and sa zero).
SITE = re.compile(r"^\s*(?P<stmt>if \(.*\})\s*/\* 0x[0-9A-F]{8}: 0x(?P<word>[0-9A-F]{8}) \*/$", re.M)
REG_NAMES = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
             "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
             "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
             "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


def first_site(text, funct):
    """Return (stmt, rs name, rt name, word) of the first usable site whose instruction has the given funct."""
    for m in SITE.finditer(text):
        word = int(m.group("word"), 16)
        if word >> 26 != 0 or word & 0x3F != funct or (word >> 6) & 0x3FF != 0:
            continue
        rs, rt = (word >> 21) & 31, (word >> 16) & 31
        if rs == rt or rs == 0 or rt == 0:
            continue
        return m.group("stmt"), REG_NAMES[rs], REG_NAMES[rt], m.group("word")
    raise SystemExit("extract_div_sites: no usable funct 0x%02X site in the input" % funct)


def wrapper(name, site):
    """Return the C function that loads n and d into the site's registers and runs its statement."""
    stmt, rs, rt, word = site
    return ("/** @brief Run the recompiled 0x%s with rs = n and rt = d.\n"
            " * @param cpu register file the statement reads and writes\n"
            " * @param n dividend, loaded into $%s\n"
            " * @param d divisor, loaded into $%s */\n"
            "void %s(CPUState* cpu, uint32_t n, uint32_t d)\n{\n"
            "    cpu->%s = n;\n    cpu->%s = d;\n    %s\n}\n" % (word, rs, rt, name, rs, rt, stmt))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, out = sys.argv[1:]
    with open(src, encoding="utf-8") as f:
        text = f.read()
    body = ["/* Copied from %s by tests/unit/extract_div_sites.py; do not edit. */" % src,
            '#include "psx_runtime.h"', "",
            wrapper("div_site", first_site(text, 0x1A)), wrapper("divu_site", first_site(text, 0x1B))]
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(body))


if __name__ == "__main__":
    main()
