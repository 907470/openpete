# games/spyro-2/CMakeLists.txt, Spyro 2: Ripto's Rage! binary.
#
# The targets come from cmake/openpete_game.cmake; this game adds its
# --skip-to-level driver, its freecam descriptor, its engine layout and its
# sub-tick re-extract spine (ORCHESTRATOR plus INTERP_REGIONS). Every PSX
# vaddr routes through the recompiler: no native-decomp TUs, no mod SDK.

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-2
    FEATURE_LIST     feature_list.c
    ORCHESTRATOR     renderer/reextract.c
    INTERP_REGIONS   renderer/interp_regions.c
    MOBY_GAME        ${CMAKE_SOURCE_DIR}/games/common/renderer/moby_game_spyro23.c
    PLAYER_GAME      ${CMAKE_SOURCE_DIR}/games/common/renderer/player_game_spyro23.c
    SYMBOLS_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest/spyro2_symbols.toml
    SOURCES
        skip_to_level.c
        features/freecam.c

        # The overlay-id boot and restore hooks, shared with Spyro 3.
        ${CMAKE_SOURCE_DIR}/games/common/game_extras_overlay_resync.c

        # The engine layout: the terrain and texture groups, the environment
        # header and animations, the visibility verdict, the fog terms, the
        # draw buffers, the primitive allocator and the camera.
        renderer/layout.c

        # The per-game addresses the shared moby decoders read.
        renderer/moby_game.c

        # The per-game tables the shared player provider reads.
        renderer/player_game.c)
