# games/spyro-3/CMakeLists.txt — Spyro: Year of the Dragon binary.
#
# The targets come from cmake/openpete_game.cmake; this game adds its
# --skip-to-level driver, its freecam descriptor, its engine layout, the
# terrain probe, and the sub-tick orchestrator and interp region table the
# shared terrain feeder presents through.
# Every PSX vaddr routes through the recompiler: no native-decomp TUs, no
# mod SDK.

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-3
    FEATURE_LIST     feature_list.c
    SYMBOLS_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest/spyro3_symbols.toml
    ORCHESTRATOR     renderer/reextract.c
    INTERP_REGIONS   renderer/interp_regions.c
    SOURCES
        skip_to_level.c
        features/freecam.c
        renderer/layout.c
        renderer/terrain_probe.c)
