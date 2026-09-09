# games/spyro-2/CMakeLists.txt — Spyro 2: Ripto's Rage! binary.
#
# Sources are the recompiled main EXE plus every overlay translated by
# scripts/regen-recomp.sh (GAME=spyro-2). Each overlay is recompiled with a
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
        "spyro-2: no recompiled output in generated/ — run "
        "`GAME=spyro-2 bash scripts/regen-recomp.sh` first. Skipping target.")
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

add_library(spyro2_recomp_module OBJECT ${GAME_MODULE_SOURCES})
target_link_libraries(spyro2_recomp_module PRIVATE openpete_core)
target_include_directories(spyro2_recomp_module PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${GAME_GENERATED}
    $<TARGET_PROPERTY:openpete_core,INTERFACE_INCLUDE_DIRECTORIES>)
set_target_properties(spyro2_recomp_module PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

# The generated translation units are enormous and entirely machine-written:
# warnings on them are noise, and -O2 over ~150k lines is the build's cost
# centre.
if(NOT MSVC)
    target_compile_options(spyro2_recomp_module PRIVATE -w)
endif()

add_executable(openpete-spyro2
    main.cpp
    game_extras_impl.c
    feature_list.c
    psyq_stub_runtime.c
    ${GAME_GENERATED}/psyq_stubs.c)

target_include_directories(openpete-spyro2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${GAME_GENERATED})

# No thunk layer yet: the generated psyq_stubs.c supplies a weak
# abort-on-call body for every PSYQ symbol, so the binary links and runs until
# the first PSYQ call and then names the surface it needs.
target_link_libraries(openpete-spyro2 PRIVATE
    spyro2_recomp_module
    openpete_core)

if(TARGET SDL3::SDL3main)
    target_link_libraries(openpete-spyro2 PRIVATE SDL3::SDL3main)
endif()
