# games/spyro-3/CMakeLists.txt — Spyro: Year of the Dragon binary.
#
# The targets come from cmake/openpete_game.cmake; this game adds its
# --skip-to-level driver and its freecam descriptor. Every PSX vaddr routes
# through the recompiler: no native-decomp TUs, no mod SDK.

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-3
    SOURCES
        skip_to_level.c
        features/freecam.c)
