# games/spyro-2/CMakeLists.txt, Spyro 2: Ripto's Rage! binary.
#
# The targets come from cmake/openpete_game.cmake; this game adds its
# --skip-to-level driver, its freecam descriptor, its engine layout and its
# sub-tick re-extract spine (ORCHESTRATOR plus INTERP_REGIONS), and its mod
# SDK (MOD_SDK, cmake/openpete_mod_sdk.cmake).

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-2
    FEATURE_LIST     feature_list.c
    ORCHESTRATOR     ${CMAKE_SOURCE_DIR}/games/common/renderer/reextract_spyro23.c
    INTERP_REGIONS   ${CMAKE_SOURCE_DIR}/games/common/renderer/interp_regions_spyro23.c
    TERRAIN_GAME     renderer/terrain_game.c
    MOBY_GAME        ${CMAKE_SOURCE_DIR}/games/common/renderer/moby_game_spyro23.c
    PLAYER_GAME      ${CMAKE_SOURCE_DIR}/games/common/renderer/player_game_spyro23.c
    SYMBOLS_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest/spyro2_symbols.toml
    MOD_SDK
    SOURCES
        skip_to_level.c
        # The moby ops of the mod content registry.
        mods/mod_moby_ops.c
        features/freecam.c

        # --skip-to-cutscene: the Dragon Shores theater's cutscene call.
        features/cutscene_skip.c

        # The dev-overlay level warp (M) and its shared overlay half.
        features/levelwarp.c
        ${CMAKE_SOURCE_DIR}/games/common/features/levelwarp_ui.c

        # The overlay-id boot and restore hooks, shared with Spyro 3.
        ${CMAKE_SOURCE_DIR}/games/common/game_extras_overlay_resync.c
        # The shared pre-tick hook, run over this game's game_frame_pre_hooks;
        # shared with Spyro 3.
        ${CMAKE_SOURCE_DIR}/games/common/game_frame_pre.c


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
        ${CMAKE_SOURCE_DIR}/games/common/renderer/portal_sky_s23.c
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
