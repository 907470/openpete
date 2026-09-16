# cmake/openpete_game.cmake — one game's targets from its directory.
#
# openpete_add_game(<id>
#     [MAIN <file>]                   # entry TU; default games/common/game_main.cpp
#     [FEATURE_LIST <file>]           # default games/common/feature_list_none.c
#     [ORCHESTRATOR <file>]           # sub-tick re-extract orchestrator; default
#                                     # games/common/renderer/orchestrator_none.c
#     [INTERP_REGIONS <file>]         # interp ring region table; default
#                                     # games/common/renderer/interp_regions_none.c
#     [TERRAIN_GAME <file>]           # the terrain feeder's per-game data; default
#                                     # games/common/renderer/terrain_game_none.c
#     [MOBY_GAME <file>]              # the moby feeder's per-game decoders and data;
#                                     # default games/common/renderer/moby_game_none.c
#     [PLAYER_GAME <file>]            # the player feeder's per-game decoders and data;
#                                     # default games/common/renderer/player_game_none.c
#     [ICON_SOURCE <file>]            # default games/common/game_icon_none.c
#     [MODULE_LINKAGE static|shared]  # default static
#     [SYMBOLS_MANIFEST <file>]       # curated alias table -> gen/<tag>_symbols.gen.h
#     [SOURCES <file>...]             # exe TUs beyond the skeleton
#     [MODULE_SOURCES <file>...])     # module TUs beyond the generated code
#
# Called from games/<id>/CMakeLists.txt; a game whose generated/ holds no
# recompiled output is skipped with a warning. <id> is the directory name
# and the game's config-scope id ("spyro-2"); the target names drop its
# hyphen:
# module `<tag>_recomp_module`, executable `openpete-<tag>`. After the call
# the caller's scope holds OPENPETE_GAME_TAG, OPENPETE_GAME_EXE,
# OPENPETE_GAME_MODULE, OPENPETE_GAME_GENERATED and OPENPETE_GAME_GEN_DIR
# (the build-tree gen/ directory), for the sources a game adds on top.
#
# A game supplies games/<id>/recomp.conf, game_extras_impl.c and
# psyq_game_provides.h; everything else is shared:
#
#  - The module: the recompiled main EXE plus every overlay translated by
#    scripts/regen-recomp.sh. Each overlay is recompiled with a unique
#    --func-prefix and its `psx_dispatch_compiled` is sed-renamed to
#    `psx_dispatch_<name>` so they coexist at link; games/common/
#    overlay_dispatch.c fans calls out to the active overlay through the
#    generated spyro_overlay_table.c, and module_main.c is the descriptor
#    the host resolves.
#  - The executable: the entry TU, the generated identity TU
#    (tools/gen_game_identity.py over recomp.conf), the shared PSYQ-to-
#    PsyCross layer with the inert default for every hook the game does not
#    name in psyq_game_provides.h, the freecam body, the write census, the
#    shared renderer TUs (the GTE float mirror and the render-path isolation
#    registry) with the game's sub-tick orchestrator or the inert default,
#    and the generated abort-on-call PSYQ stubs with their runtime.
#
# Shared linkage builds the module as <tag>-recomp.{so,dll} for dlopen at
# boot; the exe-side attach TU, export surface and implib are the game's
# (games/spyro-1/CMakeLists.txt).

function(openpete_add_game GAME_ID)
    cmake_parse_arguments(G
        ""
        "MAIN;FEATURE_LIST;ORCHESTRATOR;INTERP_REGIONS;TERRAIN_GAME;MOBY_GAME;PLAYER_GAME;ICON_SOURCE;MODULE_LINKAGE;SYMBOLS_MANIFEST"
        "SOURCES;MODULE_SOURCES"
        ${ARGN})
    if(G_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "openpete_add_game(${GAME_ID}): unknown arguments ${G_UNPARSED_ARGUMENTS}")
    endif()

    string(REPLACE "-" "" _tag "${GAME_ID}")
    set(_exe openpete-${_tag})
    set(_module ${_tag}_recomp_module)
    set(_common ${CMAKE_SOURCE_DIR}/games/common)
    set(_generated ${CMAKE_CURRENT_SOURCE_DIR}/generated)
    set(_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/gen)

    if(NOT EXISTS ${_generated}/PSX.EXE_full.c)
        message(WARNING
            "${GAME_ID}: no recompiled output in generated/ — run "
            "`GAME=${GAME_ID} bash scripts/regen-recomp.sh` first. Skipping target.")
        return()
    endif()

    if(NOT G_MAIN)
        set(G_MAIN ${_common}/game_main.cpp)
    endif()
    if(NOT G_FEATURE_LIST)
        set(G_FEATURE_LIST ${_common}/feature_list_none.c)
    endif()
    if(NOT G_ORCHESTRATOR)
        set(G_ORCHESTRATOR ${_common}/renderer/orchestrator_none.c)
    endif()
    if(NOT G_INTERP_REGIONS)
        set(G_INTERP_REGIONS ${_common}/renderer/interp_regions_none.c)
    endif()
    if(NOT G_TERRAIN_GAME)
        set(G_TERRAIN_GAME ${_common}/renderer/terrain_game_none.c)
    endif()
    if(NOT G_MOBY_GAME)
        set(G_MOBY_GAME ${_common}/renderer/moby_game_none.c)
    endif()
    if(NOT G_PLAYER_GAME)
        set(G_PLAYER_GAME ${_common}/renderer/player_game_none.c)
    endif()
    if(NOT G_ICON_SOURCE)
        set(G_ICON_SOURCE ${_common}/game_icon_none.c)
    endif()
    if(NOT G_MODULE_LINKAGE)
        set(G_MODULE_LINKAGE static)
    endif()

    # CONFIGURE_DEPENDS re-runs the GLOB so newly regenerated overlays are
    # picked up without a manual reconfigure.
    file(GLOB _overlay_full_c     CONFIGURE_DEPENDS "${_generated}/*/*.bin_full.c")
    file(GLOB _overlay_dispatch_c CONFIGURE_DEPENDS "${_generated}/*/*.bin_dispatch.c")

    set(_module_sources
        ${_generated}/PSX.EXE_full.c
        ${_generated}/PSX.EXE_dispatch.c
        ${_overlay_full_c}
        ${_overlay_dispatch_c}
        ${_generated}/spyro_overlay_table.c
        ${_common}/overlay_dispatch.c
        ${_common}/module_main.c
        ${G_MODULE_SOURCES})

    if(G_MODULE_LINKAGE STREQUAL "shared")
        add_library(${_module} SHARED ${_module_sources})
        # Route the exe-exported DATA symbols through the import table
        # (__declspec(dllimport)) instead of PE pseudo-relocation; see
        # psx_runtime.h. Module TUs only; the exe defines those symbols.
        target_compile_definitions(${_module} PRIVATE OPENPETE_RECOMP_MODULE_BUILD)
        set_target_properties(${_module} PROPERTIES
            OUTPUT_NAME "${_tag}-recomp"
            PREFIX ""                       # <tag>-recomp.so, not lib*.so
            POSITION_INDEPENDENT_CODE ON
            # The shipped module is compiled at first boot at plain -O2, so
            # this CMake-built .so/.dll is a dev artifact and gets no LTO
            # even when the preset turns IPO on globally.
            INTERPROCEDURAL_OPTIMIZATION OFF)
        if(NOT WIN32)
            # Bind module-internal references internally, matching PE
            # semantics (no cross-image preemption), and leave the
            # module->host imports to resolve at dlopen against the exe's
            # -rdynamic exports.
            target_link_options(${_module} PRIVATE
                -Wl,-Bsymbolic-functions
                -Wl,--unresolved-symbols=ignore-all)
        endif()
    else()
        add_library(${_module} OBJECT ${_module_sources})
        # Usage requirements only (psx_runtime.h et al.); OBJECT libraries
        # do not embed their dependencies.
        target_link_libraries(${_module} PRIVATE openpete_core)
    endif()
    target_include_directories(${_module} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${_common}
        ${_generated}
        # Include dirs only: a shared module must not link openpete_core (a
        # second copy of core state inside the module would be wrong).
        $<TARGET_PROPERTY:openpete_core,INTERFACE_INCLUDE_DIRECTORIES>)
    set_target_properties(${_module} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON)

    # The generated translation units are enormous and entirely machine-
    # written: warnings on them are noise. The hand-written module TUs keep
    # theirs.
    set_source_files_properties(
        ${_generated}/PSX.EXE_full.c
        ${_generated}/PSX.EXE_dispatch.c
        ${_overlay_full_c}
        ${_overlay_dispatch_c}
        PROPERTIES COMPILE_FLAGS "-w")

    # Identity: the game_extras.h identity accessors and the address-only
    # PSYQ hooks, from recomp.conf.
    set(_identity_c ${_gen_dir}/game_identity.c)
    add_custom_command(
        OUTPUT  ${_identity_c}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_game_identity.py
                --game ${GAME_ID}
                --conf ${CMAKE_CURRENT_SOURCE_DIR}/recomp.conf
                --out  ${_identity_c}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/recomp.conf
                ${CMAKE_SOURCE_DIR}/tools/gen_game_identity.py
        COMMENT "Generating ${GAME_ID} game_identity.c (gen_game_identity.py)")

    # Gameplay-state region table: the manifest the oracle hashes, generated
    # into the C header the exe compiles, the flat text the DuckStation
    # capture module reads (-psxtrace-regions), and the PCSX-Redux Lua
    # mirror. A game without the manifest registers an empty table and no
    # oracle golden can verify it.
    set(_regions_manifest ${CMAKE_CURRENT_SOURCE_DIR}/manifest/gameplay_state_regions.toml)
    set(_regions_sources)
    if(EXISTS ${_regions_manifest})
        set(_regions_header ${_gen_dir}/gameplay_state_regions.gen.h)
        set(_regions_flat   ${_gen_dir}/gameplay_state_regions.flat)
        set(_regions_lua    ${_gen_dir}/openpete_state_regions.gen.lua)
        add_custom_command(
            OUTPUT  ${_regions_header} ${_regions_flat} ${_regions_lua}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_state_regions.py
                    --manifest ${_regions_manifest}
                    --out      ${_regions_header}
                    --out-flat ${_regions_flat}
                    --out-lua  ${_regions_lua}
            DEPENDS ${_regions_manifest}
                    ${CMAKE_SOURCE_DIR}/tools/gen_state_regions.py
            COMMENT "Generating ${GAME_ID} gameplay_state_regions.gen.h + flat table + lua mirror")
        set(_regions_sources ${_regions_header})
        # The manifest's direct rows must equal their derivation from the
        # link map and the current expect file: an expect row deleted by a
        # fix re-derives the manifest in the same change.
        add_test(NAME state_regions_derivation_${GAME_ID}
                 COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/derive_state_regions.py
                         --check ${_regions_manifest})
        set_tests_properties(state_regions_derivation_${GAME_ID}
                             PROPERTIES LABELS "fence;oracle")
    endif()

    # Curated PSX symbol aliases: gen/<tag>_symbols.gen.h from the game's
    # alias table, validated by address against globals.toml / symbols.toml
    # (the build fails when an addr is not a manifest symbol start).
    set(_symbols_sources "")
    if(G_SYMBOLS_MANIFEST)
        set(_symbols_header ${_gen_dir}/${_tag}_symbols.gen.h)
        add_custom_command(
            OUTPUT  ${_symbols_header}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_symbols_header.py
                    --symbols   ${G_SYMBOLS_MANIFEST}
                    --globals   ${CMAKE_CURRENT_SOURCE_DIR}/manifest/globals.toml
                    --functions ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
                    --out       ${_symbols_header}
            DEPENDS ${G_SYMBOLS_MANIFEST}
                    ${CMAKE_CURRENT_SOURCE_DIR}/manifest/globals.toml
                    ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
                    ${CMAKE_SOURCE_DIR}/tools/gen_symbols_header.py
            COMMENT "Generating ${_tag}_symbols.gen.h (validated vs manifest)")
        set(_symbols_sources ${_symbols_header})
    endif()

    add_executable(${_exe}
        ${G_MAIN}
        game_extras_impl.c
        ${_identity_c}
        ${_common}/state_regions.c
        ${_regions_sources}
        ${G_FEATURE_LIST}
        ${G_ORCHESTRATOR}
        ${G_INTERP_REGIONS}
        ${G_TERRAIN_GAME}
        ${G_MOBY_GAME}
        ${G_PLAYER_GAME}
        ${G_ICON_SOURCE}
        ${G_SOURCES}
        ${_symbols_sources}

        # The shared PSYQ-to-PsyCross layer, plus the default for every
        # per-game hook this game does not name in psyq_game_provides.h.
        # Listed BEFORE the generated stubs so under MinGW link order lets a
        # thunk's strong definition win against the stub's (paired with
        # --allow-multiple-definition).
        ${_common}/psyq/psyq_thunks.cpp
        ${_common}/psyq/psyq_game_hooks_default.c
        ${_common}/psyq/psyq_thunk_state.c
        ${_common}/write_census.c
        ${_common}/freecam_game.c

        # Shared renderer TUs: the GTE float mirror every feeder projects
        # through, the --render-paths isolation registry, and the terrain,
        # cyclorama, moby and moby-shadow feeders (reached only through a
        # game's feature list).
        ${_common}/renderer/native_gte.c
        ${_common}/renderer/render_paths.c
        ${_common}/renderer/portal_plane.c
        ${_common}/renderer/terrain.c
        ${_common}/renderer/cyclorama.c
        ${_common}/renderer/shadow.c
        ${_common}/renderer/moby.c
        ${_common}/renderer/moby_rank.c
        ${_common}/renderer/player.c
        ${_common}/renderer/interp_regions_gen.c

        # PSYQ symbol stubs, abort-on-call. On ELF these are weak so any
        # thunk strongly defines them; on MinGW the macro becomes strong and
        # the link order plus --allow-multiple-definition resolves the
        # collision in the thunk's favour. psyq_stubs.c stays exe-side: PE
        # has no dynamic symbol preemption, so module-side weak stubs would
        # shadow the strong thunks on Windows.
        ${_generated}/psyq_stubs.c
        ${_common}/psyq/psyq_stub_runtime.c)

    # Generated C addresses psx_runtime.h via the include path provided by
    # openpete_core (which adds psxrecomp/runner/include).
    target_include_directories(${_exe} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${_common}
        ${_generated}
        ${_gen_dir})
    if(NOT EXISTS ${_regions_manifest})
        set_source_files_properties(${_common}/state_regions.c PROPERTIES
            COMPILE_DEFINITIONS OPENPETE_STATE_REGIONS_NONE)
    endif()
    set_target_properties(${_exe} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)

    if(G_MODULE_LINKAGE STREQUAL "static")
        target_link_libraries(${_exe} PRIVATE ${_module})
    else()
        target_link_libraries(${_exe} PRIVATE ${CMAKE_DL_LIBS})
    endif()
    target_link_libraries(${_exe} PRIVATE openpete_core)

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
        target_link_options(${_exe} PRIVATE -Wl,--allow-multiple-definition)
        # OpenAL on Windows is always a runtime DLL load (no static .a in the
        # openal-soft prebuilt). Copy the renamed soft_oal.dll next to the
        # .exe so the loader resolves OpenAL32.dll without a system install;
        # without it the process fails to start before main() runs.
        add_custom_command(TARGET ${_exe} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/third_party/OpenAL-mingw/x86_64-w64-mingw32/bin/OpenAL32.dll"
                "$<TARGET_FILE_DIR:${_exe}>/OpenAL32.dll"
            VERBATIM)
    endif()

    # Backtrace symbols: non-static functions resolve to names in the crash
    # handler's backtrace_symbols_fd output, and a shared module resolves
    # its host imports against these exports.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(${_exe} PRIVATE -rdynamic)
    endif()

    # SDL3's platform entry shim, required by the TU including
    # <SDL3/SDL_main.h>.
    if(TARGET SDL3::SDL3main)
        target_link_libraries(${_exe} PRIVATE SDL3::SDL3main)
    endif()

    set(OPENPETE_GAME_TAG       ${_tag}       PARENT_SCOPE)
    set(OPENPETE_GAME_EXE       ${_exe}       PARENT_SCOPE)
    set(OPENPETE_GAME_MODULE    ${_module}    PARENT_SCOPE)
    set(OPENPETE_GAME_GENERATED ${_generated} PARENT_SCOPE)
    set(OPENPETE_GAME_GEN_DIR   ${_gen_dir}   PARENT_SCOPE)
    set(OPENPETE_GAME_OVERLAY_FULL_C     ${_overlay_full_c}     PARENT_SCOPE)
    set(OPENPETE_GAME_OVERLAY_DISPATCH_C ${_overlay_dispatch_c} PARENT_SCOPE)
endfunction()
