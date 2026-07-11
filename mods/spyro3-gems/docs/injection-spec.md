# spyro3-gems — runtime injection spec

Mod goal: at runtime, via the mod API only (`override_addr`/`override_name` +
`guest()` memory writes + `call_addr`), (1) install converted Spyro-3 gem
models over `g_Models[83..87]`, (2) upload a Spyro-3 chime VAG into free SPU
RAM and repoint the `gemPickup` SoundDefinition, (3) recolour the gem burst
particles via `D_8006E44C[12..16]`.

All addresses are guest vaddrs. Evidence keys:

- `moby.h` = `<spyro>/spyro-1/include/moby.h`
- `loaders.c` = `<spyro>/spyro-1/src/loaders.c`
- `spu.c/.h` = `<spyro>/spyro-1/src/spu.c`, `include/spu.h`
- `moby_native.c` = `games/spyro-1/features/moby_native.c` (this repo)
- `full.c` = `games/spyro-1/generated/PSX.EXE_full.c` (recompiled .s; PC
  comments per instruction — the ground truth for the handwritten walk)
- `symbols.h` = `build/linux-release/games/spyro-1/sdk/openpete_sdk_symbols.h`

---

## A. Target model format — S1 SHADED SimpleModel (static)

### A.0 What the stock gems are

`g_Models[83..87]` (`MOBYCLASS_GEM_1..GEM_25`, moby.h:266-270) are **static
SimpleModels** rendered by the SHADED renderer `func_80022A2C`:

- Render routing is **per-moby**, not per-model: the queue builder
  `func_800521C0` (full.c:135147) reads the word at moby+0x48 and switches on
  byte +0x4B = `m_Renderer.raw` (moby.h: `m_Distance:7 | m_ShinyRenderer:1`):
  `0` → SHADED list at `0x800720F4` (G_SCRATCH+0x2400), bit7 set → shiny
  (specular/metal `func_80020F34`) list at +0x2200, else → REGULAR list
  (full.c:135168-135305; list writes at PCs 0x80052220/0x8005224C/0x80052278).
  Gems have `m_Renderer.raw == 0`. **The model swap does not touch routing.**
- `PatchMobyModelPointers` (loaders.c:302-315) returns
  `pModel & 0x7FFFFFFF` for any model with `m_NumAnimations < 0` — i.e. the
  `g_Models` **entry for a SimpleModel is stored with bit31 CLEAR** (confirmed
  live: "g_Models[m_Class] returns a KUSEG pointer needing |0x80000000",
  the format notes).
- Gem classes 83..87 are observed flowing through the SHADED walk
  (moby_native.c:3591 gem-dump instrument), and the gem shine is the SHADED
  **metal** face sub-mode fed from `D_8006E44C` (moby_native.c:3914-3920).

So the injection target format is the SHADED SimpleModel. **Do NOT build an
animated `Model`/`AnimationHeader`** — gems never go near the keyframe
renderer, and the animated format (moby.h:196-229) buys nothing here.

Stock gems are the **LIT** variant (verts pointer ≠ model+0x10, metal flat
faces). For injection we recommend the **BAKED static-gouraud** variant
instead: no normals, no lighting, per-corner colours emitted verbatim —
exercised and verified in both renderers by the egg fix
(`shaded_static_gouraud_table` memory; moby_native.c:3604-3620).

### A.1 The two SimpleModel layout variants

Struct per moby.h:237-242, but note the first word is NOT literally −1 for
shaded models — the renderer unpacks counts from its low bytes (the moby.h
"always -1" comment is wrong for these):

```
SimpleModel (BAKED static-gouraud variant — RECOMMENDED)
off   sz  field
0x00  1   s8   vertexCount   (1..127; read as SIGNED byte — .s lb @0x80023008,
                              full.c:34944; native moby_native.c:3601)
0x01  1   u8   faceCount     (0..255; lbu @0x8002326C, full.c:35264;
                              native moby_native.c:3603)
0x02  1   s8   nearBias      (signed byte pushed with the moby onto a
                              near-OT second-pass list when the moby's OT
                              slot is very near — lb @0x80022C3C,
                              full.c:34569; COPY FROM STOCK GEM, else 0)
0x03  1   u8   signByte      (bit7 MUST be set so the int at +0x00 is <0 ⇒
                              SimpleModel to the loader, loaders.c:309;
                              only the loader reads it; use 0xFF, or copy
                              stock)
0x04  4   u32  m_Verts   = modelAddr + 0x10       ← THIS equality selects
                              BAKED mode (see A.4); bit31 optional (masked
                              on both sides)
0x08  4   u32  m_Colors  = colour-table vaddr     (word-aligned)
0x0C  4   u32  m_Faces   = face-stream vaddr      (word-aligned)
0x10  3×N      vertex stream (3 signed bytes each, see A.2), pad to 4
...        colour table  (≤128 words 0x00BBGGRR, see A.3)
...        face stream   (faceCount × 8 bytes, see A.5)
```

The LIT variant differs only in: `m_Verts` points anywhere EXCEPT model+0x10;
model+0x10 instead holds an inline per-vertex **normal** stream (3 signed
bytes each, b0=Nz b1=Nx b2=Ny — moby_native.c:3900-3912, .s @0x80023130);
gouraud face colour fields index the runtime-lit per-vertex colours;
`m_Colors` is unread. Avoid — it drags in the directional-light and
metal-table dependencies for no benefit.

Alignment: the model base, `m_Verts` target, `m_Colors` target and `m_Faces`
target must be 4-byte aligned — first vertex is a plain `lw` (@0x80023004,
full.c:34943; subsequent verts use lwr/lwl so the 3-byte stride is fine),
face words and colour words are plain `lw`.

### A.2 Vertex record — 3 signed bytes, ×2

Decode (.s @0x80023010-0x8002302C, full.c:34946-34953; native
moby_native.c:3779-3796):

```
w  = b0 | b1<<8 | b2<<16          (3 bytes per vertex, stride 3, no pad)
VZ = sign_extend8(b0) * 2         (the >>23-instead-of->>24 gives the ×2)
VX = sign_extend8(b1) * 2
VY = sign_extend8(b2) * 2
```

- Byte order is **Z, X, Y**.
- Model-space range: even values in [−256, +254] per axis. Author each S3 gem
  quantized to `coord/2` signed bytes.
- There is **no scale field anywhere in a SimpleModel header** (the
  `m_Scale`/`m_ShortEncodeShift` fields are AnimationHeader-only,
  moby.h:196-211). The only scale knobs at render time are per-MOBY:
  - `m_RenderRadius` (moby+0x50): cull only, never geometry.
    `m_RenderRadius == 0` ⇒ moby is ALWAYS skipped (dormant-in-spawner
    mechanism, moby_native.c:3541-3551). Bit7 = billboard flag
    (moby_native.c:3623). Band cull: kept only if `|pos−cam|>>2 <
    ((r&0x7F)<<8) (+0x200 hysteresis if drawn last tick)` on all 3 axes
    (.s blocks 0x80022B40-0x80022B90, moby_native.c:3552-3580).
  - `m_DrawScale` (moby+0x57): GPF-scales the projected translation TR by
    p57/32 (moby_native.c:3742-3748). Gems leave it 0.
  - `m_DepthOffset` (moby+0x47, = m_Rotation word byte3): OT pull of 128
    world units per count (moby_native.c:3758-3768).
  The mod changes none of these — keep the converted gems at roughly stock
  gem extent so the stock `m_RenderRadius` cull still fits. Calibrate by
  dumping a stock gem model's vertex min/max at boot (they're installed from
  PETE.WAD at boot by `func_8005B7D8`, see B).

### A.3 Baked colour table

- Array of words, format **0x00BBGGRR** (R = low byte; matches
  `shaded_metal_color`/prim RGB decode, moby_native.c:3494-3501, and the .s
  copies the word verbatim into the prim). Byte3 = 0x00.
- Face colour fields are **byte offsets** into this table, field range
  0..0x1FC ⇒ **max 128 colour words (512 bytes)**.
- **Native bound-check constraint** (moby_native.c:3618-3619): the native
  decoder computes `ctab_len = (faces > ctab && faces − ctab < 0x2000) ?
  faces − ctab : 0x800`. So **place the colour table immediately BEFORE the
  face stream, with `m_Faces − m_Colors < 0x2000`** — then every in-table
  offset passes the `o < L` check (out-of-bound corners render grey 0x808080,
  moby_native.c:3951-3956).

### A.4 BAKED-mode selection (the "bit31" rule, exact semantics)

.s block @0x800230EC-0x800230FC (full.c:35082-35093):

```
s6 = raw g_Models[cls] entry (used as the model pointer directly)
taken_baked = ((m_Verts << 1) >> 1) == (s6 + 0x10)     // bit31 masked off verts
if taken_baked: colour source = [model + 8]            // delay slot, 0x800230FC
else:           colour source = per-vertex LIT colours (prepass @0x80023100+)
```

Because `PatchMobyModelPointers` stores SimpleModel entries **without bit31**
(loaders.c:313), the comparison can succeed. Native mirror
(moby_native.c:3615): `!(mraw & 0x80000000) && (vraw & 0x7FFFFFFF) ==
((mraw & 0x7FFFFFFF) + 0x10)`.

**Mod requirements:**

- Write the `g_Models[83+i]` entry as `modelVaddr & 0x7FFFFFFF` (bit31
  CLEAR). Entries live at `0x80076378 + cls*4` → **0x800764C4..0x800764D4**
  (`OP_GADDR_g_Models` 0x80076378, symbols.h:2536).
- Write `m_Verts = modelVaddr + 0x10` (with or without bit31 — both sides
  mask; suggest writing it bit31-clear to match what the entry compare sees).

### A.5 Face record — 8 bytes, count-terminated

Stream = `faceCount` records of exactly 8 bytes; the .s loop ends at
`m_Faces + faceCount*8` (@0x8002327C, full.c:35267-35268) and advances
`s5 += 8` per face (@0x80023334, full.c:35345). word0 = [face+0],
word1 = [face+4] (full.c:35278/35284).

```
word0 (topology + mode):
  bits 23..29  i0*? — extraction is (w0>>21)&0x1FC = i0*4  ⇒ encode i0<<23
  bits 16..22  (w0>>14)&0x1FC = i1*4                       ⇒ encode i1<<16
  bits  9..15  (w0>> 7)&0x1FC = i2*4                       ⇒ encode i2<<9
  bits  2.. 8  (w0    )&0x1FC = i3*4                       ⇒ encode i3<<2
  bit 1        winding swap (gouraud) / METAL sub-mode (flat) — SET 0
  bit 0        1 = FLAT-lit face, 0 = gouraud               — SET 0 (baked)
  bits 30,31   not extracted — SET 0
  (.s index extraction @0x800232B0-0x800232D8, full.c:35291-35302; flags
   t6=at&1, t7=at&2 @0x80023320-24, full.c:35339-35340; native
   moby_native.c:3931-3934, 3940-3946)

word1 (colours, gouraud/baked mode):
  same packing as word0: byte offsets into the colour table
    off0=(w1>>21)&0x1FC, off1=(w1>>14)&0x1FC, off2=(w1>>7)&0x1FC, off3=w1&0x1FC
  ⇒ encode colourWordIndexK<<{23,16,9,2}; bits 0..1 and 30..31 unread — 0.
  (.s: t2=((w1>>sh)&0x1FC)+s4, colour word used verbatim, @0x80023338+,
   full.c:35353-35411; native moby_native.c:3947-3957)
  [flat faces instead pack a face normal in w1: b3=Nz b2=Nx b1=Ny signed —
   moby_native.c:3941 — not used in the baked plan]
```

**Tri vs quad:** a face is a TRI iff `i2 == i3` (the .s compares the two
scratch ADDRESSES derived from fields 2 and 3, `a0==a1` @0x8002333C,
full.c:35354-35358; native moby_native.c:3972). There is no separate marker
bit. For a tri, also set colour field 3 = colour field 2.

**Quad topology & keep rule** (moby_native.c:3826-3832, 3972-3982):

```
TRI  (v0,v1,v2)                 drawn iff NCLIP(v0,v1,v2) > 0
QUAD (v0,v1,v2) + (v1,v3,v2)    drawn iff NCLIP(v0,v1,v2) > 0
                                      OR NCLIP(v3,v1,v2) < 0
```

i.e. a quad with perimeter A→B→C→D encodes as `i0=A, i1=B, i2=D, i3=C`
(shared diagonal B–D), and renders from either side; a lone tri is
single-sided. Order each tri's vertices so the outward-facing view yields
NCLIP > 0 (screen x right, y down); if a converted gem renders inside-out,
swap i1/i2 globally.

**Bits/modes to AVOID:** `w0 bit0=1,bit1=1` (METAL env-reflection,
block_80023720) — depends on `m_SpecularMetalColor`/`D_8006E44C` and is only
approximated natively (drawn opaque flat, moby_native.c:3986-3997);
`w0 bit0=1,bit1=0` (flat-lit) additionally goes semi-transparent (ABE) when
`m_SpecularMetalColor>>22 == 0` (moby_native.c:3921-3926) — avoidable
surprises. All-gouraud-baked sidesteps every one of these.

### A.6 Hard limits recap

| limit | value | source |
|---|---|---|
| vertexCount | ≤ 127 (signed-byte read) | full.c:34944 |
| faceCount | ≤ 255 | full.c:35264 |
| vertex index per face | ≤ 127 (0x1FC field ÷4) | full.c:35292-35301 |
| colour words | ≤ 128 (offset field ≤ 0x1FC) | full.c:35356 |
| colour-table→faces gap | faces after ctab, < 0x2000 bytes | moby_native.c:3619 |
| vertex coord | even, −256..+254 | full.c:34946-34953 |
| alignment | model/verts/ctab/faces word-aligned | full.c:34943, 35278 |

Size estimate per converted gem (100 verts, 96 colours, 160 faces):
16 + 300(+pad) + 384 + 1280 ≈ **2.0 KB**; 5 gems ≈ 10 KB.

---

## B. Guest RAM park + install hook

### B.1 Park region: **0x80008000..0x8000FFFF (32 KB), written ONCE**

- Guest RAM is a flat 2 MB host array `g_ram` (core/runtime/cpu.c:23,
  `OPENPETE_PSX_RAM_SIZE` core/include/openpete_psx.h:35), accessed as
  `addr & 0x1FFFFF`; zeroed once at init (cpu.c:185). No BIOS/kernel is ever
  loaded — all BIOS/PSYQ services are host thunks; the EXE loads at
  0x80010000 (core/runtime/boot.c:69-70). Nothing in core/, games/, or the
  game itself references phys < 0x10000 (Spyro's floor is the EXE;
  every loader pointer is ≥ `g_OverlaySpacePointer` = 0x8007AA38,
  src/overlay_pointers.c:12, psx.map:3070).
- Outside every oracle-compared region (all direct regions in
  `manifest/gameplay_state_regions.toml` are ≥ 0x800756xx; indirect pools
  live in the level arena). Persisted by savestates (whole-RAM,
  core/include/openpete_state.h:19). Stack is 0x801FF800..0x80200000
  (`_stacksize` 0x800, spyro-1/asm/data/initialization.sdata.s:8).
- Start at +0x8000 (not 0) to stay clear of stray guest null-pointer stores.
- Rejected alternative: below `m_LowerPolyBuffer` (0x80187BB0) — the level
  arena tops out within 0x8000 of it on the biggest levels, and it doubles
  as transition/titlescreen/memcard scratch (loaders.c:1327-1369,
  update.c:1006, gamestates/init.c:552). No fixed park there survives.
- **Caveat:** openpete-only. On real hardware/DuckStation the kernel owns
  0x0..0xFFFF. Fine for a mod-API mod by construction.

Suggested internal layout: 5 models (word-aligned, ~2 KB each) from
0x80008000; VAG blob (see C) at the tail, 16-byte aligned. If >32 KB is ever
needed, 0x80001000..0x80007FFF is equally dead (same evidence).

### B.2 g_Models lifecycle → hook choice

- Gem models ship in **PETE.WAD** (shared models: classes 0, 34, 58, 75, 76,
  **83..87**, 173, digits/letters…), installed **once at boot** by
  `func_8005B7D8` (src/4BEF8.c:68-75; re-run on reset-to-title,
  gamestates/init.c:408), pointing into `m_SharedAnimations` (0x801C5BB8).
- Every level load, `LoadLevel` (0x80015370) stage 10 **nulls every
  `g_Models[i]` whose phys addr < `m_SharedAnimations`**
  (loaders.c:1550-1555) then reinstalls level models via
  `PatchMobyModelPointers` (loaders.c:1558-1565). `LoadDragonCutscene`
  stage 5 does the same clear+reinstall mid-level (loaders.c:1254-1270).
  Stock gem pointers (≥ SA) survive; **the mod's pointers (phys 0x8000) get
  nulled by both** and must be re-applied.
- Call order per level: LoadLevel stage 9 → `LoadLevelData` (0x80012D58,
  loaders.c:1495) → `SetNewSoundTable` (0x80012CF0, loaders.c:225) →
  stage 10 clear+reinstall → stage 13 `LoadLevelScene` (0x8001364C,
  loaders.c:1660). LoadLevelData/SetNewSoundTable post-hooks fire BEFORE the
  stage-10 clear — **too early for the pointer writes**.

**Hook: post-hook `PatchMobyModelPointers` = `OP_FNADDR_PatchMobyModelPointers`
0x800133E0 (symbols.h:262).** It is called from every install path — boot
(4BEF8.c:72), level stage 10 (loaders.c:1563), cutscene loads
(loaders.c:929), dragon-cutscene stages 1/5 (loaders.c:1057, 1266) — and in
the clearing paths the install-loop calls always come after the clear, so a
post-hook rewrite of the 5 entries is correct and idempotent (~64 fires per
load, trivially cheap):

```c
static void on_patch_models(CPUState* cpu) {
    g_api->base(cpu);                              /* run the original */
    for (int i = 0; i < 5; i++)
        op_write_u32(0x800764C4u + 4u*i, s_model_vaddr[i] & 0x7FFFFFFFu);
}
```

Model **bytes** are written once at `openpete_mod_entry` (runs during engine
startup before the game boots, core/runtime/run.c:773; `guest()` is legal
there, `call_addr` is NOT — overrides only).

---

## C. SPU upload

### C.1 What SPU RAM holds

Level load stage 6 (loaders.c:1461-1470; cutscene twin 867-875):

```c
SpuSetTransferStartAddr(0x1010);
SpuWrite(g_Buffers.m_DiscCopyBuf, 0x80000 - 0x1010);   // whole range, always
```

Real sample data size = `g_LevelHeader.m_VramSramSize − 0x80000` (the disc
chunk is VRAM image 0x80000 + SPU samples, loaders.c:1451); the rest of the
write is garbage fill. So at runtime:

```
spu_data_end = 0x1010 + read_u32(0x80076C04) − 0x80000
```

(`g_LevelHeader` = 0x80076C00, `OP_GADDR_g_LevelHeader` symbols.h:2516;
`m_VramSramSize` at +0x04, include/wad.h:40-52; populated at stage 4,
loaders.c:1421 — valid by the time any stage-9 hook fires.)

`SoundDefinition.m_Addr` has **no length field** — you cannot compute
occupancy from the table; use the formula above.

### C.2 Where to put the chime

- **Reverb: not a constraint.** Spyro 1 never calls any reverb API (zero
  hits in spyro-1 src/include), and PsyCross implements reverb as an OpenAL
  effect with **no SPU-RAM work area**
  (third_party/PsyCross/src/audio/PsyX_SPUAL.cpp:421-481).
- **Dragon-rescue speech IS a constraint:** cutscenes stream speech into the
  TOP of SPU RAM, `SpuSetTransferStartAddr(0x80000 − spuLen)`
  (loaders.c:1070-1074, block variant 1115-1152); the overflow path restores
  only up to `spu_data_end` afterwards (loaders.c:1231-1234).

**Recommendation: upload to `align16(spu_data_end)`** — the bottom of the
free gap (speech eats the gap from the top). Re-upload **per level load**
(the stage-6 blast clobbers everything every time). Residual risk: in levels
whose free gap is smaller than a dragon speech clip, speech may overwrite
the chime until the next level load — if audible in practice, also re-upload
from a post-hook on the dragon-cutscene loader (see G).

### C.3 Upload mechanics (all guest-callable via `api->call_addr`)

| PSYQ fn | vaddr | symbols.h |
|---|---|---|
| `SpuSetTransferStartAddr(a0=spuAddr)` | `OP_FNADDR_SpuSetTransferStartAddr` 0x8005CB24 | thunk psyq_thunks.cpp:1606 |
| `SpuWrite(a0=guestBuf, a1=size)` | `OP_FNADDR_SpuWrite` 0x8005CAC4 | thunk :1598 |
| `SpuIsTransferCompleted` | 0x8005CBB0 | returns 1 always (PsyCross LIBSPU.C:79-81) |

- `PsyX_SPUAL_Write` is a **synchronous memcpy** into the 512 KB host SPU
  array (PsyX_SPUAL.cpp:724-738; `SPU_REALMEMSIZE` :63) — a hook-time upload
  needs no DMA wait. `SpuSetTransferStartAddr` rejects addr < 0x1010
  (PsyX_SPUAL.cpp:718). `SpuSetTransferMode` is a stored flag, optional.
- The `SpuWrite` source must be a **guest vaddr** (thunk does `PTR(cpu->a0)`)
  — stage the VAG bytes permanently in the Part-B park region and upload
  straight from there. (`g_Buffers.m_CopyBuf`, g_Buffers = 0x800785D8,
  buffers.h:4-17, is loader scratch and works too, but the park is simpler
  and always idle.)
- **VAG payload:** raw 16-byte ADPCM blocks only — strip the 48-byte "VAGp"
  file header. Final block flag byte = **0x01** (loop-end, no repeat): that
  is what terminates a one-shot voice in PsyCross
  (SPUAL_ScanOneShotSamples, PsyX_SPUAL.cpp:347-376; flag 0x03 = infinite
  loop). Playback decodes directly from `samplemem + attr.addr` at key-on —
  no registration beyond the SoundDefinition repoint.

### C.4 When

Post-hook **`SetNewSoundTable` = `OP_FNADDR_SetNewSoundTable` 0x80012CF0**
(symbols.h:286). It runs at loaders.c:225 (fresh level, `pPatchAddresses=1`,
after the stage-6 SPU blast) and loaders.c:1369 (transition table
relocation). In the post-hook: recompute `spu_data_end`, `call_addr` the two
Spu thunks to upload, then patch the gemPickup SoundDefinition (Part D).
Idempotent on both call sites. (`call_addr` is legal here — we're inside an
override with a live `CPUState`.)

---

## D. SoundDefinition + PlaySound

### D.1 Layout (spu.h:33-45) — 0x14 (20) bytes per record

| off | sz | field |
|---|---|---|
| 0x00 | 4 | `m_Addr` — SPU byte address (defs on disc store offsets; `SetNewSoundTable` adds 0x1010 when patching, loaders.c:34-54) |
| 0x04 | 4 | `m_LoopAddr` — 0 for one-shot |
| 0x08 | 2 | `unk_0x8` (unused volume) |
| 0x0A | 2 | `m_Pitch` |
| 0x0C | 2 | `m_PitchVariance` |
| 0x0E | 2 | `m_PitchMultiplier` |
| 0x10 | 4 | `m_VarianceType` (0 = ± variance, 1 = + only, 2 = − only) |

### D.2 g_Spu pointers (g_Spu = 0x80075F08, `OP_GADDR_g_Spu` symbols.h:2563)

Offsets computed from spu.h:47-79 (SpuCommonAttr 0x28 + ActiveSound 0x1C×24,
anchored by the `unk_0x2d8`/`unk_0x320` field names and the
`sizeof==0x340` assert, spu.h:112):

| member | offset | guest addr |
|---|---|---|
| `m_SoundDefinitions` (SoundDefinition*) | +0x2C8 | **0x800761D0** |
| `m_SoundTable` (SoundTable*) | +0x2CC | **0x800761D4** |
| `m_NextSoundOverrideFlags` | +0x2D4 | 0x800761DC |
| `m_PitchOverride` (u16) | +0x2EC | 0x800761F4 |
| `m_SoundTableSize` | +0x32C | 0x80076234 |
| `m_PlayVoice` | +0x330 | 0x80076238 |

Sound-table blob layout in RAM (loaders.c:34-54): `[SoundTable 256 B]
[int count][SoundDefinition × count]`; `m_SoundDefinitions = table + 260`.

### D.3 gemPickup resolution

`SoundTable` (include/sound_table.h) is a 256-byte logical→physical map;
**`gemPickup` is byte 0**. Callers:
`PlaySound(g_Spu.m_SoundTable->gemPickup, pMoby, 16, nullptr)`
(moby_helpers.c:1172, inside `func_8003B854`). The physical index is
**per-level data — never hardcode it**:

```c
uint32_t table  = op_read_u32(0x800761D4);
uint8_t  idx    = op_read_u8(table + 0);          /* ->gemPickup */
uint32_t def    = op_read_u32(0x800761D0) + idx * 0x14u;
```

Patch: `m_Addr = ourSpuAddr`, `m_LoopAddr = 0`, `m_Pitch = base pitch`,
`m_PitchVariance = 0` (kills the RNG jitter — see the rand() note below),
`m_VarianceType = 0`.

### D.4 PlaySound pitch computation (spu.c:136-176) — quoted

```c
if ((g_Spu.m_NextSoundOverrideFlags & 2) != 0) {
  g_Spu.m_NextSoundOverrideFlags &= ~2;
  g_Spu.m_ActiveSounds[voice_slot].m_Pitch = g_Spu.m_PitchOverride;
} else { /* ... = m_SoundDefinitions[pSoundId].m_Pitch; */ }
...
attr.pitch = g_Spu.m_SoundDefinitions[pSoundId].m_Pitch;
if (def->m_PitchVariance != 0) switch (def->m_VarianceType) {
  case 0: attr.pitch -= m_PitchMultiplier * (m_PitchVariance / 2);
          attr.pitch += (rand() % m_PitchVariance) * m_PitchMultiplier; break;
  case 1: attr.pitch += (rand() % m_PitchVariance) * m_PitchMultiplier; break;
  case 2: attr.pitch -= (rand() % m_PitchVariance) * m_PitchMultiplier; break;
}
attr.loop_addr = g_Spu.m_SoundDefinitions[pSoundId].m_LoopAddr;
```

- ⚠️ **The `m_PitchOverride` path only updates the ActiveSound mirror; the
  SPU always gets the definition's `m_Pitch`** (spu.c:148). Per-collect
  pitch cycling therefore = write `m_Pitch` into the definition before each
  collect.
- `attr.loop_addr = m_LoopAddr` verbatim; keep 0 for a one-shot (also gates
  the 0x100 looping flag, spu.c:259-260). The ADPCM end-flag block is what
  actually stops the voice.
- Pitch units: **0x1000 = the sample's native 44.1 kHz rate** (PsyCross:
  AL pitch = pitch/4096 over 44.1 kHz buffers, PsyX_SPUAL.cpp:1145; one-shot
  length = blocks·28·4096/pitch, :367-368). A 22.05 kHz-sourced VAG plays at
  recorded speed with `m_Pitch = 0x800`; the S3-style ascending ladder is
  then e.g. 0x800·2^(n/12).
- `rand()` here is gameplay RNG: variance≠0 consumes rand() per collect.
  Zeroing variance changes the RNG stream vs stock — expected and fine for a
  mod (mods are outside the oracle contract), just don't be surprised that
  gameplay diverges from unmodded runs.
- Voice allocation: PlaySound scans the 24 slots for `(m_Flags & 0xC1)==0`
  (spu.c:114-123), returns 0 if none; flag 16 = 2D non-positional at fixed
  volume 0x2000 (spu.c:181-204). Key-on happens next `SpuUpdate` via
  `m_PlayVoice |= 1<<slot` (spu.c:248). Nothing to patch.

### D.5 Per-collect pitch hook

The chime is played from **`func_8003B854(int gemValue, Moby*)`**
(`OP_FNADDR_func_8003B854` 0x8003B854, symbols.h:544; PlaySound at
moby_helpers.c:1172), called by `CollectItem` (`OP_FNADDR_CollectItem`
0x8003B9D4, moby_helpers.c:1176-1212). **Pre-hook `func_8003B854`**: read
`cpu->a0` (gem value 1/2/5/10/25), compute the next pitch in the cycle,
write it to `def+0x0A`, then `api->base(cpu)`. (Hooking here rather than
CollectItem also catches any other callers of the collect helper.)

---

## E. Burst-particle recolour — D_8006E44C

- `D_8006E44C` = guest **0x8006E44C** (psx.map:2146,
  `OP_GADDR_D_8006E44C` symbols.h:1895), declared `u_int D_8006E44C[17]`
  (moby.h:517), word format **0x00BBGGRR**.
- Burst spawn (moby_helpers.c:1194-1196, in `CollectItem`):

```c
(*D_800758E4)(6, 0xC, pMoby,
              (void *)D_8006E44C[12 + (pMoby->m_Class - MOBYCLASS_GEM_1)]);
```

  ⇒ **words 12..16 (byte offsets 0x30..0x40, guest 0x8006E47C..0x8006E48C)
  are the burst colours for gem classes 83..87.** Write 5 words, once at
  init — main-EXE .data, never reloaded.
- **Shared-consumer caveat:** the SHADED metal shine reads the same table at
  `D_8006E44C + (m_SpecularMetalColor >> 21)` as a PAIR (base tint word +
  reflected tint word at +4) (moby_native.c:3914-3920, .s block_80023720).
  If the stock gems' `m_SpecularMetalColor` values land those reads on words
  12..17, recolouring also changes the stock-model shine — irrelevant once
  the models are swapped to baked-colour (which never reads the metal
  table), but under the recolour-only fallback expect the shine to shift
  with the bursts. The flat-shaded table `D_8006E3D8` (0x8006E3D8,
  indexed `>>22`) is untouched.

---

## F. Mod runtime mechanics

### F.1 Discovery & structure

- `openpete_mods_init` (core/mods/mod_loader.c:690-700) resolves the mods
  dir: `--mods-dir <path>` (core/runtime/options.c:850-852) →
  `OPENPETE_MODS_DIR` env → default **exe-relative** `<exe>/mods`
  (SDL_GetBasePath, mod_loader.c:697-699). The repo `mods/` dir is NOT found
  by default — pass `--mods-dir mods` from the repo root.
- `enabled.txt` inside the mods dir: one id per line, `#` comments; **later
  lines = higher priority** on shared overrides (mod_loader.c:724-741).
- Mod = `mods/spyro3-gems/mod.toml` (`[mod] id="spyro3-gems"
  version="0.1.0"`, optional `api=1`, `deps`, `[test]` block) +
  `src/*.c` compiled on load: `${OPENPETE_MOD_CC:-${CC:-cc}} -shared -fPIC
  -O2 -g -fno-strict-aliasing -Wall -I<sdk> -o .build/<id>.so src/*.c`
  (mod_loader.c:540-564; log in `.build/compile.log`; mtime-based rebuild).
  `/usr/bin/cc`, gcc and clang are all present on this machine;
  `OPENPETE_MOD_CC` unset ⇒ plain `cc`.
- Entry: `int openpete_mod_entry(const openpete_mod_api_t* api,
  openpete_mod_t* self)` — return 0 or be disabled. Key API
  (core/include/openpete_mod_api.h): `override_addr` (:86) /`override_name`
  (:88), `base(cpu)` (:95 — call-position decides pre/post hook; post-hook =
  `base()` first, then your code), `guest(vaddr)` (:111), `guest_addr`
  (:129), `call_addr(cpu, addr)` (:138 — **only inside an override**),
  `log` (:101), inline `op_read_u32/op_write_u32/op_read_u8/op_write_u8`
  (:245-265), generated `op_call_<Name>()` wrappers in
  `openpete_sdk_wrappers.h`.
- `psx_override_dispatch` prologue is universal (every recompiled fn) — no
  re-link needed for any hook address.

### F.2 Running / testing

- Build: `cmake --build --preset linux-release` (always
  linux-release). Binary:
  `build/linux-release/games/spyro-1/openpete-spyro1` (present in this
  worktree).
- ⚠️ The disc is an untracked input, missing from this worktree — use
  `<spyro>/openpete/games/spyro-1/spyro-1/build/SCUS-94228.cue`
  (or symlink it in).
- Quick smoke:

```sh
cd <spyro>/openpete-s3gems && \
build/linux-release/games/spyro-1/openpete-spyro1 \
  --headless \
  --disc <spyro>/openpete/games/spyro-1/spyro-1/build/SCUS-94228.cue \
  --run-recomp 0x80012204 \
  --frame-limit 4000 \
  --mods-dir mods 2>&1 | grep -E 'mods|mod:'
```

  Useful flags (core/runtime/options.c:554-908): `--skip-to-level <name|id>`
  (level loads ~frame 1700), `--renderer psycross|vk|ab`,
  `--screenshot-on-frame N`, `--record <mp4>`, `--bk2 <replay>`,
  `--window WxH`, `--log-level`.
- Success lines (all on **stderr**, core/runtime/log.c): `loaded spyro3-gems
  v0.1.0 (<so>)`, `override registered: <name> (0xADDR)`, `N mod(s) active
  (dir: …)`; `api->log` appears as `mod:spyro3-gems: …`.
- `tools/modtest.py mods/spyro3-gems` drives a fixture mods-dir run from the
  `[test]` manifest block (frames / expect_oracle pass|fail|skip /
  expect_log substrings); env `OPENPETE_BIN`, `OPENPETE_DISC` (set the
  latter to the absolute cue path in this worktree).
- Add `spyro3-gems` to `mods/enabled.txt` for manual runs.

---

## G. Consolidated write plan

| # | What | Where | When (hook) |
|---|---|---|---|
| 1 | 5 model blobs + VAG blob | 0x80008000.. (park) | once, `openpete_mod_entry` |
| 2 | Burst colours, 5 words | 0x8006E47C..0x8006E48C | once, `openpete_mod_entry` |
| 3 | `g_Models[83..87] = park_model[i] & 0x7FFFFFFF` | 0x800764C4..0x800764D4 | post-hook `PatchMobyModelPointers` 0x800133E0 |
| 4 | SPU upload (`SpuSetTransferStartAddr(align16(spu_data_end))`; `SpuWrite(parkVagVaddr, len)`) + patch gemPickup def (`m_Addr/m_LoopAddr=0/m_Pitch/m_PitchVariance=0`) | SPU + `def` via 0x800761D0/0x800761D4 | post-hook `SetNewSoundTable` 0x80012CF0 |
| 5 | Next cycle pitch → `def+0x0A` | per collect | pre-hook `func_8003B854` 0x8003B854 |

Verification ladder: (a) modtest `expect_log` on entry logs; (b)
`--skip-to-level` + `--screenshot-on-frame` for gem visuals in both
`--renderer vk` and `psycross` (the .s walk must render the baked model too
— confirm on `--renderer ab`); (c) collect gems live for chime + burst.

## H. Uncertainties (explicit)

1. **model+0x02 (nearBias) semantics** — only consumer found is the near-OT
   second-pass push (@0x80022C3C); exact downstream use unverified. Mitigate
   by copying bytes +0x02/+0x03 from the stock gem headers at boot (the
   stock models are readable at `g_Models[83..87]|0x80000000` after
   `func_8005B7D8`).
2. **g_Spu member offsets** (+0x2C8/+0x2CC/…) — computed from struct layout
   + two name-anchored fields, not read out of a disassembled access site.
   Verify once at runtime: `read_u32(0x800761D4)` should point at a table
   whose bytes are plausible sound indices (< count at table+256).
3. **Metal-table/burst-word aliasing** (E) — whether stock gem shine reads
   exactly words 12..16 depends on the per-class `m_SpecularMetalColor`;
   moot after the model swap.
4. **Dragon-speech vs chime SPU overlap** in overflow levels (C.2) — worst
   case a corrupted chime until next level load; add a re-upload post-hook
   on the dragon-cutscene loader if audible.
5. **Winding/NCLIP sign for authored tris** — convention stated in A.5 from
   the native mirror; verify visually on first render, global i1/i2 swap is
   the fix.
6. **Tally/HUD gems**: level-exit flying gems and HUD gem icons draw the same
   classes through the billboard/HUD shaded paths (moby_native.c:3636-3693)
   — same model data, so they swap too; check they still read correctly at
   billboard scale.
