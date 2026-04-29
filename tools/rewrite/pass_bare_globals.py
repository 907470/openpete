"""Pass 2 — bare-global redirects.

Top-level globals whose declared type diverges between PSX (ILP32) and host
(LP64 / LLP64) get redirected through a uint32_t storage slot so that
--defsym aliasing into g_ram is byte-correct.

Scope of this pass (MVP):
  - Scalar diverging globals: `extern T *name;`, `extern long name;`,
    `extern u_long name;`. Pointer/long-family leaf decls.
  - Reads: identifier becomes `((<orig_t>)openpete_psx_to_host_ptr(__psx_<name>))`
    for pointer types, or `((int32_t)__psx_<name>)` for long-family types.
  - Writes: `name = expr;` becomes `__psx_<name> = openpete_host_to_psx(expr);`
    for pointer types, or `__psx_<name> = (int32_t)(expr);` for long.
  - Address-of: `&name` is unsupported in this pass (the storage *has* an
    address — `&__psx_<name>` — but it's `uint32_t*`, not the original
    pointer type the source expected). Flag as a manual-review error.

Out of scope:
  - Array globals (`extern T *name[N];`, `extern u_long name[N];`).
  - Struct globals containing diverging fields — that's Pass 1 territory.
  - Function-pointer globals.

Output:
  - Edits over the source TU's identifier references and assignments.
  - A prologue snippet (returned separately) that the driver prepends to
    the rewritten TU: `extern uint32_t __psx_<name>;` declarations plus
    a `#define <name> __opete_unused_<name>` sterilization macro that
    neutralizes any unrewritten reference (e.g. a header decl that the
    rewriter doesn't textually edit but that gets re-included after our
    prologue).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Set, Tuple

import clang.cindex as ci

from .rewriter import Edit, edit_from_range
from .types import _is_long_family, _is_pointer_kind, _is_function_ptr


@dataclass
class GlobalRedirect:
    name: str
    psx_storage_name: str       # __psx_<name>
    is_pointer: bool
    is_long: bool
    pointer_orig_type: str = ""  # original type spelling for the pointer cast
    pointee_type: str = ""       # element type for scalar pointer redirects (for sizeof)
    is_array: bool = False        # True for `T *name[N]` / `u_long name[N]`
    array_count: int = 0          # N (only valid when is_array)
    elem_orig_type: str = ""      # element type for array redirects (e.g. "Model *")


@dataclass
class BareGlobalsResult:
    edits: List[Edit] = field(default_factory=list)
    redirects: Dict[str, GlobalRedirect] = field(default_factory=dict)
    address_of_errors: List[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------

def _is_scalar_pointer_global(t: ci.Type) -> bool:
    canon = t.get_canonical()
    if canon.kind == ci.TypeKind.CONSTANTARRAY:
        return False
    return _is_pointer_kind(canon)


def _is_scalar_long_global(t: ci.Type) -> bool:
    canon = t.get_canonical()
    if canon.kind == ci.TypeKind.CONSTANTARRAY:
        return False
    return _is_long_family(t)


def _classify_global(decl: ci.Cursor) -> GlobalRedirect | None:
    """Return a GlobalRedirect for an extern/file-scope decl that diverges
    in width, else None."""
    if decl.kind != ci.CursorKind.VAR_DECL:
        return None
    if decl.semantic_parent.kind != ci.CursorKind.TRANSLATION_UNIT:
        return None  # not file-scope
    if decl.storage_class == ci.StorageClass.STATIC:
        return None  # internal linkage; --defsym can't reach it
    t = decl.type
    name = decl.spelling
    canon = t.get_canonical()
    # Array case: `T *name[N]` / `u_long name[N]`. Element type drives
    # the divergence test (host pointer is 8 bytes; PSX vaddr is 4).
    if canon.kind == ci.TypeKind.CONSTANTARRAY:
        elem = canon.element_type.get_canonical()
        elem_is_ptr = _is_pointer_kind(elem)
        elem_is_long = _is_long_family(canon.element_type)
        if not (elem_is_ptr or elem_is_long):
            return None
        if elem_is_ptr and _is_function_ptr(elem):
            # Array of function pointers — same Phase-4 deferral as
            # function-pointer struct fields. Skip; warn upstream.
            return None
        return GlobalRedirect(
            name=name,
            psx_storage_name=f"__psx_{name}",
            is_pointer=elem_is_ptr,
            is_long=elem_is_long,
            pointer_orig_type=(t.element_type.spelling
                               if t.kind == ci.TypeKind.CONSTANTARRAY
                               else canon.element_type.spelling),
            is_array=True,
            array_count=canon.element_count,
            elem_orig_type=(t.element_type.spelling
                            if t.kind == ci.TypeKind.CONSTANTARRAY
                            else canon.element_type.spelling),
        )
    is_ptr = _is_scalar_pointer_global(t)
    is_long = _is_scalar_long_global(t)
    if not (is_ptr or is_long):
        return None
    if is_ptr and _is_function_ptr(canon):
        # Scalar function-pointer global (e.g. `void (*D_xxx)(int, ...)`).
        # Same Phase-4 deferral as function-pointer struct fields and
        # arrays: a naive `psx_to_host_ptr(__psx_name)` wrap yields a
        # pointer to the recomp's RAM image, not a callable host
        # function. The caller would jump into data and SIGSEGV.
        return None
    pointee_spelling = ""
    if is_ptr:
        try:
            pointee_spelling = t.get_pointee().spelling or canon.get_pointee().spelling
        except Exception:
            pointee_spelling = ""
    return GlobalRedirect(
        name=name,
        psx_storage_name=f"__psx_{name}",
        is_pointer=is_ptr,
        is_long=is_long,
        pointer_orig_type=t.spelling if is_ptr else "",
        pointee_type=pointee_spelling,
    )


# ---------------------------------------------------------------------------
# Walk
# ---------------------------------------------------------------------------

def _is_assignment_lhs(ref: ci.Cursor) -> Tuple[bool, ci.Cursor | None]:
    """If `ref` is the immediate LHS of `lhs = rhs`, return (True, parent BinOp)."""
    parent = ref.semantic_parent  # not what we want — we need lexical parent
    # libclang doesn't expose lexical parent directly; we walk operator children.
    # Instead, the caller passes the BinaryOperator and we test if `ref` is its
    # first child.
    return False, None


def _find_assignment_writes(tu_cursor: ci.Cursor,
                            redirects: Dict[str, GlobalRedirect]
                            ) -> Dict[int, Tuple[ci.Cursor, ci.Cursor, GlobalRedirect]]:
    """Return {ref_offset: (binop_cursor, ref_cursor, redirect)} for every
    `name = rhs` site where `name` is a redirected global."""
    writes: Dict[int, Tuple[ci.Cursor, ci.Cursor, GlobalRedirect]] = {}
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.BINARY_OPERATOR:
            continue
        # libclang doesn't expose the operator spelling for BinaryOperator.
        # Reconstruct from the source text between the LHS and RHS children.
        children = list(c.get_children())
        if len(children) != 2:
            continue
        lhs, rhs = children
        if lhs.kind != ci.CursorKind.DECL_REF_EXPR:
            continue
        ref_decl = lhs.referenced
        if ref_decl is None:
            continue
        red = redirects.get(ref_decl.spelling)
        if red is None:
            continue
        # Pull the operator token between lhs.end and rhs.start.
        try:
            tu = c.translation_unit
            tokens = list(tu.get_tokens(extent=c.extent))
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
        writes[lhs.extent.start.offset] = (c, lhs, red)
    return writes


def find_array_write_extents(tu_cursor: ci.Cursor,
                             redirects: Dict[str, GlobalRedirect]
                             ) -> List[Tuple[int, int]]:
    """Return BinaryOperator extents for `arr[i] = rhs` writes through a
    redirected array global. Pass 1 consults these so its read edits don't
    overlap Pass 2's whole-binop array-write replacement."""
    extents: List[Tuple[int, int]] = []
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.BINARY_OPERATOR:
            continue
        children = list(c.get_children())
        if len(children) != 2:
            continue
        lhs, rhs = children
        if lhs.kind != ci.CursorKind.ARRAY_SUBSCRIPT_EXPR:
            continue
        lhs_kids = list(lhs.get_children())
        if not lhs_kids:
            continue
        base = lhs_kids[0]
        while base.kind == ci.CursorKind.UNEXPOSED_EXPR:
            kids = list(base.get_children())
            if not kids:
                break
            base = kids[0]
        if base.kind != ci.CursorKind.DECL_REF_EXPR:
            continue
        rd = base.referenced
        if rd is None:
            continue
        red = redirects.get(rd.spelling)
        if red is None or not red.is_array:
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
        extents.append((c.extent.start.offset, c.extent.end.offset))
    return extents


def find_array_read_extents(tu_cursor: ci.Cursor,
                            redirects: Dict[str, GlobalRedirect]
                            ) -> List[Tuple[int, int]]:
    """Return ARRAY_SUBSCRIPT_EXPR extents for `arr[i]` reads through a
    redirected array global, when the array sub isn't itself the LHS of
    an assignment. Pass 1 consults these so its inner read edits don't
    overlap Pass 2's array-sub read replacement (which already folds
    Pass 1 substitutions inline via _render_with_redirects)."""
    extents: List[Tuple[int, int]] = []
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.ARRAY_SUBSCRIPT_EXPR:
            continue
        kids = list(c.get_children())
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
        s, e = c.extent.start.offset, c.extent.end.offset
        if e <= s:
            continue
        extents.append((s, e))
    return extents


def _is_address_of(ref: ci.Cursor, parent: ci.Cursor) -> bool:
    """True if `ref` appears under a `&ref` UnaryOperator."""
    # We don't have lexical parent; the caller passes the candidate UnaryOp.
    if parent.kind != ci.CursorKind.UNARY_OPERATOR:
        return False
    children = list(parent.get_children())
    if len(children) != 1 or children[0] != ref:
        return False
    # Is the operator '&'?
    try:
        tu = parent.translation_unit
        tokens = list(tu.get_tokens(extent=parent.extent))
    except Exception:
        return False
    if not tokens:
        return False
    return tokens[0].spelling == "&"


# ---------------------------------------------------------------------------
# Public entry
# ---------------------------------------------------------------------------

def collect_redirects(tu_cursor: ci.Cursor) -> Dict[str, GlobalRedirect]:
    """Walk the TU and return all qualifying bare-global redirects, keyed by
    original name. Only includes globals that are actually *referenced* in
    the main TU's source file (avoids redirecting every header decl in
    libpsyq we never use), and that have at least one non-address-of
    reference (linker-defined boundary symbols like `main_BSS_END` are
    declared as pointer-typed but only ever address-of'd to obtain the
    symbol's link-time address — they have no PSX storage to alias)."""
    decls: Dict[str, GlobalRedirect] = {}
    referenced: Set[str] = set()
    addr_of_only: Dict[str, int] = {}  # name -> outstanding non-addr-of count
    main_path = None
    # First pass: collect decls and identify address-of contexts.
    addr_of_offsets: Set[int] = set()
    for c in tu_cursor.walk_preorder():
        if main_path is None and c.kind == ci.CursorKind.TRANSLATION_UNIT:
            main_path = str(c.translation_unit.spelling)
        if c.kind == ci.CursorKind.UNARY_OPERATOR:
            try:
                toks = list(c.translation_unit.get_tokens(extent=c.extent))
            except Exception:
                toks = []
            if toks and toks[0].spelling == "&":
                kids = list(c.get_children())
                if len(kids) == 1 and kids[0].kind == ci.CursorKind.DECL_REF_EXPR:
                    addr_of_offsets.add(kids[0].extent.start.offset)
    for c in tu_cursor.walk_preorder():
        if c.kind == ci.CursorKind.VAR_DECL:
            red = _classify_global(c)
            if red is not None and red.name not in decls:
                decls[red.name] = red
            # A file-scope DEFINITION of the global in the main TU also
            # counts as a "reference" — Pass 2 needs to elide that def
            # (otherwise its initializer pulls in linker-only symbols
            # like `&main_BSS_END` and the host link fails). The defsym
            # emitter sees the redirect and aliases the symbol to g_ram.
            if c.location.file is not None and \
                    main_path and str(c.location.file.name) == main_path and \
                    c.semantic_parent is not None and \
                    c.semantic_parent.kind == ci.CursorKind.TRANSLATION_UNIT and \
                    c.storage_class != ci.StorageClass.EXTERN and \
                    red is not None:
                referenced.add(red.name)
                addr_of_only[red.name] = addr_of_only.get(red.name, 0) + 1
        elif c.kind == ci.CursorKind.DECL_REF_EXPR:
            if c.location.file is None:
                continue
            if main_path and str(c.location.file.name) != main_path:
                continue
            ref = c.referenced
            if ref is None:
                continue
            referenced.add(ref.spelling)
            if c.extent.start.offset not in addr_of_offsets:
                addr_of_only[ref.spelling] = addr_of_only.get(ref.spelling, 0) + 1
    return {n: r for n, r in decls.items()
            if n in referenced and addr_of_only.get(n, 0) > 0}


def rewrite_bare_globals(tu_cursor: ci.Cursor,
                         redirects: Dict[str, GlobalRedirect],
                         main_file: str,
                         vaddr_form_offsets: Set[int] | None = None,
                         vaddr_form_array_sub_extents: Set[Tuple[int, int]] | None = None,
                         pass1_covered_ranges: List[Tuple[int, int]] | None = None,
                         pass1_rewrites=None
                         ) -> BareGlobalsResult:
    """Generate edits for every read/write of a redirected global within
    the main TU's source file.

    ``vaddr_form_offsets`` (Pass 4 output) is the set of read DeclRefExpr
    offsets whose replacement should be the bare ``__psx_<name>`` storage
    instead of the host-pointer round-trip — used at sites where the
    operand sits inside ``(IntT)<global>``, where the cast already expresses
    the desired narrowing.
    """
    result = BareGlobalsResult()
    result.redirects = redirects
    if not redirects:
        return result
    if vaddr_form_offsets is None:
        vaddr_form_offsets = set()
    if vaddr_form_array_sub_extents is None:
        vaddr_form_array_sub_extents = set()
    pass1_ranges = sorted(pass1_covered_ranges or [])

    def _inside_pass1(s: int, e: int) -> bool:
        # Pass 1 already rendered the redirect form inline for any ref
        # falling inside its access edit range. Skip here to avoid the
        # overlapping-edit assertion in the splicer.
        for rs, re_ in pass1_ranges:
            if s >= rs and e <= re_:
                return True
            if rs > s:
                break
        return False

    main_file_resolved = str(main_file)

    # Pre-find write sites so we can skip emitting a read-conversion at the LHS.
    writes = _find_assignment_writes(tu_cursor, redirects)
    # Pass-2-internal: write rewrites cover the whole BinaryOperator
    # extent and re-emit the RHS textually. Any redirected ref appearing
    # inside that RHS would otherwise generate its own read edit which
    # overlaps with the write. Track the write extents to suppress.
    pass2_write_ranges: List[Tuple[int, int]] = []
    for off, (binop, _lhs, _r) in writes.items():
        pass2_write_ranges.append(
            (binop.extent.start.offset, binop.extent.end.offset))
    pass2_write_ranges.sort()

    def _inside_pass2_write(s: int, e: int, lhs_off: int) -> bool:
        for ws, we in pass2_write_ranges:
            if s >= ws and e <= we and s != lhs_off:
                return True
            if ws > s:
                break
        return False

    # Detect address-of contexts: walk UnaryOperator parents.
    addr_of_offsets: Set[int] = set()
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.UNARY_OPERATOR:
            continue
        children = list(c.get_children())
        if len(children) != 1:
            continue
        ref = children[0]
        if ref.kind != ci.CursorKind.DECL_REF_EXPR:
            continue
        rd = ref.referenced
        if rd is None or rd.spelling not in redirects:
            continue
        if _is_address_of(ref, c):
            addr_of_offsets.add(ref.extent.start.offset)
            result.address_of_errors.append(
                f"&{rd.spelling} at {ref.location.file}:{ref.location.line} "
                f"— bare-globals pass cannot redirect address-of (storage "
                f"is uint32_t*, not original type)")

    # File-scope definitions of redirected globals — `void *g_X = &init;`
    # or `int g_X = 5;`. The defsym alias provides the linker symbol, and
    # the storage lives in g_ram populated by the EXE-load step. The C
    # initializer is dead code on host (the `#define name __opete_unused_*`
    # in the prologue lets the def compile, but the actual byte slot lives
    # elsewhere). Worse, the initializer can reference linker symbols the
    # host doesn't have (e.g. `&main_BSS_END`), causing link errors. Strip
    # the entire file-scope decl so neither host storage nor unresolved
    # initializer references survive.
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.VAR_DECL:
            continue
        if c.location.file is None:
            continue
        if str(c.location.file.name) != main_file_resolved:
            continue
        if c.semantic_parent is None or \
                c.semantic_parent.kind != ci.CursorKind.TRANSLATION_UNIT:
            continue
        if c.spelling not in redirects:
            continue
        # Only strip true definitions (have an initializer or no `extern`
        # storage class). Bare `extern T name;` decls stay — they don't
        # produce host storage and don't have initializers.
        if c.storage_class == ci.StorageClass.EXTERN and \
                not list(c.get_children()):
            # Forward `extern T name;` — leave it.
            continue
        # Find the trailing semicolon to extend the cut to the end of the
        # statement. Tokens up to and including the first `;` after the
        # decl extent.
        end = c.extent.end.offset
        try:
            for tok in c.translation_unit.get_tokens(
                    extent=ci.SourceRange.from_locations(
                        c.extent.end, c.extent.end)):
                pass
        except Exception:
            pass
        # Simpler: scan forward from extent.end via raw bytes for `;`.
        with open(main_file_resolved, "rb") as fh:
            fh.seek(end)
            tail = fh.read(64)
        semi = tail.find(b";")
        if semi >= 0:
            end = end + semi + 1
        result.edits.append(Edit(
            start=c.extent.start.offset,
            end=end,
            replacement=(f"/* file-scope def of redirected global "
                         f"{c.spelling} elided — storage in g_ram */"),
            file=main_file_resolved,
            note=f"elide file-scope def of {c.spelling}",
        ))

    # Pointer-global post/pre-inc/dec. `g_Ptr++` on a redirected scalar
    # pointer global produces an rvalue from the read form, so a plain
    # read-substitution at the DeclRefExpr breaks the source. Detect the
    # enclosing UnaryOperator (`++`/`--`, pre or post) and rewrite the
    # whole expression to advance the PSX storage slot transactionally
    # and yield the appropriate vaddr converted to a host pointer.
    incdec_skip_offsets: Set[int] = set()
    incdec_replaced_ranges: List[Tuple[int, int]] = []
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.UNARY_OPERATOR:
            continue
        if c.location.file is None:
            continue
        if str(c.location.file.name) != main_file_resolved:
            continue
        children = list(c.get_children())
        if len(children) != 1:
            continue
        inner = children[0]
        deref = inner
        while deref.kind == ci.CursorKind.UNEXPOSED_EXPR:
            kids = list(deref.get_children())
            if not kids:
                break
            deref = kids[0]
        if deref.kind != ci.CursorKind.DECL_REF_EXPR:
            continue
        rd = deref.referenced
        if rd is None:
            continue
        red = redirects.get(rd.spelling)
        if red is None or not red.is_pointer or red.is_array:
            continue
        try:
            toks = list(c.translation_unit.get_tokens(extent=c.extent))
        except Exception:
            continue
        if not toks:
            continue
        op_kind = None  # ("pre"|"post", "++"|"--")
        if toks[0].spelling in ("++", "--"):
            op_kind = ("pre", toks[0].spelling)
        elif toks[-1].spelling in ("++", "--"):
            op_kind = ("post", toks[-1].spelling)
        else:
            continue
        pos, opsym = op_kind
        sign = "+=" if opsym == "++" else "-="
        inv = "-" if opsym == "++" else "+"
        pointee = red.pointee_type or "char"
        if pos == "pre":
            replacement = (
                f"(({red.pointer_orig_type})openpete_psx_to_host_ptr("
                f"{red.psx_storage_name} {sign} sizeof({pointee})))")
        else:
            replacement = (
                f"(({red.pointer_orig_type})openpete_psx_to_host_ptr("
                f"({red.psx_storage_name} {sign} sizeof({pointee})) "
                f"{inv} sizeof({pointee})))")
        result.edits.append(Edit(
            start=c.extent.start.offset,
            end=c.extent.end.offset,
            replacement=replacement,
            file=main_file_resolved,
            note=f"{pos}{opsym} {red.name}"))
        incdec_skip_offsets.add(deref.extent.start.offset)
        incdec_replaced_ranges.append(
            (c.extent.start.offset, c.extent.end.offset))
    incdec_replaced_ranges.sort()

    def _inside_incdec(s: int, e: int) -> bool:
        for rs, re_ in incdec_replaced_ranges:
            if s >= rs and e <= re_:
                return True
            if rs > s:
                break
        return False

    # Array-redirect subscript pass. For `T *arr[N]` → `uint32_t arr[N]`
    # storage, an access `arr[i]` reads the i'th vaddr and we must
    # convert it to a host pointer before any further dereferencing.
    # Symmetric to Pass 1's array-of-pointer-field handling at the
    # subscript level (cannot cast to `T *[N]` — that's invalid C).
    array_sub_skip_offsets: Set[int] = set()
    array_sub_edits: Dict[Tuple[int, int], Edit] = {}
    for sub in tu_cursor.walk_preorder():
        if sub.kind != ci.CursorKind.ARRAY_SUBSCRIPT_EXPR:
            continue
        if sub.location.file is None:
            continue
        if str(sub.location.file.name) != main_file_resolved:
            continue
        children = list(sub.get_children())
        if not children:
            continue
        base = children[0]
        while base.kind == ci.CursorKind.UNEXPOSED_EXPR:
            kids = list(base.get_children())
            if not kids:
                break
            base = kids[0]
        if base.kind != ci.CursorKind.DECL_REF_EXPR:
            continue
        rd = base.referenced
        if rd is None:
            continue
        red = redirects.get(rd.spelling)
        if red is None or not red.is_array:
            continue
        s = sub.extent.start.offset
        e = sub.extent.end.offset
        if e == s:
            continue
        # Macro-expansion artifact: an ARRAY_SUBSCRIPT_EXPR whose source
        # extent doesn't contain `[` (e.g. `SPYRO_MODEL` expanding to
        # `(g_Models[0])` — the cursor extent is at the SPYRO_MODEL
        # token in source, not the literal subscript). Skip — the
        # macro definition itself is the right rewrite target (handled
        # via shadow-header redef).
        sub_extent_text = bytes_slice(sub)
        if "[" not in sub_extent_text:
            array_sub_skip_offsets.add(base.extent.start.offset)
            continue
        if _inside_pass1(s, e):
            # Pass 1's render handles the array-subscript form inline.
            array_sub_skip_offsets.add(base.extent.start.offset)
            continue
        # Determine if this subscript is the LHS of a plain assignment.
        # `_find_assignment_writes` keys on lhs.start which for an
        # ARRAY_SUBSCRIPT lhs is the bare-array DeclRefExpr offset
        # (== `s`). Promote those to array-write rewrites here, and
        # mark the binop range so the DeclRefExpr loop later skips it.
        sub_text = bytes_slice(sub)
        # Index expression text — substitute redirected globals AND
        # Pass 1's relaid field-accesses nested inside the index inline,
        # so `g_Models[g_LevelMobys[i].m_Class]` renders both the outer
        # array-redirect and the inner relayed `m_Class` field correctly.
        idx = children[1] if len(children) > 1 else None
        if idx is not None:
            if pass1_rewrites is not None:
                from .pass_struct_relayout import _render_with_redirects as _r1
                idx_text = _r1(idx, main_file_resolved, redirects, pass1_rewrites)
            else:
                idx_text = bytes_slice(idx)
        else:
            idx_text = ""
        # Build the per-element read form. base text becomes __psx_arr.
        elem_access = f"{red.psx_storage_name}[{idx_text}]"
        if (s, e) in vaddr_form_array_sub_extents:
            # Pass 4: subscript site sits inside `(IntT)arr[i]` — the outer
            # cast already narrows; expose the PSX vaddr directly.
            replacement = elem_access
        elif red.is_pointer:
            replacement = (f"(({red.elem_orig_type})"
                           f"openpete_psx_to_host_ptr({elem_access}))")
        else:
            replacement = f"((int32_t){elem_access})"
        array_sub_edits[(s, e)] = Edit(
            start=s, end=e, replacement=replacement,
            file=main_file_resolved,
            note=f"read[] {red.name}")
        # Suppress the inner DeclRefExpr to the array's bare name.
        array_sub_skip_offsets.add(base.extent.start.offset)

    # Detect writes through an array subscript: `arr[i] = expr`. The
    # binop's LHS is the ARRAY_SUBSCRIPT_EXPR; we replace the whole
    # binop with `__psx_arr[i] = host_to_psx(expr)`.
    array_write_offsets: Set[int] = set()
    array_write_replacements: List[Edit] = []
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.BINARY_OPERATOR:
            continue
        children = list(c.get_children())
        if len(children) != 2:
            continue
        lhs, rhs = children
        if lhs.kind != ci.CursorKind.ARRAY_SUBSCRIPT_EXPR:
            continue
        lhs_kids = list(lhs.get_children())
        if not lhs_kids:
            continue
        base = lhs_kids[0]
        while base.kind == ci.CursorKind.UNEXPOSED_EXPR:
            kids = list(base.get_children())
            if not kids:
                break
            base = kids[0]
        if base.kind != ci.CursorKind.DECL_REF_EXPR:
            continue
        rd = base.referenced
        if rd is None:
            continue
        red = redirects.get(rd.spelling)
        if red is None or not red.is_array:
            continue
        # Confirm op is `=` (compound-assign on array elem isn't handled
        # yet; leave it to the read+RMW path or a future extension).
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
        idx = lhs_kids[1] if len(lhs_kids) > 1 else None
        if pass1_rewrites is not None:
            from .pass_struct_relayout import _render_with_redirects as _r1
            idx_text = (_r1(idx, str(main_file), redirects, pass1_rewrites)
                        if idx is not None else "")
            rhs_text = _r1(rhs, str(main_file), redirects, pass1_rewrites)
        else:
            idx_text = bytes_slice(idx) if idx is not None else ""
            rhs_text = bytes_slice(rhs)
        if red.is_pointer:
            replacement = (f"{red.psx_storage_name}[{idx_text}] = "
                           f"openpete_host_to_psx({rhs_text})")
        else:
            replacement = (f"{red.psx_storage_name}[{idx_text}] = "
                           f"(int32_t)({rhs_text})")
        array_write_replacements.append(Edit(
            start=c.extent.start.offset,
            end=c.extent.end.offset,
            replacement=replacement,
            file=main_file_resolved,
            note=f"write[] {red.name}"))
        # Suppress the read-form for the same subscript range.
        array_sub_edits.pop(
            (lhs.extent.start.offset, lhs.extent.end.offset), None)
        array_sub_skip_offsets.add(base.extent.start.offset)
        array_write_offsets.add(c.extent.start.offset)

    for ed in array_sub_edits.values():
        result.edits.append(ed)
    for ed in array_write_replacements:
        result.edits.append(ed)

    seen_read_offsets: Set[int] = set()
    for c in tu_cursor.walk_preorder():
        if c.kind != ci.CursorKind.DECL_REF_EXPR:
            continue
        if c.location.file is None or str(c.location.file.name) != main_file_resolved:
            continue
        ref_decl = c.referenced
        if ref_decl is None:
            continue
        red = redirects.get(ref_decl.spelling)
        if red is None:
            continue
        off = c.extent.start.offset
        if off in addr_of_offsets:
            continue  # emitted as error; leave site untouched
        if off in array_sub_skip_offsets:
            # Bare-array DeclRefExpr inside an ARRAY_SUBSCRIPT_EXPR we
            # already rewrote at the subscript level.
            continue
        # Skip DeclRefExpr inside any array-subscript-rewrite range —
        # the array-sub edit's replacement text uses raw bytes_slice on
        # the index expression, but the original index text already has
        # the source-level identifier; emitting our own edit here would
        # overlap.
        in_array_sub_edit = False
        for (ase_s, ase_e) in array_sub_edits.keys():
            if off >= ase_s and c.extent.end.offset <= ase_e \
                    and off != ase_s:
                in_array_sub_edit = True
                break
        if in_array_sub_edit:
            continue
        if off in incdec_skip_offsets:
            # DeclRefExpr inside `g_Ptr++`/`++g_Ptr` already rewritten at
            # the UnaryOperator level.
            continue
        # Skip macro-expansion artifacts: a DeclRefExpr from a macro arg
        # used N times in the expansion shows up as N AST nodes all
        # pointing at the same source extent (often zero-length). Apply
        # the rewrite once at that offset and dedupe the rest.
        if c.extent.end.offset == c.extent.start.offset:
            continue
        # Macro-expansion artifact #2: cursor extent points at a macro
        # use in source whose text doesn't contain the symbol's
        # spelling (e.g. `SPYRO_MODEL` expanding to `(g_Models[0])`).
        # Skip — shadow-header redef is the right rewrite target.
        if ref_decl.spelling not in bytes_slice(c):
            continue
        if off in seen_read_offsets and off not in writes:
            continue
        seen_read_offsets.add(off)
        # If Pass 1 already covers this ref (the ref is inside a relaid
        # field-access expression), Pass 1 has rendered the redirect form
        # inline — emitting our own edit here would overlap.
        if off not in writes and _inside_pass1(off, c.extent.end.offset):
            continue
        # Likewise for Pass 2's own write rewrites — a ref inside the
        # write's RHS gets folded into the write's replacement text.
        if off not in writes and _inside_pass2_write(off, c.extent.end.offset, -1):
            continue

        if off in writes:
            # Write site: rewrite `name = ...` → `__psx_name = host_to_psx(...)`
            binop, lhs, _r = writes[off]
            rhs_children = list(binop.get_children())
            rhs = rhs_children[1]
            if pass1_rewrites is not None:
                from .pass_struct_relayout import _render_with_redirects as _r1
                rhs_text = _r1(rhs, str(main_file), redirects, pass1_rewrites)
            else:
                rhs_text = bytes_slice(rhs)
            if red.is_pointer:
                replacement = (f"{red.psx_storage_name} = "
                               f"openpete_host_to_psx({rhs_text})")
            else:
                replacement = (f"{red.psx_storage_name} = "
                               f"(int32_t)({rhs_text})")
            result.edits.append(Edit(
                start=binop.extent.start.offset,
                end=binop.extent.end.offset,
                replacement=replacement,
                file=main_file_resolved,
                note=f"write {red.name}",
            ))
        else:
            # Read site: substitute the identifier with a converting expression.
            if off in vaddr_form_offsets:
                # Pass 4: site sits inside `(IntT)<global>` — the outer cast
                # already narrows; we just expose the PSX vaddr directly.
                replacement = red.psx_storage_name
            elif red.is_pointer:
                replacement = (f"(({red.pointer_orig_type})"
                               f"openpete_psx_to_host_ptr({red.psx_storage_name}))")
            else:
                replacement = f"((int32_t){red.psx_storage_name})"
            result.edits.append(Edit(
                start=c.extent.start.offset,
                end=c.extent.end.offset,
                replacement=replacement,
                file=main_file_resolved,
                note=f"read {red.name}",
            ))

    # De-dupe: a read at the same offset as a write would be a bug; assert.
    seen = set()
    for e in result.edits:
        key = (e.file, e.start, e.end)
        if key in seen:
            raise RuntimeError(f"duplicate edit at {key}: {e}")
        seen.add(key)

    return result


def bytes_slice(cursor: ci.Cursor) -> str:
    """Return the source text spanned by a cursor's extent."""
    ext = cursor.extent
    if ext.start.file is None:
        return ""
    path = str(ext.start.file.name)
    with open(path, "rb") as fh:
        fh.seek(ext.start.offset)
        return fh.read(ext.end.offset - ext.start.offset).decode("utf-8", "replace")


def prologue_for(redirects: Dict[str, GlobalRedirect]) -> str:
    """Generate the prologue that the rewritten TU prepends.

    Declares __psx_<name> uint32_t externs (linker --defsym aliases them
    to g_ram + offset) and #defines the original name to a sentinel so
    any unrewritten reference fails to compile cleanly later if it slips
    in via a header re-include.
    """
    lines = [
        "/* generated by tools/rewrite — bare-globals redirects */",
        "#include <stdint.h>",
        "extern void* openpete_psx_to_host_ptr(uint32_t);",
        "extern uint32_t openpete_host_to_psx(const void*);",
        ("extern uint32_t openpete_psx_call_fnptr(uint32_t,"
         " uint32_t, uint32_t, uint32_t, uint32_t);"),
    ]
    for r in sorted(redirects.values(), key=lambda x: x.name):
        if r.is_array:
            lines.append(
                f"extern uint32_t {r.psx_storage_name}[{r.array_count}];")
        else:
            lines.append(f"extern uint32_t {r.psx_storage_name};")
        lines.append(f"#define {r.name} __opete_unused_{r.name}")
    lines.append("")
    return "\n".join(lines)
