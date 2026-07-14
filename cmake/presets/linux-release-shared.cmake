# linux-release-shared: linux-release + shared recomp module
# (distribution D1). ROM-free icon, no --record (drops libav deps).
include(${CMAKE_CURRENT_LIST_DIR}/linux-release.cmake)
openpete_preset_default(OPENPETE_RECOMP_LINKAGE shared)
openpete_preset_default(OPENPETE_ROM_FREE_ICON ON)
openpete_preset_default(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
openpete_preset_default(OPENPETE_ENABLE_RECORD OFF)
