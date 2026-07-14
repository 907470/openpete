# windows-mingw-shared: windows-mingw + shared recomp module
# (distribution D1). Mod sources are staged but NOT pre-compiled —
# this exe's mods dir is the first-boot-compile validation surface.
# NOTE: no LTO here — GNU LTO DCEs the .def-exported psyq_* thunks
# (DEP crash); games/spyro-1/CMakeLists.txt also forces it off on the
# exe target as a guard.
include(${CMAKE_CURRENT_LIST_DIR}/windows-mingw.cmake)
openpete_preset_default(OPENPETE_RECOMP_LINKAGE shared)
openpete_preset_default(OPENPETE_STAGE_MOD_DLLS OFF)
openpete_preset_default(OPENPETE_ROM_FREE_ICON ON)
openpete_preset_default(OPENPETE_ENABLE_RECORD OFF)
