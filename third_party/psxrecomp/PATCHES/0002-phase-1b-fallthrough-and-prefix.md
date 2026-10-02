# 0002 — Fallthrough and symbol-prefix fixes

These changes are applied directly on top of the vendored psxrecomp source
(see `ORIGIN.txt` for the upstream commit). They are documented here so a
future re-vendoring or upstream pull request can pick them up.

## Summary

1. **Fallthrough emission consults the external/manual maps.** When a
   non-PSYQ function ends without explicit control flow but the next
   address belongs to a PSYQ symbol (e.g. `func_800623D0` falls into
   `MulMatrix0` at `0x800623D8`), the emitted tail call must use the
   *external* name (`MulMatrix0`), not the address-based stub
   (`func_800623D8`). Previously the fallthrough lookup only consulted
   the in-translation-unit `Function` list, so PSYQ-targeted fallthroughs
   produced undefined references at link time. Fix: seed
   `func_name_by_addr` with `external_symbols_` and `manual_impls_` after
   it's built from `functions`.

2. **`--external-prefix=<str>` / `--manual-prefix=<str>`.** PSYQ ships
   stdlib equivalents (`memcpy`, `printf`, `sprintf`, `exit`, `rand`,
   `srand`, `strlen`, `setjmp`, `longjmp`, ...) that collide with libc
   when the C output is linked against the host. Without renaming, the
   linker resolves PSYQ calls to libc and any libc call inside the slim
   runtime resolves to PSYQ stubs — both wrong. The new flags prepend a
   prefix (e.g. `psyq_`) to every PSYQ / manual-impl symbol name at
   manifest-load time, so downstream codegen, dispatch table emission,
   and stub generation all use the prefixed names automatically.

## Files changed

- `recompiler/src/main_psx.cpp` — adds `--external-prefix` / `--manual-prefix`
  CLI flags; applies the prefix to manifest names before
  `set_external_symbols` / `set_manual_impls`.
- `recompiler/src/code_generator.cpp` — `generate_all_functions` seeds
  `func_name_by_addr` with `external_symbols_` and `manual_impls_` so
  fallthrough emission uses the correct external name.

## Companion tooling

- `tools/gen_psyq_stubs.py` (in `openpete/`) accepts `--prefix STR` so the
  generated PSYQ-stub file matches the recompiler's prefix output.

## Invocation example

```
PSXRecomp build/PSX.EXE \
    --output-dir=games/spyro-1/generated \
    --external-symbols=games/spyro-1/manifest/psyq.toml \
    --external-prefix=psyq_

python3 tools/gen_psyq_stubs.py --prefix psyq_ \
    games/spyro-1/manifest/psyq.toml \
    games/spyro-1/generated/psyq_stubs.c
```
