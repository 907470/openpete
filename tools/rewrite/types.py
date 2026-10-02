"""PSX (ILP32) type-layout helper.

Because parser.py runs libclang against the `mipsel-unknown-elf` target,
clang itself returns ILP32 sizes/offsets - `sizeof(long) == 4`,
`sizeof(void *) == 4`, struct field offsets laid out per MIPS R3000 ABI.
We therefore lean on clang's own type metrics rather than re-implementing
the layout algorithm. This module's job is to *classify* types from a
rewriter's perspective: which ones diverge in width between PSX (ILP32)
and host (LP64 / LLP64), so Pass 1 (struct relayout) and Pass 2 (bare
globals) know what to rewrite.

Width-divergent primitives:
  long / unsigned long / u_long      -  4 (PSX) vs 8 (LP64), 4 (LLP64)
  any pointer type                   -  4 (PSX) vs 8 (LP64 + LLP64)

Composite types diverge if they contain any width-divergent leaf field
(transitively through nested structs / arrays).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import clang.cindex as ci


# Bytes per primitive on PSX target.
PSX_PRIMITIVE_SIZE = {
    "char": 1, "signed char": 1, "unsigned char": 1, "u_char": 1,
    "short": 2, "signed short": 2, "unsigned short": 2, "u_short": 2,
    "int": 4, "signed int": 4, "unsigned int": 4, "u_int": 4,
    "long": 4, "signed long": 4, "unsigned long": 4, "u_long": 4,
    "long long": 8, "unsigned long long": 8, "u_long_long": 8,
    "float": 4, "double": 8,
}


@dataclass
class FieldLayout:
    name: str
    psx_offset: int            # byte offset within parent (PSX rules)
    psx_size: int              # bytes the field occupies
    type_spelling: str         # canonical-ish type spelling for diagnostics
    is_pointer: bool
    is_long_family: bool       # 'long', 'u_long', 'unsigned long'
    is_function_ptr: bool
    diverges: bool             # host width != PSX width (transitive)
    inner_record: Optional[str] = None  # qualified spelling of nested record, if any


@dataclass
class StructLayout:
    name: str                  # tag name or typedef-derived name; "" if anonymous
    psx_size: int
    psx_align: int
    fields: List[FieldLayout] = field(default_factory=list)
    diverges: bool = False     # any field diverges (transitive)

    def offset_of(self, field_name: str) -> Optional[int]:
        for f in self.fields:
            if f.name == field_name:
                return f.psx_offset
        return None


# ---------------------------------------------------------------------------
# Type classification helpers
# ---------------------------------------------------------------------------

_LONG_FAMILY_SPELLINGS = {"long", "unsigned long", "signed long",
                          "u_long", "long int", "unsigned long int"}


def _strip_quals(spelling: str) -> str:
    out = spelling
    for q in ("const ", "volatile ", "restrict "):
        out = out.replace(q, "")
    return out.strip()


def _is_pointer_kind(t: ci.Type) -> bool:
    return t.kind in (ci.TypeKind.POINTER, ci.TypeKind.MEMBERPOINTER)


def _is_long_family(t: ci.Type) -> bool:
    canon = t.get_canonical()
    if canon.kind in (ci.TypeKind.LONG, ci.TypeKind.ULONG):
        return True
    spelling = _strip_quals(t.spelling)
    return spelling in _LONG_FAMILY_SPELLINGS


def _is_function_ptr(t: ci.Type) -> bool:
    if not _is_pointer_kind(t):
        return False
    pointee = t.get_pointee().get_canonical()
    return pointee.kind == ci.TypeKind.FUNCTIONPROTO or \
           pointee.kind == ci.TypeKind.FUNCTIONNOPROTO


def type_diverges(t: ci.Type, _seen: Optional[set] = None) -> bool:
    """True if this type's host width differs from its PSX width."""
    if _seen is None:
        _seen = set()
    canon = t.get_canonical()
    # Pointers always diverge: PSX 4, host 8 (LP64 / LLP64).
    if _is_pointer_kind(canon):
        return True
    # long / u_long: 4 PSX, 8 LP64 (4 LLP64). Treat as divergent - we
    # normalize to int32 either way to avoid host-ABI sensitivity.
    if _is_long_family(t):
        return True
    if canon.kind == ci.TypeKind.CONSTANTARRAY:
        return type_diverges(canon.element_type, _seen)
    if canon.kind == ci.TypeKind.INCOMPLETEARRAY:
        return type_diverges(canon.element_type, _seen)
    if canon.kind == ci.TypeKind.RECORD:
        decl = canon.get_declaration()
        usr = decl.get_usr() or decl.spelling
        if usr in _seen:
            return False  # cycle break
        _seen.add(usr)
        for fld in decl.get_children():
            if fld.kind != ci.CursorKind.FIELD_DECL:
                continue
            if type_diverges(fld.type, _seen):
                return True
    return False


# ---------------------------------------------------------------------------
# Layout extraction
# ---------------------------------------------------------------------------

def layout_struct(decl: ci.Cursor) -> StructLayout:
    """Build a StructLayout from a clang RecordDecl cursor.

    The cursor must be a DEFINITION - forward decls have no offsets.
    Uses clang's mipsel-target metrics for size/align/offsets.
    """
    if decl.kind not in (ci.CursorKind.STRUCT_DECL, ci.CursorKind.UNION_DECL):
        raise ValueError(f"layout_struct: not a struct/union decl: {decl.kind}")
    if not decl.is_definition():
        decl = decl.get_definition()
        if decl is None:
            raise ValueError("layout_struct: forward decl with no definition")
    t = decl.type
    layout = StructLayout(
        name=decl.spelling or "",
        psx_size=t.get_size(),
        psx_align=t.get_align(),
    )
    for fld in decl.get_children():
        if fld.kind != ci.CursorKind.FIELD_DECL:
            continue
        offset_bits = t.get_offset(fld.spelling)
        if offset_bits < 0:
            # Bitfield without a stable offset, or anonymous member - skip.
            continue
        ft = fld.type
        canon = ft.get_canonical()
        inner_record = None
        if canon.kind == ci.TypeKind.RECORD:
            inner_record = canon.get_declaration().spelling
        layout.fields.append(FieldLayout(
            name=fld.spelling,
            psx_offset=offset_bits // 8,
            psx_size=ft.get_size(),
            type_spelling=ft.spelling,
            is_pointer=_is_pointer_kind(canon),
            is_long_family=_is_long_family(ft),
            is_function_ptr=_is_function_ptr(canon),
            diverges=type_diverges(ft),
            inner_record=inner_record,
        ))
    layout.diverges = any(f.diverges for f in layout.fields)
    return layout


def collect_record_layouts(tu_cursor: ci.Cursor) -> Dict[str, StructLayout]:
    """Walk a TU and return {name: StructLayout} for every defined record.

    Records are keyed by their tag spelling when present, and also by every
    typedef alias that resolves to them (e.g. `typedef struct { ... } CdState;`
    is registered under "CdState" even though the struct decl is anonymous).
    """
    out: Dict[str, StructLayout] = {}
    seen_usrs: set = set()

    def _record_layout(decl: ci.Cursor, name: str) -> None:
        if not name or name in out:
            return
        try:
            layout = layout_struct(decl)
        except ValueError:
            return
        # Override layout.name with the alias key for prettier diagnostics
        # while leaving offsets/sizes untouched.
        if not layout.name:
            layout.name = name
        out[name] = layout

    for c in tu_cursor.walk_preorder():
        if c.kind in (ci.CursorKind.STRUCT_DECL, ci.CursorKind.UNION_DECL):
            if not c.is_definition():
                continue
            usr = c.get_usr()
            if usr and usr in seen_usrs:
                continue
            if usr:
                seen_usrs.add(usr)
            if c.spelling:
                _record_layout(c, c.spelling)
        elif c.kind == ci.CursorKind.TYPEDEF_DECL:
            t = c.underlying_typedef_type.get_canonical()
            if t.kind != ci.TypeKind.RECORD:
                continue
            decl = t.get_declaration()
            if decl is None or not decl.is_definition():
                continue
            _record_layout(decl, c.spelling)
    return out


# ---------------------------------------------------------------------------
# Quick CLI: dump layouts of named structs in a TU
# ---------------------------------------------------------------------------

def _main(argv: List[str]) -> int:
    import sys
    from .parser import parse_tu, SPYRO_REPO
    src = argv[1] if len(argv) > 1 else str(SPYRO_REPO / "src" / "initialization.c")
    wanted = set(argv[2:])
    parsed = parse_tu(src)
    layouts = collect_record_layouts(parsed.cursor)
    for name, layout in sorted(layouts.items()):
        if wanted and name not in wanted:
            continue
        flag = " *" if layout.diverges else ""
        print(f"struct {name}: size={layout.psx_size:#x} align={layout.psx_align}{flag}")
        for f in layout.fields:
            mark = []
            if f.is_pointer: mark.append("ptr")
            if f.is_long_family: mark.append("long")
            if f.is_function_ptr: mark.append("fnptr")
            if f.diverges: mark.append("DIVERGES")
            tag = f" [{','.join(mark)}]" if mark else ""
            print(f"  +{f.psx_offset:#06x}  size={f.psx_size:#04x}  "
                  f"{f.type_spelling:32}  {f.name}{tag}")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(_main(sys.argv))
