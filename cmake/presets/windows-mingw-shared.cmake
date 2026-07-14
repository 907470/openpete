# windows-mingw-shared: windows-mingw + shared recomp module
# (distribution D1). Mod sources are staged but NOT pre-compiled —
# this exe's mods dir is the first-boot-compile validation surface.
# NOTE: no LTO here — GNU LTO DCEs the .def-exported psyq_* thunks
# (DEP crash); games/spyro-1/CMakeLists.txt also forces it off on the
# exe target as a guard.
include(${CMAKE_CURRENT_LIST_DIR}/windows-mingw.cmake)
# Dist hygiene: __FILE__/assert strings are referenced .rdata that
# package-time stripping can't remove — without this they leak the build
# machine's username/layout. Debug info gets the same mapping (and is
# stripped at package time anyway).
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffile-prefix-map=${CMAKE_SOURCE_DIR}=openpete -ffile-prefix-map=${CMAKE_BINARY_DIR}=openpete-build")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffile-prefix-map=${CMAKE_SOURCE_DIR}=openpete -ffile-prefix-map=${CMAKE_BINARY_DIR}=openpete-build")
openpete_preset_default(OPENPETE_RECOMP_LINKAGE shared)
openpete_preset_default(OPENPETE_STAGE_MOD_DLLS OFF)
openpete_preset_default(OPENPETE_ROM_FREE_ICON ON)
openpete_preset_default(OPENPETE_ENABLE_RECORD OFF)
