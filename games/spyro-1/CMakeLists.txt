# games/spyro-1/CMakeLists.txt — Spyro 1 binary.
#
# Sources include the recompiled main EXE plus every overlay translated
# by scripts/regen-recomp.sh. Each overlay is recompiled with a unique
# --func-prefix and its `psx_dispatch_compiled` is sed-renamed to
# `psx_dispatch_<name>` so they all coexist at link. dispatch_chain.c
# fans calls out to the active overlay via the auto-generated table in
# generated/spyro_overlay_table.c.

set(SPYRO_GENERATED ${CMAKE_CURRENT_SOURCE_DIR}/generated)

# Pick up every recompiled overlay automatically. CONFIGURE_DEPENDS
# re-runs the GLOB on subsequent cmake invocations so newly-regen'd
# overlays get picked up without a manual reconfigure.
file(GLOB SPYRO_OVERLAY_FULL_C
    CONFIGURE_DEPENDS
    "${SPYRO_GENERATED}/*/*.bin_full.c")
file(GLOB SPYRO_OVERLAY_DISPATCH_C
    CONFIGURE_DEPENDS
    "${SPYRO_GENERATED}/*/*.bin_dispatch.c")

add_executable(openpete-spyro1
    main.cpp
    game_extras_impl.c

    # Phase 3 / Phase 4 — native translations of spyro-1/src/*.c compiled
    # via the AST rewriter (tools/rewrite_tu.py). Each native TU is
    # rewritten by scripts/regen-manifest.sh into
    # ${SPYRO_GENERATED}/rewritten/<stem>.rewritten.c — Pass 2 redirects
    # diverging bare globals through __psx_<name> uint32 storage that
    # gen_defsyms.py aliases into g_ram. CMake compiles the rewritten
    # source, never the original.
    ${SPYRO_GENERATED}/rewritten/main.rewritten.c
    ${SPYRO_GENERATED}/rewritten/cd.rewritten.c
    ${SPYRO_GENERATED}/rewritten/rand.rewritten.c
    ${SPYRO_GENERATED}/rewritten/checkpoint.rewritten.c
    ${SPYRO_GENERATED}/rewritten/moby_interpolation_check.rewritten.c
    # initialization.c / completion.c re-enable gated on Pass 4/5
    # (integer-cast pointer arithmetic + struct-by-value boundary).

    # Generated from manifest/native.toml by tools/gen_dispatch.py +
    # tools/gen_thunks.py. native_dispatch.c is the inbound override
    # switch (recomp → native); native_thunks.cpp is the outbound side
    # (native → recomp / native → PSYQ).
    ${SPYRO_GENERATED}/native_dispatch.c
    ${SPYRO_GENERATED}/native_thunks.cpp

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
        # Override SDL2main's -mwindows so the .exe is a console-subsystem
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
    ${SPYRO_GENERATED})

target_link_libraries(openpete-spyro1 PRIVATE openpete_core)

# Backtrace symbols: pass -rdynamic so non-static functions resolve to names
# in the crash handler's backtrace_symbols_fd output. Cheap (~bigger symtab)
# and only relevant on the Linux/glibc build path.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(openpete-spyro1 PRIVATE -rdynamic)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW AND TARGET SDL2::SDL2main)
    target_link_libraries(openpete-spyro1 PRIVATE SDL2::SDL2main)
elseif(TARGET SDL2::SDL2main)
    target_link_libraries(openpete-spyro1 PRIVATE SDL2::SDL2main)
endif()

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

# All natively-compiled spyro-1/src/*.c TUs share the same flags now —
# the per-TU rename hand-list is gone, picked up automatically from
# native_renames.h. Add a TU by appending to native.toml's
# [[native_source]] and re-running scripts/regen-manifest.sh.
set(SPYRO_NATIVE_SOURCES
    ${SPYRO_GENERATED}/rewritten/main.rewritten.c
    ${SPYRO_GENERATED}/rewritten/cd.rewritten.c
    ${SPYRO_GENERATED}/rewritten/rand.rewritten.c
    ${SPYRO_GENERATED}/rewritten/checkpoint.rewritten.c
    ${SPYRO_GENERATED}/rewritten/moby_interpolation_check.rewritten.c
    # initialization.c / completion.c re-enable gated on Pass 4/5
    # (integer-cast pointer arithmetic + struct-by-value boundary).
)

# Per-TU shadow include trees (Pass 1 struct relayout). Each native TU
# gets ${SPYRO_GENERATED}/rewritten/<stem>/include/ prepended to its -I
# list so PSX-laid struct definitions win over the originals in
# spyro-1/include. The shadow trees are populated by tools/rewrite_tu.py
# during scripts/regen-manifest.sh.
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
