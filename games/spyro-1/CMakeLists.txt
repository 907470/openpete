# games/spyro-1/CMakeLists.txt — Spyro 1 binary.
#
# The module and executable skeleton come from cmake/openpete_game.cmake;
# this file adds what only this game has: the native-decomp TUs and their
# link-time aliases, the shared-linkage attach and export surface, the mod
# SDK and its docs, the build-time mod compile, the decomp-derived icon, the
# SoundTable names, the state-region tables, the Windows subsystem and ASLR
# choices, and the browser link.

set(SPYRO_GENERATED ${CMAKE_CURRENT_SOURCE_DIR}/generated)

# Build switch: drop the native-decomp TUs entirely and route every PSX
# vaddr through the recompiler. Useful for bisecting decomp-introduced
# regressions and for confirming the recomp path is self-sufficient.
# When ON we omit the rewritten/*.c TUs, native_thunks.cpp, the
# generated native_dispatch.c, and the link-time --defsym aliases; a
# tiny stub provides spyro_native_dispatch() returning 0 so
# psx_override_dispatch_game falls straight through to recomp.
option(OPENPETE_SKIP_DECOMP
       "Skip native-decomp TUs and run the full recomp path"
       OFF)

# Per-function CPU tracing (Tier B of the perf profiler): instrument the main
# generated EXE with -finstrument-functions so func_trace.c can attribute
# self/total time per recompiled func_80XXXXXX. ~1-8 ms/tick overhead — a
# drill-down build, never the baseline. Off by default (zero cost).
# See notes/plans/openpete-perf-profiling.md.
option(OPENPETE_FUNC_TRACE
       "Instrument the generated EXE for per-function CPU tracing"
       OFF)

# Native-decomp TU list. Centralized so OPENPETE_SKIP_DECOMP can drop
# them in one place — both from add_executable() and from the per-source
# flag/include loop further down.
set(SPYRO_NATIVE_SOURCES
    ${SPYRO_GENERATED}/rewritten/main.rewritten.c
    ${SPYRO_GENERATED}/rewritten/cd.rewritten.c
    ${SPYRO_GENERATED}/rewritten/rand.rewritten.c
    ${SPYRO_GENERATED}/rewritten/checkpoint.rewritten.c
    ${SPYRO_GENERATED}/rewritten/moby_interpolation_check.rewritten.c
    ${SPYRO_GENERATED}/rewritten/cyclorama.rewritten.c
    ${SPYRO_GENERATED}/rewritten/environment.rewritten.c
    ${SPYRO_GENERATED}/rewritten/specular_and_metal.rewritten.c
    ${SPYRO_GENERATED}/rewritten/cutscene.rewritten.c
    ${SPYRO_GENERATED}/rewritten/4BEF8.rewritten.c
    ${SPYRO_GENERATED}/rewritten/music.rewritten.c
    ${SPYRO_GENERATED}/rewritten/special_surfaces.rewritten.c
    ${SPYRO_GENERATED}/rewritten/save_file.rewritten.c
    ${SPYRO_GENERATED}/rewritten/strings.rewritten.c
    ${SPYRO_GENERATED}/rewritten/cheats.rewritten.c
    ${SPYRO_GENERATED}/rewritten/hud.rewritten.c
    ${SPYRO_GENERATED}/rewritten/gamepad.rewritten.c
    # camera.rewritten.c — deferred (SIGSEGV during demo gameplay).
    ${SPYRO_GENERATED}/rewritten/spu.rewritten.c
    ${SPYRO_GENERATED}/rewritten/overlay_pointers.rewritten.c
    ${SPYRO_GENERATED}/rewritten/moby_helpers.rewritten.c
    # pete.rewritten.c — deferred (Pass 5c splice corruption).
    # loaders.rewritten.c — re-deferred. Pass 1/4 sentinel-cast fix
    # cleared the early PatchMobyModelPointers crash but exposed
    # non-deterministic downstream crashes (SoundsUpdate at slot
    # 11 with m_Moby=0x0e11e96d junk vaddr → psx_to_host_ptr=NULL
    # → SIGSEGV; PutDrawEnv(NULL) on later runs). State corruption
    # from a still-unidentified loaders.c write site; needs deeper
    # debugging beyond this session.
    # initialization.c / completion.c re-enable gated on Pass 4/5
    # (integer-cast pointer arithmetic + struct-by-value boundary).
)

if(OPENPETE_SKIP_DECOMP)
    # native_dispatch_stub.c provides spyro_native_dispatch returning 0,
    # so psx_override_dispatch_game falls straight through to recomp.
    set(SPYRO_DECOMP_TUS native_dispatch_stub.c)
else()
    set(SPYRO_DECOMP_TUS
        ${SPYRO_NATIVE_SOURCES}
        # Generated from manifest/native.toml by tools/gen_dispatch.py +
        # tools/gen_thunks.py. native_dispatch.c is the inbound override
        # switch (recomp → native); native_thunks.cpp is the outbound side
        # (native → recomp / native → PSYQ).
        ${SPYRO_GENERATED}/native_dispatch.c
        ${SPYRO_GENERATED}/native_thunks.cpp)
endif()

# --- ROM-derived recomp module -----------------------------------------
#
# OPENPETE_RECOMP_LINKAGE selects how the spyro1_recomp_module built by
# openpete_add_game reaches the exe:
#   static — OBJECT library linked straight in (dev default;
#            byte-identical link semantics, no archive member pulling).
#   shared — spyro1-recomp.{so,dll} + dlopen loader (the distribution
#            build; the same sources users boot-compile at first run).
set(OPENPETE_RECOMP_LINKAGE "static" CACHE STRING
    "Recomp module linkage: static (dev) or shared (distribution)")
set_property(CACHE OPENPETE_RECOMP_LINKAGE PROPERTY STRINGS static shared)

if(OPENPETE_RECOMP_LINKAGE STREQUAL "shared" AND NOT OPENPETE_SKIP_DECOMP)
    message(FATAL_ERROR
        "OPENPETE_RECOMP_LINKAGE=shared requires OPENPETE_SKIP_DECOMP=ON "
        "(hybrid decomp mode is static-only: --defsym aliases + native "
        "thunks assume one image)")
endif()

# Host-side attach TU + (shared) the generated export surface.
if(OPENPETE_RECOMP_LINKAGE STREQUAL "shared")
    set(_module_def       ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro1_module_exports.def)
    set(_module_keepalive ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro1_abi_keepalive.gen.c)
    add_custom_command(
        OUTPUT  ${_module_def} ${_module_keepalive}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_module_exports.py
                --psyq   ${CMAKE_CURRENT_SOURCE_DIR}/manifest/psyq.toml
                --extras ${CMAKE_CURRENT_SOURCE_DIR}/manifest/module_abi_exports.txt
                --image  openpete-spyro1.exe
                --def-out ${_module_def}
                --keepalive-out ${_module_keepalive}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/manifest/psyq.toml
                ${CMAKE_CURRENT_SOURCE_DIR}/manifest/module_abi_exports.txt
                ${CMAKE_SOURCE_DIR}/tools/gen_module_exports.py
        COMMENT "Generating recomp-module export surface (.def + keep-alive)")
    set(SPYRO_RECOMP_ATTACH_SOURCES
        recomp_attach_shared.c
        dist/recomp_build.cpp
        ${_module_keepalive})
else()
    set(SPYRO_RECOMP_ATTACH_SOURCES recomp_attach_static.c)
endif()

# App icon. Two sources (distribution D3):
#   OPENPETE_ROM_FREE_ICON=OFF (dev default) — decoded from the PSX
#     save-file header's CLUT+icon data (save_file.h:39): the authentic
#     memcard icon, which is GAME BYTES and must not ship.
#   OPENPETE_ROM_FREE_ICON=ON (dist presets) — original procedural icon
#     from tools/gen_dist_icon.py; also drops the build's dependency on
#     the private decomp submodule's assets.
# Either path emits a C source for SDL_SetWindowIcon (Linux/macOS +
# live-window fallback on Windows), passed to openpete_add_game as the
# ICON_SOURCE, and (below the call) a .ico for the Win32 resource.
option(OPENPETE_ROM_FREE_ICON
       "Use the original (non-ROM) distribution icon" OFF)
set(_spyro_icon_c ${CMAKE_CURRENT_BINARY_DIR}/gen/openpete_spyro1_icon.c)
if(OPENPETE_ROM_FREE_ICON)
    set(_spyro_icon_tool ${CMAKE_SOURCE_DIR}/tools/gen_dist_icon.py)
    set(_spyro_icon_c_cmd python3 ${_spyro_icon_tool} --c ${_spyro_icon_c})
    set(_spyro_icon_deps ${_spyro_icon_tool})
else()
    set(_spyro_icon_src
        ${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/assets/save_header.databin.bin)
    set(_spyro_icon_tool ${CMAKE_SOURCE_DIR}/tools/mcs_icon_to_ico.py)
    set(_spyro_icon_c_cmd python3 ${_spyro_icon_tool} ${_spyro_icon_src}
        --c ${_spyro_icon_c})
    set(_spyro_icon_deps ${_spyro_icon_src} ${_spyro_icon_tool})
endif()
add_custom_command(
    OUTPUT  ${_spyro_icon_c}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND ${_spyro_icon_c_cmd}
    DEPENDS ${_spyro_icon_deps}
    COMMENT "Generating openpete_spyro1_icon.c")

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-1
    MAIN           main.cpp
    FEATURE_LIST   feature_list.c
    ORCHESTRATOR   renderer/reextract.c
    INTERP_REGIONS renderer/interp_regions.c
    TERRAIN_GAME   renderer/terrain_game.c
    MOBY_GAME      renderer/moby_game.c
    PLAYER_GAME    renderer/player_game.c
    ICON_SOURCE    ${_spyro_icon_c}
    MODULE_LINKAGE ${OPENPETE_RECOMP_LINKAGE}
    SYMBOLS_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest/spyro1_symbols.toml
    SOURCES
        psyq_hooks.cpp

        # The native renderer's game half: the feeders and the interp region
        # table. The sub-tick re-extract orchestrator is ORCHESTRATOR above.
        renderer/render_decouple.c
        renderer/spyro.c
        renderer/flame.c
        renderer/shadow_spyro.c
        renderer/particle.c
        renderer/tracer.c
        renderer/layout.c
        renderer/portal.c
        renderer/text_moby.c
        renderer/text_moby_id.c
        renderer/transition.c

        # Non-renderer feature modules.
        features/polybuf_guard.c
        features/freecam.c
        features/audio_volume.c
        features/vibration.c
        features/cheats.c
        features/credits_skip.c
        features/crash_demo_woah.c
        features/levelwarp.c

        # Native translations of spyro-1/src/*.c compiled via the AST
        # rewriter (tools/rewrite_tu.py). Each native TU is rewritten by
        # scripts/regen-manifest.sh into
        # ${SPYRO_GENERATED}/rewritten/<stem>.rewritten.c — Pass 2 redirects
        # diverging bare globals through __psx_<name> uint32 storage that
        # gen_defsyms.py aliases into g_ram. CMake compiles the rewritten
        # source, never the original. Suppressed when OPENPETE_SKIP_DECOMP=ON.
        ${SPYRO_DECOMP_TUS}

        # The attach TU registers the module (static) or dlopens it (shared).
        ${SPYRO_RECOMP_ATTACH_SOURCES}

        sfx_census.cpp
        write_trace.cpp

        # Mods: the WAD asset overlay and the moby-type and level-slot
        # registry game ops.
        mods/mod_wad_overlay.c
        mods/mod_moby_ops.c
        mods/mod_level_ops.c

        # First-run distribution: the WAD extraction, the disc library and
        # the splash window (the module build, dist/recomp_build.cpp, is a
        # shared-linkage attach source above).
        dist/recomp_extract.cpp
        dist/disc_library.cpp
        dist/splash.c)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
    if(OPENPETE_RECOMP_LINKAGE STREQUAL "shared")
        # Distribution build: GUI subsystem so no console window pops up
        # for end users. main.cpp reattaches stdout/stderr to the parent
        # console when launched from a terminal (dev/CI keep their logs).
        target_link_options(openpete-spyro1 PRIVATE -mwindows)
    else()
        # Dev build: console subsystem so stdout/stderr land in the parent
        # terminal (incl. WSL2 interop) with no extra plumbing.
        target_link_options(openpete-spyro1 PRIVATE -mconsole)
    endif()
    if(NOT OPENPETE_SKIP_DECOMP)
        # Disable ASLR — DECOMP MODE ONLY. The native-decomp globals are
        # link-aliased into g_ram via -Wl,--defsym=name=g_ram+offset. On ELF
        # this produces a PIE-aware relocation; on PE/COFF the MinGW linker
        # can't represent the absolute symbol ("stripping non-representable
        # symbol …"), so the absolute address gets baked into call sites
        # with no entry in .reloc. ASLR then moves g_ram at load time, the
        # baked addresses rot, and the first decomp global access faults.
        # Full-recomp builds (SKIP_DECOMP=ON — every release/dist preset)
        # have no defsym aliases, so they keep ASLR (helps the AV/SmartScreen
        # posture).
        target_link_options(openpete-spyro1 PRIVATE
            -Wl,--disable-dynamicbase
            -Wl,--disable-high-entropy-va)
    endif()
endif()

# The Win32 icon resource and VERSIONINFO (openpete-spyro1.rc.in). Version
# is the dist versioning scheme's placeholder until real release tags exist.
if(WIN32)
    set(_spyro_icon_ico ${CMAKE_CURRENT_BINARY_DIR}/openpete-spyro1.ico)
    set(_spyro_icon_rc  ${CMAKE_CURRENT_BINARY_DIR}/openpete-spyro1.rc)
    if(OPENPETE_ROM_FREE_ICON)
        set(_spyro_icon_ico_cmd python3 ${_spyro_icon_tool} --ico ${_spyro_icon_ico})
    else()
        set(_spyro_icon_ico_cmd python3 ${_spyro_icon_tool} ${_spyro_icon_src}
            --ico ${_spyro_icon_ico})
    endif()
    add_custom_command(
        OUTPUT  ${_spyro_icon_ico}
        COMMAND ${_spyro_icon_ico_cmd}
        DEPENDS ${_spyro_icon_deps}
        COMMENT "Generating openpete-spyro1.ico")
    set(OPENPETE_VERSION_NUM "0,1,0,0")
    set(OPENPETE_VERSION_STR "0.1.0")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/openpete-spyro1.rc.in
                   ${_spyro_icon_rc} @ONLY)
    target_sources(openpete-spyro1 PRIVATE ${_spyro_icon_ico} ${_spyro_icon_rc})
endif()

# SoundTable slot names (tools/gen_sfx_names.py over the decomp's
# sound_table.h): one table shared by the sfx resolver, the sfx pack's
# symbolic form and the census dump's file labels.
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro1_sfx_names.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_sfx_names.py
            --header ${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/include/sound_table.h
            --out ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro1_sfx_names.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/include/sound_table.h
            ${CMAKE_SOURCE_DIR}/tools/gen_sfx_names.py
    COMMENT "Generating SoundTable slot names (gen_sfx_names.py)")
add_custom_target(spyro1_sfx_names DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro1_sfx_names.h)
add_dependencies(openpete-spyro1 spyro1_sfx_names)


# --- Mod SDK v0 (notes/plans/openpete-modding-framework.md P1) ---
# Generate the name→vaddr symbol surface from manifest/symbols.toml:
# a #define header for mods + a sorted resolve table compiled into the
# exe. Staged with the API headers into <exe>/sdk/ so the mod loader can
# compile mods/<id>/src/*.c against it at load time.
set(_mod_sdk_gen ${CMAKE_CURRENT_BINARY_DIR}/mod_sdk_gen)
add_custom_command(
    OUTPUT  ${_mod_sdk_gen}/openpete_sdk_symbols.h
            ${_mod_sdk_gen}/sdk_symbols_table.c
            ${_mod_sdk_gen}/openpete_sdk_wrappers.h
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_mod_sdk.py
            --symbols ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
            --globals ${CMAKE_CURRENT_SOURCE_DIR}/manifest/globals.toml
            --protos ${CMAKE_CURRENT_SOURCE_DIR}/manifest/prototypes.toml
            --out-dir ${_mod_sdk_gen}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/globals.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/prototypes.toml
            ${CMAKE_SOURCE_DIR}/tools/gen_mod_sdk.py
    COMMENT "Generating mod SDK symbol table + natural wrappers (gen_mod_sdk.py)")
target_sources(openpete-spyro1 PRIVATE ${_mod_sdk_gen}/sdk_symbols_table.c)
# renderer/layout.c takes its member offsets from the typed structs header.
target_include_directories(openpete-spyro1 PRIVATE ${_mod_sdk_gen})

# Typed game structs for mods: every decomp typedef struct plus the PSYQ
# types they embed, guest-pointer fields rewritten to uint32_t, pinned by
# _Static_asserts read from manifest/struct_layout.toml (the MIPS layout
# probed by scripts/regen-struct-layout.sh; committed, so no build needs
# the MIPS compiler). The generator refuses a toml probed from other
# headers than the checkout's.
file(GLOB _spyro1_decomp_headers CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/include/*.h)
add_custom_command(
    OUTPUT  ${_mod_sdk_gen}/openpete_game_structs.h
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_game_structs.py
            --include ${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/include
            --layout ${CMAKE_CURRENT_SOURCE_DIR}/manifest/struct_layout.toml
            --out ${_mod_sdk_gen}/openpete_game_structs.h
    DEPENDS ${_spyro1_decomp_headers}
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/struct_layout.toml
            ${CMAKE_SOURCE_DIR}/tools/gen_game_structs.py
            ${CMAKE_SOURCE_DIR}/tools/psyq_types.py
            ${CMAKE_SOURCE_DIR}/tools/probe_struct_layout.py
    COMMENT "Generating typed game structs for the mod SDK (gen_game_structs.py)")
add_custom_target(spyro1_game_structs ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:openpete-spyro1>/sdk
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_mod_sdk_gen}/openpete_game_structs.h
            $<TARGET_FILE_DIR:openpete-spyro1>/sdk
    DEPENDS ${_mod_sdk_gen}/openpete_game_structs.h
    COMMENT "Staging typed game structs into <exe>/sdk/")
add_dependencies(openpete-spyro1 spyro1_game_structs)

# --- Mod-author docs (notes/plans/openpete-mod-docs.md) ---
# tools/gen_mod_docs.py extracts the SDK surface (api header, symbol/
# global/prototype manifests, typed structs, mod examples) into markdown;
# scripts/build-mod-docs.sh then builds the mkdocs-material site when
# mkdocs is installed (notice + green build when it isn't). Stamp-
# guarded: the docs rebuild exactly when an input changed — "the docs
# never lie" is a build property, not a policy.
set(_mod_docs_stamp ${CMAKE_CURRENT_BINARY_DIR}/moddocs.stamp)
# The reference pages cover every game's manifests and decomp headers and
# link each symbol to the decomp checkout at its commit; the reflog is
# appended on every HEAD move, so it stands in for "the submodule commit
# changed".
set(_mod_docs_game_inputs "")
foreach(_g spyro-1 spyro-2 spyro-3)
    execute_process(
        COMMAND git -C ${CMAKE_SOURCE_DIR}/games/${_g}/${_g} rev-parse --git-path logs/HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND EXISTS ${_head})
        list(APPEND _mod_docs_game_inputs ${_head})
    endif()
    file(GLOB _hdrs CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/games/${_g}/${_g}/include/*.h)
    list(APPEND _mod_docs_game_inputs ${_hdrs}
         ${CMAKE_SOURCE_DIR}/games/${_g}/manifest/symbols.toml
         ${CMAKE_SOURCE_DIR}/games/${_g}/manifest/globals.toml)
endforeach()
file(GLOB _mod_docs_manifests CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/mods/*/mod.toml)
file(GLOB _mod_docs_pages CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/docs/modding/pages/*.md)
# pages/imgui.md is written by the command below, so listing it as an input
# would make the stamp depend on its own output.
list(REMOVE_ITEM _mod_docs_pages ${CMAKE_SOURCE_DIR}/docs/modding/pages/imgui.md)
file(GLOB _mod_docs_assets CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/docs/modding/pages/assets/javascripts/*)
add_custom_command(
    OUTPUT  ${_mod_docs_stamp}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build-mod-docs.sh
            --structs ${_mod_sdk_gen}/openpete_game_structs.h
            --site-dir ${CMAKE_CURRENT_BINARY_DIR}/moddocs/site
    COMMAND ${CMAKE_COMMAND} -E touch ${_mod_docs_stamp}
    DEPENDS ${CMAKE_SOURCE_DIR}/core/include/openpete_mod_api.h
            ${CMAKE_SOURCE_DIR}/core/include/openpete_mod_ui.h
            ${CMAKE_SOURCE_DIR}/core/include/openpete_imgui.h
            ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.h
            ${CMAKE_SOURCE_DIR}/tools/gen_imgui_table.py
            ${_mod_docs_game_inputs}
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/prototypes.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/struct_layout.toml
            ${_mod_sdk_gen}/openpete_game_structs.h
            ${CMAKE_SOURCE_DIR}/tools/gen_mod_docs.py
            ${CMAKE_SOURCE_DIR}/scripts/build-mod-docs.sh
            ${CMAKE_SOURCE_DIR}/docs/modding/mkdocs.yml
            ${_mod_docs_manifests}
            ${_mod_docs_pages}
            ${_mod_docs_assets}
            ${CMAKE_SOURCE_DIR}/mods/examples.txt
    COMMENT "Building mod-author docs (gen_mod_docs.py + mkdocs)")
add_custom_target(mod_docs ALL DEPENDS ${_mod_docs_stamp})

# The staged <exe>/sdk/ tree IS the contract mods compile and #include
# against, so it must never lag the repo. An ALL target rather than a
# POST_BUILD command: POST_BUILD only fires when the exe relinks, and
# editing a header or a .glsl does not relink anything — a contract edit
# would sit unstaged until some unrelated C change happened to force a
# link, and every mod would keep building against the old contract with
# no diagnostic. copy_if_different over a dozen small files is cheap
# enough to run unconditionally.
# The directory is removed first: a file dropped from this list would
# otherwise survive in the build tree and ship with the archive, since
# package-dist copies the staged directory whole. Mod build keys hash the
# SDK sources, so the fresh copies' timestamps do not force recompiles.
add_custom_target(spyro1_sdk_stage ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            $<TARGET_FILE_DIR:openpete-spyro1>/sdk
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:openpete-spyro1>/sdk
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/core/include/openpete_mod_api.h
            ${CMAKE_SOURCE_DIR}/core/include/openpete_mod_ui.h
            ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/runner/include/psx_runtime.h
            ${_mod_sdk_gen}/openpete_sdk_symbols.h
            ${_mod_sdk_gen}/openpete_sdk_wrappers.h
            ${_mod_sdk_gen}/openpete_game_structs.h
            ${CMAKE_SOURCE_DIR}/core/mods/sdk/openpete_mod_abi.c
            ${CMAKE_SOURCE_DIR}/core/include/openpete_imgui.h
            ${CMAKE_SOURCE_DIR}/third_party/dear_bindings/dcimgui.h
            ${CMAKE_SOURCE_DIR}/third_party/imgui/imconfig.h
            ${OPENPETE_IMGUI_TABLE_DIR}/openpete_imgui_table.h
            ${OPENPETE_IMGUI_TABLE_DIR}/openpete_imgui_macros.h
            $<TARGET_FILE_DIR:openpete-spyro1>/sdk
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:openpete-spyro1>/sdk/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/core/include/openpete_shader_api.glsl
            ${CMAKE_SOURCE_DIR}/core/include/openpete_psx_material.glsl
            ${CMAKE_SOURCE_DIR}/core/include/openpete_psx_decal.glsl
            ${CMAKE_SOURCE_DIR}/core/rhi/shaders/psx_key_contest.glsl
            ${CMAKE_SOURCE_DIR}/core/rhi/shaders/terrain_shade.glsl
            $<TARGET_FILE_DIR:openpete-spyro1>/sdk/shaders
    COMMENT "Staging mod SDK headers into <exe>/sdk/"
    VERBATIM)
add_dependencies(spyro1_sdk_stage openpete-spyro1)

# --- Build-time mod compile (notes/plans/openpete-mod-compile-on-boot.md)
# `cmake --build` leaves every mod compiled, not just the engine:
#  - Native presets: run the freshly built exe in --compile-mods mode over
#    OPENPETE_COMPILE_MODS_DIR, so keys/stamps/flags are byte-identical to
#    compile-on-boot (a boot after a clean build recompiles nothing). A
#    failed mod compile fails the build (escape: -DOPENPETE_COMPILE_MODS=OFF
#    or build --target openpete-spyro1).
#  - Cross presets (windows-mingw*): the target exe can't run at build
#    time; stage every repo mod next to the exe (cmake/stage_mods.cmake:
#    exact source mirror; user-owned config.toml/enabled.txt and staged-
#    only mod dirs untouched) and cross-compile each one with the
#    preset's own toolchain into <build>/games/spyro-1/mods/<id>/.build/
#    <id>.dll, DROPPING the stamp — an unstamped prebuilt is the
#    loader's designed cross-handoff artifact (build_mod: loads silently
#    when the host has no compiler, recompiles+stamps once when it
#    does). DLLs compile from the REPO src/*.c (identical to the staged
#    mirror), so neither staged sources nor staged DLLs can rot.
#    OPENPETE_STAGE_MOD_DLLS=OFF (windows-mingw-shared, the distribution
#    preset) stages sources only, no cmake-side DLLs: that exe's mods
#    dir is the first-boot-compile validation surface — the bundled
#    llvm-mingw must build every mod at boot exactly as it would on an
#    end user's box, so cmake pre-seeding .build/ would mask it.
option(OPENPETE_COMPILE_MODS "Compile all mods as part of the build" ON)
option(OPENPETE_STAGE_MOD_DLLS
    "Cross presets: also cross-compile each staged mod's DLL (OFF = stage sources only; the exe compiles them on boot)" ON)
set(OPENPETE_COMPILE_MODS_DIR ${CMAKE_SOURCE_DIR}/mods CACHE PATH
    "Mods root the native-preset build-time compile runs over")
if(OPENPETE_COMPILE_MODS)
    if(CMAKE_CROSSCOMPILING)
        # Staged-SDK *sources* — mirrors mod_compute_key()'s sdk_inputs
        # list, so an SDK change re-cross-compiles every staged mod.
        set(_mod_sdk_deps
            ${CMAKE_SOURCE_DIR}/core/include/openpete_mod_api.h
            ${CMAKE_SOURCE_DIR}/core/include/openpete_mod_ui.h
            ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/runner/include/psx_runtime.h
            ${_mod_sdk_gen}/openpete_sdk_symbols.h
            ${_mod_sdk_gen}/openpete_sdk_wrappers.h
            ${_mod_sdk_gen}/openpete_game_structs.h
            ${CMAKE_SOURCE_DIR}/core/mods/sdk/openpete_mod_abi.c)
        # Always runs (cheap: copy-if-different over a <5MB tree); keeps
        # staged mod sources/assets current with the repo every build.
        add_custom_target(spyro1_mods_stage ALL
            COMMAND ${CMAKE_COMMAND}
                    -DSRC=${OPENPETE_COMPILE_MODS_DIR}
                    -DDST=${CMAKE_CURRENT_BINARY_DIR}/mods
                    -P ${CMAKE_SOURCE_DIR}/cmake/stage_mods.cmake
            COMMENT "Staging mods into <exe>/mods/ (source mirror)"
            VERBATIM)
        set(_staged_mod_dlls "")
        if(OPENPETE_STAGE_MOD_DLLS)
        file(GLOB _mod_manifests CONFIGURE_DEPENDS
             ${OPENPETE_COMPILE_MODS_DIR}/*/mod.toml)
        foreach(_manifest IN LISTS _mod_manifests)
            get_filename_component(_src_mod_dir ${_manifest} DIRECTORY)
            get_filename_component(_mod_id ${_src_mod_dir} NAME)
            file(GLOB _mod_srcs CONFIGURE_DEPENDS ${_src_mod_dir}/src/*.c)
            if(NOT _mod_srcs)
                continue() # assets-only/binary-only mod: nothing to build
            endif()
            file(GLOB _mod_hdrs CONFIGURE_DEPENDS ${_src_mod_dir}/src/*.h)
            set(_mod_build_dir
                ${CMAKE_CURRENT_BINARY_DIR}/mods/${_mod_id}/.build)
            set(_dll ${_mod_build_dir}/${_mod_id}.dll)
            add_custom_command(OUTPUT ${_dll}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${_mod_build_dir}
                # Same compile line mod_compile() (core/mods/mod_loader.c)
                # uses on Windows — keep in sync — plus -static-libgcc:
                # the exe ships no mingw runtime DLLs (mingw-w64.cmake
                # links -static), so the mod DLL must be self-contained.
                COMMAND ${CMAKE_C_COMPILER} -shared -O2 -g
                        -fno-strict-aliasing -Wall -static-libgcc
                        -I$<TARGET_FILE_DIR:openpete-spyro1>/sdk
                        -o ${_dll} ${_mod_srcs}
                        $<TARGET_FILE_DIR:openpete-spyro1>/sdk/openpete_mod_abi.c
                COMMAND ${CMAKE_COMMAND} -E rm -f
                        ${_mod_build_dir}/${_mod_id}.hash
                DEPENDS ${_mod_srcs} ${_mod_hdrs} ${_mod_sdk_deps}
                        openpete-spyro1
                COMMENT "Cross-compiling mod ${_mod_id}.dll"
                VERBATIM)
            list(APPEND _staged_mod_dlls ${_dll})
        endforeach()
        endif() # OPENPETE_STAGE_MOD_DLLS
        add_custom_target(spyro1_mods ALL DEPENDS ${_staged_mod_dlls})
        add_dependencies(spyro1_mods spyro1_mods_stage spyro1_sdk_stage)
    else()
        # Always runs (cheap: the exe's pass-1 content-hash check skips
        # up-to-date mods in milliseconds); nonzero exit fails the build.
        add_custom_target(spyro1_mods ALL
            COMMAND $<TARGET_FILE:openpete-spyro1> --compile-mods
                    --mods-dir ${OPENPETE_COMPILE_MODS_DIR}
            COMMENT "Compiling mods (openpete-spyro1 --compile-mods)"
            VERBATIM)
        add_dependencies(spyro1_mods openpete-spyro1 spyro1_sdk_stage)
    endif()
endif()
if(OPENPETE_RECOMP_LINKAGE STREQUAL "shared" AND WIN32)
    # The exe exports exactly the curated surface (the .def rides the
    # link line); the module DLL links an implib generated from the
    # same .def so its imports name openpete-spyro1.exe.
    target_link_options(openpete-spyro1 PRIVATE ${_module_def})
    set_property(TARGET openpete-spyro1 APPEND PROPERTY
        LINK_DEPENDS ${_module_def})
    # GNU LTO does not treat .def exports as liveness roots: with IPO
    # on, the otherwise-unreferenced ABI surface (psyq_* thunks,
    # dispatch, GTE entry points) is dead-code-eliminated and ld
    # silently emits the exports as absolute-0 — the module's first
    # PSYQ import then jumps to image_base+0xC0000000 (DEP crash).
    # Force IPO off for the exe regardless of cache/preset state.
    set_target_properties(openpete-spyro1 PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION OFF)
    if(NOT CMAKE_DLLTOOL)
        set(CMAKE_DLLTOOL x86_64-w64-mingw32-dlltool)
    endif()
    set(_module_implib
        ${CMAKE_CURRENT_BINARY_DIR}/gen/libopenpete-exe-exports.a)
    add_custom_command(
        OUTPUT  ${_module_implib}
        COMMAND ${CMAKE_DLLTOOL} -d ${_module_def} -l ${_module_implib}
                -D openpete-spyro1.exe
        DEPENDS ${_module_def}
        COMMENT "dlltool: exe-export implib for the recomp module")
    add_custom_target(spyro1_module_implib DEPENDS ${_module_implib})
    add_dependencies(spyro1_recomp_module spyro1_module_implib)
    target_link_libraries(spyro1_recomp_module PRIVATE ${_module_implib})
endif()

# Shared-linkage builds stage the first-boot inputs next to the exe:
# the converged manifests, the 5-file module source kit, and (when
# built) PSXRecomp. See dist/recomp_build.cpp.
if(OPENPETE_RECOMP_LINKAGE STREQUAL "shared")
    set(_rdir $<TARGET_FILE_DIR:openpete-spyro1>/recomp)
    add_custom_command(TARGET openpete-spyro1 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/recomp/SCUS-94228
            ${_rdir}/manifest
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/psyq.toml
            ${SPYRO_GENERATED}/channel_sites.txt
            ${_rdir}/manifest
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_rdir}/src
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/../common/overlay_dispatch.c
            ${CMAKE_SOURCE_DIR}/games/common/module_main.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../common/spyro_overlay_table.h
            ${CMAKE_SOURCE_DIR}/core/include/openpete_recomp_module.h
            ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/runner/include/psx_runtime.h
            ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/runner/include/psx_runtime_internal.h
            ${_rdir}/src
        VERBATIM)
    if(WIN32)
        set(_psxrecomp_bin ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/build-mingw/PSXRecomp.exe)
    else()
        set(_psxrecomp_bin ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/build/PSXRecomp)
    endif()
    if(EXISTS ${_psxrecomp_bin})
        add_custom_command(TARGET openpete-spyro1 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_rdir}/tools
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_psxrecomp_bin}
                ${_rdir}/tools
            VERBATIM)
    endif()
    if(WIN32)
        # The module DLL links against the exe's export implib at first
        # boot — stage it with the tools.
        add_custom_command(TARGET openpete-spyro1 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_rdir}/tools
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_module_implib}
                ${_rdir}/tools
            VERBATIM)
    endif()
endif()
# Emscripten / WebAssembly browser link.
#
#  ASYNCIFY        — required twice over: (1) libco's emscripten backend
#                    maps co_switch onto emscripten_fiber_swap, and (2) the
#                    host loop yields to the browser via emscripten_sleep
#                    inside op_sleep_until. Both are Asyncify operations.
#  STACK_SIZE      — the primary (host) fiber runs on the main C stack; the
#                    PsyCross render path is deep, so bump well past the
#                    64 KiB default. The game fiber has its own 8 MiB stack
#                    (see run.c co_create).
#  MEMORY GROWTH   — the player's disc (~630 MiB) is streamed into MEMFS, so
#                    we start modest and grow toward the wasm32 ceiling.
#  INVOKE_RUN=0    — main() is launched from the shell's callMain() only
#                    after the disc has been written into the FS.
#  FULL_ES2        — PsyCross selects RENDERER_OGLES / OGLES_VERSION 2 under
#                    __EMSCRIPTEN__ (WebGL via GLES2 emulation).
if(EMSCRIPTEN)
    target_link_options(openpete-spyro1 PRIVATE
        "-sASYNCIFY=1"
        "-sASYNCIFY_STACK_SIZE=1048576"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=268435456"      # 256 MiB
        "-sMAXIMUM_MEMORY=2147483648"     # 2 GiB
        "-sSTACK_SIZE=4194304"            # 4 MiB host/primary-fiber C stack
        "-sFORCE_FILESYSTEM=1"
        "-sINVOKE_RUN=0"
        "-sEXIT_RUNTIME=0"
        "-sEXPORTED_RUNTIME_METHODS=callMain,FS"
        "-sFULL_ES3=1"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sGL_ENABLE_GET_PROC_ADDRESS=1"
        "-sALLOW_BLOCKING_ON_MAIN_THREAD=1"
        "-lopenal"
    )
    # Optional: keep WASM function names in the binary so browser stack
    # traces are legible (negligible runtime cost, larger .wasm). Useful
    # for diagnosing Asyncify / call_indirect signature issues.
    option(OPENPETE_WASM_PROFILING "Emit named WASM functions for debugging" OFF)
    if(OPENPETE_WASM_PROFILING)
        target_link_options(openpete-spyro1 PRIVATE "--profiling-funcs")
    endif()
    # emcc emits openpete-spyro1.js + openpete-spyro1.wasm. Ship the
    # hand-authored drag-and-drop shell alongside them — NO copyrighted
    # disc data is bundled; the player supplies their own dump at runtime.
    add_custom_command(TARGET openpete-spyro1 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            "$<TARGET_FILE_DIR:openpete-spyro1>/index.html"
        VERBATIM)
endif()

# The native renderer's SPIR-V shaders are embedded in the binary (baked at
# build time by core/rhi/embed_shaders.cmake), so nothing is staged beside the
# exe — the executable is self-contained. For shader dev/modding without a
# relink, point OPENPETE_SHADER_DIR at a dir of loose .spv (e.g. the build's
# core/rhi/shaders). See Phase 8 of openpete-native-renderer.md.

# Stage the music WAV override directory next to the executable so users
# have a ready-made drop point, and seed it with the template manifest
# (full track table + instructions). copy_if_missing never clobbers a
# user-edited manifest on rebuild. See
# notes/plans/openpete-music-wav-override.md.
add_custom_command(TARGET openpete-spyro1 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:openpete-spyro1>/assets/music"
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/assets/music/manifest.txt
        -DDST=$<TARGET_FILE_DIR:openpete-spyro1>/assets/music/manifest.txt
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake
    VERBATIM)

# Under OPENPETE_FUNC_TRACE the main EXE TU carries -finstrument-functions
# on top of its -w (per-function CPU tracing). The exclude-list keeps the hot
# unaligned-access helpers and the dispatch switch uninstrumented. When the
# option is OFF nothing is added: zero instrumentation, byte-identical code.
if(OPENPETE_FUNC_TRACE)
    set(_functrace_full_flags
        "-w -finstrument-functions -finstrument-functions-exclude-function-list=psx_lwl,psx_lwr,psx_swl,psx_swr,psx_dispatch")

    # Generate the pointer→name symtab func_trace.c resolves the hooks against.
    set(_functrace_symtab ${CMAKE_CURRENT_BINARY_DIR}/gen/func_trace_symtab.c)
    add_custom_command(
        OUTPUT  ${_functrace_symtab}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_func_trace_symtab.py
                --dispatch ${SPYRO_GENERATED}/PSX.EXE_dispatch.c
                --symbols  ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
                --out      ${_functrace_symtab}
        DEPENDS ${SPYRO_GENERATED}/PSX.EXE_dispatch.c
                ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
                ${CMAKE_SOURCE_DIR}/tools/gen_func_trace_symtab.py
        COMMENT "Generating func-trace symtab")
    target_sources(openpete-spyro1 PRIVATE ${_functrace_symtab})
    set_source_files_properties(${_functrace_symtab} PROPERTIES
        COMPILE_FLAGS "-w"
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/core/include")
    set_source_files_properties(
        ${SPYRO_GENERATED}/PSX.EXE_full.c
        PROPERTIES
            COMPILE_FLAGS "${_functrace_full_flags}"
    )
endif()

# Common compile flags for any spyro-1/src/*.c TU compiled natively.
#
#   -std=gnu11 + -include assert.h: PsyCross's libgpu.h uses C11 static_assert
#   -fcommon: tentative file-scope defs become COMMON so --defsym aliases win
#   -include psyq_compat.h: bridges PSYQ-renamed types (RECT vs RECT16)
#   -include native_renames.h: auto-generated symbol redirects (manifest →
#     openpete_/spyro_native_ prefixes) — see tools/gen_renames.py
#   -w: mutes matching-decomp warnings (unused decls from gameplay headers)
set(SPYRO_DECOMP_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/include;${CMAKE_SOURCE_DIR}/third_party/PsyCross/include;${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx")
set(SPYRO_DECOMP_BASE_FLAGS
    "-std=gnu11 -fcommon -include assert.h \
-include ${CMAKE_CURRENT_SOURCE_DIR}/decomp/psyq_compat.h \
-include ${SPYRO_GENERATED}/native_renames.h -w")

# Per-TU compile flags / shadow include trees (Pass 1 struct relayout).
# Each native TU gets ${SPYRO_GENERATED}/rewritten/<stem>/include/
# prepended to its -I list so PSX-laid struct definitions win over the
# originals in spyro-1/include. The shadow trees are populated by
# tools/rewrite_tu.py during scripts/regen-manifest.sh.
#
# Skipped under OPENPETE_SKIP_DECOMP — none of the rewritten TUs are
# in the build, native_thunks.cpp is gone, and the --defsym aliases
# would dangle (the rewritten symbols they alias don't exist).
if(NOT OPENPETE_SKIP_DECOMP)
    foreach(SRC ${SPYRO_NATIVE_SOURCES})
        get_filename_component(_stem "${SRC}" NAME_WE)  # main, cd, ...
        set(_shadow "${SPYRO_GENERATED}/rewritten/${_stem}/include")
        set_source_files_properties(
            ${SRC}
            PROPERTIES
                COMPILE_FLAGS "${SPYRO_DECOMP_BASE_FLAGS}"
                INCLUDE_DIRECTORIES "${_shadow};${SPYRO_DECOMP_INCLUDES}"
        )
    endforeach()
    set_source_files_properties(
        ${SPYRO_GENERATED}/native_thunks.cpp
        PROPERTIES
            INCLUDE_DIRECTORIES "${SPYRO_DECOMP_INCLUDES}"
    )

    # Link-time aliasing of native-decomp globals into PSX RAM (g_ram + offset).
    # The defsym list is generated from manifest/native.toml + the parsed
    # psx.map (manifest/globals.toml) by tools/gen_defsyms.py — see
    # scripts/regen-manifest.sh. g_ram is the public symbol exposed by
    # core/psx/cpu.c.
    include(${SPYRO_GENERATED}/native_aliases.cmake)
    target_link_options(openpete-spyro1 PRIVATE ${SPYRO_NATIVE_DEFSYMS})
endif()
