# windows-mingw: Windows x64 cross dev build (run via WSL2 interop).
# -march is pinned to the x86-64 baseline (not left implicit - toolchains vary
# in what they default to) so the distributable runs on any x86-64 CPU. Raising
# the floor to x86-64-v3 (AVX2/FMA/BMI2) buys nothing measurable: the hot path
# is the recompiled game, scalar integer code with no vectorizable loops, and
# v3 would cut off everything before Haswell/Zen1 for no gain.
include(${CMAKE_CURRENT_LIST_DIR}/_helpers.cmake)
set(CMAKE_BUILD_TYPE RelWithDebInfo)
set(CMAKE_C_FLAGS "-march=x86-64")
set(CMAKE_CXX_FLAGS "-march=x86-64")
openpete_preset_default(OPENPETE_SKIP_DECOMP ON)
# Per-function tracing (-finstrument-functions) is a dev-only diagnostic and
# must never ship. Pinned OFF as a tracked knob so no dist build can inherit a
# stale ON; enable it deliberately in a throwaway dir with -DOPENPETE_FUNC_TRACE=ON.
openpete_preset_default(OPENPETE_FUNC_TRACE OFF)
