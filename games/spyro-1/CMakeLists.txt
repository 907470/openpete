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
        -mconsole)
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
