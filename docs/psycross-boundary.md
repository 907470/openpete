# PsyCross-boundary audit

> Phase 1.5 of the source-rewriter plan (`notes/plans/openpete-source-rewriter.md`).
> Confirms that Pass 1's struct-relayout assumptions hold against PsyCross's
> actual internals before later phases commit to layout changes, and identifies
> APIs whose `u_long *` boundary stride needs Pass 5b normalization.

## 1. `u_long *` taking PsyCross APIs

`u_long` is `unsigned long` (PsyCross/include/psx/types.h:33). 8 bytes on
LP64 (Linux x86_64), 4 bytes on LLP64 (MinGW). Public PsyCross APIs that
take `u_long *` are listed in `libcd.h` / `libgpu.h`. Spyro's matching-decomp
source (under `spyro-1/src/`) calls only two of them from C — the rest only
appear inside MIPS asm or are unused.

| API                         | Used by Spyro C? | Internal stride | Verdict |
|-----------------------------|------------------|-----------------|---------|
| `CdRead(int, u_long*, int)` | yes (`cd.c`)     | byte (`(unsigned char*)buf`) | safe — Pass 5b no-op |
| `LoadImage(RECT16*, u_long*)` | yes (`initialization.c`) | byte (`(unsigned short*)p` then `GR_CopyVRAM`) | safe — Pass 5b no-op |
| `StoreImage`, `LoadImage2`, `StoreImage2` | no | byte (same path as `LoadImage`) | safe |
| `CdReadFile`                | no               | re-enters `CdRead` | safe |
| `OpenTIM`, `OpenTMD`        | no               | byte-walks the TIM blob | safe |
| `LoadClut`, `LoadClut2`, `LoadTPage` | no       | call `LoadImage` | safe |
| `ClearOTag`, `ClearOTagR`, `DrawOTag*`, `DumpOTag`, `BreakDraw`, `FntFlush`, `KanjiFntFlush`, `CheckPrim`, `Krom2Tim` | no — called only from MIPS asm in Spyro | OT_TAG-strided (host layout) | **see §2** |

**Conclusion:** Pass 5b is currently a *no-op* for Spyro's migration set —
neither `CdRead` nor `LoadImage` strides as host `unsigned long`, so no shim
is needed. The pass should still be implemented as a guard so future TUs that
call `OpenTIM`/`ClearOTag`/etc. directly from C surface as a hard error rather
than silently mis-stride.

## 2. OT_TAG layout mismatch

`OT_TAG` (libgpu.h:371) embeds `DECLARE_P_ADDR_PTAG` which on x86_64 expands to:

```c
uintptr_t addr;        /* 8 bytes on x64 */
uint len : 16;
uint pgxp_index : 16;
```

PSX-side `OT_TAG` is `unsigned addr:24, unsigned len:8` = 4 bytes. So the
host struct is 12 bytes vs. 4 bytes on PSX. PsyCross has *already* diverged
the layout to its own host-friendly form (controlled by
`USE_EXTENDED_PRIM_POINTERS` in libgpu.h:330) — this is intentional, not a
bug we can patch out.

**Implication for native migration:** g_ram-aliased OT buffers (e.g.
`g_Buffers.m_HudOTStart`) cannot be passed directly to PsyCross's
`ClearOTag` / `DrawOTag` family from native code. The PSX-laid 4-byte
stride won't accommodate 12-byte host OT_TAG entries.

**Mitigation (deferred):** Spyro doesn't call any OT-tag API from C today —
all of `addPrim`/`setaddr`/`ClearOTag`/`DrawOTag` traffic lives in MIPS asm.
When that migrates, the rewriter will need a `[[psyq_boundary_copy]]` shim
that allocates a host-side `OT_TAG[]` scratch outside g_ram and bridges the
PSX vaddr handle to the host buffer. Track in plan §5 risks.

## 3. PSYQ structs embedded in `spyro-1/include/`

| Type     | Embedded in            | Host vs PSX layout |
|----------|------------------------|--------------------|
| `CdlLOC` | `g_CdState.m_ReadLoc`  | identical (4 × `u_char`) |
| `CdlFILE`| (decl only, no embed)  | identical (CdlLOC + u_int + char[16]) |
| `CdlATV` | `g_CdMusic.m_Aud`      | identical (4 × `u_char`) |
| `RECT`   | macro alias of `RECT16`| identical (4 × `short`) |
| `MATRIX` | gameplay headers       | PsyCross uses `int t[3]` not `long t[3]` — identical |
| `DRAWENV`| `g_DB[N].m_DrawEnv`    | **divergent** — embeds DR_ENV with `uintptr_t addr` field (12 bytes on x64 vs 4 on PSX) |
| `DISPENV`| `g_DB[N].m_DispEnv`    | identical (RECT16×2 + 4×u_char) |

**Implication:** `g_DB`'s `--defsym` aliasing into g_ram is unsafe on the host
side because `DRAWENV.dr_env` has a different size. PsyCross calls (`PutDrawEnv`,
`SetDefDrawEnv`, etc.) take `DRAWENV*` and stride at host size.

Today this is masked because:
- `g_DB` reads/writes from C are field-level (not aggregate copies), and
  the fields up to `b0` agree with PSX layout.
- The `DR_ENV dr_env` tail isn't read by `initialization.c`'s C code — only
  PsyCross's `PutDrawEnv` writes through it, using its own host-laid view.
- If the recomp side reads `g_DB[N].m_DrawEnv` via `gp+offset`, only the PSX-laid
  prefix is in g_ram; the `dr_env` tail in g_ram is never updated by PsyCross.
  Recomp side doesn't appear to read it, so we get away with it.

**Recommended fix (future):** the rewriter detects DRAWENV-bearing globals
and emits a host-laid + PSX-laid pair, with PsyCross writes going to the
host side and recomp-visible reads going to the PSX side. Pre-condition for
migrating any TU that sets up draw envs from C and lets recomp read the
prefix back. `initialization.c` already does this — but only the prefix
fields, so it works today.

## 4. Decisions

- **Pass 5b** lands as a guard pass in Phase 4: walk every `CallExpr` whose
  callee parameter type is `u_long *`, verify the target API is in the safe
  list (§1). Hard-error on any other PsyCross API.
- **OT-tag boundary** is a Phase 7+ concern. No native TU on the migration
  list today calls an OT-tag API from C.
- **DRAWENV `dr_env` tail** is on the watch-list. If a future native TU
  needs the recomp side to observe `dr_env` writes (or vice versa), the
  rewriter needs the dual-layout treatment described above. For now the
  prefix-only access pattern in `initialization.c` is safe.
- No PsyCross patch is required at this time. No `[[psyq_boundary_copy]]`
  shim entry is needed for any current API call.

## 5. Methodology

- Public `u_long *` APIs enumerated by:
  ```sh
  grep -n "u_long.*\*" third_party/PsyCross/include/psx/*.h
  ```
- Spyro C call-sites enumerated by:
  ```sh
  grep -hoE "\b(LoadImage|CdRead|...)\b" spyro-1/src/*.c spyro-1/include/*.h | sort -u
  ```
- PSYQ-struct layout audited via `tools/rewrite/types.py` (the PSX-layout
  calculator) compared against the PsyCross header definitions byte-by-byte
  for primitive-only structs; manual inspection for structs with macro-driven
  layouts (DRAWENV).
