"""Pass 5c — INCLUDE_ASM splice removal.

Matching-decomp source uses ``INCLUDE_ASM(folder, name);`` and
``INCLUDE_ASM_REORDER_HACK(folder, name);`` to embed not-yet-decompiled
functions. The macro expands to a file-scope ``__asm__`` block with
``.include "folder/name.s"`` — fine for the PSX matching toolchain, but
the host linker has no ``.s`` file to splice and the inline asm has MIPS
mnemonics anyway.

Pass 5c removes those macro invocations from the rewritten source. The
function those splices were defining still exists at link time via the
recompiled body in ``PSX.EXE_full.c`` (or ``<overlay>.bin_full.c`` for
overlays). If the same TU's C code calls one of those functions, Pass 6's
outbound discovery already classifies it as a recomp callee and the
existing thunk pipeline routes the call through ``psx_dispatch_compiled``.

Detection is textual rather than AST-based — INCLUDE_ASM is a macro that
libclang expands away before AST construction, so the AST has only the
``__asm__`` blob. The regex below matches both the bare and REORDER_HACK
forms in the original source. Robust because both macros take exactly
``(FOLDER, NAME)``: a string literal and an identifier, with no nesting.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field as dc_field
from pathlib import Path
from typing import List

from .rewriter import Edit


# Match either INCLUDE_ASM or INCLUDE_ASM_REORDER_HACK, whitespace-tolerant,
# capturing the name for diagnostics. Trailing semicolon swallowed too.
_INCLUDE_ASM_RE = re.compile(
    r"""
    \bINCLUDE_ASM(?:_REORDER_HACK)?      # macro name
    \s*\(
        \s*"(?P<folder>[^"]*)"           # FOLDER literal
        \s*,
        \s*(?P<name>[A-Za-z_]\w*)        # NAME identifier
        \s*
    \)
    \s*;?                                # optional trailing semicolon
    """,
    re.VERBOSE,
)


@dataclass
class IncludeAsmSplice:
    name: str             # function name spliced
    folder: str           # asm subdirectory (diagnostic)
    line: int             # source line in the input TU


@dataclass
class IncludeAsmResult:
    edits: List[Edit] = dc_field(default_factory=list)
    splices: List[IncludeAsmSplice] = dc_field(default_factory=list)


def _line_for_offset(src: str, offset: int) -> int:
    return src.count("\n", 0, offset) + 1


def run(input_path: Path) -> IncludeAsmResult:
    """Scan the TU's source text and emit deletion edits over every
    INCLUDE_ASM / INCLUDE_ASM_REORDER_HACK invocation."""
    result = IncludeAsmResult()
    text = input_path.read_text()
    main_file = str(input_path.resolve())
    for m in _INCLUDE_ASM_RE.finditer(text):
        result.edits.append(Edit(
            start=m.start(),
            end=m.end(),
            replacement="",
            file=main_file,
            note=f"INCLUDE_ASM splice removal: {m.group('name')}",
        ))
        result.splices.append(IncludeAsmSplice(
            name=m.group("name"),
            folder=m.group("folder"),
            line=_line_for_offset(text, m.start()),
        ))
    return result
