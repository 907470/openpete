# games/spyro-1/CMakeLists.txt — Spyro 1 binary.
#
# Sources include the recompiled main EXE plus every overlay translated
# by scripts/regen-recomp.sh. Each overlay is recompiled with a unique
# --func-prefix and its `psx_dispatch_compiled` is sed-renamed to
# `psx_dispatch_<name>` so they all coexist at link. dispatch_chain.c
# fans calls out to the active overlay via the auto-generated table in
# generated/spyro_overlay_table.c.

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

# Pick up every recompiled overlay automatically. CONFIGURE_DEPENDS
# re-runs the GLOB on subsequent cmake invocations so newly-regen'd
# overlays get picked up without a manual reconfigure.
file(GLOB SPYRO_OVERLAY_FULL_C
    CONFIGURE_DEPENDS
    "${SPYRO_GENERATED}/*/*.bin_full.c")
file(GLOB SPYRO_OVERLAY_DISPATCH_C
    CONFIGURE_DEPENDS
    "${SPYRO_GENERATED}/*/*.bin_dispatch.c")

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

# --- ROM-derived recomp module (distribution seam, plan D0/D1) ----------
#
# Everything ROM-derived lives in this library behind the
# openpete_recomp_module descriptor (module_main.c): the recompiled main
# EXE + every overlay, the overlay registry, and dispatch_chain.c.
# psyq_stubs.c intentionally stays EXE-side next to psyq_thunks.cpp —
# PE has no dynamic symbol preemption, so module-side weak stubs would
# shadow the strong thunks on Windows (D0-spike finding).
#
# OPENPETE_RECOMP_LINKAGE selects how it reaches the exe:
#   static — OBJECT library linked straight in (dev default;
#            byte-identical link semantics, no archive member pulling).
#   shared — spyro1-recomp.{so,dll} + dlopen loader (distribution D1;
#            the same sources users boot-compile in D2).
set(OPENPETE_RECOMP_LINKAGE "static" CACHE STRING
    "Recomp module linkage: static (dev) or shared (distribution)")
set_property(CACHE OPENPETE_RECOMP_LINKAGE PROPERTY STRINGS static shared)

if(OPENPETE_RECOMP_LINKAGE STREQUAL "shared" AND NOT OPENPETE_SKIP_DECOMP)
    message(FATAL_ERROR
        "OPENPETE_RECOMP_LINKAGE=shared requires OPENPETE_SKIP_DECOMP=ON "
        "(hybrid decomp mode is static-only: --defsym aliases + native "
        "thunks assume one image)")
endif()

set(SPYRO_MODULE_SOURCES
    # Recompiled main EXE. Its `psx_dispatch_compiled` is sed-renamed to
    # `psx_dispatch_main_exe` by regen-recomp.sh.
    ${SPYRO_GENERATED}/PSX.EXE_full.c
    ${SPYRO_GENERATED}/PSX.EXE_dispatch.c
    # Every translated overlay (titlescreen, credits, level_10..level_64).
    ${SPYRO_OVERLAY_FULL_C}
    ${SPYRO_OVERLAY_DISPATCH_C}
    # Auto-generated overlay registry — id-magic -> dispatcher.
    ${SPYRO_GENERATED}/spyro_overlay_table.c
    # Fans psx_dispatch_compiled out to main EXE + active overlay.
    dispatch_chain.c
    # The descriptor the host resolves (openpete_recomp_module.h).
    module_main.c)

if(OPENPETE_RECOMP_LINKAGE STREQUAL "shared")
    add_library(spyro1_recomp_module SHARED ${SPYRO_MODULE_SOURCES})
    set_target_properties(spyro1_recomp_module PROPERTIES
        OUTPUT_NAME "spyro1-recomp"
        PREFIX ""                       # spyro1-recomp.so, not lib*.so
        POSITION_INDEPENDENT_CODE ON)
    if(NOT WIN32)
        # Bind module-internal references internally — matches PE
        # semantics (no cross-image preemption) so both OSes behave
        # identically, and skips PLT interposition on internal calls.
        target_link_options(spyro1_recomp_module PRIVATE
            -Wl,-Bsymbolic-functions)
        # Module->host imports resolve at dlopen against the exe's
        # -rdynamic exports; leave them undefined at link.
        target_link_options(spyro1_recomp_module PRIVATE
            -Wl,--unresolved-symbols=ignore-all)
    endif()
else()
    add_library(spyro1_recomp_module OBJECT ${SPYRO_MODULE_SOURCES})
    # Usage requirements only (psx_runtime.h et al.); OBJECT libraries
    # don't embed their dependencies.
    target_link_libraries(spyro1_recomp_module PRIVATE openpete_core)
endif()
target_include_directories(spyro1_recomp_module PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SPYRO_GENERATED}
    # Include dirs only — the shared module must NOT link openpete_core
    # (a second copy of core state inside the module would be wrong).
    $<TARGET_PROPERTY:openpete_core,INTERFACE_INCLUDE_DIRECTORIES>)
set_target_properties(spyro1_recomp_module PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

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
        recomp_build.cpp
        ${_module_keepalive})
else()
    set(SPYRO_RECOMP_ATTACH_SOURCES recomp_attach_static.c)
endif()

add_executable(openpete-spyro1
    main.cpp
    game_extras_impl.c
    feature_list.c
    features/render_paths.c
    features/render_decouple.c
    features/native_gte.c
    features/cyclorama_native.c
    features/moby_native.c
    features/spyro_native.c
    features/flame_native.c
    features/shadow_native.c
    features/particle_native.c
    features/tracer_native.c
    features/terrain_native.c
    features/ui_sprite_native.c
    features/portal_native.c
    features/text_moby_native.c
    features/transition_native.c

    # Phase 3 / Phase 4 — native translations of spyro-1/src/*.c compiled
    # via the AST rewriter (tools/rewrite_tu.py). Each native TU is
    # rewritten by scripts/regen-manifest.sh into
    # ${SPYRO_GENERATED}/rewritten/<stem>.rewritten.c — Pass 2 redirects
    # diverging bare globals through __psx_<name> uint32 storage that
    # gen_defsyms.py aliases into g_ram. CMake compiles the rewritten
    # source, never the original. Suppressed when OPENPETE_SKIP_DECOMP=ON.
    ${SPYRO_DECOMP_TUS}

    # ROM-derived recomp code lives in spyro1_recomp_module (above);
    # the attach TU registers it (static) or dlopens it (shared).
    ${SPYRO_RECOMP_ATTACH_SOURCES}

    # Hand-written PSYQ → PsyCross ABI thunks. Listed BEFORE the
    # generated stubs so under MinGW link order lets the thunk's strong
    # def win against the stub's strong def (paired with
    # --allow-multiple-definition).
    psyq_thunks.cpp
    recomp_extract.cpp
    disc_library.cpp
    splash.c
    write_trace.cpp

    # PSYQ symbol stubs — abort-on-call. On ELF these are weak so any
    # thunk strongly defines them; on MinGW the macro becomes strong
    # and the link order + --allow-multiple-definition resolves the
    # collision in the thunk's favor.
    ${SPYRO_GENERATED}/psyq_stubs.c
    psyq_stub_runtime.c
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
    target_link_options(openpete-spyro1 PRIVATE
        -Wl,--allow-multiple-definition
        # Override SDL3main's -mwindows so the .exe is a console-subsystem
        # binary; otherwise stdout/stderr aren't attached to the parent
        # terminal when launched via WSL2 interop.
        -mconsole
        )
    if(NOT OPENPETE_SKIP_DECOMP)
        # Disable ASLR — DECOMP MODE ONLY. The Phase 3/4 native-decomp
        # globals are link-aliased into g_ram via
        # -Wl,--defsym=name=g_ram+offset. On ELF this produces a PIE-aware
        # relocation; on PE/COFF the MinGW linker can't represent the
        # absolute symbol ("stripping non-representable symbol …"), so the
        # absolute address gets baked into call sites with no entry in
        # .reloc. ASLR then moves g_ram at load time, the baked addresses
        # rot, and the first decomp global access faults. Full-recomp
        # builds (SKIP_DECOMP=ON — every release/dist preset) have no
        # defsym aliases, so they keep ASLR (distribution D3: helps the
        # AV/SmartScreen posture).
        target_link_options(openpete-spyro1 PRIVATE
            -Wl,--disable-dynamicbase
            -Wl,--disable-high-entropy-va)
    endif()
endif()

# Generated C addresses psx_runtime.h via the include path provided by
# openpete_core (which adds psxrecomp/runner/include).
target_include_directories(openpete-spyro1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SPYRO_GENERATED}
    ${CMAKE_CURRENT_BINARY_DIR}/gen)

# Gameplay-state-regions table: generate from manifest, wrap in a TU
# that registers the table with openpete_core's extern hooks. See
# manifest/gameplay_state_regions.toml +
# notes/plans/openpete-determinism-enforcement.md Phase 1.
set(_spyro_state_regions_manifest
    ${CMAKE_SOURCE_DIR}/manifest/gameplay_state_regions.toml)
set(_spyro_state_regions_header
    ${CMAKE_CURRENT_BINARY_DIR}/gen/gameplay_state_regions.gen.h)
set(_spyro_state_regions_lua
    ${CMAKE_CURRENT_BINARY_DIR}/gen/openpete_state_regions.gen.lua)
add_custom_command(
    OUTPUT  ${_spyro_state_regions_header}
            ${_spyro_state_regions_lua}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_state_regions.py
            --manifest ${_spyro_state_regions_manifest}
            --out      ${_spyro_state_regions_header}
            --out-lua  ${_spyro_state_regions_lua}
    DEPENDS ${_spyro_state_regions_manifest}
            ${CMAKE_SOURCE_DIR}/tools/gen_state_regions.py
    COMMENT "Generating Spyro gameplay_state_regions.gen.h + lua mirror")

# Curated PSX symbol aliases: generate spyro1_symbols.gen.h from the hand-
# maintained alias manifest, validated by address against globals.toml /
# symbols.toml (fails the build if an addr is not a manifest symbol start —
# i.e. drifted). Replaces ~90 per-file #define copies. See plan W0.8.
set(_spyro_symbols_manifest ${CMAKE_CURRENT_SOURCE_DIR}/manifest/spyro1_symbols.toml)
set(_spyro_symbols_header   ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro1_symbols.gen.h)
add_custom_command(
    OUTPUT  ${_spyro_symbols_header}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_symbols_header.py
            --symbols   ${_spyro_symbols_manifest}
            --globals   ${CMAKE_CURRENT_SOURCE_DIR}/manifest/globals.toml
            --functions ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
            --out       ${_spyro_symbols_header}
    DEPENDS ${_spyro_symbols_manifest}
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/globals.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
            ${CMAKE_SOURCE_DIR}/tools/gen_symbols_header.py
    COMMENT "Generating Spyro spyro1_symbols.gen.h (validated vs manifest)")
target_sources(openpete-spyro1 PRIVATE
    ${_spyro_state_regions_header}
    ${_spyro_symbols_header}
    spyro_state_regions.c
    # Phase 2 of fps-unlock: render-side interpolation region table.
    # Hardcoded (g_Spyro + g_Camera) for v1 scaffolding; will graduate
    # to manifest-generated when mobies land in #59.
    spyro_interp_regions.c
    # Sub-tick re-feed orchestrator (native-renderer high-FPS spine, step 2).
    spyro_native_reextract.c)

if(OPENPETE_RECOMP_LINKAGE STREQUAL "static")
    target_link_libraries(openpete-spyro1 PRIVATE spyro1_recomp_module)
else()
    target_link_libraries(openpete-spyro1 PRIVATE ${CMAKE_DL_LIBS})
    if(WIN32)
        # The exe exports exactly the curated surface (the .def rides the
        # link line); the module DLL links an implib generated from the
        # same .def so its imports name openpete-spyro1.exe.
        target_link_options(openpete-spyro1 PRIVATE ${_module_def})
        set_property(TARGET openpete-spyro1 APPEND PROPERTY
            LINK_DEPENDS ${_module_def})
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
endif()
target_link_libraries(openpete-spyro1 PRIVATE openpete_core)

# Shared-linkage builds stage the first-boot inputs next to the exe:
# the converged manifests, the 5-file module source kit, and (when
# built) PSXRecomp. See recomp_build.cpp (distribution D2.4).
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
            ${CMAKE_CURRENT_SOURCE_DIR}/dispatch_chain.c
            ${CMAKE_CURRENT_SOURCE_DIR}/module_main.c
            ${CMAKE_CURRENT_SOURCE_DIR}/spyro_overlay_table.h
            ${CMAKE_SOURCE_DIR}/core/include/openpete_recomp_module.h
            ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/runner/include/psx_runtime.h
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

# App icon. Two sources (distribution D3):
#   OPENPETE_ROM_FREE_ICON=OFF (dev default) — decoded from the PSX
#     save-file header's CLUT+icon data (save_file.h:39): the authentic
#     memcard icon, which is GAME BYTES and must not ship.
#   OPENPETE_ROM_FREE_ICON=ON (dist presets) — original procedural icon
#     from tools/gen_dist_icon.py; also drops the build's dependency on
#     the private decomp submodule's assets.
# Either path emits a .ico for the Win32 resource and a C source for
# SDL_SetWindowIcon (Linux/macOS + live-window fallback on Windows).
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
target_sources(openpete-spyro1 PRIVATE ${_spyro_icon_c})

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
    file(GENERATE OUTPUT ${_spyro_icon_rc}
         CONTENT "1 ICON \"${_spyro_icon_ico}\"\n")
    target_sources(openpete-spyro1 PRIVATE ${_spyro_icon_ico} ${_spyro_icon_rc})
endif()

# Backtrace symbols: pass -rdynamic so non-static functions resolve to names
# in the crash handler's backtrace_symbols_fd output. Cheap (~bigger symtab)
# and only relevant on the Linux/glibc build path.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(openpete-spyro1 PRIVATE -rdynamic)
endif()

# SDL3's platform entry shim. Required by the TU including <SDL3/SDL_main.h>.
if(TARGET SDL3::SDL3main)
    target_link_libraries(openpete-spyro1 PRIVATE SDL3::SDL3main)
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

set_target_properties(openpete-spyro1 PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# The generated C is large (now millions of lines across all overlays)
# and the recompiler emits a few patterns the compiler should not nag
# about (unused labels, unused variables held for future use). Mute
# warnings on every generated TU.
set_source_files_properties(
    ${SPYRO_GENERATED}/PSX.EXE_full.c
    ${SPYRO_GENERATED}/PSX.EXE_dispatch.c
    ${SPYRO_OVERLAY_FULL_C}
    ${SPYRO_OVERLAY_DISPATCH_C}
    PROPERTIES
        COMPILE_FLAGS "-w"
)

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
    # core/runtime/cpu.c.
    include(${SPYRO_GENERATED}/native_aliases.cmake)
    target_link_options(openpete-spyro1 PRIVATE ${SPYRO_NATIVE_DEFSYMS})
endif()

# OpenAL on Windows is always a runtime DLL load (no static .a in the
# openal-soft prebuilt). Copy the renamed soft_oal.dll next to the .exe
# so the loader resolves OpenAL32.dll without a system install.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
    add_custom_command(TARGET openpete-spyro1 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/OpenAL-mingw/x86_64-w64-mingw32/bin/OpenAL32.dll"
            "$<TARGET_FILE_DIR:openpete-spyro1>/OpenAL32.dll"
        VERBATIM)
endif()
