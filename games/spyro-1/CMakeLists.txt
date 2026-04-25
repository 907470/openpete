# games/spyro-1/CMakeLists.txt — Spyro 1 binary.
#
# Phase 1b adds the recompiled-C source set, the dispatch table, and a
# generated PSYQ stub layer so the link succeeds. PsyCross wiring (real
# implementations of those PSYQ stubs) lands in Phase 1c.

set(SPYRO_GENERATED ${CMAKE_CURRENT_SOURCE_DIR}/generated)

add_executable(openpete-spyro1
    main.cpp
    game_extras_impl.c

    # Recompiler outputs.
    ${SPYRO_GENERATED}/PSX.EXE_full.c
    ${SPYRO_GENERATED}/PSX.EXE_dispatch.c

    # Titlescreen overlay (Phase 1d.5) — synth PS1-EXE wrapper around the
    # build/wad/titlescreen_code.ovl bytes, loaded to vaddr 0x8007AA38.
    # Its dispatcher is renamed via -D so it doesn't clash with the main
    # EXE's; both feed into `psx_dispatch_compiled` from dispatch_chain.c.
    # Function-symbol prefix `func_titlescreen_` (set at recompile time)
    # keeps its names unique versus other overlays at the same vaddr.
    ${SPYRO_GENERATED}/titlescreen/titlescreen.bin_full.c
    ${SPYRO_GENERATED}/titlescreen/titlescreen.bin_dispatch.c

    # Level 10 — Artisans Homeworld (Phase 2). Same overlay vaddr as
    # titlescreen, different bytes at runtime. Function symbols use the
    # `func_level_10_` prefix.
    ${SPYRO_GENERATED}/level_10/level_10.bin_full.c
    ${SPYRO_GENERATED}/level_10/level_10.bin_dispatch.c
    dispatch_chain.c

    # PSYQ symbol stubs — `__attribute__((weak))` abort-on-call. Each
    # thunk in psyq_thunks.c is `strong` and overrides its same-named
    # weak stub at link time. Anything not yet thunked aborts on first
    # call, surfacing the next missing PSYQ surface.
    ${SPYRO_GENERATED}/psyq_stubs.c
    psyq_stub_runtime.c

    # Hand-written PSYQ → PsyCross ABI thunks (Phase 1c).
    psyq_thunks.cpp
)

# Generated C addresses psx_runtime.h via the include path provided by
# openpete_core (which adds psxrecomp/runner/include).
target_include_directories(openpete-spyro1 PRIVATE ${SPYRO_GENERATED})

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

# The generated C is large (160k lines) and the recompiler emits a few
# patterns the compiler should not nag about (unused labels, unused
# variables held for future use). Mute those for the generated TUs.
set_source_files_properties(
    ${SPYRO_GENERATED}/PSX.EXE_full.c
    ${SPYRO_GENERATED}/PSX.EXE_dispatch.c
    PROPERTIES
        COMPILE_FLAGS "-w"
)

# Rename each generated dispatcher to its overlay-specific name so they can
# coexist. `dispatch_chain.c` provides the real psx_dispatch_compiled and
# fans out to each in turn. This keeps the generated files pristine — no
# post-processing needed on every regen.
set_source_files_properties(
    ${SPYRO_GENERATED}/PSX.EXE_dispatch.c
    PROPERTIES
        COMPILE_FLAGS "-w -Dpsx_dispatch_compiled=psx_dispatch_main_exe"
)
set_source_files_properties(
    ${SPYRO_GENERATED}/titlescreen/titlescreen.bin_full.c
    ${SPYRO_GENERATED}/titlescreen/titlescreen.bin_dispatch.c
    ${SPYRO_GENERATED}/level_10/level_10.bin_full.c
    ${SPYRO_GENERATED}/level_10/level_10.bin_dispatch.c
    PROPERTIES
        COMPILE_FLAGS "-w"
)
set_source_files_properties(
    ${SPYRO_GENERATED}/titlescreen/titlescreen.bin_dispatch.c
    PROPERTIES
        COMPILE_FLAGS "-w -Dpsx_dispatch_compiled=psx_dispatch_titlescreen"
)
set_source_files_properties(
    ${SPYRO_GENERATED}/level_10/level_10.bin_dispatch.c
    PROPERTIES
        COMPILE_FLAGS "-w -Dpsx_dispatch_compiled=psx_dispatch_level_10"
)
