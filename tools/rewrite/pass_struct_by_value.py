"""Pass 5 — struct-by-value boundary guard.

A native function calling a recomp/PSYQ thunk that takes (or returns) a
struct *by value* runs into an ABI shear: the host LP64/LLP64 calling
convention packs struct args / returns differently from PSYQ MIPS, and
post-Pass-1 the host-laid struct is PSX-laid in memory but the call
site still has to marshal those bytes through a0..a3 / stack slots.

The plan's full rewrite for this case is to allocate a g_ram scratch
slot, memcpy the host struct into it, and pass the PSX vaddr in
``cpu->aN`` instead — symmetric for return values. That's a real chunk
of work and only worth landing once a real call site needs it.

A scan across every TU in ``spyro-1/src/`` reports **zero** struct-by-value
arg sites and zero struct-by-value return sites today — Spyro's
matching-decomp consistently passes structs by pointer. Pass 5 is the
guard: hard-error the first time a future migration surfaces either
case, so the wrong-marshalling failure can't go silent.

If/when the guard trips, the next step is to lift the offender into
``[[psyq_boundary_copy]]`` (or its recomp equivalent) and have a
companion pass materialize the per-call scratch-buffer wrapper.
"""

from __future__ import annotations

from dataclasses import dataclass, field as dc_field
from typing import List

import clang.cindex as ci


@dataclass
class StructByValueViolation:
    callee: str
    kind: str          # "arg" | "return"
    arg_index: int     # only meaningful when kind == "arg"
    type_spelling: str
    location: str


@dataclass
class StructByValueResult:
    violations: List[StructByValueViolation] = dc_field(default_factory=list)


def _is_record_value(ty: ci.Type) -> bool:
    """True if the type is a struct/union passed *by value* — not a
    pointer to one, and not an array."""
    canon = ty.get_canonical()
    return canon.kind == ci.TypeKind.RECORD


def _format_location(c: ci.Cursor) -> str:
    if c.location.file is None:
        return "<unknown>"
    return f"{c.location.file.name}:{c.location.line}:{c.location.column}"


def run(tu_cursor: ci.Cursor, main_path: str) -> StructByValueResult:
    """Walk every CallExpr in the TU's main file. Hard-flag any site
    where the callee takes (or returns) a struct by value."""
    result = StructByValueResult()

    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.CALL_EXPR:
            continue
        if c.location.file is None:
            continue
        if str(c.location.file.name) != main_path:
            continue

        callee = c.referenced
        if callee is None or callee.kind != ci.CursorKind.FUNCTION_DECL:
            continue
        name = callee.spelling
        if not name:
            continue

        # Argument-by-value.
        for idx, param in enumerate(callee.get_arguments()):
            if _is_record_value(param.type):
                result.violations.append(StructByValueViolation(
                    callee=name,
                    kind="arg",
                    arg_index=idx,
                    type_spelling=param.type.spelling,
                    location=_format_location(c),
                ))
                break  # one violation per call site

        # Return-by-value.
        rt = callee.result_type
        if _is_record_value(rt):
            result.violations.append(StructByValueViolation(
                callee=name,
                kind="return",
                arg_index=-1,
                type_spelling=rt.spelling,
                location=_format_location(c),
            ))

    return result


def format_violations(result: StructByValueResult) -> str:
    if not result.violations:
        return ""
    lines = ["Pass 5: struct-by-value boundary guard tripped:"]
    for v in result.violations:
        if v.kind == "arg":
            lines.append(f"  {v.location}: call to {v.callee}() — "
                         f"arg {v.arg_index} passed by value "
                         f"({v.type_spelling})")
        else:
            lines.append(f"  {v.location}: call to {v.callee}() — "
                         f"returns by value ({v.type_spelling})")
        lines.append("    needs a g_ram scratch copy-in/out shim")
    return "\n".join(lines)
