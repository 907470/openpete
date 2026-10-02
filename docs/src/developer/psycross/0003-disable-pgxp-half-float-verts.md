# 0003 - Disable PGXP (half-float vertex coords)

When `USE_EXTENDED_PRIM_POINTERS=1` (the default on x86_64, required so
the host-side `P_TAG` can hold a 64-bit `next` pointer), PsyCross also
sets `USE_PGXP=1` by default - see `include/PsyX/PsyX_config.h:13`:

```c
#ifndef USE_PGXP
#define USE_PGXP USE_EXTENDED_PRIM_POINTERS
#endif
```

PGXP changes `VERTTYPE` from `short` (the PSX convention) to a `half`
struct that wraps a 16-bit IEEE-754 half-float. Every `POLY_F3`,
`POLY_G3`, `POLY_GT4`, etc. read by `ProcessFlatPoly` /
`ProcessGouraudPoly` / `ProcessTileAndSprt` then converts those 2-byte
fields to `float` via half-float decoding.

The OpenPete bridge in `psyq_thunks.cpp::psyq_DrawOTag` builds host
slots by `memcpy`ing PSX primitive payloads verbatim - the vertex
coordinates remain int16 because that's how the recompiled MIPS code
emits them. Reading those int16 bytes as half-float collapses every
vertex to a near-zero magnitude (e.g. `313 (0x0139) -> ~9.5e-5`) so
*every* triangle degenerates and nothing rasterises. The visible
symptom is a screen filled entirely with the draw env clear colour
(yellow `#DDAD00` for Spyro's title cutscene) despite tens of thousands
of valid `POLY_G3`/`POLY_GT4` primitives reaching `DrawOTag` each frame.

Fix: disable PGXP for `psycross_static` via `target_compile_definitions`.
This keeps `USE_EXTENDED_PRIM_POINTERS=1` (still needed for 64-bit
pointers) but reverts `VERTTYPE` to `short`, matching the bridge's byte
layout. PGXP's high-precision Z transform isn't useful here yet - the
recompiled GTE writes to `cpu->gte_data[]`/`cpu->gte_ctrl[]` and the
bridge syncs those into PsyCross's `gteRegs` only at libgte
boundaries; the PGXP cache machinery would never see the values
either way.

Re-enabling PGXP later requires translating int16 vertex bytes to
half-float in the bridge per primitive type (different layouts for
`POLY_G3`/`POLY_GT3`/`SPRT`/etc.), and likely plumbing through the
recompiled GTE state so PGXP can take advantage of the precision.

## Files changed

- `CMakeLists.txt`
