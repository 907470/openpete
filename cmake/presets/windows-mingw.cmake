# windows-mingw: Windows x64 cross dev build (run via WSL2 interop).
# -march=x86-64-v3 is the portable perf floor for distributable Windows
# binaries (see notes plans: windows perf tuning).
include(${CMAKE_CURRENT_LIST_DIR}/_helpers.cmake)
set(CMAKE_BUILD_TYPE RelWithDebInfo)
set(CMAKE_C_FLAGS "-march=x86-64-v3")
set(CMAKE_CXX_FLAGS "-march=x86-64-v3")
openpete_preset_default(OPENPETE_SKIP_DECOMP ON)
