# Missing geometry: the DR_TPAGE drawenv bug

A PsyCross bug that made an arbitrary suffix of an OT chain render
invisibly. Solved; the fix is in the PsyCross fork. This page records the
root cause, the instruments built to find it, and the reason path-forcing
experiments produce misleading evidence against this class of bug.

## Symptom

Two reproducers, one cause:

- `games/spyro-1/bk2/missing-geometry.bk2` — third-person camera circling
  Spyro in Artisans Homeworld. Parts of Spyro's mesh, terrain edges, and
  bridge facets disappear and reappear from one present to the next.
  Also reproduced in Stone Hill and Dark Hollow.
- `games/spyro-1/bk2/missing-world.bk2` — hold triangle for the
  first-person camera. World geometry past a distance stops rendering.
  Spyro is hidden in first person, so his renderer (`func_80023AC4`) is
  not involved.

Geometry vanishes rather than being chopped, so the loss is not clipping.

## Root cause

`ProcessDrawEnv` in `third_party/PsyCross/src/gpu/PsyX_GPU.cpp` honoured
bit 10 of an inline `DR_TPAGE` primitive and wrote it to
`activeDrawEnv.dfe`. Two unrelated meanings share that name:

- GP0(E1) bit 10 on PSX hardware: drawing to the display area allowed
  (`0` = prohibited, `1` = allowed). A write-gating flag.
- `activeDrawEnv.dfe` inside PsyCross: selects the screen-coordinate remap
  mode. `DrawEnvDimensions` in that file picks disp-rect vs clip-rect
  dimensions from it, `DrawEnvOffset` picks the disp-relative offset vs
  zero, and `DrawSplit` derives each split's clip/offscreen state from it.

Spyro's renderers emit `DR_TPAGE` with bit 10 clear. When such a prim
entered the chain, every prim after it was scaled and offset with the
wrong rect, transforming far geometry off screen. Which prims were
affected depended on where in the OT chain the `DR_TPAGE` landed, which is
why the loss looked per-polygon and camera-dependent.

The observed prim was code `0xE1000220` at OT slot 0, the deepest bucket
and therefore the first prim drawn, which corrupted the whole chain.

## Fix

`ProcessDrawEnv` updates `tpage` and `dtd` from an inline `DR_TPAGE` and
no longer assigns `dfe`. The coord-remap mode is owned by `PutDrawEnv`,
which the game calls once per drawn tick (`spyro-1/src/gamestates/draw.c:2762`),
so it is now stable for the whole chain.

The change lives on the fork's `openpete` branch, not in a build-time
patch. It has no entry in `docs/psycross/`.

## Instruments

Built for this investigation, still present, useful for any
"prims reach the chain but nothing draws" question.

`OPENPETE_OT_DUMP_FRAMES=A-B`, `OPENPETE_OT_DUMP_PATH=<path>`
(`games/spyro-1/psyq_thunks.cpp:645-685`, default `/tmp/ot-dump.txt`)
writes one row per OT slot per `DrawOTag` call over an `openpete_frame`
window: `frame,slot,call,psx_addr,plen,code,payload_words`. This is what
established that prim count and per-code distribution were unchanged
across the good-to-bad transition, and what found the rogue `DR_TPAGE` by
per-slot inspection.

`OPENPETE_OT_TRUNCATE=N` (`psyq_thunks.cpp:724-741`) submits only the
first N slots. Slot indices line up 1:1 with the OT dump's `slot` column,
so an `-og` capture at successive cutoffs shows which slot a pixel's final
colour comes from.

`OPENPETE_WRITE_TRACE_FRAMES=A-B`, `OPENPETE_WRITE_TRACE_PATH=<path>`
(`games/spyro-1/write_trace.cpp`, default `/tmp/write-trace.csv`) logs
every `cpu->write_word` issued from inside `func_80023AC4` over an
`openpete_frame` window, as `frame,call,addr,value,t9,at,t8`. The hook in
`games/spyro-1/game_extras_impl.c:722-730` swaps `cpu->write_word` for the
logging wrapper on entry to `0x80023AC4` and restores it on return. The
three register columns exist because renderer writes are mostly
`write_word(t9 + N, ...)` or `write_word(at + N, ...)`; differencing
`addr - t9` or `addr - at` recovers the emitting source line.

The write trace is pointed at one hard-coded address. Retargeting it means
editing the dispatch arm in `game_extras_impl.c`.

## Why path-forcing experiments misled

Before the drawenv cause was found, temporary env-gated patches forced
`func_80023AC4` down one of its two polygon-emit paths (NCLIP sign
forced, small-tri emit forced, big-quad emit forced). Every one of them
changed which polygons were visible, and each result read as evidence that
one emit path was miscompiled. None of the patches shipped.

The mechanism: the emit path a polygon takes determines its OT bucket, and
the bucket determines whether the polygon is drawn before or after the
rogue `DR_TPAGE`. Forcing the path moved polygons across that boundary.
Forcing the big-quad path moved most geometry ahead of the corruption,
which read as a fix while also emitting back-facing polygons the NCLIP
path would have culled, visible as Spyro's face through the back of his
head.

The general form: when a bug's effect depends on position in an ordered
chain, any experiment that perturbs the ordering produces a visible
change. That change is not evidence about the perturbed subsystem. An
experiment against such a bug has to hold chain order fixed, or measure
the chain directly (the OT dump) rather than measuring the picture.

Contributing to the same false trail: the GTE was verified bit-identical
across three implementations on the affected VBLs
(`docs/gte-trace-vs-reference.md`), which was true and correctly narrowed
the search downstream of the GTE, but the search then stopped at the
recompiled renderer instead of continuing past it into PsyCross.
