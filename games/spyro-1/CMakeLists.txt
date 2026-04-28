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
    # DIRECTLY from the matching-decomp tree (no shim layer). Each TU is
    # added with custom flags below; outbound thunks are generated.
    spyro-1/src/main.c
    spyro-1/src/cd.c

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
# Each per-source block reuses these and adds TU-specific bits.
#
#   -std=gnu11 + -include assert.h: PsyCross's libgpu.h uses C11 static_assert
#   -fcommon: tentative file-scope defs become COMMON so --defsym aliases win
#   -include psyq_compat.h: bridges PSYQ-renamed types (RECT vs RECT16)
#   -w: mutes matching-decomp warnings (unused decls from gameplay headers)
set(SPYRO_DECOMP_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/include;${CMAKE_SOURCE_DIR}/third_party/PsyCross/include;${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx")
set(SPYRO_DECOMP_BASE_FLAGS
    "-std=gnu11 -fcommon -include assert.h -include ${CMAKE_CURRENT_SOURCE_DIR}/decomp/psyq_compat.h -w")

# Phase 3 — compile spyro-1/src/main.c directly.
#
# - `-fcommon` makes the four file-scope globals (g_PadMutex, g_DeltaTime,
#   g_UnprocessedFrames, g_StateSwitch) tentative/common defs so the linker
#   resolves them against the strong --defsym aliases below instead of
#   allocating host BSS.
# - `-Dmain=spyro_native_main` keeps the source's `void main(void)` from
#   colliding with the host C runtime entry point.
# - `-w` mutes the matching-decomp source's warnings (mostly unused
#   declarations from heavily-included gameplay headers).
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/src/main.c
    PROPERTIES
        COMPILE_FLAGS "${SPYRO_DECOMP_BASE_FLAGS} -Dmain=spyro_native_main"
        INCLUDE_DIRECTORIES "${SPYRO_DECOMP_INCLUDES}"
)

# Phase 4 — compile spyro-1/src/cd.c directly. Routes its PSYQ Cd*
# calls through the generated outbound thunks (native_thunks.cpp) so
# the Spyro-specific enhancements in psyq_thunks.cpp (XA streaming arm,
# CdSync XA pump, …) stay in the path. Spyro-internal callees
# (SoundsUpdate, CDMusicUpdate) likewise route through the generated
# thunks file back into psx_dispatch_compiled.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/src/cd.c
    PROPERTIES
        COMPILE_FLAGS "${SPYRO_DECOMP_BASE_FLAGS} \
-DCdControl=openpete_CdControl \
-DCdRead=openpete_CdRead \
-DCdSync=openpete_CdSync \
-DCdIntToPos=openpete_CdIntToPos"
        INCLUDE_DIRECTORIES "${SPYRO_DECOMP_INCLUDES}"
)
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
