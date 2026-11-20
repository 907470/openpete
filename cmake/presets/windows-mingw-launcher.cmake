# windows-mingw-launcher: windows-mingw + shared recomp module.
# Mod sources are staged but NOT pre-compiled -
# this exe's mods dir is the first-boot-compile validation surface.
# NOTE: no LTO here - GNU LTO DCEs the .def-exported psyq_* thunks
# (DEP crash); cmake/openpete_game.cmake also forces it off on every
# shared-linkage exe target as a guard.
include(${CMAKE_CURRENT_LIST_DIR}/windows-mingw.cmake)
openpete_preset_default(OPENPETE_RECOMP_LINKAGE shared)
openpete_preset_default(OPENPETE_RECOMP_FIRST_BOOT ON)
openpete_preset_default(OPENPETE_STAGE_MOD_DLLS OFF)
openpete_preset_default(OPENPETE_ENABLE_RECORD OFF)
# Marks a distributed build: core/CMakeLists.txt refuses a GPL static
# FFmpeg under it. Not a knob, so set unconditionally.
set(OPENPETE_DIST_PRESET ON)
# Distributed build: --help lists only the user-facing flags. Display-only -
# every debug flag still parses and works here.
openpete_preset_default(OPENPETE_HIDE_DEBUG_FLAGS ON)
