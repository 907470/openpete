# cmake/openpete_mod_sdk.cmake - one game's mod SDK: generation and staging.
#
# openpete_mod_sdk(<game-id> [ROOT_COPIES]) is called by openpete_add_game
# for a game given MOD_SDK, in the game's directory scope.
#
# Generated into <game build dir>/mod_sdk_gen/ from the game's manifests:
#   openpete_sdk_symbols.h, openpete_sdk_wrappers.h, openpete_game.h and
#   sdk_symbols_table.c (tools/gen_mod_sdk.py; the table is compiled into
#   the executable), and openpete_game_structs.h when
#   manifest/struct_layout.toml exists (tools/gen_game_structs.py).
#
# Staged (every target ALL, since a header edit relinks nothing):
#   <exe>/sdk/                 the game-neutral files (OPENPETE_MOD_SDK_ROOT_FILES)
#                              and shaders/ (OPENPETE_MOD_SDK_SHADERS)
#   <exe>/sdk/games/<id>/      the game's generated headers
#   <exe>/sdk/ (ROOT_COPIES)   the game's generated headers except
#                              openpete_game.h, so a build line using
#                              `-I sdk` alone still compiles
#   <build>/mod-sdk-test/sdk/  the test SDK directory: the root files from
#                              openpete_mod_sdk_test_root, and games/<id>/
#                              from <tag>_mod_sdk_test_stage, which removes
#                              and rewrites only its own game's directory
#
# The mod compiler hashes, compiles against and stamps exactly these files
# (core/mods/mod_loader.c sdk_inputs), resolving each in `games/<id>/`
# before the root.

set(OPENPETE_MOD_SDK_ROOT_FILES
    ${CMAKE_SOURCE_DIR}/core/include/openpete_mod_api.h
    ${CMAKE_SOURCE_DIR}/core/include/openpete_mod_ui.h
    ${CMAKE_SOURCE_DIR}/core/include/psxrt/psx_runtime.h
    ${CMAKE_SOURCE_DIR}/core/mods/sdk/openpete_mod_abi.c
    ${CMAKE_SOURCE_DIR}/core/include/openpete_imgui.h
    ${CMAKE_SOURCE_DIR}/third_party/dear_bindings/dcimgui.h
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imconfig.h
    ${OPENPETE_IMGUI_TABLE_DIR}/openpete_imgui_table.h
    ${OPENPETE_IMGUI_TABLE_DIR}/openpete_imgui_macros.h)
set(OPENPETE_MOD_SDK_SHADERS
    ${CMAKE_SOURCE_DIR}/core/include/openpete_shader_api.glsl
    ${CMAKE_SOURCE_DIR}/core/include/openpete_psx_material.glsl
    ${CMAKE_SOURCE_DIR}/core/include/openpete_psx_decal.glsl
    ${CMAKE_SOURCE_DIR}/core/rhi/shaders/psx_key_contest.glsl
    ${CMAKE_SOURCE_DIR}/core/rhi/shaders/psx_clut.glsl
    ${CMAKE_SOURCE_DIR}/core/rhi/shaders/psx_texel.glsl
    ${CMAKE_SOURCE_DIR}/core/rhi/shaders/terrain_shade.glsl
    ${CMAKE_SOURCE_DIR}/core/rhi/shaders/portal_cover.glsl
    ${CMAKE_SOURCE_DIR}/core/rhi/shaders/portal_cover_scene.glsl)
set(OPENPETE_MOD_SDK_TEST_DIR ${CMAKE_BINARY_DIR}/mod-sdk-test/sdk)

# The test SDK directory's game-neutral half, defined once for every game.
if(NOT TARGET openpete_mod_sdk_test_root)
    add_custom_target(openpete_mod_sdk_test_root ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${OPENPETE_MOD_SDK_TEST_DIR}/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${OPENPETE_MOD_SDK_ROOT_FILES} ${OPENPETE_MOD_SDK_TEST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${OPENPETE_MOD_SDK_SHADERS} ${OPENPETE_MOD_SDK_TEST_DIR}/shaders
        COMMENT "Staging the game-neutral mod SDK into mod-sdk-test/sdk/"
        VERBATIM)
    # The two ImGui table headers are outputs of a custom command in
    # core/, which a target in another directory cannot name in DEPENDS;
    # the target-level edge orders the generator before the copy.
    add_dependencies(openpete_mod_sdk_test_root openpete_imgui_table)
endif()

function(openpete_mod_sdk GAME_ID)
    cmake_parse_arguments(S "ROOT_COPIES" "" "" ${ARGN})
    string(REPLACE "-" "" _tag "${GAME_ID}")
    set(_exe openpete-${_tag})
    set(_gen ${CMAKE_CURRENT_BINARY_DIR}/mod_sdk_gen)
    set(_manifest ${CMAKE_CURRENT_SOURCE_DIR}/manifest)
    set(_overlay_table ${CMAKE_CURRENT_SOURCE_DIR}/generated/spyro_overlay_table.c)

    set(_protos_args "")
    set(_protos_deps "")
    if(EXISTS ${_manifest}/prototypes.toml)
        set(_protos_args --protos ${_manifest}/prototypes.toml)
        set(_protos_deps ${_manifest}/prototypes.toml)
    endif()
    add_custom_command(
        OUTPUT  ${_gen}/openpete_sdk_symbols.h
                ${_gen}/sdk_symbols_table.c
                ${_gen}/openpete_sdk_wrappers.h
                ${_gen}/openpete_game.h
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_mod_sdk.py
                --game ${GAME_ID}
                --symbols ${_manifest}/symbols.toml
                --overlay-table ${_overlay_table}
                --globals ${_manifest}/globals.toml
                ${_protos_args}
                --out-dir ${_gen}
        DEPENDS ${_manifest}/symbols.toml
                ${_manifest}/globals.toml
                ${_protos_deps}
                ${_overlay_table}
                ${CMAKE_SOURCE_DIR}/tools/gen_mod_sdk.py
        COMMENT "Generating the ${GAME_ID} mod SDK symbols, wrappers and identity (gen_mod_sdk.py)")
    set(_game_files
        ${_gen}/openpete_game.h
        ${_gen}/openpete_sdk_symbols.h
        ${_gen}/openpete_sdk_wrappers.h)

    # Typed game structs: every decomp typedef struct plus the PSYQ types
    # they embed, guest-pointer fields rewritten to uint32_t, pinned by
    # _Static_asserts read from manifest/struct_layout.toml (the MIPS
    # layout probed by GAME=<id> scripts/regen-struct-layout.sh; committed,
    # so no build needs the MIPS compiler). The generator refuses a toml
    # probed from other headers than the checkout's.
    if(EXISTS ${_manifest}/struct_layout.toml)
        file(GLOB _decomp_headers CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/${GAME_ID}/include/*.h
             ${CMAKE_CURRENT_SOURCE_DIR}/${GAME_ID}/include/*/*.h)
        add_custom_command(
            OUTPUT  ${_gen}/openpete_game_structs.h
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_game_structs.py
                    --game ${GAME_ID}
                    --include ${CMAKE_CURRENT_SOURCE_DIR}/${GAME_ID}/include
                    --layout ${_manifest}/struct_layout.toml
                    --out ${_gen}/openpete_game_structs.h
            DEPENDS ${_decomp_headers}
                    ${_manifest}/struct_layout.toml
                    ${CMAKE_SOURCE_DIR}/tools/gen_game_structs.py
                    ${CMAKE_SOURCE_DIR}/tools/game_struct_config.py
                    ${CMAKE_SOURCE_DIR}/tools/psyq_types.py
                    ${CMAKE_SOURCE_DIR}/tools/probe_struct_layout.py
            COMMENT "Generating typed game structs for ${GAME_ID} (gen_game_structs.py)")
        list(APPEND _game_files ${_gen}/openpete_game_structs.h)
        # Compiling the header runs every layout pin on every build.
        target_sources(${_exe} PRIVATE
            ${CMAKE_SOURCE_DIR}/games/common/game_structs_check.c)
    endif()

    # Per-overlay sorted entry addresses (the `case` labels of each
    # overlay's generated dispatch), which override_ovl checks a binding
    # against; compiled into the exe beside the symbol table.
    file(GLOB _overlay_dispatch CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/generated/*/*.bin_dispatch.c)
    add_custom_command(
        OUTPUT  ${_gen}/mod_overlay_entries.c
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_overlay_entries.py
                --generated ${CMAKE_CURRENT_SOURCE_DIR}/generated
                --out ${_gen}/mod_overlay_entries.c
        DEPENDS ${_overlay_table} ${_overlay_dispatch}
                ${CMAKE_SOURCE_DIR}/tools/gen_overlay_entries.py
        COMMENT "Generating the ${GAME_ID} overlay entry arrays (gen_overlay_entries.py)")

    target_sources(${_exe} PRIVATE ${_gen}/sdk_symbols_table.c
                                   ${_gen}/mod_overlay_entries.c)
    target_include_directories(${_exe} PRIVATE ${_gen})
    add_custom_target(${_tag}_game_structs DEPENDS ${_game_files})
    add_dependencies(${_exe} ${_tag}_game_structs)
    add_dependencies(openpete_generated_headers ${_tag}_game_structs)

    set(_root_copy_cmd "")
    if(S_ROOT_COPIES)
        set(_root_copies ${_game_files})
        list(REMOVE_ITEM _root_copies ${_gen}/openpete_game.h)
        set(_root_copy_cmd COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_root_copies} $<TARGET_FILE_DIR:${_exe}>/sdk)
    endif()
    # The directory is removed first: a file dropped from these lists would
    # otherwise survive in the build tree and ship with the archive, since
    # package-dist copies the staged directory whole. Mod build keys hash
    # the SDK file contents, so fresh timestamps force no recompile.
    add_custom_target(${_tag}_sdk_stage ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:${_exe}>/sdk
        COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:${_exe}>/sdk/shaders
                $<TARGET_FILE_DIR:${_exe}>/sdk/games/${GAME_ID}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${OPENPETE_MOD_SDK_ROOT_FILES} $<TARGET_FILE_DIR:${_exe}>/sdk
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${OPENPETE_MOD_SDK_SHADERS} $<TARGET_FILE_DIR:${_exe}>/sdk/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_game_files} $<TARGET_FILE_DIR:${_exe}>/sdk/games/${GAME_ID}
        ${_root_copy_cmd}
        DEPENDS ${_game_files}
        COMMENT "Staging the ${GAME_ID} mod SDK into <exe>/sdk/"
        VERBATIM)
    add_dependencies(${_tag}_sdk_stage ${_exe})

    add_custom_target(${_tag}_mod_sdk_test_stage ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf
                ${OPENPETE_MOD_SDK_TEST_DIR}/games/${GAME_ID}
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${OPENPETE_MOD_SDK_TEST_DIR}/games/${GAME_ID}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_game_files} ${OPENPETE_MOD_SDK_TEST_DIR}/games/${GAME_ID}
        DEPENDS ${_game_files}
        COMMENT "Staging the ${GAME_ID} mod SDK into mod-sdk-test/sdk/games/${GAME_ID}/"
        VERBATIM)
    add_dependencies(${_tag}_mod_sdk_test_stage openpete_mod_sdk_test_root)

    set(OPENPETE_GAME_SDK_GEN   ${_gen}        PARENT_SCOPE)
    set(OPENPETE_GAME_SDK_FILES ${_game_files} PARENT_SCOPE)
endfunction()
