"""Pass 5b - PsyCross ``u_long *`` boundary guard.

Per the audit in ``docs/src/developer/psycross-boundary.md``, PsyCross declares many
public APIs as taking ``u_long *``. ``u_long`` is a typedef for host
``unsigned long`` - 4 bytes on LLP64 (MinGW), **8 bytes on LP64 (Linux x86_64)**.
PSX-side callers pass buffers laid out at 4-byte stride. Routines that
internally byte-walk or cast to ``uint32_t *`` are safe; routines that stride
as host ``unsigned long`` would silently mis-step through PSX-laid memory.

The audit verified that Spyro's two C-side ``u_long *`` callers (``CdRead``,
``LoadImage``) byte-walk internally -> no shim needed today. The remaining
``u_long *`` PsyCross APIs (``OpenTIM``, ``ClearOTag``, ``DrawOTag*``, ...) are
either safe-but-unused, or strided in host layout (``OT_TAG`` family) and
require a per-call copy-shim before C-side migration.

This pass is the **guard**: walk every ``CallExpr``, and if any parameter type
is ``u_long *`` (or its canonical ``unsigned long *``), the callee must be in
the audited safe list. Otherwise hard-error so the next migrating TU surfaces
the boundary explicitly rather than silently corrupting memory.

Future work - when an unsafe API needs to migrate, lift it from
``BLOCKED_PSYQ_API`` into a ``[[psyq_boundary_copy]]`` shim entry and have
the rewriter materialize a per-call scratch-buffer wrapper. Not needed for
the current TU set (cd.c, main.c).
"""

from __future__ import annotations

from dataclasses import dataclass, field as dc_field
from typing import List

import clang.cindex as ci


# APIs whose internals byte-walk / cast to ``uint32_t *`` - safe to call
# directly from native code with a PSX-laid buffer.
SAFE_PSYQ_API = frozenset({
    "CdRead",
    "CdReadFile",
    "LoadImage",
    "LoadImage2",
    "StoreImage",
    "StoreImage2",
    "OpenTIM",
    "OpenTMD",
    "LoadClut",
    "LoadClut2",
    "LoadTPage",
})

# APIs that stride as host ``OT_TAG`` (12 bytes on x64 vs. 4 on PSX) - must
# not be called from native C with a g_ram-aliased buffer until a copy-shim
# pass lands. Diagnostic only; the guard treats *any* non-safe API the same
# way (hard error).
BLOCKED_PSYQ_API = frozenset({
    "ClearOTag",
    "ClearOTagR",
    "DrawOTag",
    "DrawOTag2",
    "DrawOTagEnv",
    "DumpOTag",
    "BreakDraw",
    "FntFlush",
    "KanjiFntFlush",
    "CheckPrim",
    "Krom2Tim",
})


@dataclass
class BoundaryViolation:
    callee: str
    param_index: int
    param_type: str
    location: str  # "<file>:<line>:<col>"
    note: str


@dataclass
class BoundaryResult:
    violations: List[BoundaryViolation] = dc_field(default_factory=list)
    safe_calls: int = 0  # diagnostic counter


def _is_ulong_pointer(ty: ci.Type) -> bool:
    """True if the parameter is ``u_long *`` / ``unsigned long *``.

    Detects by canonical pointee kind (UCHAR/USHORT/etc. won't match) so
    the check is robust against typedef chains (``u_long`` -> ``unsigned long``,
    or vendored aliases).
    """
    canon = ty.get_canonical()
    if canon.kind != ci.TypeKind.POINTER:
        return False
    pointee = canon.get_pointee().get_canonical()
    # ``unsigned long`` - distinct kind from ``unsigned int`` even when both
    # are 4 bytes under our mipsel target.
    return pointee.kind == ci.TypeKind.ULONG


def _format_location(c: ci.Cursor) -> str:
    if c.location.file is None:
        return "<unknown>"
    return f"{c.location.file.name}:{c.location.line}:{c.location.column}"


def run(tu_cursor: ci.Cursor, main_path: str) -> BoundaryResult:
    """Walk the TU, flag any ``u_long *``-taking call to a non-safe API."""
    result = BoundaryResult()

    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.CALL_EXPR:
            continue
        # Restrict to call sites in the TU's main file. Calls inside
        # transitively-included headers (e.g. inline helpers) aren't part
        # of this TU's migration surface.
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

        # Examine each parameter type.
        for idx, param in enumerate(callee.get_arguments()):
            if not _is_ulong_pointer(param.type):
                continue
            if name in SAFE_PSYQ_API:
                result.safe_calls += 1
                continue
            note = ("OT_TAG-strided host layout - needs psyq_boundary_copy shim"
                    if name in BLOCKED_PSYQ_API
                    else "unaudited u_long* API - add to SAFE_PSYQ_API after "
                         "verifying it byte-walks, or stage a copy-shim")
            result.violations.append(BoundaryViolation(
                callee=name,
                param_index=idx,
                param_type=param.type.spelling,
                location=_format_location(c),
                note=note,
            ))
            # One violation per call site is enough; don't double-report
            # multi-u_long* signatures.
            break

    return result


def format_violations(result: BoundaryResult) -> str:
    if not result.violations:
        return ""
    lines = ["Pass 5b: PsyCross u_long* boundary guard tripped:"]
    for v in result.violations:
        lines.append(f"  {v.location}: call to {v.callee}() - "
                     f"param {v.param_index} ({v.param_type})")
        lines.append(f"    {v.note}")
    return "\n".join(lines)
