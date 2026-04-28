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
from typing import Dict, Set

import clang.cindex as ci

from .pass_bare_globals import GlobalRedirect


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
    # Diagnostic — `(<target>)<global>` sites we redirected.
    redirected: list = dc_field(default_factory=list)  # (target_type, name, location)


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


def run(tu_cursor: ci.Cursor,
        main_path: str,
        redirects: Dict[str, GlobalRedirect]) -> CastResult:
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
