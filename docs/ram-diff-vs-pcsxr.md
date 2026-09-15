# openpete ↔ PCSX-Redux single-shot RAM diff

Run PCSX-Redux to a chosen PC and capture its 2 MB main RAM plus CPU
state; run openpete from the same seed to the same PC; byte-diff the two
RAMs. Any diverging byte is a candidate recompiler or PSYQ-thunk defect.

## Relationship to the DuckStation oracle

The canonical PSX oracle is DuckStation (`docs/duckstation-oracle.md`,
`scripts/check_psx_oracle.sh`): real SCPH1001 BIOS, per-tick hashes of
the regions in `games/spyro-1/manifest/gameplay_state_regions.toml`, 20k-tick soak.
PCSX-Redux runs OpenBIOS and its GPU emulation wedges in Spyro 1's
cyclorama renderer around VBL 120, so it cannot produce a long trace.

What PCSX-Redux still provides that the oracle does not: a full 2 MB RAM
image at an exact instant, over the whole address space rather than the
manifest regions, with a matching CPU-register dump. That is the tool for
attributing a byte-level difference to a specific function, and for
verifying a PSYQ thunk writes what the real PSYQ library writes. The
DuckStation oracle names the first divergent tick and region; this names
the bytes. `scripts/capture_psx_oracle_ds.sh` supersedes
`scripts/capture_psx_oracle.sh` for trace capture; the PCSX-Redux path
below is kept for these single-shot dumps.

The DuckStation module now takes the same single-shot dump
(`-dump-ram-at-anchor`), and `scripts/check_ram_diff.sh` runs both sides
and classifies the diff (see `duckstation-oracle.md`, "Whole-RAM diff at
an anchor"); the PCSX-Redux path below is the forensic fallback.

Companion to [gte-trace-vs-reference](gte-trace-vs-reference.md), which
verifies GTE math rather than whole-RAM state.

## Components

### openpete flags

Parsed in `core/config/options.c`; help text under the debug block.

| Flag | Effect |
| --- | --- |
| `--seed-ram PATH` | Overwrite `g_ram` (2 MB) from PATH after EXE load, so openpete starts from the RAM PCSX-Redux had at the seed PC. |
| `--seed-cpu PATH` | Overwrite the 32 GPRs, pc/hi/lo, cop0 and both GTE banks from a 528-byte `'CSv1'` blob (`load_cpu_seed`, `core/runtime/run.c`). Suppresses any `--test-recomp-gp` override. |
| `--halt-at-pc HEX` | When `psx_override_dispatch` sees a call into the function at HEX, dump `g_ram` to `--ram-dump-output` and `_Exit(0)`. Fires at a function-call boundary, so the dump instant is exact. |
| `--halt-hit N` | Dump on the Nth (1-indexed) call to `--halt-at-pc`; default 1. |
| `--ram-dump-on-frame N` | Dump after VBL N's game code yields, without halting. |
| `--ram-dump-output PATH` | Output path for all three dump modes; default `./ram.bin`. |

### PCSX-Redux Lua (`scripts/pcsx-redux-ram-dump.lua`)

Driven by environment variables.

| Var | Default | Effect |
| --- | --- | --- |
| `PCSXR_DUMP_PC` | `0x8003385c` | Target PC; the breakpoint fires here. |
| `PCSXR_DUMP_OUT` | `/tmp/pcsxr_ram.bin` | RAM (2 MB) output path. |
| `PCSXR_DUMP_CPU` | unset | Optional CPU output path (528 bytes, `'CSv1'`). Omit to skip. |
| `PCSXR_DUMP_HITS` | `0` | Non-zero dumps on the Nth breakpoint hit and ignores `PCSXR_DUMP_VSYNCS`. |
| `PCSXR_DUMP_VSYNCS` | `0` | 0 dumps immediately on the hit. Larger values count vblank events after the hit and dump on the Nth. |

The script clears `PCSX.settings.emulator.Mcd{1,2}Inserted` and calls
`PCSX.hardResetEmulator()` before running, so PCSX-Redux boots without
memory-card slot bookkeeping. openpete has no BIOS-side memcard state,
and leaving the slots inserted puts hundreds of unmatchable bytes in the
diff.

PCSX-Redux must be launched with `--debugger` for breakpoints to fire.

### `'CSv1'` CPU-state file format

528 bytes, little-endian. Produced by the Lua `dump_cpu` helper, consumed
by `load_cpu_seed` in `core/runtime/run.c`.

```
offset  size  field
   0    4     magic 0x31765343 ('CSv1')
   4    128   gpr[32]  in MIPS order: zero, at, v0, v1, a0..a3,
                       t0..t7, s0..s7, t8, t9, k0, k1, gp, sp, fp, ra
 132    4     pc
 136    4     hi
 140    4     lo
 144    128   cop0[32]
 272    128   gte_data[32]
 400    128   gte_ctrl[32]
```

## Recipe

Two PCSX-Redux captures, one openpete run, one diff. The seed PC is a
point both engines are known to agree on; the oracle PC is the point
under investigation.

```sh
PCSXR=openpete/.tools/pcsx-redux/pcsx-redux
SCRIPT=openpete/scripts/pcsx-redux-ram-dump.lua
DISC=spyro-1/build/SCUS-94228.cue
BIOS=openpete/.tools/pcsx-redux/openbios.bin

SEED_PC=0x80012204     # Spyro 1 main entry
PCSXR_DUMP_PC=$SEED_PC PCSXR_DUMP_OUT=/tmp/seed.ram PCSXR_DUMP_CPU=/tmp/seed.cpu \
    $PCSXR --no-ui --debugger --interpreter \
        --bios $BIOS --iso $DISC --run --dofile $SCRIPT

ORACLE_PC=0x8003385c   # GamestateUpdate, post-Initialize
PCSXR_DUMP_PC=$ORACLE_PC PCSXR_DUMP_OUT=/tmp/oracle.ram PCSXR_DUMP_CPU=/tmp/oracle.cpu \
    $PCSXR --no-ui --debugger --interpreter \
        --bios $BIOS --iso $DISC --run --dofile $SCRIPT

openpete/build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc $DISC --headless \
    --run-recomp $SEED_PC \
    --seed-ram /tmp/seed.ram --seed-cpu /tmp/seed.cpu \
    --halt-at-pc $ORACLE_PC --ram-dump-output /tmp/op.ram

cmp -l /tmp/op.ram /tmp/oracle.ram | wc -l    # bytes differing
cmp -l /tmp/op.ram /tmp/oracle.ram | head -20 # first divergences
```

Region histogram from the `cmp -l` output:

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

Mask the stack below `sp` on both sides before reading a stack figure:
`sp` is GPR 29, at offset 120 of each side's `'CSv1'` dump, and the bytes below
it are dead sub-frames plus BIOS scratch (`cdrom:SYSTEM.CNF;1` and
similar) that no game code reads. Unmasked, they dominate the stack
column and read as divergence.

## Picking a halt PC

The halt PC must be the entry of a recompiled game function. The
recompiler emits `if (psx_override_dispatch(cpu, 0xADDR)) return;` at the
top of every generated body
(`third_party/psxrecomp/recompiler/src/code_generator.cpp:1973`), and
`--halt-at-pc` is checked inside that call (`core/psx/dispatch.c`,
`psx_override_dispatch`).

A PSYQ address has no generated body: it is a `case 0xADDR:
psyq_FOO(cpu); return 1;` arm in
`games/spyro-1/generated/PSX.EXE_dispatch.c`, which never reaches the
override gate. A halt on a PSYQ PC silently never fires.

Symbols come from the decompile's map file:

```sh
grep -E "GamestateUpdate|GamestateDraw|Initialize" spyro-1/build/psx.map
```

`GamestateDraw` (`0x8001ED5C`) is the canonical per-VBL pin: `main()`
calls it once per VBL after `GamestateUpdate()` returns, so
`--halt-at-pc 0x8001ED5C --halt-hit N` and `PCSXR_DUMP_PC=0x8001ED5C
PCSXR_DUMP_HITS=N` land both engines in the same call at the same depth.
It is also the default anchor of both oracle capture scripts.

Do not pin by VBL count instead. `--ram-dump-on-frame N` counts
`VSync(0)` syscalls, where openpete is yielded inside its PsyCross stub;
`PCSXR_DUMP_VSYNCS=N` counts the emulator's vblank events, where
PCSX-Redux is inside the BIOS vblank IRQ handler. The two counters name different
instants and different call stacks.

## Known non-defect divergences

### Synchronous CdRead

`psyq_CdRead` (`games/common/psyq/psyq_thunks.cpp`) calls `CdReadSync(0,
NULL)` inline, so a transfer completes before the thunk returns.
PCSX-Redux runs real PSX cycle counts and leaves the same transfer
pending across many VBLs. Immediately after a large read, openpete's
target buffer is populated and PCSX-Redux's is not; the diff converges as
PCSX-Redux catches up. The affected ranges are the `buf=` addresses in
openpete's `thunks: CdRead` log lines. This is a timing skew, not a
recompiler defect. The same skew is why the DuckStation oracle flags the
three CD-load pool regions `cd_load_transient` in
`games/spyro-1/manifest/gameplay_state_regions.toml`.

Closing it byte-exactly would need `CdReadSync` deferred across host
yields at the PSX single-speed rate (~150 KB/s), or a settle wait that
halts N VBLs past the target PC. Halting tens of VBLs past the read
already brings the heap diff down by an order of magnitude.

### libpad-driven kernel growth

`PadInitDirect` / `PadStartCom` / `VSyncCallback(PadVSync)` leave BIOS
event-manager state that the C thunks do not reproduce, and the slot-2
pad buffer holds a different idle pattern on each side. The
`pad_input_buffers` region of `games/spyro-1/manifest/gameplay_state_regions.toml` is
masked for this reason.

### PSYQ sub-call stack frames

Real PSYQ functions call further into libgpu and the BIOS; each callee
pushes its own frame below the caller's. openpete's thunks mirror only
the top-level frame, leaving the region between the mirrored frame and
the caller's `sp` zero on the openpete side.

## Making a thunk byte-faithful

The residual after seeding is dominated by bytes PCSX-Redux wrote by
executing real PSYQ MIPS code that openpete's C thunks do not write. Two
mechanisms cover most of it.

Direct writes. `psyq_ResetGraph` writes the 128-byte env struct at
`0x80074A64` and three helper-table words at `0x80074B68`, `0x80074B6C`
and `0x80074B78`, replicating what the real function's callees leave
behind (NTSC mode 0 only). `psyq___main` writes the initialised sentinel
at `0x800730BC`.

Prologue mirrors. `psyq_mirror_stack_frame` writes a MIPS prologue's
saved-register pushes to the PSX stack:

```cpp
struct PsxStackPush { uint32_t offset; uint32_t value; };
static void psyq_mirror_stack_frame(CPUState* cpu, uint32_t frame_size,
                                    uint32_t ra_off,
                                    const PsxStackPush* pushes, size_t n);
```

For a new thunk: disassemble the real function's prologue, read off
`addiu sp, sp, -N` and each `sw <reg>, off(sp)`, and encode the latter as
a `PsxStackPush` array. `ra` is mandatory and takes the dedicated
`ra_off` parameter. The helper restores `sp` before returning.

## Files

- `scripts/pcsx-redux-ram-dump.lua` — capture script
- `core/runtime/run.c` — `load_cpu_seed`, seed-ram block
- `core/psx/dispatch.c` — halt-at-pc hook in `psx_override_dispatch`
- `core/config/options.c` — flag parsing
- `core/include/openpete.h` — option fields
- `games/common/psyq/psyq_thunks.cpp` — thunk bodies, `psyq_mirror_stack_frame`
