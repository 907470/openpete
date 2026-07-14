# windows-mingw-opt: windows-mingw + LTO — distributable static exe
# (slow link). --help shows only the user-facing flag block.
include(${CMAKE_CURRENT_LIST_DIR}/windows-mingw.cmake)
openpete_preset_default(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
openpete_preset_default(OPENPETE_HIDE_DEBUG_FLAGS ON)
