# The PsyCross ABI boundary

PsyCross is openpete's re-implementation of the PSYQ SDK. Two kinds of
caller cross into it:

- The PSYQ thunks (`games/common/psyq/psyq_thunks.cpp`), which unpack a
  recompiled MIPS call's registers and re-issue it as a host call. This is
  the path every shipping preset takes.
- Rewritten decomp C, which calls PsyCross directly as native code. This is
  decomp mode, pinned off in every non-debug preset; the source rewriter
  that produces it is dormant (`notes/plans/openpete-source-rewriter.md`,
  `status: blocked`).

The hazard on both paths is the same: PsyCross's public signatures are the
PSYQ ones, but its structs and its `long`-family types are laid out for the
host, while the buffers the game hands it are laid out for the PSX
(ILP32). This document records which types diverge and which APIs are
stride-safe.

## 1. Width-divergent types

`u_long` is `unsigned long` (`third_party/PsyCross/include/psx/types.h:33`):
4 bytes on PSX, 8 on LP64 (Linux x86_64), 4 on LLP64 (MinGW). Every pointer
type is 4 bytes on PSX and 8 on both host models. `tools/rewrite/types.py`
classifies a composite as divergent if any transitive leaf field is one of
these.

A `u_long *` parameter is therefore not by itself a hazard — the hazard is
an implementation that strides the buffer as host `unsigned long`. PsyCross
routines that byte-walk or cast to `unsigned short *` / `unsigned char *`
are safe with a PSX-laid buffer.

## 2. `u_long *`-taking PsyCross APIs

| API | Implementation | Internal stride | Verdict |
|-----|----------------|-----------------|---------|
| `CdRead` | `LIBCD.C:553` | stores `(unsigned char *)buf`; `CdReadSync` `memcpy`s sectors through it | safe |
| `CdReadFile` | `LIBCD.C:351` | re-enters `CdRead` | safe |
| `LoadImage`, `LoadImage2` | `LIBGPU.C:75`, `LIBGPU.C:82` | `(unsigned short *)p` into `GR_CopyVRAM` | safe |
| `StoreImage`, `StoreImage2` | `LIBGPU.C:182`, `LIBGPU.C:188` | `(unsigned short *)p` into `GR_ReadVRAM` | safe |
| `LoadClut`, `LoadClut2`, `LoadTPage` | `LIBGPU.C:642`, `:650`, `:589` | call `LoadImage` | safe |
| `ClearOTag`, `ClearOTagR` | `LIBGPU.C:197`, `:219` | `(OT_TAG *)ot`, then `setaddr`/`setlen` per element | host-strided, see §3 |
| `DrawOTag`, `DrawOTagEnv` | `LIBGPU.C:433`, `:427` | walks the chain as host `OT_TAG` | host-strided, see §3 |
| `BreakDraw`, `FntFlush`, `KanjiFntFlush` | `LIBGPU.C:839`, `:669`, `:663` | no `u_long *` parameter (`u_long *` is the return type) | not a boundary |
| `OpenTIM`, `OpenTMD`, `DrawOTag2`, `DumpOTag`, `CheckPrim`, `Krom2Tim` | declared in `libgpu.h`, no definition in the submodule | — | a call is a link error |

Among the TUs currently enabled in `games/spyro-1/manifest/native.toml`,
`cd.c`'s three `CdRead` calls are the only `u_long *` boundary crossings.
`LoadImage` is called from `spyro-1/src/initialization.c`,
`spyro-1/src/loaders.c`, `spyro-1/src/gamestates/init.c`,
`spyro-1/src/gamestates/draw.c` and `spyro-1/include/overlays/flight.inc.h`,
none of which are enabled.

## 3. `OT_TAG` and the host prim header

Every PsyCross primitive begins with `DECLARE_P_ADDR`, whose expansion is
target-conditional (`libgpu.h:330`-`libgpu.h:384`). On x86_64 and on
wasm32 it is a `uintptr_t addr` plus two 16-bit fields, `P_LEN == 3`
(12 bytes); on other 32-bit hosts `P_LEN == 2` (8 bytes). The wasm32 branch
pads the pointer region deliberately so its layout is byte-identical to
amd64, because openpete's OT walk assumes the 12-byte header. The PSX
header is a single 4-byte word: `unsigned addr:24, unsigned len:8`, which
is what `DECLARE_P_ADDR_PTAG` degrades to when `USE_EXTENDED_PRIM_POINTERS`
is off.

`OT_TAG` (`libgpu.h:390`) is nothing but that header, so it is 12 bytes on
the host against 4 on the PSX. The divergence is intentional: PsyCross
needs a real host pointer per prim, which does not fit in 24 bits.

Consequence: a `g_ram`-aliased OT buffer (`g_Buffers.m_HudOTStart`,
`g_WorldOT`) cannot be handed to `ClearOTag` or the `DrawOTag` family. The
PSX-laid 4-byte stride does not accommodate 12-byte host entries; the
callee would write past the buffer and read garbage tags.

openpete resolves this in the thunk rather than in the buffer.
`psyq_DrawOTag` (`games/common/psyq/psyq_thunks.cpp`) does not forward the PSX
pointer: it walks the PSX-laid chain in `g_ram`, rebuilds it into a
host-side `P_TAG` scratch arena outside `g_ram` (one host slot per PSX
entry, payload copied verbatim, `addr` fields re-chained to the next host
slot, the last pointed at PsyCross's `prim_terminator`), and draws that.
The same scratch is what the native renderer's extraction walk parses.

For rewritten decomp C there is no equivalent bridge. A native TU that
called an OT-tag API would need a per-call copy shim; no enabled TU does.

## 4. PSYQ structs embedded in `spyro-1/include/`

| Type | Embedded in | Host vs PSX layout |
|------|-------------|--------------------|
| `CdlLOC` | `g_CdState.m_ReadLoc` (`spyro-1/include/cd.h:17`) | identical, 4 × `u_char` |
| `CdlFILE` | declared only, not embedded | identical: `CdlLOC` + `u_int` + `char[16]` |
| `CdlATV` | `g_CdMusic.m_Aud` (`spyro-1/include/music.h:22`) | identical, 4 × `u_char` |
| `RECT` | `g_HudEggTargetRect`, `HUD.m_SpriteRect` | identical: `#define RECT RECT16` in `games/spyro-1/decomp/psyq_compat.h:27`, 4 × `short` |
| `MATRIX` | gameplay headers | identical: PsyCross declares `int t[3]`, not `long t[3]` (`libgte.h:10`) |
| `DISPENV` | `g_DB[N].m_DispEnv` | identical: `RECT16` × 2 + 4 × `u_char` (`libgpu.h:752`) |
| `DRAWENV` | `g_DB[N].m_DrawEnv` (`spyro-1/include/graphics.h:16`) | divergent tail, see below |

`DRAWENV` (`libgpu.h:744`) ends in a `DR_ENV dr_env` field.  `DR_ENV`
(`libgpu.h:730`) is `DECLARE_P_ADDR` plus `u_int code[15]`, so it is 72
bytes on x86_64 against 64 on the PSX, and the `uintptr_t` forces 8-byte
alignment, moving the field itself from offset 28 to offset 32. Everything
ahead of it — `clip`, `ofs`, `tw`, `tpage`, `dtd`, `dfe`, `isbg`,
`r0`/`g0`/`b0` — agrees with PSX layout byte for byte.

`g_DB` is aliased into `g_ram` by a linker `--defsym` (`gen_defsyms.py`, via
`scripts/regen-manifest.sh`). That alias is only sound because access to it
is field-level and confined to the agreeing prefix: no aggregate `DRAWENV`
copy is made, and PsyCross's `PutDrawEnv`/`SetDefDrawEnv` write `dr_env`
through their own host-laid view, so the `dr_env` bytes sitting in `g_ram`
are never updated and are never read back.

A future native TU that needs the recompiled side to observe `dr_env`
writes, or the reverse, would need the rewriter to emit a host-laid and a
PSX-laid copy of the global and route each side to its own.

## 5. Pass 5b: the boundary guard

`tools/rewrite/pass_psyq_boundary.py` implements the guard, wired into the
rewriter at `tools/rewrite_tu.py:266`; a violation prints and exits 3.

It walks every `CallExpr` in the TU, and for any callee with a `u_long *`
(canonically `unsigned long *`) parameter, requires the callee to be in
`SAFE_PSYQ_API`. Anything else is a hard error, so the next TU to migrate
surfaces the boundary explicitly instead of mis-striding silently.
`BLOCKED_PSYQ_API` only refines the error message.

Two known limitations:

- The guard filters call sites to those whose location file equals the TU's
  main file. Calls inside a textually included body — `flight.inc.h` calls
  `LoadImage` and `StoreImage` — are invisible to it. Both are on the safe
  list, so nothing is currently missed, but the filter is a hole, not a
  policy.
- `SAFE_PSYQ_API` lists `OpenTIM` and `OpenTMD`, which have no definition
  in the submodule. Calling either fails at link time rather than at the
  guard.

## 6. Re-deriving this

Enumerate the `u_long *` surface:

```sh
grep -n 'u_long *\*' third_party/PsyCross/include/psx/*.h
```

For each hit, read the definition under `third_party/PsyCross/src/psx/` and
classify the stride: a cast to `unsigned char *`, `unsigned short *` or
`uint32_t *` is safe; a cast to `OT_TAG *` or any prim struct is host-laid.

Find the callers that matter — the TUs enabled in
`games/spyro-1/manifest/native.toml`, not the whole decomp corpus:

```sh
grep -nE '\b(LoadImage|CdRead|ClearOTag|DrawOTag)[A-Za-z0-9_]*\s*\(' ../spyro-1/src/<tu>.c
```

Struct layouts: `tools/rewrite/types.py` computes PSX sizes from libclang
under the `mipsel-unknown-elf` target;
`python3 -m tools.rewrite.validate_layouts` checks those sizes against the
symbol gaps in `psx.map`. Macro-driven layouts (`DECLARE_P_ADDR`) need
reading the header, since the expansion is target-conditional.
