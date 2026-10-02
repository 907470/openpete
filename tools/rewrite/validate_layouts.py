"""Validate the PSX layout calculator (types.py) against psx.map.

For a hand-listed set of well-known struct globals, parse the relevant
TU + headers, compute the global's PSX size (struct size × array count if
declared as `T name[N]`), and check it against the gap to the next labeled
symbol in psx.map.

A *match* means our computed size <= map gap (gap may include unlabeled
trailing globals; we accept gap >= computed). A *mismatch* (computed > gap)
fails the validator - that means the struct is too big to fit, so layout
disagrees with PSX.

Run:
    python3 -m tools.rewrite.validate_layouts
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import clang.cindex as ci

from .parser import SPYRO_REPO, parse_tu
from .types import collect_record_layouts, layout_struct, StructLayout


PSX_MAP = SPYRO_REPO / "build" / "psx.map"


# Globals validated against psx.map. All resolved through a single
# synthetic TU (`_all_headers.c`) that #includes every spyro-1 header,
# so visibility never depends on which production TU we'd otherwise pick.
TARGETS = [
    "g_CdState",
    "g_CdMusic",
    "g_Camera",
    "g_DB",
    "g_Pad",
    "g_PadBackup",
    "g_Sparkles",
    "g_Buffers",
    "g_LevelHeader",
    "g_NewCyclorama",
]


SYNTHETIC_HEADERS = [
    "common.h", "cd.h", "music.h", "spu.h", "camera.h", "graphics.h",
    "gamepad.h", "renderers.h", "buffers.h", "wad.h", "cyclorama.h",
]


# ---------------------------------------------------------------------------
# psx.map parsing
# ---------------------------------------------------------------------------

_LABEL_RE = re.compile(r"^\s+0x([0-9a-f]+)\s+([A-Za-z_][A-Za-z0-9_]*)\s*$")


def parse_map_labels(map_path: Path) -> List[Tuple[int, str]]:
    """Return list of (addr, name) sorted by addr from psx.map."""
    out: List[Tuple[int, str]] = []
    seen = set()
    with map_path.open() as fh:
        for line in fh:
            m = _LABEL_RE.match(line.rstrip("\n"))
            if not m:
                continue
            addr = int(m.group(1), 16)
            name = m.group(2)
            if addr < 0x80000000 or addr >= 0x80800000:
                continue
            key = (addr, name)
            if key in seen:
                continue
            seen.add(key)
            out.append((addr, name))
    out.sort()
    return out


def label_gap(labels: List[Tuple[int, str]], name: str) -> Optional[Tuple[int, int]]:
    """Return (addr, gap_to_next_label) for `name`, or None if not found."""
    for i, (addr, n) in enumerate(labels):
        if n != name:
            continue
        # Find next strictly-greater addr
        for j in range(i + 1, len(labels)):
            naddr, _ = labels[j]
            if naddr > addr:
                return addr, naddr - addr
        return addr, 0
    return None


# ---------------------------------------------------------------------------
# Resolve a global's declared type -> (struct_name, array_count)
# ---------------------------------------------------------------------------

@dataclass
class GlobalDecl:
    name: str
    record_name: str
    array_count: int  # 1 if scalar
    psx_size: int     # struct.psx_size * array_count
    struct: StructLayout


def find_global_layout(parsed_cursor: ci.Cursor,
                       layouts: Dict[str, StructLayout],
                       global_name: str) -> Optional[GlobalDecl]:
    """Walk the TU for an extern/decl of `global_name` and return its layout."""
    for c in parsed_cursor.walk_preorder():
        if c.kind != ci.CursorKind.VAR_DECL:
            continue
        if c.spelling != global_name:
            continue
        t = c.type
        count = 1
        canon = t.get_canonical()
        if canon.kind == ci.TypeKind.CONSTANTARRAY:
            count = canon.element_count
            elem = canon.element_type.get_canonical()
        else:
            elem = canon
        if elem.kind != ci.TypeKind.RECORD:
            continue
        rec_name = elem.get_declaration().spelling
        # Look up by tag spelling first; if anon-via-typedef, use the typedef.
        layout = layouts.get(rec_name)
        if layout is None:
            # Try resolving via typedef chain on `t` (un-canonicalized).
            tt = t
            if tt.kind == ci.TypeKind.CONSTANTARRAY:
                tt = tt.element_type
            type_spell = tt.spelling.replace("const ", "").strip()
            layout = layouts.get(type_spell)
        if layout is None:
            # Anonymous inline struct (`} g_Foo;` form) - compute from
            # the var's record decl directly.
            decl = elem.get_declaration()
            if decl is not None and decl.is_definition():
                try:
                    layout = layout_struct(decl)
                except ValueError:
                    layout = None
        if layout is None:
            return None
        return GlobalDecl(
            name=global_name,
            record_name=layout.name,
            array_count=count,
            psx_size=layout.psx_size * count,
            struct=layout,
        )
    return None


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main() -> int:
    if not PSX_MAP.exists():
        print(f"missing {PSX_MAP}", file=sys.stderr)
        return 2
    labels = parse_map_labels(PSX_MAP)
    print(f"parsed {len(labels)} labels from psx.map")

    # Synthesize a single TU that pulls in every relevant header.
    import tempfile
    src = "\n".join(f'#include "{h}"' for h in SYNTHETIC_HEADERS) + "\n"
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as fh:
        fh.write(src)
        synth_path = fh.name
    parsed = parse_tu(synth_path)
    cursor = parsed.cursor
    layouts = collect_record_layouts(cursor)

    fails = 0
    checks = 0
    for global_name in TARGETS:
        gl = find_global_layout(cursor, layouts, global_name)
        gap = label_gap(labels, global_name)

        checks += 1
        if gl is None:
            print(f"  ?? {global_name:24}  could not resolve layout")
            fails += 1
            continue
        if gap is None:
            print(f"  ?? {global_name:24}  not found in psx.map")
            fails += 1
            continue

        addr, map_gap = gap
        verdict = "OK"
        if map_gap and gl.psx_size > map_gap:
            verdict = "FAIL (computed > gap)"
            fails += 1
        elif map_gap == 0:
            verdict = "OK (last label)"
        elif gl.psx_size == map_gap:
            verdict = "EXACT"
        elif gl.psx_size < map_gap:
            verdict = f"OK (gap +0x{map_gap - gl.psx_size:x} unlabeled)"

        arr = f"[{gl.array_count}]" if gl.array_count != 1 else ""
        print(f"  {verdict:32}  {global_name:24}  "
              f"{gl.record_name}{arr}  size=0x{gl.psx_size:x}  "
              f"addr=0x{addr:x}  gap=0x{map_gap:x}")

    print(f"\n{checks - fails}/{checks} layouts validated")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
