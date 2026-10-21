# games/spyro-3/CMakeLists.txt - Spyro: Year of the Dragon binary.
#
# The targets come from cmake/openpete_game.cmake; this game adds its
# --skip-to-level driver, its freecam descriptor, its engine layout, the
# terrain probe, the terrain feeder's per-game data, and the sub-tick
# orchestrator and interp region table the shared terrain feeder presents
# through, and its mod SDK (MOD_SDK, cmake/openpete_mod_sdk.cmake).

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-3
    FEATURE_LIST     feature_list.c
    SYMBOLS_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest/spyro3_symbols.toml
    MOD_SDK
    ORCHESTRATOR     ${CMAKE_SOURCE_DIR}/games/common/renderer/reextract_spyro23.c
    INTERP_REGIONS   ${CMAKE_SOURCE_DIR}/games/common/renderer/interp_regions_spyro23.c
    TERRAIN_GAME     renderer/terrain_game.c
    MOBY_GAME        ${CMAKE_SOURCE_DIR}/games/common/renderer/moby_game_spyro23.c
    PLAYER_GAME      ${CMAKE_SOURCE_DIR}/games/common/renderer/player_game_spyro23.c
    SOURCES
        skip_to_level.c
        # The moby ops of the mod content registry.
        mods/mod_moby_ops.c

        # The watch and mod guard over the anti-piracy scans' code ranges.
        scan_watch.c
        # The entry guard over EXE functions a level overlay stubs out.
        code_patch_guard.c
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

        # OPENPETE_S3_STATE_LOG: the read-only game.state change log.
        features/state_log.c

        # The level overlays' anti-piracy scan observer (read-only).
        features/antipiracy_scan.c

        # The overlay-id boot and restore hooks, shared with Spyro 2.
        ${CMAKE_SOURCE_DIR}/games/common/game_extras_overlay_resync.c
        # The shared pre-tick hook, run over this game's game_frame_pre_hooks;
        # shared with Spyro 2.
        ${CMAKE_SOURCE_DIR}/games/common/game_frame_pre.c


        renderer/layout.c
        # The per-game addresses the shared moby decoders read.
        renderer/moby_game.c
        # The per-game tables the shared player provider reads.
        renderer/player_game.c
        ${CMAKE_SOURCE_DIR}/games/common/renderer/portal_spyro23.c
        ${CMAKE_SOURCE_DIR}/games/common/renderer/portal_sky_s23.c
        # The 3D glyph text producer: settings menu, OSD toasts, performance
        # readout, drawn with the credits font read from the disc.
        ${CMAKE_SOURCE_DIR}/games/common/renderer/text_moby.c
        ${CMAKE_SOURCE_DIR}/games/common/renderer/wad_subfile.c
        renderer/portal_game.c
        renderer/terrain_probe.c
    FIRST_BOOT_SOURCES
        # The first-boot module build: each overlay carved from the WAD.WAD
        # entry its converged manifest names, decrypted and restored as the
        # manifest records, then recompile, compile, link.
        ${CMAKE_SOURCE_DIR}/games/common/recomp_build.cpp
        ${CMAKE_SOURCE_DIR}/games/common/recomp_extract_wad_entry.cpp
    FIRST_BOOT_EXTRACT openpete_extract_wad_entry_inputs)

# SoundTable slot names (tools/gen_sfx_names.py over the decomp's spu.h,
# padded to the 256 slots the loader reads): the sfx resolver's names.
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro3_sfx_names.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_sfx_names.py
            --prefix spyro3 --pad
            --header ${CMAKE_CURRENT_SOURCE_DIR}/spyro-3/include/spu.h
            --out ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro3_sfx_names.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/spyro-3/include/spu.h
            ${CMAKE_SOURCE_DIR}/tools/gen_sfx_names.py
    COMMENT "Generating SoundTable slot names (gen_sfx_names.py)")
add_custom_target(spyro3_sfx_names DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro3_sfx_names.h)
add_dependencies(${OPENPETE_GAME_HOST} spyro3_sfx_names)
add_dependencies(openpete_generated_headers spyro3_sfx_names)
