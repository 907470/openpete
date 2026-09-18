# games/spyro-2/CMakeLists.txt, Spyro 2: Ripto's Rage! binary.
#
# The targets come from cmake/openpete_game.cmake; this game adds its
# --skip-to-level driver, its freecam descriptor and its engine layout. Every
# PSX vaddr routes through the recompiler: no native-decomp TUs, no mod SDK.

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-2
    FEATURE_LIST     feature_list.c
    SYMBOLS_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest/spyro2_symbols.toml
    SOURCES
        skip_to_level.c
        features/freecam.c

        # The overlay-id boot and restore hooks, shared with Spyro 3.
        ${CMAKE_SOURCE_DIR}/games/common/game_extras_overlay_resync.c

        # The engine layout: the terrain and texture groups, the environment
        # header and animations, the visibility verdict, the fog terms, the
        # draw buffers, the primitive allocator and the camera.
        renderer/layout.c)
