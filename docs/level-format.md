# Spyro 1 level data format

How Spyro the Dragon (NTSC-U, SCUS-94228) stores and loads level data. openpete
needs this format for widescreen geometry reprojection, LOD control, native
rendering, custom-level support, and runtime modding.

## Sources and trust level

Three sources, in descending order of authority:

- **The matching decomp** (`spyro-1/src/loaders.c`, `spyro-1/include/*.h`). The
  actual game loader, recompiled byte-for-byte. Where it parses a field, that
  parse is ground truth. It types several geometry payloads `void *` and walks
  past them without interpreting their interior.
- **openpete's native walks**, which decode exactly the interiors the decomp
  leaves opaque, verified field-by-field against the handwritten renderer asm
  (`spyro-1/asm/renderers/r_environment.s`) and A/B-checked against the prims
  the game itself emits: `games/spyro-1/features/terrain_native.c` (environment
  sectors, faces, textures), `games/spyro-1/features/moby_native.c` (moby
  instances), `games/spyro-1/features/cyclorama_native.c` (sky sectors). The
  full renderer contract map is the format notes.
- **SpyroEdit** (`../SpyroEdit/Source/SpyroScene.{h,cpp}`, `SpyroData.h`), an
  emulator-plugin editor primarily for Spyro 2/3 with partial Spyro 1 support.
  Corroboration only. Its Spyro-1 sector-header count word and its 11/11/10
  vertex bit positions hold; its `SceneFace` accessors, its vertex Z scaling,
  its texture-record structs, and its `Moby` field offsets do not (each
  contradiction is named below).

Terminology: SpyroEdit calls the world geometry the "scene". The decomp uses
"scene" for a different blob (§5) and calls world geometry the environment.
This doc uses the decomp's names.

## 1. On-disc layout: the WAD

Level data lives in `WAD.WAD`, indexed by a fixed header struct read into RAM as
`g_WadHeader` (`spyro-1/include/wad.h`). The header is the first 2048 bytes of
`WAD.WAD`, at disc LBA 37 (`g_CdState.m_WadSector`); every entry is an
`OffsetLength { int m_Offset; int m_Length; }` giving a byte offset into the WAD
and a length, and every offset is sector-aligned, so a read resolves to LBA
`37 + m_Offset/2048` (`games/spyro-1/recomp_extract.cpp`, `kWadSector`).

Byte offsets into the header, in `wad.h` field order:

```
  0  m_UniversalLogo          8  m_wad1 (titlescreen logo/text graphics)
 16  m_TitleScreenOverlay    24  m_CutsceneData[4]
 56  m_GameOverSkybox        64  m_PETE
 72  m_LevelEntry[36] { OffsetLength m_Overlay; OffsetLength m_Data; }
648  m_CreditsOverlay       656  m_Credits1Data[10]  736  m_Credits2Data[10]
```

The per-level pair is the spine. `m_Overlay` is executable code (moby/particle
behaviour for that level); `m_Data` is the geometry/texture/object-placement
payload described here. They load independently, to different RAM regions. See
`level-overlays.md` for how the two interact.

`m_LevelEntry` is indexed by `g_LevelIndex`, computed from the logical level id
(`loaders.c`, `LoadLevel` case 2):

```c
g_Homeworld  = (g_LevelId / 10) - 1;
g_LevelIndex = (g_Homeworld * 6) + (g_LevelId % 10);
```

The last of the 36 slots is empty.

## 2. The level Data blob and its sub-header (`LevelHeader`)

Stage 3 reads 0x800 bytes from the head of the Data entry; the first 464 bytes
(`sizeof(LevelHeader)`, `wad.h`) are copied into `g_LevelHeader`. Every offset
in it is relative to the start of the Data entry.

```c
typedef struct {
  int m_VramSramOffset, m_VramSramSize;    // 512x512x2 VRAM image, then SPU audio
  int m_DataOffset,      m_DataSize;       // environment payload -> LoadLevelData()
  int m_ModelDataOffset, m_ModelDataSize;  // moby/animation models
  int m_SceneOffset,     m_SceneSize;      // object placement + anims -> LoadLevelScene()
  OffsetLength m_Dragons[6];               // dragon cutscene data
  int   m_ModelOffsets[64];                // per-model byte offsets into the Data entry
  short m_ModelIndices[64];                // which g_Models[] slot each model fills
} LevelHeader;
```

`LoadLevel` (`loaders.c`) is a 14-stage state machine driven by async CD reads.
Stages above 1 gate on `CdSync` before running.

| Stage | Action |
|------:|--------|
| 0     | Copy the current cyclorama and the sound table into free memory below `g_Buffers.m_LowerPolyBuffer` so they survive the transition |
| 1     | Alternative entry: drop the cyclorama immediately instead of preserving it |
| 2     | Set `g_LevelId`/`g_LevelIndex`, call `SetOverlayPointers`, load the level overlay from `m_Overlay` |
| 3     | Read 0x800 bytes from the head of `m_Data` |
| 4     | Copy out the `LevelHeader`, start loading the 512×512×2 VRAM image |
| 5     | `LoadImage` it into VRAM in two 512×256 halves, then load the SPU block |
| 6     | `SpuSetTransferStartAddr(0x1010)` + `SpuWrite`. The size passed is the constant `0x80000 - 0x1010`, not `m_VramSramSize - 0x80000`; the decomp flags this as a game bug |
| 7     | Wait for `SpuIsTransferCompleted` |
| 8     | Load the environment payload (`m_DataOffset`/`m_DataSize`) |
| 9     | `LoadLevelData()` parses it and returns the first free byte, which becomes `g_Buffers.m_ModelData`; publish `g_Cyclorama`; load model data there |
| 10    | `PatchInSpyroAnimations`, `PatchMobyModelPointers`, register models in `g_Models[]`, place `g_Buffers.m_LevelScene` after the model data |
| 11    | Load the scene payload (`m_SceneOffset`/`m_SceneSize`) |
| 12    | Wait for camera/portal to settle (transitions only) |
| 13    | Set music track and visited flag, clear the checkpoint, then `LoadLevelScene()` |

The two parsers that decode level structure are `LoadLevelData`
(environment/world) and `LoadLevelScene` (objects and animations).

## 3. The component container format

Both parsers consume a stream of length-prefixed components. The decomp
expresses this with four macros (`loaders.c`, above `LoadLevelData`):

```c
#define READ_I32(x)               *(int *)x, x += 4       // read+advance
#define READ_COMPONENT_HEADER(x)  componentStart = (char *)x
#define COMPONENT_ADVANCE(x, y)   x += y
#define COMPONENT_START(x)        READ_COMPONENT_HEADER(x), COMPONENT_ADVANCE(x, 4)
#define COMPONENT_END(x)          x = ((char *)componentStart + *(int *)componentStart)
```

A component is:

```
+0x00  u32  component_size   (INCLUDES these 4 bytes)
+0x04  ...  payload
```

`COMPONENT_END` jumps to `componentStart + component_size` regardless of how far
the payload was walked, so a parser can read a count and a few pointers and let
the size word skip the rest. Every component is self-sizing and self-skipping,
which is what lets `tools/extract_wad_entry.py` reach the cyclorama background
colour by skipping five components blind.

Two sites in the format break the pattern. The portal block (§4) is a bare
count, not a sized component. The moby-pointer fix-up table (§5) opens with a
sized-component header but uses the leading word as an entry count and never
calls `COMPONENT_END`.

### Pointer fix-up

Pointers inside a component are stored relative to the payload start
(`componentStart + 4`) and patched to absolute at load:

```c
#define PATCH_POINTER_RELATIVE_TO_COMPONENT(dest)
    PATCH_POINTER(*(int *)dest, componentStart + 4), dest += 4
// PATCH_POINTER(d, base): d = base + d          (common.h)
```

Three sites use a different base: the cyclorama occlusion groups patch relative
to `componentStart + *(int *)(componentStart + 4) + 4`, the moby pods patch
relative to `componentStart` itself, and the moby-pointer table patches relative
to `sceneBase` (the start of the whole scene blob).

Once a level blob is loaded and patched, its internal pointers are absolute PS1
addresses into `g_ram`. Any host-side scheme that relocates or double-buffers
level data must re-run the equivalent fix-up or keep the blob at its original
address.

## 4. `LoadLevelData` — the environment payload

Parsed in order. Each numbered item is one component unless noted. State lands
in `g_Environment` (`environment.h`), `g_NewCyclorama`, `g_Portals[]`, `g_Spu`.

1. **Texture table.** `u32 count`, then `count` × 16-byte entries.
   `m_LQTexturePointer` points at the table start; `m_HQTexturePointer` is set
   to `start + 16*count`, the byte after it. The two pointers are not quality
   tiers of one table and are not parallel-indexed; see §7.
2. **Environment sectors.** `u32 m_SectorCount`, then one relative pointer per
   sector, each patched to absolute. The sectors are the world geometry (§6).
   The decomp types `m_SectorPointer` as `void *`.
3. **Occlusion data** (skipped for cutscenes). Visibility groups for the
   environment and the cyclorama. If the component size word is ≤ 4 the level
   ships empty occlusion and all three group pointers/counts are zeroed (some
   levels, including Terrace Village, emit the component with zero counts
   rather than omitting it). Otherwise: `u32` size of the environment portion,
   `u32` group count, patched pointer array; then the cyclorama block at
   `componentStart + envPortionSize + 4`, same shape.
4. **Special surfaces** (non-cutscene). `u32 m_SurfaceCount` + patched pointer
   array. `SpecialSurface { u_char m_Type; int m_Param1, m_Param2; }`
   (`environment.h`) tags terrain with behaviour (lava, ice, supercharge ramps).
   The surface flags of the last height query land in `g_SurfaceBelowFlags`.
5. **Collision** (non-cutscene). One `TerrainCollision` struct (§8), five
   internal pointers fixed up in place relative to the struct's own address.
6. **Cyclorama** (skybox). `Color m_BackgroundColor` (3 bytes, then advance 4),
   `u32 m_SectorCount`, patched sector pointer array. Stored in
   `g_NewCyclorama`; stage 9 copies it to `g_Cyclorama`.
7. **Portals** (non-cutscene, and not a component). `u32 g_PortalCount`, then
   per portal a `Portal` (`cyclorama.h`). The loader sets
   `m_Skybox = &m_Points[m_PointCount - 1]`, deliberately overlapping the last
   point slot, advances by `sizeof(Cyclorama)` (the decomp notes this size is
   wrong for the actual type), then parses a nested sized component holding the
   portal's own sky: `copyBgColor`, `copySectorCount`, patched sector pointers.
   The overlapping-storage shape is why `PortalDataSky` (`cyclorama.h`) carries
   both `copy*` fields and a second `m_BackgroundColor`/`m_SectorCount` pair.
8. **Particle textures.** `u32 count`, then variable-length records. Each record
   starts with a `short` id, which indexes `D_80076278[]`, and self-sizes via a
   `short` length at `+2`: the walk advances `*(short *)(p + 2) + 4`.
9. **Sound table.** `u32 size` + the SPU sound table, installed via
   `SetNewSoundTable(..., pPatchAddressesInTable=1)`, which adds the 0x1010 SPU
   base to every `SoundDefinition::m_Addr`. Trailing 4-byte alignment.

`LoadLevelData` returns the first free byte after the sound table; stage 9 makes
that `g_Buffers.m_ModelData`.

## 5. `LoadLevelScene` — objects, sprites, and animations

Parsed from `g_Buffers.m_LevelScene`. Begins with a fixed header, then seven
animation components, then object placement.

### `LevelSceneHeader` (fixed, at offset 0)

```c
typedef struct {                 // loaders.c, above LoadLevelScene
  Vector3D  m_StartingPosition;  // Spyro's spawn point
  Vector3D8 m_StartingRotation;  // packed 8-bit euler; only .z is read
  Tiledef   m_FlameTexture;      // -> g_SpyroFlame.m_FlameTexture
  Tiledef   m_ShadowTexture;     // -> g_MobyShadows.shadow
  Tiledef   m_UnusedTexture;     // skipped
  Tiledef   m_OrbAndEggSprite[10];  // -> g_Hud.m_OrbAndEggSprite[0..9]
  Tiledef   m_SuperFlameTexture; // -> g_SpyroFlame.m_SuperFlameTexture
  Tiledef   m_SpecularMetalTexture; // -> D_800756F0
} LevelSceneHeader;
```

The spawn position seeds `g_Checkpoint.m_StartingPosition` unless the save has
`m_StoodOnCheckpoint`. Flight levels (`g_LevelId % 10 == 5`) and level
transitions adjust it afterwards.

### Animation components

Seven components, each `u32 count` + patched pointer array, in this order:

1. Texture animations — textures that borrow another texture's UVs (supercharge
   ramps).
2. Scrolling textures — UVs that scroll per tick (waterfalls).
3. High-poly geometry animations (moving platforms).
4. Low-poly geometry animations.
5. Collision animations — mirror the geometry anims on the collision mesh.
6. High-colour animations.
7. Low-colour animations.

Counts and pointers land in `EnvironmentAnimations g_EnvironmentAnimations`
(`environment.h`), whose field order does not match the on-disc order. That
struct also declares `section7Count`/`section7` and `section8Count`/`section8`,
which nothing reads or writes: dead slots.

`LoadLevelScene(pAnimationsAndSparx)` calls `func_8002B4AC` to initialise the
animations when the flag is set. When it is clear (a cutscene reload) it zeroes
five of the seven counts — texture, scrolling, collision, high-colour, and
low-colour. The two geometry-animation counts are left standing.

### Object placement (the mobys)

- **Level mobys.** A component holding `u32 count` then `count` × `Moby` (0x58
  bytes each, §9). `g_LevelMobys` points at the array. `g_DynMobys` and
  `g_MobyAllocPtr` are seeded at `&g_LevelMobys[count]`, and that entry's
  `m_State` is set to 0xFF as the walk terminator. The props region runs from
  the component's end to `g_DynMobySpaceEnd`, at 24 bytes per dynamic moby, so
  `g_DynMobyMax = (spaceEnd - g_DynMobys) / (sizeof(Moby) + 24)`. The props
  region is skipped by adding its own leading size word.
- **Moby pods.** `u32 m_MobyPodCount` + pointer array (`g_MobyPods`), patched
  relative to `componentStart`.
- **Moby collision chain.** A region threading mobys into spatial collision
  buckets (`g_MobyCollisionChain`); the loader only records its address.
- **Moby pointers.** A fix-up table: each 4-byte entry is an offset relative to
  `sceneBase` naming a pointer that is itself patched relative to `sceneBase`.
  The loop bound is the component's leading word used as an entry count, and no
  `COMPONENT_END` follows.
- **Demo data** (demo mode only). The bytes after the fix-up table are a
  recorded input stream; `g_DemoDataPtr` points at them for playback. In record
  mode the pointer is the fixed address 0x80600000 instead, which needs 8 MB.

The loader then walks every moby to init shadow/shading, reconciles the
killed-moby bitmask against `g_Checkpoint.m_KilledMobysSaved`, resets particles,
and spawns Sparx.

## 6. World geometry: the environment sector interior

Each sector is 0x1C bytes of header followed by a packed data block. Verified
against `r_environment.s` and decoded by
`games/spyro-1/features/terrain_native.c` (`tsc_decode_verts`,
`tsc_decode_faces`).

| Off | Contents |
|---|---|
| +0x00 | `u16 centreY` (low) ‖ `u16 centreX` (high) |
| +0x04 | `u16 (flags & 0xE000 \| radius & 0x1FFF)` (low) ‖ `u16 centreZ` (high) |
| +0x08 | `u16 originY` (low) ‖ `u16 originX` (high) — sector origin, world/16 |
| +0x0C | `u16 flistId` (low) ‖ `u16 originZ` (high) |
| +0x10 | LP count word `{b0 numLpVertices, b1 numLpColours, b2 numLpFaces, b3 unused}` |
| +0x14 | HP word `{b0 numHpVertices, b1 numHpColours, b2 numHpFaces, b3 hpVertsOffset}` |
| +0x18 | draw-flags / anim-control word; also read as four anim sentinel bytes |
| +0x1C | data block |

`centre*` and `radius` are the bounding sphere the frustum cull uses. The flag
field is the top 3 bits of the halfword at +0x04, not a nibble; it selects
queue and tier, not geometry. `(flags & 0x4000) != 0` marks a sector with no HP
data: those draw LP only, and walking them as HP reads garbage.

`hpVertsOffset` (byte 3 of +0x14, which SpyroEdit calls `hpUnknown`) is the
precomputed byte offset ÷ 4 from `sector + 0x1C` to the HP vertex section, equal
to `numLpVertices + numLpColours + numLpFaces*2` words.

Data block layout and size:

```
+0x1C: LP verts   [numLpVertices  x  4B]
       LP colours [numLpColours   x  4B]
       LP faces   [numLpFaces     x  8B]
       HP verts   [numHpVertices  x  4B]   (== +0x1C + hpVertsOffset*4)
       HP colours [numHpColours   x  8B]   (near+far pair)
       HP faces   [numHpFaces     x 16B]

size_bytes = (7 + numLpVertices + numLpColours + numLpFaces*2
                + numHpVertices + numHpColours*2 + numHpFaces*4) * 4
```

### Vertex packing

One `uint32` per vertex, same layout for both tiers: X = bits 31..21, Y = bits
20..10, Z = bits 9..0.

```c
x = (v >> 21) & 0x7FF;
y = (v >> 10) & 0x7FF;
z =  v        & 0x3FF;
```

Proven by the animation path's re-pack `(X << 21) | (Y << 10) | Z`. The offsets
apply to the sector origin with **X added, Y and Z subtracted**
(`terrain_native.c`, the `tgpu` bound helper and the debug decode both apply
`ox + x`, `oy - y`, `oz - z`).

Two SpyroEdit claims are false here. There is no extra `>>1` on Z for Spyro 1:
Z is simply a 10-bit field, half the range of X/Y at the identical per-LSB
scale. There is no flat-sector `z >>= 3`: no flag in the renderer shifts a
vertex axis.

The LP and HP walks feed the GTE at different fixed-point scales (LP world/16,
HP world/4, the same 11-bit field shifted `<<2`), which changes only integer
truncation, not the world position. A float renderer needs one scale.

### Faces

LP and HP faces are different records, not one type.

**LP face, 8 bytes.** `word0` = four 6-bit vertex indices at shifts 23/17/11/5
(`(w0 >> s) & 0x1F8`, pre-scaled ×8); `word0 & 0x1F` is the authored LP
admission distance and `word0 & 0x80` marks double-sided. `word1` = four 6-bit
colour indices at shifts 24/18/12/6 (`& 0xFC`); `(word1 >> 3) & 0x1F` is the OT
depth bias; `word1 & 0x4` enables semi-transparency with ABR = `word1 & 3`. LP
faces carry no texture word and always draw flat-gouraud.

**HP face, 16 bytes.** `word0` = four 8-bit vertex indices; `word1` = four 8-bit
colour indices; `word2` = the texture word; `word3` = flags.

| Field | Location | Note |
|---|---|---|
| texture index | `word2 & 0x7F` | `(int8)word2 == -1` (0xFF) means untextured |
| semi-transparent | `word2 & 0x80` | blend mode comes from the tpage ABR bits |
| sub-tile rotation | `(word2 >> 8) & 3` | |
| flip | `word3 & 0x2` | negates the NCLIP result; does not reorder vertices |
| fog bias | `word3 & 0x1` | adds 0x2000 to the fog reference |
| OT depth bias | `(word3 >> 3) & 0x7` | 3 bits, not 5 |

A face is a triangle when index2 == index3, otherwise a quad.

SpyroEdit's Spyro-1 `SceneFace` accessors agree on texture (`word3 & 0x7F` in
its 1-based word naming) and flip, but its depth field `word4 >> 3 & 0x1F` is
the LP mask applied to an HP face: the HP field is 3 bits wide. Its edge
accessor has no Spyro-1 implementation, and none has been derived here either.
Code needing edge adjacency has no source and must fall back to the renderer's
triangle emission.

## 7. Textures

### `Tiledef` (`common.h`) — a UV pair + CLUT/TPage

```c
typedef struct {
  union { struct { u_char u0, v0; u_short clut;  } s; int all; } uv0;
  union { struct { u_char u1, v1; u_short tpage; } s; int all; } uv1;
} Tiledef;
```

8 bytes. `decode_tiledef` (`terrain_native.c`) unpacks it as
`tpx = (tpage & 0xF) * 64`, `tpy = ((tpage >> 4) & 1) * 256`,
`bpp` from `(tpage >> 7) & 3` (0→4, 1→8, else 16),
`clx = (clut & 0x3F) * 16`, `cly = (clut >> 6) & 0x1FF`, ABR = `(tpage >> 5) & 3`.
The four spatial corners of a 32×32 tile are `(u0,v0)`, `(u1,v1)`,
`(u0, v0+0x1F)`, `(u0+0x1F, v0+0x1F)` in TL/TR/BL/BR order.

### The two texture pointers

`m_LQTexturePointer` and `m_HQTexturePointer` are different indexing strategies
for different cell categories, not quality tiers of one table. The loader's
`HQ = LQ + 16*count` locates the second table's base; it does not make the two
parallel-indexed, and the same texture byte names a different texture in each.

- `m_LQTexturePointer`: 16 bytes per entry, indexed `texByte * 16`, used by the
  main HP walk for the bulk of terrain. Each 16-byte entry holds **two 8-byte
  `Tiledef` halves**: `[0..7]` is the far/low-detail tile (typically bpp4),
  `[8..15]` the near/high-detail tile (typically bpp8). The renderer's
  distance-driven `+8` bump selects the near half; `--no-lod` saturates
  `m_LodDistance` so the near half is used everywhere.
- `m_HQTexturePointer`: a **168-byte record per cell**, indexed
  `(word2 & 0x7F) * 168`, holding 21 8-byte `Tiledef`s. `func_800276A0` (a
  fall-through pass after the main environment renderer) splits a near cell 2×2
  and textures the four sub-quads from entries `(i+1)*8` of that record —
  offsets 8/16/24/32, since entry 0 is the detail-path base.

Cells with higher curvature use a finer-than-2×2 split driven by a per-vertex
detail-bit scratch and a 12-byte pattern table (`D_8006CF98`). That subdivision
is not decoded; openpete's native near tier approximates it with the 2×2 split.

SpyroEdit's `TexLq`/`TexHq`/`Tex` structs and its `region`-band VRAM arithmetic
describe the Spyro 2/3 texture cache. They do not describe these records: the
composite `Tex` is 48 bytes against a 16-byte on-disc entry, and the Spyro 1
entry carries a PSX CLUT/TPage pair, not palette and band coordinates.

VRAM itself is a 512×512×2 image loaded wholesale in `LoadLevel` stages 4-5; the
texture tables are UV/CLUT bookkeeping into it.

## 8. Collision (`TerrainCollision`, `environment.h`)

```c
typedef struct {
  int      m_TriangleCount;
  int      m_FlagCount;
  u_short *m_BlockTree;                     // spatial acceleration over blocks
  u_short *m_Blocks;                        // triangle index lists per block
  void    *m_Triangles;                     // packed collision triangles
  u_char  *m_TriangleOcclusionAssignment;
  u_char  *m_Flags;                         // per-triangle surface flags
} TerrainCollision;
```

All five pointers are patched relative to the struct's own address at load.

The triangle interior is not typed, but it is recoverable rather than unknown.
The collision module is still handwritten MIPS (`spyro-1/asm/collision.s`, 4190
lines, unmatched); `collision.h` declares the queries that read the triangles
and the bodies live in that file as `glabel`s: `func_8004BE4C` (sphere),
`func_8004AE38` (segment between two points), `func_8004D5EC` (floor below),
`func_8004E3C8` (moby collision at a position), `func_8004DF24` (camera
occlusion group). The byte layout is fully determined by how `func_8004BE4C`
walks `m_BlockTree` → `m_Blocks` → `m_Triangles`; it has not been hoisted into a
named struct because the reader code is not decompiled. SpyroEdit's
signature-scanned `spyroCollision` is only partially typed for Spyro 1, so the
recovery path is the asm.

## 9. Object instances (`Moby`, 0x58 bytes)

`spyro-1/include/moby.h` is authoritative. openpete's native moby walk reads
these offsets directly (`games/spyro-1/features/moby_native.c`), which confirms
them against live data.

| Off | Field | Type | Note |
|---|---|---|---|
| 0x00 | `m_Props` | `void *` | class-specific property block |
| 0x04 | `m_CollisionChainNext` | `Moby *` | next moby in this collision bucket |
| 0x08 | `m_CollisionGroup` | `void *` | active collision group from the model |
| 0x0C | `m_Position` | `Vector3D` | 3 ints |
| 0x18 | `m_DamageFlags` | `int` | |
| 0x1C | `m_ShadowDistance` | `int` | equals the floor distance |
| 0x20 | `m_RotationMatrix` | `SHORTMATRIX` | 3×3 shorts + pad, 20 bytes |
| 0x34 | `m_CollisionRegion` | `short` | 32×32 spatial grid cell |
| 0x36 | `m_Class` | `short` | indexes `g_Models[]` |
| 0x38 | `m_FloorDistance` | `short` | |
| 0x3A | `m_DroppedFlag` | `u_char` | |
| 0x3B | `m_CollisionRange` | `u_char` | |
| 0x3C | `m_AnimationState` | `AnimationState` | 7 bytes: anim, next anim, frame, next frame, frame progress (0x40), per-frame progress, flags |
| 0x43 | `m_Pod` | `u_char` | group id; see the moby-pod list in §5 |
| 0x44 | `m_Rotation` | `Vector3D8` | 3 packed euler bytes |
| 0x47 | `m_DepthOffset` | `char` | signed OT sort bias for the whole moby |
| 0x48 | `m_State` | `u_char` | 0xFF terminates the level moby array |
| 0x49 | `m_Substate` | `u_char` | |
| 0x4A | `m_SectorIndex` | `u_char` | sector used for culling |
| 0x4B | `m_Renderer` | `u_char` | bits 0-6 render distance, bit 7 shiny renderer |
| 0x4C | `m_SpecularMetalColor[3]` | `u_char[3]` | 6 bits/channel for specular, 8 for metal |
| 0x4F | `m_SpecularMetalType` | `u_char` | shiny: 0 specular, 1/2 metal; shaded: colour index |
| 0x50 | `m_RenderRadius` | `u_char` | clipping radius >> 2 |
| 0x51 | `m_WasDrawn` | `u_char` | set by the render pass, read by gameplay next tick |
| 0x52 | `m_UpdateDistance` | `u_char` | signed on read; 0xFF = always update |
| 0x53 | `m_DropMoby` | `u_char` | ANDed with 0x7F |
| 0x54 | `m_SoundChannel` | `u_char` | |
| 0x55 | `m_SoundDistance` | `u_char` | |
| 0x56 | `m_MobyIndex` | `u_char` | spawner index, dynamic mobys only |
| 0x57 | `m_ScaleOverride` | `u_char` | 0 = regular size |

SpyroEdit's `Moby` view agrees only through 0x48. Its `distDraw/distUnknown/
distHp/scaleDown` block at 0x4C is `m_SpecularMetalColor` + `m_SpecularMetalType`;
its `levelSectorIndex` at 0x52 is `m_UpdateDistance` (the sector index is at
0x4A); and its `uint32 colour` at 0x54 is the four sound/index/scale bytes.

`m_Class` indexes `g_Models[]`; the model (`Model` / `AnimationHeader` in
`moby.h`) carries animation frames, LP/HP faces, collision sub-models, and sound
ids, and its internal pointers are patched by `PatchMobyModelPointers` at load
stage 10. The per-level code overlay supplies the behaviour for that level's
moby classes; see `level-overlays.md`.

`m_WasDrawn` is written by the stock render pass and read by gameplay on the
next tick, which makes it one of the presentation-to-gameplay channels the
oracle guards. Presentation-only interpolation of moby positions must
never write back into these bytes.

## 10. Where this matters for openpete

| Concern | Relevant section |
|---|---|
| Native terrain walk and widescreen reprojection | §6 sector header, vertex packing, face records |
| `--no-lod` and LOD control | §6 LP/HP tiers and the 0x4000 flag, §7 the `+8` Tiledef half, §9 `m_UpdateDistance` |
| Texture replacement packs | §7 both texture pointers and the CLUT/TPage decode |
| Level blob relocation or double-buffering | §3 pointer fix-up must be replayed or addresses preserved |
| Custom and edited levels | §3 self-sizing components; §4, §5 payload order |
| Level-load timing | §2 the 14-stage async state machine |
| Determinism | §9 `m_WasDrawn`, §6 the sector visibility verdict |

See `level-overlays.md` for the code side.
