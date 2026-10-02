# 0005 - MinGW: include MSVC-implicit headers explicitly

PsyCross builds clean under MSVC because two of its system-header pulls
drag in transitively-included declarations that MinGW's headers do not.
The cross-compile under `gcc-mingw-w64-x86-64` (verified against GCC 10
with the `windows-mingw` preset) breaks at compile time without these.

## 1. `src/PsyX_main.cpp` - `<windows.h>`

The `Spew_*` console-color helpers (added under `#ifdef _DEBUG`) reference
`WORD`, `BACKGROUND_INTENSITY`, `InitializeCriticalSection`,
`OutputDebugStringA`, etc. These come from `<windows.h>`.

Original code includes only `<pla.h>` (Performance Logs and Alerts API).
MSVC's `<pla.h>` transitively pulls in `<windows.h>` via its OLE/COM
dependency chain, so the spew helpers see all the symbols they need.

MinGW's `<pla.h>` (under `/usr/x86_64-w64-mingw32/include/pla.h`) is a
minimal stub: just the `_AutoPathFormat` enum and a few PLA interface
declarations, with no `<windows.h>` chain. Result: every Win32 type
referenced by the spew code becomes an `unknown identifier` error.

Fix: explicitly `#include <windows.h>` immediately before `<pla.h>`. The
ordering doesn't matter under MSVC (idempotent) and is required under
MinGW.

## 2. `src/render/PsyX_render.cpp` - `<math.h>`

`GR_Perspective3D` calls `sinf` / `cosf`. These are declared in
`<math.h>`. Original code never explicitly includes `<math.h>` - under
MSVC they reach the user via `<windows.h>` -> CRT chain or via
`<assert.h>` -> `<float.h>` chain (varies by compiler version).

MinGW's `<assert.h>` doesn't pull in `<math.h>`, so `sinf`/`cosf` are
undeclared. GCC's "did you mean sinF" diagnostic flagged the local
variable `sinF` (capital F) as the closest match.

Fix: `#include <math.h>` next to the other system-header includes.

## Files changed

- `src/PsyX_main.cpp` - explicit `#include <windows.h>` inside the
  existing `#ifdef _WIN32` block.
- `src/render/PsyX_render.cpp` - explicit `#include <math.h>` next to
  `<assert.h>` / `<string.h>`.

## Upstream consideration

Both fixes are strict improvements: they don't change behaviour under
MSVC (the headers are already pulled in transitively), and they make the
TUs portable across the MinGW toolchain. Reasonable candidates for a
PsyCross upstream PR.
