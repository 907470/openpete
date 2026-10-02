# 0001 - Linux build fixes

Vendored PsyCross targets MSVC + MinGW; building under modern Linux GCC
needs three small changes. Documented here so a future re-vendor can pick
them up (or upstream them).

## Changes

1. **`CMakeLists.txt`** - `find_package(OPENAL REQUIRED)` was the wrong
   case for CMake's standard `FindOpenAL.cmake` module on Linux. Renamed
   to `find_package(OpenAL REQUIRED)` and switched the link/include vars
   to the documented `OPENAL_LIBRARY` / `OPENAL_INCLUDE_DIR`.

2. **`src/PsyX_main.cpp`** - the non-Windows path `#include <strings.h>`
   accidentally resolved to PsyCross's own `include/psx/strings.h`
   (which is on the include path and shadows the system header). That
   shim doesn't declare `strcasecmp`, breaking `_stricmp` on Linux.
   Replaced the include with an explicit `extern "C"` declaration of
   `strcasecmp` - matches what `<strings.h>` would have given us.

3. **`src/gpu/PsyX_GPU.cpp`** - `OT_TAG prim_terminator = { -1, 0 };`
   triggers a narrowing-conversion error under `-std=gnu++17`
   (`-1` -> `uintptr_t`). Cast to `(uintptr_t)-1` to keep the same bit
   pattern (all-ones) without the diagnostic.

## Files changed

- `CMakeLists.txt`
- `src/PsyX_main.cpp`
- `src/gpu/PsyX_GPU.cpp`
