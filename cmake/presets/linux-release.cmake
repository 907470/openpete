# linux-release: the default dev build (full-recompilation mode).
include(${CMAKE_CURRENT_LIST_DIR}/_helpers.cmake)
set(CMAKE_BUILD_TYPE RelWithDebInfo)
openpete_preset_default(OPENPETE_SKIP_DECOMP ON)
