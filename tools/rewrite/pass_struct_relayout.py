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


import re as _re
_FN_HEX_SUFFIX_RE = _re.compile(r"_([0-9A-Fa-f]{8})$")


def _resolve_function_psx_vaddr(rhs: ci.Cursor, symbols: Dict[str, int]) -> Optional[int]:
    """If `rhs` evaluates to a function reference whose PSX vaddr is
    known (either from `symbols.toml` or from the trailing-hex naming
    convention used by overlay-defined `func_level_<lvl>_<HEX>` decls
    that aren't catalogued by index), return the vaddr; else None.

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
                m = _FN_HEX_SUFFIX_RE.search(name)
                if m:
                    try:
                        return int(m.group(1), 16)
                    except ValueError:
                        return None
            return None
        kids = list(cur.get_children())
        if not kids:
            return None
        cur = kids[0]
    return None


# ---------------------------------------------------------------------------
# Where structs come from — game vs PSYQ vs system
# ---------------------------------------------------------------------------

def _is_int_to_ptr_cast(cursor: ci.Cursor) -> bool:
    """True if `cursor` is `(PtrT)(int_expr)` — a C-style cast whose target
    is a pointer type and whose operand has integer kind. Used by Pass 1's
    pointer-field write rendering to skip host_to_psx on literal PSX-vaddr
    integer expressions like `(void*)(0x80200000 - X)`."""
    c = cursor
    while c.kind == ci.CursorKind.UNEXPOSED_EXPR:
        kids = list(c.get_children())
        if not kids:
            return False
        c = kids[0]
    if c.kind != ci.CursorKind.CSTYLE_CAST_EXPR:
        return False
    target = c.type.get_canonical()
    if not _is_pointer_kind(target):
        return False
    kids = list(c.get_children())
    operand = kids[-1] if kids else None
    if operand is None:
        return False
    op_canon = operand.type.get_canonical()
    return op_canon.kind in (
        ci.TypeKind.INT, ci.TypeKind.UINT, ci.TypeKind.LONG,
        ci.TypeKind.ULONG, ci.TypeKind.LONGLONG, ci.TypeKind.ULONGLONG,
        ci.TypeKind.SHORT, ci.TypeKind.USHORT, ci.TypeKind.CHAR_S,
        ci.TypeKind.CHAR_U, ci.TypeKind.SCHAR, ci.TypeKind.UCHAR,
    )


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
    is_array: bool = False           # field type is `T arr[N]` (array)
    elem_orig_type: str = ""         # element type for array fields (e.g. "AnimationHeader *")
    is_ptr_to_ptr: bool = False      # field type is `T **f` — points to a PSX-laid-out array of vaddrs
    is_opaque_pad: bool = False      # nested non-game record diverges (e.g. PsyCross DRAWENV)
    opaque_pad_size: int = 0         # PSX size to pad to when is_opaque_pad
    opaque_pad_orig_type: str = ""   # original type spelling for diagnostics
    field_extent_end: int = 0        # FIELD_DECL.extent.end.offset (used for opaque pad rewrite)


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

    # Field-set: keys of game-struct fields whose subfields are accessed
    # anywhere in this TU (`obj.field.subfield` or `(&obj.field)->...`).
    # We skip the opaque-pad rewrite for those, leaving the host's native
    # layout — same as before this pass landed. Pad only fields with
    # zero subfield access in the TU.
    accessed_subfield_keys: Set[Tuple[str, str]] = set()
    for k in tu_cursor.walk_preorder():
        if k.kind != ci.CursorKind.MEMBER_REF_EXPR:
            continue
        kids = list(k.get_children())
        if not kids:
            continue
        base = kids[0]
        # Outer member-ref; if its base is itself a MEMBER_REF_EXPR (or an
        # ARRAY_SUBSCRIPT_EXPR / paren etc. wrapping one), the inner
        # member-ref names the field whose subfield we're touching.
        inner = base
        while inner.kind in (ci.CursorKind.UNEXPOSED_EXPR,
                             ci.CursorKind.PAREN_EXPR,
                             ci.CursorKind.ARRAY_SUBSCRIPT_EXPR,
                             ci.CursorKind.UNARY_OPERATOR):
            ikids = list(inner.get_children())
            if not ikids:
                break
            inner = ikids[0]
        if inner.kind != ci.CursorKind.MEMBER_REF_EXPR:
            continue
        rd = inner.referenced
        if rd is None or rd.kind != ci.CursorKind.FIELD_DECL:
            continue
        parent = rd.semantic_parent
        if parent is None:
            continue
        accessed_subfield_keys.add((parent.get_usr(), rd.spelling))

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
                # Nested-record case: a field whose type is a struct/union
                # (or array thereof) defined OUTSIDE spyro-1/include/ — i.e.
                # PsyCross / PSYQ — and whose host vs PSX size diverges
                # because the foreign struct embeds host pointers (e.g.
                # DRAWENV's DR_ENV → uintptr_t addr). Replace with a
                # PSX-sized byte-array pad in the per-TU shadow header so
                # parent-struct layouts match PSX. Subfield access through
                # the pad is forbidden and surfaces as a hard error below.
                rec_t = elem if elem.kind == ci.TypeKind.RECORD else canon
                if rec_t.kind == ci.TypeKind.RECORD:
                    inner_decl = rec_t.get_declaration()
                    inner_file = (str(inner_decl.location.file.name)
                                  if inner_decl.location and inner_decl.location.file
                                  else "")
                    if inner_file and not _is_game_header(inner_file):
                        from .types import type_diverges as _td
                        if _td(ft) and (usr, fld.spelling) not in accessed_subfield_keys:
                            ident_off = _identifier_token_offset(fld)
                            if ident_off is None:
                                continue
                            file_ = str(fld.location.file.name) if fld.location.file else ""
                            psx_field_size = ft.get_size()
                            rewrites[FieldKey(usr, fld.spelling)] = FieldRewrite(
                                record_usr=usr,
                                record_name=rec_name,
                                field_name=fld.spelling,
                                field_decl_offset=fld.extent.start.offset,
                                type_prefix_end=ident_off,
                                file=file_,
                                is_pointer=False,
                                is_long_signed=False,
                                pointer_orig_type="",
                                is_function_ptr=False,
                                is_array=False,
                                elem_orig_type="",
                                is_ptr_to_ptr=False,
                                is_opaque_pad=True,
                                opaque_pad_size=psx_field_size,
                                opaque_pad_orig_type=ft.spelling,
                                field_extent_end=fld.extent.end.offset,
                            )
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
            is_array = canon.kind == ci.TypeKind.CONSTANTARRAY
            elem_orig_type = ""
            is_ptr_to_ptr = False
            if is_array and is_ptr and not is_fnptr:
                # Element type spelling — for `T *m_F[N]` we want `T *`
                # so element-access rewrites cast each PSX vaddr back to
                # the original element pointer type.
                elem_orig_type = ft.element_type.spelling \
                    if ft.kind == ci.TypeKind.CONSTANTARRAY \
                    else canon.element_type.spelling
            elif is_ptr and not is_array and not is_fnptr:
                # `T **f` (or `u_long *f`-equivalent) — the field holds a
                # PSX vaddr to an array of PSX vaddrs. Subscripting must
                # convert each element through psx_to_host_ptr separately.
                pointee = elem.get_pointee()
                if pointee.kind != ci.TypeKind.INVALID and \
                        _is_pointer_kind(pointee.get_canonical()) and \
                        not _is_function_ptr(pointee.get_canonical()):
                    is_ptr_to_ptr = True
                    # elem_orig_type holds the inner pointer type, e.g.
                    # "SpecialSurface *" for `SpecialSurface **`.
                    elem_orig_type = ft.get_pointee().spelling \
                        if ft.kind != ci.TypeKind.CONSTANTARRAY \
                        else canon.get_pointee().spelling
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
                is_array=is_array,
                elem_orig_type=elem_orig_type,
                is_ptr_to_ptr=is_ptr_to_ptr,
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
        if fr.is_opaque_pad:
            # Replace the entire FIELD_DECL with a byte-array pad sized to
            # match PSX. The foreign nested struct (e.g. PsyCross DRAWENV)
            # has a bigger host layout — opaque storage on the native side,
            # written by the recompiled-PSX side which sees PSX layout.
            replacement = (
                f"char {fr.field_name}[{fr.opaque_pad_size}]"
                f" /* opaque PSX-layout pad for {fr.opaque_pad_orig_type} */"
            )
            result.field_edits.append(Edit(
                start=fr.field_decl_offset,
                end=fr.field_extent_end,
                replacement=replacement,
                file=fr.file,
                note=f"opaque-pad {fr.record_name}.{fr.field_name}",
            ))
            result.rewritten_headers.add(fr.file)
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

# Compound-assignment operators we recognize as writes. A compound
# assign rewrites as `lhs = (cast)((lhs_read_form) BINOP rhs)` so the
# relaid storage is updated through one assignment expression.
_COMPOUND_ASSIGN_OPS = frozenset({
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
})


def _find_member_writes(tu_cursor: ci.Cursor,
                        rewrites: Dict[FieldKey, FieldRewrite]
                        ) -> Dict[int, Tuple[ci.Cursor, ci.Cursor, FieldRewrite, str]]:
    """Return {member_ref_start_offset: (binop, member_ref, rewrite, op_spelling)}
    for every `obj.field = rhs` or `obj.field <op>= rhs` site where
    `field` is a rewritten field. `op_spelling` is "=" for plain
    assignment or a compound op like "+=", "&=", etc."""
    writes: Dict[int, Tuple[ci.Cursor, ci.Cursor, FieldRewrite, str]] = {}
    for c in tu_cursor.walk_preorder():
        if c.kind not in (ci.CursorKind.BINARY_OPERATOR,
                          ci.CursorKind.COMPOUND_ASSIGNMENT_OPERATOR):
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
        if fr is None or fr.is_function_ptr or fr.is_opaque_pad:
            continue
        try:
            tokens = list(c.translation_unit.get_tokens(extent=c.extent))
        except Exception:
            continue
        # Find the assignment operator token between lhs and rhs. For
        # multi-character ops (`+=`, `<<=`), libclang's tokenizer emits
        # them as a single PUNCTUATION token.
        op_str = None
        for tok in tokens:
            if tok.location.offset >= lhs.extent.end.offset and \
               tok.location.offset < rhs.extent.start.offset and \
               tok.kind == ci.TokenKind.PUNCTUATION:
                if tok.spelling == "=" or tok.spelling in _COMPOUND_ASSIGN_OPS:
                    op_str = tok.spelling
                    break
        if op_str is None:
            continue
        writes[lhs.extent.start.offset] = (c, lhs, fr, op_str)
    return writes


def _bytes_slice(file_path: str, start: int, end: int) -> str:
    with open(file_path, "rb") as fh:
        fh.seek(start)
        return fh.read(end - start).decode("utf-8", "replace")


def _render_with_redirects(cursor: ci.Cursor,
                           main_resolved: str,
                           redirects,
                           rewrites: Optional[Dict[FieldKey, "FieldRewrite"]] = None,
                           include_self: bool = False
                           ) -> str:
    """Read cursor's source slice, substituting Pass 2's redirected-global
    read form for any DeclRefExpr inside the slice and Pass 1's read form
    for any nested member-ref to a relaid field.

    Without nested substitution Pass 1's outer write/read edits carry raw
    inner expressions, and Pass 1 then separately emits an inner read edit
    that overlaps. Folding both inline keeps a single edit per outermost
    site.
    """
    start = cursor.extent.start.offset
    end = cursor.extent.end.offset
    if end == start:
        # Macro-arg cursor with zero-length extent. Use the cursor's
        # tokens (which still report textual positions) to synthesize
        # the source range.
        try:
            toks = list(cursor.translation_unit.get_tokens(extent=cursor.extent))
        except Exception:
            toks = []
        if toks:
            start = toks[0].location.offset
            end = toks[-1].location.offset + len(toks[-1].spelling)
    text = _bytes_slice(main_resolved, start, end)
    if not redirects and not rewrites:
        return text
    subs: List[Tuple[int, int, str]] = []
    seen: Set[int] = set()
    cursor_self_extent = (cursor.extent.start.offset, cursor.extent.end.offset)
    # First pass: detect ARRAY_SUBSCRIPT_EXPR sites whose base is an
    # array-redirect DeclRefExpr; emit the per-element host-pointer
    # form there and mark the inner DeclRefExpr to skip below.
    array_sub_inner_skips: Set[int] = set()
    for k in cursor.walk_preorder():
        if k.kind != ci.CursorKind.ARRAY_SUBSCRIPT_EXPR or not redirects:
            continue
        kids = list(k.get_children())
        if not kids:
            continue
        base = kids[0]
        while base.kind == ci.CursorKind.UNEXPOSED_EXPR:
            bkids = list(base.get_children())
            if not bkids:
                break
            base = bkids[0]
        if base.kind != ci.CursorKind.DECL_REF_EXPR:
            continue
        rd = base.referenced
        if rd is None:
            continue
        red = redirects.get(rd.spelling)
        if red is None or not red.is_array:
            continue
        ks = k.extent.start.offset
        ke = k.extent.end.offset
        if ke == ks or ks < start or ke > end:
            continue
        if ks in seen:
            continue
        seen.add(ks)
        idx = kids[1] if len(kids) > 1 else None
        idx_text = (_bytes_slice(main_resolved,
                                 idx.extent.start.offset, idx.extent.end.offset)
                    if idx is not None else "")
        elem_access = f"{red.psx_storage_name}[{idx_text}]"
        if red.is_pointer:
            repl = (f"(({red.elem_orig_type})"
                    f"openpete_psx_to_host_ptr({elem_access}))")
        else:
            repl = f"((int32_t){elem_access})"
        subs.append((ks - start, ke - start, repl))
        array_sub_inner_skips.add(base.extent.start.offset)
    for k in cursor.walk_preorder():
        if k.location.file is None:
            continue
        if str(Path(k.location.file.name).resolve()) != main_resolved:
            continue
        # Skip the cursor itself unless include_self is set (compound-
        # assign LHS rendering needs the read form OF the top-level
        # member-ref, not just its inner refs).
        if not include_self and \
                (k.extent.start.offset, k.extent.end.offset) == cursor_self_extent \
                and k.kind == cursor.kind:
            continue
        if k.kind == ci.CursorKind.DECL_REF_EXPR and redirects:
            rd = k.referenced
            if rd is None:
                continue
            red = redirects.get(rd.spelling)
            if red is None:
                continue
            ks = k.extent.start.offset
            ke = k.extent.end.offset
            if ke == ks:
                # Macro-arg synthetic extent — recover from tokens.
                try:
                    ktoks = list(k.translation_unit.get_tokens(extent=k.extent))
                except Exception:
                    ktoks = []
                if not ktoks:
                    continue
                ks = ktoks[0].location.offset
                ke = ktoks[-1].location.offset + len(ktoks[-1].spelling)
            if ke <= ks or ks < start or ke > end:
                continue
            if ks in seen or ks in array_sub_inner_skips:
                continue
            seen.add(ks)
            # Array redirect referenced bare (no subscript) — rare;
            # decay to pointer-to-elem.
            if red.is_array:
                if red.is_pointer:
                    repl = (f"(({red.elem_orig_type} *)"
                            f"openpete_psx_to_host_ptr"
                            f"((uint32_t)(uintptr_t){red.psx_storage_name}))")
                else:
                    repl = f"({red.psx_storage_name})"
            elif red.is_pointer:
                repl = (f"(({red.pointer_orig_type})"
                        f"openpete_psx_to_host_ptr({red.psx_storage_name}))")
            else:
                repl = f"((int32_t){red.psx_storage_name})"
            subs.append((ks - start, ke - start, repl))
        elif k.kind == ci.CursorKind.MEMBER_REF_EXPR and rewrites:
            rd = k.referenced
            if rd is None or rd.kind != ci.CursorKind.FIELD_DECL:
                continue
            parent = rd.semantic_parent
            if parent is None:
                continue
            fr = rewrites.get(FieldKey(parent.get_usr(), rd.spelling))
            if fr is None or fr.is_function_ptr or fr.is_opaque_pad:
                continue
            # Skip array-of-pointer fields here — those are handled at
            # the subscript level by the caller, not at the member ref.
            if fr.is_array and fr.is_pointer:
                continue
            ks = k.extent.start.offset
            ke = k.extent.end.offset
            if ke == ks or ks < start or ke > end:
                continue
            if ks in seen:
                continue
            seen.add(ks)
            inner = _bytes_slice(main_resolved, ks, ke)
            if redirects:
                for name, red in redirects.items():
                    if name in inner:
                        if red.is_pointer:
                            r2 = (f"(({red.pointer_orig_type})"
                                  f"openpete_psx_to_host_ptr"
                                  f"({red.psx_storage_name}))")
                        else:
                            r2 = f"((int32_t){red.psx_storage_name})"
                        inner = inner.replace(name, r2)
            if fr.is_pointer:
                repl = (f"(({fr.pointer_orig_type})"
                        f"openpete_psx_to_host_ptr({inner}))")
            else:
                cast = "int32_t" if fr.is_long_signed else "uint32_t"
                repl = f"(({cast})({inner}))"
            subs.append((ks - start, ke - start, repl))
    if not subs:
        return text
    subs.sort()
    # Drop inner subs whose range falls inside a wider sub (outermost wins).
    pruned: List[Tuple[int, int, str]] = []
    for s, e, r in subs:
        if pruned and s >= pruned[-1][0] and e <= pruned[-1][1]:
            continue
        pruned.append((s, e, r))
    out = text
    for rs, re_, repl in reversed(pruned):
        out = out[:rs] + repl + out[re_:]
    return out


def _array_subscript_field_ref(sub: ci.Cursor,
                               rewrites: Dict[FieldKey, FieldRewrite]
                               ) -> Optional[Tuple[ci.Cursor, "FieldRewrite"]]:
    """If `sub` is `member.field[idx]` or `obj->field[idx]` where `field`
    is an array-of-pointer relaid field, return (member_ref_cursor, fr).
    Otherwise return None."""
    if sub.kind != ci.CursorKind.ARRAY_SUBSCRIPT_EXPR:
        return None
    children = list(sub.get_children())
    if not children:
        return None
    base = children[0]
    # Strip a single layer of UNEXPOSED_EXPR (array-to-pointer decay).
    while base.kind == ci.CursorKind.UNEXPOSED_EXPR:
        kids = list(base.get_children())
        if not kids:
            break
        base = kids[0]
    if base.kind != ci.CursorKind.MEMBER_REF_EXPR:
        return None
    rd = base.referenced
    if rd is None or rd.kind != ci.CursorKind.FIELD_DECL:
        return None
    parent = rd.semantic_parent
    if parent is None:
        return None
    fr = rewrites.get(FieldKey(parent.get_usr(), rd.spelling))
    if fr is None or fr.is_function_ptr:
        return None
    is_array_of_ptr = fr.is_array and fr.is_pointer
    if not (is_array_of_ptr or fr.is_ptr_to_ptr):
        return None
    return base, fr


def emit_access_edits(tu_cursor: ci.Cursor,
                      rewrites: Dict[FieldKey, FieldRewrite],
                      main_file: str,
                      result: StructRelayoutResult,
                      symbols: Optional[Dict[str, int]] = None,
                      redirects=None,
                      pass2_write_ranges: Optional[List[Tuple[int, int]]] = None
                      ) -> None:
    if not rewrites:
        return
    main_resolved = str(Path(main_file).resolve())
    p2_writes = sorted(pass2_write_ranges or [])

    def _inside_pass2_write(s: int, e: int) -> bool:
        for ws, we in p2_writes:
            if s >= ws and e <= we:
                return True
            if ws > s:
                break
        return False

    writes = _find_member_writes(tu_cursor, rewrites)

    # Array-of-pointer fields: rewrite at `obj.arr[i]` rather than at
    # `obj.arr` so each PSX-vaddr element is converted to a host pointer
    # individually. Casting `(T *[N])` is invalid C; per-element casts
    # `(T*)psx_to_host_ptr(obj.arr[i])` work uniformly.
    array_subscript_edits: Dict[int, Edit] = {}
    skip_member_ref_offsets: Set[int] = set()
    for sub in tu_cursor.walk_preorder():
        match = _array_subscript_field_ref(sub, rewrites)
        if match is None:
            continue
        member, fr = match
        if sub.location.file is None:
            continue
        if str(Path(sub.location.file.name).resolve()) != main_resolved:
            continue
        s = sub.extent.start.offset
        e = sub.extent.end.offset
        if e == s:
            try:
                toks = list(sub.translation_unit.get_tokens(extent=sub.extent))
            except Exception:
                toks = []
            if not toks:
                continue
            s = toks[0].location.offset
            e = toks[-1].location.offset + len(toks[-1].spelling)
        if s in array_subscript_edits:
            continue
        cast = fr.elem_orig_type or "void *"
        if fr.is_ptr_to_ptr:
            # `T **field` — the field holds a vaddr to an array of vaddrs.
            # Read `obj.field` as a uint32_t (no cast to T**), then
            # convert to a host `uint32_t*` so subscripting reads PSX-
            # sized elements; convert each element separately.
            sub_kids = list(sub.get_children())
            idx = sub_kids[1] if len(sub_kids) > 1 else None
            if idx is not None:
                idx_text = _render_with_redirects(
                    idx, main_resolved, redirects, rewrites)
            else:
                idx_text = ""
            field_text = _bytes_slice(
                main_resolved,
                member.extent.start.offset, member.extent.end.offset)
            replacement = (
                f"(({cast})openpete_psx_to_host_ptr("
                f"((uint32_t *)openpete_psx_to_host_ptr({field_text}))"
                f"[{idx_text}]))")
        else:
            # Use the AST-aware renderer so array-redirect bases are
            # substituted at subscript level (`((T*)psx_to_host(__psx_arr[i]))`)
            # rather than the scalar-pointer form which is wrong for arrays.
            sub_text = _render_with_redirects(sub, main_resolved, redirects,
                                              rewrites)
            replacement = (f"(({cast})openpete_psx_to_host_ptr({sub_text}))")
        array_subscript_edits[s] = Edit(
            start=s, end=e, replacement=replacement,
            file=main_resolved,
            note=f"read[] {fr.record_name}.{fr.field_name}",
        )
        # Suppress the inner member-ref rewrite that would otherwise
        # collide.
        skip_member_ref_offsets.add(member.extent.start.offset)
        # Also suppress for synthetic-extent (macro-arg) member refs
        # whose computed start would land at the same token.
        try:
            mtoks = list(member.translation_unit.get_tokens(extent=member.extent))
        except Exception:
            mtoks = []
        if mtoks:
            skip_member_ref_offsets.add(mtoks[0].location.offset)
    for ed in array_subscript_edits.values():
        result.access_edits.append(ed)

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

    # Pre-collect write extents so nested member-refs inside them are
    # suppressed (the write's RHS rendering folds them inline).
    write_ranges: List[Tuple[int, int]] = []
    for off, w in writes.items():
        binop = w[0]
        write_ranges.append((binop.extent.start.offset, binop.extent.end.offset))
    write_ranges.sort()

    def _inside_write(s: int, e: int) -> Optional[Tuple[int, int]]:
        for ws, we in write_ranges:
            if s >= ws and e <= we and not (s == ws and e == we):
                return (ws, we)
            if ws > s:
                break
        return None

    # Also: when two relaid member-refs are nested (e.g. obj.outer[i].inner
    # where both `outer` and `inner` are diverging fields), the outer
    # access edit covers both. Pre-walk to find the outermost extent at
    # each location and suppress inner emits.
    outer_ranges: List[Tuple[int, int]] = []
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.MEMBER_REF_EXPR:
            continue
        if c.location.file is None:
            continue
        if str(Path(c.location.file.name).resolve()) != main_resolved:
            continue
        rd = c.referenced
        if rd is None or rd.kind != ci.CursorKind.FIELD_DECL:
            continue
        parent = rd.semantic_parent
        if parent is None:
            continue
        if rewrites.get(FieldKey(parent.get_usr(), rd.spelling)) is None:
            continue
        s = c.extent.start.offset
        e = c.extent.end.offset
        if e == s:
            try:
                toks = list(c.translation_unit.get_tokens(extent=c.extent))
            except Exception:
                toks = []
            if not toks:
                continue
            s = toks[0].location.offset
            e = toks[-1].location.offset + len(toks[-1].spelling)
            if e <= s:
                continue
        outer_ranges.append((s, e))
    outer_ranges.sort()
    # Filter to outermost only — drop any range fully inside another.
    pruned_outer: List[Tuple[int, int]] = []
    for s, e in sorted(outer_ranges, key=lambda r: (r[0], -r[1])):
        if pruned_outer and s >= pruned_outer[-1][0] and e <= pruned_outer[-1][1]:
            continue
        pruned_outer.append((s, e))

    def _strictly_inside_outer(s: int, e: int) -> bool:
        for os_, oe in pruned_outer:
            if os_ < s and e <= oe:
                return True
            if os_ > s:
                break
            if os_ <= s and e < oe:
                return True
        return False

    seen_member_offsets: Set[Tuple[int, int]] = set()
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
        if fr is None or fr.is_function_ptr or fr.is_opaque_pad:
            continue
        # Macro-arg expansion: a MEMBER_REF_EXPR appearing inside a macro
        # arg used N times in the body shows up as N AST nodes, all with
        # zero-length source extents. Synthesize an extent from the
        # cursor's tokens so the rewrite still lands at the textual arg
        # location. Dedupe by synthetic extent so the N copies become one.
        ext_start = c.extent.start.offset
        ext_end = c.extent.end.offset
        if ext_end == ext_start:
            try:
                toks = list(c.translation_unit.get_tokens(extent=c.extent))
            except Exception:
                toks = []
            if not toks:
                continue
            ext_start = toks[0].location.offset
            ext_end = toks[-1].location.offset + len(toks[-1].spelling)
            if ext_end <= ext_start:
                continue
        off = ext_start
        if off in addr_of_offsets:
            continue
        if off in skip_member_ref_offsets:
            # Array-of-pointer field handled at subscript level above.
            continue
        if (ext_start, ext_end) in seen_member_offsets:
            continue
        seen_member_offsets.add((ext_start, ext_end))
        # writes is keyed on lhs.extent.start; only the actual outermost
        # LHS qualifies — nested member-refs sharing the start are reads.
        is_write_lhs = False
        if off in writes:
            _binop, _lhs, _fr, _op = writes[off]
            if _lhs.extent.end.offset == ext_end:
                is_write_lhs = True
        # If this ref sits inside a Pass 1 write extent (and isn't the
        # write's own LHS), suppress — the write rendering folds it.
        if not is_write_lhs and _inside_write(ext_start, ext_end):
            continue
        # If this ref is strictly nested inside another relaid member-ref,
        # the outer's render folds it inline.
        if not is_write_lhs and _strictly_inside_outer(ext_start, ext_end):
            continue
        # If this ref sits inside a Pass 2 write extent, Pass 2 will fold
        # it into its rhs rendering. Suppress here to avoid overlap.
        if not is_write_lhs and _inside_pass2_write(ext_start, ext_end):
            continue

        if is_write_lhs:
            binop, lhs, _, op_str = writes[off]
            rhs_children = list(binop.get_children())
            rhs = rhs_children[1]
            # `lhs_text` is the storage-write target. Render its nested
            # refs (e.g. `obj.m_RelaidPtr[i].m_RelaidLong = ...` needs
            # the inner m_RelaidPtr substituted with its host-ptr read
            # form before subscripting). The outer ref (the field we're
            # writing to) stays bare via include_self=False.
            lhs_text = _render_with_redirects(lhs, main_resolved,
                                              redirects, rewrites)
            rhs_text = _render_with_redirects(rhs, main_resolved,
                                              redirects, rewrites)
            if op_str != "=":
                # Compound assign: `obj.field <op>= rhs` rewrites as
                # `obj.field = (cast)((lhs_read_form) <op> (rhs))`. The
                # binop here is the C-level operator with the trailing
                # `=` stripped. Long-family fields stay in uint32 space
                # (per plan §1.6.5 — field-to-field arithmetic on relaid
                # storage). Pointer compound-assigns (`+=`, `-=` for
                # pointer arithmetic) hard-error today; deferred until
                # we hit a real one in the wild.
                if fr.is_pointer:
                    raise RuntimeError(
                        f"compound-assign on relaid pointer field "
                        f"{fr.record_name}.{fr.field_name} at "
                        f"{lhs.location.file}:{lhs.location.line} — "
                        f"pointer arithmetic compound-assign not yet "
                        f"supported (Pass 1)")
                bare_op = op_str[:-1]  # "+=" -> "+", "<<=" -> "<<"
                cast = "int32_t" if fr.is_long_signed else "uint32_t"
                # Build the lhs read form: first render with nested
                # refs substituted (this is `lhs_text` already), then
                # wrap that in the relaid-field cast for the read.
                lhs_read = f"(({cast})({lhs_text}))"
                replacement = (f"{lhs_text} = ({cast})("
                               f"{lhs_read} {bare_op} ({rhs_text}))")
            elif fr.is_pointer:
                # If the RHS is a function reference with a known PSX
                # vaddr, substitute the literal — the host fn-ptr lives
                # outside g_ram, so host_to_psx() would yield 0 and
                # corrupt the field. Plan §1.6.3 / §2.3 Pass 1.
                fn_addr = (_resolve_function_psx_vaddr(rhs, symbols)
                           if symbols else None)
                if fn_addr is not None:
                    replacement = f"{lhs_text} = 0x{fn_addr:08X}u"
                elif _is_int_to_ptr_cast(rhs):
                    # `field = (PtrT)(int_expr)` — the source intends a
                    # literal PSX vaddr (e.g. `0x80200000 - X`). Storing
                    # via host_to_psx would treat the integer as a host
                    # pointer and yield 0. Skip the round-trip; store the
                    # integer value directly into the uint32_t storage.
                    inner = list(rhs.get_children())[0] \
                        if list(rhs.get_children()) else rhs
                    inner_text = _render_with_redirects(
                        inner, main_resolved, redirects, rewrites)
                    replacement = f"{lhs_text} = (uint32_t)({inner_text})"
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
                note=f"write {fr.record_name}.{fr.field_name} ({op_str})",
            ))
        else:
            if c.extent.end.offset == c.extent.start.offset:
                # Macro-arg synthetic extent: read text directly from
                # the file at the synthesized range and apply redirect
                # substitutions textually (no AST inside macro args).
                access_text = _bytes_slice(main_resolved, ext_start, ext_end)
                if redirects:
                    for name, red in redirects.items():
                        if name in access_text:
                            if red.is_pointer:
                                sub = (f"(({red.pointer_orig_type})"
                                       f"openpete_psx_to_host_ptr"
                                       f"({red.psx_storage_name}))")
                            else:
                                sub = f"((int32_t){red.psx_storage_name})"
                            access_text = access_text.replace(name, sub)
            else:
                access_text = _render_with_redirects(c, main_resolved,
                                                     redirects, rewrites)
            if fr.is_pointer:
                replacement = (f"(({fr.pointer_orig_type})"
                               f"openpete_psx_to_host_ptr({access_text}))")
            else:
                cast = "int32_t" if fr.is_long_signed else "uint32_t"
                replacement = f"(({cast})({access_text}))"
            result.access_edits.append(Edit(
                start=ext_start,
                end=ext_end,
                replacement=replacement,
                file=main_resolved,
                note=f"read {fr.record_name}.{fr.field_name}",
            ))


# ---------------------------------------------------------------------------
# Public entry
# ---------------------------------------------------------------------------

def run(tu_cursor: ci.Cursor, main_file: str,
        symbols: Optional[Dict[str, int]] = None,
        redirects=None,
        pass2_write_ranges: Optional[List[Tuple[int, int]]] = None
        ) -> StructRelayoutResult:
    result = StructRelayoutResult()
    rewrites = collect_field_rewrites(tu_cursor)
    result.rewrites = rewrites
    emit_header_edits(rewrites, result)
    emit_access_edits(tu_cursor, rewrites, main_file, result, symbols,
                      redirects=redirects,
                      pass2_write_ranges=pass2_write_ranges)
    return result
