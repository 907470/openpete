# Level overlays: code ↔ data, and how openpete recompiles them

`level-format.md` covers the per-level **data** blob (geometry, textures, object
placement). This doc covers the per-level **code overlay** that runs that data,
how the two are bound at runtime in the original game, and how openpete's
recompiler turns 23 mutually-exclusive overlays into co-resident C.

## 1. Why overlays exist

The PSX has 2 MiB of RAM. Spyro can't fit the behaviour code for every level's
mobys, particles, and balloonist/flight logic in memory at once, so each level
ships a **code overlay**: a raw `.text + .rodata + .data` blob that is streamed
from the WAD into a fixed RAM window and executed there. Only one overlay is
resident at a time — the next level's overlay overwrites the previous one's
bytes.

Per the WAD layout (`level-format.md` §1), each `g_WadHeader.m_LevelEntry[i]`
carries **both** halves:

```
m_LevelEntry[i].m_Overlay { offset, length }   // the code blob, this doc
m_LevelEntry[i].m_Data    { offset, length }   // the data blob, level-format.md
```

`LoadLevel` stage 2 streams the overlay to `g_OverlaySpacePointer`
(`= &main_BSS_END`, i.e. immediately after the main EXE's BSS); stage 8 streams
the data. They are independent CD reads to independent RAM regions.

## 2. The overlay RAM window and id word

All level overlays load to the **same kseg0 virtual address: `0x8007AA38`**
(`g_OverlaySpacePointer` resolves there at link time). The overlay window spans
roughly `0x8007AA38 .. 0x8008EA38` (~0x14000 bytes — the range openpete's
fn-pointer harvester scans).

The original build's `overlays.ld` inserts a **LONG id word at the head of each
overlay section** (the "magic"). At runtime the first word at `0x8007AA38` tells
you which overlay's bytes are currently live. The id-to-name mapping (from
openpete's auto-generated table):

```
credits=47   titlescreen=48
level_10=11  level_11=12  level_12=13  level_13=14  level_14=15  level_15=16
level_20=17  level_21=18  ...          (one id per level slot)
```

Filename → section-name convention (from the matching decomp's `build/wad/`):

```
level_<F>_*_code.ovl  ->  overlay_id = F + 11
                          section "level_NN", NN = (F/6 + 1)*10 + (F%6)
titlescreen_code.ovl  ->  id 48
credits.ovl           ->  id 47
```

## 3. Binding code to data: `SetOverlayPointers` and the callback slots

The main EXE never calls overlay functions by symbol. Instead the overlay
exports its entry points by writing **function pointers into fixed RAM slots**
that the main engine calls indirectly. `SetOverlayPointers`
(`spyro-1/src/overlay_pointers.c`, called from `LoadLevel` stage 2) installs the
current level's pointers into globals like:

```c
extern Moby *(*g_SpawnMoby)(int pClass, Moby *pParent);   // per-level moby spawn
extern void  (*g_UpdateMoby)();
extern void  (*D_800758E4)(...);                           // g_ParticleSpawn
extern void  (*g_UpdateParticle)(int);
extern void  (*D_8007574C)();                              // balloonist camera
extern int   (*D_800758C4)(int, Vector3D*, int);           // flight timer draw
// ... balloonist/flight callbacks, etc.
```

In the decomp these per-level functions are named with the `FOR_LEVELS` /
`NAME_OVERLAY_FUNCTION` macro machinery, yielding symbols like
`func_level_10_8007XXXX`. Each level slot fills the same handful of slots with
its own implementations. The engine's generic update loop then calls
`g_SpawnMoby(...)`, `g_UpdateMoby()`, etc., and execution lands inside whichever
overlay is resident.

**This indirection is the seam openpete exploits and must preserve.** The data
blob (`level-format.md`) references moby *classes* by index; the overlay
supplies the *behaviour* for those classes via these slots. Data and code are
matched by being loaded together for the same `m_LevelEntry[i]`.

## 4. How openpete recompiles 23 overlays into one binary

psxrecomp does static (build-time) translation of MIPS → C. Overlays break two
of its assumptions: (a) they're raw blobs with no PS1-EXE header, and (b) they
all share one virtual address, so their symbols collide. The pipeline in
`scripts/regen-recomp.sh` solves both.

### 4.1 Synthesize a PS1-EXE wrapper

`tools/synth_overlay_exe.py` prepends a 2 KiB `PS-X EXE` header in front of the
raw `.ovl` bytes, with `load_address = 0x8007AA38`. The result is byte-identical
to a real EXE that loads the overlay at its window; only the entry point is
faked (real entries come from `--forced-entry`/`--extra-funcs`, not the header).

### 4.2 Per-overlay symbol namespace

Each overlay is translated with `--func-prefix=<name>` so its functions don't
collide with the other 22 overlays (all at `0x8007AA38`). The generated
dispatcher `psx_dispatch_compiled` is then `sed`-renamed to
`psx_dispatch_<name>` so all overlays link together.

### 4.3 Multi-pass forced-entry discovery

Because overlay entry points are reached *indirectly* (function-pointer slots,
jump tables, handwritten `jr` returns), psxrecomp can't find them all
statically. `process_overlay` runs up to three passes:

1. **Pass 1** — seed with handwritten-asm dispatch targets
   (`extract_asm_dispatch_targets.py` / `extract_handwritten_ra_returns.py`)
   plus the overlay-fn-pointer constants harvested from the *main EXE*
   (`extract_overlay_func_pointers.py` scans `lui+addiu` immediate pairs that
   resolve into the overlay window). The fn-pointer list is filtered per overlay
   to drop entries landing on delay slots in *this* overlay's bytes.
2. **Pass 2** — add jump-table targets discovered from pass-1 output
   (`extract_jumptable_targets.py`), and emit `external_jals.txt`.
3. **Pass 3** — add jal-continuation split targets
   (`extract_jal_cont_targets.py`): external JALs whose target isn't a known
   symbol get split into `call_by_address(cpu, jal+8)`, which needs a forced
   entry at `jal+8` or it stub-misses at runtime.

Out-of-range forced entries are silently dropped by psxrecomp, so a single
union list of fn-pointer targets can be fed to every overlay.

### 4.4 Runtime overlay routing (`dispatch_chain.c`)

Since all overlays share `0x8007AA38`, only the *resident* one may match a
dispatch. openpete tracks the active overlay by the id word:

```c
// psyq_CdRead reads the first word at 0x8007AA38 after each disc transfer
// and calls spyro_set_active_overlay_by_magic(id).
int psx_dispatch_compiled(CPUState* cpu, uint32_t addr) {
    if (psx_dispatch_main_exe(cpu, addr)) return 1;   // main EXE always live
    if (s_active) return s_active->fn(cpu, addr);      // only the resident overlay
    return 0;
}
```

The `magic → name → dispatcher` table lives in the auto-generated
`games/spyro-1/generated/spyro_overlay_table.c`; `dispatch_chain.c` selects
`s_active` whenever a CD read swaps the resident bytes. This means **openpete's
overlay correctness depends on faithfully simulating the CD read that loads the
overlay** — the id word must land at `0x8007AA38` for routing to flip. See the
CD-drive-simulation plan.

## 5. Decomp ⇄ recomp boundary for overlays

A level overlay is in one of two states in openpete at any time:

- **Recompiled** — psxrecomp's translated C runs the overlay (current default
  for most overlays). Behaviour is bit-exact with the original MIPS.
- **Decomped** — once `spyro-1/src/overlays/level_NN/*.c` matches, those
  functions can be swapped in via the manifest/thunk infrastructure (Phase 2+),
  same as any other function. The decomp already has matched overlay sources for
  several levels (`spyro-1/src/overlays/level_{10,15,20,...}/`), each split into
  `moby.c`, `particle.c`, `terminator.c`, `flight.c`, `balloonist.c`.

Either way the **function-pointer slot mechanism (§3) is unchanged** — a
decomped overlay installs the same `g_SpawnMoby` etc. pointers the recompiled
one would. The seam is address-stable, which is exactly why the
generic-hook/per-ROM-anchor refactor (`openpete-generic-hook-anchor-refactor`)
and the modding framework can target overlay behaviour by named anchor.

## 6. Gotchas for openpete work

- **One overlay resident at a time.** Never assume two level overlays' symbols
  coexist in RAM — they don't. Cross-level references go through the main EXE.
- **Routing follows the CD read.** If a feature short-circuits or reorders disc
  loads (fast-load, custom CD sim, savestates), it must still deposit the
  correct id word at `0x8007AA38` and call
  `spyro_set_active_overlay_by_magic`, or dispatch silently runs the wrong
  overlay (stub-miss or, worse, plausible-but-wrong code).
- **Regen ordering.** `regen-recomp.sh` needs `PSX.EXE_full.c` from a prior run
  to harvest fn-pointer constants; the very first run misses them (acceptable,
  fixed on the second). Re-run after bumping psxrecomp, changing a forced-entry,
  or pulling a new EXE/overlay dump.
- **Stale `generated/`.** An un-converged regen leaves a partial overlay table
  and causes moby-queue corruption mid-demo (this has bitten before — Dr Shemp
  hang traced to a 36-entry `main_exe_dispatch_stubs.txt` shortfall). If overlay
  behaviour goes wrong after a toolchain change, re-run `regen-recomp.sh` to
  convergence before debugging the C.

See `level-format.md` for the data side and `level-modding.md` for how custom
levels / mods interact with both halves.
