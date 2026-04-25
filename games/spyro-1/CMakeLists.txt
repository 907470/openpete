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

    # Phase 1b PSYQ stubs — abort-on-call placeholders so unresolved
    # PSYQ externs don't break linkage. Phase 1c regenerates this file
    # with real ABI thunks into PsyCross.
    ${SPYRO_GENERATED}/psyq_stubs.c
    psyq_stub_runtime.c
)

# Generated C addresses psx_runtime.h via the include path provided by
# openpete_core (which adds psxrecomp/runner/include).
target_include_directories(openpete-spyro1 PRIVATE ${SPYRO_GENERATED})

target_link_libraries(openpete-spyro1 PRIVATE openpete_core)

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
