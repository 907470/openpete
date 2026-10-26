# cmake/openpete_mod_sdk.cmake - one game's mod SDK: generation and staging.
#
# openpete_mod_sdk(<game-id>) is called by openpete_add_game for a game
# given MOD_SDK, in the game's directory scope.
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
#
# The game-neutral files include one dispatcher per per-game header except
# openpete_game.h (OPENPETE_MOD_SDK_DISPATCHERS, configured from
# core/mods/sdk/openpete_sdk_dispatch.h.in): a build line using `-I sdk`
# alone reaches it and it includes games/spyro-1/'s header, or another
# game's on that game's OPENPETE_GAME_* macro; a line with
# `-I sdk/games/<id>` first never opens it.
#   <build>/mod-sdk-test/sdk/  the test SDK directory: the root files from
#                              openpete_mod_sdk_test_root, and games/<id>/
#                              from <tag>_mod_sdk_test_stage, which removes
#                              and rewrites only its own game's directory
#
# <exe> is the directory of the executable that runs the game. With the one
# executable (OPENPETE_ONE_EXECUTABLE) every game shares <build>/sdk/, which
# one target, openpete_sdk_stage (openpete_mod_sdk_stage_all, called by
# openpete_link_games), removes and rewrites whole; each game's
# <tag>_sdk_stage depends on it. Per-game targets each rewriting the shared
# root would race, and one removing only its own games/<id>/ would leave a
# file dropped from the root lists in place.
#
# The mod compiler hashes, compiles against and stamps exactly these files
# (core/mods/mod_loader.c sdk_inputs), resolving each in `games/<id>/`
# before the root.

set(OPENPETE_MOD_SDK_DISPATCHERS "")
foreach(_h openpete_sdk_symbols.h openpete_sdk_wrappers.h openpete_game_structs.h)
    set(OPENPETE_SDK_DISPATCH_HEADER ${_h})
    configure_file(${CMAKE_SOURCE_DIR}/core/mods/sdk/openpete_sdk_dispatch.h.in
                   ${CMAKE_BINARY_DIR}/mod_sdk_dispatch/${_h} @ONLY)
    list(APPEND OPENPETE_MOD_SDK_DISPATCHERS ${CMAKE_BINARY_DIR}/mod_sdk_dispatch/${_h})
endforeach()
set(OPENPETE_MOD_SDK_ROOT_FILES
    ${OPENPETE_MOD_SDK_DISPATCHERS}
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
    ${CMAKE_SOURCE_DIR}/core/rhi/shaders/portal_cover_scene.glsl
    ${CMAKE_SOURCE_DIR}/core/rhi/shaders/psx_moby_snap.glsl)
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
    string(REPLACE "-" "" _tag "${GAME_ID}")
    # The game's executable target, the host library its generated tables
    # compile into, and the executable's directory, where the SDK stages
    # (openpete_add_game sets these before the call).
    set(_exe ${OPENPETE_GAME_EXE})
    set(_host ${OPENPETE_GAME_HOST})
    set(_exe_dir ${OPENPETE_GAME_EXE_DIR})
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
        # Compiling the header runs every layout pin on every build. The TU
        # sits beside the header, which it includes relative to itself.
        set(_structs_check ${_gen}/${_tag}_game_structs_check.c)
        file(CONFIGURE OUTPUT ${_structs_check} CONTENT
"/* Generated by cmake/openpete_mod_sdk.cmake - compiles ${GAME_ID}'s typed
 * game structs so their pins run. openpete_game_structs.h ends in one
 * _Static_assert per struct size and per member offset, taken from
 * manifest/struct_layout.toml - the MIPS o32 layout probed from the
 * decompilation's own headers - so a header change that moves a member
 * fails the build instead of silently moving an offsetof. */
#include \"openpete_game_structs.h\"

/* Marks the layout pins as compiled; a translation unit needs one external
 * declaration. */
const int ${_tag}_game_structs_checked = 1;
")
        target_sources(${_host} PRIVATE ${_structs_check})
    endif()

    # Per-overlay sorted entry addresses (the `case` labels of each
    # overlay's generated dispatch), which override_ovl checks a binding
    # against; compiled into the exe beside the symbol table.
    file(GLOB _overlay_dispatch CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/generated/*/*.bin_dispatch.c)
    add_custom_command(
        OUTPUT  ${_gen}/mod_overlay_entries.c
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_overlay_entries.py
                --tag ${_tag}
                --generated ${CMAKE_CURRENT_SOURCE_DIR}/generated
                --out ${_gen}/mod_overlay_entries.c
        DEPENDS ${_overlay_table} ${_overlay_dispatch}
                ${CMAKE_SOURCE_DIR}/tools/gen_overlay_entries.py
        COMMENT "Generating the ${GAME_ID} overlay entry arrays (gen_overlay_entries.py)")

    target_sources(${_host} PRIVATE ${_gen}/sdk_symbols_table.c
                                    ${_gen}/mod_overlay_entries.c)
    add_custom_target(${_tag}_game_structs DEPENDS ${_game_files})
    add_dependencies(${_host} ${_tag}_game_structs)
    add_dependencies(openpete_generated_headers ${_tag}_game_structs)

    if(OPENPETE_ONE_EXECUTABLE)
        # Staged by openpete_sdk_stage with every other game's.
        set_property(GLOBAL APPEND PROPERTY OPENPETE_SDK_GAMES ${GAME_ID})
        set_property(GLOBAL PROPERTY OPENPETE_SDK_FILES_${GAME_ID} ${_game_files})
        add_custom_target(${_tag}_sdk_stage ALL)
        add_dependencies(${_tag}_sdk_stage openpete_sdk_stage)
    else()
        # The directory is removed first: a file dropped from these lists
        # would otherwise survive in the build tree and ship with the
        # archive, since package-dist copies the staged directory whole.
        # Mod build keys hash the SDK file contents, so fresh timestamps
        # force no recompile.
        add_custom_target(${_tag}_sdk_stage ALL
            COMMAND ${CMAKE_COMMAND} -E rm -rf ${_exe_dir}/sdk
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${_exe_dir}/sdk/shaders
                    ${_exe_dir}/sdk/games/${GAME_ID}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${OPENPETE_MOD_SDK_ROOT_FILES} ${_exe_dir}/sdk
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${OPENPETE_MOD_SDK_SHADERS} ${_exe_dir}/sdk/shaders
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_game_files} ${_exe_dir}/sdk/games/${GAME_ID}
            DEPENDS ${_game_files}
            COMMENT "Staging the ${GAME_ID} mod SDK into <exe>/sdk/"
            VERBATIM)
        add_dependencies(${_tag}_sdk_stage ${_exe})
    endif()

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

# The one executable's SDK, <build>/sdk/: removed, then the game-neutral
# files, then each game's generated headers under games/<id>/. Defined
# once, after every game.
function(openpete_mod_sdk_stage_all)
    get_property(_games GLOBAL PROPERTY OPENPETE_SDK_GAMES)
    if(NOT _games)
        return()
    endif()
    set(_dir ${CMAKE_BINARY_DIR}/sdk)
    set(_cmds "")
    set(_deps "")
    foreach(_g IN LISTS _games)
        get_property(_files GLOBAL PROPERTY OPENPETE_SDK_FILES_${_g})
        list(APPEND _cmds
             COMMAND ${CMAKE_COMMAND} -E make_directory ${_dir}/games/${_g}
             COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_files} ${_dir}/games/${_g})
        list(APPEND _deps ${_files})
    endforeach()
    add_custom_target(openpete_sdk_stage ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_dir}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_dir}/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${OPENPETE_MOD_SDK_ROOT_FILES} ${_dir}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${OPENPETE_MOD_SDK_SHADERS} ${_dir}/shaders
        ${_cmds}
        DEPENDS ${_deps}
        COMMENT "Staging every game's mod SDK into sdk/"
        VERBATIM)
    add_dependencies(openpete_sdk_stage openpete openpete_imgui_table)
endfunction()
