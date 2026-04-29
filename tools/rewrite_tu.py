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
from tools.rewrite.pass_casts import run as run_casts, apply_cast_wraps  # noqa: E402
from tools.rewrite.pass_fnptr_calls import (  # noqa: E402
    rewrite_fnptr_global_calls, rewrite_fnptr_global_writes,
)
from tools.rewrite.pass_sizeof import (  # noqa: E402
    run as run_sizeof, render_static_asserts,
)
from tools.rewrite.pass_psyq_boundary import (  # noqa: E402
    run as run_psyq_boundary, format_violations,
)
from tools.rewrite.pass_include_asm import run as run_include_asm  # noqa: E402
from tools.rewrite.pass_struct_by_value import (  # noqa: E402
    run as run_struct_by_value, format_violations as format_sbv_violations,
)
from tools.rewrite.pass_discover import (  # noqa: E402
    run as run_discover, render_toml as render_discovered_toml,
)
from tools.rewrite.pass_outbound_lint import (  # noqa: E402
    run as run_outbound_lint, format_warnings as format_outbound_lint,
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
    ap.add_argument("--native-toml", type=Path, default=None,
                    help="manifest/native.toml — used by Pass 5e (host-pointer-"
                         "to-thunk lint) to enumerate the set of outbound thunks "
                         "to inspect call sites of.")
    ap.add_argument("--globals-toml", type=Path, default=None,
                    help="manifest/globals.toml — used by Pass 5e to recognize "
                         "address-of game globals as safe (--defsym-aliased into "
                         "g_ram).")
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

    # --- Pass 4: pointer-to-integer cast classification -------------------
    # Runs ahead of Pass 1 because Pass 1's access-text rendering folds
    # the redirected-global form inline; both Passes 1 and 2 consult
    # `redirects` so each ref is rewritten exactly once.
    redirects = collect_redirects(parsed.cursor)
    casts = run_casts(parsed.cursor, main_path, redirects)

    # --- Precompute Pass 2 write extents ----------------------------------
    # Pass 1 needs to know which spans Pass 2 will rewrite as writes so
    # it can suppress its own reads inside them (Pass 2's RHS rendering
    # folds the relaid field reads inline).
    from tools.rewrite.pass_bare_globals import (
        _find_assignment_writes, find_array_write_extents)
    pre_writes = _find_assignment_writes(parsed.cursor, redirects)
    pass2_write_ranges = [(binop.extent.start.offset, binop.extent.end.offset)
                          for (binop, _l, _r) in pre_writes.values()]
    pass2_write_ranges.extend(
        find_array_write_extents(parsed.cursor, redirects))

    # --- Pass 1: struct relayout ------------------------------------------
    # Header field rewrites + .c-body access conversions. Header edits
    # accumulate per-header in `plan`; we splice each touched header into
    # the shadow include tree below. Pass 1 receives the redirects map +
    # Pass 2's write extents so it can fold redirected-global references
    # inline and skip nested refs inside Pass 2 writes.
    sr = run_struct_relayout(parsed.cursor, main_path, symbols,
                             redirects=redirects,
                             pass2_write_ranges=pass2_write_ranges)
    # Now that Pass 1's rewrites map exists, run the cast-wrap stage of
    # Pass 4 so its renderer can fold relaid-field reads inline.
    apply_cast_wraps(parsed.cursor, main_path, redirects, casts, sr.rewrites)
    plan.add_many(sr.field_edits)
    # Pass 4b folds Pass 1 reads inline within its wrap text; suppress
    # Pass 1 access edits whose range sits strictly inside a Pass 4b
    # range to avoid overlapping the wrap.
    pass4b_ranges_for_pass1 = sorted(
        [(ed.start, ed.end) for ed in casts.edits
         if ed.note == "int->ptr cast wrap" and ed.file == main_path])
    def _inside_p4b(s: int, e: int) -> bool:
        for rs, re_ in pass4b_ranges_for_pass1:
            if s >= rs and e <= re_ and (s > rs or e < re_):
                return True
            if rs > s:
                break
        return False
    for ed in sr.access_edits:
        if ed.file == main_path and _inside_p4b(ed.start, ed.end):
            continue
        plan.add(ed)
    for w in sr.fnptr_warnings:
        print(f"  WARN: {w}", file=sys.stderr)
    for w in sr.address_of_warnings:
        print(f"  WARN: {w}", file=sys.stderr)

    # --- Pass 2: bare-global redirects ------------------------------------
    # Pass 2 receives Pass 1's access edits as covered ranges and Pass 1's
    # `rewrites` map so its write rhs renders any nested relaid field
    # accesses inline. With both passes folded the splicer never sees an
    # overlap on this composition pattern.
    pass1_ranges = [(e.start, e.end) for e in sr.access_edits
                    if e.file == main_path]
    bg = rewrite_bare_globals(parsed.cursor, redirects, main_path,
                              vaddr_form_offsets=casts.vaddr_form_offsets,
                              vaddr_form_array_sub_extents=casts.vaddr_form_array_sub_extents,
                              pass1_covered_ranges=pass1_ranges,
                              pass1_rewrites=sr.rewrites)
    for err in bg.address_of_errors:
        print(f"  WARN: {err}", file=sys.stderr)
    plan.add_many(bg.edits)

    # --- Pass 4b/4c: cast wraps (literal PSX vaddr conversions) -----------
    # Skip casts that fall inside a Pass 1 or Pass 2 write extent — those
    # rewrites already consume the cast specially. Also skip Pass 4c
    # `ptr->int cast wrap` edits whose site is inside a Pass 4b
    # `int->ptr cast wrap` edit — Pass 4b folds the host_to_psx
    # substitution into its replacement text inline.
    # Pass 2 array-of-pointer subscripts and redirected-global reads also
    # cover their full extent; Pass 4c casts wrapping such an operand
    # would overlap Pass 2's edit.
    pass2_edit_ranges = [(e.start, e.end) for e in bg.edits
                         if e.file == main_path]
    suppress_ranges = sorted(pass1_ranges + pass2_write_ranges
                             + pass2_edit_ranges)
    pass4b_ranges = sorted([(ed.start, ed.end) for ed in casts.edits
                            if ed.note == "int->ptr cast wrap"
                            and ed.file == main_path])
    def _inside(s: int, e: int, ranges) -> bool:
        for rs, re_ in ranges:
            if s >= rs and e <= re_:
                return True
            if rs > s:
                break
        return False
    # Pass 4d fn-ptr call rewrites cover the whole call extent, including
    # arg subtrees that Pass 4b/4c may want to wrap; Pass 4d's _coerce_arg
    # already handles host_to_psx for pointer args, so skip cast wraps
    # inside fn-ptr-call ranges. Compute these now so the cast-edit filter
    # can consult them.
    fnptr_edits_precompute = rewrite_fnptr_global_calls(parsed.cursor, main_path)
    fnptr_call_pre = sorted([(e.start, e.end) for e in fnptr_edits_precompute])
    for ed in casts.edits:
        if ed.file != main_path:
            continue
        if _inside(ed.start, ed.end, suppress_ranges):
            continue
        if _inside(ed.start, ed.end, fnptr_call_pre):
            continue
        if ed.note == "ptr->int cast wrap" and \
                _inside(ed.start, ed.end, pass4b_ranges):
            continue
        plan.add(ed)

    # --- Pass 4d: fn-ptr global call dispatch -----------------------------
    # `(*FN_PTR_GLOBAL)(args...)` rewrites through openpete_psx_call_fnptr
    # so the call routes via the recompiler instead of jumping into the
    # raw 4-byte vaddr storage as if it were a host function pointer.
    for ed in fnptr_edits_precompute:
        if _inside(ed.start, ed.end, suppress_ranges):
            continue
        plan.add(ed)
    fnptr_write_edits = rewrite_fnptr_global_writes(
        parsed.cursor, main_path, symbols or {})
    for ed in fnptr_write_edits:
        if _inside(ed.start, ed.end, suppress_ranges):
            continue
        plan.add(ed)

    # --- Pass 5: struct-by-value boundary guard ---------------------------
    # Hard-error if the TU passes (or returns) a struct by value at a call
    # site — the host LP64/LLP64 calling convention can't marshal those
    # bytes through cpu->aN today. No rewrite path implemented; trips are
    # zero across spyro-1/src/. See plan §2.3 Pass 5.
    sbv = run_struct_by_value(parsed.cursor, main_path)
    if sbv.violations:
        print(format_sbv_violations(sbv), file=sys.stderr)
        return 4

    # --- Pass 5b: PsyCross u_long* boundary guard -------------------------
    # Hard-error on any call to a non-audited u_long*-taking PsyCross API.
    # Per docs/psycross-boundary.md the current TU set is no-op (CdRead /
    # LoadImage internally byte-walk); the guard catches future migrations.
    pb = run_psyq_boundary(parsed.cursor, main_path)
    if pb.violations:
        print(format_violations(pb), file=sys.stderr)
        return 3

    # --- Pass 5e: host-pointer-to-thunk lint ------------------------------
    # Warn on call sites where a native function passes a host-allocated
    # pointer (stack-local / host-global / host-heap) through an outbound
    # thunk. host_to_psx returns 0 for those and the callee dereferences
    # NULL — silent on platforms whose smoke gate doesn't reach the call,
    # loud on platforms where it does. Plan §"Pass 5e".
    thunked_fns: set[str] = set()
    game_global_names: set[str] = set()
    if args.native_toml is not None and args.native_toml.exists():
        import tomli
        with open(args.native_toml, "rb") as fh:
            nt = tomli.load(fh)
        for e in nt.get("outbound_recomp", []):
            thunked_fns.add(e["name"])
        for e in nt.get("outbound_psyq", []):
            thunked_fns.add(e["psyq_name"])
        # Skipped names link directly to host symbols — not thunked.
        for n in nt.get("outbound_psyq_skip", []):
            thunked_fns.discard(n)
        for n in nt.get("outbound_recomp_skip", []):
            thunked_fns.discard(n)
        # Hand-written openpete_<NAME> thunks accept host pointers
        # directly (no host_to_psx round-trip), so they're safe — not
        # subject to the lint.
        for e in nt.get("outbound_psyq_hand", []):
            thunked_fns.discard(e["psyq_name"])
    if args.globals_toml is not None and args.globals_toml.exists():
        import tomli
        with open(args.globals_toml, "rb") as fh:
            gt = tomli.load(fh)
        for g in gt.get("global", []):
            game_global_names.add(g["name"])
    ol = run_outbound_lint(parsed.cursor, main_path, thunked_fns,
                           game_global_names)
    if ol.warnings:
        print(format_outbound_lint(ol), file=sys.stderr)

    # --- Pass 5c: INCLUDE_ASM splice removal ------------------------------
    # Textual scan over the original source — INCLUDE_ASM expands to inline
    # asm that won't link on host; the recompiled body in PSX.EXE_full.c /
    # <overlay>.bin_full.c provides the actual implementation, and Pass 6
    # discovery + outbound thunks route any in-TU call sites correctly.
    ia = run_include_asm(args.input)
    plan.add_many(ia.edits)

    # Surface spliced names to gen_renames.py so it can exclude them from
    # the native func_* rename set — those functions have no native body
    # (we just spliced the only definition), so a rename to spyro_native_*
    # would dangle if anything calls them post-migration.
    if ia.splices:
        spliced_path = args.output.parent / f"{args.input.stem}.spliced.txt"
        spliced_path.write_text("\n".join(s.name for s in ia.splices) + "\n")

    # --- Pass 3: sizeof verification --------------------------------------
    # Compile-time guard: for every sizeof() of a relaid struct in this TU,
    # static_assert that the host size still matches the PSX size after
    # Pass 1's relayout. No-op today (Pass 1 makes this true); catches
    # future drift instantly.
    sz = run_sizeof(parsed.cursor, main_path)

    # --- Apply edits to .c body -------------------------------------------
    rewritten = plan.apply(args.input)
    prologue = prologue_for(redirects)
    # Pass 3's static_asserts go at the *end* of the rewritten file.
    # Up top, struct types are still forward-declared (the includes
    # come later in the source), so `sizeof(struct Foo)` would fail
    # with "incomplete type". After all includes + bodies, every
    # referenced struct is complete.
    sizeof_block = render_static_asserts(sz)
    final = prologue + "\n" + rewritten + "\n" + sizeof_block

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(final)

    # --- Apply edits to touched headers; write into the shadow tree -------
    # Mirror every spyro-1/include header into the shadow tree, applying
    # edits to the rewritten ones and copying un-rewritten ones verbatim.
    # GCC's quote-include "same directory of including file" rule wins
    # over -I — so an un-shadowed header (e.g. hud.h) including a shadowed
    # one (moby.h) would otherwise resolve back to spyro-1/include/moby.h
    # and miss Pass 1's relayout. Mirroring keeps every transitive include
    # inside the shadow tree.
    if args.shadow_include_dir is not None:
        spyro_include = (SPYRO_REPO / "include").resolve()
        rewritten_resolved = {Path(p).resolve() for p in sr.rewritten_headers}
        for src in spyro_include.rglob("*.h"):
            rel = src.relative_to(spyro_include)
            dst = args.shadow_include_dir / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if src in rewritten_resolved:
                dst.write_text(plan.apply(src))
            else:
                dst.write_bytes(src.read_bytes())

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
