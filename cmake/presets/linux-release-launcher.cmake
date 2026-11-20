# linux-release-launcher: linux-release (shared recomp module) + the
# first-boot module build, no --record (drops libav deps).
include(${CMAKE_CURRENT_LIST_DIR}/linux-release.cmake)
openpete_preset_default(OPENPETE_RECOMP_FIRST_BOOT ON)
openpete_preset_default(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
openpete_preset_default(OPENPETE_ENABLE_RECORD OFF)
# Marks a distributed build: core/CMakeLists.txt refuses a GPL static
# FFmpeg under it. Not a knob, so set unconditionally.
set(OPENPETE_DIST_PRESET ON)
# Distributed build: --help lists only the user-facing flags. Display-only -
# every debug flag still parses and works here.
openpete_preset_default(OPENPETE_HIDE_DEBUG_FLAGS ON)
