# 0003 — Block-label trailing `;` for MinGW-GCC 10

These changes are applied directly on top of the vendored psxrecomp source
(see `ORIGIN.txt` for the upstream commit). They are documented here so a
future re-vendoring or upstream pull request can pick them up.

## Summary

`code_generator.cpp` emits a basic-block label as

```c
block_<addr>:
```

immediately followed (in the common case where the block needs to save
its branch condition before a delay slot) by a declaration:

```c
block_8006B178:
    int _bc_8006B178 = (cpu->v0 == cpu->zero);  /* save branch cond */
```

Pre-C23, a label can only label a *statement*, not a declaration. GCC 11+
treats the label-then-decl construct as a `[-Wpedantic]` warning and
accepts it. **MinGW-GCC 10** (the version Ubuntu 22.04 ships in
`gcc-mingw-w64-x86-64-posix`, the toolchain the `windows-mingw` preset
uses) rejects it as a hard error:

```
error: a label can only be part of a statement and a declaration is not a statement
```

The mid-block branch-target emitter further down the same function
(`block_{:08X}:;\n`, line ~872) already does the right thing: it emits
the label with a trailing `;` (a null statement), which legalises any
declaration that follows. The basic-block label emitter at line ~830
just predates that fix.

## Change

```cpp
// recompiler/src/code_generator.cpp
- ss << fmt::format("block_{:08X}:\n", block.start_addr);
+ ss << fmt::format("block_{:08X}:;\n", block.start_addr);
```

The trailing `;` is invisible at runtime (null statement, no codegen),
makes the label legal C in all modern standards regardless of what
follows, and matches the convention the mid-block emitter already uses.

## One-shot fixup of existing generated C

The fix only takes effect on the *next* recompiler regen. Existing
generated files (e.g. `games/spyro-1/generated/PSX.EXE_full.c`) needed an
in-place rewrite:

```bash
sed -i -E 's/^(block_[0-9A-Fa-f]+):$/\1:;/' games/spyro-1/generated/*_full.c
```

Touched ~14k label lines per game EXE; build was reproducible after.

## Files changed

- `recompiler/src/code_generator.cpp` — one-character emit change at the
  basic-block label site.

## Upstream consideration

Strict improvement, no behavioural change under any compiler. Trivial
upstream PR candidate.
