#!/usr/bin/env python3
"""Copy one recompiled function out of a generated PSX.EXE_full.c into its own translation unit.

The output includes psx_runtime_internal.h, declares every func_XXXXXXXX the
body calls (the test defines them), and holds the function's text verbatim,
so a unit test runs the exact code the game module runs against a bus and RAM
image of its own. Refuses (exit 1) when the function is absent, appears
twice, or its body uses a file-static helper of the generated file
(psx_lwl and kin), which the copy would not carry.

usage: extract_recomp_function.py <PSX.EXE_full.c> <func_XXXXXXXX> <out.c>
"""

import re
import sys


def extract(text, name):
    """Return the text of `void <name>(CPUState* cpu) { ... }` up to its closing brace at column 0."""
    head = "void %s(CPUState* cpu)\n{\n" % name
    start = text.find(head)
    if start < 0:
        raise SystemExit("extract_recomp_function: %s not defined in the input" % name)
    if text.find(head, start + 1) >= 0:
        raise SystemExit("extract_recomp_function: %s defined twice in the input" % name)
    end = text.find("\n}\n", start)
    if end < 0:
        raise SystemExit("extract_recomp_function: %s has no closing brace" % name)
    return text[start:end + 3]


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    src, name, out = sys.argv[1:]
    with open(src, encoding="utf-8") as f:
        body = extract(f.read(), name)
    statics = sorted(set(re.findall(r"\bpsx_(?:lwl|lwr|swl|swr)\b", body)))
    if statics:
        raise SystemExit("extract_recomp_function: %s uses file-static helpers %s" % (name, ", ".join(statics)))
    callees = sorted(set(re.findall(r"\b(func_[0-9A-F]{8})\(cpu\)", body)) - {name})
    lines = ["/* Copied from %s by tests/unit/extract_recomp_function.py; do not edit. */" % src,
             '#include "psx_runtime_internal.h"', ""]
    lines += ["void %s(CPUState* cpu);" % c for c in callees + [name]]
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n\n" + body)


if __name__ == "__main__":
    main()
