"""Pass 5e — host-pointer-to-thunk lint.

When a native function passes a host-allocated pointer (stack local,
host heap, or a host-defined global) through an outbound thunk, the
auto-generated thunk does ``cpu->aN = host_to_psx(p)``. ``host_to_psx``
returns 0 for any host pointer outside g_ram / g_scratch, and the
callee then dereferences NULL.

This pass walks every CallExpr whose callee is a thunked outbound and
classifies each pointer-typed argument. Clear-bad shapes warn; safe
shapes pass; ambiguous shapes (function-parameter pass-through, etc.)
are silent — better to miss than false-positive on every callee.

Mitigations:
  - Flavor A — PsyCross outbound where the host signature matches.
    Add to ``outbound_psyq_skip``; source link-resolves to host symbol.
  - Flavor B — recomp outbound where the callee runs MIPS gp+offset.
    Boundary-copy shim materializing a g_ram scratch slot per call.

Surfaced 2026-04-28 by the windows-mingw smoke gate hitting
``CdIntToPos(i, &local_pos)`` from music.c / spu.c — stack-local
``CdlLOC`` ⇒ host_to_psx ⇒ 0 ⇒ NULL deref.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Set

import clang.cindex as ci


# Function-name spellings that, when seen at the head of a callee chain,
# indicate the argument was already a PSX vaddr converted to a host
# pointer — host_to_psx round-trips it back, so it's safe.
_PSX_TO_HOST_FNS = frozenset({
    "openpete_psx_to_host_ptr", "psx_to_host_ptr", "PTR",
})


@dataclass
class OutboundLintWarning:
    file: str
    line: int
    callee: str
    arg_index: int
    arg_text: str
    classification: str       # "stack-local", "host-global", "host-heap"
    suggestion: str           # short mitigation hint


@dataclass
class OutboundLintResult:
    warnings: List[OutboundLintWarning] = field(default_factory=list)


def _strip_wrappers(c: ci.Cursor) -> ci.Cursor:
    """Walk through PAREN_EXPR / UNEXPOSED_EXPR / CSTYLE_CAST_EXPR /
    implicit casts to reach the meaningful child."""
    seen = 0
    while seen < 8:
        if c.kind in (ci.CursorKind.PAREN_EXPR,
                      ci.CursorKind.UNEXPOSED_EXPR,
                      ci.CursorKind.CSTYLE_CAST_EXPR):
            kids = list(c.get_children())
            if not kids:
                return c
            c = kids[-1]  # cast wraps its operand last
            seen += 1
            continue
        return c
    return c


def _is_unary_address_of(c: ci.Cursor) -> bool:
    if c.kind != ci.CursorKind.UNARY_OPERATOR:
        return False
    try:
        toks = list(c.translation_unit.get_tokens(extent=c.extent))
    except Exception:
        toks = []
    return bool(toks) and toks[0].spelling == "&"


def _root_decl_of_member_chain(c: ci.Cursor) -> Optional[ci.Cursor]:
    """Walk a MEMBER_REF/ARRAY_SUBSCRIPT chain down to its root
    DECL_REF_EXPR, returning the referenced decl. Returns None on any
    other shape."""
    seen = 0
    while seen < 16:
        c = _strip_wrappers(c)
        if c.kind == ci.CursorKind.DECL_REF_EXPR:
            return c.referenced
        if c.kind in (ci.CursorKind.MEMBER_REF_EXPR,
                      ci.CursorKind.ARRAY_SUBSCRIPT_EXPR):
            kids = list(c.get_children())
            if not kids:
                return None
            c = kids[0]
            seen += 1
            continue
        return None
    return None


def _is_call_to(call: ci.Cursor, names: frozenset) -> bool:
    if call.kind != ci.CursorKind.CALL_EXPR:
        return False
    ref = call.referenced
    if ref is not None and ref.spelling in names:
        return True
    # Macro-style PTR(...) might not bind a referenced FUNCTION_DECL.
    # Fall back to the first identifier token.
    try:
        toks = list(call.translation_unit.get_tokens(extent=call.extent))
    except Exception:
        return False
    if toks and toks[0].kind == ci.TokenKind.IDENTIFIER \
            and toks[0].spelling in names:
        return True
    return False


def _arg_text(c: ci.Cursor, main_resolved: str) -> str:
    s = c.extent.start.offset
    e = c.extent.end.offset
    if e <= s:
        return c.spelling or "<expr>"
    try:
        with open(main_resolved, "rb") as fh:
            fh.seek(s)
            return fh.read(e - s).decode("utf-8", "replace")
    except Exception:
        return c.spelling or "<expr>"


def _classify_pointer_arg(arg: ci.Cursor,
                          game_globals: Set[str],
                          ) -> Optional[tuple[str, str]]:
    """Returns (classification, suggestion) if the arg is a clearly-bad
    host-pointer shape; None otherwise (safe or ambiguous).

    `game_globals` is the set of names known to be --defsym-aliased into
    g_ram (e.g. from globals.toml). Address-of these is safe.
    """
    arg = _strip_wrappers(arg)

    # Address-of: &x or &x.field or &x[i] or &x->field.
    if _is_unary_address_of(arg):
        kids = list(arg.get_children())
        if not kids:
            return None
        operand = _strip_wrappers(kids[0])

        # Resolve the root decl of any chained access.
        root = None
        if operand.kind == ci.CursorKind.DECL_REF_EXPR:
            root = operand.referenced
        elif operand.kind in (ci.CursorKind.MEMBER_REF_EXPR,
                              ci.CursorKind.ARRAY_SUBSCRIPT_EXPR):
            root = _root_decl_of_member_chain(operand)

        if root is None or root.kind != ci.CursorKind.VAR_DECL:
            return None

        # Stack-local: parm or local var.
        sp = root.semantic_parent
        if sp is not None and sp.kind != ci.CursorKind.TRANSLATION_UNIT:
            return ("stack-local",
                    "use a g_ram scratch slot or a game-defined global")

        # File-scope global. Safe iff it's --defsym-aliased into g_ram.
        if root.spelling in game_globals:
            return None
        # Static linkage: never aliased, always host storage.
        if root.storage_class == ci.StorageClass.STATIC:
            return ("host-global (static)",
                    "use a game-defined global, or boundary-copy")
        return ("host-global",
                "if this is a host-only symbol, route through a "
                "boundary-copy shim or a g_ram scratch slot")

    # Call expressions: psx_to_host_ptr / PTR / openpete_psx_to_host_ptr
    # round-trip a PSX vaddr; safe.
    if _is_call_to(arg, _PSX_TO_HOST_FNS):
        return None

    # Identifier malloc/calloc/realloc/strdup/etc. → host heap. Flag.
    if arg.kind == ci.CursorKind.CALL_EXPR:
        ref = arg.referenced
        if ref is not None and ref.spelling in (
            "malloc", "calloc", "realloc", "strdup", "alloca",
        ):
            return ("host-heap",
                    "host-allocated buffer; use a g_ram scratch slot "
                    "or copy into a game-defined buffer")

    # Anything else (NULL literal, arithmetic on a vaddr, parameter
    # pass-through, returned host pointer from another thunk) — silent.
    return None


def _callee_pointer_param_indices(call: ci.Cursor) -> List[int]:
    """Return indices of the callee's parameters whose declared type is
    a pointer. Returns empty list if the callee has no FUNCTION_DECL
    (variadic / unresolved)."""
    ref = call.referenced
    if ref is None or ref.kind != ci.CursorKind.FUNCTION_DECL:
        return []
    out: List[int] = []
    for i, param in enumerate(ref.get_arguments()):
        try:
            t = param.type.get_canonical()
        except Exception:
            continue
        if t.kind in (ci.TypeKind.POINTER,
                      ci.TypeKind.INCOMPLETEARRAY,
                      ci.TypeKind.CONSTANTARRAY):
            out.append(i)
    return out


def run(tu_cursor: ci.Cursor,
        main_file: str,
        thunked_fns: Set[str],
        game_globals: Set[str]) -> OutboundLintResult:
    """Walk every CallExpr in the main TU; for each call to a function
    in `thunked_fns`, classify each pointer argument and flag bad
    shapes."""
    result = OutboundLintResult()
    if not thunked_fns:
        return result
    main_resolved = str(Path(main_file).resolve())

    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.CALL_EXPR:
            continue
        if c.location.file is None:
            continue
        if str(Path(c.location.file.name).resolve()) != main_resolved:
            continue
        ref = c.referenced
        if ref is None:
            continue
        callee = ref.spelling
        if callee not in thunked_fns:
            continue
        ptr_indices = set(_callee_pointer_param_indices(c))
        if not ptr_indices:
            continue
        # CallExpr children: [callee_ref, arg0, arg1, ...]. Some libclang
        # versions surface only the args; iterate and grab args by their
        # KIND being expression-like rather than the callee DECL_REF.
        kids = list(c.get_children())
        # libclang's CALL_EXPR children put the callee identifier first
        # (as UNEXPOSED_EXPR or DECL_REF_EXPR — depends on the version),
        # then the arguments in source order. Drop the leading callee
        # by matching its spelling against the resolved function name.
        if kids and kids[0].spelling == callee:
            kids = kids[1:]
        for idx, arg in enumerate(kids):
            if idx not in ptr_indices:
                continue
            verdict = _classify_pointer_arg(arg, game_globals)
            if verdict is None:
                continue
            cls, sugg = verdict
            result.warnings.append(OutboundLintWarning(
                file=str(c.location.file.name),
                line=c.location.line,
                callee=callee,
                arg_index=idx,
                arg_text=_arg_text(arg, main_resolved).strip(),
                classification=cls,
                suggestion=sugg,
            ))
    return result


def format_warnings(res: OutboundLintResult) -> str:
    """Format warnings for stderr — one per line, prefixed `WARN:`."""
    out: List[str] = []
    for w in res.warnings:
        out.append(
            f"WARN: outbound-thunk {w.callee}(arg{w.arg_index}={w.arg_text}) "
            f"at {w.file}:{w.line} — {w.classification}; {w.suggestion}")
    return "\n".join(out)
