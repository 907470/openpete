# openpete ↔ PCSX-Redux RAM-diff workflow

A primitive for the iterative-debugging loop: run pcsx-redux to a chosen
PC, capture its 2 MB main RAM and CPU state; run openpete from the same
seed up to the same PC; byte-diff the two RAMs. Any diverging byte is a
candidate openpete recompiler / PSYQ-thunk bug.

Companion to [gte-trace-vs-reference](gte-trace-vs-reference.md): GTE
verifies math, this verifies _whole-game state_ over an arbitrary span of
execution.

## Components

### openpete flags

| Flag                     | What                                                                                                                                                                                            |
| ------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--seed-ram PATH`        | Overwrite `g_ram` (2 MB) from PATH **after** EXE-load, so openpete starts from the same RAM pcsx-redux had at the seed PC.                                                                      |
| `--seed-cpu PATH`        | Overwrite the 32 GPRs + pc/hi/lo + cop0 + GTE banks from PATH (528-byte `'CSv1'` blob). Suppresses any `--test-recomp-gp` override.                                                             |
| `--halt-at-pc HEX`       | When `psx_override_dispatch` sees the recomp call into the function at HEX, dump `g_ram` to `--ram-dump-output` and `_Exit(0)`. Hooked at function-call boundary, so the dump instant is exact. |
| `--ram-dump-on-frame N`  | Dump after frame N's game code yields. Used for "post-vsync" snapshots when you don't want to halt; pairs with PCSXR_DUMP_VSYNCS on the Lua side.                                               |
| `--ram-dump-output PATH` | Shared output path for both halt-at-pc and ram-dump-on-frame.                                                                                                                                   |

### pcsx-redux Lua (`scripts/pcsx-redux-ram-dump.lua`)

Driven by env vars (no edit-and-rerun loop):

| Var                 | Default              | What                                                                                                                                                                 |
| ------------------- | -------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `PCSXR_DUMP_PC`     | `0x8003385c`         | Target PC. BP fires here.                                                                                                                                            |
| `PCSXR_DUMP_OUT`    | `/tmp/pcsxr_ram.bin` | RAM (2 MB) output path.                                                                                                                                              |
| `PCSXR_DUMP_CPU`    | _(unset)_            | Optional CPU output path (528 bytes, `'CSv1'` format). Omit to skip.                                                                                                 |
| `PCSXR_DUMP_VSYNCS` | `0`                  | When 0, dump fires immediately on BP hit. Larger values count vblank events after BP and dump on the Nth — useful when you want a "+N vsync after PC" capture. |

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
    --headless \
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
`--seed-ram` + `--seed-cpu` from main entry. Four columns per halt:
raw baseline, after `psyq___main` thunk fix, after the libgpu thunk
pass (commits `2d3e998` + `4cf43cb` + `89e67dd`), and after the
no-memcards Lua tweak (the dump script flips
`PCSX.settings.emulator.Mcd{1,2}Inserted = false` and
`PCSX.hardResetEmulator()` so pcsxr boots without slot bookkeeping —
matches openpete, which has no BIOS-side memcard state):

| halt at                           | baseline | +\_\_main | +libgpu | +no-mcd |
| --------------------------------- | -------: | --------: | ------: | ------: |
| `0x80012204` main entry           |        0 |         0 |       0 |       0 |
| `0x800122a8` GraphicsInitialize   |        5 |         0 |       0 |       0 |
| `0x80012460` MemoryCardInitialize |      386 |       381 |     247 | **247** |
| `0x800123c8` GamepadInitialize    |      778 |       773 |     642 | **512** |
| `0x8005595c` SpuInitialize        |      910 |       905 |     780 | **656** |
| `0x80012480` CDInitialize         |      936 |       931 |     812 | **675** |
| `0x8001256c` MusicInitialize      |    1,155 |     1,150 |   1,041 | **901** |
| `0x8003385c` GamestateUpdate      |   1.14 M |     (n/a) |   (n/a) |   (n/a) |

After the libgpu/libcard/libpad replay pass (`psyq_MemCardInit` +
`psyq_PadInitDirect` + `psyq_VSyncCallback` post-init writes — commits
`de18c29` + `2e3c242`) and switching to **sp-masked diff** (zero
stack-region bytes below sp on both sides; sp comes from each side's
'CSv1' CPU dump):

| halt at        | masked total | kernel | EXE | post-EXE |
| -------------- | -----------: | -----: | --: | -------: |
| MemoryCardInit |           69 |     69 |   0 |        0 |
| GamepadInit    |          303 |    303 |   0 |        0 |
| SpuInit        |          340 |    311 |   7 |       22 |
| CDInit         |          343 |    305 |  16 |       22 |
| MusicInit      |          529 |    385 | 113 |       31 |

Stack diff is now **zero** at every halt — sp masking confirms the
118+ bytes of stack divergence we saw earlier were all pre-call sub-
frames and `cdrom:SYSTEM.CNF;1` BIOS scratch that lived below sp,
not live state the game would ever read.

### Frame-N pinning via game-function hit count

`--ram-dump-on-frame N` and `PCSXR_DUMP_VSYNCS=N` are _not_ equivalent:
the openpete counter ticks on `VSync(0)` syscalls (PsyCross-side stub
yield), while the pcsxr counter ticks on vblank events (PSX
hardware vblank IRQ inside openbios). The two counters disagree on
"frame N" because pcsxr is _inside the BIOS VSync IRQ handler_ at
vblank time while openpete is yielded inside its PsyCross stub
— different sp / call-stack states.

Use **Nth-call pinning on a per-frame game function** instead. New
flags:

openpete: `--halt-hit N` (default 1)
pcsxr: `PCSXR_DUMP_HITS=N` (env)

`GamestateDraw` (0x8001ED5C) is the canonical pin: called once per
frame from `main()` after `GamestateUpdate()` returns. Both engines
land in identical CPU state at hit 1:

```
pc=0x8001ED5C sp=0x801FFFD8 ra=0x80012284 gp=0x80075264
```

so cmp-masked.py's stack masking works as designed.

### Frame-0 heap diff — synchronous-CD timing skew (resolved as known)

Masked diff at `--halt-at-pc 0x8001ED5C --halt-hit N` (Nth call to
`GamestateDraw`):

| hit N |  total | kernel | EXE | post-EXE |       heap |
| ----: | -----: | -----: | --: | -------: | ---------: |
|     1 | 79,702 |    402 | 247 |       28 | **79,025** |
|     5 | 79,893 |    421 | 340 |       84 |     79,048 |
|    10 | 73,632 |    426 | 348 |       84 |     72,774 |
|    30 |  6,722 |    434 | 348 |       84 |  **5,856** |

The 79 KB heap diff at frame 0 is **op-only nonzero** at clusters from
0x80147BB0 to 0x801C5800. Bisection: heap diff is 0 at every halt
inside `Initialize()` (through `func_8005B7D8`, `StartCutscenePlayback`,
`func_8002D170`); it appears once main's loop is reached. The smoking
gun is in the openpete log — two large CdReads land before the first
`GamestateDraw`:

```
thunks: CdRead sectors=128 buf=0x80147BB0 mode=0x80   # 256 KB
thunks: CdRead sectors=116 buf=0x8018B800 mode=0x80   # 237 KB
```

These addresses cover the diff region exactly.

**Root cause**: `psyq_CdRead`
([games/spyro-1/psyq_thunks.cpp:1101](../games/spyro-1/psyq_thunks.cpp))
calls `CdReadSync(0, NULL)` _inline_, so every CdRead transfer
completes synchronously before the thunk returns. pcsxr's interpreter,
running real PSX cycle counts, leaves those transfers pending across
many frames; the bytes show up gradually as the CD subsystem ticks.
The diff converges from 79 KB → 5.8 KB between frame 1 and frame 30 as
pcsxr catches up.

**Status**: known timing skew, not an openpete recompiler bug.
Genuine same-instant frame-0 RAM parity would require either:

- (a) deferring `CdReadSync` to fire over many host yields (mirrors
  real PSX ~150 KB/sec single-speed throughput),
- (b) a "settle" wait — halt N frames after the target PC and let
  pcsxr's pending reads complete before diffing, or
- (c) live with the skew, mask CD-target buffers in the differ.

Halting at frame 30+ already gives a clean ≪10 KB heap diff — that's
the practical baseline for "frames 1-10 verification" work going
forward. The libgpu kernel/EXE init residual (~700 bytes) is unchanged
across frames and is the remaining bug surface for byte-exact parity.

The libgpu pass is two pieces: (a) `psyq_ResetGraph` writes the
128-byte env struct at `0x80074A64` plus three helper-table words at
`0x80074B68/6C/78` — recovers ~117 bytes one-time; (b) every libgpu
thunk now calls `psyq_mirror_stack_frame(...)` to write its MIPS
prologue's saved-register pushes to the PSX stack, recovering ~10–20
bytes per call point and propagating downstream.

Direction of every cluster up through MusicInit: **pcsxr-only nonzero ≫
openpete-only**. That is, the bytes that differ are bytes pcsxr wrote
(via real PSYQ MIPS code) that openpete's C thunks didn't write. So the
remaining bug surface is "PSYQ thunks aren't byte-faithful with the real
PSYQ libraries pcsxr executes."

### Already-faithful thunks

- `psyq___main` (commit `2d3e998`). The 5 bytes that diverged at the
  GraphicsInitialize halt before this fix were: a "init done" sentinel
  byte at `0x800730BC`, plus 4 bytes of saved-`ra` in `__main`'s own
  stack frame at `0x801FFFD4`. GraphicsInit halt diff went 5 → 0.
- `psyq_ResetGraph` (commit `4cf43cb`). Writes the 128-byte env struct
  at `0x80074A64` plus three helper-table words via `func_80061DEC`'s
  effects. NTSC mode 0 only.
- libgpu prologue mirrors (commit `89e67dd`). `SetDispMask`,
  `SetGraphDebug`, `DrawSync`, `ClearImage`, `PutDrawEnv`, `PutDispEnv`,
  `SetDefDrawEnv` each call `psyq_mirror_stack_frame(...)` matching the
  real MIPS function's prologue (`sp -= N; sw ra/s0..` slots).

### `psyq_mirror_stack_frame` helper

```cpp
struct PsxStackPush { uint32_t offset; uint32_t value; };
static void psyq_mirror_stack_frame(CPUState* cpu, uint32_t frame_size,
                                    uint32_t ra_off,
                                    const PsxStackPush* pushes, size_t n);
```

Pattern for any new PSYQ thunk: disassemble the real function's
prologue, read off `addiu sp, sp, -N` and the `sw <reg>, off(sp)`
instructions, encode as a `PsxStackPush` array. `ra` is mandatory and
gets its own dedicated `ra_off` parameter. The helper restores `sp`
before returning — caller's invariants stay intact.

### Big remaining offenders, in priority order

1. **libgpu kernel/EXE init residual** — 69 kernel bytes + 60 EXE bytes
   constant from MemoryCardInit on. Driven by `ResetGraph`'s sub-calls
   (`func_8005DDC8` -> a vtable slot, `func_80061DEC`, etc.) into
   PSYQ's BIOS event-manager / GPU-command dispatch tables. Kernel
   range: 0x05FC8..0x09245 (EvCB-stride 0x1C visible at 0x08F35..0x08FA5
   confirms event-control-block init). EXE range: 0x80073924..0x8007398B,
   0x80074990..0x800749A3, 0x80074B58..0x80074B7C, 0x80075978..0x8007598F
   — libgpu's command-buffer dispatch / vtable init.
2. **libpad-driven kernel growth** — kernel diff jumps from 69 (at
   MemCardInit halt) to 303 (at GamepadInit halt). 234 bytes of
   `PadInitDirect` / `PadStartCom` / `VSyncCallback(PadVSync)` BIOS
   event-manager state.
3. **Sub-call stack frames** — real `ResetGraph` calls `Memset`,
   `func_80061DEC`, etc.; each pushes its own frame _below_ ResetGraph's.
   Our mirror only covers the top-level frame, leaving the
   `caller_sp - resetgraph_frame - sub_call_frame` regions zero (~110
   bytes residual at MemoryCardInit).
4. **PsyCross-side scratch on the stack** (e.g. `CDFS_Init` writes the
   `STEM.CNF;1` filename at `0x801FFE30`). Real PSYQ doesn't write that
   string there. Hard to suppress without rewriting PsyCross's path
   handling.

### Heap explosion past `MusicInit`

The jump from 1,150 → 1.14M bytes between MusicInitialize and
GamestateUpdate is dominated by the rest of `Initialize()`:
`InitGeom`, `SetGeomOffset`/`Screen`, the RGB24 image-decompression loop
(`func_80017F24`, ~360 KB Memset writes), `WadInitialize` + `CDLoadSync`
(reads 2 KB of WAD header into `g_OverlaySpacePointer`), and lots of
PSYQ display traffic. Bisecting _that_ requires intermediate halt PCs
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
