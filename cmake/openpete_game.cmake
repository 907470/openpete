# cmake/openpete_game.cmake - one game's targets from its directory.
#
# openpete_add_game(<id>
#     [MAIN <file>]                   # entry TU; default games/common/game_main.cpp
#     [FEATURE_LIST <file>]           # the feature list; default none
#     [ORCHESTRATOR <file>]           # sub-tick re-extract orchestrator; default none
#     [INTERP_REGIONS <file>]         # interp ring region table; default none
#     [TERRAIN_GAME <file>]           # the terrain feeder's per-game data; default none
#     [MOBY_GAME <file>]              # the moby feeder's per-game decoders and data;
#                                     # default none
#     [PLAYER_GAME <file>]            # the player feeder's per-game decoders and data;
#                                     # default none
#     [ICON_SOURCE <file>]            # the generated icon TU; default none
#     [MODULE_LINKAGE static|shared]  # default OPENPETE_RECOMP_LINKAGE
#     [FIRST_BOOT_SOURCES <file>...]  # the first-boot module build (shared
#                                     # linkage under OPENPETE_RECOMP_FIRST_BOOT);
#                                     # default none: a missing module is refused
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
# A game supplies games/<id>/recomp.conf and game_extras_impl.c; everything
# else is shared, including the game_extras
# entries whose body reduces to a game_layout field
# (games/common/game_extras_default.c, always linked). game_extras_impl.c
# defines the game's runtime descriptor (core/include/openpete_game_runtime.h)
# as `<tag>_game_runtime`, the name the entry TU registers through the
# OPENPETE_GAME_RUNTIME definition; a slot file above is reached only through
# that descriptor, which names no table for an omitted slot. The descriptor's
# `psyq` member is the game's psyq_game_hooks_t
# (games/common/psyq/psyq_game_hooks.h), which names the generated
# `<tag>_psyq_conf` (game_identity.c) and `<tag>_psyq_stub_table`
# (gen/psyq_stub_table.c).
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
#    PsyCross layer with its per-game hooks forwarded to the game's
#    descriptor, the freecam body, the write census, the shared renderer TUs
#    (the GTE float mirror and the render-path isolation registry) with the
#    orchestrator entries forwarded to the game's descriptor, the game's
#    PSYQ stub address table, and the compile-once abort-on-call PSYQ stubs
#    with their runtime (openpete_psyq_stubs, top-level CMakeLists.txt).
#
# Shared linkage builds the module as <tag>-recomp.{so,dll} beside the
# executable, which loads it at boot (games/common/recomp_attach_shared.c)
# and depends on it, so a built executable always has its module. The
# executable's export surface is generated from the game's psyq.toml and the
# module ABI lists (tools/gen_module_exports.py): a PE .def plus an implib
# the module links against on Windows, and on every platform a keep-alive TU
# that pulls the ABI's archive members into the executable. The host->module
# forwarders are the game's module_imports.h. On ELF the target
# <tag>_module_imports refuses the build unless every module import resolves
# to exactly one definition in the merged image (tools/check_module_imports.py);
# on PE the module's link against the implib is that refusal.

include(${CMAKE_SOURCE_DIR}/cmake/openpete_mod_sdk.cmake)

# The shared-linkage wiring between one game's executable and module.
#
# PE: the executable exports exactly the .def surface, and the module links
# an implib generated from the same .def, so every module import names the
# executable; a module import missing from the .def fails the module link.
# GNU LTO does not treat .def exports as liveness roots (with IPO the
# otherwise-unreferenced ABI surface is dead-code-eliminated and ld emits
# the exports as absolute 0), so IPO is forced off for the executable.
#
# ELF: <tag>_module_imports runs tools/check_module_imports.py over the
# built pair and fails the build on an import with no definition or more
# than one, or a symbol both images define outside the forwarders listed in
# module_imports.h and the four overlay-dispatch entry points.
function(_openpete_game_shared_link TAG EXE MODULE MODULE_DEF IMPORTS_HEADER)
    if(WIN32)
        target_link_options(${EXE} PRIVATE ${MODULE_DEF})
        set_property(TARGET ${EXE} APPEND PROPERTY LINK_DEPENDS ${MODULE_DEF})
        set_target_properties(${EXE} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
        if(NOT CMAKE_DLLTOOL)
            set(CMAKE_DLLTOOL x86_64-w64-mingw32-dlltool)
        endif()
        set(_implib ${CMAKE_CURRENT_BINARY_DIR}/gen/libopenpete-exe-exports.a)
        add_custom_command(
            OUTPUT  ${_implib}
            COMMAND ${CMAKE_DLLTOOL} -d ${MODULE_DEF} -l ${_implib} -D ${EXE}.exe
            DEPENDS ${MODULE_DEF}
            COMMENT "dlltool: ${EXE} export implib for the recomp module")
        add_custom_target(${TAG}_module_implib DEPENDS ${_implib})
        add_dependencies(${MODULE} ${TAG}_module_implib)
        target_link_libraries(${MODULE} PRIVATE ${_implib})
        set(OPENPETE_GAME_MODULE_IMPLIB ${_implib} PARENT_SCOPE)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_stamp ${CMAKE_CURRENT_BINARY_DIR}/gen/${TAG}_module_imports.stamp)
        add_custom_command(
            OUTPUT  ${_stamp}
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_module_imports.py
                    --module $<TARGET_FILE:${MODULE}>
                    --exe    $<TARGET_FILE:${EXE}>
                    --forwarder-header ${IMPORTS_HEADER}
                    --forwarder spyro_set_active_overlay_by_magic
                    --forwarder spyro_get_active_overlay_magic
                    --forwarder spyro_overlay_find
                    --forwarder spyro_overlay_set_resync
                    --stamp  ${_stamp}
            DEPENDS ${MODULE} ${EXE} ${IMPORTS_HEADER}
                    ${CMAKE_SOURCE_DIR}/tools/check_module_imports.py
            COMMENT "Checking ${TAG}-recomp's imports against ${EXE}"
            VERBATIM)
        add_custom_target(${TAG}_module_imports ALL DEPENDS ${_stamp})
    endif()
endfunction()

# The first-boot module build's inputs, staged next to the executable as
# <exe dir>/recomp/: manifest/ (the game's converged manifest
# manifest/recomp/<serial>/, its psyq.toml and, when the regen wrote one, its
# channel_sites.txt), src/ (the module source kit) and tools/ (PSXRecomp when
# built; on Windows the exe-export implib the module links against). The
# serial is recomp.conf's EXE_FILENAME with `_` -> `-` and the dot dropped.
function(_openpete_game_stage_first_boot GAME_ID EXE GENERATED SERIAL)
    set(_manifest ${CMAKE_CURRENT_SOURCE_DIR}/manifest/recomp/${SERIAL})
    if(NOT EXISTS ${_manifest}/manifest.toml)
        message(FATAL_ERROR "openpete_add_game(${GAME_ID}): FIRST_BOOT_SOURCES "
                            "needs the converged manifest ${_manifest} "
                            "(GAME=${GAME_ID} bash scripts/emit-recomp-manifest.sh)")
    endif()
    set(_rdir $<TARGET_FILE_DIR:${EXE}>/recomp)
    set(_manifest_files ${CMAKE_CURRENT_SOURCE_DIR}/manifest/psyq.toml)
    if(EXISTS ${GENERATED}/channel_sites.txt)
        list(APPEND _manifest_files ${GENERATED}/channel_sites.txt)
    endif()
    add_custom_command(TARGET ${EXE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${_manifest} ${_rdir}/manifest
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_manifest_files} ${_rdir}/manifest
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_rdir}/src
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/games/common/overlay_dispatch.c
            ${CMAKE_SOURCE_DIR}/games/common/module_main.c
            ${CMAKE_SOURCE_DIR}/games/common/spyro_overlay_table.h
            ${CMAKE_SOURCE_DIR}/core/include/openpete_recomp_module.h
            ${CMAKE_SOURCE_DIR}/core/include/psxrt/psx_runtime.h
            ${CMAKE_SOURCE_DIR}/core/include/psxrt/psx_runtime_internal.h
            ${_rdir}/src
        VERBATIM)
    if(WIN32)
        set(_psxrecomp_bin ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/build-mingw/PSXRecomp.exe)
    else()
        set(_psxrecomp_bin ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/build/PSXRecomp)
    endif()
    set(_tools "")
    if(EXISTS ${_psxrecomp_bin})
        list(APPEND _tools ${_psxrecomp_bin})
    endif()
    if(WIN32)
        list(APPEND _tools ${OPENPETE_GAME_MODULE_IMPLIB})
    endif()
    if(_tools)
        add_custom_command(TARGET ${EXE} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_rdir}/tools
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_tools} ${_rdir}/tools
            VERBATIM)
    endif()
endfunction()

function(openpete_add_game GAME_ID)
    cmake_parse_arguments(G
        "MOD_SDK;SDK_ROOT_COPIES"
        "MAIN;FEATURE_LIST;ORCHESTRATOR;INTERP_REGIONS;TERRAIN_GAME;MOBY_GAME;PLAYER_GAME;ICON_SOURCE;MODULE_LINKAGE;SYMBOLS_MANIFEST"
        "SOURCES;MODULE_SOURCES;FIRST_BOOT_SOURCES"
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
    if(NOT G_MODULE_LINKAGE)
        set(G_MODULE_LINKAGE ${OPENPETE_RECOMP_LINKAGE})
    endif()
    if(NOT G_MODULE_LINKAGE MATCHES "^(static|shared)$")
        message(FATAL_ERROR "openpete_add_game(${GAME_ID}): MODULE_LINKAGE '${G_MODULE_LINKAGE}' is neither static nor shared")
    endif()
    # The first-boot module build is compiled in only where it can run: a
    # shared module, a preset that ships one, a game that has one.
    set(_first_boot OFF)
    if(G_MODULE_LINKAGE STREQUAL "shared" AND OPENPETE_RECOMP_FIRST_BOOT AND G_FIRST_BOOT_SOURCES)
        set(_first_boot ON)
    endif()

    # The module attach and, for shared linkage, the executable's export
    # surface: the PE .def and the keep-alive TU.
    if(G_MODULE_LINKAGE STREQUAL "shared")
        set(_module_def       ${_gen_dir}/${_tag}_module_exports.def)
        set(_module_keepalive ${_gen_dir}/${_tag}_abi_keepalive.gen.c)
        set(_abi_lists ${_common}/module_abi_exports.txt)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/manifest/module_abi_exports.txt)
            list(APPEND _abi_lists ${CMAKE_CURRENT_SOURCE_DIR}/manifest/module_abi_exports.txt)
        endif()
        set(_abi_args "")
        foreach(_l IN LISTS _abi_lists)
            list(APPEND _abi_args --extras ${_l})
        endforeach()
        add_custom_command(
            OUTPUT  ${_module_def} ${_module_keepalive}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_module_exports.py
                    --psyq   ${CMAKE_CURRENT_SOURCE_DIR}/manifest/psyq.toml
                    ${_abi_args}
                    --tag    ${_tag}
                    --image  ${_exe}.exe
                    --def-out ${_module_def}
                    --keepalive-out ${_module_keepalive}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/manifest/psyq.toml
                    ${_abi_lists}
                    ${CMAKE_SOURCE_DIR}/tools/gen_module_exports.py
            COMMENT "Generating the ${GAME_ID} recomp-module export surface (.def + keep-alive)")
        set(_attach_sources
            ${_common}/recomp_attach_shared.c
            ${_module_keepalive})
        if(_first_boot)
            list(APPEND _attach_sources ${G_FIRST_BOOT_SOURCES})
        endif()
    else()
        set(_attach_sources ${_common}/recomp_attach_static.c)
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

    # The game's guest address for each PSYQ abort-on-call stub, from its
    # generated psyq_stubs.c minus the shared thunk layer's names
    # (tools/filter_psyq_stubs.py table); the stub bodies themselves are the
    # compile-once union in openpete_psyq_stubs.
    set(_stub_table ${_gen_dir}/psyq_stub_table.c)
    add_custom_command(
        OUTPUT  ${_stub_table}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/filter_psyq_stubs.py
                table ${_stub_table} --tag ${_tag}
                --stubs ${_generated}/psyq_stubs.c
                --thunks ${OPENPETE_PSYQ_THUNK_SOURCES}
        DEPENDS ${_generated}/psyq_stubs.c ${OPENPETE_PSYQ_THUNK_SOURCES}
                ${CMAKE_SOURCE_DIR}/tools/filter_psyq_stubs.py
        COMMENT "Generating ${_tag} PSYQ stub address table"
        VERBATIM)

    add_executable(${_exe}
        ${G_MAIN}
        game_extras_impl.c
        ${_identity_c}
        ${_common}/state_regions.c
        ${_common}/disc_boot_order.c
        ${_common}/game_extras_default.c
        ${_common}/game_host_desc.c
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
        ${_attach_sources}
        ${_symbols_sources}

        # The shared PSYQ-to-PsyCross layer and its per-game hooks,
        # forwarded to the game's descriptor.
        ${_common}/psyq/psyq_thunks.cpp
        ${_common}/psyq/psyq_game_hooks.c
        ${_common}/psyq/psyq_sfx_slots.c
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
        ${_common}/renderer/orchestrator.c
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

        # This game's PSYQ stub addresses (generated above).
        ${_stub_table})

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
    # The entry TU registers the game's runtime descriptor by this name.
    target_compile_definitions(${_exe} PRIVATE
        OPENPETE_GAME_RUNTIME=${_tag}_game_runtime)
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
        # The default module path is the executable's directory, where the
        # module target writes; building the executable builds its module.
        add_dependencies(${_exe} ${_module})
        target_compile_definitions(${_exe} PRIVATE
            OPENPETE_GAME_MODULE_NAME="${_tag}-recomp"
            OPENPETE_RECOMP_FIRST_BOOT=$<BOOL:${_first_boot}>)
        _openpete_game_shared_link(${_tag} ${_exe} ${_module}
            ${_module_def} ${CMAKE_CURRENT_SOURCE_DIR}/module_imports.h)
        if(_first_boot)
            # The first-boot pipeline's per-game facts from recomp.conf: the
            # overlay window its generated overlay table carries, and the
            # serial naming the staged manifest.
            file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/recomp.conf _range
                 REGEX "^OVERLAY_RANGE=")
            file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/recomp.conf _exe_name
                 REGEX "^EXE_FILENAME=")
            if(NOT _range MATCHES "^OVERLAY_RANGE=(0x[0-9A-Fa-f]+):(0x[0-9A-Fa-f]+)$")
                message(FATAL_ERROR "openpete_add_game(${GAME_ID}): recomp.conf has no OVERLAY_RANGE=<lo>:<hi>")
            endif()
            set(_window_lo ${CMAKE_MATCH_1})
            set(_window_hi ${CMAKE_MATCH_2})
            if(NOT _exe_name MATCHES "^EXE_FILENAME=\"([A-Z]+)_([0-9]+)\.([0-9]+)\"$")
                message(FATAL_ERROR "openpete_add_game(${GAME_ID}): recomp.conf has no EXE_FILENAME=\"XXXX_NNN.NN\"")
            endif()
            set(_serial "${CMAKE_MATCH_1}-${CMAKE_MATCH_2}${CMAKE_MATCH_3}")
            target_compile_definitions(${_exe} PRIVATE
                OPENPETE_GAME_OVERLAY_WINDOW_LO="${_window_lo}"
                OPENPETE_GAME_OVERLAY_WINDOW_HI="${_window_hi}")
            _openpete_game_stage_first_boot(${GAME_ID} ${_exe} ${_generated} ${_serial})
        endif()
    endif()
    target_link_libraries(${_exe} PRIVATE openpete_core openpete_psyq_stubs)

    # The executable and the recompiled module read guest RAM (uint8_t
    # arrays) through wider pointer casts; -fno-strict-aliasing keeps the
    # optimizer from reordering or dropping those accesses. The first-boot
    # module build (games/common/recomp_build.cpp) passes the same flag.
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

    set(OPENPETE_GAME_FIRST_BOOT ${_first_boot} PARENT_SCOPE)
    set(OPENPETE_GAME_MODULE_IMPLIB "${OPENPETE_GAME_MODULE_IMPLIB}" PARENT_SCOPE)
    set(OPENPETE_GAME_TAG       ${_tag}       PARENT_SCOPE)
    set(OPENPETE_GAME_EXE       ${_exe}       PARENT_SCOPE)
    set(OPENPETE_GAME_MODULE    ${_module}    PARENT_SCOPE)
    set(OPENPETE_GAME_GENERATED ${_generated} PARENT_SCOPE)
    set(OPENPETE_GAME_GEN_DIR   ${_gen_dir}   PARENT_SCOPE)
    set(OPENPETE_GAME_OVERLAY_FULL_C     ${_overlay_full_c}     PARENT_SCOPE)
    set(OPENPETE_GAME_OVERLAY_DISPATCH_C ${_overlay_dispatch_c} PARENT_SCOPE)
endfunction()
