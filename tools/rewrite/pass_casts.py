"""Pass 4 — pointer-to-integer cast substitution.

Pass 2 redirects bare-pointer globals through ``__psx_<name>`` storage and
rewrites every read as ``((T*)openpete_psx_to_host_ptr(__psx_name))`` so the
source-level ``T*`` typing survives. That round-trip is wrong when the
source is taking the *PSX vaddr* of the global, not the host pointer:

    int x = (int)g_OverlaySpacePointer;
    // Pass 2 alone:  (int)((void*)openpete_psx_to_host_ptr(__psx_g_…))
    //                ^ host vaddr, narrowed; loses the PSX-vaddr meaning
    // What we want: (int)__psx_g_OverlaySpacePointer   (the PSX vaddr itself)

Pass 4 detects integer-targeted ``CSTYLE_CAST_EXPR`` whose operand is a
DeclRefExpr to a redirected-pointer global and tells Pass 2 to substitute
the inner read with the bare ``__psx_<name>`` storage form. The outer cast
expresses the desired narrowing; ``__psx_<name>`` is already ``uint32_t``
so the math is identical to what the PSX would compute.

This is a no-op on the current native TU set (cd.c / main.c don't take a
pointer-to-int cast on any redirected global) but unblocks the integer-
cast-pointer-arithmetic patterns that gate ``initialization.c`` re-enable
(see ``manifest/native.toml``'s init.c gating comment).

Pass 4 is consumed by ``rewrite_bare_globals`` via a ``vaddr_form_offsets``
parameter — it doesn't emit edits itself, only redirects which form Pass 2
chooses for affected read sites.
"""

from __future__ import annotations

from dataclasses import dataclass, field as dc_field
from typing import Dict, List, Set, Tuple

import clang.cindex as ci

from .pass_bare_globals import GlobalRedirect
from .rewriter import Edit
from .types import _is_pointer_kind


# Type kinds whose host width is the PSX-vaddr's natural representation.
# `int`, `unsigned int`, `long`, `unsigned long`, plus the stdint variants
# that decay to those in the parser's mipsel target view.
_INTEGER_CAST_KINDS = frozenset({
    ci.TypeKind.INT, ci.TypeKind.UINT,
    ci.TypeKind.LONG, ci.TypeKind.ULONG,
    ci.TypeKind.LONGLONG, ci.TypeKind.ULONGLONG,
    ci.TypeKind.SHORT, ci.TypeKind.USHORT,
    ci.TypeKind.SCHAR, ci.TypeKind.UCHAR,
    ci.TypeKind.CHAR_S, ci.TypeKind.CHAR_U,
})


@dataclass
class CastResult:
    # DeclRefExpr extent.start.offset for every read site whose Pass 2 form
    # should be `__psx_<name>` (bare storage) instead of the host-pointer
    # round-trip.
    vaddr_form_offsets: Set[int] = dc_field(default_factory=set)
    # ARRAY_SUBSCRIPT_EXPR extent (start, end) for each subscript site
    # whose Pass 2 array-redirect read should emit the bare `__psx_arr[i]`
    # form instead of the host-pointer round-trip — operand of a (IntT)
    # cast that intends the PSX vaddr.
    vaddr_form_array_sub_extents: Set[Tuple[int, int]] = dc_field(default_factory=set)
    # Diagnostic — `(<target>)<global>` sites we redirected.
    redirected: list = dc_field(default_factory=list)  # (target_type, name, location)
    # Pass 4b — int-to-ptr cast wrap edits (psx_to_host_ptr around literal-vaddr
    # operands so the surrounding code receives a host pointer).
    edits: List[Edit] = dc_field(default_factory=list)


# Wrappers Pass 4 walks through to find the cast's bare-identifier operand.
# Anything substructural (ARRAY_SUBSCRIPT_EXPR, MEMBER_REF_EXPR, BINARY_OPERATOR,
# etc.) means the cast targets a *result* of operating on the global, not
# the global itself — Pass 4 must NOT mark the inner DeclRefExpr as
# vaddr-form, otherwise Pass 2 emits the bare uint32_t storage where a
# typed pointer is needed (e.g. `(u_char)g_LevelMobys[i].m_State` →
# `(u_char)__psx_g_LevelMobys[i].m_State` — subscript on a uint32_t).
_WALKABLE_WRAPPERS = frozenset({
    ci.CursorKind.PAREN_EXPR,
    ci.CursorKind.UNEXPOSED_EXPR,
})


def _unwrap_to_decl_ref(c: ci.Cursor) -> ci.Cursor | None:
    """Walk through PAREN_EXPR / UNEXPOSED_EXPR wrappers and return the
    inner DeclRefExpr, or None if the operand isn't a bare identifier
    reference."""
    cur = c
    for _ in range(6):
        if cur.kind == ci.CursorKind.DECL_REF_EXPR:
            return cur
        if cur.kind not in _WALKABLE_WRAPPERS:
            return None
        kids = list(cur.get_children())
        if not kids:
            return None
        cur = kids[0]
    return None


def _is_host_to_psx_call(c: ci.Cursor) -> bool:
    """True if `c` is a CallExpr whose callee resolves to
    `openpete_host_to_psx`. Such a call already converts a host pointer
    to a PSX vaddr; its result is integer-arithmetic-safe."""
    if c.kind != ci.CursorKind.CALL_EXPR:
        return False
    ref = c.referenced
    if ref is not None and ref.spelling == "openpete_host_to_psx":
        return True
    # Some libclang versions report the spelling on the cursor itself.
    if c.spelling == "openpete_host_to_psx":
        return True
    return False


def _operand_has_pointer_term(operand: ci.Cursor,
                              redirects: Dict[str, GlobalRedirect]) -> bool:
    """True if `operand`'s subtree contains any pointer-typed DeclRefExpr,
    MemberRefExpr, or pointer-to-int CSTYLE_CAST_EXPR — signs that the
    integer value is host-pointer-derived (host arithmetic), not a literal
    PSX vaddr expression. Pruned at `openpete_host_to_psx(...)` boundaries:
    the call has already converted a host pointer to a PSX vaddr, so its
    result counts as integer-safe regardless of what's inside."""

    def _walk(node: ci.Cursor) -> bool:
        if _is_host_to_psx_call(node):
            return False  # subtree pruned
        if node.kind == ci.CursorKind.DECL_REF_EXPR:
            try:
                kt = node.type.get_canonical()
            except Exception:
                kt = None
            if kt is not None and _is_pointer_kind(kt):
                return True
            ref = node.referenced
            if ref is not None and ref.spelling in redirects:
                if redirects[ref.spelling].is_pointer:
                    return True
        elif node.kind == ci.CursorKind.MEMBER_REF_EXPR:
            try:
                kt = node.type.get_canonical()
            except Exception:
                kt = None
            if kt is not None and _is_pointer_kind(kt):
                return True
        elif node.kind == ci.CursorKind.CSTYLE_CAST_EXPR:
            kids = list(node.get_children())
            inner = next((kk for kk in kids
                          if kk.kind != ci.CursorKind.TYPE_REF), None)
            if inner is not None:
                try:
                    it = inner.type.get_canonical()
                except Exception:
                    it = None
                if it is not None and _is_pointer_kind(it):
                    tt = node.type.get_canonical()
                    if tt.kind in _INTEGER_CAST_KINDS:
                        # Pass 4c will wrap this `(IntT)host_ptr` site
                        # with `host_to_psx`, so the resulting expression
                        # is integer-arithmetic-safe. Prune the subtree.
                        return False
        for child in node.get_children():
            if _walk(child):
                return True
        return False

    return _walk(operand)


def _wrap_ptr_to_int_casts(tu_cursor: ci.Cursor,
                           main_path: str,
                           redirects: Dict[str, GlobalRedirect],
                           result: CastResult) -> None:
    """Walk every CSTYLE_CAST_EXPR `(IntT)pointer_expr`. The source intends
    the PSX vaddr (a 32-bit value), but the operand is now a host pointer
    (often 64-bit). Cast `(int)host_ptr` truncates and breaks subsequent
    arithmetic. Wrap the operand with `host_to_psx` so the (IntT) cast
    receives a uint32_t PSX vaddr.

    Skip:
    - Redirected-pointer global DeclRefExpr — Pass 4 marks those for the
      bare `__psx_<name>` storage form (no round-trip needed).
    - Operands whose canonical type is already integer (paranoia — the
      cast is a no-op or pointer-to-int between numeric types).
    """
    seen_ranges: Set[Tuple[int, int]] = set()
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.CSTYLE_CAST_EXPR:
            continue
        if c.location.file is None or str(c.location.file.name) != main_path:
            continue
        target_canon = c.type.get_canonical()
        if target_canon.kind not in _INTEGER_CAST_KINDS:
            continue
        kids = list(c.get_children())
        operand = next((k for k in kids
                        if k.kind != ci.CursorKind.TYPE_REF), None)
        if operand is None:
            continue
        op_canon = operand.type.get_canonical()
        if not _is_pointer_kind(op_canon):
            continue
        # Dedupe — a cast site with multiple visits or repeated macro
        # expansions should only emit once.
        key = (operand.extent.start.offset, operand.extent.end.offset)
        if key in seen_ranges:
            continue
        seen_ranges.add(key)
        # Skip when the operand reduces to a redirected-pointer-global
        # DeclRefExpr — Pass 4 already routes that to the __psx_<name>
        # bare storage form, which is the PSX vaddr directly.
        ref = _unwrap_to_decl_ref(operand)
        if ref is not None:
            ref_decl = ref.referenced
            if ref_decl is not None and ref_decl.spelling in redirects:
                if redirects[ref_decl.spelling].is_pointer:
                    continue
        # Skip macro-expanded casts where libclang reports the cast and
        # its operand with identical extents (both pointing at the macro
        # invocation site). Those are phantom expansions: PATCH_POINTER's
        # `(u_int)(dest)` shows up three times per call, all with the
        # same outer extent — emitting three edits over the same range
        # collides on commit. Pass 1's write rewrite at the macro's
        # outermost binop already takes the whole site.
        if operand.extent.start.offset == c.extent.start.offset and \
                operand.extent.end.offset == c.extent.end.offset:
            continue
        # Read operand text and replace just the operand range with
        # the host_to_psx-wrapped form. Keep the outer (IntT) cast
        # unchanged.
        ext = operand.extent
        if ext.start.file is None:
            continue
        with open(str(ext.start.file.name), "rb") as fh:
            fh.seek(ext.start.offset)
            operand_text = fh.read(
                ext.end.offset - ext.start.offset).decode("utf-8", "replace")
        result.edits.append(Edit(
            start=ext.start.offset,
            end=ext.end.offset,
            replacement=f"openpete_host_to_psx({operand_text})",
            file=str(c.location.file.name),
            note=f"ptr->int cast wrap",
        ))


def _render_with_inner_ptr_to_int_wraps(operand: ci.Cursor,
                                        file_path: str,
                                        redirects: Dict[str, GlobalRedirect],
                                        pass1_rewrites=None) -> str:
    """Read the operand source text and substitute:
      - `(IntT)pointer_expr` sub-casts → `(IntT)openpete_host_to_psx(<inner>)`
        (Pass 4c's effect, folded into Pass 4b's wrap text).
      - Pass 1's relaid struct-field reads inline (when `pass1_rewrites` is
        provided). Without this, Pass 1's own read edit lands inside Pass
        4b's wrap range and the splicer flags an overlap."""
    start = operand.extent.start.offset
    end = operand.extent.end.offset
    with open(file_path, "rb") as fh:
        fh.seek(start)
        text = fh.read(end - start).decode("utf-8", "replace")
    if end <= start:
        return text
    subs: List[Tuple[int, int, str]] = []
    for k in operand.walk_preorder():
        if k.kind != ci.CursorKind.CSTYLE_CAST_EXPR:
            continue
        if k.location.file is None or str(k.location.file.name) != file_path:
            continue
        tt = k.type.get_canonical()
        if tt.kind not in _INTEGER_CAST_KINDS:
            continue
        kids = list(k.get_children())
        inner = next((kk for kk in kids
                      if kk.kind != ci.CursorKind.TYPE_REF), None)
        if inner is None:
            continue
        it = inner.type.get_canonical()
        if not _is_pointer_kind(it):
            continue
        # Skip when the inner pointer expression is a redirected-pointer
        # global (Pass 4 marks those for the bare __psx_<name> form).
        ref = _unwrap_to_decl_ref(inner)
        if ref is not None:
            ref_decl = ref.referenced
            if ref_decl is not None and ref_decl.spelling in redirects:
                if redirects[ref_decl.spelling].is_pointer:
                    continue
        ks = inner.extent.start.offset
        ke = inner.extent.end.offset
        if ke <= ks or ks < start or ke > end:
            continue
        # Inner text — render with Pass 1+2 substitutions so any relaid
        # field-reads or redirected-global reads get folded inline.
        if pass1_rewrites is not None:
            from .pass_struct_relayout import _render_with_redirects as _r1
            inner_text = _r1(inner, file_path, redirects, pass1_rewrites)
        else:
            with open(file_path, "rb") as fh:
                fh.seek(ks)
                inner_text = fh.read(ke - ks).decode("utf-8", "replace")
        subs.append((ks - start, ke - start,
                     f"openpete_host_to_psx({inner_text})"))
    if not subs:
        return text
    subs.sort()
    out = text
    for rs, re_, repl in reversed(subs):
        out = out[:rs] + repl + out[re_:]
    return out


def _wrap_int_to_ptr_casts(tu_cursor: ci.Cursor,
                           main_path: str,
                           redirects: Dict[str, GlobalRedirect],
                           result: CastResult,
                           pass1_rewrites=None) -> None:
    """Walk every CSTYLE_CAST_EXPR `(PtrT)(int_expr)` whose int_expr is a
    pure-integer expression (no host-pointer-derived term). The integer
    value is intended as a literal PSX vaddr — wrap with psx_to_host_ptr
    so the surrounding code (local var init, function arg, etc.) receives
    a usable host pointer.

    Pass 1's pointer-field write rendering already handles the case where
    the result is stored into a relaid pointer field directly. This pass
    catches every other consumer (locals, arguments, return values).
    """
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.CSTYLE_CAST_EXPR:
            continue
        if c.location.file is None or str(c.location.file.name) != main_path:
            continue
        target_canon = c.type.get_canonical()
        if not _is_pointer_kind(target_canon):
            continue
        kids = list(c.get_children())
        operand = next((k for k in kids
                        if k.kind != ci.CursorKind.TYPE_REF), None)
        if operand is None:
            continue
        op_canon = operand.type.get_canonical()
        if op_canon.kind not in _INTEGER_CAST_KINDS:
            continue
        cast_start = c.extent.start.offset
        cast_end = c.extent.end.offset
        # libclang sometimes emits a phantom CSTYLE_CAST_EXPR whose
        # extent equals its operand's extent (no `(Type)` prefix in
        # the source range) and whose location is wholly unrelated.
        # Dedupe by skipping when extent == operand extent.
        if (cast_start == operand.extent.start.offset
                and cast_end == operand.extent.end.offset):
            continue
        if _operand_has_pointer_term(operand, redirects):
            continue
        if c.extent.start.file is None:
            continue
        # Read the operand source text and inline any Pass-4c-style
        # `(IntT)pointer_expr` substitutions so they don't emit their own
        # overlapping edits.
        operand_text = _render_with_inner_ptr_to_int_wraps(
            operand, str(c.location.file.name), redirects, pass1_rewrites)
        # Cast target spelling — keep the source's pointer type for the
        # outer cast (so the host pointer flows with the right type).
        target_spelling = c.type.spelling
        replacement = (f"(({target_spelling})openpete_psx_to_host_ptr("
                       f"{operand_text}))")
        result.edits.append(Edit(
            start=cast_start,
            end=cast_end,
            replacement=replacement,
            file=str(c.location.file.name),
            note=f"int->ptr cast wrap",
        ))


def run(tu_cursor: ci.Cursor,
        main_path: str,
        redirects: Dict[str, GlobalRedirect],
        pass1_rewrites=None) -> CastResult:
    """Walk the TU. For every ``(IntT)redirected_pointer_global`` cast in the
    main file, mark the inner DeclRefExpr's offset so Pass 2 emits the
    ``__psx_<name>`` form."""
    result = CastResult()
    if not redirects:
        return result

    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.CSTYLE_CAST_EXPR:
            continue
        if c.location.file is None or str(c.location.file.name) != main_path:
            continue
        target_canon = c.type.get_canonical()
        if target_canon.kind not in _INTEGER_CAST_KINDS:
            continue

        # The cast expression has one child — the operand.
        kids = list(c.get_children())
        if not kids:
            continue
        # Cast cursors sometimes expose the type-ref as a leading child;
        # try each child until we find a non-type cursor.
        operand = None
        for k in kids:
            if k.kind == ci.CursorKind.TYPE_REF:
                continue
            operand = k
            break
        if operand is None:
            continue

        # Two operand shapes get the bare-vaddr form treatment:
        # (1) bare DeclRef to a redirected scalar pointer global, and
        # (2) ARRAY_SUBSCRIPT_EXPR `arr[i]` whose base is a redirected
        #     array-of-pointer global. Both flatten to a `uint32_t` PSX
        #     vaddr that the outer (IntT) cast can narrow directly.
        op = operand
        while op.kind in _WALKABLE_WRAPPERS:
            ks = list(op.get_children())
            if not ks:
                break
            op = ks[0]
        if op.kind == ci.CursorKind.ARRAY_SUBSCRIPT_EXPR:
            ks = list(op.get_children())
            if ks:
                base = ks[0]
                while base.kind == ci.CursorKind.UNEXPOSED_EXPR:
                    bks = list(base.get_children())
                    if not bks:
                        break
                    base = bks[0]
                if base.kind == ci.CursorKind.DECL_REF_EXPR:
                    bd = base.referenced
                    if bd is not None:
                        bred = redirects.get(bd.spelling)
                        if bred is not None and bred.is_array and bred.is_pointer:
                            result.vaddr_form_array_sub_extents.add(
                                (op.extent.start.offset, op.extent.end.offset))
                            result.redirected.append(
                                (target_canon.spelling, bred.name,
                                 f"{c.location.file.name}:{c.location.line}"))
                            continue

        ref = _unwrap_to_decl_ref(operand)
        if ref is None:
            continue
        ref_decl = ref.referenced
        if ref_decl is None:
            continue
        red = redirects.get(ref_decl.spelling)
        if red is None or not red.is_pointer:
            continue

        result.vaddr_form_offsets.add(ref.extent.start.offset)
        result.redirected.append(
            (target_canon.spelling, red.name,
             f"{c.location.file.name}:{c.location.line}")
        )

    return result


def apply_cast_wraps(tu_cursor: ci.Cursor,
                     main_path: str,
                     redirects: Dict[str, GlobalRedirect],
                     result: CastResult,
                     pass1_rewrites=None) -> None:
    """Run Pass 4b/4c cast wraps. Split out from `run` so the caller can
    invoke it after Pass 1 has produced its rewrites map — Pass 4b's
    `_render_with_inner_ptr_to_int_wraps` folds Pass 1's relaid-field
    reads inline."""
    _wrap_int_to_ptr_casts(tu_cursor, main_path, redirects, result, pass1_rewrites)
    _wrap_ptr_to_int_casts(tu_cursor, main_path, redirects, result)
