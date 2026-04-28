"""Pass 6 — auto-manifest discovery.

Walks the libclang AST to enumerate, per native TU:

  - [[dispatch]] candidates — functions defined in this TU whose name is in
    symbols.toml. The hand-curated ``native.toml`` may carry extras (e.g.
    ``psyq_main = true`` for ``main``) that auto-discovery doesn't generate;
    the merge step in ``regen-manifest.sh`` treats hand entries as overrides.

  - [[outbound_recomp]] candidates — every called function whose definition
    is not in this TU and not in any other native TU within the domain, but
    whose name *is* in ``symbols.toml`` (i.e. it's recompiled MIPS).

  - [[outbound_psyq]] candidates — every called function whose declaring
    header lives under ``third_party/PsyCross/include/`` (the host-side PSYQ
    surface).

Emits a per-TU TOML at ``${SPYRO_GENERATED}/rewritten/<stem>.discovered.toml``
that downstream ``gen_*.py`` tools merge with ``native.toml``. The TOML is
*advisory*: hand-curated entries always win on conflict.

This pass is the project goal of §1.4 made concrete — the manifest treadmill
shrinks from "hand-list every callee" to "hand-list the policy bits", with
the address book emerging from the AST itself.
"""

from __future__ import annotations

from dataclasses import dataclass, field as dc_field
from pathlib import Path
from typing import Dict, List, Optional, Set

import clang.cindex as ci


def _arg_token(ty: ci.Type, idx: int) -> str:
    """libclang Type → dispatch marshalling token (u32 / u8 / ptr / stack_u32)."""
    canon = ty.get_canonical()
    kind = canon.kind
    if kind in (ci.TypeKind.POINTER, ci.TypeKind.INCOMPLETEARRAY,
                ci.TypeKind.CONSTANTARRAY, ci.TypeKind.VARIABLEARRAY):
        tok = "ptr"
    elif kind in (ci.TypeKind.UCHAR, ci.TypeKind.SCHAR, ci.TypeKind.CHAR_S,
                  ci.TypeKind.CHAR_U):
        tok = "u8"
    else:
        tok = "u32"
    if idx < 4:
        return tok
    offset = 16 + (idx - 4) * 4
    return f"stack_u32:{offset}"


def _derive_dispatch_args(fn_decl: ci.Cursor) -> list[str]:
    """Pull the parameter list from a function-definition cursor and
    convert each to a dispatch marshalling token. Empty list for a
    `void`-arg function."""
    args: list[str] = []
    for idx, p in enumerate(fn_decl.get_arguments()):
        args.append(_arg_token(p.type, idx))
    return args


def _is_psyq_header(path: str) -> bool:
    """Header declares a PSYQ-surface API.

    Matches both:
      - the matching-decomp's ``spyro-1/psyq/include/`` tree (where C TUs
        actually pull the PSYQ API names from when libclang parses with
        mipsel-elf target);
      - PsyCross's host include tree under ``third_party/PsyCross/include/``
        (used when the rewriter's parse finds PsyCross's copy first).
    """
    p = str(path)
    return ("/PsyCross/include/" in p
            or "/spyro-1/psyq/include/" in p)


def _is_game_source(path: str, native_files: Set[str]) -> bool:
    """Source lives in the matching-decomp tree (spyro-1/src/)."""
    p = str(path)
    return "/spyro-1/src/" in p


@dataclass
class DiscoverResult:
    # Functions defined in this TU's main source file.
    # (name, psx_addr, args_tokens)
    dispatch: List[tuple] = dc_field(default_factory=list)
    # Function names called from this TU and resolved to symbols.toml.
    outbound_recomp: List[str] = dc_field(default_factory=list)
    # Function names called from this TU declared by PsyCross headers.
    outbound_psyq: List[str] = dc_field(default_factory=list)
    # Names that aren't classified — diagnostic.
    unresolved: List[str] = dc_field(default_factory=list)


def run(tu_cursor: ci.Cursor,
        main_path: str,
        symbols: Dict[str, int],
        native_function_names: Set[str]) -> DiscoverResult:
    """Walk the AST. ``symbols`` maps function name → PSX vaddr (parsed from
    symbols.toml). ``native_function_names`` is the union of function names
    defined by all TUs in this domain's native-source set, used to filter
    same-domain native→native calls out of the recomp list.
    """
    result = DiscoverResult()

    own = str(Path(main_path).resolve())
    seen_recomp: Set[str] = set()
    seen_psyq: Set[str] = set()
    seen_dispatch: Set[str] = set()

    for c in tu_cursor.walk_preorder():
        # --- Function definitions in this TU → dispatch candidates --------
        if c.kind == ci.CursorKind.FUNCTION_DECL and c.is_definition():
            if c.location.file is None:
                continue
            if str(Path(c.location.file.name).resolve()) != own:
                continue
            name = c.spelling
            if name in seen_dispatch:
                continue
            seen_dispatch.add(name)
            addr = symbols.get(name)
            if addr is not None:
                args = _derive_dispatch_args(c)
                result.dispatch.append((name, addr, args))

        # --- Call sites → outbound classification -------------------------
        if c.kind == ci.CursorKind.CALL_EXPR:
            callee = c.referenced
            if callee is None or callee.kind != ci.CursorKind.FUNCTION_DECL:
                continue
            name = callee.spelling
            if not name or name.startswith("__"):
                continue  # builtins, intrinsics

            # Where is the callee declared?
            decl_path = None
            if callee.location.file is not None:
                decl_path = str(callee.location.file.name)

            # Is it defined here in this TU? — local helper, skip.
            defn = callee.get_definition()
            if defn is not None and defn.location.file is not None:
                defn_path = str(Path(defn.location.file.name).resolve())
                if defn_path == own:
                    continue

            # Is it defined in another native TU? — skip (direct native call).
            if name in native_function_names and name not in seen_dispatch:
                # name is provided by another native TU in this domain
                continue

            # PsyCross-declared → outbound_psyq.
            if decl_path and _is_psyq_header(decl_path):
                if name not in seen_psyq:
                    seen_psyq.add(name)
                    result.outbound_psyq.append(name)
                continue

            # In symbols.toml → outbound_recomp.
            if name in symbols:
                if name not in seen_recomp:
                    seen_recomp.add(name)
                    result.outbound_recomp.append(name)
                continue

            # Otherwise — host helper (libc, openpete_*, etc.) — no entry.
            if name not in seen_recomp and name not in seen_psyq:
                # Track unclassified names that *look* game-relevant (skip
                # libc/compiler builtins by prefix).
                if not (name.startswith("openpete_") or name.startswith("psyq_")
                        or name.startswith("spyro_native_")):
                    result.unresolved.append(name)

    result.outbound_recomp.sort()
    result.outbound_psyq.sort()
    result.dispatch.sort()
    return result


def render_toml(stem: str, result: DiscoverResult) -> str:
    """Emit a TOML fragment for this TU. Lives at
    ``${REWRITTEN}/<stem>.discovered.toml``; the merge step concatenates these
    with the hand-curated ``native.toml``."""
    out: List[str] = [
        f"# Auto-generated by tools/rewrite/pass_discover.py for {stem}.c.",
        "# Hand entries in native.toml override these on conflict.",
        "",
    ]
    for name, addr, args in result.dispatch:
        out.append("[[discovered_dispatch]]")
        out.append(f'name = "{name}"')
        out.append(f"addr = 0x{addr:08X}")
        rendered_args = ", ".join(f'"{a}"' for a in args)
        out.append(f"args = [{rendered_args}]")
        out.append("")
    for name in result.outbound_recomp:
        out.append("[[discovered_outbound_recomp]]")
        out.append(f'name = "{name}"')
        out.append("")
    for name in result.outbound_psyq:
        out.append("[[discovered_outbound_psyq]]")
        out.append(f'psyq_name = "{name}"')
        out.append("")
    if result.unresolved:
        out.append("# Callees that don't fit any bucket — review:")
        for name in sorted(set(result.unresolved)):
            out.append(f"#   {name}")
        out.append("")
    return "\n".join(out)
