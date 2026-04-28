#!/usr/bin/env python3
"""rewrite_tu.py — drive all rewrite passes over a single matching-decomp TU.

Phase 2 scope: only Pass 2 (bare-global redirects) runs. Pass 1 (struct
relayout), 3-5 etc. land in later phases.

Usage:
    rewrite_tu.py <input.c> <output.c> --domain=<NAME>
                  [--auto-manifest=<auto.toml>]
                  [--defsym-out=<file.cmake>]

Side outputs:
    <output.c>            rewritten source (compiled by host CMake)
    <defsym>.cmake        SPYRO_REWRITER_DEFSYMS_<DOMAIN> list, appended
                          to the linker via target_link_options
    <auto-manifest>       (Phase 3+) auto-discovered manifest entries
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Ensure tools/ is on sys.path when invoked as a script.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.rewrite.parser import parse_tu, SPYRO_REPO  # noqa: E402
from tools.rewrite.pass_bare_globals import (  # noqa: E402
    collect_redirects, rewrite_bare_globals, prologue_for,
)
from tools.rewrite.pass_struct_relayout import run as run_struct_relayout  # noqa: E402
from tools.rewrite.pass_casts import run as run_casts  # noqa: E402
from tools.rewrite.pass_sizeof import (  # noqa: E402
    run as run_sizeof, render_static_asserts,
)
from tools.rewrite.pass_psyq_boundary import (  # noqa: E402
    run as run_psyq_boundary, format_violations,
)
from tools.rewrite.pass_include_asm import run as run_include_asm  # noqa: E402
from tools.rewrite.pass_discover import (  # noqa: E402
    run as run_discover, render_toml as render_discovered_toml,
)
from tools.rewrite.rewriter import EditPlan  # noqa: E402


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--domain", default="main_exe")
    ap.add_argument("--redirects-out", type=Path, default=None,
                    help="Write the list of bare-global redirects this TU "
                         "introduced (one `<__psx_name> <orig_name>` per line). "
                         "gen_defsyms.py merges these to emit __psx_<name> "
                         "linker aliases pointing into g_ram.")
    ap.add_argument("--symbols-toml", type=Path, default=None,
                    help="manifest/symbols.toml — used to resolve function "
                         "names to PSX vaddrs when rewriting struct field "
                         "writes whose RHS is a function reference.")
    ap.add_argument("--shadow-include-dir", type=Path, default=None,
                    help="Per-TU shadow include tree. Pass 1 writes rewritten "
                         "spyro-1/include/<file>.h copies here so the host "
                         "compiler sees PSX-laid struct definitions. Compile "
                         "invocation must prepend this directory to its -I list.")
    ap.add_argument("--discovered-toml-out", type=Path, default=None,
                    help="Write Pass 6's auto-discovered manifest entries "
                         "([[discovered_dispatch]] / [[discovered_outbound_recomp]] "
                         "/ [[discovered_outbound_psyq]]) for this TU. The merge "
                         "step in regen-manifest.sh combines these with hand-"
                         "curated native.toml entries.")
    ap.add_argument("--native-fn-names", type=Path, default=None,
                    help="File listing function names defined by *other* native "
                         "TUs in this domain (one per line). Pass 6 uses this "
                         "to suppress same-domain native→native calls from the "
                         "outbound_recomp set.")
    args = ap.parse_args(argv)

    parsed = parse_tu(args.input)
    own_errors = [d for d in parsed.diagnostics(False)
                  if d.severity >= 3]  # Diagnostic.Error == 3
    if own_errors:
        for d in own_errors:
            print(f"  parse error: {d.location.file}:{d.location.line}: {d.spelling}",
                  file=sys.stderr)
        return 2

    main_path = str(args.input.resolve())

    plan = EditPlan()

    # Optional: load symbols.toml for function-vaddr lookups.
    symbols = None
    if args.symbols_toml is not None and args.symbols_toml.exists():
        import tomli
        with open(args.symbols_toml, "rb") as fh:
            sym_data = tomli.load(fh)
        symbols = {f["name"]: f["addr"] for f in sym_data.get("function", [])}

    # --- Pass 1: struct relayout ------------------------------------------
    # Header field rewrites + .c-body access conversions. Header edits
    # accumulate per-header in `plan`; we splice each touched header into
    # the shadow include tree below.
    sr = run_struct_relayout(parsed.cursor, main_path, symbols)
    plan.add_many(sr.field_edits)
    plan.add_many(sr.access_edits)
    for w in sr.fnptr_warnings:
        print(f"  WARN: {w}", file=sys.stderr)
    for w in sr.address_of_warnings:
        print(f"  WARN: {w}", file=sys.stderr)

    # --- Pass 4: pointer-to-integer cast classification -------------------
    # Runs ahead of Pass 2 (single AST walk; logical pre-stage). Produces
    # the offset set Pass 2 consults to pick the bare-vaddr read form for
    # sites inside `(IntT)<global>`.
    redirects = collect_redirects(parsed.cursor)
    casts = run_casts(parsed.cursor, main_path, redirects)

    # --- Pass 2: bare-global redirects ------------------------------------
    bg = rewrite_bare_globals(parsed.cursor, redirects, main_path,
                              vaddr_form_offsets=casts.vaddr_form_offsets)
    for err in bg.address_of_errors:
        print(f"  WARN: {err}", file=sys.stderr)
    plan.add_many(bg.edits)

    # --- Pass 5b: PsyCross u_long* boundary guard -------------------------
    # Hard-error on any call to a non-audited u_long*-taking PsyCross API.
    # Per docs/psycross-boundary.md the current TU set is no-op (CdRead /
    # LoadImage internally byte-walk); the guard catches future migrations.
    pb = run_psyq_boundary(parsed.cursor, main_path)
    if pb.violations:
        print(format_violations(pb), file=sys.stderr)
        return 3

    # --- Pass 5c: INCLUDE_ASM splice removal ------------------------------
    # Textual scan over the original source — INCLUDE_ASM expands to inline
    # asm that won't link on host; the recompiled body in PSX.EXE_full.c /
    # <overlay>.bin_full.c provides the actual implementation, and Pass 6
    # discovery + outbound thunks route any in-TU call sites correctly.
    ia = run_include_asm(args.input)
    plan.add_many(ia.edits)

    # --- Pass 3: sizeof verification --------------------------------------
    # Compile-time guard: for every sizeof() of a relaid struct in this TU,
    # static_assert that the host size still matches the PSX size after
    # Pass 1's relayout. No-op today (Pass 1 makes this true); catches
    # future drift instantly.
    sz = run_sizeof(parsed.cursor, main_path)

    # --- Apply edits to .c body -------------------------------------------
    rewritten = plan.apply(args.input)
    prologue = prologue_for(redirects)
    sizeof_block = render_static_asserts(sz)
    final = prologue + "\n" + sizeof_block + "\n" + rewritten

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(final)

    # --- Apply edits to touched headers; write into the shadow tree -------
    if args.shadow_include_dir is not None and sr.rewritten_headers:
        spyro_include = (SPYRO_REPO / "include").resolve()
        for header_path in sr.rewritten_headers:
            header_resolved = Path(header_path).resolve()
            try:
                rel = header_resolved.relative_to(spyro_include)
            except ValueError:
                # Outside spyro-1/include — skip (PSYQ headers etc.)
                continue
            dst = args.shadow_include_dir / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(plan.apply(header_resolved))

    # --- Pass 6: auto-manifest discovery ----------------------------------
    if args.discovered_toml_out is not None:
        native_fn_names: set[str] = set()
        if args.native_fn_names is not None and args.native_fn_names.exists():
            for line in args.native_fn_names.read_text().splitlines():
                line = line.strip()
                if line and not line.startswith("#"):
                    native_fn_names.add(line)
        sym_map = symbols or {}
        disc = run_discover(parsed.cursor, main_path, sym_map, native_fn_names)
        stem = args.input.stem
        args.discovered_toml_out.parent.mkdir(parents=True, exist_ok=True)
        args.discovered_toml_out.write_text(render_discovered_toml(stem, disc))

    if args.redirects_out is not None:
        # One `<__psx_name> <orig_name>` per line. gen_defsyms.py joins
        # this with globals.toml to emit `--defsym=__psx_<name>=g_ram+off`.
        lines = [f"{r.psx_storage_name} {r.name}"
                 for r in sorted(redirects.values(), key=lambda x: x.name)]
        args.redirects_out.parent.mkdir(parents=True, exist_ok=True)
        args.redirects_out.write_text("\n".join(lines) + ("\n" if lines else ""))

    print(f"  rewrote {args.input.name}: "
          f"{len(redirects)} bare-globals, {len(bg.edits)} edits, "
          f"{len(sz.checks)} sizeof asserts, "
          f"{pb.safe_calls} safe u_long* calls, "
          f"{len(casts.redirected)} ptr→int casts, "
          f"{len(ia.splices)} INCLUDE_ASM splices")
    return 0


if __name__ == "__main__":
    sys.exit(main())
