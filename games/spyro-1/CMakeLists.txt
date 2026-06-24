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

add_executable(openpete-spyro1
    main.cpp
    game_extras_impl.c
    feature_list.c
    features/no_lod.c
    features/widescreen.c
    features/render_paths.c
    features/render_decouple.c
    features/polybuf.c
    features/cyclorama_native.c
    features/moby_native.c
    features/spyro_native.c
    features/flame_native.c
    features/shadow_native.c
    features/particle_native.c
    features/trail_native.c
    features/terrain_native.c
    features/ui_sprite_native.c
    features/portal_native.c
    features/text_moby_native.c

    # Phase 3 / Phase 4 — native translations of spyro-1/src/*.c compiled
    # via the AST rewriter (tools/rewrite_tu.py). Each native TU is
    # rewritten by scripts/regen-manifest.sh into
    # ${SPYRO_GENERATED}/rewritten/<stem>.rewritten.c — Pass 2 redirects
    # diverging bare globals through __psx_<name> uint32 storage that
    # gen_defsyms.py aliases into g_ram. CMake compiles the rewritten
    # source, never the original. Suppressed when OPENPETE_SKIP_DECOMP=ON.
    ${SPYRO_DECOMP_TUS}

    # Recompiled main EXE. Its `psx_dispatch_compiled` is sed-renamed to
    # `psx_dispatch_main_exe` by regen-recomp.sh.
    ${SPYRO_GENERATED}/PSX.EXE_full.c
    ${SPYRO_GENERATED}/PSX.EXE_dispatch.c

    # Every translated overlay (titlescreen, credits, level_10..level_64).
    # Function symbols use unique `func_<name>_` prefixes; dispatch fns
    # are unique-named via sed. Both globs above feed in here.
    ${SPYRO_OVERLAY_FULL_C}
    ${SPYRO_OVERLAY_DISPATCH_C}

    # Auto-generated overlay registry — id-magic -> dispatcher.
    ${SPYRO_GENERATED}/spyro_overlay_table.c

    dispatch_chain.c

    # Hand-written PSYQ → PsyCross ABI thunks. Listed BEFORE the
    # generated stubs so under MinGW link order lets the thunk's strong
    # def win against the stub's strong def (paired with
    # --allow-multiple-definition).
    psyq_thunks.cpp
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
        # Disable ASLR. The Phase 3/4 native-decomp globals are link-aliased
        # into g_ram via -Wl,--defsym=name=g_ram+offset. On ELF this produces
        # a PIE-aware relocation; on PE/COFF the MinGW linker can't represent
        # the absolute symbol ("stripping non-representable symbol …"), so
        # the absolute address gets baked into call sites with no entry in
        # .reloc. ASLR then moves g_ram at load time, the baked addresses
        # rot, and the first decomp global access faults. Pinning the image
        # base sidesteps this until we move to a real PE-friendly aliasing
        # mechanism (see plan §8: linker script vs def-file vs macro indirection).
        -Wl,--disable-dynamicbase
        -Wl,--disable-high-entropy-va)
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
target_sources(openpete-spyro1 PRIVATE
    ${_spyro_state_regions_header}
    spyro_state_regions.c
    # Phase 2 of fps-unlock: render-side interpolation region table.
    # Hardcoded (g_Spyro + g_Camera) for v1 scaffolding; will graduate
    # to manifest-generated when mobies land in #59.
    spyro_interp_regions.c
    # Sub-tick re-feed orchestrator (native-renderer high-FPS spine, step 2).
    spyro_native_reextract.c)

target_link_libraries(openpete-spyro1 PRIVATE openpete_core)

# App icon decoded from the PSX save-file header's CLUT+icon data
# (see spyro-1/include/save_file.h:39). The same generator emits a
# .ico for the Win32 resource compiler and a C source for
# SDL_SetWindowIcon at runtime; the runtime path covers Linux/macOS
# and is also a fallback on Windows for the live window.
set(_spyro_icon_src
    ${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/assets/save_header.databin.bin)
set(_spyro_icon_tool ${CMAKE_SOURCE_DIR}/tools/mcs_icon_to_ico.py)
set(_spyro_icon_c ${CMAKE_CURRENT_BINARY_DIR}/gen/openpete_spyro1_icon.c)
add_custom_command(
    OUTPUT  ${_spyro_icon_c}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${_spyro_icon_tool} ${_spyro_icon_src}
            --c ${_spyro_icon_c}
    DEPENDS ${_spyro_icon_src} ${_spyro_icon_tool}
    COMMENT "Decoding PSX memcard icon → openpete_spyro1_icon.c")
target_sources(openpete-spyro1 PRIVATE ${_spyro_icon_c})

if(WIN32)
    set(_spyro_icon_ico ${CMAKE_CURRENT_BINARY_DIR}/openpete-spyro1.ico)
    set(_spyro_icon_rc  ${CMAKE_CURRENT_BINARY_DIR}/openpete-spyro1.rc)
    add_custom_command(
        OUTPUT  ${_spyro_icon_ico}
        COMMAND python3 ${_spyro_icon_tool} ${_spyro_icon_src}
                --ico ${_spyro_icon_ico}
        DEPENDS ${_spyro_icon_src} ${_spyro_icon_tool}
        COMMENT "Decoding PSX memcard icon → openpete-spyro1.ico")
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

# Stage the native renderer's compiled SPIR-V shaders next to the exe so the
# RHI finds them via SDL_GetBasePath() ("shaders/" beside the binary) on a
# relocated/Windows install. Dev runs from the build tree also resolve via the
# RHI's baked RHI_SHADER_DIR fallback, but the beside-exe copy is the shipped
# path. One copy command per .spv with a real DEPENDS, so a shader-only change
# re-stages without needing the exe to relink (RHI_SHADER_SPV is exported by
# core/rhi/CMakeLists.txt; the exe lands in this dir, so beside-exe ==
# ${CMAKE_CURRENT_BINARY_DIR}/shaders).
set(_spyro_staged_shaders "")
foreach(_spv IN LISTS RHI_SHADER_SPV)
    get_filename_component(_spv_name "${_spv}" NAME)
    set(_spv_dst "${CMAKE_CURRENT_BINARY_DIR}/shaders/${_spv_name}")
    add_custom_command(
        OUTPUT  "${_spv_dst}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_spv}" "${_spv_dst}"
        DEPENDS "${_spv}"
        COMMENT "Staging shader ${_spv_name} next to openpete-spyro1"
        VERBATIM)
    list(APPEND _spyro_staged_shaders "${_spv_dst}")
endforeach()
add_custom_target(openpete_spyro1_stage_shaders DEPENDS ${_spyro_staged_shaders})
add_dependencies(openpete-spyro1 openpete_spyro1_stage_shaders)

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
