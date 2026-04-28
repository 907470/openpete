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

from tools.rewrite.parser import parse_tu  # noqa: E402
from tools.rewrite.pass_bare_globals import (  # noqa: E402
    collect_redirects, rewrite_bare_globals, prologue_for,
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

    # --- Pass 2: bare-global redirects ------------------------------------
    redirects = collect_redirects(parsed.cursor)
    bg = rewrite_bare_globals(parsed.cursor, redirects, main_path)
    for err in bg.address_of_errors:
        print(f"  WARN: {err}", file=sys.stderr)

    plan = EditPlan()
    plan.add_many(bg.edits)
    rewritten = plan.apply(args.input)

    prologue = prologue_for(redirects)
    final = prologue + "\n" + rewritten

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(final)

    if args.redirects_out is not None:
        # One `<__psx_name> <orig_name>` per line. gen_defsyms.py joins
        # this with globals.toml to emit `--defsym=__psx_<name>=g_ram+off`.
        lines = [f"{r.psx_storage_name} {r.name}"
                 for r in sorted(redirects.values(), key=lambda x: x.name)]
        args.redirects_out.parent.mkdir(parents=True, exist_ok=True)
        args.redirects_out.write_text("\n".join(lines) + ("\n" if lines else ""))

    print(f"  rewrote {args.input.name}: "
          f"{len(redirects)} bare-globals, {len(bg.edits)} edits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
