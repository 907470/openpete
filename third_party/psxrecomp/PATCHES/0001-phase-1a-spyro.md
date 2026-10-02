# 0001 — Spyro adaptations

These changes are applied directly on top of the vendored psxrecomp source
(see `ORIGIN.txt` for the upstream commit). They are documented here so a
future re-vendoring or upstream pull request can pick them up.

## Summary

1. **Game-agnostic forced entries** — replace the hard-coded Tomba entry
   `analyzer.add_forced_entry(0x8006B58Cu)` with a CLI-driven, repeatable
   `--forced-entry=<addr>` flag. No game addresses live in the recompiler
   source any more.
2. **`jr` codegen for non-`$ra`** — `code_generator.cpp` `case 0x08` formerly
   emitted `return;` unconditionally. Now: if `rs == $ra` emit `return;`,
   otherwise emit `call_by_address(cpu, <rs>); return;`. Required for the
   handwritten asm functions that compile `switch` statements to `jr` jump
   tables.
3. **PSYQ symbol stubbing (`--external-symbols=<toml>`)** — addresses listed
   in the manifest are NOT translated. JAL targets and dispatch entries
   pointing at them are emitted as `extern void <name>(CPUState* cpu);`
   calls. PsyCross supplies the bodies at link time. See
   `tools/gen_psyq_toml.py` for the manifest generator.
4. **Manual implementation (`--manual-impl=<toml>`)** — same wiring as
   `--external-symbols`, but for hand-written replacements of non-matching
   asm functions. Same TOML schema (`[[symbols]]` blocks with `name` and
   `addr`).
5. **`--skip-on-error`** — wraps each function's codegen in `try`. On
   failure, emits a stub that logs the address and `abort()`s when called,
   then continues. Lets a partial output build while individual
   codegen edge cases are open.
6. **`--output-dir=<dir>`** — writes `<stem>_full.c`, `<stem>_dispatch.c`,
   and `discovered_functions.log` into `<dir>`. Defaults to `generated/`
   for backwards compat.

## Files changed

- `recompiler/include/code_generator.h` — new public API for externals,
  manual impls, skip-on-error.
- `recompiler/src/code_generator.cpp` — JAL/`j`/jr emit paths consult the
  external/manual maps; `generate_all_functions` skips externals/manuals
  and applies `--skip-on-error`; `generate_file` emits extern decls.
- `recompiler/src/main_psx.cpp` — restructured CLI parsing, removed Tomba
  forced entry, loads TOML manifests, writes outputs to `--output-dir`,
  emits `discovered_functions.log`, dispatch table includes externals and
  manuals.

## Manifest schema (informal)

```toml
range_start = 0x8005b8e0    # informational only — not used at codegen time
range_end   = 0x8006bbe0

[[symbols]]
name = "SpuInit"
addr = 0x8005ba60

[[symbols]]
name = "func_8005BB78"
addr = 0x8005bb78
```

The TOML is parsed by a hand-rolled line scanner in `main_psx.cpp` (no
toml11 dependency added). `[[manual]]` is accepted as a synonym for
`[[symbols]]` so the same parser handles both manifest types.
