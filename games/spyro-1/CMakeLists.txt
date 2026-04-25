# games/spyro-1/CMakeLists.txt — Spyro 1 binary.
#
# Phase 0b: empty binary that opens a window using core's run loop.
# Phase 1b will add the recompiled-game source set, PsyCross, and the
# manifest-driven thunk codegen.

add_executable(openpete-spyro1
    main.cpp
    game_extras_impl.c
)

# Link core (which transitively brings SDL2 and stb).
target_link_libraries(openpete-spyro1 PRIVATE openpete_core)

# SDL2main (when present) supplies the platform main entry point.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW AND TARGET SDL2::SDL2main)
    target_link_libraries(openpete-spyro1 PRIVATE SDL2::SDL2main)
elseif(TARGET SDL2::SDL2main)
    target_link_libraries(openpete-spyro1 PRIVATE SDL2::SDL2main)
endif()

set_target_properties(openpete-spyro1 PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
