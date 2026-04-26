# openpete ↔ PCSX-Redux RAM-diff workflow

A primitive for the iterative-debugging loop: run pcsx-redux to a chosen
PC, capture its 2 MB main RAM and CPU state; run openpete from the same
seed up to the same PC; byte-diff the two RAMs. Any diverging byte is a
candidate openpete recompiler / PSYQ-thunk bug.

Companion to [gte-trace-vs-reference](gte-trace-vs-reference.md): GTE
verifies math, this verifies *whole-game state* over an arbitrary span of
execution.

## Components

### openpete flags

| Flag | What |
|------|------|
| `--seed-ram PATH`       | Overwrite `g_ram` (2 MB) from PATH **after** EXE-load, so openpete starts from the same RAM pcsx-redux had at the seed PC. |
| `--seed-cpu PATH`       | Overwrite the 32 GPRs + pc/hi/lo + cop0 + GTE banks from PATH (528-byte `'CSv1'` blob). Suppresses any `--test-recomp-gp` override. |
| `--halt-at-pc HEX`      | When `psx_override_dispatch` sees the recomp call into the function at HEX, dump `g_ram` to `--ram-dump-output` and `_Exit(0)`. Hooked at function-call boundary, so the dump instant is exact. |
| `--ram-dump-on-frame N` | Dump after frame N's game code yields. Used for "post-vsync" snapshots when you don't want to halt; pairs with PCSXR_DUMP_VSYNCS on the Lua side. |
| `--ram-dump-output PATH`| Shared output path for both halt-at-pc and ram-dump-on-frame. |

### pcsx-redux Lua (`scripts/pcsx-redux-ram-dump.lua`)

Driven by env vars (no edit-and-rerun loop):

| Var | Default | What |
|-----|---------|------|
| `PCSXR_DUMP_PC`     | `0x8003385c` | Target PC. BP fires here. |
| `PCSXR_DUMP_OUT`    | `/tmp/pcsxr_ram.bin` | RAM (2 MB) output path. |
| `PCSXR_DUMP_CPU`    | *(unset)*    | Optional CPU output path (528 bytes, `'CSv1'` format). Omit to skip. |
| `PCSXR_DUMP_VSYNCS` | `0`          | When 0, dump fires immediately on BP hit. Larger values count vblank events after BP and dump on the Nth — useful when you want a "+N vsync after PC" capture. |

Requires pcsx-redux launched with `--debugger`; see the same caveat in
`gte-trace-vs-reference.md`.

### `'CSv1'` CPU-state file format

528 bytes, little-endian throughout:

```
offset  size  field
   0    4     magic 0x31765343 ('CSv1')
   4    128   gpr[32]   in MIPS order: zero, at, v0, v1, a0..a3,
                        t0..t7, s0..s7, t8, t9, k0, k1, gp, sp, fp, ra
 132    4     pc
 136    4     hi
 140    4     lo
 144    128   cop0[32]
 272    128   gte_data[32]
 400    128   gte_ctrl[32]
```

Producer: the Lua dumper (`dump_cpu` helper).
Consumer: openpete `load_cpu_seed` in `core/runtime/run.c`.

## Recipe

Two pcsxr captures, one openpete run, one diff.

```sh
PCSXR=openpete/.tools/pcsx-redux/pcsx-redux
SCRIPT=openpete/scripts/pcsx-redux-ram-dump.lua
DISC=spyro-1/build/SCUS-94228.cue
BIOS=openpete/.tools/pcsx-redux/openbios.bin

# 1. Seed snapshot — pcsxr at SEED_PC (pre-divergence point).
SEED_PC=0x80012204    # Spyro 1 main entry
PCSXR_DUMP_PC=$SEED_PC \
PCSXR_DUMP_OUT=/tmp/seed.ram \
PCSXR_DUMP_CPU=/tmp/seed.cpu \
    $PCSXR --no-ui --debugger --interpreter \
        --bios $BIOS --iso $DISC --run --dofile $SCRIPT

# 2. Oracle snapshot — pcsxr at ORACLE_PC (the point we want openpete to match).
ORACLE_PC=0x8003385c  # Spyro 1 GamestateUpdate (post-Initialize)
PCSXR_DUMP_PC=$ORACLE_PC \
PCSXR_DUMP_OUT=/tmp/oracle.ram \
PCSXR_DUMP_CPU=/tmp/oracle.cpu \
    $PCSXR --no-ui --debugger --interpreter \
        --bios $BIOS --iso $DISC --run --dofile $SCRIPT

# 3. openpete — seed both, halt at oracle, dump.
openpete/build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc $DISC \
    --run-recomp 0x80012204 \
    --seed-ram /tmp/seed.ram \
    --seed-cpu /tmp/seed.cpu \
    --halt-at-pc $ORACLE_PC \
    --press "" --headless \
    --ram-dump-output /tmp/op.ram

# 4. Diff.
cmp -l /tmp/op.ram /tmp/oracle.ram | wc -l         # how many bytes differ
cmp -l /tmp/op.ram /tmp/oracle.ram | head -20      # first divergences
```

To produce a region histogram and direction breakdown, awk over `cmp -l`:

```sh
cmp -l /tmp/op.ram /tmp/oracle.ram > /tmp/cmp.txt
awk '{
    off = $1 - 1
    if      (off < 0x010000) r = "kernel"
    else if (off < 0x076000) r = "EXE"
    else if (off < 0x080000) r = "post-EXE"
    else if (off < 0x1F0000) r = "heap"
    else                     r = "stack"
    counts[r]++
} END { for (r in counts) printf "  %6d  %s\n", counts[r], r | "sort -rn" }' \
    /tmp/cmp.txt
```

## Picking a halt PC

The PC you halt at must be a **recompiled function entry** — it's matched
by `psx_override_dispatch`, which fires at function-call boundaries, not
mid-instruction. Look up function symbols in `spyro-1/build/psx.map`:

```sh
grep -E "GamestateUpdate|GamestateDraw|Initialize" spyro-1/build/psx.map
```

For Spyro 1, useful candidates inside `Initialize()` (for bisection):
the first few functions main calls in order. Halt at each in sequence
and watch where the diff jumps from "small" to "large" — the first
"large" PC is the first diverging function.

## Bisection inside `Initialize()` — current baseline (2026-04-26)

With the recipe above and Spyro 1's `main → Initialize() → ...`, halting
both sides at function-entry boundaries via `--halt-at-pc`. **Halt PCs
must be game functions, not PSYQ thunks** — the dispatch chain handles
PSYQ as `case 0xADDR: psyq_FOO(cpu); return 1`, which bypasses
`psx_override_dispatch`, so a halt on a PSYQ PC will silently never fire.
Game functions (anything with a recompiled body in `PSX.EXE_full.c`) hit
the override gate and halt cleanly.

Sweep across `Initialize()`'s game-function call chain, with
`--seed-ram` + `--seed-cpu` from main entry:

| halt at | total diff | kernel | EXE | heap | stack |
|---------|-----------:|-------:|----:|-----:|------:|
| `0x80012204` main entry              | **0**   | 0   | 0   | 0 | 0   |
| `0x800122a8` GraphicsInitialize      | **0**   | 0   | 0   | 0 | 0   |
| `0x80012460` MemoryCardInitialize    | 381     | 69  | 176 | 0 | 136 |
| `0x800123c8` GamepadInitialize       | 773     | 451 | 191 | 0 | 131 |
| `0x8005595c` SpuInitialize           | 905     | 453 | 301 | 0 | 129 |
| `0x80012480` CDInitialize            | 931     | 447 | 310 | 0 | 152 |
| `0x8001256c` MusicInitialize         | 1,150   | 525 | 404 | 0 | 190 |
| `0x8003385c` GamestateUpdate         | 1.14 M  | 532 | 319,464 | 746,072 | 58,066 |

Direction of every cluster up through MusicInit: **pcsxr-only nonzero ≫
openpete-only**. That is, the bytes that differ are bytes pcsxr wrote
(via real PSYQ MIPS code) that openpete's C thunks didn't write. So the
remaining bug surface is "PSYQ thunks aren't byte-faithful with the real
PSYQ libraries pcsxr executes."

### Two thunks already faithful

- `psyq___main` (commit *this commit*). The 5 bytes that diverged at the
  GraphicsInitialize halt before this fix were: a "init done" sentinel
  byte at `0x800730BC`, plus 4 bytes of saved-`ra` in `__main`'s own
  stack frame at `0x801FFFD4`. The thunk now writes both, taking the
  GraphicsInit halt diff from 5 → 0.

### Big remaining offenders, in priority order

1. **`libgpu` family** — `psyq_ResetGraph`, `psyq_SetDefDrawEnv`,
   `psyq_SetDefDispEnv`, `psyq_PutDrawEnv`/`PutDispEnv`,
   `psyq_ClearImage`, `psyq_DrawSync`. Real PSYQ
   `ResetGraph()` writes a DRAWENV struct at `0x80074A64` and an OT/jtb
   table at `0x80074A1C` (boot log: `ResetGraph:jtb=80074a1c,env=80074a64`).
   Openpete's thunks delegate to PsyCross which manages host-side state
   without writing these PSX-side bytes. ~116 bytes / call.
2. **`libpad` / `MemCardInit`** — kernel-region writes (BIOS pad slot
   state at `0x800091BE..0x80009245`). Probably the BIOS-side kernel
   memory openpete has no equivalent of.
3. Stack divergence (~130 bytes/halt and growing): every PSYQ thunk
   skips its own MIPS prologue's `sw $ra/s0..` push, so saved-register
   slots in each thunk's would-be stack frame stay at zero.

### Heap explosion past `MusicInit`

The jump from 1,150 → 1.14M bytes between MusicInitialize and
GamestateUpdate is dominated by the rest of `Initialize()`:
`InitGeom`, `SetGeomOffset`/`Screen`, the RGB24 image-decompression loop
(`func_80017F24`, ~360 KB Memset writes), `WadInitialize` + `CDLoadSync`
(reads 2 KB of WAD header into `g_OverlaySpacePointer`), and lots of
PSYQ display traffic. Bisecting *that* requires intermediate halt PCs
inside the image loop or post-WAD; many of those would-be halt points
are PSYQ functions that won't fire on `--halt-at-pc`. A separate diff
loop using `--ram-dump-on-frame` may be more practical for that span.

So the harness is sound — register state, RAM seed, halt instant are all
verified working byte-for-byte at main entry, and the bisection has
revealed that the remaining bug surface is concentrated in PSYQ thunks.

## Files

- `scripts/pcsx-redux-ram-dump.lua` — capture script
- `core/runtime/run.c` — `load_cpu_seed`, `seed-ram` block
- `core/runtime/dispatch.c` — `openpete_halt_at_pc` hook in `psx_override_dispatch`
- `core/runtime/options.c` — flag parsing
- `core/include/openpete.h` — option fields
