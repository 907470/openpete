# linux-release: the default dev build (full-recompilation mode).
include(${CMAKE_CURRENT_LIST_DIR}/_helpers.cmake)
set(CMAKE_BUILD_TYPE RelWithDebInfo)
# Per-function tracing (-finstrument-functions) is a dev-only diagnostic and
# must never ship. Pinned OFF as a tracked knob so no dist build can inherit a
# stale ON; enable it deliberately in a throwaway dir with -DOPENPETE_FUNC_TRACE=ON.
openpete_preset_default(OPENPETE_FUNC_TRACE OFF)
# Each game's recompiled code is a module beside its executable, loaded at
# boot. The tree builds every module, so a run never compiles one from the
# disc (OPENPETE_RECOMP_FIRST_BOOT stays OFF here).
openpete_preset_default(OPENPETE_RECOMP_LINKAGE shared)
