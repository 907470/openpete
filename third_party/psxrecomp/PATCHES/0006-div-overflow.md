# 0006 — signed `div` of 0x80000000 by -1

**Files:** `recompiler/src/code_generator.cpp` (`translate_div`,
`translate_divu` comment)

Upstream: mstan/psxrecomp guards the same case in
`recompiler/src/strict_translator.cpp` (lines 343-377 at 76c56124f). The
vendored commit shares no merge base with that history (see `ORIGIN.txt`),
so the change is applied here in tree.

## Defect

`translate_div` emitted `(int32_t)rs / (int32_t)rt` and
`(int32_t)rs % (int32_t)rt` behind an `rt != 0` test. For
rs = 0x80000000, rt = 0xFFFFFFFF the quotient 2^31 does not fit in
`int32_t`: the C behaviour is undefined, and x86 `idiv` raises #DE, which
the host delivers as SIGFPE. The R3000 does not trap; it writes
LO = 0x80000000, HI = 0.

## Change

The emitted statement tests that pair first:

```c
if (rt == 0xFFFFFFFFu && rs == 0x80000000u) { cpu->lo = 0x80000000u; cpu->hi = 0; }
else if (rt != 0) { /* C / and %, unchanged */ }
else { cpu->lo = ((int32_t)rs >= 0) ? 0xFFFFFFFF : 1; cpu->hi = (uint32_t)rs; }
```

The division-by-zero arm was already R3000-exact (LO = -1 for rs >= 0,
+1 for rs < 0; HI = rs) and is unchanged. `divu` cannot overflow and its
zero arm (LO = 0xFFFFFFFF, HI = rs) was already exact; only its comment
changed, so no `divu` site in the generated C changes.

## Effect on generated C

Every `div` site gains the leading test; no other line changes. Regenerate
with `scripts/regen-recomp.sh` per game.

## Check

`tests/unit/div_semantics_test.c` runs the first `div` and `divu` site of
each game's generated `PSX.EXE_full.c` on INT32_MIN/-1, INT32_MIN/1, 7/-2,
-7/2 and x/0.
