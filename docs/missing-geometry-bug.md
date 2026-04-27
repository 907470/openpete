# Missing-geometry bug — investigation notes

## Symptom

In Artisans Homeworld, Stone Hill, and Dark Hollow, parts of geometry render
as if culled. Most reproducibly: parts of Spyro's mesh disappear from frame to
frame as the camera circles him. Other casualties: terrain edges, sparks,
bridge facets that briefly let the ground show through.

Reproducer: `games/spyro-1/bk2/missing-geometry.bk2`. The clearest pair is
**openpete_frame 2472 (bad — front legs + torso missing)** vs **2474 (good —
fully visible)**, camera barely moved between them. User-reported framing of
the bug:

- "Culled, not clipped" — prims look gone, not chopped.
- Distance-dependent — culled prims tend to be at similar distance from
  the camera when it happens.
- Doesn't happen in cutscenes (cutscenes have static, scripted cameras;
  gameplay doesn't).

## What's been ruled out

### GTE math is bit-identical (already verified)

`docs/gte-trace-vs-reference.md` traces every COP2 op through three engines —
psxrecomp's GTE (`rec`), PsyCross's GTE (`psy`), and the vendored pcsx-redux
GTE (`pcsx`). On the bug-reproducing frames (2472–2476):

- 4,970 RTPS / NCLIP / RTPT / AVSZ3 / AVSZ4 ops compared — zero diff.
- 16,333 lighting ops (DPCS / CC / GPF / GPL / INTPL) compared — zero diff.

So vertex transform, perspective projection, lighting/colour computation, and
the OT-bucket-derivation maths are all correct.

### Prims reach the OT chain with valid coords on bad frames

The new `OPENPETE_OT_DUMP_FRAMES=A-B` env var (psyq_thunks.cpp) writes one CSV
row per OT slot per DrawOTag call. On the bad frame:

- Total prims: 2100 (matches good frame to within ±7).
- Code distribution: identical mix of POLY_G3/G4/GT3/GT4/F3/F4/env packets.
- In Spyro's body screen region (x∈[100,220], y∈[130,220]): **45 prims on bad
  frame, 45 on good frame**, identical code split (41 × POLY_GT4, 4 × POLY_GT3).
- Vertex SXYs are sensible — zero degenerate-collinear, zero offscreen.

Spyro's body tris are *in* the OT chain on bad frames. They render
invisibly. So this is not a clip-/cull-/culling-out-of-OT bug.

### Recompiler integer/load semantics

Audited and cleared:

- `MFC2`/`CFC2`/`MTC2`/`CTC2` codegen: simple memcpy through
  `cpu->gte_data[]` / `cpu->gte_ctrl[]`, kept consistent with the runner's
  per-op `gte_mfc2`/`gte_mtc2` semantics (sign-extends IR0..3, zero-extends
  SZ/OTZ, packs SXY, mirrors SXY2→SXYP). The codegen does NOT mirror SXY2
  on bare MTC2 reg 14 nor push the SXYP FIFO on MTC2 reg 15, but a scan of
  generated code shows zero MTC2→MFC2 of IR/SXY/SZ regs without an
  intervening GTE op, so this gap is unexercised.
- `LWL`/`LWR`/`SWL`/`SWR`: **runtime helpers in `core/runtime/cpu.c` are
  correct LE formulas** (cross-checked against pcsx-redux's shift tables;
  see comment at `cpu.c:88-96` documenting a previously-fixed BE-vs-LE
  bug). The reference impls embedded by the codegen at
  `code_generator.cpp:1708-1770` use BE formulas, but the emitted MIPS
  callsites invoke `cpu->lwl(...)` (function pointer) and the runtime sets
  these to the correct `do_lwl/...` — so the codegen-side wrong impls are
  dead code.
- `SRA` / `SRAV`: cast-then-shift on `int32_t`, arithmetic-shift behaviour
  on every host platform we target.
- MIPS R3000 load-delay-slot is not modelled, but only matters when
  hand-written asm reads the destination of an `lw/lh/lb` in the
  immediately-following instruction — uncommon, and Spyro's draw path
  doesn't appear to rely on it. GTE op completion is handled correctly
  because `gte_execute` is synchronous (real hardware stalls MFC2/CFC2
  while a GTE op is in flight, so synchronous execution gives the same
  result).

## Where the bug must live

By elimination, in priority order:

1. **`func_80023AC4` (Spyro's renderer) recompilation.** This is hand-written
   asm — uses `$sp/$gp/$fp/$ra` as general-purpose pointers, custom
   save/restore region at `0x80077DD8`, deep COP2 register reuse. Even with
   GTE math correct, a recomp bug in any non-GTE op inside this function
   could write subtly-wrong bytes into the prim packets that downstream
   rendering then dutifully samples as blank/transparent/garbage.

2. **PsyCross primitive processing.** `ParsePrimitivesLinkedList` /
   `ProcessGouraudPoly` etc. could be dropping or mis-rendering specific
   prims based on tpage/CLUT/UV state. The user's "doesn't happen in
   cutscenes" observation fits a state-churn bug here — gameplay flips
   tpage/CLUT mid-frame far more than cutscenes do.

3. **OpenGL backend** — texture binding / sampling / depth state on
   specific draw types.

## Next steps

### Option A — pcsx-redux ground-truth (most decisive)

`pcsx-redux` and `openbios.bin` are already built at
`.tools/pcsx-redux/`. The existing dumper at
`scripts/pcsx-redux-gte-dump.lua` works for the titlescreen cutscene; it
needs adapting to (a) reach Artisans Homeworld at frame ~2470 and (b)
break on `func_80023AC4` instead of the cop2 op list.

Reaching arbitrary in-game state from a fresh OpenBIOS boot is gappy
(documented in `docs/gte-trace-vs-reference.md`). The pragmatic path is:

1. Run pcsx-redux interactively (drop `--no-ui`), play to roughly frame
   2470 of `missing-geometry.bk2`, savestate at the cutscene-equivalent
   entry.
2. Adapt the Lua dumper: bp at `0x80023AC4`, on hit, walk Spyro's prim
   buffer (RAM region depends on the double-buffer phase — capture the
   start address from the function's prologue) and emit a CSV of every
   prim packet in the order written.
3. Run our recomp with the same bk2 + an equivalent dump from
   `psyq_DrawOTag` filtered to Spyro-body slots.
4. Diff. If pcsx-redux produces *different* prim packets, the bug is in
   `func_80023AC4`'s recompilation and we have a concrete pre/post pair
   to bisect against. If pcsx-redux produces *the same* prim packets, the
   bug is downstream of the OT chain (PsyCross or GL).

### Option B — recomp self-instrumentation (fully self-contained, ~30 min)

Wrap `cpu->write_word` calls inside `func_80023AC4` (and the split pieces
it falls into) with a frame-gated logger. Run for both 2472 and 2474.
Diff the write streams.

If on identical entry registers the function emits structurally-different
byte patterns to the prim buffer (beyond what animation should produce),
the bug is unambiguously in our recompilation of that function. Bisect by
binary-searching the 1424-byte function body with a write-position
cutoff (instrument writes ≤ 0x80023XXX, run, walk down).

Option B doesn't require pcsx-redux setup but does assume "what we'd
expect to write" is recognisable. Recommended as the first probe — if it
shows clean writes (matching across frames where they should match), the
bug is downstream and Option A becomes worth the effort.

## Diagnostic infra committed in `b2b567d`

- `atexit` log of final `openpete_frame` — quoting a frame number from a
  manual play session is now trivial.
- `OPENPETE_OT_DUMP_FRAMES=A-B` + `OPENPETE_OT_DUMP_PATH=...` — captures
  every prim slot in DrawOTag's chain over a frame window.
- `bk2/missing-geometry.bk2` — the user's reproducer; camera-circle around
  Spyro in Artisans Homeworld.
