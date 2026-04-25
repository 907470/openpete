# core/CMakeLists.txt — game-agnostic OpenPete runtime.
#
# Builds a static lib `openpete_core` consumed by per-game targets under
# games/<game>/. Anything Spyro-specific belongs in games/spyro-1/, not
# here. See notes/plans/openpete-pc-port.md.

set(PSXRECOMP_RUNNER ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/runner)

add_library(openpete_core STATIC
    runtime/log.c
    runtime/options.c
    runtime/screenshot.c
    runtime/run.c

    # Phase 1b: slim PSX runtime
    runtime/cpu.c
    runtime/boot.c
    runtime/dispatch.c
    runtime/crash.c
    runtime/exe_loader.cpp

    # Vendored psxrecomp pieces we keep verbatim (per notes plan §5 Phase 1b
    # "keep gte.cpp + memory helpers + iso_reader"). Compiled directly here so
    # we don't drag the rest of psxrecomp's runtime.c into the build.
    ${PSXRECOMP_RUNNER}/src/gte.cpp
    ${PSXRECOMP_RUNNER}/src/iso_reader.cpp
)

# libco: cooperative single-fiber scheduler. The vendored libco/libco.c
# `#include`s a per-architecture source (amd64.c, arm.c, ...) that wasn't
# fetched in Phase 0b — only libco.c + libco.h made it in. Phase 1c
# regenerates the vendor with the platform sources before any fibers
# actually get created. For Phase 1b we only call co_active(), which has
# a trivial header-only fallback (see core/runtime/libco_shim.c).
target_sources(openpete_core PRIVATE runtime/libco_shim.c)

target_include_directories(openpete_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PSXRECOMP_RUNNER}/include
        ${CMAKE_SOURCE_DIR}/third_party/libco
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/stb
)

set_target_properties(openpete_core PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    # iso_reader.cpp uses C++20 ends_with; bump the whole core lib so we
    # don't need per-source overrides.
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# SDL2 is required for window/render/screenshot.
if(TARGET SDL2::SDL2-static)
    target_link_libraries(openpete_core PUBLIC SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(openpete_core PUBLIC SDL2::SDL2)
else()
    target_include_directories(openpete_core PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(openpete_core PUBLIC ${SDL2_LIBRARIES})
endif()

# Fail the build (cheap automated check) if anyone smuggles a Spyro
# address or Win32-only header into core/. The exit-criteria greps in
# §5 Phase 0b live here as build-time guards.
if(NOT CMAKE_HOST_WIN32)
    add_custom_target(openpete_core_purity_check ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking core/ purity..."
        COMMAND bash -c "! grep -rE '0x800[0-9a-fA-F]{4}' ${CMAKE_SOURCE_DIR}/core/ --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp'"
        COMMAND bash -c "! grep -rE 'CreateFiber|<windows.h>' ${CMAKE_SOURCE_DIR}/core/ --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp'"
        COMMENT "core/ purity check: no Spyro addresses, no Win32-only APIs"
        VERBATIM)
endif()
