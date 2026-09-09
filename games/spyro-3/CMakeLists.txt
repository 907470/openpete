# games/spyro-3/CMakeLists.txt — Spyro: Year of the Dragon binary.
#
# Sources are the recompiled main EXE plus every overlay translated by
# scripts/regen-recomp.sh (GAME=spyro-3). Each overlay is recompiled with a
# unique --func-prefix and its `psx_dispatch_compiled` is sed-renamed to
# `psx_dispatch_<name>` so they all coexist at link; games/common/
# overlay_dispatch.c fans calls out to the active overlay through the
# auto-generated table in generated/spyro_overlay_table.c.
#
# Bring-up scope: no feature modules, no native-decomp TUs, no mod SDK.
# Every PSX vaddr routes through the recompiler.

set(GAME_GENERATED ${CMAKE_CURRENT_SOURCE_DIR}/generated)

if(NOT EXISTS ${GAME_GENERATED}/PSX.EXE_full.c)
    message(WARNING
        "spyro-3: no recompiled output in generated/ — run "
        "`GAME=spyro-3 bash scripts/regen-recomp.sh` first. Skipping target.")
    return()
endif()

# CONFIGURE_DEPENDS re-runs the GLOB so newly regenerated overlays are picked
# up without a manual reconfigure.
file(GLOB GAME_OVERLAY_FULL_C     CONFIGURE_DEPENDS "${GAME_GENERATED}/*/*.bin_full.c")
file(GLOB GAME_OVERLAY_DISPATCH_C CONFIGURE_DEPENDS "${GAME_GENERATED}/*/*.bin_dispatch.c")

set(GAME_MODULE_SOURCES
    ${GAME_GENERATED}/PSX.EXE_full.c
    ${GAME_GENERATED}/PSX.EXE_dispatch.c
    ${GAME_OVERLAY_FULL_C}
    ${GAME_OVERLAY_DISPATCH_C}
    ${GAME_GENERATED}/spyro_overlay_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/overlay_dispatch.c
    module_main.c)

add_library(spyro3_recomp_module OBJECT ${GAME_MODULE_SOURCES})
target_link_libraries(spyro3_recomp_module PRIVATE openpete_core)
target_include_directories(spyro3_recomp_module PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${GAME_GENERATED}
    $<TARGET_PROPERTY:openpete_core,INTERFACE_INCLUDE_DIRECTORIES>)
set_target_properties(spyro3_recomp_module PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

# The generated translation units are enormous and entirely machine-written:
# warnings on them are noise, and -O2 over ~150k lines is the build's cost
# centre.
if(NOT MSVC)
    target_compile_options(spyro3_recomp_module PRIVATE -w)
endif()

add_executable(openpete-spyro3
    main.cpp
    game_extras_impl.c
    feature_list.c
    game_icon_none.c
    psyq_stub_runtime.c

    # The shared PSYQ-to-PsyCross layer, plus the default for every per-game
    # hook this game does not name in psyq_game_provides.h. Listed BEFORE the
    # generated stubs so under MinGW link order lets a thunk's strong
    # definition win against the stub's (paired with
    # --allow-multiple-definition).
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/psyq_thunks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/psyq_game_hooks_default.c

    # PSYQ symbol stubs — abort-on-call. On ELF these are weak so any thunk
    # strongly defines them; on MinGW the macro becomes strong and the link
    # order plus --allow-multiple-definition resolves the collision in the
    # thunk's favour.
    ${GAME_GENERATED}/psyq_stubs.c)

target_include_directories(openpete-spyro3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${GAME_GENERATED})

# Any PSYQ symbol the shared layer does not implement still resolves to the
# generated weak abort stub, which names the surface on first call.
target_link_libraries(openpete-spyro3 PRIVATE
    spyro3_recomp_module
    openpete_core)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
    target_link_options(openpete-spyro3 PRIVATE -Wl,--allow-multiple-definition)

    # OpenAL on Windows is always a runtime DLL load (no static .a in the
    # openal-soft prebuilt). Copy the renamed soft_oal.dll next to the .exe so
    # the loader resolves OpenAL32.dll without a system install; without it the
    # process fails to start before main() runs.
    add_custom_command(TARGET openpete-spyro3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/OpenAL-mingw/x86_64-w64-mingw32/bin/OpenAL32.dll"
            "$<TARGET_FILE_DIR:openpete-spyro3>/OpenAL32.dll"
        VERBATIM)
endif()

if(TARGET SDL3::SDL3main)
    target_link_libraries(openpete-spyro3 PRIVATE SDL3::SDL3main)
endif()
