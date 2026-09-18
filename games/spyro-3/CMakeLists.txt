# games/spyro-3/CMakeLists.txt — Spyro: Year of the Dragon binary.
#
# The targets come from cmake/openpete_game.cmake; this game adds its
# --skip-to-level driver, its freecam descriptor, its engine layout, the
# terrain probe, the terrain feeder's per-game data, and the sub-tick
# orchestrator and interp region table the shared terrain feeder presents
# through.
# Every PSX vaddr routes through the recompiler: no native-decomp TUs, no
# mod SDK.

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-3
    FEATURE_LIST     feature_list.c
    SYMBOLS_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest/spyro3_symbols.toml
    ORCHESTRATOR     renderer/reextract.c
    INTERP_REGIONS   renderer/interp_regions.c
    TERRAIN_GAME     renderer/terrain_game.c
    MOBY_GAME        ${CMAKE_SOURCE_DIR}/games/common/renderer/moby_game_spyro23.c
    PLAYER_GAME      renderer/player_game.c
    SOURCES
        skip_to_level.c
        features/freecam.c

        # The overlay-id boot and restore hooks, shared with Spyro 2.
        ${CMAKE_SOURCE_DIR}/games/common/game_extras_overlay_resync.c

        renderer/layout.c
        # The per-game addresses the shared moby decoders read.
        renderer/moby_game.c
        renderer/portal.c
        renderer/tracers.c
        renderer/glow.c
        renderer/glint.c
        renderer/glint2.c
        renderer/terrain_probe.c
        renderer/text_sprt.c)

# Typed game structs: every decomp typedef struct plus the PSYQ types they
# embed, guest-pointer fields rewritten to uint32_t, pinned by
# _Static_asserts read from manifest/struct_layout.toml (the MIPS layout
# probed by GAME=spyro-3 scripts/regen-struct-layout.sh; committed, so no
# build needs the MIPS compiler). The generator refuses a toml probed from
# other headers than the checkout's. renderer/game_structs_check.c
# compiles the header so every pin is checked on every build.
set(_spyro3_structs_gen ${CMAKE_CURRENT_BINARY_DIR}/game_structs_gen)
file(GLOB _spyro3_decomp_headers CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/spyro-3/include/*.h
     ${CMAKE_CURRENT_SOURCE_DIR}/spyro-3/include/*/*.h)
add_custom_command(
    OUTPUT  ${_spyro3_structs_gen}/openpete_game_structs.h
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_game_structs.py
            --game spyro-3
            --include ${CMAKE_CURRENT_SOURCE_DIR}/spyro-3/include
            --layout ${CMAKE_CURRENT_SOURCE_DIR}/manifest/struct_layout.toml
            --out ${_spyro3_structs_gen}/openpete_game_structs.h
    DEPENDS ${_spyro3_decomp_headers}
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/struct_layout.toml
            ${CMAKE_SOURCE_DIR}/tools/gen_game_structs.py
            ${CMAKE_SOURCE_DIR}/tools/game_struct_config.py
            ${CMAKE_SOURCE_DIR}/tools/psyq_types.py
            ${CMAKE_SOURCE_DIR}/tools/probe_struct_layout.py
    COMMENT "Generating typed game structs for Spyro 3 (gen_game_structs.py)")
target_sources(${OPENPETE_GAME_EXE} PRIVATE
    renderer/game_structs_check.c
    ${_spyro3_structs_gen}/openpete_game_structs.h)
target_include_directories(${OPENPETE_GAME_EXE} PRIVATE ${_spyro3_structs_gen})
