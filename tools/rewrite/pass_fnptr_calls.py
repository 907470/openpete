"""Pass 4d — fn-pointer global call dispatch.

Matching-decomp source stores PSX function vaddrs in scalar globals
(`extern void (*D_800758E4)(int, ...)`, `extern Moby *(*g_SpawnMoby)(...)`)
and calls them as if they were host function pointers:

    (*D_800758E4)(16, 77, &vec, nullptr);
    Moby *m = (*g_SpawnMoby)(MOBYCLASS_NUMBER_0, nullptr);

The linker maps the global to its slot inside `g_ram` (4 bytes holding a
PSX vaddr). The host C declaration reports the pointer as 8-byte fn-ptr
typed; reading and calling it dereferences a host-aligned 8-byte slot
that overlays the 4-byte vaddr plus 4 bytes of adjacent storage — a
garbage host address that SIGSEGVs as soon as you call it.

Pass 4d detects these call sites and rewrites them to dispatch through
`openpete_psx_call_fnptr(vaddr, a0, a1, a2, a3)` — a runtime helper
that loads `cpu->aN`, calls `call_by_address`, and returns `cpu->v0`.

    (*GLOBAL)(arg1, arg2, arg3, arg4)
    →
    openpete_psx_call_fnptr(
        *(const uint32_t*)&GLOBAL,
        <coerce arg1 to uint32_t>, ..., <coerce arg4>
    )

For pointer-typed args, the coercion is `openpete_host_to_psx(arg)`;
for integer-typed args, `(uint32_t)(arg)`. Up to 4 register args are
supported today (matching-decomp's fn-ptr-global call sites use ≤ 4).

Return-value handling depends on the call expression's type:
  - void:    bare call, no cast wrap.
  - pointer: `((T*)openpete_psx_to_host_ptr(<call>))`
  - integer: `((IntT)<call>)`

Pass 2 already skips fn-ptr scalar globals at classification time, so
the global keeps its native linker symbol (defsym maps to `g_ram +
offset`). `&GLOBAL` is the host address of that 4-byte storage; reading
it as `uint32_t` lifts out the PSX vaddr.
"""

from __future__ import annotations

import re
from typing import Dict, List, Optional

import clang.cindex as ci

from .rewriter import Edit
from .types import _is_pointer_kind, _is_function_ptr


# Trailing 8-hex-digit suffix on a function name encodes its PSX vaddr.
# Used as a fallback when the function isn't in symbols.toml — every
# overlay-defined `func_level_<lvl>_<HEX>` follows this convention.
_HEX_SUFFIX_RE = re.compile(r"_([0-9A-Fa-f]{8})$")


def _unwrap_callee(callee: ci.Cursor) -> Optional[ci.Cursor]:
    """Walk a CALL_EXPR's first child through the wrappers C uses around
    function pointers and return the underlying DECL_REF_EXPR, or None."""
    cur = callee
    for _ in range(8):
        if cur.kind == ci.CursorKind.DECL_REF_EXPR:
            return cur
        if cur.kind not in (
            ci.CursorKind.UNEXPOSED_EXPR,
            ci.CursorKind.PAREN_EXPR,
            ci.CursorKind.UNARY_OPERATOR,  # `(*GLOBAL)(...)` — explicit deref
        ):
            return None
        kids = list(cur.get_children())
        if not kids:
            return None
        cur = kids[0]
    return None


def _is_fnptr_global(decl: ci.Cursor) -> bool:
    """True if `decl` is a file-scope VAR_DECL whose type is a function
    pointer (or array of one). Static-storage VAR_DECLs are excluded —
    those have no defsym alias and no PSX-vaddr storage."""
    if decl is None or decl.kind != ci.CursorKind.VAR_DECL:
        return False
    if decl.semantic_parent is None:
        return False
    if decl.semantic_parent.kind != ci.CursorKind.TRANSLATION_UNIT:
        return False
    if decl.storage_class == ci.StorageClass.STATIC:
        return False
    canon = decl.type.get_canonical()
    if canon.kind == ci.TypeKind.CONSTANTARRAY:
        canon = canon.element_type.get_canonical()
    return _is_pointer_kind(canon) and _is_function_ptr(canon)


def _resolve_fn_psx_vaddr(rhs: ci.Cursor,
                          symbols: Dict[str, int]) -> Optional[int]:
    """If `rhs` is a function reference whose PSX vaddr is known (either
    via `symbols.toml` or via the trailing-hex naming convention),
    return the vaddr. Mirrors `_resolve_function_psx_vaddr` in
    pass_struct_relayout for fn-ptr struct fields, but adds the
    name-suffix fallback for overlay funcs that aren't in symbols.toml
    by index — those still encode their vaddr in the name (e.g.
    `func_level_0_80080548`)."""
    cur = rhs
    for _ in range(6):
        if cur.kind == ci.CursorKind.DECL_REF_EXPR:
            ref = cur.referenced
            if ref is None or ref.kind != ci.CursorKind.FUNCTION_DECL:
                return None
            name = ref.spelling
            if name in symbols:
                return symbols[name]
            m = _HEX_SUFFIX_RE.search(name)
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


def _bytes_slice(file_path: str, start: int, end: int) -> str:
    with open(file_path, "rb") as fh:
        fh.seek(start)
        return fh.read(end - start).decode("utf-8", "replace")


def _coerce_arg(arg: ci.Cursor, main_path: str) -> str:
    """Render `arg` as a uint32_t value: pointer args → host_to_psx,
    integer args → (uint32_t) cast."""
    text = _bytes_slice(main_path, arg.extent.start.offset, arg.extent.end.offset)
    canon = arg.type.get_canonical()
    if _is_pointer_kind(canon):
        return f"openpete_host_to_psx({text})"
    return f"(uint32_t)({text})"


def rewrite_fnptr_global_calls(tu_cursor: ci.Cursor,
                               main_path: str) -> List[Edit]:
    """Walk the TU. For every `(*GLOBAL)(args...)` or `GLOBAL(args...)`
    site whose callee resolves to a fn-ptr scalar global, emit an Edit
    that replaces the entire call expression with the dispatch form.

    Skips call sites whose argument count exceeds 4 — those need stack
    args and are out of scope for this pass; a warning surfaces upstream.
    """
    edits: List[Edit] = []
    skipped_too_many_args: List[str] = []

    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.CALL_EXPR:
            continue
        if c.location.file is None or str(c.location.file.name) != main_path:
            continue
        kids = list(c.get_children())
        if not kids:
            continue
        callee = kids[0]
        ref = _unwrap_callee(callee)
        if ref is None:
            continue
        decl = ref.referenced
        if not _is_fnptr_global(decl):
            continue
        args = kids[1:]
        if len(args) > 4:
            skipped_too_many_args.append(
                f"{decl.spelling} at "
                f"{c.location.file.name}:{c.location.line}: "
                f"{len(args)} args (>4 not supported)")
            continue

        coerced: List[str] = []
        for a in args:
            coerced.append(_coerce_arg(a, main_path))
        # Pad to 4.
        while len(coerced) < 4:
            coerced.append("0u")

        global_name = decl.spelling
        vaddr_load = f"(*(const uint32_t*)(uintptr_t)&{global_name})"
        call = (f"openpete_psx_call_fnptr({vaddr_load}, "
                f"{coerced[0]}, {coerced[1]}, {coerced[2]}, {coerced[3]})")

        # Wrap by return type: pointer -> psx_to_host_ptr, int -> cast.
        ret_canon = c.type.get_canonical()
        if ret_canon.kind == ci.TypeKind.VOID:
            replacement = call
        elif _is_pointer_kind(ret_canon):
            ret_spelling = c.type.spelling
            replacement = (f"(({ret_spelling})openpete_psx_to_host_ptr("
                           f"{call}))")
        else:
            ret_spelling = c.type.spelling
            replacement = f"(({ret_spelling}){call})"

        edits.append(Edit(
            start=c.extent.start.offset,
            end=c.extent.end.offset,
            replacement=replacement,
            file=main_path,
            note=f"fnptr-call dispatch: {global_name}",
        ))

    if skipped_too_many_args:
        # Diagnostic only — the upstream caller decides what to do.
        for msg in skipped_too_many_args:
            print(f"[pass_fnptr_calls] skipped: {msg}")
    return edits


def rewrite_fnptr_global_writes(tu_cursor: ci.Cursor,
                                main_path: str,
                                symbols: Dict[str, int]) -> List[Edit]:
    """Walk the TU. For every `GLOBAL = function_name;` assignment whose
    LHS is a fn-ptr scalar global (skipped by Pass 2) and whose RHS is a
    function reference with a known PSX vaddr, rewrite the assignment to:

        *(uint32_t*)(uintptr_t)&GLOBAL = <vaddr>u

    Without this, the host C code references the function by name, but
    overlay-defined functions have no host symbol — link fails. Storing
    the literal vaddr matches what the original PSX EXE has at the
    storage slot.
    """
    edits: List[Edit] = []
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.BINARY_OPERATOR:
            continue
        if c.location.file is None or str(c.location.file.name) != main_path:
            continue
        kids = list(c.get_children())
        if len(kids) != 2:
            continue
        lhs, rhs = kids
        # Op token between lhs and rhs must be plain `=`.
        op_tok = None
        try:
            tokens = list(c.translation_unit.get_tokens(extent=c.extent))
        except Exception:
            tokens = []
        for tok in tokens:
            if (tok.location.offset >= lhs.extent.end.offset
                    and tok.location.offset < rhs.extent.start.offset
                    and tok.kind == ci.TokenKind.PUNCTUATION):
                op_tok = tok.spelling
                break
        if op_tok != "=":
            continue
        # LHS must resolve to a fn-ptr scalar global DeclRef.
        lhs_ref = _unwrap_callee(lhs)  # same wrappers
        if lhs_ref is None:
            continue
        lhs_decl = lhs_ref.referenced
        if not _is_fnptr_global(lhs_decl):
            continue
        vaddr = _resolve_fn_psx_vaddr(rhs, symbols)
        if vaddr is None:
            continue
        replacement = (f"*(uint32_t*)(uintptr_t)&{lhs_decl.spelling} = "
                       f"0x{vaddr:08X}u")
        edits.append(Edit(
            start=c.extent.start.offset,
            end=c.extent.end.offset,
            replacement=replacement,
            file=main_path,
            note=f"fnptr-global write: {lhs_decl.spelling}",
        ))
    return edits
