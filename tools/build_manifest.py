#!/usr/bin/env python3
"""Parse a game's psx.map into a structured symbol manifest.

Emits two TOML files into <out_dir>:

  symbols.toml   functions: addr, name, kind=decomp|recomp, source_file, size
  globals.toml   globals:   addr, name, kind=decomp|recomp, source_file, section

`kind` is derived from the .o path: build/src/* (matching-decomp C) ->
"decomp"; build/asm/* (handwritten MIPS) -> "recomp". The downstream
generators (gen_defsyms, gen_dispatch, gen_thunks) consume these
manifests to emit the per-game plumbing that today is hand-written in
games/spyro-1/decomp/ and game_extras_impl.c.

Map file format (GNU ld):

    .text          0x0000000080012204       0xa4 build/src/main.o
                   0x0000000080012204                main
    .text          0x00000000800163e4      0x3a0 build/src/cd.o
                   0x00000000800163e4                CDLoadTime
                   0x0000000080016490                CDReadDone
                   ...

Symbol-line tokens with `=`, parentheses, `*`, or that look like linker-
generated names (`*_TEXT_START`, `_END`, `_SIZE`, `ALIGN`) are skipped.

CLI:
    build_manifest.py <psx.map> <out_dir>
"""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# Section header - leading single space, then section name, addr, size, .o path.
#   " .text          0x0000000080012204       0xa4 build/src/main.o"
SECTION_RE = re.compile(
    r"^ \.(text|data|rodata|sdata|sbss|bss)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)\s*$"
)

# Symbol entry - long indent, addr, name. Computed entries (= ..., ALIGN, etc.)
# share the same prefix; we filter those out by checking the trailing token.
#   "                0x0000000080012204                main"
SYMBOL_RE = re.compile(r"^\s+0x([0-9a-f]+)\s+(\S.*)$")

# Names we never want in the manifest: linker script artifacts.
#   `main_TEXT_START`, `main_RODATA_END`, `main_SBSS_SIZE`, etc.
LINKER_SUFFIXES = ("_TEXT_START", "_TEXT_END", "_TEXT_SIZE",
                   "_DATA_START", "_DATA_END", "_DATA_SIZE",
                   "_RODATA_START", "_RODATA_END", "_RODATA_SIZE",
                   "_SDATA_START", "_SDATA_END", "_SDATA_SIZE",
                   "_SBSS_START", "_SBSS_END", "_SBSS_SIZE",
                   "_BSS_START", "_BSS_END", "_BSS_SIZE")

CODE_SECTIONS = {"text"}
DATA_SECTIONS = {"data", "rodata", "sdata", "sbss", "bss"}


@dataclass
class Symbol:
    addr: int
    name: str
    section: str       # text/data/bss/...
    source_file: str   # build/src/main.o -> src/main.c
    kind: str          # decomp | recomp


@dataclass
class Section:
    name: str
    addr: int
    size: int
    source_object: str
    symbols: list[Symbol] = field(default_factory=list)


def is_clean_symbol(rest: str) -> bool:
    """True for plain `name`, false for `name = expr`, `(.text*)`, `ALIGN ...`."""
    if not rest:
        return False
    # First token only - anything with `=`, `(`, or starting with `.` is a
    # linker construct, not a real symbol.
    first = rest.split()[0]
    if any(c in rest for c in "=()"):
        return False
    if first.startswith(".") or first.startswith("*"):
        return False
    if first in {"ALIGN", "PROVIDE", "LOAD", "OUTPUT", "ABSOLUTE"}:
        return False
    if any(first.endswith(suf) for suf in LINKER_SUFFIXES):
        return False
    return True


def object_to_source(obj_path: str) -> str:
    """`build/src/cd.o` -> `src/cd.c`; `build/asm/math.o` -> `asm/math.s`."""
    p = obj_path
    if p.startswith("build/"):
        p = p[len("build/"):]
    if p.endswith(".o"):
        stem = p[:-2]
        if stem.startswith("src/"):
            return stem + ".c"
        if stem.startswith("asm/"):
            return stem + ".s"
        return stem
    return p


def kind_for_object(obj_path: str) -> str:
    if obj_path.startswith("build/src/"):
        return "decomp"
    if obj_path.startswith("build/asm/"):
        return "recomp"
    return "other"


def parse_map(path: Path) -> list[Section]:
    sections: list[Section] = []
    current: Section | None = None

    for line in path.read_text(errors="replace").splitlines():
        m = SECTION_RE.match(line)
        if m:
            sec_name, addr_hex, size_hex, obj = m.groups()
            current = Section(
                name=sec_name,
                addr=int(addr_hex, 16),
                size=int(size_hex, 16),
                source_object=obj,
            )
            sections.append(current)
            continue

        if current is None:
            continue

        # Symbol entries are indented further than section headers (which start
        # with a single space). Anything else closes the section.
        if not line.startswith("                "):
            # Blank line between sections is fine; only break on a non-empty
            # non-symbol line that isn't another section header.
            if line.strip() == "":
                continue
            current = None
            continue

        sm = SYMBOL_RE.match(line)
        if not sm:
            continue
        addr_hex, rest = sm.groups()
        rest = rest.rstrip()
        if not is_clean_symbol(rest):
            continue
        name = rest.split()[0]
        # The decomp's INCLUDE_ASM macro emits a C wrapper symbol
        # `__maspsx_include_asm_hack_<name>` at the same address as
        # <name>; the wrapper is an artefact of that build, not a game
        # symbol.
        if name.startswith("__maspsx_include_asm_hack_"):
            continue
        current.symbols.append(Symbol(
            addr=int(addr_hex, 16),
            name=name,
            section=current.name,
            source_file=object_to_source(current.source_object),
            kind=kind_for_object(current.source_object),
        ))

    return sections


def write_toml(path: Path, header: str, table_name: str, rows: list[dict]) -> None:
    """Hand-roll TOML - avoids a tomli_w dep. Rows must use plain types."""
    lines = [header.rstrip(), ""]
    for row in rows:
        lines.append(f"[[{table_name}]]")
        for key, val in row.items():
            if isinstance(val, int):
                if key == "addr":
                    lines.append(f'{key} = 0x{val:08X}')
                elif key == "size":
                    lines.append(f"{key} = 0x{val:X}")
                else:
                    lines.append(f"{key} = {val}")
            elif isinstance(val, str):
                escaped = val.replace("\\", "\\\\").replace('"', '\\"')
                lines.append(f'{key} = "{escaped}"')
            else:
                raise TypeError(f"unsupported TOML value: {key}={val!r}")
        lines.append("")
    path.write_text("\n".join(lines))


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: build_manifest.py <psx.map> <out_dir>", file=sys.stderr)
        return 2
    map_path = Path(argv[1])
    out_dir = Path(argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    sections = parse_map(map_path)

    functions: list[dict] = []
    globals_: list[dict] = []
    for sec in sections:
        for sym in sec.symbols:
            row = {
                "addr": sym.addr,
                "name": sym.name,
                "kind": sym.kind,
                "source_file": sym.source_file,
            }
            if sec.name in CODE_SECTIONS:
                # Per-symbol size isn't recorded in the map for individual
                # functions - only per-.o size. Leave size off for now;
                # downstream consumers don't need it yet.
                functions.append(row)
            elif sec.name in DATA_SECTIONS:
                row["section"] = sec.name
                globals_.append(row)

    # Stable order = by address. Easier to diff.
    functions.sort(key=lambda r: r["addr"])
    globals_.sort(key=lambda r: r["addr"])

    write_toml(
        out_dir / "symbols.toml",
        "# Auto-generated by tools/build_manifest.py - do not edit.\n"
        f"# Source: {map_path}",
        "function",
        functions,
    )
    write_toml(
        out_dir / "globals.toml",
        "# Auto-generated by tools/build_manifest.py - do not edit.\n"
        f"# Source: {map_path}",
        "global",
        globals_,
    )

    decomp_fn = sum(1 for r in functions if r["kind"] == "decomp")
    recomp_fn = sum(1 for r in functions if r["kind"] == "recomp")
    decomp_gl = sum(1 for r in globals_ if r["kind"] == "decomp")
    recomp_gl = sum(1 for r in globals_ if r["kind"] == "recomp")
    print(f"functions: {len(functions):4d}  ({decomp_fn} decomp, {recomp_fn} recomp)")
    print(f"globals:   {len(globals_):4d}  ({decomp_gl} decomp, {recomp_gl} recomp)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
