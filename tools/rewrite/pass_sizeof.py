"""Pass 3 - sizeof verification.

After Pass 1's struct relayout, every game-defined struct's host size should
equal its PSX size - pointer / long fields are now ``uint32_t`` slots. This
pass confirms it: walk every ``sizeof(T)`` in the TU body, look up T's PSX
layout, and emit a ``_Static_assert(sizeof(T) == N, ...)`` for each unique
type into the rewritten-source prologue.

That's both a no-op verifier today (Pass 1 already does the work) and a
durable compile-time guard: any future struct change that breaks the PSX
layout invariant fails the host build immediately, instead of silently
mis-aliasing into ``g_ram``.

Scope:
  - Only ``sizeof`` expressions in the TU's main file (not transitively
    included headers).
  - Only types that are or contain a relaid game-defined struct. Primitive
    sizeof (``sizeof(int)``) is uninteresting.
  - Arrays unwrap to their element type.
"""

from __future__ import annotations

from dataclasses import dataclass, field as dc_field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

import clang.cindex as ci

from .types import StructLayout, layout_struct, type_diverges


@dataclass
class SizeofCheck:
    type_name: str       # spelling used in the static_assert
    psx_size: int        # expected sizeof under PSX layout
    location: str        # "<file>:<line>" - diagnostic only


@dataclass
class SizeofResult:
    checks: List[SizeofCheck] = dc_field(default_factory=list)
    skipped: List[Tuple[str, str]] = dc_field(default_factory=list)  # (type, reason)


def _is_game_header(path: str) -> bool:
    return "/spyro-1/include/" in str(path)


def _record_for_type(t: ci.Type) -> Optional[ci.Cursor]:
    """Drill through array / typedef wrappers and return the underlying
    RecordDecl cursor, or None if the type isn't a struct."""
    canon = t.get_canonical()
    while canon.kind in (ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY,
                         ci.TypeKind.VARIABLEARRAY):
        canon = canon.element_type.get_canonical()
    if canon.kind != ci.TypeKind.RECORD:
        return None
    decl = canon.get_declaration()
    if decl is None or not decl.is_definition():
        return None
    return decl


def _format_location(c: ci.Cursor) -> str:
    if c.location.file is None:
        return "<unknown>"
    return f"{Path(c.location.file.name).name}:{c.location.line}"


def run(tu_cursor: ci.Cursor, main_path: str) -> SizeofResult:
    """Walk the TU, collect static_assert candidates for every sizeof()
    of a game-defined relaid struct (or array thereof)."""
    result = SizeofResult()
    seen_types: Set[str] = set()
    seen_skipped: Set[str] = set()

    for c in tu_cursor.walk_preorder():
        # libclang surfaces sizeof / alignof / vec_step under
        # CXX_UNARY_EXPR. The argument is either a type (UNEXPOSED) or
        # an expression - both work via cursor.type once we restrict to
        # the operator's operand.
        if c.kind != ci.CursorKind.CXX_UNARY_EXPR:
            continue
        if c.location.file is None:
            continue
        if str(c.location.file.name) != main_path:
            continue
        # Reject non-sizeof unary exprs (alignof / __alignof). The token
        # stream is the cleanest discriminator the C bindings expose.
        toks = list(c.get_tokens())
        if not toks or toks[0].spelling not in ("sizeof",):
            continue

        # The operand's clang type is the result of `c.type`'s child.
        # For `sizeof(T)` clang reports the size in c.type (always size_t),
        # so we have to introspect children to recover T.
        operand_type: Optional[ci.Type] = None
        for kid in c.get_children():
            operand_type = kid.type
            break
        if operand_type is None:
            # `sizeof(T)` form leaves no child cursor in the C bindings;
            # parse the spelling out of the token stream as a fallback.
            # Tokens look like:  ['sizeof', '(', 'CdState', ')']
            if len(toks) >= 4 and toks[1].spelling == "(":
                # Reconstruct everything between the parens; could be
                # `struct Foo`, `Foo[3]`, etc.
                spelling = " ".join(t.spelling for t in toks[2:-1])
                seen_skipped.add(spelling)
                result.skipped.append((spelling, "sizeof(type) - type not "
                                                  "exposed via libclang AST"))
            continue

        decl = _record_for_type(operand_type)
        if decl is None:
            continue
        if decl.location.file is None:
            continue
        if not _is_game_header(decl.location.file.name):
            continue

        try:
            layout = layout_struct(decl)
        except ValueError:
            continue
        # Only emit guards for types that Pass 1 actually relaid - skipping
        # PSX-identical structs keeps the static_assert block tight.
        if not layout.diverges:
            continue

        # Use the typedef spelling if there is one; fall back to `struct Tag`.
        canon = operand_type.get_canonical()
        # strip arrays for the type name
        t_for_name = operand_type
        while t_for_name.get_canonical().kind in (
                ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY):
            t_for_name = t_for_name.get_canonical().element_type
        name = t_for_name.spelling.strip()
        if not name:
            continue
        if name in seen_types:
            continue
        seen_types.add(name)
        result.checks.append(SizeofCheck(
            type_name=name,
            psx_size=layout.psx_size,
            location=_format_location(c),
        ))

    return result


def render_static_asserts(result: SizeofResult) -> str:
    """Emit a C block of ``_Static_assert``s; suitable for prepending to the
    rewritten TU body. Empty string when nothing to check."""
    if not result.checks:
        return ""
    lines = [
        "/* Pass 3 (sizeof verification): every relaid struct that this TU",
        " * sizeof()s must match its PSX size on the host build. Pass 1's",
        " * relayout makes this true; the asserts catch regressions. */",
    ]
    for ch in sorted(result.checks, key=lambda c: c.type_name):
        # Use the type spelling as-is. C11 _Static_assert is available
        # under -std=gnu11 (the spyro native flags).
        lines.append(
            f'_Static_assert(sizeof({ch.type_name}) == {ch.psx_size}, '
            f'"Pass 3: sizeof({ch.type_name}) != {ch.psx_size} (PSX size); '
            f'Pass 1 relayout drift?");'
        )
    return "\n".join(lines) + "\n"
