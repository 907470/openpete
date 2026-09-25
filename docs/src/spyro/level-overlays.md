# Level overlays: code ↔ data, and how openpete recompiles them

`level-format.md` covers the per-level data blob (geometry, textures, object
placement). This doc covers the per-level code overlay that runs that data, how
the two are bound at runtime in the original game, and how openpete's
recompiler turns 37 same-address overlays into co-resident C.

## 1. Why overlays exist

The PSX has 2 MiB of RAM. Spyro does not fit the behaviour code for every
level's mobys, particles, and balloonist/flight logic in memory at once, so
each level ships a code overlay: a raw `.text + .rodata + .data + .bss` blob
streamed from the WAD into a fixed RAM window and executed there. Only one
overlay is resident at a time; the next level's overlay overwrites the previous
one's bytes.

Per the WAD layout (`level-format.md` §1), each `g_WadHeader.m_LevelEntry[i]`
carries both halves (`spyro-1/include/wad.h`, `m_LevelEntry[36]`, last entry
empty):

```
m_LevelEntry[i].m_Overlay { offset, length }   // the code blob, this doc
m_LevelEntry[i].m_Data    { offset, length }   // the data blob, level-format.md
```

`LoadLevel` (`spyro-1/src/loaders.c`) is a stage machine driven by
`g_LoadStage`. Stage 2 sets `g_LevelId` from `g_NextLevelId`, calls
`SetOverlayPointers`, then issues `CDLoadAsync(..., g_OverlaySpacePointer,
m_Overlay.m_Length, m_Overlay.m_Offset, 600)`. The data blob is read in later
stages from `m_Data.m_Offset`: stage 3 the `LevelHeader`, stages 4-5 the VRAM
image, stage 8 the level data, stage 9 the models, stage 11 the scene. Code and
data are independent CD reads to independent RAM regions.

## 2. The overlay RAM window and id word

All level overlays load to the same kseg0 virtual address `0x8007AA38`.
`g_OverlaySpacePointer` is initialized to `&main_BSS_END`
(`spyro-1/src/overlay_pointers.c`), which the link resolves to `0x8007aa38`
(`spyro-1/build/psx.map`, `main_BSS_END`). openpete's fn-pointer harvester
scans `0x8007AA38:0x8008EA38`, a 0x14000-byte window
(`scripts/regen-recomp.sh`, `--overlay-range`).

`spyro-1/overlays.ld` defines the overlay sections through three macros —
`LEVEL`, `LEVEL_BALLOON`, `LEVEL_FLIGHT` — each of which emits `LONG(overlay_id)`
as the first word of the section, ahead of any code or rodata. At runtime the
first word at `0x8007AA38` therefore identifies which overlay's bytes are live.
Sections are padded to a 2048-byte sector boundary.

Filename → section-name → id, as `scripts/regen-recomp.sh` classifies the
matching decomp's `build/wad/*.ovl`:

```
level_<F>_*_code.ovl  ->  id = F + 11, section "level_NN", NN = (F/6 + 1)*10 + (F%6)
titlescreen_code.ovl  ->  id 48
credits.ovl           ->  id 47
```

So `level_0_artisans_home_code.ovl` is section `level_10` with id 11,
`level_10_peace_keepers_doctor_shemp_code.ovl` is section `level_24` with id 21,
and the last level slot `level_64` has id 45. The ids in `overlays.ld` match:
`.level_10 { LEVEL_BALLOON(11, level_10) }`. 35 level overlays plus the
titlescreen and credits overlays make 37 entries in
`games/spyro-1/generated/spyro_overlay_table.c`.

## 3. Binding code to data: `SetOverlayPointers` and the callback slots

The main EXE never calls overlay functions by symbol. The binding is a fixed set
of RAM slots holding function pointers, which the engine calls indirectly.
`SetOverlayPointers` (`spyro-1/src/overlay_pointers.c`, called from `LoadLevel`
stage 2) switches on `g_LevelId` and installs the current level's pointers.
The slots, with the decomp's names for them
(`spyro-1/include/overlay_pointers.h`):

```c
g_Buffers.m_CopyBuf                                 // per-level copy-buffer routine
Moby *(*g_SpawnMoby)(int pClass, Moby *pParent);
void  (*g_UpdateMoby)();
void  (*D_800758E4)(int pAmount, int pClass, void*, void*);  // g_ParticleSpawn
void  (*g_UpdateParticle)(int);
void  (*D_800757A0)();                              // g_BalloonistTriggerCode
void  (*D_800758A8)();                              // g_BalloonistCloseDialogue
void  (*D_8007574C)();                              // g_BalloonistCameraCode
void  (*D_800758D8)();                              // g_BalloonistTalkCode
void  (*D_80075694)();                              // g_UnkFunc1Flight
void  (*D_800757A8)(int pUnk);                      // g_UnkFunc2Flight
void  (*g_FlightResultsUpdate)();
void  (*D_8007567C)();                              // g_DrawGameState7
int   (*D_800758C4)(int pTimer, Vector3D*, int);    // g_DrawTime
```

Not every level fills every slot. A plain level fills the five moby/particle/
copy-buffer slots and leaves the rest holding the previous level's values (the
unset assignments are commented out in `SetOverlayPointers`, matching the
original). Balloonist levels add the four `Balloonist*` slots; flight levels add
the five flight slots.

Two naming schemes coexist in the decomp for overlay functions, and they refer
to the same address space:

- Matched functions are emitted by the `NAME_OVERLAY_FUNCTION` /
  `NAME_OVERLAY_FUNCTION_LEVEL` macros in `spyro-1/include/overlay_pointers.h`,
  which paste the per-TU `#define LEVEL <NN>` onto a generic name. The shared
  bodies live in `spyro-1/include/overlays/{moby_spawn,balloonist,flight}.inc.h`
  and yield `SpawnMoby_level_15`, `Balloonist1_level_10`, `Flight4_level_15`.
  `FOR_LEVELS`, `FOR_BALLOONS`, and `FOR_FLIGHTS` in the header enumerate which
  slots are shared so far.
- Unmatched functions keep the `func_level_<NN>_<addr>` name from
  `spyro-1/asm/nonmatchings/overlays/level_<NN>/`, e.g.
  `func_level_15_8007CFB4`. `NN` here is the section number, not the WAD file
  index.

The engine's generic update loop calls `g_SpawnMoby(...)`, `g_UpdateMoby()`, and
the rest, and execution lands inside whichever overlay is resident. This
indirection is the seam openpete preserves. The data blob references moby
classes by index; the overlay supplies the behaviour for those classes through
these slots. Data and code are matched by being loaded together for the same
`m_LevelEntry[i]`.

## 4. How openpete recompiles 37 overlays into one binary

psxrecomp does static (build-time) translation of MIPS to C. Overlays break two
of its assumptions: they are raw blobs with no PS1-EXE header, and they all
share one virtual address, so their symbols collide. `scripts/regen-recomp.sh`
solves both, one overlay at a time in `process_overlay`.

### 4.1 Raw-blob input (`--raw-bin`)

psxrecomp ingests the raw `.ovl` bytes:
`--raw-bin --load-addr=0x8007AA38 --output-stem=<name>.bin`. It prepends a
2 KiB `PS-X EXE` header in memory (`third_party/psxrecomp/recompiler/src/main_psx.cpp`,
the `raw_bin` branch): magic, `initial_pc` = `initial_load_address` =
`--load-addr`, `initial_gp` = 0, `file_size` = blob size. The entry point is a
placeholder — real entry points come from `--forced-entry` and `--extra-funcs`.
`--raw-bin` requires a KSEG0 load address. This replaced an out-of-tree
`tools/synth_overlay_exe.py` wrapper so the first-boot compile pipeline can
recompile carved overlay segments without python.

### 4.2 Per-overlay symbol namespace

Each overlay is translated with `--func-prefix=func_<name>_` so its functions do
not collide with the other 36 at `0x8007AA38`; `level_24`'s functions are
`func_level_24_8007AC8C` and so on. The generated dispatcher
`psx_dispatch_compiled` is then `sed`-renamed to `psx_dispatch_<name>` in
`<name>.bin_dispatch.c` so all overlays link together.

### 4.3 Multi-pass forced-entry discovery

Overlay entry points are reached indirectly (function-pointer slots, jump
tables, handwritten `jr` returns), so psxrecomp cannot find them all
statically. `process_overlay` runs two seeding passes, then loops until it
converges:

1. Pass 1 seeds with the per-binary handwritten-asm dispatch targets from
   `games/spyro-1/manifest/asm_dispatch_targets/<name>.txt` (regenerated by
   `tools/extract_asm_dispatch_targets.py` and
   `tools/extract_handwritten_ra_returns.py`), any hand-curated entries in
   `games/spyro-1/manifest/forced_entries/<name>.txt`, and the overlay
   fn-pointer constants harvested from the main EXE by
   `tools/extract_overlay_func_pointers.py`, which scans `lui+addiu` immediate
   pairs resolving into the overlay window.
   `tools/filter_overlay_fn_pointers.py` then drops entries that land on a
   delay slot in this overlay's bytes: forcing an entry on a delay slot makes
   psxrecomp split the preceding function so that its post-`jal` continuation
   becomes an unreachable dispatch entry.
2. Pass 2 adds jump-table targets discovered from pass-1 output
   (`tools/extract_jumptable_targets.py`) and emits `external_jals.txt`.
3. Passes 3 and up add cross-piece split targets
   (`tools/extract_split_targets.py`), repeating until no new target appears, at
   most five passes; an unconverged overlay aborts the regen. psxrecomp emits a
   guest function as several C pieces when a piece boundary is forced — an
   external JAL needs its continuation separately dispatchable, and a forced
   entry can land mid-range — and writes every transfer across such a boundary
   as `call_by_address(<target>); return;`. A target with no registered entry
   stub-misses at runtime, and the stub returns instead of transferring, so the
   guest epilogue never runs and `cpu->sp` leaks one stack frame per call until
   it walks out of RAM. The loop is required because promoting a target splits
   its containing function, which can expose further cross-piece transfers.

`FunctionAnalyzer::add_forced_entry`
(`third_party/psxrecomp/recompiler/src/function_analysis.cpp`) discards any
address outside the image, so one union list of fn-pointer targets can be fed to
every overlay.

The main EXE is recompiled last, so each overlay's `external_jals.txt` can be
folded into its forced-entry set: overlays JAL into main-EXE bytes (gem
accounting, level setup, audio helpers) that the main-EXE static JAL pass cannot
see.

`OPENPETE_REGEN_SCOPE=main_exe` re-translates only the main EXE and reuses the
overlay outputs and registry on disk. `OPENPETE_REGEN_PC_TRACKING=1` adds
`--emit-pc-tracking` for exact-instruction write attribution.

### 4.4 Converged manifest

After a full regen, `scripts/emit-recomp-manifest.sh` snapshots the converged
inputs into `games/spyro-1/manifest/recomp/SCUS-94228/`: one `[[overlay]]` table
per overlay with its name, id, `func_prefix`, `load_addr`, WAD filename, input
sha256, and final forced-entry list. The first-boot compile pipeline runs a
single psxrecomp pass per binary from that manifest, with no discovery and no
python. `scripts/verify-recomp-manifest.sh` proves single-pass equals
multi-pass.

The committed snapshot is a distribution input, so the emitter compares what
the regen converged to against it and fails on any difference instead of
rewriting it; a regen from a fresh checkout therefore proves the snapshot
current. A forced-entry, `psyq.toml`, or psxrecomp change that moves discovery
re-runs the regen with `OPENPETE_MANIFEST_BLESS=1` and commits the snapshot in
the same change.

### 4.5 Runtime overlay routing (`dispatch_chain.c`)

Since all overlays share `0x8007AA38`, only the resident one may match a
dispatch. `games/spyro-1/dispatch_chain.c` tracks the resident overlay in
`s_active`:

```c
int psx_dispatch_compiled(CPUState* cpu, uint32_t addr) {
    if (psx_dispatch_main_exe(cpu, addr)) return 1;   // main EXE always live
    if (s_active) return s_active->fn(cpu, addr);      // only the resident overlay
    return 0;
}
```

`psyq_CdRead` (`games/common/psyq/psyq_thunks.cpp`) updates `s_active` after the
bytes have settled: when the read's destination `cpu->a1` is exactly
`0x8007AA38`, it reads the first word from RAM and calls
`spyro_set_active_overlay_by_magic(magic)`. An unrecognized id leaves the
tracker alone, which is what makes cutscene-style loads to the same vaddr safe
for routing — their first word is never an overlay id. Those loads still clobber
the overlay's bytes.

The id → name → dispatcher table is the auto-generated
`games/spyro-1/generated/spyro_overlay_table.c`, declared by
`games/spyro-1/spyro_overlay_table.h`. Savestates carry the active id in the
game-side extra payload (`game_state_extra_save` in
`games/spyro-1/game_extras_impl.c`, field `ovl` at offset 8, version 2 and up)
rather than re-deriving it from the RAM marker, because a cutscene-style load
can legitimately overwrite that word before the snapshot is taken.
`game_on_state_loaded` re-reads the word as the fallback path.

## 5. Decomp ⇄ recomp boundary for overlays

Every overlay currently runs as psxrecomp's translated C. No overlay TU appears
in `SPYRO_NATIVE_SOURCES` in `games/spyro-1/CMakeLists.txt`; only the main-EXE
side `overlay_pointers.rewritten.c` is rewritten, and the shipping presets
(`cmake/presets/linux-release.cmake`, `windows-mingw.cmake`, `wasm.cmake`) pin
`OPENPETE_SKIP_DECOMP=ON`, which drops the rewritten TUs entirely and runs the
full recomp path.

The decomp has partially matched overlay sources under
`spyro-1/src/overlays/level_<NN>/`, split into `moby.c`, `particle.c`,
`terminator.c`, plus `balloonist.c` or `flight.c` where the level has one. Each
file is a mix of `INCLUDE_ASM` stubs and shared `.inc.h` bodies. Swapping such a
function into openpete uses the same manifest/thunk infrastructure as any other
function; nothing in the overlay path is special-cased against it.

Either way the function-pointer slot mechanism (§3) is unchanged: a decomped
overlay installs the same `g_SpawnMoby` pointers the recompiled one would. The
seam is address-stable, which is why the named-anchor hook refactor
(`notes/plans/openpete-generic-hook-anchor-refactor.md`) and the modding
framework can target overlay behaviour by anchor.

## 6. Gotchas for openpete work

- One overlay resident at a time. Two level overlays' symbols never coexist in
  RAM. Cross-level references go through the main EXE.
- Routing follows the CD read. A feature that short-circuits or reorders disc
  loads (fast-load, custom CD simulation, savestates) must still leave the
  correct id word at `0x8007AA38` and call
  `spyro_set_active_overlay_by_magic`, or dispatch runs the wrong overlay: a
  stub miss, or plausible-but-wrong code. See
  `notes/plans/openpete-cd-drive-simulation.md`.
- Regen inputs. `regen-recomp.sh` harvests fn-pointer constants from the
  PSX.EXE bytes, so a run into an empty `generated/` is complete. Re-run after
  bumping psxrecomp, editing a forced-entry list, or pulling a new EXE or
  overlay dump.
- Under-converged main-EXE stubs. The main EXE is recompiled in two passes:
  pass 1 writes a scratch `generated/_main_exe_pass1/PSX.EXE_full.c`, from which
  `tools/extract_dispatch_call_by_address.py` derives
  `generated/main_exe_dispatch_stubs.txt` (37 entries as committed); pass 2
  folds those in and produces the linked `generated/PSX.EXE_full.c`. The
  `SetOverlayPointers` jump table lives behind those stubs, so a short stubs
  file means some level swaps never re-install their fn-pointer slots, the
  previous overlay's stale pointers get `jalr`'d, and dispatch cascades into
  stub misses. Pass 1 deletes pass 2's outputs first so a failed pass 2 cannot
  leave a stale converged file to link.
- No third pass on the main EXE. Adding a single forced entry to the main EXE
  shifts function-boundary detection enough to lose pre-existing dispatch
  entries; the split-target loop is enabled for overlays only. The mechanism is
  not understood and the investigation is deferred.

See `level-format.md` for the data side and `docs/modding/pages/levels.md` for how
custom levels are built and shipped.
