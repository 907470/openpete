# cmake/openpete_game.cmake - one game's targets from its directory.
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
#     [MOD_SDK]                       # generate and stage the game's mod SDK
#                                     # (cmake/openpete_mod_sdk.cmake)
#     [SDK_ROOT_COPIES]               # with MOD_SDK: also stage the generated
#                                     # headers at <exe>/sdk/ root
#     [SOURCES <file>...]             # exe TUs beyond the skeleton
#     [MODULE_SOURCES <file>...])     # module TUs beyond the generated code
#
# Called from games/<id>/CMakeLists.txt; a game whose generated/ holds no
# recompiled output stops the configure with FATAL_ERROR. <id> is the
# directory name and the game's config-scope id ("spyro-2"); the target
# names drop its hyphen:
# module `<tag>_recomp_module`, executable `openpete-<tag>`. After the call
# the caller's scope holds OPENPETE_GAME_TAG, OPENPETE_GAME_EXE,
# OPENPETE_GAME_MODULE, OPENPETE_GAME_GENERATED and OPENPETE_GAME_GEN_DIR
# (the build-tree gen/ directory), for the sources a game adds on top.
#
# A game supplies games/<id>/recomp.conf, game_extras_impl.c and
# psyq_game_provides.h; everything else is shared, including the game_extras
# entries whose body reduces to a game_layout field
# (games/common/game_extras_default.c, always linked):
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

include(${CMAKE_SOURCE_DIR}/cmake/openpete_mod_sdk.cmake)

function(openpete_add_game GAME_ID)
    cmake_parse_arguments(G
        "MOD_SDK;SDK_ROOT_COPIES"
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

    # The recompiled C under generated/ is gitignored and produced only by
    # scripts/regen-recomp.sh. A game listed in OPENPETE_GAMES without it
    # stops the configure; -DOPENPETE_GAMES= configures the core alone and
    # -DOPENPETE_GAMES=<id> narrows the configure to the games that have it.
    if(NOT EXISTS ${_generated}/PSX.EXE_full.c)
        set(_other_games ${OPENPETE_GAMES})
        list(REMOVE_ITEM _other_games ${GAME_ID})
        message(FATAL_ERROR
            "${GAME_ID}: no recompiled output in ${_generated}. "
            "Run `GAME=${GAME_ID} bash scripts/regen-recomp.sh` from the source "
            "directory first; it needs PSXRecomp built under "
            "third_party/psxrecomp/build and the decomp built under "
            "games/${GAME_ID}/${GAME_ID}/build (docs/src/developer/building.md). "
            "To configure without this game: -DOPENPETE_GAMES=\"${_other_games}\" "
            "(an empty list configures the core alone).")
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
        # psx_runtime_internal.h. Module TUs only; the exe defines those symbols.
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

    # Every in-image word-table `jr` in the generated C dispatches through an
    # index switch, or is named in recomp.conf's JUMPTABLE_UNSWITCHED; the
    # regen runs the same check.
    add_test(NAME jumptable_dispatch_${GAME_ID}
             COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_jumptable_dispatch.py
                     --game ${GAME_ID})
    set_tests_properties(jumptable_dispatch_${GAME_ID}
                         PROPERTIES LABELS "fence" SKIP_RETURN_CODE 64)

    # Disc boot-order profile: the sector ranges the disc cache loader
    # reads first, generated into the C table games/common/disc_boot_order.c
    # installs. A game without the manifest loads in file order.
    set(_boot_order_manifest ${CMAKE_CURRENT_SOURCE_DIR}/manifest/disc_boot_order.toml)
    set(_boot_order_sources)
    if(EXISTS ${_boot_order_manifest})
        set(_boot_order_header ${_gen_dir}/disc_boot_order.gen.h)
        add_custom_command(
            OUTPUT  ${_boot_order_header}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/disc_boot_order.py header
                    --manifest ${_boot_order_manifest}
                    --out      ${_boot_order_header}
            DEPENDS ${_boot_order_manifest}
                    ${CMAKE_SOURCE_DIR}/tools/disc_boot_order.py
            COMMENT "Generating ${GAME_ID} disc_boot_order.gen.h")
        set(_boot_order_sources ${_boot_order_header})
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

    # PSYQ abort-on-call stubs for the symbols no thunk defines, filtered
    # from the generated psyq_stubs.c by scanning the thunk-bearing sources
    # (tools/filter_psyq_stubs.py). Every stub left is a strong definition,
    # so the executable links with the linker's duplicate-definition check
    # on and resolves each symbol the same way on ELF and PE: a thunk the
    # scan misses fails the link as a multiple definition, and a name it
    # matches that no source compiles fails it as an undefined reference.
    set(_stubs_filtered ${CMAKE_CURRENT_BINARY_DIR}/psyq_stubs_unthunked.c)
    set(_thunk_sources
        ${G_MAIN}
        ${G_SOURCES}
        ${_common}/psyq/psyq_thunks.cpp
        ${_common}/psyq/psyq_game_hooks_default.c
        ${_common}/psyq/psyq_thunk_state.c
        ${_common}/psyq/psyq_stub_runtime.c)
    add_custom_command(
        OUTPUT  ${_stubs_filtered}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/filter_psyq_stubs.py
                ${_generated}/psyq_stubs.c ${_stubs_filtered} ${_thunk_sources}
        DEPENDS ${_generated}/psyq_stubs.c ${_thunk_sources}
                ${CMAKE_SOURCE_DIR}/tools/filter_psyq_stubs.py
        COMMENT "Filtering ${_tag} PSYQ stubs against the thunks"
        VERBATIM)

    add_executable(${_exe}
        ${G_MAIN}
        game_extras_impl.c
        ${_identity_c}
        ${_common}/state_regions.c
        ${_common}/disc_boot_order.c
        ${_common}/game_extras_default.c
        ${_regions_sources}
        ${_boot_order_sources}
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
        ${_common}/psyq/psyq_thunks.cpp
        ${_common}/psyq/psyq_game_hooks_default.c
        ${_common}/psyq/psyq_thunk_state.c
        ${_common}/write_census.c
        ${_common}/dispatch_census.c
        ${_common}/dispatch_preamble.c
        ${_common}/freecam_game.c
        ${_common}/named_target.c

        # Shared renderer TUs: the GTE float mirror every feeder projects
        # through, the --render-paths isolation registry, and the terrain,
        # cyclorama, moby and moby-shadow feeders (reached only through a
        # game's feature list).
        ${_common}/renderer/native_gte.c
        ${_common}/renderer/hook_bracket.c
        ${_common}/renderer/render_paths.c
        ${_common}/renderer/portal_plane.c
        ${_common}/renderer/terrain.c
        ${_common}/renderer/cyclorama.c
        ${_common}/renderer/shadow.c
        ${_common}/renderer/shadow_player.c
        ${_common}/renderer/flame.c
        # The glow/glint/tracer effect family: five self-linking producers,
        # registered only by a game whose game_layout names their routines.
        ${_common}/renderer/glow.c
        ${_common}/renderer/glint.c
        ${_common}/renderer/zap.c
        ${_common}/renderer/tracers.c
        ${_common}/renderer/particles.c
        ${_common}/renderer/moby.c
        ${_common}/renderer/moby_rank.c
        ${_common}/renderer/moby_rank_cal.c
        ${_common}/renderer/player.c
        ${_common}/renderer/player_ranges.c
        ${_common}/renderer/ui_sprite.c
        # The SPRT text feeder: registered only by a game whose ui_game names a
        # self-linking chain emitter, inert in the others.
        ${_common}/renderer/text_sprt.c
        ${_common}/renderer/interp_regions_gen.c
        ${_common}/renderer/reextract_view.c
        ${_common}/renderer/cut_guard.c

        # PSYQ symbol stubs, abort-on-call, for the symbols no thunk defines
        # (filtered above). They stay exe-side: PE has no dynamic symbol
        # preemption, so module-side stubs would shadow the thunks on
        # Windows.
        ${_stubs_filtered}
        ${_common}/psyq/psyq_stub_runtime.c)

    # The region table and symbol aliases as a target of their own, so the
    # source parsers order after them without waiting for the executable.
    add_custom_target(${_tag}_generated_headers
        DEPENDS ${_regions_sources} ${_boot_order_sources} ${_symbols_sources})
    add_dependencies(${_exe} ${_tag}_generated_headers)
    add_dependencies(openpete_generated_headers ${_tag}_generated_headers)

    # Generated C addresses psx_runtime.h via the include path provided by
    # openpete_core (which adds core/include/psxrt).
    target_include_directories(${_exe} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${_common}
        ${_generated}
        ${_gen_dir})
    if(NOT EXISTS ${_regions_manifest})
        set_source_files_properties(${_common}/state_regions.c PROPERTIES
            COMPILE_DEFINITIONS OPENPETE_STATE_REGIONS_NONE)
    endif()
    if(NOT EXISTS ${_boot_order_manifest})
        set_source_files_properties(${_common}/disc_boot_order.c PROPERTIES
            COMPILE_DEFINITIONS OPENPETE_DISC_BOOT_ORDER_NONE)
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

    # The executable and the recompiled module read guest RAM (uint8_t
    # arrays) through wider pointer casts; -fno-strict-aliasing keeps the
    # optimizer from reordering or dropping those accesses. The first-boot
    # module build (dist/recomp_build.cpp) passes the same flag.
    target_compile_options(${_exe} PRIVATE -fno-strict-aliasing)
    target_compile_options(${_module} PRIVATE -fno-strict-aliasing)

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
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

    if(G_MOD_SDK)
        if(G_SDK_ROOT_COPIES)
            openpete_mod_sdk(${GAME_ID} ROOT_COPIES)
        else()
            openpete_mod_sdk(${GAME_ID})
        endif()
        set(OPENPETE_GAME_SDK_GEN   ${OPENPETE_GAME_SDK_GEN}   PARENT_SCOPE)
        set(OPENPETE_GAME_SDK_FILES ${OPENPETE_GAME_SDK_FILES} PARENT_SCOPE)
    elseif(G_SDK_ROOT_COPIES)
        message(FATAL_ERROR "openpete_add_game(${GAME_ID}): SDK_ROOT_COPIES needs MOD_SDK")
    endif()

    set(OPENPETE_GAME_TAG       ${_tag}       PARENT_SCOPE)
    set(OPENPETE_GAME_EXE       ${_exe}       PARENT_SCOPE)
    set(OPENPETE_GAME_MODULE    ${_module}    PARENT_SCOPE)
    set(OPENPETE_GAME_GENERATED ${_generated} PARENT_SCOPE)
    set(OPENPETE_GAME_GEN_DIR   ${_gen_dir}   PARENT_SCOPE)
    set(OPENPETE_GAME_OVERLAY_FULL_C     ${_overlay_full_c}     PARENT_SCOPE)
    set(OPENPETE_GAME_OVERLAY_DISPATCH_C ${_overlay_dispatch_c} PARENT_SCOPE)
endfunction()
