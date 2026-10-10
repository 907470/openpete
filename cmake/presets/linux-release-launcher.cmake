# linux-release-launcher: linux-release + shared recomp module.
# ROM-free icon, no --record (drops libav deps).
include(${CMAKE_CURRENT_LIST_DIR}/linux-release.cmake)
# Dist hygiene - same rationale as windows-mingw-launcher.cmake.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffile-prefix-map=${CMAKE_SOURCE_DIR}=openpete -ffile-prefix-map=${CMAKE_BINARY_DIR}=openpete-build")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffile-prefix-map=${CMAKE_SOURCE_DIR}=openpete -ffile-prefix-map=${CMAKE_BINARY_DIR}=openpete-build")
openpete_preset_default(OPENPETE_RECOMP_LINKAGE shared)
openpete_preset_default(OPENPETE_ROM_FREE_ICON ON)
openpete_preset_default(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
openpete_preset_default(OPENPETE_ENABLE_RECORD OFF)
# Marks a distributed build: core/CMakeLists.txt refuses a GPL static
# FFmpeg under it. Not a knob, so set unconditionally.
set(OPENPETE_DIST_PRESET ON)
# Distributed build: --help lists only the user-facing flags. Display-only -
# every debug flag still parses and works here.
openpete_preset_default(OPENPETE_HIDE_DEBUG_FLAGS ON)
