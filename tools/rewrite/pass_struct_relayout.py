"""Pass 1 — PSX struct relayout.

For every game-defined struct (under spyro-1/include/) that the TU
references and whose host layout would diverge from PSX layout, rewrite
its diverging fields in-place inside the *header*:

    void *m_OutBuf;     →   uint32_t m_OutBuf;     (PSX vaddr slot)
    long  m_Field;      →   int32_t  m_Field;
    u_long m_Field;     →   uint32_t m_Field;
    void *m_Cb;         →   uint32_t m_Cb;         (skipped if fn-ptr — see below)
    void *m_Arr[N];     →   uint32_t m_Arr[N];

Each rewritten *header* lands in a per-TU shadow tree
${SPYRO_GENERATED}/rewritten/<tu>/include/<rel-path>; the TU's compile
invocation prepends that tree to its include path so the rewritten
defs win.

Field-access conversions in the .c body:
    READ:   `obj.m_OutBuf` (used as void*) → `(void*)openpete_psx_to_host_ptr(obj.m_OutBuf)`
    WRITE:  `obj.m_OutBuf = host_ptr;`     → `obj.m_OutBuf = openpete_host_to_psx(host_ptr);`
    READ:   `obj.m_LongFld` (used as long) → `(int32_t)obj.m_LongFld`
    WRITE:  `obj.m_LongFld = expr;`         → `obj.m_LongFld = (int32_t)(expr);`

Out of scope (deferred):
  - PSYQ structs (defined under spyro-1/psyq/include/) — Phase 1.5 audit.
  - Function-pointer fields — flagged as warnings, not rewritten.
  - sizeof() agreement, struct-by-value, struct-pointer arithmetic.
"""

from __future__ import annotations

from dataclasses import dataclass, field as dc_field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

import clang.cindex as ci

from .rewriter import Edit
from .types import (
    StructLayout, layout_struct, _is_long_family, _is_pointer_kind,
    _is_function_ptr,
)


def _resolve_function_psx_vaddr(rhs: ci.Cursor, symbols: Dict[str, int]) -> Optional[int]:
    """If `rhs` evaluates to a function reference whose PSX vaddr is in
    `symbols`, return the vaddr; else None.

    Handles direct DeclRefExpr to a FUNCTION_DECL, plus a single layer of
    UNEXPOSED_EXPR / CSTYLE_CAST_EXPR / PAREN_EXPR wrapping (the
    function-to-pointer decay produces an UNEXPOSED_EXPR around the ref).
    """
    cur = rhs
    for _ in range(4):
        if cur.kind == ci.CursorKind.DECL_REF_EXPR:
            ref = cur.referenced
            if ref is not None and ref.kind == ci.CursorKind.FUNCTION_DECL:
                name = ref.spelling
                if name in symbols:
                    return symbols[name]
            return None
        kids = list(cur.get_children())
        if not kids:
            return None
        cur = kids[0]
    return None


# ---------------------------------------------------------------------------
# Where structs come from — game vs PSYQ vs system
# ---------------------------------------------------------------------------

def _is_game_header(path: str) -> bool:
    """Returns True for files under spyro-1/include/."""
    p = str(path)
    return "/spyro-1/include/" in p


@dataclass
class FieldKey:
    record_usr: str
    field_name: str

    def __hash__(self) -> int:
        return hash((self.record_usr, self.field_name))

    def __eq__(self, other) -> bool:
        return (isinstance(other, FieldKey)
                and self.record_usr == other.record_usr
                and self.field_name == other.field_name)


@dataclass
class FieldRewrite:
    """One field that Pass 1 will retype + insert conversions for."""
    record_usr: str
    record_name: str
    field_name: str
    field_decl_offset: int   # FIELD_DECL.extent.start.offset
    type_prefix_end: int     # offset of the IDENTIFIER token (= name)
    file: str
    is_pointer: bool
    is_long_signed: bool     # True for long/signed-long; False for u_long/unsigned long
    pointer_orig_type: str   # original type spelling for cast diagnostics
    is_function_ptr: bool


@dataclass
class StructRelayoutResult:
    field_edits: List[Edit] = dc_field(default_factory=list)   # header rewrites
    access_edits: List[Edit] = dc_field(default_factory=list)  # .c-body rewrites
    rewrites: Dict[FieldKey, FieldRewrite] = dc_field(default_factory=dict)
    rewritten_headers: Set[str] = dc_field(default_factory=set)
    fnptr_warnings: List[str] = dc_field(default_factory=list)
    address_of_warnings: List[str] = dc_field(default_factory=list)


# ---------------------------------------------------------------------------
# Discovery — which struct fields need rewriting
# ---------------------------------------------------------------------------

def _identifier_token_offset(decl: ci.Cursor) -> Optional[int]:
    """Return the byte offset of the IDENTIFIER token whose spelling matches
    `decl.spelling` within `decl.extent`. Used to split a FIELD_DECL into
    type-prefix and name+suffix halves."""
    name = decl.spelling
    if not name:
        return None
    tu = decl.translation_unit
    for tok in tu.get_tokens(extent=decl.extent):
        if tok.kind == ci.TokenKind.IDENTIFIER and tok.spelling == name:
            return tok.location.offset
    return None


def collect_field_rewrites(tu_cursor: ci.Cursor) -> Dict[FieldKey, FieldRewrite]:
    """Walk every game-defined struct definition; for each diverging leaf
    field (pointer or long-family scalar / array of those), record a
    FieldRewrite. Fields nested through other structs aren't rewritten
    here — those structs get their own entries.
    """
    rewrites: Dict[FieldKey, FieldRewrite] = {}
    seen_usrs: Set[str] = set()

    for c in tu_cursor.walk_preorder():
        if c.kind not in (ci.CursorKind.STRUCT_DECL, ci.CursorKind.UNION_DECL):
            continue
        if not c.is_definition():
            continue
        if c.location.file is None:
            continue
        if not _is_game_header(c.location.file.name):
            continue
        usr = c.get_usr()
        if usr in seen_usrs:
            continue
        seen_usrs.add(usr)

        try:
            layout = layout_struct(c)
        except ValueError:
            continue
        if not layout.diverges:
            continue

        rec_name = c.spelling or "<anon>"
        for fld in c.get_children():
            if fld.kind != ci.CursorKind.FIELD_DECL:
                continue
            ft = fld.type
            canon = ft.get_canonical()
            elem = canon
            if canon.kind == ci.TypeKind.CONSTANTARRAY:
                elem = canon.element_type.get_canonical()
            is_ptr = _is_pointer_kind(elem)
            is_long = _is_long_family(canon if elem is canon else canon.element_type)
            is_fnptr = is_ptr and _is_function_ptr(elem)
            if not (is_ptr or is_long):
                continue
            ident_off = _identifier_token_offset(fld)
            if ident_off is None:
                continue
            file_ = str(fld.location.file.name) if fld.location.file else ""
            # signedness for long-family
            is_long_signed = False
            if is_long:
                spell = ft.spelling.replace("const ", "").replace("volatile ", "").strip()
                if spell.startswith("u_") or spell.startswith("unsigned"):
                    is_long_signed = False
                else:
                    is_long_signed = True
            rewrites[FieldKey(usr, fld.spelling)] = FieldRewrite(
                record_usr=usr,
                record_name=rec_name,
                field_name=fld.spelling,
                field_decl_offset=fld.extent.start.offset,
                type_prefix_end=ident_off,
                file=file_,
                is_pointer=is_ptr,
                is_long_signed=is_long_signed,
                pointer_orig_type=ft.spelling if is_ptr and not is_fnptr else "",
                is_function_ptr=is_fnptr,
            )
    return rewrites


# ---------------------------------------------------------------------------
# Header field rewrites — emit one Edit per diverging field
# ---------------------------------------------------------------------------

def emit_header_edits(rewrites: Dict[FieldKey, FieldRewrite],
                      result: StructRelayoutResult) -> None:
    for key, fr in rewrites.items():
        if fr.is_function_ptr:
            # Function-pointer fields: leave alone for MVP, warn.
            result.fnptr_warnings.append(
                f"function-pointer field {fr.record_name}.{fr.field_name} "
                f"at {fr.file}: not rewritten (Phase 4)")
            continue
        if fr.is_pointer:
            new_type = "uint32_t "
        elif fr.is_long_signed:
            new_type = "int32_t "
        else:
            new_type = "uint32_t "
        # Replace from FIELD_DECL.start..IDENTIFIER.start with the new type
        # text. The IDENTIFIER and any trailing array/qualifier suffix
        # stay in place.
        result.field_edits.append(Edit(
            start=fr.field_decl_offset,
            end=fr.type_prefix_end,
            replacement=new_type,
            file=fr.file,
            note=f"retype {fr.record_name}.{fr.field_name}",
        ))
        result.rewritten_headers.add(fr.file)


# ---------------------------------------------------------------------------
# .c-body field-access rewrites
# ---------------------------------------------------------------------------

def _find_member_writes(tu_cursor: ci.Cursor,
                        rewrites: Dict[FieldKey, FieldRewrite]
                        ) -> Dict[int, Tuple[ci.Cursor, ci.Cursor, FieldRewrite]]:
    """Return {member_ref_start_offset: (binop, member_ref, rewrite)} for
    every `obj.field = rhs` site where `field` is a rewritten field."""
    writes: Dict[int, Tuple[ci.Cursor, ci.Cursor, FieldRewrite]] = {}
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.BINARY_OPERATOR:
            continue
        children = list(c.get_children())
        if len(children) != 2:
            continue
        lhs, rhs = children
        if lhs.kind != ci.CursorKind.MEMBER_REF_EXPR:
            continue
        ref_decl = lhs.referenced
        if ref_decl is None or ref_decl.kind != ci.CursorKind.FIELD_DECL:
            continue
        parent = ref_decl.semantic_parent
        if parent is None:
            continue
        key = FieldKey(parent.get_usr(), ref_decl.spelling)
        fr = rewrites.get(key)
        if fr is None or fr.is_function_ptr:
            continue
        try:
            tokens = list(c.translation_unit.get_tokens(extent=c.extent))
        except Exception:
            continue
        op_tok = None
        for tok in tokens:
            if tok.location.offset >= lhs.extent.end.offset and \
               tok.location.offset < rhs.extent.start.offset and \
               tok.kind == ci.TokenKind.PUNCTUATION:
                op_tok = tok
                break
        if op_tok is None or op_tok.spelling != "=":
            continue
        writes[lhs.extent.start.offset] = (c, lhs, fr)
    return writes


def _bytes_slice(file_path: str, start: int, end: int) -> str:
    with open(file_path, "rb") as fh:
        fh.seek(start)
        return fh.read(end - start).decode("utf-8", "replace")


def emit_access_edits(tu_cursor: ci.Cursor,
                      rewrites: Dict[FieldKey, FieldRewrite],
                      main_file: str,
                      result: StructRelayoutResult,
                      symbols: Optional[Dict[str, int]] = None) -> None:
    if not rewrites:
        return
    main_resolved = str(Path(main_file).resolve())

    writes = _find_member_writes(tu_cursor, rewrites)

    # Address-of guards.
    addr_of_offsets: Set[int] = set()
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.UNARY_OPERATOR:
            continue
        children = list(c.get_children())
        if len(children) != 1 or children[0].kind != ci.CursorKind.MEMBER_REF_EXPR:
            continue
        try:
            tokens = list(c.translation_unit.get_tokens(extent=c.extent))
        except Exception:
            continue
        if not tokens or tokens[0].spelling != "&":
            continue
        member = children[0]
        ref_decl = member.referenced
        if ref_decl is None or ref_decl.kind != ci.CursorKind.FIELD_DECL:
            continue
        parent = ref_decl.semantic_parent
        if parent is None:
            continue
        key = FieldKey(parent.get_usr(), ref_decl.spelling)
        fr = rewrites.get(key)
        if fr is None:
            continue
        addr_of_offsets.add(member.extent.start.offset)
        result.address_of_warnings.append(
            f"&{fr.record_name}.{fr.field_name} at "
            f"{member.location.file}:{member.location.line} "
            f"— address-of a relayout'd field has no original-type address")

    seen_member_offsets: Set[int] = set()
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.MEMBER_REF_EXPR:
            continue
        if c.location.file is None:
            continue
        if str(Path(c.location.file.name).resolve()) != main_resolved:
            continue
        ref_decl = c.referenced
        if ref_decl is None or ref_decl.kind != ci.CursorKind.FIELD_DECL:
            continue
        parent = ref_decl.semantic_parent
        if parent is None:
            continue
        key = FieldKey(parent.get_usr(), ref_decl.spelling)
        fr = rewrites.get(key)
        if fr is None or fr.is_function_ptr:
            continue
        off = c.extent.start.offset
        if off in addr_of_offsets:
            continue
        # Macro-expansion artifacts: a MEMBER_REF_EXPR from a macro arg
        # used N times in the body shows up as N AST nodes, often with
        # zero-length source extents (start == end). Slicing an empty
        # range yields an empty access_text and the rewrite emits broken
        # `psx_to_host_ptr()` calls. Skip them — the parent macro
        # invocation gets handled separately.
        if c.extent.end.offset == c.extent.start.offset:
            continue
        if off in seen_member_offsets:
            continue
        seen_member_offsets.add(off)

        if off in writes:
            binop, lhs, _ = writes[off]
            rhs_children = list(binop.get_children())
            rhs = rhs_children[1]
            lhs_text = _bytes_slice(main_resolved,
                                    lhs.extent.start.offset, lhs.extent.end.offset)
            rhs_text = _bytes_slice(main_resolved,
                                    rhs.extent.start.offset, rhs.extent.end.offset)
            if fr.is_pointer:
                # If the RHS is a function reference with a known PSX
                # vaddr, substitute the literal — the host fn-ptr lives
                # outside g_ram, so host_to_psx() would yield 0 and
                # corrupt the field. Plan §1.6.3 / §2.3 Pass 1.
                fn_addr = (_resolve_function_psx_vaddr(rhs, symbols)
                           if symbols else None)
                if fn_addr is not None:
                    replacement = f"{lhs_text} = 0x{fn_addr:08X}u"
                else:
                    replacement = (f"{lhs_text} = "
                                   f"openpete_host_to_psx({rhs_text})")
            else:
                cast = "int32_t" if fr.is_long_signed else "uint32_t"
                replacement = f"{lhs_text} = ({cast})({rhs_text})"
            result.access_edits.append(Edit(
                start=binop.extent.start.offset,
                end=binop.extent.end.offset,
                replacement=replacement,
                file=main_resolved,
                note=f"write {fr.record_name}.{fr.field_name}",
            ))
        else:
            access_text = _bytes_slice(main_resolved,
                                       c.extent.start.offset, c.extent.end.offset)
            if fr.is_pointer:
                replacement = (f"(({fr.pointer_orig_type})"
                               f"openpete_psx_to_host_ptr({access_text}))")
            else:
                cast = "int32_t" if fr.is_long_signed else "uint32_t"
                replacement = f"(({cast})({access_text}))"
            result.access_edits.append(Edit(
                start=c.extent.start.offset,
                end=c.extent.end.offset,
                replacement=replacement,
                file=main_resolved,
                note=f"read {fr.record_name}.{fr.field_name}",
            ))


# ---------------------------------------------------------------------------
# Public entry
# ---------------------------------------------------------------------------

def run(tu_cursor: ci.Cursor, main_file: str,
        symbols: Optional[Dict[str, int]] = None) -> StructRelayoutResult:
    result = StructRelayoutResult()
    rewrites = collect_field_rewrites(tu_cursor)
    result.rewrites = rewrites
    emit_header_edits(rewrites, result)
    emit_access_edits(tu_cursor, rewrites, main_file, result, symbols)
    return result
