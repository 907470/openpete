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

        # The overlay-id boot and restore hooks, shared with Spyro 3.
        ${CMAKE_SOURCE_DIR}/games/common/game_extras_overlay_resync.c

        # The engine layout: the camera pose and the player position base;
        # every other field 0.
        renderer/layout.c)
