# games/spyro-3/CMakeLists.txt - Spyro: Year of the Dragon binary.
#
# The targets come from cmake/openpete_game.cmake; this game adds its
# --skip-to-level driver, its freecam descriptor, its engine layout, the
# terrain probe, the terrain feeder's per-game data, and the sub-tick
# orchestrator and interp region table the shared terrain feeder presents
# through.
# Every PSX vaddr routes through the recompiler: no native-decomp TUs, no
# mod SDK.

# App icon, as in Spyro 1. OPENPETE_ROM_FREE_ICON=OFF decodes frame 0 of the
# memory-card save-file header (D_title_8007ECA0 in the title overlay, 512
# bytes: "SC", 3 icon frames, CLUT at +0x60, frame 0 at +0x80; the options
# and loading overlays carry identical copies), sliced at build time
# from the decomp's data listing by tools/extract_save_header.py; the bytes
# are game data and stay out of the dist presets. OPENPETE_ROM_FREE_ICON=ON
# emits the procedural mark from tools/gen_dist_icon.py.
option(OPENPETE_ROM_FREE_ICON
       "Use the original (non-ROM) distribution icon" OFF)
set(_spyro3_icon_c ${CMAKE_CURRENT_BINARY_DIR}/gen/openpete_spyro3_icon.c)
if(OPENPETE_ROM_FREE_ICON)
    set(_spyro3_icon_tool ${CMAKE_SOURCE_DIR}/tools/gen_dist_icon.py)
    set(_spyro3_icon_args --game spyro-3)
    set(_spyro3_icon_deps ${_spyro3_icon_tool})
else()
    set(_spyro3_icon_asm
        ${CMAKE_CURRENT_SOURCE_DIR}/spyro-3/asm/data/overlays/title/title1.data.s)
    set(_spyro3_icon_src ${CMAKE_CURRENT_BINARY_DIR}/gen/save_header.bin)
    set(_spyro3_icon_tool ${CMAKE_SOURCE_DIR}/tools/mcs_icon_to_ico.py)
    set(_spyro3_icon_args ${_spyro3_icon_src})
    set(_spyro3_icon_deps ${_spyro3_icon_src} ${_spyro3_icon_tool})
    add_custom_command(
        OUTPUT  ${_spyro3_icon_src}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/extract_save_header.py
                ${_spyro3_icon_asm} --vaddr 0x8007ECA0 --size 512
                --out ${_spyro3_icon_src}
        DEPENDS ${_spyro3_icon_asm}
                ${CMAKE_SOURCE_DIR}/tools/extract_save_header.py
        COMMENT "Extracting the Spyro 3 save-file header")
endif()
add_custom_command(
    OUTPUT  ${_spyro3_icon_c}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${_spyro3_icon_tool} ${_spyro3_icon_args}
            --c ${_spyro3_icon_c}
    DEPENDS ${_spyro3_icon_deps}
    COMMENT "Generating openpete_spyro3_icon.c")

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-3
    FEATURE_LIST     feature_list.c
    ICON_SOURCE      ${_spyro3_icon_c}
    SYMBOLS_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest/spyro3_symbols.toml
    ORCHESTRATOR     renderer/reextract.c
    INTERP_REGIONS   renderer/interp_regions.c
    TERRAIN_GAME     renderer/terrain_game.c
    MOBY_GAME        ${CMAKE_SOURCE_DIR}/games/common/renderer/moby_game_spyro23.c
    PLAYER_GAME      ${CMAKE_SOURCE_DIR}/games/common/renderer/player_game_spyro23.c
    SOURCES
        skip_to_level.c
        features/freecam.c

        # The queue of host-requested calls into the game's transition
        # functions, drained at Draw's entry, and --skip-to-area and
        # --skip-to-cutscene on it.
        features/transition_call.c
        features/skip_to_area.c
        features/cutscene_skip.c

        # The dev-overlay level warp (M) and its shared overlay half.
        features/levelwarp.c
        ${CMAKE_SOURCE_DIR}/games/common/features/levelwarp_ui.c

        # The overlay-id boot and restore hooks, shared with Spyro 2.
        ${CMAKE_SOURCE_DIR}/games/common/game_extras_overlay_resync.c

        renderer/layout.c
        # The per-game addresses the shared moby decoders read.
        renderer/moby_game.c
        # The per-game tables the shared player provider reads.
        renderer/player_game.c
        ${CMAKE_SOURCE_DIR}/games/common/renderer/portal_spyro23.c
        # The 3D glyph text producer: settings menu, OSD toasts, performance
        # readout, drawn with the home worlds' portal-name font.
        ${CMAKE_SOURCE_DIR}/games/common/renderer/text_moby.c
        renderer/portal_game.c
        renderer/terrain_probe.c)

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

# The Win32 icon resource: the same source as the window icon, upscaled into
# a multi-size .ico.
if(WIN32)
    set(_spyro3_icon_ico ${CMAKE_CURRENT_BINARY_DIR}/openpete-spyro3.ico)
    set(_spyro3_icon_rc  ${CMAKE_CURRENT_BINARY_DIR}/openpete-spyro3.rc)
    add_custom_command(
        OUTPUT  ${_spyro3_icon_ico}
        COMMAND python3 ${_spyro3_icon_tool} ${_spyro3_icon_args}
                --ico ${_spyro3_icon_ico}
        DEPENDS ${_spyro3_icon_deps}
        COMMENT "Generating openpete-spyro3.ico")
    file(WRITE ${_spyro3_icon_rc} "1 ICON \"${_spyro3_icon_ico}\"\n")
    target_sources(${OPENPETE_GAME_EXE} PRIVATE
        ${_spyro3_icon_ico} ${_spyro3_icon_rc})
endif()
