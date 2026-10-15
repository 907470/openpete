# games/spyro-2/CMakeLists.txt, Spyro 2: Ripto's Rage! binary.
#
# The targets come from cmake/openpete_game.cmake; this game adds its
# --skip-to-level driver, its freecam descriptor, its engine layout and its
# sub-tick re-extract spine (ORCHESTRATOR plus INTERP_REGIONS), and its mod
# SDK (MOD_SDK, cmake/openpete_mod_sdk.cmake).

# App icon, as in Spyro 1. OPENPETE_ROM_FREE_ICON=OFF decodes frame 0 of the
# memory-card save-file header (D_8006410C in the EXE data, 512 bytes: "SC",
# 3 icon frames, CLUT at +0x60, frame 0 at +0x80), sliced at build time
# from the decomp's data listing by tools/extract_save_header.py; the bytes
# are game data and stay out of the dist presets. OPENPETE_ROM_FREE_ICON=ON
# emits the procedural mark from tools/gen_dist_icon.py.
option(OPENPETE_ROM_FREE_ICON
       "Use the original (non-ROM) distribution icon" OFF)
set(_spyro2_icon_c ${CMAKE_CURRENT_BINARY_DIR}/gen/openpete_spyro2_icon.c)
if(OPENPETE_ROM_FREE_ICON)
    set(_spyro2_icon_tool ${CMAKE_SOURCE_DIR}/tools/gen_dist_icon.py)
    set(_spyro2_icon_args --game spyro-2)
    set(_spyro2_icon_deps ${_spyro2_icon_tool})
else()
    set(_spyro2_icon_asm
        ${CMAKE_CURRENT_SOURCE_DIR}/spyro-2/asm/data/game.data.s)
    set(_spyro2_icon_src ${CMAKE_CURRENT_BINARY_DIR}/gen/save_header.bin)
    set(_spyro2_icon_tool ${CMAKE_SOURCE_DIR}/tools/mcs_icon_to_ico.py)
    set(_spyro2_icon_args ${_spyro2_icon_src})
    set(_spyro2_icon_deps ${_spyro2_icon_src} ${_spyro2_icon_tool})
    add_custom_command(
        OUTPUT  ${_spyro2_icon_src}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/extract_save_header.py
                ${_spyro2_icon_asm} --vaddr 0x8006410C --size 512
                --out ${_spyro2_icon_src}
        DEPENDS ${_spyro2_icon_asm}
                ${CMAKE_SOURCE_DIR}/tools/extract_save_header.py
        COMMENT "Extracting the Spyro 2 save-file header")
endif()
add_custom_command(
    OUTPUT  ${_spyro2_icon_c}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${_spyro2_icon_tool} ${_spyro2_icon_args}
            --c ${_spyro2_icon_c} --c-name spyro2_game_icon
    DEPENDS ${_spyro2_icon_deps}
    COMMENT "Generating openpete_spyro2_icon.c")

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-2
    FEATURE_LIST     feature_list.c
    ICON_SOURCE      ${_spyro2_icon_c}
    ORCHESTRATOR     renderer/reextract.c
    INTERP_REGIONS   renderer/interp_regions.c
    TERRAIN_GAME     renderer/terrain_game.c
    MOBY_GAME        ${CMAKE_SOURCE_DIR}/games/common/renderer/moby_game_spyro23.c
    PLAYER_GAME      ${CMAKE_SOURCE_DIR}/games/common/renderer/player_game_spyro23.c
    SYMBOLS_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest/spyro2_symbols.toml
    MOD_SDK
    SOURCES
        skip_to_level.c
        features/freecam.c

        # --skip-to-cutscene: the Dragon Shores theater's cutscene call.
        features/cutscene_skip.c

        # The dev-overlay level warp (M) and its shared overlay half.
        features/levelwarp.c
        ${CMAKE_SOURCE_DIR}/games/common/features/levelwarp_ui.c

        # The overlay-id boot and restore hooks, shared with Spyro 3.
        ${CMAKE_SOURCE_DIR}/games/common/game_extras_overlay_resync.c


        # The engine layout: the terrain and texture groups, the environment
        # header and animations, the visibility verdict, the fog terms, the
        # draw buffers, the primitive allocator and the camera.
        renderer/layout.c

        # The per-game addresses the shared moby decoders read.
        renderer/moby_game.c

        # The per-game tables the shared player provider reads.
        renderer/player_game.c

        # The shared portal preview feeder and the three addresses it reaches.
        ${CMAKE_SOURCE_DIR}/games/common/renderer/portal_spyro23.c
        # The 3D glyph text producer: settings menu, OSD toasts, performance
        # readout, drawn with the credits font read from the disc.
        ${CMAKE_SOURCE_DIR}/games/common/renderer/text_moby.c
        ${CMAKE_SOURCE_DIR}/games/common/renderer/wad_subfile.c
        renderer/portal_game.c
    FIRST_BOOT_SOURCES
        # The first-boot module build: each overlay carved from the WAD.WAD
        # entry its converged manifest names, then recompile, compile, link.
        ${CMAKE_SOURCE_DIR}/games/common/recomp_build.cpp
        ${CMAKE_SOURCE_DIR}/games/common/recomp_extract_wad_entry.cpp
    FIRST_BOOT_EXTRACT openpete_extract_wad_entry_inputs)

# SoundTable slot names (tools/gen_sfx_names.py). The decomp declares no
# SoundTable. Each --name slot keys, on the attract demo, a sample whose
# ADPCM bytes equal the sample Spyro 3 keys from the same slot index, and
# takes Spyro 3's decomp name for it; the rest of the 256 slots the loader
# reads are sound_0xNN.
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro2_sfx_names.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_sfx_names.py
            --prefix spyro2 --pad
            --name 0x00=gemChime --name 0x01=gemCollect --name 0x03=flame
            --name 0x06=underwater --name 0x10=spyroSkid
            --name 0x12=spyroStop --name 0x22=jump
            --out ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro2_sfx_names.h
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_sfx_names.py
    COMMENT "Generating SoundTable slot names (gen_sfx_names.py)")
add_custom_target(spyro2_sfx_names DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro2_sfx_names.h)
add_dependencies(${OPENPETE_GAME_HOST} spyro2_sfx_names)
add_dependencies(openpete_generated_headers spyro2_sfx_names)
