# games/spyro-2/CMakeLists.txt — Spyro 2: Ripto's Rage! binary.
#
# The targets come from cmake/openpete_game.cmake; this game adds its
# --skip-to-level driver and its freecam descriptor. Every PSX vaddr routes
# through the recompiler: no native-decomp TUs, no mod SDK.

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-2
    SOURCES
        skip_to_level.c
        features/freecam.c

        # No engine-layout addresses located yet: the shared renderer TUs link
        # against the empty descriptor.
        ${CMAKE_SOURCE_DIR}/games/common/renderer/layout_none.c)
