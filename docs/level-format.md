# Spyro 1 level data format

Authoritative reference for how *Spyro the Dragon* (NTSC-U, SCUS-94228) stores
and loads level data. This is the format openpete must understand to do
anything beyond a faithful pass-through: widescreen geometry reprojection, LOD
control, host-side render redirection, custom-level support, and runtime
modding all touch the structures described here.

## Sources and trust level

Two independent reverse-engineered sources agree on this format, and this doc
reconciles them:

- **The matching decomp** (`spyro-1/src/loaders.c`, `spyro-1/include/*.h`).
  This is the *actual game loader*, recompiled byte-for-byte. Where it parses a
  field, that parse is ground truth. Its weakness is that several geometry
  payloads are typed `void *` with `// TODO: Type` — the loader walks past them
  without interpreting their interior.
- **SpyroEdit** (`../SpyroEdit/Source/SpyroScene.{h,cpp}`, `SpyroData.h`,
  `SpyroTextures.h`). A real-time emulator-plugin editor by LXShades. It fills
  in exactly the interiors the decomp leaves opaque — sector vertex/face
  bitfields, object struct, texture-cache layout. **Caveat:** SpyroEdit is
  primarily a Spyro 2/3 tool with "limited compatibility" for Spyro 1. Its
  Spyro-1 paths are guarded by `if (game == SPYRO1)` branches that are
  empirically derived and occasionally incomplete (e.g. `SceneFace::GetEdge`
  has no SPYRO1 implementation). Treat SpyroEdit's SPYRO1 bitfield layouts as
  *high-confidence but verify against the decomp renderer before relying on
  them for byte-exact work.*

Terminology note: SpyroEdit calls the world geometry the "scene". The decomp
uses "scene" for a *different* blob (see below) and calls world geometry the
**environment**. This doc uses the decomp's names and flags the collision.

## 1. On-disc layout: the WAD

Everything lives in one `WAD.WAD` file on the disc, indexed by a fixed header
struct loaded into RAM as `g_WadHeader` (`spyro-1/include/wad.h`). Each entry
is an `OffsetLength { int m_Offset; int m_Length; }` — a byte offset into the
WAD and a length. The relevant entries:

```
g_WadHeader.m_LevelEntry[36]   // one per level slot; last is empty
    .m_Overlay   { offset, length }   // per-level CODE overlay (see level-overlays.md)
    .m_Data      { offset, length }   // per-level DATA blob (this document)
```

There are also `m_CutsceneData[4]`, titlescreen/logo entries, credits, and a
game-over skybox, but the per-level pair `{m_Overlay, m_Data}` is the spine.
The **overlay** is executable code (moby/particle/terminator behaviour for that
level); the **data** is the geometry/textures/object-placement payload. They
are loaded independently and to different RAM regions. See `level-overlays.md`
for how the two interact.

`g_LevelEntry` is indexed by `g_LevelIndex`, computed from the logical level id:

```c
g_Homeworld  = (g_LevelId / 10) - 1;
g_LevelIndex = (g_Homeworld * 6) + (g_LevelId % 10);   // loaders.c LoadLevel case 2
```

## 2. The level "Data" blob and its sub-header (`LevelHeader`)

The first 0x800 bytes of the Data entry is a `LevelHeader` (`wad.h`) describing
five sub-regions, each as an offset *relative to the start of the Data entry*
plus a size:

```c
typedef struct {
  int m_VramSramOffset, m_VramSramSize;   // 512x512x2 VRAM image + SPU audio
  int m_DataOffset,     m_DataSize;        // environment payload -> LoadLevelData()
  int m_ModelDataOffset, m_ModelDataSize;  // moby/animation models
  int m_SceneOffset,    m_SceneSize;       // object placement + anims -> LoadLevelScene()
  OffsetLength m_Dragons[6];               // dragon cutscene data
  int   m_ModelOffsets[64];                // per-model byte offsets into ModelData
  short m_ModelIndices[64];                // which g_Models[] slot each model fills
} LevelHeader;
```

The load is a 14-stage state machine (`LoadLevel`, `loaders.c:1290`), driven by
async CD reads. Summarised:

| Stage | Action |
|------:|--------|
| 0/1   | Preserve cyclorama + sound table across a level transition |
| 2     | Load the level **overlay** (code) from `m_Overlay` |
| 3     | Read the 0x800 `LevelHeader` from the head of `m_Data` |
| 4     | Load the 512×512×2 VRAM texture image |
| 5     | `LoadImage` it into VRAM in two 512×256 halves |
| 6/7   | DMA the SPU audio block to SPU RAM at `0x1010` |
| 8     | Load the environment payload (`m_DataOffset`/`m_DataSize`) |
| 9     | **`LoadLevelData()`** parses it; returns ptr to free space |
| 9     | Load model data (`m_ModelDataOffset`) into that free space |
| 10    | Patch model pointers, register them in `g_Models[]` |
| 11    | Load the scene payload (`m_SceneOffset`/`m_SceneSize`) |
| 12    | Wait for camera/portal to settle (transitions only) |
| 13    | **`LoadLevelScene()`** parses it; spawn Sparx, start music |

So the two parsers that actually decode level structure are
**`LoadLevelData`** (environment/world) and **`LoadLevelScene`** (objects +
animations). The rest is plumbing.

## 3. The "component" container format

Both parsers consume a stream of length-prefixed **components**. The decomp
expresses this with four macros (`loaders.c:56`):

```c
#define READ_I32(x)               *(int *)x, x += 4       // read+advance
#define READ_COMPONENT_HEADER(x)  componentStart = (char *)x
#define COMPONENT_ADVANCE(x, y)   x += y
#define COMPONENT_START(x)        READ_COMPONENT_HEADER(x), COMPONENT_ADVANCE(x, 4)
#define COMPONENT_END(x)          x = (componentStart + *(int *)componentStart)
```

A component is therefore:

```
+0x00  u32  component_size   (INCLUDES these 4 bytes)
+0x04  ...  payload
```

`COMPONENT_END` always jumps to `componentStart + component_size`, regardless
of how far the payload was actually walked — so a parser can read a count and a
few pointers and trust the size word to skip the rest. This is the single most
important invariant for anyone rewriting or relocating level data: **every
component is self-sizing and self-skipping.**

### Pointer fix-up

Pointers inside a component are stored **relative to the payload start**
(`componentStart + 4`) and patched to absolute at load:

```c
#define PATCH_POINTER_RELATIVE_TO_COMPONENT(dest)   // loaders.c:64
    PATCH_POINTER(*(int *)dest, componentStart + 4), dest += 4
// PATCH_POINTER(d,base): d = base + d
```

A handful of sites instead patch relative to `sceneBase` (the start of the
whole scene blob) — noted inline where they occur. **Implication for openpete:
once a level blob is loaded and patched, its internal pointers are absolute
PS1 addresses into `g_ram`.** Any host-side scheme that relocates or
double-buffers level data must re-run the equivalent fix-up or keep the blob at
its original address (see `level-modding.md` and the RAM-virtualization plan).

## 4. `LoadLevelData` — the environment payload

Parsed in order (`loaders.c:73`). Each bullet is one component unless noted.
State lands in `g_Environment`, `g_NewCyclorama`, `g_Portals[]`, `g_Spu`.

1. **Texture table.** `u32 count`, then `count` × 16-byte texture descriptors.
   `g_Environment.m_LQTexturePointer` points at the table start (LQ tier);
   `m_HQTexturePointer` at `start + 16*count` (HQ tier). 16 bytes/entry, LQ in
   bytes `[0..7]` and HQ in `[8..15]` — this is the paired LOD-tier table; the
   renderer's `+8` bump selects HQ. (Layout detailed in §7.)
2. **Environment sectors.** `u32 m_SectorCount`, then an array of
   relative pointers (one per sector), each patched to absolute. The sectors
   themselves are the **world geometry** — see §6. The decomp types
   `m_SectorPointer` as `void *`; SpyroEdit decodes the interior.
3. **Occlusion data** *(skipped for cutscenes)*. PVS-style visibility groups
   for both the environment and the cyclorama. Guarded: if the component is
   ≤4 bytes the level ships empty occlusion (e.g. Terrace Village). Otherwise
   two sub-blocks — environment occlusion groups, then cyclorama occlusion
   groups — each a count + patched relative pointers.
4. **Special surfaces** *(non-cutscene)*. `u32 m_SurfaceCount` + pointer array.
   Each `SpecialSurface { u8 m_Type; int m_Param1, m_Param2; }` tags terrain
   triangles with behaviour (lava, ice, supercharge ramps, etc.). Surface flags
   from the last height query surface in `g_SurfaceBelowFlags`.
5. **Collision** *(non-cutscene)*. A single `TerrainCollision` struct (§8) with
   five internal pointers fixed up in place.
6. **Cyclorama** (skybox). `Color m_BackgroundColor` (3 bytes, 4-byte aligned),
   then `u32 m_SectorCount` + patched sector pointer array. Stored in
   `g_NewCyclorama`.
7. **Portals** *(non-cutscene)*. `u32 g_PortalCount`, then per portal a
   `Portal` (`cyclorama.h`) whose trailing `m_Points[]` array is immediately
   followed by an embedded per-portal skybox (`PortalDataSky`). The decomp
   itself flags this layout as "so weird" — the skybox pointer is computed as
   `&m_Points[m_PointCount-1]`, deliberately overlapping the last point slot.
8. **Particle textures.** `u32 count`, then variable-length records keyed by a
   `short` id into `D_80076278[]`; each record self-sizes via a `short` length
   at `+2`.
9. **Sound table.** `u32 size` + the SPU sound table, installed via
   `SetNewSoundTable(..., patch=1)`. Trailing 4-byte alignment.

`LoadLevelData` returns a pointer to the first free byte after the sound table
— this becomes `g_Buffers.m_ModelData`, where stage 9 loads model data.

## 5. `LoadLevelScene` — objects, sprites, and animations

Parsed from `g_Buffers.m_LevelScene` (`loaders.c:384`). Begins with a fixed
header, then a run of animation components, then object placement.

### `LevelSceneHeader` (fixed, at offset 0)

```c
typedef struct {                 // loaders.c:373
  Vector3D  m_StartingPosition;  // Spyro's spawn point
  Vector3D8 m_StartingRotation;  // packed 8-bit euler
  Tiledef   m_FlameTexture;      // HUD/sprite UVs follow (see Tiledef, §7)
  Tiledef   m_ShadowTexture;     // moby drop-shadow
  Tiledef   m_UnusedTexture;     // skipped — not stored anywhere
  Tiledef   m_OrbAndEggSprite[10];
  Tiledef   m_SuperFlameTexture;
  Tiledef   m_SpecularMetalTexture;
} LevelSceneHeader;
```

The spawn position seeds `g_Checkpoint.m_StartingPosition`; flight levels
(`g_LevelId % 10 == 5`) and level transitions adjust it afterwards.

### Animation components (each `u32 count` + patched pointer array)

In this exact order (the renderer relies on it):

1. Texture animations — textures that borrow another texture's UVs
   (supercharge ramps).
2. Scrolling textures — UVs that scroll per frame (waterfalls).
3. High-poly geometry animations (moving platforms, etc.).
4. Low-poly geometry animations.
5. Collision animations — mirror the geometry anims on the collision mesh.
6. High-color animations.
7. Low-color animations.

Counts/pointers land in `g_EnvironmentAnimations` (`environment.h`). When loaded
without the `pAnimationsAndSparx` flag (e.g. a cutscene reload), the counts are
zeroed so nothing animates. Note the decomp comments that `section7`/`section8`
in that struct are present but unreferenced — dead slots in the format.

### Object placement (the "mobys")

After the animations:

- **Level mobys.** `u32 count`, then `count` × `Moby` (0x58 bytes each, §9).
  `g_LevelMobys` points at the array; entry `[count]` gets `m_State = 0xFF` as a
  terminator. The dynamic-moby allocator (`g_DynMobys`, `g_MobyAllocPtr`) is
  seeded just past it, and a props region (24 bytes/dynamic moby) follows.
- **Moby pods.** `u32 m_MobyPodCount` + patched pointer array (`g_MobyPods`).
- **Moby collision chain.** A region used to thread mobys into spatial
  collision buckets (`g_MobyCollisionChain`).
- **Moby pointers.** A fix-up table: each entry is an offset (relative to
  `sceneBase`) of a pointer that itself needs patching relative to `sceneBase`.
- **Demo data** (optional). In demo mode the post-scene bytes are a recorded
  input stream; `g_DemoDataPtr` points at them.

The loader then walks every moby to init shadows/shading, reconcile the
killed-moby bitmask against the save (`g_Checkpoint.m_KilledMobysSaved`), reset
particles, and spawn Sparx.

## 6. World geometry: the environment sector interior

This is the payload the decomp leaves as `void *` and SpyroEdit decodes
(`SpyroScene.h`, `SceneSectorHeader`). A sector is a self-describing block of
two LOD tiers (low-poly "LP" and high-poly "HP") of vertices, colours, and
faces:

```c
struct SceneSectorHeader {           // SpyroEdit
  uint16 centreY, centreX;
  uint16 centreRadiusAndFlags;       // flags in the top nibble (>>12); bit 0 = "flat" sector
  uint16 centreZ;
  uint32 xyPos, zPos;                // sector world origin; low 13 bits of zPos = flist id
  uint8  numLpVertices, numLpColours, numLpFaces, lpUnknown;
  uint8  numHpVertices, numHpColours, numHpFaces, hpUnknown;
  uint32 zTerminator;
  uint32 data32[...];                // packed: LP verts | LP colours | LP faces |
                                     //         HP verts | HP colours | HP faces
};
```

Sub-array offsets are computed by accumulation (`GetLpVertices`/`GetHpFaces`
etc.); total size:

```
size_bytes = (7 + numLpVertices + numLpColours + numLpFaces*2
                + numHpVertices + numHpColours*2 + numHpFaces*4) * 4
```

### Vertex packing (Spyro 1 differs from 2/3)

Each vertex is a `uint32` of packed signed offsets from the sector origin.
SpyroEdit's `GetHpVertexAbsolutePositions`:

```c
x = sectorX + ((v >> 19 & 0x1FFC) >> 2);
y = sectorY + ((v >>  8 & 0x1FFC) >> 2);
z = sectorZ + ((v <<  3 & 0x1FFC) >> 2);   // 2/3
if (game == SPYRO1) z = sectorZ + ((v << 3 & 0x1FFC) >> 3);   // SPYRO1: extra >>1 on Z
if (isSectorFlat)  z >>= 3;                 // "flat" sectors compress Z
```

The **SPYRO1 Z scale is half that of Spyro 2/3** — a concrete gotcha for any
geometry reprojection (widescreen, interpolation) that borrows Spyro-2/3
tooling assumptions.

### Faces (`SceneFace`, 16 bytes) — Spyro 1 bitfields

```c
struct SceneFace { uint32 word1, word2, word3, word4; };  // verts in word1, colours in word2
```

SpyroEdit's accessors are explicitly forked for Spyro 1:

| Field   | Spyro 2/3            | **Spyro 1**                       |
|---------|----------------------|-----------------------------------|
| texture | `word4 & 0x7F`       | `word3 & 0x7F`                    |
| flip    | `(word4 >> 10) & 1`  | `(word4 >> 1) & 1`                |
| depth   | —                    | `word4 >> 3 & 0x1F` (SPYRO1 only) |
| edges   | bitpacked in word3/4 | **no SPYRO1 decode provided**     |

The missing Spyro-1 edge decode is a known gap; if openpete ever needs edge
adjacency (e.g. for crack-free reprojection), this is unsolved territory — fall
back to the decomp renderer's actual triangle emission.

## 7. Textures

### `Tiledef` (decomp, `common.h`) — a UV quad + CLUT/TPage

```c
typedef struct {
  union { struct { u_char u0, v0; u_short clut;  } s; int all; } uv0;
  union { struct { u_char u1, v1; u_short tpage; } s; int all; } uv1;
} Tiledef;
```

This is the runtime sprite/HUD UV form (flame, shadow, orb/egg sprites). The
**on-disc texture-table** entries (the 16-byte records from `LoadLevelData` §1)
are decoded by SpyroEdit as paired LQ/HQ descriptors:

```c
struct TexLq { u8 xmin,ymin, palettex,palettey, xmax,ymax, region, alphaEtc; };  // 8 bytes
struct TexHq { u8 xmin,ymin; u16 palette; u8 xmax,ymax, region, unknown; };       // 8 bytes
struct Tex   { TexLq lqData[2]; TexHq hqData[4]; };
```

`region` indexes a 128px-wide VRAM band; `GetXMin = region*128 % 2048 + xmin`,
`GetYMin = (region & 0x1F)/16 * 256 + ymin`. This matches the decomp loader's
treatment of `m_LQTexturePointer` as a 16-byte/entry paired table with the HQ
tier at byte offset `[8..15]` (the renderer's `+8` bump is the LOD-tier select).

VRAM itself is a 512×512×2 image loaded wholesale in `LoadLevel` stage 5; the
texture table is just UV/CLUT bookkeeping into it.

## 8. Collision (`TerrainCollision`, `environment.h`)

```c
typedef struct {
  int      m_TriangleCount;
  int      m_FlagCount;
  u_short *m_BlockTree;       // spatial accel (BVH/grid) over blocks
  u_short *m_Blocks;          // triangle index lists per block
  void    *m_Triangles;       // packed collision triangles
  u_char  *m_TriangleOcclusionAssignment;
  u_char  *m_Flags;           // per-triangle surface flags
} TerrainCollision;
```

All five pointers are relative-to-component and fixed up in place at load
(`loaders.c`, collision component). The triangle interior type is still `void *`
in the decomp — but **this is "not yet typed", not "unknown".** The entire
collision module is still MIPS assembly (`spyro-1/asm/collision.s`, ~4190 lines,
unmatched); the queries that read the triangles — `func_8004BE4C` (sphere),
`func_8004AE38` (segment), `func_8004D5EC` (floor-below), `func_8004E3C8` (moby)
— live there as `glabel`s, not in `src/`. (`collision.h` only declares them.)
The exact triangle byte layout is therefore fully determined and recoverable by
reading how `func_8004BE4C` walks `m_BlockTree → m_Blocks → m_Triangles`; it
simply hasn't been hoisted into a named struct because the reader code isn't
decomped yet. SpyroEdit's signature-scanned `spyroCollision` is only partially
typed for Spyro 1, so **the recovery path is the decomp asm, not SpyroEdit.**
This is the least-*typed* part of the format, but unlike the SPYRO1 edge decode
(§6) it is not missing a source.

## 9. Object instances (`Moby`, 0x58 bytes)

The decomp's authoritative struct is `spyro-1/include/moby.h`. SpyroEdit's
`Moby` (`SpyroData.h`) is a compatible, partially-named view — useful for the
field offsets it *does* name:

```c
struct Moby {                 // 0x58 bytes
  int32 extraData;            // 0x00
  SpyroPointer<Moby> nextCollisionChain; // 0x04
  int32 collisionData;        // 0x08
  int32 x, y, z;              // 0x0C  world position
  uint32 attackFlags;         // 0x18
  ...
  int16 collisionRegion;      // 0x34  (x>>13) + ((y>>13)<<5) — 32x32 spatial grid
  int16 type;                 // 0x36  moby class / behaviour selector
  Animation anim;             // 0x3C  {prevAnim,nextAnim,prevFrame,nextFrame}
  uint8 animProgress;         // 0x40
  Angle angle;                // 0x44  packed int8 x,y,z
  int8  state;                // 0x48
  int8  distDraw, distUnknown, distHp, scaleDown;  // 0x4C LOD distances (default scaleDown=0x20)
  int8  levelSectorIndex;     // 0x52
  uint32 colour;              // 0x54
};
```

`type` (the decomp calls it `m_Class`) indexes `g_Models[]`; the model itself
(`Model`/`AnimationHeader` in `moby.h`) carries animation frames, LP/HP faces,
collision sub-models, and sound ids. The **per-level code overlay** supplies
the *behaviour* for that level's moby classes — see `level-overlays.md`.

`SetPosition` shows the collision-grid rethreading the engine does on movement:
moving a moby across a 13-bit grid boundary unlinks it from its old
`mobyCollisionRegions[]` chain and relinks it into the new one. Anything in
openpete that teleports or interpolates object positions must respect this if it
writes back into game state (it must not, for presentation-only interp — that's
the whole point of the transform-interp render-layer split).

## 10. Where this matters for openpete

| Concern | Relevant section |
|---|---|
| Widescreen geometry reprojection | §6 vertex packing (note SPYRO1 Z half-scale) |
| `--no-lod` / LOD control | §1 paired LQ/HQ table, §6 LP/HP sector tiers, §9 moby `distHp` |
| Host-side polybuf redirect | §3 component fix-up (pointers become absolute g_ram addrs) |
| RAM virtualization / relocation | §3 pointer fix-up must be replayed or addresses preserved |
| Transform interpolation | §9 moby positions (render-layer only, never write game state) |
| Custom / edited levels | entire doc; SpyroEdit is the working-editor reference |
| CD drive simulation | §2 the 14-stage async load is what the CD sim must satisfy |

See `level-overlays.md` for the code side and `level-modding.md` for how the
above maps onto specific `notes/plans/openpete-*` plans.
