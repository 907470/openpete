# Level data and openpete's modification plans

How the level format (`level-format.md`) and overlay system (`level-overlays.md`)
constrain and enable the various `notes/plans/openpete-*` modification plans.
This is the "so what" doc: read the other two for the format, read this to know
which plans touch which structures and what the landmines are.

It also documents **SpyroEdit** as the working reference editor, since openpete
now has it as a sibling repo (`../SpyroEdit`).

## 1. SpyroEdit as a reference (and its limits for Spyro 1)

SpyroEdit (LXShades) is a real-time emulator-plugin editor: it attaches to a
running ePSXe/PCSX, **signature-scans** RAM to locate the live structures, and
edits them in place. It lives as a sibling repo at `../SpyroEdit`, pinned (at
the time this doc was written) to upstream commit
[`e311b00`](https://github.com/LXShades/spyroedit/commit/e311b00721d427757be9e2b794788da013eae167)
— it's a live upstream project, so re-check against that commit if its line
references drift. For openpete it is valuable as:

- **A second, independent decode** of the geometry interiors the matching
  decomp leaves as `void *` (sector vertex/face bitfields, texture cache, object
  struct). Cross-referenced throughout `level-format.md`.
- **A worked example of safe in-place level mutation** — resizing sectors,
  generating craters, converting to/from its "Genesis" 3D editor format,
  saving/loading scenes. If openpete ever grows runtime level editing, SpyroEdit
  is the prior art.

**Hard caveats:**

- SpyroEdit is a **Spyro 2/3-first** tool with self-described "limited
  compatibility" for Spyro 1. Its SPYRO1 code paths are `if (game == SPYRO1)`
  branches that are empirically derived and sometimes absent (e.g.
  `SceneFace::GetEdge` has no SPYRO1 case). Verify any SPYRO1 bitfield against
  the decomp renderer before trusting it for byte-exact work.
- It locates data by **instruction-pattern scanning**, not symbol. openpete
  doesn't need this — the matching decomp gives every structure a named global
  (`g_Environment`, `g_Buffers.m_LevelScene`, `g_LevelMobys`, `g_Portals`, …).
  Use the decomp names; SpyroEdit's scanners are only interesting if you need to
  understand *what shape* it's looking for.
- It mutates **live PS1 RAM**. openpete's determinism north star means gameplay
  state must not be touched by presentation features — see §3.

## 2. Plan-by-plan: what touches level data

| Plan | Touches | Key constraint |
|---|---|---|
| `openpete-disable-lods` | §6 LP/HP sector tiers, §1 LQ/HQ texture table, §9 moby `distHp` | Saturating LOD-distance fields, not deleting LP geometry; some sectors have no LP and the renderer must not request it (`g_SkipLowPolyWorld`, levels 40/42) |
| `openpete-widescreen-fov` | §6 vertex packing, projection | **SPYRO1 Z is half-scale** vs 2/3; reproject in render layer only |
| `openpete-transform-interp` | §9 moby positions, world transform | Render-layer reprojection of cached frames; **never write back into moby state** (collision-grid rethread in `Moby::SetPosition` would desync) |
| `openpete-polybuf-host-redirect` | §3 component pointers (now absolute g_ram addrs) | Redirect happens at the polybuf/primitive layer, downstream of the loaded blob |
| `openpete-ram-virtualization` | §3 pointer fix-up | Any relocation/double-buffer of level blobs must replay fix-up or keep original addresses; loaded pointers are absolute `0x8xxxxxxx` |
| `openpete-cd-drive-simulation` | §2 14-stage async load, overlays §4.4 | Must satisfy the async-CD state machine AND deposit the overlay id word at `0x8007AA38` so dispatch routing flips |
| `openpete-modding-framework` | all | Mods target named anchors (overlay slots §3, data structures); gameplay mods rebase the oracle baseline |
| `openpete-distribution-local-recomp` | overlays §4 | Per-ROM recompile resolves overlay symbols per binary; custom levels are another recomp input |
| `openpete-psx-oracle-ci` | all | The 20k-frame determinism gate hashes gameplay-state regions that level data feeds into |

## 3. The determinism boundary (most important rule)

openpete's correctness model splits the world in two:

- **Gameplay state** — everything the game logic reads/writes in `g_ram`:
  moby positions/state, Spyro, collision, RNG, save flags. This must stay
  **bit-identical** to real PSX hardware (enforced by the oracle CI gate).
- **Presentation** — render output, projection, interpolation, resolution.
  Lives in the render layer where gameplay can't observe it.

Level data straddles this. The **loaded blob and its fixed-up pointers are
gameplay state** — they're what the engine reads. The **derived geometry the
renderer emits** (projected vertices, polybuf primitives) is presentation.

Therefore:

- ✅ Reproject §6 vertices / §9 moby positions **in the render path** (widescreen,
  interp) — read game state, transform for display, never write back.
- ✅ Saturate LOD distance fields **as a presentation choice** *if* it doesn't
  change what gameplay reads — but note moby `distHp`/`distDraw` (§9) are read by
  game logic in some paths, so `--no-lod` had to be careful (see the plan's
  phase notes about the `m_Distance==0` uninit-sentinel that the renderer hangs
  on if clobbered).
- ❌ A presentation feature must **never** call anything resembling
  `Moby::SetPosition` (§9) — moving a moby across a 13-bit collision-grid
  boundary rethreads `mobyCollisionRegions[]` chains; that's gameplay state and
  would diverge the oracle instantly.
- ⚠️ **Gameplay mods deliberately diverge** and are opt-in: per the
  modding-framework plan, an enabled gameplay modset gets its **own oracle
  baseline** (run on hardware), it does not share the canonical gate.

## 4. Custom / edited levels (forward-looking)

openpete has no custom-level support today, but the format makes the shape
clear. A custom level needs, at minimum:

1. A **data blob** matching `LoadLevelData` + `LoadLevelScene` component layout
   (`level-format.md` §3–§5): self-sizing components, payload-relative pointers,
   the exact section order. The loaders are unforgiving — they trust the
   component size words to skip, so a wrong size word desyncs everything after.
2. A **`LevelHeader`** (§2) describing the VRAM/SRAM/Data/ModelData/Scene
   sub-regions and the model offset/index tables.
3. A **code overlay** (`level-overlays.md`) supplying the moby-class behaviour
   via the `SetOverlayPointers` slots — or reuse an existing level's overlay if
   the custom level uses only stock moby classes.
4. A **VRAM image** (512×512×2) + texture table (§7) + collision mesh (§8).

Two viable authoring routes:

- **In-place edit of a stock level** (SpyroEdit's model): load a real level,
  mutate sectors/objects/textures within the existing component framework,
  re-emit. Lowest risk; bounded by stock VRAM/collision.
- **Synthetic blob generation**: build the component stream from scratch with a
  tool. This is where the openpete recompiler's overlay-synthesis pattern
  (`synth_overlay_exe.py`) is a useful precedent — a small Python tool that
  emits the exact byte layout the loader expects.

Either way, custom levels are a **content mod** under the modding-framework
plan: opt-in, with their own determinism baseline (there is no "correct"
hardware reference for a level that never shipped, so the baseline is the
custom build itself).

## 5. Quick map: decomp global ⇄ structure ⇄ doc

| Decomp global | What it holds | Doc |
|---|---|---|
| `g_WadHeader.m_LevelEntry[i]` | per-level {overlay, data} offsets | format §1, overlays §1 |
| `g_LevelHeader` | sub-region offsets for the data blob | format §2 |
| `g_Environment` | sectors, occlusion, surfaces, textures, collision | format §4, §6–§8 |
| `g_EnvironmentAnimations` | texture/scroll/poly/color/collision anims | format §5 |
| `g_NewCyclorama` / `g_Portals[]` | skybox + portal skyboxes | format §4 |
| `g_Buffers.m_LevelScene` | the scene blob (objects + anims) | format §5 |
| `g_LevelMobys` / `g_DynMobys` | object instance array | format §5, §9 |
| `g_Models[]` | per-class moby models (geometry/anim) | format §9 |
| `g_OverlaySpacePointer` (`0x8007AA38`) | resident overlay window | overlays §2 |
| `g_SpawnMoby` / `g_UpdateMoby` / … slots | overlay→engine callback bindings | overlays §3 |

---

*Companion docs: `level-format.md` (on-disc/in-RAM structures),
`level-overlays.md` (code overlays + recompilation). Reference editor:
`../SpyroEdit` (sibling repo, Spyro 2/3-first, limited Spyro 1).*
