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

## Option B infra (this commit)

`OPENPETE_WRITE_TRACE_FRAMES=A-B` + `OPENPETE_WRITE_TRACE_PATH=...`
captures every `cpu->write_word` issued from inside `func_80023AC4`
during the openpete_frame window. Implemented in `write_trace.cpp`; the
override hook in `game_extras_impl.c` swaps `cpu->write_word` for a
logging wrapper on entry to `0x80023AC4` and restores it on return.

Repro command (the bk2 needs `--bk2-frame-offset 0` and
`--fast-boot-until-frame 850` to land at the right openpete_frame —
the C-side default `-first_input_frame` misaligns):

```
OPENPETE_WRITE_TRACE_FRAMES=2472-2474 \
OPENPETE_WRITE_TRACE_PATH=/tmp/write-trace.csv \
./build/linux-release/games/spyro-1/openpete-spyro1 \
  --bk2 games/spyro-1/bk2/missing-geometry.bk2 --bk2-frame-offset 0 \
  --headless --frame-limit 2480 --fast-boot-until-frame 850 \
  --disc <spyro>/roms/SCUS-94228/SCUS-94228.cue \
  --run-recomp 0x80012204
```

### First findings from the trace

- 3237 writes on bad frame 2472, 3221 on good frame 2474. Counts close
  but not identical.
- Bad frame writes the prim buffer at `0x80180000`, good frame at
  `0x801A0000`. **The renderer alternates between two buffers per
  frame.** Worth checking whether "bad" frames consistently hit one
  buffer vs the other — if so, a buffer-A-specific stale-state bug is
  in play.
- 612 vs 601 writes to the `0x80070000` region (control / OT / pointer
  area), 32 vs 28 writes to `0x80060000`.
- Counts of all-zero and all-FF writes are within noise (133/130 zeros,
  8/7 FFs).

### Caveat

Raw write-stream diffing between bad and good frames is dominated by
animation noise — Spyro's pose changes between frames, so per-vertex
SXY/UV/RGB writes legitimately differ. The signal we want is a write
that *shouldn't* differ — e.g. the same polygon emitting different
CLUT/TPAGE bits, or one frame writing a polygon's RGB as zero. Need a
matching strategy that pairs equivalent polygon-emit slices between
frames before diffing.

### Next probes

1. Pair polygon-emit slices across frames by call-position and compare
   only structural fields (CLUT, TPAGE, packet code byte, RGB slots).
   Animation moves vertices but preserves these.
2. Capture 2470 / 2476 / 2478 alongside to test the
   buffer-alternation hypothesis (does "bad" track buffer A?).
3. If structural fields match, the bug is downstream: PsyCross's
   prim-list traversal or the GL backend reads valid packets but
   renders one path of them invisibly. That demotes Option B and
   promotes Option A (pcsx-redux ground-truth) only as a tiebreaker —
   the more direct probe is then to instrument PsyCross's per-prim
   dispatch.

## NCLIP-force experiment (2026-04-27)

User confirmed every gameplay frame has missing-geometry artifacts (no
clean reference frame in the bk2 — earlier comparisons of "good" 2474
vs "bad" 2472 were both bugged, just to different degrees).

Tested temporary patch in `gte_nclip` to force MAC0 = ±0x7FFFFFFF
behind `OPENPETE_NCLIP_FORCE=+/-`. Result on the live Windows build:

- **`+`**: holes at one set of polygons, no longer distance-linked.
- **`-`**: holes at a *different* set of polygons.
- Both produce the same *type* of artifact (missing polys, not
  back-face leak-through).

Conclusion: NCLIP MAC0 sign **selects between two polygon-emit paths**
in the renderer chain (split-quad diagonal A vs diagonal B is the
likely shape). Both paths emit real prims; **both paths have bugs**.
The unmodified run hits both paths depending on real winding sign, so
artifacts are the union; forcing one sign exposes only that path's
bugs, but on every prim that flips into it.

Translation in the recompilation: the source line is
`int _bc_80024D3C = ((int32_t)cpu->t6 < 0);` at
`generated/PSX.EXE_full.c:38246`. `t6` was loaded from
`cpu->gte_data[24]` (MAC0). The two paths are
`block_80024D60` (taken-bltz) and `block_80024D44` (fall-through).
Instrumenting both paths' write streams should localize the bug.

Patch reverted in this commit; the env-var trigger is deliberately not
shipped. Reuse by re-applying the diff if needed.
