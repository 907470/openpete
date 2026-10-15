#!/usr/bin/env python3
"""merge_module_defs.py - the one executable's PE export surface: the union
of the per-game .def files tools/gen_module_exports.py writes.

Every game's recomp module imports from the same executable, so the
executable exports each name any linked game's module imports, once. A name
exported by more than one game's .def is written once; a name one .def
exports as DATA and another as code is refused, since the module side
compiles a DATA import through __declspec(dllimport) and a code import as a
call through the import thunk, and one export cannot serve both.

Input lines are `NAME <image>`, `LIBRARY <image>`, `EXPORTS`, and one export
per line, `<symbol>` or `<symbol> DATA`. The output keeps the first-seen
order and names the image with NAME (not LIBRARY, which marks the linked
image a DLL).

Exit 0 written, 1 refused (each conflict on stderr), 2 usage or parse error.
"""
import argparse
import sys


def read_def(path):
    """Return [(symbol, is_data)] from one .def, in file order."""
    out = []
    for n, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.split(";", 1)[0].strip()
        if not line or line == "EXPORTS":
            continue
        head = line.split()[0]
        if head in ("NAME", "LIBRARY"):
            continue
        parts = line.split()
        if len(parts) > 2 or (len(parts) == 2 and parts[1] != "DATA"):
            sys.stderr.write(f"{path}:{n}: unparsed export line: {line}\n")
            sys.exit(2)
        out.append((parts[0], len(parts) == 2))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True, help="the executable's file name")
    ap.add_argument("--out", required=True)
    ap.add_argument("defs", nargs="+")
    args = ap.parse_args()

    kind = {}      # symbol -> (is_data, first .def)
    order = []
    conflicts = []
    for path in args.defs:
        for sym, is_data in read_def(path):
            if sym not in kind:
                kind[sym] = (is_data, path)
                order.append(sym)
            elif kind[sym][0] != is_data:
                conflicts.append(f"{sym}: DATA in {path if is_data else kind[sym][1]}, "
                                 f"code in {kind[sym][1] if is_data else path}")
    if conflicts:
        for c in conflicts:
            sys.stderr.write(f"merge_module_defs: {c}\n")
        return 1
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(f"NAME {args.image}\nEXPORTS\n")
        for sym in order:
            f.write(f"  {sym} DATA\n" if kind[sym][0] else f"  {sym}\n")
    print(f"merge_module_defs: {len(order)} exports from {len(args.defs)} .def files",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
